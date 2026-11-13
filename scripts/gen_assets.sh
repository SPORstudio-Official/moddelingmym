#!/usr/bin/env bash
# Regenerate the embedded assets (needs python3 + pillow + numpy).
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/gen_font.py src/font_data.h
python3 tools/gen_audio.py assets/audio src/audio_data.h
python3 tools/gen_icon.py
