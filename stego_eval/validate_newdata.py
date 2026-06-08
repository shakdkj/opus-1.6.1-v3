import argparse
import json
import numpy as np
import random
import subprocess
import sys
import time
import wave
from pathlib import Path


def default_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run_cmd(cmd, timeout_sec=120):
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout_sec,
        check=True,
    )


def wav_to_pcm_s16le(wav_path: Path, pcm_path: Path, target_rate: int = 48000, target_channels: int = 1):
    with wave.open(str(wav_path), "rb") as wf:
        channels = wf.getnchannels()
        sample_rate = wf.getframerate()
        sampwidth = wf.getsampwidth()
        frames = wf.readframes(wf.getnframes())
    if sampwidth != 2:
        raise ValueError(f"{wav_path.name}: only 16-bit WAV is supported, got {sampwidth * 8}-bit")
    if channels not in (1, 2):
        raise ValueError(f"{wav_path.name}: only mono/stereo WAV is supported, got {channels} channels")

    pcm = np.frombuffer(frames, dtype="<i2")
    if channels > 1:
        pcm = pcm.reshape(-1, channels)

    if channels == 2 and target_channels == 1:
        pcm = np.rint((pcm[:, 0].astype(np.float64) + pcm[:, 1].astype(np.float64)) * 0.5).astype(np.int16)
        channels = 1
    elif channels != target_channels:
        raise ValueError(f"{wav_path.name}: unsupported channel conversion {channels} -> {target_channels}")

    if channels == 1 and pcm.ndim != 1:
        pcm = pcm.reshape(-1)

    if sample_rate != target_rate:
        src = pcm.astype(np.float64)
        new_len = max(1, int(round(len(src) * float(target_rate) / float(sample_rate))))
        src_x = np.arange(len(src), dtype=np.float64)
        dst_x = np.linspace(0.0, float(max(0, len(src) - 1)), num=new_len, dtype=np.float64)
        pcm = np.rint(np.interp(dst_x, src_x, src)).clip(-32768, 32767).astype(np.int16)
        sample_rate = target_rate

    pcm_path.write_bytes(pcm.tobytes())
    return sample_rate, channels


def pcm_to_wav_s16le(pcm_path: Path, wav_path: Path, sample_rate: int, channels: int):
    data = pcm_path.read_bytes()
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(data)


def parse_metric_value(stdout: str, key: str):
    for line in stdout.splitlines():
        if key in line:
            return line.split(":", 1)[1].strip()
    return "N/A"


def parse_float_prefix(text: str):
    if text == "N/A":
        return None
    try:
        return float(text.split()[0])
    except Exception:
        return None


def compute_bit_accuracy(secret_path: Path, recovered_path: Path, embedded_bits=None):
    secret = secret_path.read_bytes()
    recovered = recovered_path.read_bytes()
    n = min(len(secret), len(recovered))
    if n <= 0:
        return 0.0
    if embedded_bits is not None and embedded_bits > 0:
        full_bytes = embedded_bits // 8
        rem_bits = embedded_bits % 8
        total_bits = embedded_bits
        ok_bits = 0
        for i in range(min(n, full_bytes)):
            ok_bits += 8 - bin(secret[i] ^ recovered[i]).count("1")
        if rem_bits > 0 and full_bytes < n:
            mask = (1 << rem_bits) - 1
            ok_bits += rem_bits - bin((secret[full_bytes] ^ recovered[full_bytes]) & mask).count("1")
        return ok_bits / total_bits
    total_bits = n * 8
    ok_bits = 0
    for i in range(n):
        ok_bits += 8 - bin(secret[i] ^ recovered[i]).count("1")
    return ok_bits / total_bits


