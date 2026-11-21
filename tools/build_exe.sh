#!/usr/bin/env bash
# ============================================================================
# Build AE.exe -- the standalone Windows application.
#
#   ./tools/build_exe.sh
#
# Produces one self-contained .exe that carries the whole operating system
# inside it.  Double-click it and AE starts as a native Windows application.
#
# Two parts, joined end to end:
#   1. tools/launcher/launcher.c compiled freestanding to a tiny GUI executable
#      (no C runtime; imports only kernel32/shell32/advapi32/user32).  It owns
#      the icon, the version metadata, the AppUserModelID and the Start-menu
#      entry, extracts the image once into %LOCALAPPDATA%\AE and runs it.
#   2. dist/AE-os.exe -- the OS itself, a complete second PE image built from
#      the same dev/ sources against Win32.  It creates its own window with
#      GDI, runs its own event loop, paints every pixel with its own compositor
#      and fetches pages with WinHTTP.  No browser, no engine, no web page.
#
# The browser build (dist/AE.html, WebAssembly) is still produced as a
# companion, but it is no longer what the application runs.
#
# Every failure path shows a message box, so the exe can never fail silently.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

ZIG=${ZIG:-$HOME/.local/zig/zig}
if [ ! -x "$ZIG" ]; then
  if command -v zig >/dev/null 2>&1; then ZIG=$(command -v zig)
  else echo "zig not found -- run tools/setup_toolchain.sh" >&2; exit 1; fi
fi

# Two artefacts come out of the build:
#
#   dist/AE-os.exe   the operating system itself -- the native Win32 build of
#                    dev/*.c (own window, own event loop, own compositor, own
#                    HTTP client).  This is the payload AE.exe carries.
#   dist/AE.html     the same OS compiled to WebAssembly, for the browser.  Kept
#                    as a companion because it needs no install, but it is no
#                    longer what the application runs.
echo "==> building the native operating system image"
ZIG="$ZIG" ./tools/build.sh

OS=AE.exe
[ -f "$OS" ] || { echo "missing $OS" >&2; exit 1; }
cp "$OS" dist/AE-os.exe

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
cat "$STUB" dist/AE-os.exe > "$OUT"

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

# The payload is a second, complete PE image: the OS itself.  Find it by the
# marker, then walk forward to the next MZ/PE header.
m = data.find(marker)
start = m if m > 0 else 0
idx = -1
while True:
    idx = data.find(b'MZ', start)
    if idx < 0:
        raise AssertionError('no PE image found after the launcher')
    pe = struct.unpack_from('<I', data, idx + 0x3c)[0]
    if data[idx + pe:idx + pe + 4] == b'PE\0\0':
        break
    start = idx + 2
payload = data[idx:]
pl_pe = struct.unpack_from('<I', payload, 0x3c)[0]
assert payload[pl_pe:pl_pe + 4] == b'PE\0\0', 'payload is not a PE image'
pl_sub = struct.unpack_from('<H', payload, pl_pe + 24 + 68)[0]
assert pl_sub == 2, 'payload is not a GUI application (%d)' % pl_sub
assert len(payload) > 100000, 'payload is too small to be the OS'

print('  file      %s' % path)
print('  size      %s bytes' % format(len(data), ','))
print('  machine   x86-64, PE32+, GUI subsystem')
print('  stub      %s bytes' % format(idx, ','))
print('  payload   %s bytes' % format(len(payload), ','))
print('  payload   %s' % payload[:15].decode('ascii', 'replace'))
PY

sha=$(sha256sum "$OUT" | cut -d' ' -f1)
printf '\nbuilt %s (%s bytes)\nsha256 %s\n' "$OUT" "$(stat -c%s "$OUT")" "$sha"
