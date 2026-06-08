/***********************************************************************
 * Watermark API implementation for Opus with SILK gain-matrix stego.
 *
 * Wraps the Opus encoder/decoder and exposes WatermarkEncode /
 * WatermarkDecode per the delivery standard interface.
 ***********************************************************************/
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "opus.h"
#include "opus_defines.h"
#include "watermark.h"

/* ---- Internal constants ---- */
#define STEGO_FRAME_MAX_BITS   6
#define STEGO_SEQ_BITS         2
#define STEGO_PAYLOAD_BITS     4
#define XOR3_DATA_FRAMES       2
#define XOR3_PARITY_FRAMES     1
#define XOR3_BLOCK_FRAMES      (XOR3_DATA_FRAMES + XOR3_PARITY_FRAMES)
#define MAX_PACKET_SIZE        4096

/* ---- Time helpers ---- */
static int64_t clock_us(void)
{
    return (int64_t)(clock() * 1000000LL / CLOCKS_PER_SEC);
}

/* ---- Budget accumulator ---- */
static int budget_allow_bits(int64_t *acc, int bps, int samples,
                             int sampling_rate, int max_bits)
{
    int64_t allowed;
    *acc += (int64_t)bps * samples;
    allowed = *acc / sampling_rate;
    if (allowed > max_bits) allowed = max_bits;
    *acc -= allowed * sampling_rate;
    return (int)allowed;
}

/* ---- XOR-3 FEC (C implementation of Python validate_lossy.py logic) ---- */

/* Encode: input raw bytes → output nibble stream (1 byte per nibble).
 * Returns number of output nibbles. */
static size_t xor3_encode(const uint8_t *data, size_t data_len, uint8_t *out)
{
    size_t nibble_count = 0;
    for (size_t i = 0; i < data_len; i++) {
        out[nibble_count++] = data[i] & 0x0F;
        out[nibble_count++] = (data[i] >> 4) & 0x0F;
    }
    /* Group into blocks of 2 data nibbles + 1 parity nibble */
    size_t blocks = nibble_count / XOR3_DATA_FRAMES;
    if (nibble_count % XOR3_DATA_FRAMES) blocks++;
    /* Pad last block with zeros if needed */
    while (nibble_count < blocks * XOR3_DATA_FRAMES)
        out[nibble_count++] = 0;
    /* Compute parity and interleave */
    size_t total_nibbles = blocks * XOR3_BLOCK_FRAMES;
    /* Work backwards to avoid overwriting */
    for (size_t b = blocks; b > 0; b--) {
        size_t src_off = (b - 1) * XOR3_DATA_FRAMES;
        size_t dst_off = (b - 1) * XOR3_BLOCK_FRAMES;
        uint8_t d0 = out[src_off];
        uint8_t d1 = out[src_off + 1];
        uint8_t p  = (d0 ^ d1) & 0x0F;
        out[dst_off]     = d0;
        out[dst_off + 1] = d1;
        out[dst_off + 2] = p;
    }
    return total_nibbles;
}

/* Decode: input nibble stream (1 byte per nibble, gap_positions for lost nibbles).
 * gap_positions[n] = 1 means nibble n was lost. */
