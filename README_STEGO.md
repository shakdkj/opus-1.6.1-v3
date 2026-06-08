# Opus 1.6.1 SILK 增益矩阵隐写方案 — 完整技术报告

## 一、项目概述

在 Opus 1.6.1 音频编解码器的 SILK 编码模块中，利用 4 个子帧增益索引（GainsIndices[0..3]，每个 0~47 整数）作为隐写载体，通过矩阵编码嵌入秘密信息。每 20ms 音频帧可嵌入 2~6 bit，对应 100~300 bps。

### 核心算法

```
符号 = (g0×1 + g1×3 + g2×9 + g3×27) & 63   // 6-bit 密码锁

嵌入: 遍历 625 候选 (d0,d1,d2,d3 ∈ [-2,2])
      → 找符号==目标的 → 选 L1 代价最小的 → 修改增益
提取: 重算加权和 & 63 → 恢复 6 bit
```

**代价函数**: L1 距离 `|d0|+|d1|+|d2|+|d3|` → L2 距离（打平）→ 字典序（再打平）

### 测试环境

48000 Hz, 单声道, VBR, 100 样本 (10 子目录 × 10 WAV), seed=20260504

---

## 二、各版本方案与结果

### v1 (`v1-gain-matrix`) — 初始 Gain 矩阵方案

**架构**: 纯矩阵搜索，无门控，每帧独立嵌入 5~6 bit

**100 样本结果** (250 bps):

| PSNR | 容量 | bit_acc |
|------|------|---------|
| 46.6 dB | 31.4 B/s | 99.8% |

PSNR 分布: <40dB 占 12%, 40-50dB 占 49%, 50-60dB 占 33%, >60dB 占 6%

---

### v2 (`v2-gating`) — 门控 MAX_COST_DATA=2

**特殊设计**: 代价 > 2 的帧跳过不嵌入（无同步）。解码端不知情，被跳帧读到的是原始符号（垃圾）。

**100 样本结果**:

| PSNR | 净嵌入容量 | bit_acc |
|------|-----------|---------|
| 50.6 dB (+4.0) | 15.2 B/s (-48%) | 51.9% |

**结论**: 门控有效提升 PSNR 但需要配套同步机制（valid-bit 等均引入新失真）

---

### v3 (`v3-4bit-baseline`) — 4 bit/帧 Baseline

**特殊设计**: 通过 `--stego-bps 200` 使预算器分配 4 bit/帧。4 bit 对齐字节边界（2 帧 = 1 字节），消除尾部截断误差。

**100 样本结果** (200 bps):

| PSNR | 容量 | bit_acc |
|------|------|---------|
| 48.6 dB (+2.0) | 24.8 B/s | 99.93% |

**6 bit/帧测试** (300 bps):

| PSNR | 容量 | bit_acc |
|------|------|---------|
| 46.3 dB | 37.5 B/s | 99.97% |

---

### v4 (`v4-loss-resilience`) — 丢包对抗: seq + XOR-3 FEC

**特殊设计**: 三层抗丢包机制

1. **帧序号 (seq)**: 每帧 6 bit 拆为 `[seq:2][payload:4]`，seq 模 4 循环
2. **间隙检测+补零**: 解码端检测 seq 跳号 → 自动补零占位 → 打印 `SEQ GAP` 日志
3. **XOR-3 FEC**: 2 数据 nibble + 1 校验 nibble（33% 开销），任意 1 个丢失可 XOR 恢复

**与 Opus 内部机制兼容**: PLC 模式下 `lostFlag` 绕过隐写提取（不产生垃圾比特）；LBRR 恢复帧正常提取隐写

**100 样本结果** (15% 随机丢包, VBR):

| PSNR (总/隐写) | 净容量 | bit_acc |
|---------------|--------|---------|
| 30.6 / ~43.5 dB | 16.7 B/s | 96.4% |

