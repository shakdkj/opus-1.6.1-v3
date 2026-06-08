# Opus 1.6.1 SILK 增益矩阵隐写方案

在 Opus 1.6.1 音频编解码器的 SILK 编码模块中，利用 4 个子帧增益索引（GainsIndices[0..3]，每个 0~63）作为隐写载体，通过矩阵编码嵌入秘密信息。

## 核心算法

```
symbol = (g0×1 + g1×3 + g2×9 + g3×27) & 63   // 6-bit 符号

嵌入: 遍历 625 候选 (d0,d1,d2,d3 ∈ [-2,2])
      → 找 symbol==目标的 → 选 L1 代价最小的 → 修改增益
提取: 重算加权和 & 63 → 恢复 6 bit
```

代价函数：L1 距离 `|d0|+|d1|+|d2|+|d3|` → L2 距离（打平）→ 字典序（再打平）

## Git 版本清单

| Tag | 方案 | PSNR | 容量 | bit_acc | 说明 |
|-----|------|------|------|---------|------|
| `v1-gain-matrix` | 初始增益矩阵 | 46.3 dB | 37.5 B/s | 99.97% | 625 候选暴力搜索，6 bit/帧 |
| `v2-gating` | 门控 MAX_COST=2 | 50.6 dB | 15.2 B/s | 51.9% | 高代价帧跳过，缺同步机制 |
| `v3-4bit-baseline` | 4 bit/帧 Baseline | 48.6 dB | 24.8 B/s | 99.93% | 预算器控制嵌入率，字节对齐 |
| `v4-standard-api` | **标准化 v4** ← 当前 | 46.3 dB | 37.5 B/s | 100% | seq+XOR-3 FEC 丢包对抗 + Watermark API + 文档 |
| `v7-planA-stc` | Plan A 跨帧 STC | 48.4 dB | 18.8 B/s | 99.9% | Python 两遍编码 + STC Viterbi |

```bash
git checkout <tag>   # 切换到任意版本
```

## v4-standard-api（当前版本）

基于 v4 丢包对抗方案，按《交付代码规范.md》8 条要求标准化：

- **API 层**: `include/watermark.h` — WatermarkEncode / WatermarkDecode 标准接口
- **动态库**: `watermark/` → `libopuswatermark.dll`（含 Opus + 隐写 API）
- **测试程序**: `test/test_watermark.exe`（命令行，与核心算法分离）
- **文档**: `doc/algorithm.md` `doc/code_organization.md` `doc/api_reference.md` `doc/usage.md`
- **无隐写一致性**: `watermarkLen==0` 时输出与原始 Opus 完全一致
- **计时打点**: 返回编解码耗时（微秒）

### 构建

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
# 产物: watermark/libopuswatermark.dll, test/test_watermark.exe
```

### 测试

```bash
# C 测试程序
./test/test_watermark --input test.wav --bitrate 6000 --bps 300 --seed 42

# 丢包对抗模式
./test/test_watermark --input test.wav --bitrate 6000 --bps 300 --loss 0.15 --resilience --seed 42

# Python 批量验证
python stego_eval/validate_newdata.py \
  --input-dir test_data --output-dir output \
  --opus-demo build/opus_demo.exe \
  --samples-per-dir 10 --sample-seed 20260504 \
  --bitrate 6000 --vbr --stego-bps 300
```

## 丢包对抗机制（v4）

每帧 6 bit 拆为 `[seq:2bit][payload:4bit]`，配合 XOR-3 FEC：

| 模式 | 开关 | PSNR | 净容量 | bit_acc (15%丢包) |
|------|------|------|--------|-------------------|
| 无丢包 | `lossMode=NONE` | 46.3 dB | 37.5 B/s | 100% |
| 丢包对抗 | `lossMode=SEQ_FEC` | 30.6 dB | 16.7 B/s | 96.4% |

## 核心文件

| 文件 | 作用 |
|------|------|
| `silk/stego_pulse_matrix.c` | 增益矩阵嵌入/提取核心 |
| `silk/float/encode_frame_FLP.c` | FLP 编码集成点 |
| `silk/decode_frame.c` | 解码提取点 |
| `watermark/watermark_opus.c` | WatermarkEncode/Decode API 实现 |
| `include/watermark.h` | 公开 API 头文件 |
| `test/test_watermark.c` | 统一测试可执行文件 |
| `stego_eval/validate_newdata.py` | 无丢包批量验证 |
| `stego_eval/validate_lossy.py` | 丢包对抗批量验证 |
| `doc/algorithm.md` | 算法设计文档 |
| `doc/api_reference.md` | API 接口参考 |
