#!/usr/bin/env python3
import argparse
import array
import math
from pathlib import Path


def read_pcm_s16le(path: Path):
    data = array.array("h")
    with path.open("rb") as f:
        data.frombytes(f.read())
    return data


def file_size(path: Path):
    return path.stat().st_size


def parse_opus_demo_payload_bytes(path: Path):
    data = path.read_bytes()
    off = 0
    total_payload = 0
    while off + 8 <= len(data):
        pkt_len = int.from_bytes(data[off:off + 4], "big", signed=False)
        off += 4
        off += 4  # final range
        if pkt_len < 0 or off + pkt_len > len(data):
            # Not an opus_demo encoded stream, fallback to raw size.
            return None
        total_payload += pkt_len
        off += pkt_len
    if off != len(data):
        return None
    return total_payload


def mse_for_shift(src, dst, shift):
    if shift >= 0:
        s0 = 0
        d0 = shift
    else:
        s0 = -shift
        d0 = 0
    n = min(len(src) - s0, len(dst) - d0)
    if n <= 0:
        return None, 0, s0, d0
    acc = 0.0
    for i in range(n):
        d = int(src[s0 + i]) - int(dst[d0 + i])
        acc += d * d
    return acc / n, n, s0, d0


def main():
    p = argparse.ArgumentParser(description="Compute task-focused Opus stego metrics.")
    p.add_argument("--baseline-pcm", required=True, help="Decoded PCM from normal codec (s16le)")
    p.add_argument("--stego-pcm", required=True, help="Decoded PCM from modified codec (s16le)")
    p.add_argument("--sample-rate", type=int, required=True, help="Sample rate, e.g. 48000")
    p.add_argument("--channels", type=int, default=1, help="Channel count, default 1")
    p.add_argument("--bitstream", help="Encoded Opus bitstream path for bitrate estimation")
    p.add_argument("--recovered", help="Recovered hidden payload file")
    p.add_argument("--target-kbps", type=float, default=6.0, help="Target Opus bitrate kb/s")
    p.add_argument("--target-bytes-per-sec", type=float, default=31.25, help="Target stego throughput B/s")
    p.add_argument("--target-psnr", type=float, default=40.0, help="Target minimum PSNR dB")
    p.add_argument("--max-shift", type=int, default=1200,
                   help="Max sample shift (±) to align baseline vs stego PCM before MSE; 0 disables search (strictest)")
    args = p.parse_args()

    baseline = read_pcm_s16le(Path(args.baseline_pcm))
    stego = read_pcm_s16le(Path(args.stego_pcm))
    if len(baseline) == 0 or len(stego) == 0:
        raise SystemExit("PCM input is empty.")
    if args.max_shift == 0:
        mse, n, s0, d0 = mse_for_shift(baseline, stego, 0)
        if mse is None:
            raise SystemExit("No overlapping samples after alignment.")
        best, best_shift, best_n, best_s0, best_d0 = mse, 0, n, s0, d0
    else:
        best = None
        best_shift = 0
        best_n = 0
        best_s0 = 0
        best_d0 = 0
        for shift in range(-args.max_shift, args.max_shift + 1):
            mse, n, s0, d0 = mse_for_shift(baseline, stego, shift)
            if mse is None:
                continue
            if best is None or mse < best:
                best = mse
                best_shift = shift
                best_n = n
                best_s0 = s0
                best_d0 = d0
        if best is None:
            raise SystemExit("No overlapping samples after alignment.")
    mse = best
    n = best_n
    peak = 32767.0
    psnr = float("inf") if mse == 0 else 10.0 * math.log10((peak * peak) / mse)

    duration_s = n / float(args.sample_rate * args.channels)
    total_duration_s = len(stego) / float(args.sample_rate * args.channels)

    opus_kbps = None
    stego_bytes_per_s = None

    if args.bitstream:
        bitstream_path = Path(args.bitstream)
        payload_bytes = parse_opus_demo_payload_bytes(bitstream_path)
        bitstream_size = payload_bytes if payload_bytes is not None else bitstream_path.stat().st_size
        opus_bps = (bitstream_size * 8.0) / total_duration_s if total_duration_s > 0 else 0.0
        opus_kbps = opus_bps / 1000.0

    if args.recovered:
        rec_size = file_size(Path(args.recovered))
        stego_bytes_per_s = rec_size / total_duration_s if total_duration_s > 0 else 0.0

    print("=== Task Metrics ===")
    print("PSNR reference: baseline decoded PCM vs stego decoded PCM")
    print("PSNR: inf dB" if math.isinf(psnr) else f"PSNR: {psnr:.3f} dB")
    if opus_kbps is not None:
        print(f"Opus bitrate: {opus_kbps:.3f} kb/s")
    if stego_bytes_per_s is not None:
        print(f"Stego info rate: {stego_bytes_per_s:.3f} B/s")

    print("\n=== Target Check ===")
    psnr_ok = math.isinf(psnr) or psnr >= args.target_psnr
    print(f"PSNR >= {args.target_psnr:.1f} dB: {'PASS' if psnr_ok else 'FAIL'}")
    if opus_kbps is not None:
        bitrate_ok = abs(opus_kbps - args.target_kbps) <= 0.5
        print(f"Opus bitrate ~= {args.target_kbps:.1f} kb/s: {'PASS' if bitrate_ok else 'FAIL'}")
    if stego_bytes_per_s is not None:
        stego_ok = stego_bytes_per_s >= args.target_bytes_per_sec
        print(f"Stego rate >= {args.target_bytes_per_sec:.1f} B/s: {'PASS' if stego_ok else 'FAIL'}")


if __name__ == "__main__":
    main()
