/***********************************************************************
 * Watermark API: SILK gain-matrix steganography for Opus.
 *
 * Provides clean WatermarkEncode / WatermarkDecode interfaces suitable
 * for integration with a unified cross-codec test platform.
 *
 * Reference: "交付代码规范.md" Requirement 6
 ***********************************************************************/
#ifndef WATERMARK_H
#define WATERMARK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Return codes ---- */
#define WATERMARK_OK             0
#define WATERMARK_ERR_NULL       -1   /* NULL pointer argument */
#define WATERMARK_ERR_ENCODE     -2   /* Opus encode failed */
#define WATERMARK_ERR_DECODE     -3   /* Opus decode failed */
#define WATERMARK_ERR_MEM        -4   /* Allocation failed */

/* ---- Opus application mode ---- */
#define WATERMARK_OPUS_MODE_VOIP   2048
#define WATERMARK_OPUS_MODE_AUDIO  2049
#define WATERMARK_OPUS_MODE_RESTRICTED_LOWDELAY  2051

/* ---- Loss-resilience mode ---- */
#define WATERMARK_LOSS_NONE      0    /* No seq header / FEC: max capacity */
#define WATERMARK_LOSS_SEQ_FEC   1    /* [seq:2][payload:4] + XOR-3 FEC */

/**
 * Embed watermark bytes into PCM audio, producing a standard Opus bitstream.
 *
 * The function runs the full Opus encode pipeline internally. When
 * `watermarkLen == 0` the output bitstream is bit-exact identical to a
 * normal Opus encode (requirement 1).
 *
 * @param pcmStream         Input PCM samples (s16le, 48000 Hz, mono).
 * @param pcmLen            Number of samples in pcmStream.
 * @param watermarkBytes    Secret payload to embed (may be NULL if len==0).
 * @param watermarkLen      Payload length in bytes.
 * @param bitrate           Opus target bitrate in bps (e.g. 6000).
 * @param stegoBps          Stego embedding rate in bps (e.g. 300 → 6 bit/frame).
 * @param lossMode          WATERMARK_LOSS_NONE or WATERMARK_LOSS_SEQ_FEC.
 * @param opusStream        Output buffer for the encoded Opus bitstream.
 * @param opusLen           [in] capacity of opusStream, [out] actual length.
 * @param encodeTimeUs      [out] Encode wall-clock time in microseconds (may be NULL).
 * @return                  WATERMARK_OK (0) on success, negative on error.
 */
int WatermarkEncode(
    const int16_t *pcmStream,
    size_t         pcmLen,
    const uint8_t *watermarkBytes,
    size_t         watermarkLen,
    int            bitrate,
    int            stegoBps,
    int            lossMode,
    uint8_t       *opusStream,
    size_t        *opusLen,
    int64_t       *encodeTimeUs
);

/**
 * Extract watermark bytes from an Opus bitstream while decoding to PCM.
 *
 * When `watermarkBytes == NULL` or `watermarkLen == 0` this acts as a
 * normal Opus decode (no extraction overhead).
 *
 * @param opusStream        Input Opus bitstream.
 * @param opusLen           Bitstream length in bytes.
 * @param bitrate           Opus bitrate (informational, for budget sync).
 * @param stegoBps          Stego embedding rate in bps.
 * @param lossMode          WATERMARK_LOSS_NONE or WATERMARK_LOSS_SEQ_FEC.
 * @param erasureFlags      Per-frame loss flag array (NULL = no loss, length = numFrames).
 *                          When non-NULL: erasureFlags[i]==1 means frame i was lost.
 * @param numFrames         Total number of frames in the stream (pass 0 for auto-detect).
 * @param pcmStream         Output buffer for decoded PCM.
 * @param pcmLen            [in] capacity in samples, [out] actual decoded samples.
 * @param watermarkBytes    Output buffer for recovered payload.
 * @param watermarkLen      [in] capacity in bytes, [out] actual recovered bytes.
 * @param decodeTimeUs      [out] Decode wall-clock time in microseconds (may be NULL).
 * @return                  WATERMARK_OK (0) on success, negative on error.
 */
int WatermarkDecode(
    const uint8_t *opusStream,
    size_t         opusLen,
    int            bitrate,
    int            stegoBps,
    int            lossMode,
    const int     *erasureFlags,
    int            numFrames,
    int16_t       *pcmStream,
    size_t        *pcmLen,
    uint8_t       *watermarkBytes,
    size_t        *watermarkLen,
    int64_t       *decodeTimeUs
);

#ifdef __cplusplus
}
#endif
#endif /* WATERMARK_H */
