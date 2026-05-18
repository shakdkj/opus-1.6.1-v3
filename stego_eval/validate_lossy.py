#!/usr/bin/env python3
"""Stego validation with XOR-3 FEC + packet loss simulation.

Per-frame format: [seq:2 bits][payload:4 bits]
FEC: XOR-3 — 2 data frames + 1 parity frame per block (33% overhead).
     Any 2 of 3 frames recover the 2 data frames.
Loss: drop 15% of Opus packets uniformly at random.
Net throughput: 37.5 * 2/3 * 4/6 = 16.7 B/s.
"""
import argparse
import json
import random
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

DATA_FRAMES = 2
PARITY_FRAMES = 1
BLOCK_FRAMES = DATA_FRAMES + PARITY_FRAMES  # 3
LOSS_RATE = 0.15
STEGO_BPS = 300
PAYLOAD_BITS = 4
BITRATE = 6000
TIMEOUT = 180


def default_repo_root():
    return Path(__file__).resolve().parent.parent


def run_cmd(cmd, timeout_sec=120):
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout_sec, check=True)


def wav_to_pcm_s16le(wav_path, pcm_path, target_rate=48000, target_channels=1):
    with wave.open(str(wav_path), "rb") as wf:
        ch, sr, sw = wf.getnchannels(), wf.getframerate(), wf.getsampwidth()
        frames = wf.readframes(wf.getnframes())
    if sw != 2:
        raise ValueError(f"{wav_path.name}: 16-bit only")
    pcm = np.frombuffer(frames, dtype="<i2")
    if ch > 1:
        pcm = pcm.reshape(-1, ch)
    if ch == 2 and target_channels == 1:
        pcm = np.rint((pcm[:, 0].astype(np.float64) +
                       pcm[:, 1].astype(np.float64)) * 0.5).astype(np.int16)
    elif ch != target_channels:
        raise ValueError(f"channel mismatch {ch}->{target_channels}")
    if pcm.ndim != 1 and pcm.shape[1] == 1:
        pcm = pcm.reshape(-1)
    if sr != target_rate:
        src = pcm.astype(np.float64)
        new_len = max(1, int(round(len(src) * target_rate / sr)))
        src_x = np.arange(len(src), dtype=np.float64)
        dst_x = np.linspace(0.0, float(max(0, len(src) - 1)),
                            num=new_len, dtype=np.float64)
        pcm = np.rint(np.interp(dst_x, src_x, src)).clip(-32768, 32767).astype(np.int16)
    pcm_path.write_bytes(pcm.tobytes())
    return target_rate, 1


def xor3_encode(input_path, output_path):
    """XOR-3 FEC: for each 2 nibbles, append 1 parity nibble (XOR of the 2).

    Returns (original_bytes, encoded_nibble_count).
    """
    data = input_path.read_bytes()
    nibbles = []
    for b in data:
        nibbles.append(b & 0x0F)
        nibbles.append((b >> 4) & 0x0F)
    encoded = []
    for i in range(0, len(nibbles), DATA_FRAMES):
        chunk = nibbles[i:i + DATA_FRAMES]
        while len(chunk) < DATA_FRAMES:
            chunk.append(0)
        d0, d1 = chunk[0], chunk[1]
        p = (d0 ^ d1) & 0x0F
        encoded.extend([d0, d1, p])
    output = bytearray()
    for nib in encoded:
        output.append(nib)
    output_path.write_bytes(bytes(output))
    return len(data), len(encoded)


def xor3_decode(input_path, original_bytes, gap_positions, output_path):
    """XOR-3 decode. gap_positions: set of nibble indices that were lost.

    The input file contains nibbles packed as bytes (one nibble per byte).
    For each 3-nibble block, if 1 is lost, recover via XOR of the other 2.
    If 0 or 2+ are lost, use as-is or mark as unrecoverable.
    """
    raw = input_path.read_bytes()
    nibbles = list(raw)  # each byte is one 4-bit nibble value (0-15)
    decoded_nibbles = []
    blocks_ok = 0
    blocks_failed = 0
    for i in range(0, len(nibbles), BLOCK_FRAMES):
        chunk = nibbles[i:i + BLOCK_FRAMES]
        if len(chunk) < BLOCK_FRAMES:
            break
        missing = [j for j in range(BLOCK_FRAMES) if (i + j) in gap_positions]
        if len(missing) == 0:
            decoded_nibbles.extend(chunk[:DATA_FRAMES])
            blocks_ok += 1
        elif len(missing) == 1:
            # Recover the missing nibble via XOR
            m = missing[0]
            if m == 0:
                chunk[0] = chunk[1] ^ chunk[2]
            elif m == 1:
                chunk[1] = chunk[0] ^ chunk[2]
            else:  # m == 2, parity lost
                pass  # parity not needed for data
            decoded_nibbles.extend(chunk[:DATA_FRAMES])
            blocks_ok += 1
        else:
            decoded_nibbles.extend([0, 0])
            blocks_failed += 1
    # Pack nibbles back into bytes
    output = bytearray()
    target_nibbles = original_bytes * 2
    for j in range(0, min(len(decoded_nibbles), target_nibbles), 2):
        lo = decoded_nibbles[j] & 0x0F
        hi = decoded_nibbles[j + 1] & 0x0F if j + 1 < len(decoded_nibbles) else 0
        output.append(lo | (hi << 4))
    output_path.write_bytes(bytes(output))
    return blocks_ok, blocks_failed


