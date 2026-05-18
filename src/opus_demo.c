/* Stego-capable opus_demo for SILK workflow verification.
 * Stream format:
 *   [4-byte big-endian packet length]
 *   [4-byte big-endian final range]
 *   [packet payload bytes]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"

#define MAX_PACKET_SIZE 1500
#define STEGO_DEFAULT_BPS 250
#define STEGO_FRAME_MAX_BITS 6
#define STEGO_SEQ_BITS 2
#define STEGO_PAYLOAD_BITS 4

typedef struct {
    unsigned char *input;
    opus_int32 input_len;
    opus_int32 input_pos;
    opus_int64 embedded_bits;
} StegoTxState;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s -e voip <rate> <channels> <bitrate> [-cbr] [-stego_in file] [-stego_bps n] <in.pcm> <out.bit>\n"
            "  %s -d <rate> <channels> [-stego_out file] [-stego_bps n] <in.bit> <out.pcm>\n",
            prog, prog);
}

static int read_u32_be(FILE *f, opus_uint32 *out)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) {
        return 0;
    }
    *out = ((opus_uint32)b[0] << 24) |
           ((opus_uint32)b[1] << 16) |
           ((opus_uint32)b[2] << 8) |
           ((opus_uint32)b[3]);
    return 1;
}

static int write_u32_be(FILE *f, opus_uint32 v)
{
    unsigned char b[4];
    b[0] = (unsigned char)((v >> 24) & 0xFF);
    b[1] = (unsigned char)((v >> 16) & 0xFF);
    b[2] = (unsigned char)((v >> 8) & 0xFF);
    b[3] = (unsigned char)(v & 0xFF);
    return fwrite(b, 1, 4, f) == 4;
}

static int stego_load_file(const char *path, unsigned char **buffer, opus_int32 *length)
{
    FILE *f;
    long sz;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    *length = (opus_int32)sz;
    *buffer = (unsigned char*)malloc((size_t)(*length));
    if (*length > 0 && *buffer == NULL) {
        fclose(f);
        return 0;
    }
    if (*length > 0 && fread(*buffer, 1, (size_t)(*length), f) != (size_t)(*length)) {
        free(*buffer);
        *buffer = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int stego_tx_next_bit(StegoTxState *st)
{
    int bit;
    if (st->input_pos >= st->input_len * 8) return 0;
    bit = (st->input[st->input_pos / 8] >> (st->input_pos % 8)) & 1;
    st->input_pos++;
    return bit;
}

static int stego_tx_has_next_bit(const StegoTxState *st)
{
    return st->input_pos < st->input_len * 8;
}

static int stego_budget_allow_bits(opus_int64 *acc, int bps, int samples, int sampling_rate, int max_bits)
{
    int allowed;
    *acc += (opus_int64)bps * samples;
    allowed = (int)(*acc / sampling_rate);
    if (allowed > max_bits) allowed = max_bits;
    *acc -= (opus_int64)allowed * sampling_rate;
    return allowed;
}

static void stego_tx_prepare_frame_bits(StegoTxState *st, int max_bits, int *bits_out, int *nbits_out)
{
    *bits_out = 0;
    *nbits_out = 0;
    while (*nbits_out < max_bits && stego_tx_has_next_bit(st)) {
        *bits_out |= stego_tx_next_bit(st) << *nbits_out;
        (*nbits_out)++;
    }
}

static int stego_rx_write_bits(FILE *f, int bits, int nbits, unsigned char *partial_byte, int *partial_bits)
{
    int i;
    for (i = 0; i < nbits; i++) {
        int bit = (bits >> i) & 1;
        *partial_byte |= (unsigned char)(bit << *partial_bits);
        (*partial_bits)++;
        if (*partial_bits == 8) {
            if (fwrite(partial_byte, 1, 1, f) != 1) return 0;
            *partial_byte = 0;
            *partial_bits = 0;
        }
    }
    return 1;
}

static int run_encode(int argc, char **argv)
{
    int err;
    int rate;
    int channels;
    int bitrate_bps;
    int cbr = 0;
    int stego_bps = STEGO_DEFAULT_BPS;
    int stego_seq = 0;
    const char *stego_in_file = NULL;
    const char *pcm_in_path;
    const char *bit_out_path;
    int argi;
    FILE *fin = NULL;
    FILE *fout = NULL;
    OpusEncoder *enc = NULL;
    opus_int16 *in_pcm = NULL;
    unsigned char packet[MAX_PACKET_SIZE];
    int frame_size;
    unsigned char *stego_in_buf = NULL;
    opus_int32 stego_in_len = 0;
    StegoTxState stego_tx;
    opus_int64 stego_budget_acc = 0;
    opus_int64 frames = 0;
    int frame_seq_counter = 0;

    if (argc < 8) {
        return 1;
    }
    if (strcmp(argv[2], "voip") != 0) {
        fprintf(stderr, "Only voip mode is supported in this demo.\n");
        return 1;
    }
    rate = atoi(argv[3]);
    channels = atoi(argv[4]);
    bitrate_bps = atoi(argv[5]);
    argi = 6;
    while (argi < argc - 2) {
        if (strcmp(argv[argi], "-cbr") == 0) {
            cbr = 1;
            argi++;
        } else if (strcmp(argv[argi], "-stego_in") == 0 && argi + 1 < argc - 1) {
            stego_in_file = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "-stego_bps") == 0 && argi + 1 < argc - 1) {
            stego_bps = atoi(argv[argi + 1]);
            argi += 2;
        } else if (strcmp(argv[argi], "-stego_seq") == 0) {
            stego_seq = 1;
            argi++;
        } else {
            fprintf(stderr, "Unknown encode option: %s\n", argv[argi]);
            return 1;
        }
    }
    pcm_in_path = argv[argc - 2];
    bit_out_path = argv[argc - 1];

    if (rate <= 0 || channels <= 0 || channels > 2 || bitrate_bps <= 0) {
        fprintf(stderr, "Invalid rate/channels/bitrate.\n");
        return 1;
    }
    frame_size = rate / 50;
    if (frame_size <= 0) {
        fprintf(stderr, "Unsupported frame size for rate=%d.\n", rate);
        return 1;
    }

    if (stego_in_file) {
        if (!stego_load_file(stego_in_file, &stego_in_buf, &stego_in_len)) {
            fprintf(stderr, "Failed to read stego input: %s\n", stego_in_file);
            return 1;
        }
    }
    stego_tx.input = stego_in_buf;
    stego_tx.input_len = stego_in_len;
    stego_tx.input_pos = 0;
    stego_tx.embedded_bits = 0;

    fin = fopen(pcm_in_path, "rb");
    if (!fin) {
        fprintf(stderr, "Failed to open PCM input: %s\n", pcm_in_path);
        free(stego_in_buf);
        return 1;
    }
    fout = fopen(bit_out_path, "wb");
    if (!fout) {
        fprintf(stderr, "Failed to open bitstream output: %s\n", bit_out_path);
        fclose(fin);
        free(stego_in_buf);
        return 1;
    }

    enc = opus_encoder_create(rate, channels, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        fprintf(stderr, "opus_encoder_create failed: %d\n", err);
        fclose(fin);
        fclose(fout);
        free(stego_in_buf);
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate_bps));
    if (cbr) {
        opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    }

    in_pcm = (opus_int16*)malloc((size_t)frame_size * channels * sizeof(opus_int16));
    if (!in_pcm) {
        fprintf(stderr, "Out of memory.\n");
        opus_encoder_destroy(enc);
        fclose(fin);
        fclose(fout);
        free(stego_in_buf);
        return 1;
    }

    while (1) {
        size_t got = fread(in_pcm, sizeof(opus_int16), (size_t)frame_size * channels, fin);
        int bits = 0, nbits = 0;
        int stego_ctl = 0;
        int packet_len;
        int applied_ctl = 0;
        int applied_nbits;
        opus_uint32 final_range = 0;
        size_t i;

        if (got == 0) break;
        if (got < (size_t)frame_size * channels) {
            for (i = got; i < (size_t)frame_size * channels; i++) in_pcm[i] = 0;
        }

        if (stego_in_buf && stego_tx_has_next_bit(&stego_tx)) {
            int allow = stego_budget_allow_bits(&stego_budget_acc, stego_bps, frame_size, rate, STEGO_FRAME_MAX_BITS);
            opus_int32 saved_pos = stego_tx.input_pos;
            int payload_nbits = 0;
            if (stego_seq) {
                /* Seq mode: read up to 4 payload bits, prepend 2-bit seq counter. */
                int payload_bits = 0;
                int max_payload = STEGO_PAYLOAD_BITS;
                if (max_payload > allow - STEGO_SEQ_BITS) max_payload = allow - STEGO_SEQ_BITS;
                if (max_payload < 0) max_payload = 0;
                stego_tx_prepare_frame_bits(&stego_tx, max_payload, &payload_bits, &payload_nbits);
                nbits = payload_nbits + STEGO_SEQ_BITS;
                bits = payload_bits | ((frame_seq_counter & ((1 << STEGO_SEQ_BITS) - 1)) << payload_nbits);
                frame_seq_counter++;
            } else {
                stego_tx_prepare_frame_bits(&stego_tx, allow, &bits, &nbits);
            }
            stego_ctl = ((nbits & 0xFF) << 8) | (bits & 0xFF);
            opus_encoder_ctl(enc, OPUS_SET_STEGO_BITS(stego_ctl));

            packet_len = opus_encode(enc, in_pcm, frame_size, packet, MAX_PACKET_SIZE);
            if (packet_len < 0) {
                fprintf(stderr, "opus_encode failed: %d\n", packet_len);
                free(in_pcm);
                opus_encoder_destroy(enc);
                fclose(fin);
                fclose(fout);
                free(stego_in_buf);
                return 1;
            }

            opus_encoder_ctl(enc, OPUS_GET_STEGO_BITS(&applied_ctl));
            applied_nbits = (applied_ctl >> 8) & 0xFF;
            /* Only consume payload bits from the secret, not seq bits. */
            if (stego_seq) {
                int payload_applied = applied_nbits - STEGO_SEQ_BITS;
                if (payload_applied < 0) payload_applied = 0;
                if (payload_applied > payload_nbits) payload_applied = payload_nbits;
                stego_tx.input_pos = saved_pos + payload_applied;
                stego_tx.embedded_bits += payload_applied;
            } else {
                stego_tx.input_pos = saved_pos + applied_nbits;
                stego_tx.embedded_bits += applied_nbits;
            }
            /* Return unused budget to the accumulator. */
            stego_budget_acc += (opus_int64)(allow - applied_nbits) * rate;
        } else {
            packet_len = opus_encode(enc, in_pcm, frame_size, packet, MAX_PACKET_SIZE);
            if (packet_len < 0) {
                fprintf(stderr, "opus_encode failed: %d\n", packet_len);
                free(in_pcm);
                opus_encoder_destroy(enc);
                fclose(fin);
                fclose(fout);
                free(stego_in_buf);
                return 1;
            }
        }

        opus_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&final_range));
        if (!write_u32_be(fout, (opus_uint32)packet_len) ||
            !write_u32_be(fout, final_range) ||
            fwrite(packet, 1, (size_t)packet_len, fout) != (size_t)packet_len) {
            fprintf(stderr, "Failed writing bitstream.\n");
            free(in_pcm);
            opus_encoder_destroy(enc);
            fclose(fin);
            fclose(fout);
            free(stego_in_buf);
            return 1;
        }
        frames++;
    }

    fprintf(stderr, "Encoded %lld frames.\n", (long long)frames);
    if (stego_in_buf) {
        fprintf(stderr, "Stego embedded bits: %lld.\n", (long long)stego_tx.embedded_bits);
    }

    free(in_pcm);
    opus_encoder_destroy(enc);
    fclose(fin);
    fclose(fout);
    free(stego_in_buf);
    return 0;
}

