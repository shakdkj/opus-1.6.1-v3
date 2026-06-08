/***********************************************************************
 * Unified watermark test platform for Opus SILK gain-matrix stego.
 *
 * Usage:
 *   test_watermark --input in.wav --bitrate 6000 --bps 300 [--loss 0.15]
 *
 * Builds a single executable that invokes WatermarkEncode / Decode
 * through the opuswatermark shared library.
 ***********************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "watermark.h"

/* ---- WAV reader (16-bit PCM, mono/stereo → mono) ---- */
static int16_t *read_wav(const char *path, size_t *samples_out, int *sr_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    /* Read RIFF header */
    uint8_t header[44];
    if (fread(header, 1, 44, f) != 44) { fclose(f); return NULL; }

    int channels   = header[22] | (header[23] << 8);
    int sample_rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    int bits_sample = header[34] | (header[35] << 8);
    int data_size   = header[40] | (header[41] << 8) | (header[42] << 16) | (header[43] << 24);

    if (bits_sample != 16) { fclose(f); return NULL; }

    int total_samples = data_size / 2;
    int16_t *raw = (int16_t *)malloc((size_t)total_samples * sizeof(int16_t));
    if (!raw) { fclose(f); return NULL; }
    fread(raw, 2, (size_t)total_samples, f);
    fclose(f);

    if (channels == 2) {
        /* Stereo → mono: average */
        int mono_samples = total_samples / 2;
        int16_t *mono = (int16_t *)malloc((size_t)mono_samples * sizeof(int16_t));
        for (int i = 0; i < mono_samples; i++)
            mono[i] = (int16_t)(((int)raw[i*2] + (int)raw[i*2+1]) / 2);
        free(raw);
        raw = mono;
        total_samples = mono_samples;
    }

    *samples_out = (size_t)total_samples;
    *sr_out = sample_rate;
    return raw;
}

/* ---- Random bytes ---- */
static void rand_bytes(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(rand() & 0xFF);
}

/* ---- PSNR ---- */
static double compute_psnr(const int16_t *a, const int16_t *b, size_t n)
{
    double mse = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse == 0.0) return 999.0;
    return 10.0 * log10(32767.0 * 32767.0 / mse);
}

/* ---- Bit accuracy ---- */
static double compute_bit_acc(const uint8_t *a, const uint8_t *b, size_t n)
{
    if (n == 0) return 1.0;
    size_t ok = 0;
    for (size_t i = 0; i < n; i++)
        ok += (size_t)(8 - __builtin_popcount(a[i] ^ b[i]));
    return (double)ok / (double)(n * 8);
}