**用法**: 不加 `-stego_seq` = 无丢包 baseline；加 `-stego_seq` = 丢包对抗模式

---

### v5 (`v5-stc-framework`) — STC 框架

**特殊设计**: 
- 新增 `silk_stego_gain_cost_map()`: 一遍遍历 625 候选输出 64 符号最小代价
- 新增 `silk/stego_stc.c/h`: Viterbi 编码器 (h=6,w=12) + 提取器
- 新增 API: `OPUS_GET_GAIN_INDICES`, `OPUS_SET_STC_INDEP`, `-stego_stc_target`

**方案**: Python 两遍编码 (Pass1 无 stego 取增益 → STC → Pass2 嵌目标)

**结果**: **bit_acc ~53%，失败**。根因: Pass1 增益 ≠ Pass2 增益（66.6% 偏差），代价表无效

---

### v6 (`v6-intra-stc`) — 帧内 STC

**特殊设计**: 在 C 编码器内，单帧 NSQ 后实时算代价表 → 从 64 符号中选最低代价且低 3 bit 匹配的 → 矩阵嵌入。α=50%（嵌 6 取 3）。

**100 样本结果** (150 bps):

| PSNR | 容量 | bit_acc |
|------|------|---------|
| 48.2 dB | 18.7 B/s | 99.96% |

**结论**: 成功但不敌 4 bit/帧矩阵方案（48.6 dB / 24.8 B/s）

---

### v7 (`v7-planA-stc`) — Plan A 跨帧 STC

**特殊设计**: Python 两遍编码 + STC 跨 2 帧 Viterbi 优化

```
Pass1: 无stego编码 → 解码 → 提取6-bit原始符号 → Hamming距离代价表
       ↓
 STC:  2帧一组, Viterbi最小化总Hamming代价 → 每帧6-bit目标符号
       ↓
Pass2: 打包目标符号为秘密文件 → -stego_in 嵌入 (CODE_INDEPENDENTLY)
```

**关键突破**: 
- 使用 `-stego_in` 绕过 `-stego_stc_target` Bug
- `CODE_INDEPENDENTLY` 保证两遍增益相同 (0% diff 验证)
- 符号提取（-stego_out）替代不可用的 gain dump

**100 样本结果** (300 bps/6kbps VBR):

| PSNR | 净容量 | bit_acc |
|------|--------|---------|
| 48.4 dB | 18.8 B/s | 99.9% |

**限制**:
- 代价表使用 Hamming 距离近似（非真实 L1，因 MinGW 构建下 `OPUS_GET_GAIN_INDICES` 不可用）
- `-stego_stc_target` 命令行选项存在 Bug（改用 `-stego_in` 绕开）
- 容量偏低（α=50% 开销），不敌 4 bit/帧方案

---

## 三、探索过的优化方向

### 已实测（均无 PSNR 改善）

| 方向 | 方法 | 结论 |
|------|------|------|
| 3 子帧 | 只修改 3 个增益 | 最优解中 g3 改动本为 0 |
| 早停 | 按代价递增遍历 | 纯加速，不改变结果 |
| 非对称搜索 | 边界增益扩搜索窗口 | 边界帧占比太低 |
| 权重组合 | [1,5,25,61], [1,7,49,23] | 代价无显著差异 |
| L1 替代代价函数 | 加权 L1/L2/能量感知 | L1 已是 PSNR 数学最优代理 |

### 有效的 PSNR 提升手段

| 方向 | PSNR 变化 | 容量变化 | 代码改动 |
|------|----------|---------|---------|
| 降嵌入率 (4→3→2 bit/帧) | +2~3 dB | -25~50% | 零 |
| 升码率 (6→8→16→24 kbps) | +1~3 dB | 不变 | 零 |
| 门控 MAX_COST=1 | +14 dB | -86% | 需同步 |
| 跨帧 STC (Plan A) | +2.1 dB | -50% | Python 两遍 |

### 零代码改动最优参数（全部基于 v4）