def build_parser():
    parser = argparse.ArgumentParser(
        description="One-shot SILK stego validation for WAV corpus."
    )
    repo = default_repo_root()
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=repo / "stego_eval" / "sample_10",
        help="Directory containing WAV files to test.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=repo / "stego_eval" / "output",
        help="Output directory for generated files.",
    )
    parser.add_argument(
        "--opus-demo",
        type=Path,
        default=repo / "build" / "Debug" / "opus_demo.exe",
        help="Path to opus_demo executable.",
    )
    parser.add_argument(
        "--metrics-script",
        type=Path,
        default=repo / "tools" / "stego_metrics.py",
        help="Path to stego_metrics.py.",
    )
    parser.add_argument(
        "--secret",
        type=Path,
        default=repo / "secret.bin",
        help="Path to secret payload file.",
    )
    parser.add_argument(
        "--bitrate",
        type=int,
        default=6000,
        help="Opus encode bitrate in bps.",
    )
    parser.add_argument(
        "--vbr",
        action="store_true",
        help="Use Opus VBR mode. Default keeps CBR by passing -cbr to opus_demo.",
    )
    parser.add_argument(
        "--stego-bps",
        type=int,
        default=250,
        help="Target stego info rate in bits per second (250 bit/s = 31.25 B/s).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Timeout seconds for each subprocess call.",
    )
    parser.add_argument(
        "--no-wav-export",
        action="store_true",
        help="Disable exporting *_dec.wav for listening.",
    )
    parser.add_argument(
        "--target-rate",
        type=int,
        default=48000,
        help="Target PCM stream sample rate for Opus stego validation.",
    )
    parser.add_argument(
        "--target-channels",
        type=int,
        default=1,
        help="Target channel count for validation pipeline.",
    )
    parser.add_argument(
        "--flat-wav-only",
        action="store_true",
        help="Only *.wav in --input-dir itself (no subfolders). Default: recursive rglob.",
    )
    parser.add_argument(
        "--samples-per-dir",
        type=int,
        default=None,
        help="Randomly select this many WAV files from each immediate child directory of --input-dir.",
    )
    parser.add_argument(
        "--sample-seed",
        type=int,
        default=20260504,
        help="Random seed used with --samples-per-dir.",
    )
    parser.add_argument(
        "--results-json",
        type=Path,
        default=None,
        help="Write full results + summary JSON here (default: <output-dir>/validation_results.json).",
    )
    return parser


def _safe_stem_from_wav(wav_path: Path, input_dir: Path) -> str:
    """Unique, filesystem-safe stem including relative subfolders."""
    input_dir = input_dir.resolve()
    wav_path = wav_path.resolve()
    try:
        rel = wav_path.relative_to(input_dir)
    except ValueError:
        rel = Path(wav_path.name)
    base = str(rel.with_suffix("")).replace("\\", "__").replace("/", "__")
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in base)


