# CLAUDE.md

## Shell preference
- Use **PowerShell** for all shell commands. This is a Windows-native project (MSVC build, Windows paths, Git for Windows).
- Only use Bash for explicitly POSIX-only tasks.

## Build
- Build directory: `build_codex/`
- Build command: `cmake --build build_codex --config Release`
- Key outputs: `build_codex/opus_demo.exe`, `build_codex/opus_compare.exe`, `build_codex/libopus.a`

## Test
- Validation script: `python stego_eval/validate_newdata.py`
- Typical args: `--input-dir test_data --output-dir stego_eval/output_<name> --opus-demo build_codex/opus_demo.exe --samples-per-dir 10 --sample-seed 20260504 --bitrate 6000 --vbr --stego-bps 250`
- Test data: `test_data/` (10 subdirs, WAV files)
