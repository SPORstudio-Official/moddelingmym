#!/usr/bin/env bash
# Build AE for the host (Linux) with the headless chassis and run a script.
#   tools/headless_run.sh [script-file]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=/tmp/aether_hl.c
{
  for f in dev/00_prelude.c dev/font_data.h dev/10_util.c dev/20_hal.c \
           dev/30_irq.c dev/40_gfx.c dev/50_mm.c dev/60_vfs.c dev/65_proc.c dev/67_procfs.c \
           dev/70_bytecode.c dev/75_registry.c dev/80_wm.c dev/85_app_sh.c \
           dev/86_app_sysmon.c dev/87_app_explorer.c dev/88_app_edit.c \
           dev/89_app_hexview.c dev/90_app_paint.c dev/91_app_settings.c \
           dev/92_app_calc.c dev/93_app_console.c dev/74_html.c dev/94_app_browser.c dev/95_main.c; do
    echo "/* ---- $f ---- */"
    cat "$f"
  done
} > $OUT
gcc -O2 -std=gnu11 -DAETHER_HEADLESS -Wall -Wextra \
    -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
    $OUT -o /tmp/aether_hl -lm
if [ $# -ge 1 ]; then AETHER_SCRIPT="$1" timeout 120 /tmp/aether_hl 2>&1; else
  AETHER_SCRIPT=/dev/null timeout 120 /tmp/aether_hl; fi