def main():
    args = build_parser().parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results_path = args.results_json
    if results_path is None:
        results_path = args.output_dir / "validation_results.json"
    progress_jsonl = args.output_dir / "validation_progress.jsonl"

    if args.flat_wav_only:
        wav_files = sorted(args.input_dir.glob("*.wav"))
    elif args.samples_per_dir is not None:
        rng = random.Random(args.sample_seed)
        wav_files = []
        child_dirs = sorted(p for p in args.input_dir.resolve().iterdir() if p.is_dir())
        if not child_dirs:
            child_dirs = [args.input_dir.resolve()]
        for child_dir in child_dirs:
            candidates = sorted(child_dir.rglob("*.wav"))
            if len(candidates) > args.samples_per_dir:
                candidates = sorted(rng.sample(candidates, args.samples_per_dir))
            wav_files.extend(candidates)
    else:
        wav_files = sorted(args.input_dir.resolve().rglob("*.wav"))
    if not wav_files:
        raise SystemExit(f"No WAV files found in: {args.input_dir}")
    if not args.opus_demo.exists():
        raise SystemExit(f"opus_demo not found: {args.opus_demo}")
    if not args.metrics_script.exists():
        raise SystemExit(f"metrics script not found: {args.metrics_script}")
    if not args.secret.exists():
        raise SystemExit(f"secret file not found: {args.secret}")

    results = []
    progress_jsonl.write_text("", encoding="utf-8")
    print(f"Start validating {len(wav_files)} file(s) from: {args.input_dir}\n")
    print(f"Progress log: {progress_jsonl}\n")

    for wav_path in wav_files:
        start = time.time()
        try:
            rel_display = str(wav_path.resolve().relative_to(args.input_dir.resolve()))
        except ValueError:
            rel_display = wav_path.name
        item = {"file": rel_display}
        stem = _safe_stem_from_wav(wav_path, args.input_dir)
        pcm = args.output_dir / f"{stem}.pcm"
        base_bit = args.output_dir / f"{stem}_base.bit"
        base_pcm = args.output_dir / f"{stem}_base_dec.pcm"
        stego_bit = args.output_dir / f"{stem}_stego.bit"
        stego_pcm = args.output_dir / f"{stem}_stego_dec.pcm"
        recovered = args.output_dir / f"{stem}_rec.bin"

        try:
            rate, channels = wav_to_pcm_s16le(
                wav_path,
                pcm,
                target_rate=args.target_rate,
                target_channels=args.target_channels,
            )

            base_encode_cmd = [
                str(args.opus_demo),
                "-e",
                "voip",
                str(rate),
                str(channels),
                str(args.bitrate),
            ]
            if not args.vbr:
                base_encode_cmd.append("-cbr")
            base_encode_cmd.extend(
                [
                    str(pcm),
                    str(base_bit),
                ]
            )

            stego_encode_cmd = [
                str(args.opus_demo),
                "-e",
                "voip",
                str(rate),
                str(channels),
                str(args.bitrate),
            ]
            if not args.vbr:
                stego_encode_cmd.append("-cbr")
            stego_encode_cmd.extend(
                [
                    "-stego_in",
                    str(args.secret),
                    "-stego_bps",
                    str(args.stego_bps),
                    str(pcm),
                    str(stego_bit),
                ]
            )

            # Base encode/decode (no stego)
            run_cmd(base_encode_cmd, timeout_sec=args.timeout)
            run_cmd([
                str(args.opus_demo),
                "-d",
                str(rate),
                str(channels),
                str(base_bit),
                str(base_pcm),
            ], timeout_sec=args.timeout)

            # Stego encode: capture stderr for embedded bits count
            stego_enc_result = run_cmd(stego_encode_cmd, timeout_sec=args.timeout)
            embedded_bits = 0
            for line in stego_enc_result.stderr.splitlines():
                if "Stego embedded bits:" in line:
                    try:
                        # Format: "Stego embedded bits: 12345."
                        embedded_bits = int(line.split(":")[1].strip().rstrip("."))
                    except ValueError:
                        pass

            run_cmd([
                str(args.opus_demo),
                "-d",
                str(rate),
                str(channels),
                "-stego_out",
                str(recovered),
                "-stego_bps",
                str(args.stego_bps),
                str(stego_bit),
                str(stego_pcm),
            ], timeout_sec=args.timeout)

            metrics_cmd = [
                sys.executable,
                str(args.metrics_script),
                "--baseline-pcm",
                str(base_pcm),
                "--stego-pcm",
                str(stego_pcm),
                "--sample-rate",
                str(rate),
                "--channels",
                str(channels),
                "--max-shift",
                "0",
                "--bitstream",
                str(stego_bit),
                "--recovered",
                str(recovered),
            ]
            m = run_cmd(metrics_cmd, timeout_sec=args.timeout)

            psnr = parse_metric_value(m.stdout, "PSNR:")
            opus_kbps = parse_metric_value(m.stdout, "Opus bitrate:")
            stego_Bps = parse_metric_value(m.stdout, "Stego info rate:")
            bit_acc = compute_bit_accuracy(args.secret, recovered, embedded_bits)

            # Compute encoder-side net embedded B/s
            enc_duration_s = len(stego_pcm.read_bytes()) / float(rate * channels * 2) if stego_pcm.exists() else 0
            embedded_Bps = (embedded_bits / 8.0) / enc_duration_s if enc_duration_s > 0 else 0.0

            if not args.no_wav_export:
                pcm_to_wav_s16le(base_pcm, args.output_dir / f"{stem}_base_dec.wav", rate, channels)
                pcm_to_wav_s16le(stego_pcm, args.output_dir / f"{stem}_stego_dec.wav", rate, channels)

            item.update(
                {
                    "status": "success",
                    "target_stego_bps": args.stego_bps,
                    "matrix_channel": "gain_matrix_4subfr",
                    "opus_mode": "vbr" if args.vbr else "cbr",
                    "sample_rate": rate,
                    "channels": channels,
                    "psnr": psnr,
                    "opus_kbps": opus_kbps,
                    "stego_Bps": stego_Bps,
                    "embedded_Bps": round(embedded_Bps, 3),
                    "embedded_bits": embedded_bits,
                    "bit_acc": round(bit_acc, 4),
                    "time_sec": round(time.time() - start, 2),
                }
            )
        except subprocess.TimeoutExpired as e:
            item.update({"status": "timeout", "error": str(e), "time_sec": round(time.time() - start, 2)})
        except Exception as e:
            item.update({"status": "failed", "error": str(e), "time_sec": round(time.time() - start, 2)})

        results.append(item)
        with progress_jsonl.open("a", encoding="utf-8") as jf:
            jf.write(json.dumps(item, ensure_ascii=False) + "\n")
        print(
            f"{rel_display}: status={item['status']} | "
            f"PSNR={item.get('psnr', 'N/A')} | "
            f"Stego={item.get('stego_Bps', 'N/A')} | "
            f"EmbBps={item.get('embedded_Bps', 'N/A')} | "
            f"bit_acc={item.get('bit_acc', 'N/A')}"
        )

    psnr_vals = [parse_float_prefix(r.get("psnr", "N/A")) for r in results if r.get("status") == "success"]
    psnr_vals = [v for v in psnr_vals if v is not None]
    stego_vals = [parse_float_prefix(r.get("stego_Bps", "N/A")) for r in results if r.get("status") == "success"]
    stego_vals = [v for v in stego_vals if v is not None]
    embedded_vals = [r.get("embedded_Bps") for r in results if r.get("status") == "success" and isinstance(r.get("embedded_Bps"), (int, float))]
    bit_acc_vals = [r.get("bit_acc") for r in results if r.get("status") == "success" and isinstance(r.get("bit_acc"), float)]

    summary = {
        "success": sum(1 for r in results if r.get("status") == "success"),
        "failed": sum(1 for r in results if r.get("status") == "failed"),
        "timeout": sum(1 for r in results if r.get("status") == "timeout"),
        "avg_psnr_db": round(sum(psnr_vals) / len(psnr_vals), 3) if psnr_vals else None,
        "avg_stego_Bps": round(sum(stego_vals) / len(stego_vals), 3) if stego_vals else None,
        "avg_embedded_Bps": round(sum(embedded_vals) / len(embedded_vals), 3) if embedded_vals else None,
        "avg_bit_acc": round(sum(bit_acc_vals) / len(bit_acc_vals), 4) if bit_acc_vals else None,
    }

    payload = {
        "summary": summary,
        "results": results,
        "input_dir": str(args.input_dir.resolve()),
        "target_stego_bps": args.stego_bps,
        "matrix_channel": "gain_matrix_4subfr",
        "opus_mode": "vbr" if args.vbr else "cbr",
        "samples_per_dir": args.samples_per_dir,
        "sample_seed": args.sample_seed if args.samples_per_dir is not None else None,
    }
    results_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")

    if len(results) <= 50:
        print("\n=== Detailed Results ===")
        print(json.dumps(results, indent=2, ensure_ascii=False))
    else:
        print(f"\n(Detailed results omitted from console; see {results_path})")
    print("\n=== Summary ===")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"\nOutput directory: {args.output_dir}")
    print(f"Full JSON: {results_path}")
    if not args.no_wav_export:
        print("Listening files exported: *_base_dec.wav and *_stego_dec.wav")


if __name__ == "__main__":
    main()
