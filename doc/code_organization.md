# 代码组织文档

## 项目结构

```
opus-1.6.1-v3/
├── include/
│   ├── opus.h              # Opus 公开 API（原版）
│   ├── opus_defines.h      # Opus CTL 定义（含隐写 CTL）
│   ├── opus_types.h        # Opus 类型定义
│   └── watermark.h         # [新增] WatermarkEncode/WatermarkDecode 接口头文件
├── silk/
│   ├── stego_pulse_matrix.c/h  # 核心算法：增益矩阵嵌入/提取
│   ├── enc_API.c           # SILK 编码器 API（隐写 bit 分发）
│   ├── dec_API.c           # SILK 解码器 API（隐写状态重置）
│   ├── decode_frame.c      # 解码帧处理（隐写提取调用点）
│   ├── float/encode_frame_FLP.c  # 浮点编码（隐写嵌入调用点）
│   ├── fixed/encode_frame_FIX.c  # 定点编码（隐写嵌入调用点）
│   ├── structs.h           # 编解码状态结构体（stego_bits 等字段）
│   ├── control.h           # 编码控制结构体（stego_bits/stego_nbits）
│   └── debug.h             # TIC/TOC 计时宏
├── src/
│   ├── opus_encoder.c      # Opus 编码器（OPUS_SET_STEGO_BITS 处理）
│   ├── opus_decoder.c      # Opus 解码器（OPUS_GET_STEGO_BITS 处理）
│   └── opus_demo.c         # CLI 工具（预算累积器、seq/FEC 逻辑）
├── watermark/              # [新增] API 封装层
│   ├── CMakeLists.txt
│   └── watermark_opus.c    # WatermarkEncode/WatermarkDecode 实现
├── test/                   # [新增] 独立测试代码
│   ├── CMakeLists.txt
│   └── test_watermark.c    # 统一测试可执行文件
├── stego_eval/             # Python 验证脚本
│   ├── validate_newdata.py # 无丢包测试
│   └── validate_lossy.py   # 丢包对抗测试
├── tools/
│   └── stego_metrics.py    # PSNR/比特率计算
├── doc/                    # [新增] 文档
│   ├── algorithm.md        # 算法设计文档
│   ├── code_organization.md # 本文档
│   └── api_reference.md    # API 参考
├── CMakeLists.txt          # 根构建文件（含 watermark/test 子目录）
└── README_STEGO.md         # 项目总体说明
```

## 模块依赖

```
test_watermark.exe
  └── libopuswatermark.dll (watermark/)
        ├── watermark.h (公开头文件)
        └── libopus.a (内部)
              ├── silk/stego_pulse_matrix.c  ← 核心嵌入/提取
              ├── silk/enc_API.c             ← bit 分发
              ├── silk/decode_frame.c        ← bit 收集
              └── src/opus_encoder.c         ← CTL 路由
```

核心算法（silk/stego_pulse_matrix.c）编译进 libopus.a，watermark API 封装层链接 libopus.a 并导出公开接口。测试代码只依赖 watermark.h 和 libopuswatermark.dll。

## 构建

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .

# 产物
ls watermark/libopuswatermark.dll   # 动态链接库
ls test/test_watermark.exe          # 测试可执行文件
```

跨平台要求：使用 CMake + GNU Make，已验证 MinGW (Windows)，兼容 Linux GCC。

## 关键宏定义

| 宏 | 值 | 说明 |
|----|-----|------|
| `SILK_STEGO_MAX_BITS` | 6 | 每帧最大嵌入 bit 数 |
| `STEGO_SEQ_BITS` | 2 | seq 序号占用的 bit 数（丢包模式） |
| `STEGO_PAYLOAD_BITS` | 4 | 每帧有效载荷 bit 数（丢包模式） |
| `OPUS_SET_STEGO_BITS(x)` | 4060 | 编码器 CTL：设置隐写 bit |
| `OPUS_GET_STEGO_BITS(x)` | 4061 | 解码器 CTL：读取隐写 bit |
| `WATERMARK_LOSS_NONE` | 0 | 无丢包对抗 |
| `WATERMARK_LOSS_SEQ_FEC` | 1 | [seq:2][payload:4] + XOR-3 FEC |
