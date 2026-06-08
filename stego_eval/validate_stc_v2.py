#!/usr/bin/env python3
"""STC cross-frame: Python two-pass with CODE_INDEPENDENTLY for determinism.

   Proved: with -stego_stc_indep, identical targets → identical gains (0% diff).
   STC minimizes total L1 cost across a block of K frames.
   alpha = h/w = 6/12 = 50%, 3 message bits per frame net.
"""
import argparse, json, random, subprocess, sys, wave, os
from pathlib import Path
import numpy as np

STC_H = 6; STC_W = 12; STC_POLY = 0x43; INF = 1 << 28
STC_BUF_K = 2  # cross-frame STC over 2 frames

def sym(g): return (g[0] + 3*g[1] + 9*g[2] + 27*g[3]) & 63

def _hat_col(j):
    s = 1
    for _ in range(j):
        s <<= 1
        if s & (1 << STC_H): s ^= STC_POLY
    return s & ((1 << STC_H) - 1)

def cost_map(gains, gmin=0, gmax=63):  # INDEPENDENT: 0-63; CONDITIONAL: 0-40
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
    """STC encode 2 frames, 6 message bits into 12 cover bits."""
    costs = [c0, c1]
    orig = [min(range(64), key=lambda s: costs[k][s]) for k in range(2)]
    stego = list(orig)
    n_bits = 12; n_sub = 1; cols = n_sub * STC_W; msg_total = n_sub * STC_H
    states = 1 << STC_H

    # Per-bit flip costs and original bits
    fc, ob = [INF] * cols, [0] * cols
    for k in range(2):
        o = orig[k]
        for b in range(6):
            p = k * 6 + b; ob[p] = (o >> b) & 1
            fs = o ^ (1 << b)
            c = costs[k][fs]
            fc[p] = 8 if c >= INF else min(c, 8)

    hc = [_hat_col(j) for j in range(STC_W)]
    target = (msg_byte & 0x3F)  # 6 message bits
    hx = 0
    for j in range(STC_W):
        if ob[j]: hx ^= hc[j]
    target ^= hx

    # Forward Viterbi
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
    ap.add_argument("--output-dir", type=Path, default=repo / "stego_eval" / "output_stc_v2")
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
    print(f"STC v2: {len(wavs)} files, K={STC_BUF_K}, alpha={STC_H}/{STC_W}={STC_H/STC_W:.0%}\n")

    for wav_path in wavs:
        try: rd = str(wav_path.resolve().relative_to(a.input_dir.resolve()))
        except: rd = wav_path.name
        it = {"file": rd}; st = stem(wav_path, a.input_dir)
        pcm = a.output_dir / f"{st}.pcm"
        bb = a.output_dir / f"{st}_base.bit"; bp = a.output_dir / f"{st}_base_dec.pcm"
        sb = a.output_dir / f"{st}_stego.bit"; sp = a.output_dir / f"{st}_stego_dec.pcm"
        gd = a.output_dir / f"{st}_gains.bin"; tf = a.output_dir / f"{st}_targets.bin"
        sg = a.output_dir / f"{st}_sgains.bin"; rc = a.output_dir / f"{st}_rec.bin"

        try:
            rate = wav_pcm(wav_path, pcm)

            # Pass 1: encode without stego (INDEPENDENT for determinism)
            run_cmd([opus, "-e", "voip", "48000", "1", str(a.bitrate),
                     "-stego_stc_indep", str(pcm), str(bb)], a.timeout)
            run_cmd([opus, "-d", "48000", "1",
                     "-stego_dump_gains", str(gd),
                     str(bb), str(bp)], a.timeout)

            # Read gains, compute cost maps, run STC
            gd_bytes = gd.read_bytes(); nf = len(gd_bytes) // 4
            targets = []; total_cost = 0
            secret_data = a.secret.read_bytes()
            msg_byte_pos = 0

            for k in range(0, nf, STC_BUF_K):
                if k + 1 >= nf: break
                # Gains for 2 consecutive frames
                g0 = [(b - 256) if b > 127 else b for b in gd_bytes[k*4:(k+1)*4]]
                g1 = [(b - 256) if b > 127 else b for b in gd_bytes[(k+1)*4:(k+2)*4]]
                c0 = cost_map(g0); c1 = cost_map(g1)
                mb = secret_data[msg_byte_pos] if msg_byte_pos < len(secret_data) else 0
                msg_byte_pos += 1
                stego_syms = stc_encode_2f(c0, c1, mb)
                targets.extend(stego_syms)
                total_cost += c0[stego_syms[0]] + c1[stego_syms[1]]

            if nf % 2 == 1:
                g0 = [(b - 256) if b > 127 else b for b in gd_bytes[(nf-1)*4:nf*4]]
                c0 = cost_map(g0)
                best = min(range(64), key=lambda s: c0[s])
                targets.append(best)
            targets = targets[:nf]
            tf.write_bytes(bytes(t & 0x3F for t in targets))

            # Pass 2: encode with STC targets (INDEPENDENT)
            run_cmd([opus, "-e", "voip", "48000", "1", str(a.bitrate),
                     "-stego_stc_indep",
                     "-stego_stc_target", str(tf),
                     str(pcm), str(sb)], a.timeout)

            # Decode with gain dump
            run_cmd([opus, "-d", "48000", "1",
                     "-stego_dump_gains", str(sg),
                     str(sb), str(sp)], a.timeout)

            # STC-extract message
            sg_bytes = sg.read_bytes()
            ns = min(len(sg_bytes) // 4, nf)
            recovered = bytearray()
            for k in range(0, ns, 2):
                if k + 1 >= ns: break
                g0 = [(b - 256) if b > 127 else b for b in sg_bytes[k*4:(k+1)*4]]
                g1 = [(b - 256) if b > 127 else b for b in sg_bytes[(k+1)*4:(k+2)*4]]
                s0, s1 = sym(g0), sym(g1)
                recovered.append(stc_extract_2f([s0, s1]))
            rc.write_bytes(bytes(recovered[:msg_byte_pos]))

            bit_acc_val = bit_acc(a.secret, rc)
            avg_cost = total_cost / max(1, nf)

            psnr_v = "N/A"
            ms = repo / "tools" / "stego_metrics.py"
            if ms.exists():
                m = run_cmd([sys.executable, str(ms),
                             "--baseline-pcm", str(bp), "--stego-pcm", str(sp),
                             "--sample-rate", "48000", "--channels", "1",
                             "--max-shift", "0"], a.timeout)
                psnr_v = parse_psnr(m.stdout)

            it.update({"status": "success", "psnr": psnr_v,
                       "bit_acc": round(bit_acc_val, 4), "frames": nf,
                       "avg_cost": round(avg_cost, 2)})
        except subprocess.TimeoutExpired as e:
            it.update({"status": "timeout", "error": str(e)})
        except Exception as e:
            it.update({"status": "failed", "error": str(e)})
        results.append(it)
        print(f"{rd}: status={it['status']} | PSNR={it.get('psnr','N/A')} | "
              f"bit_acc={it.get('bit_acc','N/A')} | cost={it.get('avg_cost','N/A')}")

    ok = [r for r in results if r.get("status") == "success"]
    pv = []
    for r in ok:
        v = r.get("psnr", "N/A")
        if v != "N/A":
            try: pv.append(float(v.split()[0]))
            except: pass
    av_val = [r["bit_acc"] for r in ok if "bit_acc" in r]
    cv = [r["avg_cost"] for r in ok if "avg_cost" in r]
    sm = {"success": len(ok),
          "failed": sum(1 for r in results if r.get("status") == "failed"),
          "timeout": sum(1 for r in results if r.get("status") == "timeout"),
          "avg_psnr_db": round(sum(pv) / len(pv), 3) if pv else None,
          "avg_bit_acc": round(sum(av_val) / len(av_val), 4) if av_val else None,
          "avg_cost": round(sum(cv) / len(cv), 2) if cv else None,
          "stc_alpha": f"{STC_H}/{STC_W}={STC_H/STC_W:.0%}"}
    (a.output_dir / "validation_results.json").write_text(
        json.dumps({"summary": sm, "results": results}, indent=2, ensure_ascii=False))
    print("\n=== STC v2 Summary ===")
    print(json.dumps(sm, indent=2, ensure_ascii=False))

if __name__ == "__main__":
    main()
