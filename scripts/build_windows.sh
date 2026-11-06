#!/usr/bin/env bash
# Cross-compile DEAD AIR to a standalone Windows x64 executable.
# Requires: zig (https://ziglang.org/download/)
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -z "${ZIG:-}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  elif python3 -c "import ziglang, os, sys; sys.stdout.write(os.path.join(os.path.dirname(ziglang.__file__), 'zig'))" >/dev/null 2>&1; then
    ZIG="$(python3 -c "import ziglang, os, sys; sys.stdout.write(os.path.join(os.path.dirname(ziglang.__file__), 'zig'))")"
  else
    echo "zig not found: set ZIG=/path/to/zig (or pip install ziglang)" >&2
    exit 1
  fi
fi
mkdir -p build

echo "[1/3] compiling resources"
"$ZIG" rc src/deadair.rc build/deadair.res

echo "[2/3] compiling + linking"
"$ZIG" cc -target x86_64-windows-gnu -O2 -std=c99 \
    -Wl,--subsystem,windows \
    -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va \
    -o build/DEAD_AIR.exe \
    src/main_win32.c src/game.c src/draw.c src/dsp.c src/content.c src/face.c \
    build/deadair.res \
    -lwinmm -lgdi32 -luser32

echo "[3/3] done"
ls -la build/DEAD_AIR.exe