def simulate_packet_loss(bitstream, damaged, loss_rate=0.15):
    """Drop random Opus packets from bitstream file."""
    data = bitstream.read_bytes()
    out = bytearray()
    off = 0
    total, dropped = 0, 0
    rng = random.Random(42)
    while off + 8 <= len(data):
        pkt_len = int.from_bytes(data[off:off + 4], "big", signed=False)
        pkt_end = off + 8 + pkt_len
        if pkt_len < 0 or pkt_end > len(data):
            break
        total += 1
        if rng.random() >= loss_rate:
            out.extend(data[off:pkt_end])
        else:
            dropped += 1
        off = pkt_end
    damaged.write_bytes(bytes(out))
    return total, dropped


def parse_gap_nibbles(stderr_text):
    """Parse 'SEQ GAP: N frames at nibble POS' messages."""
    gaps = set()
    for line in stderr_text.splitlines():
        if "SEQ GAP:" in line:
            # Format: "SEQ GAP: 1 frames at nibble 42"
            parts = line.split()
            try:
                count = int(parts[2])
                pos = int(parts[5])
            except (ValueError, IndexError):
                continue
            for k in range(count):
                gaps.add(pos + k)
    return gaps


def compute_bit_accuracy(secret_path, recovered_path):
    a = secret_path.read_bytes()
    b = recovered_path.read_bytes()
    n = min(len(a), len(b))
    if n <= 0:
        return 0.0
    ok = sum(8 - bin(a[i] ^ b[i]).count("1") for i in range(n))
    return ok / (n * 8)


def _safe_stem(wav_path, input_dir):
    input_dir = input_dir.resolve()
    wav_path = wav_path.resolve()
    try:
        rel = wav_path.relative_to(input_dir)
    except ValueError:
        rel = Path(wav_path.name)
    base = str(rel.with_suffix("")).replace("\\", "__").replace("/", "__")
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in base)


def parse_psnr(stdout):
    for line in stdout.splitlines():
        if "PSNR:" in line:
            return line.split(":", 1)[1].strip()
    return "N/A"


def build_parser():
    p = argparse.ArgumentParser()
    repo = default_repo_root()
    p.add_argument("--input-dir", type=Path, default=repo / "test_data")
    p.add_argument("--output-dir", type=Path, default=repo / "stego_eval" / "output_lossy")
    p.add_argument("--opus-demo", type=Path, default=repo / "build_codex" / "opus_demo.exe")
    p.add_argument("--secret", type=Path, default=repo / "secret.bin")
    p.add_argument("--bitrate", type=int, default=BITRATE)
    p.add_argument("--stego-bps", type=int, default=STEGO_BPS)
    p.add_argument("--loss-rate", type=float, default=LOSS_RATE)
    p.add_argument("--timeout", type=int, default=TIMEOUT)
    p.add_argument("--samples-per-dir", type=int, default=10)
    p.add_argument("--sample-seed", type=int, default=20260504)
    return p


