#!/usr/bin/env python3
"""Plan A: Cross-frame STC via Python two-pass with INDEPENDENT coding.
   Uses -stego_in (not -stego_stc_target) to embed per-frame 6-bit targets.
   Gains are IDENTICAL between passes (proven: 0% diff with INDEPENDENT).
"""
import argparse, json, random, subprocess, sys, wave, os
from pathlib import Path
import numpy as np

STC_H = 6; STC_W = 12; STC_POLY = 0x43; INF = 1 << 28
STC_K = 2  # cross-frame over K frames

def _hat_col(j):
    s = 1
    for _ in range(j):
        s <<= 1
        if s & (1 << STC_H): s ^= STC_POLY
    return s & ((1 << STC_H) - 1)

def sym(g): return (g[0] + 3*g[1] + 9*g[2] + 27*g[3]) & 63

def cost_map(gains, gmin=0, gmax=63):
    costs = [INF] * 64
    for d0 in range(-2, 3):
        c0 = gains[0] + d0
        if c0 < gmin or c0 > gmax: continue
        c0a = abs(d0)
        for d1 in range(-2, 3):
            c1 = gains[1] + d1
            if c1 < gmin or c1 > gmax: continue
            c1a = c0a + abs(d1)
            for d2 in range(-2, 3):
                c2 = gains[2] + d2
                if c2 < gmin or c2 > gmax: continue
                c2a = c1a + abs(d2)
                for d3 in range(-2, 3):
                    c3 = gains[3] + d3
                    if c3 < gmin or c3 > gmax: continue
                    cost = c2a + abs(d3)
                    s = sym([c0, c1, c2, c3])
                    if cost < costs[s]: costs[s] = cost
    return costs

def stc_encode_2f(c0, c1, msg_byte):
    costs = [c0, c1]
    orig = [min(range(64), key=lambda s: costs[k][s]) for k in range(2)]
    stego = list(orig)
    n_bits = 12; n_sub = 1; cols = n_sub * STC_W; msg_total = n_sub * STC_H
    states = 1 << STC_H

    fc, ob = [INF] * cols, [0] * cols
    for k in range(2):
        o = orig[k]
        for b in range(6):
            p = k * 6 + b; ob[p] = (o >> b) & 1
            fs = o ^ (1 << b); c = costs[k][fs]
            fc[p] = 8 if c >= INF else min(c, 8)

    hc = [_hat_col(j) for j in range(STC_W)]
    target = (msg_byte & 0x3F)
    hx = 0
    for j in range(STC_W):
        if ob[j]: hx ^= hc[j]
    target ^= hx

    dp = [[INF] * states for _ in range(STC_W + 1)]; dp[0][0] = 0
    prev = [[0] * states for _ in range(STC_W)]
    for col in range(STC_W):
        flip_c = fc[col]; cv = hc[col]
        for s in range(states):
            cur = dp[col][s]
            if cur >= INF: continue
            if cur < dp[col + 1][s]: dp[col + 1][s] = cur; prev[col][s] = (s << 1) | 0
            ns = s ^ cv; nc = cur + flip_c
            if nc < dp[col + 1][ns]: dp[col + 1][ns] = nc; prev[col][ns] = (s << 1) | 1

    if dp[STC_W][target] < INF:
        state = target
        for col in range(STC_W - 1, -1, -1):
            entry = prev[col][state]; ps = entry >> 1; fl = entry & 1
            if fl and col < n_bits:
                fi, fb = col // 6, col % 6; stego[fi] ^= (1 << fb)
            state = ps
    return stego

def stc_extract_2f(syms):
    syn = 0
    for j in range(STC_W):
        fi, fb = j // 6, j % 6
        if (syms[fi] >> fb) & 1: syn ^= _hat_col(j)
    return syn & 0x3F

def targets_to_secret(targets, frames):
    """Pack per-frame 6-bit targets into bitstream for -stego_in.
       At 300 bps: 6 bits/frame. Each frame's target (6-bit) maps to
       6 consumed bits from secret."""
    bits_needed = frames * 6
    bytes_needed = (bits_needed + 7) // 8
    secret = bytearray(bytes_needed)
    bit_pos = 0
    for k in range(frames):
        val = targets[k] & 0x3F
        for b in range(6):
            byte_idx = bit_pos >> 3
            bit_off = bit_pos & 7
            if (val >> b) & 1:
                secret[byte_idx] |= (1 << bit_off)
            bit_pos += 1
    return bytes(secret)

def run_cmd(cmd, to=120):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=to, check=True)

def wav_pcm(wf, pf):
    with wave.open(str(wf), "rb") as w:
        ch, sr, sw = w.getnchannels(), w.getframerate(), w.getsampwidth()
        fr = w.readframes(w.getnframes())
    p = np.frombuffer(fr, dtype="<i2")
    if ch == 2: p = np.rint((p[:,0].astype(np.float64) + p[:,1].astype(np.float64)) * 0.5).astype(np.int16)
    if p.ndim != 1: p = p.reshape(-1)
    if sr != 48000:
        s = p.astype(np.float64); nl = max(1, int(round(len(s) * 48000 / sr)))
        sx = np.arange(len(s)); dx = np.linspace(0, len(s)-1, num=nl)
        p = np.rint(np.interp(dx, sx, s)).clip(-32768, 32767).astype(np.int16)
    Path(pf).write_bytes(p.tobytes())
    return 48000

