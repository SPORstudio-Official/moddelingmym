#!/usr/bin/env bash
# Fast syntax check of every module, compiled as one translation unit for the
# headless target.  Catches errors without needing the Windows toolchain.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=/tmp/aether_syntax.c
{
  echo '#define _GNU_SOURCE 1'
  for f in dev/00_prelude.c dev/font_data.h dev/10_util.c dev/20_hal.c \
           dev/30_irq.c dev/40_gfx.c dev/50_mm.c dev/60_vfs.c dev/65_proc.c dev/67_procfs.c \
           dev/70_bytecode.c dev/75_registry.c dev/80_wm.c dev/85_app_sh.c \
           dev/86_app_sysmon.c dev/87_app_explorer.c dev/88_app_edit.c \
           dev/89_app_hexview.c dev/90_app_paint.c dev/91_app_settings.c \
           dev/92_app_calc.c dev/93_app_console.c dev/95_main.c; do
    [ -f "$f" ] || continue
    echo "/* ---- $f ---- */"
    cat "$f"
  done
} > $OUT
FLAGS="-fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare"
rc=0
# Headless chassis (host libc available).
gcc $FLAGS -std=gnu11 -DAETHER_HEADLESS $OUT || rc=1

# WebAssembly chassis: freestanding, so no libc header may leak in.  zig cannot
# do a front-end-only pass (-fsyntax-only still emits), so run the real build in
# check mode, which discards the artefact.  Skipped when zig is absent.
if [ -n "${WASM_CC:-}" ] || command -v zig >/dev/null 2>&1; then
    ZIG=${WASM_CC:-$(command -v zig)} ./tools/build_wasm.sh --check || rc=1
else
    echo "syntax_check: zig not found, skipping the wasm target" >&2
fi
exit $rc
