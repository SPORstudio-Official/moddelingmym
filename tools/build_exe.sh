#!/usr/bin/env bash
# ============================================================================
# Build AE.exe -- the standalone Windows application.
#
#   ./tools/build_exe.sh
#
# Produces one self-contained .exe that carries the whole single-file AE
# build inside it. Double-click it and the OS opens in an app-style window.
#
# Two parts, joined by a marker:
#   1. tools/launcher/launcher.c compiled freestanding to a tiny GUI executable
#      (no C runtime; imports only kernel32, shell32 and advapi32)
#   2. dist/AE.html appended after the PE image
#
# The launcher finds the marker, writes the payload to %TEMP%\AE.html and
# opens it in Edge or Chrome --app mode. Every failure path shows a message box,
# so the exe can never fail silently the way the old build did.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

ZIG=${ZIG:-$HOME/.local/zig/zig}
if [ ! -x "$ZIG" ]; then
  if command -v zig >/dev/null 2>&1; then ZIG=$(command -v zig)
  else echo "zig not found -- run tools/setup_toolchain.sh" >&2; exit 1; fi
fi

echo "==> building the WebAssembly image and the single-file app"
./tools/build_wasm.sh
node tools/bundle_standalone.js

HTML=dist/AE.html
[ -f "$HTML" ] || { echo "missing $HTML" >&2; exit 1; }

echo "==> compiling the launcher stub"
# -nostdlib also strips the include search path, so point the compiler at zig's
# bundled mingw headers explicitly (headers only -- nothing is linked in).
ZIG_LIBC=$(dirname "$ZIG")/lib/libc/include/any-windows-any
[ -d "$ZIG_LIBC" ] || ZIG_LIBC=$(dirname "$ZIG")/lib/libc/include/x86_64-windows-any

STUB=$(mktemp -d)/AE-stub.exe
trap 'rm -rf "$(dirname "$STUB")"' EXIT

# Resources: the AE icon and the version metadata that Explorer, the Start menu,
# search and the taskbar read instead of the host browser's.  `zig rc` is a
# drop-in rc.exe, so no Windows toolchain is needed to build them.
echo "==> compiling the icon and version resources"
RES=$(mktemp -d)/ae.res
"$ZIG" rc /fo "$RES" tools/launcher/ae.rc

# -nostdlib: no C runtime, exactly like the kernel build.  The launcher supplies
# its own string helpers, so the import table stays at kernel32/shell32/advapi32.
"$ZIG" cc -target x86_64-windows-gnu -O2 -std=c11 \
    -Wl,--subsystem,windows -Wl,--entry,aether_launcher_entry \
    -nostdlib -isystem "$ZIG_LIBC" \
    tools/launcher/launcher.c "$RES" -o "$STUB" \
    -Wl,--stack,1048576 \
    -lkernel32 -lshell32 -ladvapi32 -luser32

echo "==> appending the payload"
OUT=dist/AE.exe
cat "$STUB" "$HTML" > "$OUT"

# --- verify the result -----------------------------------------------------
echo "==> verifying"
python3 - "$OUT" <<'PY'
import sys, struct, os
path = sys.argv[1]
data = open(path, 'rb').read()

assert data[:2] == b'MZ', 'missing MZ'
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe+4] == b'PE\0\0', 'missing PE signature'
machine = struct.unpack_from('<H', data, pe+4)[0]
subsystem = struct.unpack_from('<H', data, pe+24+68)[0]
magic = struct.unpack_from('<H', data, pe+24)[0]
assert machine == 0x8664, 'not x86-64: %#x' % machine
assert magic == 0x20b, 'not PE32+'
assert subsystem == 2, 'not a GUI subsystem (%d)' % subsystem

marker = b'\nAETHEROS-PAYLOAD-V1\n'
assert data.find(marker) > 0, 'payload marker not found in the stub'
anchor = b'<!DOCTYPE html'
idx = data.find(anchor)
assert idx > 0, 'payload anchor not found'
payload = data[idx:]
assert payload.rstrip().endswith(b'</html>'), 'payload is truncated'
assert b'WebAssembly' in payload and b'aether-host' not in payload[:0], 'payload is not the app'
# The launcher was linked with the marker; make sure the two agree.
assert data[:2] == b'MZ'

print('  file      %s' % path)
print('  size      %s bytes' % format(len(data), ','))
print('  machine   x86-64, PE32+, GUI subsystem')
print('  stub      %s bytes' % format(idx, ','))
print('  payload   %s bytes' % format(len(payload), ','))
print('  payload   %s' % payload[:15].decode('ascii', 'replace'))
PY

sha=$(sha256sum "$OUT" | cut -d' ' -f1)
printf '\nbuilt %s (%s bytes)\nsha256 %s\n' "$OUT" "$(stat -c%s "$OUT")" "$sha"
