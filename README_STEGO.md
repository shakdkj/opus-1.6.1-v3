# Opus SILK Gain-Matrix Steganography

本项目在 Opus 1.6.1 的 SILK 路径中实现基于增益索引的矩阵隐写通道。载体为每帧 4 个 subframe 的 `GainsIndices[0..3]`，不使用 PCM LSB，不新增公开 API。

## 当前方案

4 个 subframe gain index 联合形成矩阵符号：

```text
symbol = (g0 + 3*g1 + 9*g2 + 27*g3) & ((1 << nbits) - 1)
```

编码端在合法 gain index 范围内搜索 `[-2, 2]` 扰动，使矩阵符号匹配待嵌入秘密值，并优先选择绝对扰动代价最小的候选。解码端从解码出的 `GainsIndices[0..3]` 计算同一符号，不依赖额外边信息。

## 交付口径

| 项目 | 配置 |
|------|------|
| PCM 输入 | 48 kHz, mono, s16le |
| Opus 输出 | 6 kbps CBR |
| 隐写通道 | SILK gain-index matrix embedding |
| 最大容量 | 6 bit/frame，约 300 bit/s |
| 默认测试 | 250 bit/s，约 5 bit/frame，31.25 B/s |

## 构建

```powershell
cmake --build build_codex --target opus_demo -j 4
```

## new_data 验证

```powershell
python stego_eval\validate_newdata.py `
  --input-dir ..\opus-1.6.1\new_data `
  --opus-demo build_codex\opus_demo.exe `
  --secret secret.bin `
  --bitrate 6000 `
  --stego-bps 250 `
  --no-wav-export `
  --output-dir stego_eval\output_48k_6kbps_gain_matrix_250
```

## opus_demo 示例

```powershell
build_codex\opus_demo.exe -e voip 48000 1 6000 -cbr input.pcm baseline.bit
build_codex\opus_demo.exe -d 48000 1 baseline.bit baseline_dec.pcm
build_codex\opus_demo.exe -e voip 48000 1 6000 -cbr -stego_in secret.bin -stego_bps 250 input.pcm stego.bit
build_codex\opus_demo.exe -d 48000 1 -stego_out recovered.bin -stego_bps 250 stego.bit stego_dec.pcm
python tools\stego_metrics.py --baseline-pcm baseline_dec.pcm --stego-pcm stego_dec.pcm --sample-rate 48000 --channels 1 --max-shift 0 --bitstream stego.bit --recovered recovered.bin
```

## 相关文件

| 文件 | 作用 |
|------|------|
| `silk/stego_pulse_matrix.c` | Gain-Matrix 嵌入/提取实现 |
| `silk/stego_pulse_matrix.h` | 隐写通道声明与最大 bit 数 |
| `silk/float/encode_frame_FLP.c` | float 编码路径调用嵌入 |
| `silk/fixed/encode_frame_FIX.c` | fixed 编码路径调用嵌入 |
| `silk/decode_frame.c` | 解码路径提取 |
| `src/opus_demo.c` | CLI 隐写输入、输出和速率预算 |
| `stego_eval/validate_newdata.py` | 48 kHz、6 kbps、new_data 验证脚本 |
| `tools/stego_metrics.py` | PSNR、Opus bitrate、Stego B/s 指标 |
