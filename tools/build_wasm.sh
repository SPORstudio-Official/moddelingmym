#!/usr/bin/env bash
# ============================================================================
# AE -- WebAssembly build
#
#   Compiles the same kernel sources used by the Windows executable into a
#   freestanding wasm32 module that the Electron shell in app/ instantiates.
#
#   Nothing about the OS changes: this is the identical compositor, scheduler,
#   VFS and bytecode VM.  Only the chassis differs, exactly as it does for the
#   Win32 and headless builds -- a slip of glue that hands the kernel a block of
#   memory, a framebuffer, a clock and raw input events.
#
#   Usage:
#     ./tools/build_wasm.sh            # -> app/aether.wasm
#     ./tools/build_wasm.sh --check    # compile only, discard the output
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

# zig is fetched by tools/setup_toolchain.sh (Windows build) and is also the
# wasm toolchain: it ships a wasm32-freestanding target and wasm-ld.
ZIG=${ZIG:-$HOME/.local/zig/zig}
if [ ! -x "$ZIG" ]; then
  if command -v zig >/dev/null 2>&1; then ZIG=$(command -v zig)
  else echo "zig not found -- run tools/setup_toolchain.sh" >&2; exit 1; fi
fi

OUT=${OUT:-app/aether.wasm}
mkdir -p app

# Same concatenation order as the Windows build, so the two targets are
# guaranteed to be compiled from byte-identical inputs.
MODULES=(
  dev/00_prelude.c
  dev/font_data.h
  dev/10_util.c
  dev/20_hal.c
  dev/30_irq.c
  dev/40_gfx.c
  dev/50_mm.c
  dev/60_vfs.c
  dev/65_proc.c
  dev/67_procfs.c
  dev/70_bytecode.c
  dev/75_registry.c
  dev/76_mcp.c
  dev/80_wm.c
  dev/85_app_sh.c
  dev/86_app_sysmon.c
  dev/87_app_explorer.c
  dev/88_app_edit.c
  dev/89_app_hexview.c
  dev/90_app_paint.c
  dev/91_app_settings.c
  dev/92_app_calc.c
  dev/93_app_console.c
  dev/74_html.c
  dev/94_app_browser.c
  dev/95_main.c
)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TU=$TMP/aether_wasm.c
: > "$TU"
for f in "${MODULES[@]}"; do
  {
    echo
    echo "/* ======================== $f ======================== */"
    cat "$f"
  } >> "$TU"
done

# Exports come from __attribute__((export_name(...))) on the entry points in
# dev/95_main.c, so no linker --export flags are needed here.

# -nostdlib: the kernel brings its own string/memory/maths routines, exactly as
# it does on Windows.  --no-entry: there is no _start, the shell calls the
# exported entry points itself.  Memory is exported so the shell can read the
# framebuffer directly.
[ "${1:-}" = "--check" ] || set -x
"$ZIG" cc -target wasm32-freestanding -O2 -std=c11 -g \
    -DAETHER_WASM -Wall -Wextra \
    -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
    -nostdlib -Wl,--no-entry -Wl,--export-memory \
    \
    "$TU" -o "$OUT"
[ "${1:-}" = "--check" ] || set +x

if [ "${1:-}" = "--check" ]; then
  rm -f "$OUT"
  echo "wasm compile check passed"
else
  echo "built $OUT ($(stat -c%s "$OUT") bytes)"
fi
