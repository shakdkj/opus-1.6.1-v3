# CLAUDE.md

## Shell preference
- Use **PowerShell** for all shell commands. This is a Windows-native project.
- Only use Bash for explicitly POSIX-only tasks.

## Build
- Build directory: `build_v4_mingw2/`
- Config: `cmake .. -G "MinGW Makefiles"`
- Build command: `cmake --build build_v4_mingw2`
- Key outputs:
  - `build_v4_mingw2/watermark/libopuswatermark.dll` — shared library (watermark API)
  - `build_v4_mingw2/test/test_watermark.exe` — unified test executable
  - `build_v4_mingw2/opus_demo.exe` — legacy CLI tool (if OPUS_BUILD_PROGRAMS=ON)
  - `build_v4_mingw2/libopus.a` — static library

## Test
- C test: `build_v4_mingw2/test/test_watermark.exe --input <wav> --bitrate 6000 --bps 300`
- Python validation: `python stego_eval/validate_newdata.py`
  - Typical args: `--input-dir test_data --output-dir stego_eval/output_<name> --opus-demo build_v4_mingw2/opus_demo.exe --samples-per-dir 10 --sample-seed 20260504 --bitrate 6000 --vbr --stego-bps 300`
- Test data: `test_data/` (10 subdirs, WAV files)

## Code standards (交付代码规范.md)
See `D:\new_method\交付代码规范.md` for the 8 delivery requirements.
Key compliance documents:
- `doc/algorithm.md` — algorithm design
- `doc/code_organization.md` — file tree and module dependencies
- `doc/api_reference.md` — WatermarkEncode/WatermarkDecode API reference
- `include/watermark.h` — public API header
