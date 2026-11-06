#!/usr/bin/env bash
# Cross-compile DEAD AIR to a standalone Windows x64 executable.
# Requires: zig (https://ziglang.org/download/)
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG="${ZIG:-zig}"
mkdir -p build

echo "[1/3] compiling resources"
"$ZIG" rc src/deadair.rc build/deadair.res

echo "[2/3] compiling + linking"
"$ZIG" cc -target x86_64-windows-gnu -O2 -std=c99 \
    -Wl,--subsystem,windows \
    -o build/DEAD_AIR.exe \
    src/main_win32.c src/game.c src/draw.c src/dsp.c src/content.c src/face.c \
    build/deadair.res \
    -lwinmm -lgdi32 -luser32

echo "[3/3] done"
ls -la build/DEAD_AIR.exe