static int run_decode(int argc, char **argv)
{
    int err;
    int rate;
    int channels;
    int stego_bps = STEGO_DEFAULT_BPS;
    int stego_seq = 0;
    const char *stego_out_file = NULL;
    const char *bit_in_path;
    const char *pcm_out_path;
    int argi;
    FILE *fin = NULL;
    FILE *fout = NULL;
    FILE *fstego = NULL;
    OpusDecoder *dec = NULL;
    opus_int16 *out_pcm = NULL;
    unsigned char *packet = NULL;
    int packet_cap = MAX_PACKET_SIZE;
    int frame_size;
    opus_int64 stego_budget_acc = 0;
    unsigned char partial_byte = 0;
    int partial_bits = 0;
    opus_int64 extracted_bits = 0;
    opus_int64 frames = 0;
    int rx_expected_seq = 0;
    int rx_seq_inited = 0;
    int rx_nibble_pos = 0;

    if (argc < 6) {
        return 1;
    }
    rate = atoi(argv[2]);
    channels = atoi(argv[3]);
    argi = 4;
    while (argi < argc - 2) {
        if (strcmp(argv[argi], "-stego_out") == 0 && argi + 1 < argc - 1) {
            stego_out_file = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "-stego_bps") == 0 && argi + 1 < argc - 1) {
            stego_bps = atoi(argv[argi + 1]);
            argi += 2;
        } else if (strcmp(argv[argi], "-stego_seq") == 0) {
            stego_seq = 1;
            argi++;
        } else {
            fprintf(stderr, "Unknown decode option: %s\n", argv[argi]);
            return 1;
        }
    }
    bit_in_path = argv[argc - 2];
    pcm_out_path = argv[argc - 1];

    if (rate <= 0 || channels <= 0 || channels > 2) {
        fprintf(stderr, "Invalid rate/channels.\n");
        return 1;
    }
    frame_size = rate / 50;
    if (frame_size <= 0) {
        fprintf(stderr, "Unsupported frame size for rate=%d.\n", rate);
        return 1;
    }

    fin = fopen(bit_in_path, "rb");
    if (!fin) {
        fprintf(stderr, "Failed to open bitstream input: %s\n", bit_in_path);
        return 1;
    }
    fout = fopen(pcm_out_path, "wb");
    if (!fout) {
        fprintf(stderr, "Failed to open PCM output: %s\n", pcm_out_path);
        fclose(fin);
        return 1;
    }
    if (stego_out_file) {
        fstego = fopen(stego_out_file, "wb");
        if (!fstego) {
            fprintf(stderr, "Failed to open stego output: %s\n", stego_out_file);
            fclose(fin);
            fclose(fout);
            return 1;
        }
    }

    dec = opus_decoder_create(rate, channels, &err);
    if (!dec || err != OPUS_OK) {
        fprintf(stderr, "opus_decoder_create failed: %d\n", err);
        if (fstego) fclose(fstego);
        fclose(fin);
        fclose(fout);
        return 1;
    }

    out_pcm = (opus_int16*)malloc((size_t)frame_size * channels * sizeof(opus_int16));
    packet = (unsigned char*)malloc((size_t)packet_cap);
    if (!out_pcm || !packet) {
        fprintf(stderr, "Out of memory.\n");
        free(out_pcm);
        free(packet);
        opus_decoder_destroy(dec);
        if (fstego) fclose(fstego);
        fclose(fin);
        fclose(fout);
        return 1;
    }

    while (1) {
        opus_uint32 pkt_len_u32, final_range;
        int pkt_len;
        int decoded_samples;
        int stego_ctl = 0;
        int bits, nbits, allow;

        if (!read_u32_be(fin, &pkt_len_u32)) break;
        if (!read_u32_be(fin, &final_range)) {
            fprintf(stderr, "Malformed bitstream header.\n");
            free(out_pcm);
            free(packet);
            opus_decoder_destroy(dec);
            if (fstego) fclose(fstego);
            fclose(fin);
            fclose(fout);
            return 1;
        }
        (void)final_range;
        pkt_len = (int)pkt_len_u32;
        if (pkt_len < 0 || pkt_len > 10000000) {
            fprintf(stderr, "Invalid packet length in stream: %d\n", pkt_len);
            free(out_pcm);
            free(packet);
            opus_decoder_destroy(dec);
            if (fstego) fclose(fstego);
            fclose(fin);
            fclose(fout);
            return 1;
        }
        if (pkt_len > packet_cap) {
            unsigned char *new_buf;
            packet_cap = pkt_len;
            new_buf = (unsigned char*)realloc(packet, (size_t)packet_cap);
            if (!new_buf) {
                fprintf(stderr, "Out of memory reallocating packet buffer.\n");
                free(out_pcm);
                free(packet);
                opus_decoder_destroy(dec);
                if (fstego) fclose(fstego);
                fclose(fin);
                fclose(fout);
                return 1;
            }
            packet = new_buf;
        }
        if (fread(packet, 1, (size_t)pkt_len, fin) != (size_t)pkt_len) {
            fprintf(stderr, "Truncated packet payload.\n");
            free(out_pcm);
            free(packet);
            opus_decoder_destroy(dec);
            if (fstego) fclose(fstego);
            fclose(fin);
            fclose(fout);
            return 1;
        }

        decoded_samples = opus_decode(dec, packet, pkt_len, out_pcm, frame_size, 0);
        if (decoded_samples < 0) {
            fprintf(stderr, "opus_decode failed: %d\n", decoded_samples);
            free(out_pcm);
            free(packet);
            opus_decoder_destroy(dec);
            if (fstego) fclose(fstego);
            fclose(fin);
            fclose(fout);
            return 1;
        }
        if (fwrite(out_pcm, sizeof(opus_int16), (size_t)decoded_samples * channels, fout) != (size_t)decoded_samples * channels) {
            fprintf(stderr, "Failed writing PCM output.\n");
            free(out_pcm);
            free(packet);
            opus_decoder_destroy(dec);
            if (fstego) fclose(fstego);
            fclose(fin);
            fclose(fout);
            return 1;
        }

        if (fstego) {
            allow = stego_budget_allow_bits(&stego_budget_acc, stego_bps, frame_size, rate, STEGO_FRAME_MAX_BITS);
            opus_decoder_ctl(dec, OPUS_GET_STEGO_BITS(&stego_ctl));
            bits = stego_ctl & 0xFF;
            nbits = (stego_ctl >> 8) & 0xFF;
            if (nbits > allow) nbits = allow;
            if (stego_seq && nbits >= STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS) {
                /* Seq mode: split each 6-bit frame into [seq:2][payload:4]. */
                int pos = 0;
                while (pos + STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS <= nbits) {
                    int frame_bits = (bits >> pos) & ((1 << (STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS)) - 1);
                    int seq = frame_bits >> STEGO_PAYLOAD_BITS;
                    int payload = frame_bits & ((1 << STEGO_PAYLOAD_BITS) - 1);
                    /* Fill gaps from lost frames with zero payload. */
                    if (rx_seq_inited) {
                        int seq_mask = (1 << STEGO_SEQ_BITS) - 1;
                        int gap_count = 0;
                        while (seq != rx_expected_seq) {
                            if (!stego_rx_write_bits(fstego, 0, STEGO_PAYLOAD_BITS,
                                                     &partial_byte, &partial_bits))
                                return 1;
                            extracted_bits += STEGO_PAYLOAD_BITS;
                            rx_expected_seq = (rx_expected_seq + 1) & seq_mask;
                            gap_count++;
                        }
                        if (gap_count > 0) {
                            fprintf(stderr, "SEQ GAP: %d frames at nibble %d\n",
                                    gap_count, rx_nibble_pos);
                            rx_nibble_pos += gap_count;
                        }
                    }
                    if (!stego_rx_write_bits(fstego, payload, STEGO_PAYLOAD_BITS,
                                             &partial_byte, &partial_bits)) {
                        fprintf(stderr, "Failed writing stego output (seq).\n");
                        return 1;
                    }
                    extracted_bits += STEGO_PAYLOAD_BITS;
                    rx_nibble_pos++;
                    if (!rx_seq_inited) {
                        rx_expected_seq = seq;
                        rx_seq_inited = 1;
                    }
                    rx_expected_seq = (rx_expected_seq + 1) & ((1 << STEGO_SEQ_BITS) - 1);
                    pos += STEGO_SEQ_BITS + STEGO_PAYLOAD_BITS;
                }
            } else if (nbits > 0) {
                if (!stego_rx_write_bits(fstego, bits, nbits, &partial_byte, &partial_bits)) {
                    fprintf(stderr, "Failed writing stego output.\n");
                    free(out_pcm);
                    free(packet);
                    opus_decoder_destroy(dec);
                    if (fstego) fclose(fstego);
                    fclose(fin);
                    fclose(fout);
                    return 1;
                }
                extracted_bits += nbits;
            }
        }
        frames++;
    }

    if (fstego && partial_bits > 0) {
        fwrite(&partial_byte, 1, 1, fstego);
    }
    fprintf(stderr, "Decoded %lld frames.\n", (long long)frames);
    if (fstego) {
        fprintf(stderr, "Stego extracted bits: %lld.\n", (long long)extracted_bits);
    }

    free(out_pcm);
    free(packet);
    opus_decoder_destroy(dec);
    if (fstego) fclose(fstego);
    fclose(fin);
    fclose(fout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "-e") == 0) {
        if (run_encode(argc, argv) != 0) {
            usage(argv[0]);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "-d") == 0) {
        if (run_decode(argc, argv) != 0) {
            usage(argv[0]);
            return 1;
        }
        return 0;
    }
    usage(argv[0]);
    return 1;
}