def main():
    args = build_parser().parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

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
    if not wav_files:
        raise SystemExit(f"No WAV files in {args.input_dir}")

    opus_demo = str(args.opus_demo)
    results = []
    print(f"Start validating {len(wav_files)} files (loss={args.loss_rate*100:.0f}%,")
    print(f"  XOR-3 FEC, {STEGO_BPS}bps, {BITRATE}bps VBR)\n")

    for wav_path in wav_files:
        try:
            rel_display = str(wav_path.resolve().relative_to(args.input_dir.resolve()))
        except ValueError:
            rel_display = wav_path.name
        item = {"file": rel_display}
        stem = _safe_stem(wav_path, args.input_dir)
        pcm = args.output_dir / f"{stem}.pcm"
        base_bit = args.output_dir / f"{stem}_base.bit"
        base_pcm = args.output_dir / f"{stem}_base_dec.pcm"
        stego_bit = args.output_dir / f"{stem}_stego.bit"
        stego_dmg = args.output_dir / f"{stem}_stego_damaged.bit"
        stego_pcm = args.output_dir / f"{stem}_stego_dec.pcm"
        secret_fec = args.output_dir / f"{stem}_secret_fec.bin"
        recovered = args.output_dir / f"{stem}_rec.bin"
        recovered_final = args.output_dir / f"{stem}_rec_final.bin"

        try:
            rate, _ = wav_to_pcm_s16le(wav_path, pcm)

            # XOR-3 encode the secret
            orig_len, nib_count = xor3_encode(args.secret, secret_fec)

            # Base encode/decode
            run_cmd([opus_demo, "-e", "voip", str(rate), "1", str(args.bitrate),
                     str(pcm), str(base_bit)], timeout_sec=args.timeout)
            run_cmd([opus_demo, "-d", str(rate), "1",
                     str(base_bit), str(base_pcm)], timeout_sec=args.timeout)

            # Stego encode with seq mode
            stego_enc = run_cmd([opus_demo, "-e", "voip", str(rate), "1",
                                 str(args.bitrate),
                                 "-stego_in", str(secret_fec),
                                 "-stego_bps", str(args.stego_bps),
                                 "-stego_seq",
                                 str(pcm), str(stego_bit)],
                                timeout_sec=args.timeout)
            emb = 0
            for line in stego_enc.stderr.splitlines():
                if "Stego embedded bits:" in line:
                    try:
                        emb = int(line.split(":")[1].strip().rstrip("."))
                    except ValueError:
                        pass

            # Simulate packet loss
            total_pkts, dropped = simulate_packet_loss(stego_bit, stego_dmg, args.loss_rate)

            # Decode damaged bitstream with seq mode
            stego_dec = run_cmd([opus_demo, "-d", str(rate), "1",
                                 "-stego_out", str(recovered),
                                 "-stego_bps", str(args.stego_bps),
                                 "-stego_seq",
                                 str(stego_dmg), str(stego_pcm)],
                                timeout_sec=args.timeout)

            # Parse gap positions from stderr
            gap_nibbles = parse_gap_nibbles(stego_dec.stderr)

            # XOR-3 decode
            blocks_ok, blocks_failed = xor3_decode(
                recovered, orig_len, gap_nibbles, recovered_final)

            # PSNR
            psnr_str = "N/A"
            ms = default_repo_root() / "tools" / "stego_metrics.py"
            if ms.exists():
                m = run_cmd([sys.executable, str(ms),
                             "--baseline-pcm", str(base_pcm),
                             "--stego-pcm", str(stego_pcm),
                             "--sample-rate", str(rate), "--channels", "1",
                             "--max-shift", "0",
                             "--bitstream", str(stego_bit),
                             "--recovered", str(recovered_final)],
                            timeout_sec=args.timeout)
                psnr_str = parse_psnr(m.stdout)

            bit_acc = compute_bit_accuracy(args.secret, recovered_final)
            actual_loss = dropped / total_pkts if total_pkts else 0

            item.update({
                "status": "success", "psnr": psnr_str,
                "embedded_bits": emb, "bit_acc": round(bit_acc, 4),
                "total_pkts": total_pkts, "dropped": dropped,
                "actual_loss": round(actual_loss, 4),
                "blocks_ok": blocks_ok, "blocks_failed": blocks_failed,
            })
        except subprocess.TimeoutExpired as e:
            item.update({"status": "timeout", "error": str(e)})
        except Exception as e:
            item.update({"status": "failed", "error": str(e)})

        results.append(item)
        print(f"{rel_display}: status={item['status']} | "
              f"PSNR={item.get('psnr','N/A')} | "
              f"bit_acc={item.get('bit_acc','N/A')} | "
              f"loss={item.get('actual_loss','N/A')}")

    success = [r for r in results if r.get("status") == "success"]
    psnr_vals = []
    for r in success:
        v = r.get("psnr", "N/A")
        if v and v != "N/A":
            try:
                psnr_vals.append(float(v.split()[0]))
            except (ValueError, IndexError):
                pass
    acc_vals = [r["bit_acc"] for r in success if "bit_acc" in r]
    loss_vals = [r.get("actual_loss", 0) for r in success]

    summary = {
        "success": len(success),
        "failed": sum(1 for r in results if r.get("status") == "failed"),
        "timeout": sum(1 for r in results if r.get("status") == "timeout"),
        "avg_psnr_db": round(sum(psnr_vals)/len(psnr_vals), 3) if psnr_vals else None,
        "avg_bit_acc": round(sum(acc_vals)/len(acc_vals), 4) if acc_vals else None,
        "avg_loss": round(sum(loss_vals)/len(loss_vals), 4) if loss_vals else None,
    }
    payload = {"summary": summary, "results": results,
               "config": {"loss_rate": args.loss_rate,
                          "stego_bps": args.stego_bps,
                          "fec": "XOR-3 (2 data + 1 parity)"}}
    (args.output_dir / "validation_results.json").write_text(
        json.dumps(payload, indent=2, ensure_ascii=False))

    print("\n=== Summary ===")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"\nOutput: {args.output_dir}")


if __name__ == "__main__":
    main()