def bit_acc(s, r):
    a = Path(s).read_bytes(); b = Path(r).read_bytes()
    n = min(len(a), len(b))
    return sum(8 - bin(a[i] ^ b[i]).count("1") for i in range(n)) / (n * 8) if n else 0

def parse_psnr(out):
    for l in out.splitlines():
        if "PSNR:" in l: return l.split(":", 1)[1].strip()
    return "N/A"

def stem(w, idir):
    try: rel = str(Path(w).resolve().relative_to(Path(idir).resolve()))
    except: rel = Path(w).name
    base = str(Path(rel).with_suffix("")).replace("\\", "__").replace("/", "__")
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in base)

def main():
    ap = argparse.ArgumentParser()
    repo = Path(__file__).resolve().parent.parent
    ap.add_argument("--input-dir", type=Path, default=repo / "test_data")
    ap.add_argument("--output-dir", type=Path, default=repo / "stego_eval" / "output_planA")
    ap.add_argument("--opus-demo", type=Path, default=repo / "build_v4_mingw2" / "opus_demo.exe")
    ap.add_argument("--secret", type=Path, default=repo / "secret.bin")
    ap.add_argument("--bitrate", type=int, default=6000)
    ap.add_argument("--spd", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260504)
    ap.add_argument("--timeout", type=int, default=180)
    a = ap.parse_args()
    a.output_dir.mkdir(parents=True, exist_ok=True)

    rng = random.Random(a.seed)
    wavs = []
    cds = sorted(d for d in a.input_dir.resolve().iterdir() if d.is_dir()) or [a.input_dir.resolve()]
    for cd in cds:
        cand = sorted(cd.rglob("*.wav"))
        if len(cand) > a.spd: cand = sorted(rng.sample(cand, a.spd))
        wavs.extend(cand)

    opus = str(a.opus_demo); results = []
    print(f"Plan A: STC K={STC_K}, alpha={STC_H}/{STC_W}={STC_H/STC_W:.0%}, INDEPENDENT coding")
    print(f"Embedding targets via -stego_in secret file\n")

    for wav_path in wavs:
        try: rd = str(wav_path.resolve().relative_to(a.input_dir.resolve()))
        except: rd = wav_path.name
        it = {"file": rd}; st = stem(wav_path, a.input_dir)
        pcm = a.output_dir / f"{st}.pcm"
        bb = a.output_dir / f"{st}_base.bit"; bp = a.output_dir / f"{st}_base_dec.pcm"
        sb = a.output_dir / f"{st}_stego.bit"; sp = a.output_dir / f"{st}_stego_dec.pcm"
        gd = a.output_dir / f"{st}_gains.bin"
        sbf = a.output_dir / f"{st}_target_secret.bin"
        rec = a.output_dir / f"{st}_rec.bin"

        try:
            rate = wav_pcm(wav_path, pcm)

            # Pass 1: encode WITHOUT stego (INDEPENDENT), decode and extract
            # original symbols via stego extraction (decoder always extracts 6-bit symbol)
            run_cmd([opus, "-e", "voip", "48000", "1", str(a.bitrate),
                     "-stego_stc_indep",
                     str(pcm), str(bb)], a.timeout)
            run_cmd([opus, "-d", "48000", "1",
                     str(bb), str(bp)], a.timeout)
            # Extract original symbols via stego_out (decoder reads 6 bits/frame)
            ref_sym_bit = a.output_dir / f"{st}_ref_syms.bin"
            run_cmd([opus, "-d", "48000", "1",
                     "-stego_out", str(ref_sym_bit),
                     "-stego_bps", "300",
                     str(bb), str(bp)], a.timeout)

            ref_bytes = ref_sym_bit.read_bytes()
            nf = len(ref_bytes) * 8 // 6
            if nf < 2: continue

            frame_syms = []
            bit_pos = 0
            for k in range(nf):
                sym_val = 0
                for b in range(6):
                    byte_idx = bit_pos >> 3; bit_off = bit_pos & 7
                    if byte_idx < len(ref_bytes) and (ref_bytes[byte_idx] >> bit_off) & 1:
                        sym_val |= (1 << b)
                    bit_pos += 1
                frame_syms.append(sym_val & 0x3F)

            # We need cost maps. For each frame's original symbol s, the cost to
            # reach any other symbol t is unknown without gain indices.
            # Approximation: cost = Hamming distance between s and t (0-6).
            # This treats each bit flip as cost 1.
            # Better: use the actual bit-level cost derived from encoded symbols.
            frame_costs = []
            for s in frame_syms:
                costs = []
                for t in range(64):
                    dist = bin(s ^ t).count('1')
                    costs.append(dist)
                frame_costs.append(costs)

            # Pack 6-bit messages from secret (not 8-bit bytes)
            secret_data = a.secret.read_bytes()
            msg_6bit = []
            bit_pos = 0
            while bit_pos + 5 < len(secret_data) * 8:
                val = 0
                for b in range(6):
                    byte_idx = bit_pos >> 3; bit_off = bit_pos & 7
                    if (secret_data[byte_idx] >> bit_off) & 1:
                        val |= (1 << b)
                    bit_pos += 1
                msg_6bit.append(val)
            if not msg_6bit: msg_6bit = [0]

            targets = []; msg_idx = 0
            for k in range(0, nf - 1, STC_K):
                c0 = frame_costs[k]; c1 = frame_costs[k + 1]
                mb = msg_6bit[msg_idx] if msg_idx < len(msg_6bit) else 0
                msg_idx += 1
                ts = stc_encode_2f(c0, c1, mb)
                targets.extend(ts)
            if len(targets) < nf:
                best = min(range(64), key=lambda s: frame_costs[-1][s])
                targets.append(best)
            targets = targets[:nf]

            # Pack targets into secret file
            target_secret = targets_to_secret(targets, nf)
            sbf.write_bytes(target_secret)

            msg_pos = msg_idx  # for STC decode

            # Pass 2: encode with stego_in using target secret (INDEPENDENT coding)
            run_cmd([opus, "-e", "voip", "48000", "1", str(a.bitrate),
                     "-stego_stc_indep",
                     "-stego_in", str(sbf),
                     "-stego_bps", "300",
                     str(pcm), str(sb)], a.timeout)

            # Decode and extract stego bits
            run_cmd([opus, "-d", "48000", "1",
                     "-stego_out", str(rec),
                     "-stego_bps", "300",
                     str(sb), str(sp)], a.timeout)

            # STC decode: convert recovered bits back to symbols, then extract message
            rec_bytes = rec.read_bytes()
            rec_total_bits = len(rec_bytes) * 8
            recovered_syms = []
            bit_pos = 0
            for k in range(min(nf, rec_total_bits // 6)):
                val = 0
                for b in range(6):
                    byte_idx = bit_pos >> 3; bit_off = bit_pos & 7
                    if byte_idx < len(rec_bytes) and (rec_bytes[byte_idx] >> bit_off) & 1:
                        val |= (1 << b)
                    bit_pos += 1
                recovered_syms.append(val & 0x3F)

            # STC decode: get 6-bit message values, pack into bytes
            recovered_6bit = []
            for k in range(0, min(nf, len(recovered_syms)) - 1, 2):
                recovered_6bit.append(stc_extract_2f([recovered_syms[k], recovered_syms[k+1]]))
            # Pack 6-bit values into bytes
            recovered_bytes = bytearray()
            bit_buffer = 0; bit_count = 0
            for val in recovered_6bit:
                for b in range(6):
                    if (val >> b) & 1: bit_buffer |= (1 << bit_count)
                    bit_count += 1
                    if bit_count == 8:
                        recovered_bytes.append(bit_buffer)
                        bit_buffer = 0; bit_count = 0
            if bit_count > 0: recovered_bytes.append(bit_buffer)
            recovered_path = a.output_dir / f"{st}_recovered.bin"
            recovered_path.write_bytes(bytes(recovered_bytes))

            bit_acc_val = bit_acc(a.secret, recovered_path)

            psnr_v = "N/A"
            ms = repo / "tools" / "stego_metrics.py"
            if ms.exists():
                m = run_cmd([sys.executable, str(ms),
                             "--baseline-pcm", str(bp), "--stego-pcm", str(sp),
                             "--sample-rate", "48000", "--channels", "1",
                             "--max-shift", "0"], a.timeout)
                psnr_v = parse_psnr(m.stdout)

            it.update({"status": "success", "psnr": psnr_v,
                       "bit_acc": round(bit_acc_val, 4), "frames": nf})
        except subprocess.TimeoutExpired as e:
            it.update({"status": "timeout", "error": str(e)})
        except Exception as e:
            it.update({"status": "failed", "error": str(e)})
        results.append(it)
        print(f"{rd}: status={it['status']} | PSNR={it.get('psnr','N/A')} | "
              f"bit_acc={it.get('bit_acc','N/A')}")

    ok = [r for r in results if r.get("status") == "success"]
    pv = [float(r["psnr"].split()[0]) for r in ok if r.get("psnr","N/A") != "N/A"]
    av_val = [r["bit_acc"] for r in ok if "bit_acc" in r]
    sm = {"success": len(ok),
          "failed": sum(1 for r in results if r.get("status") == "failed"),
          "timeout": sum(1 for r in results if r.get("status") == "timeout"),
          "avg_psnr_db": round(sum(pv) / len(pv), 3) if pv else None,
          "avg_bit_acc": round(sum(av_val) / len(av_val), 4) if av_val else None,
          "stc_alpha": f"{STC_H}/{STC_W}={STC_H/STC_W:.0%}"}
    (a.output_dir / "validation_results.json").write_text(
        json.dumps({"summary": sm, "results": results}, indent=2, ensure_ascii=False))
    print("\n=== Plan A Summary ===")
    print(json.dumps(sm, indent=2, ensure_ascii=False))

if __name__ == "__main__":
    main()