static size_t xor3_decode(const uint8_t *nibbles, size_t nibble_count,
                          const int *gap_positions,
                          size_t original_data_len, uint8_t *out)
{
    uint8_t decoded[8192]; /* max nibbles */
    size_t decoded_count = 0;

    for (size_t i = 0; i + XOR3_BLOCK_FRAMES <= nibble_count;
         i += XOR3_BLOCK_FRAMES) {
        uint8_t chunk[3] = { nibbles[i], nibbles[i+1], nibbles[i+2] };
        int missing[3], mcount = 0;
        for (int j = 0; j < XOR3_BLOCK_FRAMES; j++) {
            if (gap_positions && gap_positions[i + j])
                missing[mcount++] = j;
        }
        if (mcount == 0) {
            decoded[decoded_count++] = chunk[0];
            decoded[decoded_count++] = chunk[1];
        } else if (mcount == 1) {
            int m = missing[0];
            if (m == 0) chunk[0] = chunk[1] ^ chunk[2];
            else if (m == 1) chunk[1] = chunk[0] ^ chunk[2];
            /* m == 2: parity lost, data intact */
            decoded[decoded_count++] = chunk[0];
            decoded[decoded_count++] = chunk[1];
        } else {
            decoded[decoded_count++] = 0;
            decoded[decoded_count++] = 0;
        }
    }

    /* Pack nibbles back to bytes */
    size_t target_nibbles = original_data_len * 2;
    size_t out_bytes = 0;
    for (size_t j = 0; j + 1 < decoded_count && j < target_nibbles; j += 2) {
        out[out_bytes++] = (decoded[j] & 0x0F) | ((decoded[j+1] & 0x0F) << 4);
    }
    return out_bytes;
}

/* ---- Bit-level read helpers (LSB-first, matches opus_demo stego_tx) ---- */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;    /* bit position */
} BitReader;

static int bit_reader_has_next(const BitReader *br)
{
    return br->pos < br->len * 8;
}

static int bit_reader_next(BitReader *br)
{
    int byte_pos = (int)(br->pos >> 3);
    int bit_pos  = (int)(br->pos & 7);
    br->pos++;
    return (br->data[byte_pos] >> bit_pos) & 1;
}

/* ---- Bit-level write helpers (LSB-first, matches opus_demo stego_rx) ---- */
typedef struct {
    uint8_t *data;
    size_t   capacity;
    size_t   byte_pos;
    int      bit_pos;
    size_t   total_bits;
} BitWriter;

static void bit_writer_init(BitWriter *bw, uint8_t *buf, size_t cap)
{
    bw->data = buf;
    bw->capacity = cap;
    bw->byte_pos = 0;
    bw->bit_pos = 0;
    bw->total_bits = 0;
    if (cap > 0) memset(buf, 0, cap);
}

static int bit_writer_write(BitWriter *bw, int bit)
{
    if (bw->byte_pos >= bw->capacity) return 0;
    if (bit) bw->data[bw->byte_pos] |= (uint8_t)(1 << bw->bit_pos);
    bw->bit_pos++;
    bw->total_bits++;
    if (bw->bit_pos == 8) {
        bw->bit_pos = 0;
        bw->byte_pos++;
    }
    return 1;
}

static void bit_writer_flush(BitWriter *bw)
{
    if (bw->bit_pos > 0 && bw->byte_pos < bw->capacity)
        bw->byte_pos++;
}

/* ---- Memory write helpers (big-endian u32, matches opus_demo write_u32_be) ---- */
static void write_u32_be(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val >> 24);
    dst[1] = (uint8_t)(val >> 16);
    dst[2] = (uint8_t)(val >> 8);
    dst[3] = (uint8_t)(val);
}

static uint32_t read_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8)  |  (uint32_t)src[3];
}

/* ===================================================================
 * WatermarkEncode
 * =================================================================== */