| 码率 | 嵌入率 | bit/帧 | PSNR | 容量 | bit_acc |
|------|--------|--------|------|------|---------|
| 6 kbps | 300 bps | 6 | 46.3 dB | 37.5 B/s | 99.97% |
| 6 kbps | 150 bps | ~3 | 48.5 dB | 18.8 B/s | 99.96% |
| 6 kbps | 100 bps | ~2 | **49.4 dB** | 12.6 B/s | 99.80% |
| 8 kbps | 300 bps | 6 | 47.5 dB | 37.5 B/s | 99.97% |
| 16 kbps | 300 bps | 6 | 47.2 dB | 37.5 B/s | 99.97% |
| 16 kbps | 150 bps | ~3 | 48.9 dB | 18.8 B/s | 99.96% |
| 24 kbps | 300 bps | 6 | 48.3 dB | 36.8 B/s | 98.43% ⚠️ |

> ⚠️ 24kbps 准确率下降因高码率帧数变多，尾部对齐误差放大。

---

## 四、Git 标签清单

| Tag | 方案 | 核心指标 |
|-----|------|---------|
| `v1-gain-matrix` | 初始 gain 矩阵 | PSNR 46.6 dB |
| `v2-gating` | 门控 MAX_COST=2 | PSNR 50.6 dB |
| `v3-4bit-baseline` | 4 bit/帧 | PSNR 48.6 dB |
| `v4-loss-resilience` | seq+XOR-3 丢包对抗 | 96.4% bit_acc |
| `v5-stc-framework` | STC 基础设施 | 未完成 |
| `v6-intra-stc` | 帧内 STC | PSNR 48.2 dB |
| `v7-planA-stc` | Plan A 跨帧 STC | PSNR 48.4 dB |

克隆后切换: `git checkout <tag>`

---

## 五、核心文件

| 文件 | 作用 |
|------|------|
| `silk/stego_pulse_matrix.c` (257行) | 矩阵嵌入/提取核心 |
| `silk/stego_pulse_matrix.h` | `SILK_STEGO_MAX_BITS=6` |
| `silk/float/encode_frame_FLP.c` | FLP 编码集成点 |
| `silk/decode_frame.c` | 解码提取点 |
| `src/opus_demo.c` | 测试脚手架（预算累积器 + 命令行） |
| `stego_eval/validate_newdata.py` | 无丢包测试脚本 |
| `stego_eval/validate_lossy.py` | 丢包对抗测试脚本 |
| `stego_eval/validate_planA.py` | Plan A STC 测试脚本 |

## 六、已知问题

### bit_acc 未达 100% 的原因

bit_acc 99.8%~99.97%（非 100%）来自**尾部字节对齐**：

- **6 bit/帧**: 秘密 N 字节 = 8N bit，每帧嵌入 5~6 bit，最后一帧不满。
  解码端按预算提取固定位数，多读的 bit 是原始符号 LSB（噪声），导致最后一帧部分 bit 错误。
- **4 bit/帧**: 4 整除 8，每 2 帧精确产出 1 字节，尾部对齐。残差 0.07% 来自音频太短、秘密未完全嵌入。
- **2 bit/帧**: 同理对齐，残差更小。

### MinGW 构建注意

`OPUS_GET_GAIN_INDICES` 在 MinGW GCC 下返回未初始化数据。如需使用 gain dump 功能，建议用 MSVC (NMake) 构建。

---

## 七、复现步骤

```bash
git clone https://github.com/shakdkj/opus-1.6.1-v3.git
cd opus-1.6.1-v3
git checkout <tag>

mkdir build && cd build && cmake .. -G "MinGW Makefiles"
cmake --build .

cd ..
python stego_eval/validate_newdata.py \
  --input-dir test_data --output-dir output \
  --opus-demo build/opus_demo.exe \
  --samples-per-dir 10 --sample-seed 20260504 \
  --bitrate 6000 --vbr --stego-bps 300
```
