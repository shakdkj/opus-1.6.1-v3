# 使用说明

## 1. 环境要求

| 工具 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.16 | 构建系统 |
| GCC / MinGW | ≥ 8.0 | 编译器（Windows 用 MinGW，Linux 用 GCC） |
| Python | ≥ 3.8 | 批量验证脚本（可选） |

## 2. 构建

```bash
cd opus-1.6.1-v3
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

产物：

| 文件 | 路径 | 说明 |
|------|------|------|
| 动态库 | `watermark/libopuswatermark.dll` (.so on Linux) | 含 Opus + 隐写 API |
| 头文件 | `../include/watermark.h` | 公开接口 |
| 测试程序 | `test/test_watermark.exe` | 命令行测试工具 |
| 传统 CLI | `opus_demo.exe` | 原始 opus_demo（需 `OPUS_BUILD_PROGRAMS=ON`） |

## 3. 测试

### 3.1 C 测试程序

```bash
# 无丢包基线测试
./test/test_watermark --input test.wav --bitrate 6000 --bps 300 --seed 42

# 丢包对抗测试（15% 丢包 + seq + XOR-3 FEC）
./test/test_watermark --input test.wav --bitrate 6000 --bps 300 \
    --loss 0.15 --resilience --seed 42
```

输出示例：
```
# input: test.wav, samples=225760, sr=44100
# encode: opus_bytes=5428, time_us=47000
# decode: pcm_samples=226560, wm_bytes=32, time_us=3000
PSNR: 28.749 dB
bit_acc: 1.0000
```

### 3.2 Python 批量验证（100 样本）

```bash
python stego_eval/validate_newdata.py \
    --input-dir test_data \
    --output-dir stego_eval/output_test \
    --opus-demo build/opus_demo.exe \
    --samples-per-dir 10 \
    --sample-seed 20260504 \
    --bitrate 6000 --vbr --stego-bps 300
```

### 3.3 丢包模拟测试

```bash
python stego_eval/validate_lossy.py \
    --input-dir test_data \
    --output-dir stego_eval/output_lossy \
    --opus-demo build/opus_demo.exe \
    --bitrate 6000 --loss-rate 0.15
```

## 4. 集成到第三方项目

### 4.1 链接动态库

```c
#include "watermark.h"

// 编码
int ret = WatermarkEncode(pcm, pcm_len,
                           watermark, watermark_len,
                           6000,    // bitrate
                           300,     // stego bps
                           WATERMARK_LOSS_NONE,
                           opus_buf, &opus_len, NULL);

// 解码
ret = WatermarkDecode(opus_buf, opus_len,
                       6000, 300,
                       WATERMARK_LOSS_NONE,
                       NULL, 0,    // no erasure
                       pcm_out, &pcm_out_len,
                       recovered, &recovered_len, NULL);
```

### 4.2 CMake 集成

```cmake
find_library(OPUSWATERMARK_LIB opuswatermark PATHS /path/to/build/watermark)
target_link_libraries(your_app PRIVATE ${OPUSWATERMARK_LIB})
target_include_directories(your_app PRIVATE /path/to/opus-1.6.1-v3/include)
```

## 5. 参数速查

| 场景 | bitrate | stegoBps | lossMode | 预期 PSNR | 预期容量 |
|------|---------|----------|----------|-----------|----------|
| 最大容量 | 6000 | 300 | NONE | 46.3 dB | 37.5 B/s |
| 平衡质量 | 6000 | 150 | NONE | 48.5 dB | 18.8 B/s |
| 最佳质量 | 6000 | 100 | NONE | 49.4 dB | 12.6 B/s |
| 丢包对抗 | 6000 | 300 | SEQ_FEC | 30.6 dB | 16.7 B/s |
| 高质量码率 | 24000 | 300 | NONE | 48.3 dB | 36.8 B/s |

## 6. 限制与已知问题

- 输入 PCM 必须为 48000 Hz 单声道 16-bit（API 不做重采样）
- 在 MinGW GCC 下 `OPUS_GET_GAIN_INDICES` 返回未初始化数据（不影响隐写提取）
- 丢包对抗模式下 net 有效容量约为标称值的一半（seq + FEC 开销）
