#!/usr/bin/env bash
# Build the headless Linux harness (drives the shared game core, dumps BMP/WAV).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
gcc -O2 -w -std=c99 -o build/harness \
    src/harness.c src/game.c src/draw.c src/dsp.c src/content.c src/face.c -lm
echo "built build/harness"
