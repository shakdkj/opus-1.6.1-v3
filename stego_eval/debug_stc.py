"""Debug: compare Pass 1 vs Pass 2 gain indices."""
import subprocess, sys, wave, numpy as np
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
opus = str(repo / "build_codex" / "opus_demo.exe")
wav = str(repo / "test_data" / "00003" / "00003_01_01_d001_1_30.6_20220607103559.wav")

# Convert WAV to PCM
with wave.open(wav, "rb") as wf:
    ch, sr, sw = wf.getnchannels(), wf.getframerate(), wf.getsampwidth()
    frames = wf.readframes(wf.getnframes())
p = np.frombuffer(frames, dtype="<i2")
if ch == 2:
    p = np.rint((p[:, 0].astype(np.float64) + p[:, 1].astype(np.float64)) * 0.5).astype(np.int16)
if p.ndim != 1:
    p = p.reshape(-1)
(repo / "tmp_debug.pcm").write_bytes(p.tobytes())

def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True)

# Pass 1: no stego, independent coding
run([opus, "-e", "voip", "48000", "1", "6000", "-stego_stc_indep",
     str(repo / "tmp_debug.pcm"), str(repo / "tmp1.bit")])
run([opus, "-d", "48000", "1", "-stego_dump_gains", str(repo / "tmp1_gains.bin"),
     str(repo / "tmp1.bit"), str(repo / "tmp1.pcm")])

# Create target file (all zeros)
Path(repo / "tmp_targets.bin").write_bytes(bytes([0] * 200))

# Pass 2: with stego targets, independent coding
run([opus, "-e", "voip", "48000", "1", "6000",
     "-stego_stc_target", str(repo / "tmp_targets.bin"),
     "-stego_stc_indep",
     str(repo / "tmp_debug.pcm"), str(repo / "tmp2.bit")])
run([opus, "-d", "48000", "1", "-stego_dump_gains", str(repo / "tmp2_gains.bin"),
     str(repo / "tmp2.bit"), str(repo / "tmp2.pcm")])

# Compare gain indices
g1 = (repo / "tmp1_gains.bin").read_bytes()
g2 = (repo / "tmp2_gains.bin").read_bytes()
n = min(len(g1), len(g2)) // 4
diffs = 0
for k in range(n):
    for i in range(4):
        b1 = g1[k * 4 + i]
        b2 = g2[k * 4 + i]
        s1 = (b1 - 256) if b1 > 127 else b1
        s2 = (b2 - 256) if b2 > 127 else b2
        if s1 != s2:
            diffs += 1
            if diffs <= 5:
                print(f"  Frame {k} gain[{i}]: pass1={s1} pass2={s2}")

print(f"\nTotal frames: {n}, gain diffs: {diffs}/{n * 4} ({100 * diffs / (n * 4):.1f}%)")
if diffs == 0:
    print(">>> Gains IDENTICAL — independent coding works!")
else:
    print(">>> Gains DIFFER — independent coding not enough")