int WatermarkEncode(
    const int16_t *pcmStream, size_t pcmLen,
    const uint8_t *watermarkBytes, size_t watermarkLen,
    int bitrate, int stegoBps, int lossMode,
    uint8_t *opusStream, size_t *opusLen,
    int64_t *encodeTimeUs)
{
    int err, ret = WATERMARK_OK;
    OpusEncoder *enc = NULL;
    int64_t t0;
    int frame_size;
    int channels = 1, rate = 48000;

    /* ---- Validation ---- */
    if (!pcmStream || !opusStream || !opusLen) return WATERMARK_ERR_NULL;
    if (*opusLen < (size_t)pcmLen) return WATERMARK_ERR_NULL; /* rough cap check */

    t0 = clock_us();

    /* ---- Create encoder ---- */
    enc = opus_encoder_create(rate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) { ret = WATERMARK_ERR_ENCODE; goto cleanup; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0)); /* CBR */

    frame_size = rate / 50; /* 20ms @ 48kHz */

    /* ---- FEC encoding (if loss-resilience mode) ---- */
    uint8_t  fec_buf[4096];
    size_t   fec_len = 0;
    BitReader tx_reader;
    int      use_seq = (lossMode == WATERMARK_LOSS_SEQ_FEC);

    if (watermarkLen > 0 && watermarkBytes) {
        if (use_seq) {
            fec_len = xor3_encode(watermarkBytes, watermarkLen, fec_buf);
            tx_reader.data = fec_buf;
            tx_reader.len  = fec_len;
        } else {
            tx_reader.data = watermarkBytes;
            tx_reader.len  = watermarkLen;
        }
        tx_reader.pos = 0;
    } else {
        tx_reader.data = NULL;
        tx_reader.len  = 0;
        tx_reader.pos  = 0;
    }

    /* ---- Encode loop ---- */
    int64_t  budget_acc = 0;
    int      seq_counter = 0;
    size_t   stream_pos = 0;
    int      total_frames = 0;
    opus_int32 total_embedded = 0;

    for (size_t off = 0; off < pcmLen; off += (size_t)frame_size) {
        size_t this_frame = frame_size;
        if (off + this_frame > pcmLen) {
            /* Pad last partial frame with zeros */
            this_frame = pcmLen - off;
        }

        int bits = 0, nbits = 0, payload_nbits = 0;
        int applied_nbits = 0, allow = 0;
        opus_int32 saved_pos = (opus_int32)tx_reader.pos;

        if (bit_reader_has_next(&tx_reader)) {
            allow = budget_allow_bits(&budget_acc, stegoBps,
                                       (int)this_frame, rate, STEGO_FRAME_MAX_BITS);
            int payload_nbits = 0;

            if (use_seq) {
                int max_payload = STEGO_PAYLOAD_BITS;
                if (max_payload > allow - STEGO_SEQ_BITS)
                    max_payload = allow - STEGO_SEQ_BITS;
                if (max_payload < 0) max_payload = 0;

                int payload_bits = 0, pn = 0;
                while (pn < max_payload && bit_reader_has_next(&tx_reader)) {
                    payload_bits |= bit_reader_next(&tx_reader) << pn;
                    pn++;
                }
                payload_nbits = pn;
                nbits = payload_nbits + STEGO_SEQ_BITS;
                bits = payload_bits | ((seq_counter & ((1 << STEGO_SEQ_BITS) - 1)) << payload_nbits);
                seq_counter++;
            } else {
                while (nbits < allow && bit_reader_has_next(&tx_reader)) {
                    bits |= bit_reader_next(&tx_reader) << nbits;
                    nbits++;
                }
            }

            {
                int stego_ctl = ((nbits & 0xFF) << 8) | (bits & 0xFF);
                opus_encoder_ctl(enc, OPUS_SET_STEGO_BITS(stego_ctl));
            }
        }

        {
            opus_int16 frame_buf[960]; /* 20ms @ 48kHz */
            int copy_len = (int)this_frame;
            if (copy_len > 960) copy_len = 960;
            /* Read from pcmStream; pad with zeros if short */
            memcpy(frame_buf, pcmStream + off, (size_t)copy_len * sizeof(int16_t));
            if (copy_len < 960)
                memset(frame_buf + copy_len, 0, (size_t)(960 - copy_len) * sizeof(int16_t));

            unsigned char packet[MAX_PACKET_SIZE];
            int packet_len = opus_encode(enc, frame_buf, 960, packet, MAX_PACKET_SIZE);
            if (packet_len < 0) { ret = WATERMARK_ERR_ENCODE; goto cleanup; }

            /* Read back applied stego bits */
            {
                int applied_ctl = 0;
                opus_encoder_ctl(enc, OPUS_GET_STEGO_BITS(&applied_ctl));
                applied_nbits = (applied_ctl >> 8) & 0xFF;
            }

            /* Adjust reader position (seq: only consume payload bits) */
            if (use_seq) {
                int payload_applied = applied_nbits - STEGO_SEQ_BITS;
                if (payload_applied < 0) payload_applied = 0;
                if (payload_applied > payload_nbits) payload_applied = payload_nbits;
                tx_reader.pos = (size_t)saved_pos + (size_t)payload_applied;
                total_embedded += payload_applied;
            } else {
                tx_reader.pos = (size_t)saved_pos + (size_t)applied_nbits;
                total_embedded += applied_nbits;
            }

            /* Return unused budget (matches opus_demo.c budget logic) */
            budget_acc += (int64_t)(allow - applied_nbits) * rate;

            opus_uint32 final_range;
            opus_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&final_range));

            /* Write packet: [len:4 BE][final_range:4 BE][data] */
            if (stream_pos + 8 + (size_t)packet_len > *opusLen) {
                ret = WATERMARK_ERR_ENCODE; goto cleanup;
            }
            write_u32_be(opusStream + stream_pos, (uint32_t)packet_len);
            write_u32_be(opusStream + stream_pos + 4, final_range);
            memcpy(opusStream + stream_pos + 8, packet, (size_t)packet_len);
            stream_pos += 8 + (size_t)packet_len;
            total_frames++;
        }
    }

    *opusLen = stream_pos;
    (void)total_embedded;
    (void)total_frames;