/* ---- CLI ---- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --input <wav> [options]\n"
        "Options:\n"
        "  --bitrate N    Opus bitrate bps (default 6000)\n"
        "  --bps N        Stego embedding bps (default 300)\n"
        "  --loss 0.15    Packet loss rate (default 0, no loss)\n"
        "  --seed N       Random seed (default 42)\n"
        "  --watermark N  Watermark size in bytes (default 32)\n"
        "  --resilience   Enable seq+FEC loss resilience\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    int bitrate = 6000, stego_bps = 300, seed = 42;
    double loss_rate = 0.0;
    int watermark_size = 32;
    int resilience = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--input") && i + 1 < argc)
            input_path = argv[++i];
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc)
            bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bps") && i + 1 < argc)
            stego_bps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc)
            loss_rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watermark") && i + 1 < argc)
            watermark_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--resilience"))
            resilience = 1;
        else {
            usage(argv[0]); return 1;
        }
    }
    if (!input_path) { usage(argv[0]); return 1; }

    srand((unsigned)seed);

    /* ---- Read WAV ---- */
    size_t pcm_len = 0;
    int sr = 0;
    int16_t *pcm_orig = read_wav(input_path, &pcm_len, &sr);
    if (!pcm_orig) { fprintf(stderr, "Cannot read %s\n", input_path); return 1; }
    printf("# input: %s, samples=%zu, sr=%d\n", input_path, pcm_len, sr);

    /* ---- Generate watermark ---- */
    uint8_t *wm_in = (uint8_t *)malloc((size_t)watermark_size);
    rand_bytes(wm_in, (size_t)watermark_size);

    /* ---- Encode ---- */
    size_t opus_cap = pcm_len * 2;  /* generous */
    uint8_t *opus_stream = (uint8_t *)malloc(opus_cap);
    size_t opus_len = opus_cap;
    int64_t enc_us = 0;

    int ret = WatermarkEncode(pcm_orig, pcm_len,
                               wm_in, (size_t)watermark_size,
                               bitrate, stego_bps,
                               resilience ? WATERMARK_LOSS_SEQ_FEC : WATERMARK_LOSS_NONE,
                               opus_stream, &opus_len, &enc_us);
    if (ret != WATERMARK_OK) {
        fprintf(stderr, "WatermarkEncode failed: %d\n", ret);
        free(pcm_orig); free(wm_in); free(opus_stream);
        return 1;
    }
    printf("# encode: opus_bytes=%zu, time_us=%lld\n",
           (unsigned long long)opus_len, (long long)enc_us);

    /* ---- (Optional) Simulate packet loss ---- */
    if (loss_rate > 0.0) {
        uint8_t *tmp = (uint8_t *)malloc(opus_len);
        size_t out_pos = 0;
        const uint8_t *src = opus_stream;
        size_t remaining = opus_len;
        int dropped = 0, total = 0;
        while (remaining >= 8) {
            uint32_t pkt_len = (uint32_t)src[0] << 24 | (uint32_t)src[1] << 16 |
                               (uint32_t)src[2] << 8 | (uint32_t)src[3];
            size_t pkt_total = 8 + (size_t)pkt_len;
            if (pkt_total > remaining) break;
            total++;
            if ((double)rand() / RAND_MAX >= loss_rate) {
                memcpy(tmp + out_pos, src, pkt_total);
                out_pos += pkt_total;
            } else {
                dropped++;
            }
            src += pkt_total;
            remaining -= pkt_total;
        }
        printf("# loss_sim: total_pkts=%d, dropped=%d (%.1f%%)\n",
               total, dropped, 100.0 * (double)dropped / (double)total);
        memcpy(opus_stream, tmp, out_pos);
        opus_len = out_pos;
        free(tmp);
    }

    /* ---- Decode ---- */
    size_t pcm_out_cap = pcm_len + 960;
    int16_t *pcm_dec = (int16_t *)calloc(pcm_out_cap, sizeof(int16_t));
    size_t pcm_out_len = pcm_out_cap;
    uint8_t *wm_out = (uint8_t *)calloc((size_t)watermark_size + 1, 1);
    size_t wm_out_len = (size_t)watermark_size;
    int64_t dec_us = 0;

    ret = WatermarkDecode(opus_stream, opus_len,
                           bitrate, stego_bps,
                           resilience ? WATERMARK_LOSS_SEQ_FEC : WATERMARK_LOSS_NONE,
                           NULL, 0,
                           pcm_dec, &pcm_out_len,
                           wm_out, &wm_out_len, &dec_us);
    if (ret != WATERMARK_OK) {
        fprintf(stderr, "WatermarkDecode failed: %d\n", ret);
        free(pcm_orig); free(wm_in); free(opus_stream);
        free(pcm_dec); free(wm_out);
        return 1;
    }
    printf("# decode: pcm_samples=%zu, wm_bytes=%zu, time_us=%lld\n",
           (unsigned long long)pcm_out_len, (unsigned long long)wm_out_len,
           (long long)dec_us);

    /* ---- Metrics ---- */
    size_t cmp_samples = pcm_len < pcm_out_len ? pcm_len : pcm_out_len;
    double psnr = compute_psnr(pcm_orig, pcm_dec, cmp_samples);
    double bit_acc = compute_bit_acc(wm_in, wm_out,
        (size_t)watermark_size < wm_out_len ? (size_t)watermark_size : wm_out_len);

    printf("PSNR: %.3f dB\n", psnr);
    printf("bit_acc: %.4f\n", bit_acc);

    free(pcm_orig); free(wm_in); free(opus_stream);
    free(pcm_dec); free(wm_out);
    return 0;
}
