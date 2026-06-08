# API 参考文档

## WatermarkEncode

```c
int WatermarkEncode(
    const int16_t *pcmStream,      /* 输入 */
    size_t         pcmLen,          /* 输入 */
    const uint8_t *watermarkBytes, /* 输入 */
    size_t         watermarkLen,    /* 输入 */
    int            bitrate,         /* 输入 */
    int            stegoBps,        /* 输入 */
    int            lossMode,        /* 输入 */
    uint8_t       *opusStream,      /* 输出 */
    size_t        *opusLen,         /* 输入/输出 */
    int64_t       *encodeTimeUs     /* 输出 */
);
```

将水印字节嵌入 PCM 音频，输出 Opus 码流。

| 参数 | 类型 | 方向 | 说明 |
|------|------|------|------|
| `pcmStream` | `const int16_t*` | 输入 | PCM 采样（s16le, 48000 Hz, 单声道） |
| `pcmLen` | `size_t` | 输入 | PCM 采样数 |
| `watermarkBytes` | `const uint8_t*` | 输入 | 待嵌入的隐秘数据（可为 NULL，此时 `watermarkLen` 必须为 0） |
| `watermarkLen` | `size_t` | 输入 | 隐秘数据字节数。为 0 时输出与原始 Opus 编码完全一致（规范要求 1） |
| `bitrate` | `int` | 输入 | Opus 目标码率（bps），如 6000、16000 |
| `stegoBps` | `int` | 输入 | 隐写嵌入率（bps），如 300 → 6 bit/帧。小于等于 0 时关闭隐写 |
| `lossMode` | `int` | 输入 | 丢包对抗模式：`WATERMARK_LOSS_NONE`（0）或 `WATERMARK_LOSS_SEQ_FEC`（1） |
| `opusStream` | `uint8_t*` | 输出 | Opus 码流缓冲区。格式：`[pkt_len:4 BE][final_range:4 BE][opus_data:pkt_len]` 重复 |
| `opusLen` | `size_t*` | 输入/输出 | 输入为缓冲区容量（字节），输出为实际写入长度 |
| `encodeTimeUs` | `int64_t*` | 输出 | 编码耗时（微秒），可为 NULL |
| **返回值** | `int` | — | `WATERMARK_OK`(0) 成功，负数表示错误码 |

### 码流格式

输出的 `opusStream` 由连续 Opus 包组成，每包格式：
```
[4 字节 BE: 包体长度 N] [4 字节 BE: final_range] [N 字节: Opus 编码数据]
```

---

## WatermarkDecode

```c
int WatermarkDecode(
    const uint8_t *opusStream,     /* 输入 */
    size_t         opusLen,         /* 输入 */
    int            bitrate,         /* 输入 */
    int            stegoBps,        /* 输入 */
    int            lossMode,        /* 输入 */
    const int     *erasureFlags,    /* 输入 */
    int            numFrames,       /* 输入 */
    int16_t       *pcmStream,       /* 输出 */
    size_t        *pcmLen,          /* 输入/输出 */
    uint8_t       *watermarkBytes,  /* 输出 */
    size_t        *watermarkLen,    /* 输入/输出 */
    int64_t       *decodeTimeUs     /* 输出 */
);
```

从 Opus 码流提取水印并解码 PCM。

| 参数 | 类型 | 方向 | 说明 |
|------|------|------|------|
| `opusStream` | `const uint8_t*` | 输入 | Opus 码流（格式同上） |
| `opusLen` | `size_t` | 输入 | 码流长度（字节） |
| `bitrate` | `int` | 输入 | Opus 码率（当前为信息性参数，预算同步通过 stegoBps） |
| `stegoBps` | `int` | 输入 | 隐写嵌入率（bps），需与编码端一致 |
| `lossMode` | `int` | 输入 | 与编码端一致的丢包对抗模式 |
| `erasureFlags` | `const int*` | 输入 | 每帧丢包标志数组（NULL = 无丢包）。`erasureFlags[i]==1` 表示第 i 帧丢失，触发 PLC 重建 |
| `numFrames` | `int` | 输入 | 总帧数（传 0 则自动检测，读到码流末尾为止） |
| `pcmStream` | `int16_t*` | 输出 | 解码后的 PCM 采样 |
| `pcmLen` | `size_t*` | 输入/输出 | 输入为 PCM 缓冲区容量（采样数），输出为实际解码采样数 |
| `watermarkBytes` | `uint8_t*` | 输出 | 恢复的隐秘数据 |
| `watermarkLen` | `size_t*` | 输入/输出 | 输入为缓冲区容量（字节），输出为实际恢复字节数 |
| `decodeTimeUs` | `int64_t*` | 输出 | 解码耗时（微秒），可为 NULL |
| **返回值** | `int` | — | `WATERMARK_OK`(0) 成功，负数表示错误码 |

---

## 宏定义

| 宏 | 值 | 说明 |
|----|-----|------|
| `WATERMARK_OK` | 0 | 成功 |
| `WATERMARK_ERR_NULL` | -1 | 空指针参数 |
| `WATERMARK_ERR_ENCODE` | -2 | Opus 编码失败 |
| `WATERMARK_ERR_DECODE` | -3 | Opus 解码失败 |
| `WATERMARK_ERR_MEM` | -4 | 内存分配失败 |
| `WATERMARK_LOSS_NONE` | 0 | 无丢包对抗（最大容量） |
| `WATERMARK_LOSS_SEQ_FEC` | 1 | [seq:2][payload:4] + XOR-3 FEC 丢包对抗 |

---

## 丢包对抗模式详解

当 `lossMode == WATERMARK_LOSS_SEQ_FEC`：

**编码端**：
1. 对 watermark 字节做 XOR-3 FEC 编码（每 2 nibble 产生 1 校验 nibble，33% 开销）
2. 每帧嵌入 `[seq:2bit][payload:4bit]`，seq 模 4 循环递增

**解码端**：
1. 从每帧提取 seq + payload
2. 检测 seq 间隙 → 自动插入零占位 nibble（标记为丢失）
3. XOR-3 解码器恢复：每 3 nibble 块中任意 1 个丢失可通过 XOR 恢复

**容量计算**：
- 原始容量：300 bps → 6 bit/帧 → 37.5 B/s
- seq 开销：2/6 = 33%
- FEC 开销：(3-2)/3 = 33%
- 净容量：37.5 × (4/6) × (2/3) = 16.7 B/s