cleanup:
    if (enc) opus_encoder_destroy(enc);
    if (encodeTimeUs) *encodeTimeUs = clock_us() - t0;
    return ret;
}

/* ===================================================================
 * WatermarkDecode
 * =================================================================== */
int WatermarkDecode(
    const uint8_t *opusStream, size_t opusLen,
    int bitrate, int stegoBps, int lossMode,
    const int *erasureFlags, int numFrames,
    int16_t *pcmStream, size_t *pcmLen,
    uint8_t *watermarkBytes, size_t *watermarkLen,
    int64_t *decodeTimeUs)
{
    int err, ret = WATERMARK_OK;
    OpusDecoder *dec = NULL;
    int64_t t0;
    int rate = 48000, channels = 1;

    if (!opusStream || !pcmStream || !pcmLen) return WATERMARK_ERR_NULL;
    (void)bitrate;

    t0 = clock_us();

    dec = opus_decoder_create(rate, channels, &err);
    if (err != OPUS_OK) { ret = WATERMARK_ERR_DECODE; goto cleanup; }

    /* ---- Decode loop ---- */
    int64_t  budget_acc = 0;
    int      use_seq = (lossMode == WATERMARK_LOSS_SEQ_FEC);
    int      rx_seq_inited = 0, rx_expected_seq = 0, rx_nibble_pos = 0;
    BitWriter rx_writer;
    uint8_t  rx_buf[8192];
    int     *gap_array = NULL;
    size_t   gap_capacity = 0;

    bit_writer_init(&rx_writer, rx_buf, sizeof(rx_buf));

    if (use_seq) {
        gap_capacity = 4096;
        gap_array = (int *)calloc(gap_capacity, sizeof(int));
    }

    size_t   pcm_out = 0;
    int      frame_size = 960;
    int      frame_idx = 0;
    int      total_extracted = 0;

    for (size_t off = 0; off + 8 <= opusLen; ) {
        uint32_t pkt_len = read_u32_be(opusStream + off);
        off += 4;
        uint32_t final_range = read_u32_be(opusStream + off);
        off += 4;
        (void)final_range;

        if (pkt_len > MAX_PACKET_SIZE || off + pkt_len > opusLen) break;

        int lost = (erasureFlags && frame_idx < numFrames) ? erasureFlags[frame_idx] : 0;
        frame_idx++;

        int decoded_samples;
        if (lost) {
            decoded_samples = opus_decode(dec, NULL, 0,
                                          (opus_int16 *)(pcmStream + pcm_out),
                                          frame_size, 1); /* FEC */
        } else {
            decoded_samples = opus_decode(dec, opusStream + off, (int32_t)pkt_len,
                                          (opus_int16 *)(pcmStream + pcm_out),
                                          frame_size, 0);
        }
        if (decoded_samples < 0) { ret = WATERMARK_ERR_DECODE; goto cleanup; }
        pcm_out += (size_t)decoded_samples;

        /* ---- Stego extraction ---- */
        if (watermarkBytes && watermarkLen && *watermarkLen > 0) {
            int allow = budget_allow_bits(&budget_acc, stegoBps, frame_size,
                                          rate, STEGO_FRAME_MAX_BITS);

            if (!lost) {
                int stego_ctl = 0;
                opus_decoder_ctl(dec, OPUS_GET_STEGO_BITS(&stego_ctl));
                int bits = stego_ctl & 0xFF;
                int nbits = (stego_ctl >> 8) & 0xFF;
                if (nbits > allow) nbits = allow;

                if (use_seq && nbits >= STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS) {
                    int pos = 0;
                    while (pos + STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS <= nbits) {
                        int frame_bits = (bits >> pos) & ((1 << (STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS)) - 1);
                        int seq     = frame_bits >> STEGO_PAYLOAD_BITS;
                        int payload = frame_bits & ((1 << STEGO_PAYLOAD_BITS) - 1);

                        if (rx_seq_inited) {
                            int seq_mask = (1 << STEGO_SEQ_BITS) - 1;
                            while (seq != rx_expected_seq) {
                                /* Gap: insert zero nibble */
                                for (int b = 0; b < STEGO_PAYLOAD_BITS; b++)
                                    bit_writer_write(&rx_writer, 0);
                                if (gap_array && rx_nibble_pos < gap_capacity)
                                    gap_array[rx_nibble_pos] = 1;
                                rx_nibble_pos++;
                                rx_expected_seq = (rx_expected_seq + 1) & seq_mask;
                            }
                        }
                        /* Write payload bits */
                        for (int b = 0; b < STEGO_PAYLOAD_BITS; b++)
                            bit_writer_write(&rx_writer, (payload >> b) & 1);
                        rx_nibble_pos++;
                        total_extracted += STEGO_PAYLOAD_BITS;

                        if (!rx_seq_inited) {
                            rx_expected_seq = seq;
                            rx_seq_inited = 1;
                        }
                        rx_expected_seq = (rx_expected_seq + 1) & ((1 << STEGO_SEQ_BITS) - 1);
                        pos += STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS;
                    }
                } else if (nbits > 0) {
                    for (int b = 0; b < nbits; b++)
                        bit_writer_write(&rx_writer, (bits >> b) & 1);
                    total_extracted += nbits;
                }
            } else {
                /* Lost frame: zero payload in seq mode */
                if (use_seq) {
                    for (int b = 0; b < STEGO_PAYLOAD_BITS; b++)
                        bit_writer_write(&rx_writer, 0);
                    if (gap_array && rx_nibble_pos < gap_capacity)
                        gap_array[rx_nibble_pos] = 1;
                    rx_nibble_pos++;
                }
            }
        }

        off += pkt_len;
        if (numFrames > 0 && frame_idx >= numFrames) break;
    }

    *pcmLen = pcm_out;

    /* ---- Finalize watermark output ---- */
    if (watermarkBytes && watermarkLen && *watermarkLen > 0) {
        bit_writer_flush(&rx_writer);
        size_t raw_bytes = rx_writer.byte_pos;

        if (use_seq) {
            /* XOR-3 decode */
            size_t recovered = xor3_decode(
                rx_buf, rx_nibble_pos, gap_array,
                raw_bytes /* approximate */, watermarkBytes);
            /* Ensure we don't exceed output buffer */
            if (recovered > *watermarkLen) recovered = *watermarkLen;
            *watermarkLen = recovered;
        } else {
            size_t n = raw_bytes;
            if (n > *watermarkLen) n = *watermarkLen;
            memcpy(watermarkBytes, rx_buf, n);
            *watermarkLen = n;
        }
    }

cleanup:
    free(gap_array);
    if (dec) opus_decoder_destroy(dec);
    if (decodeTimeUs) *decodeTimeUs = clock_us() - t0;
    return ret;
}
