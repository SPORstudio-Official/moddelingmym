#!/usr/bin/env bash
# Installs the Windows cross-compiler toolchain (Zig, ships MinGW-w64 headers/libs)
# into ~/.local so the repo stays clean. Idempotent.
set -e
ZIGDIR="$HOME/.local/zig"
if [ -x "$ZIGDIR/zig" ]; then echo "zig already installed: $($ZIGDIR/zig version)"; exit 0; fi
mkdir -p "$HOME/.local"
TMP=$(mktemp -d)
python3 -m pip download ziglang -d "$TMP/pkgs" --no-deps -q
python3 -m pip install --break-system-packages --no-deps --target "$TMP/site" "$TMP"/pkgs/ziglang-*.whl -q
mkdir -p "$ZIGDIR"
cp -r "$TMP/site/ziglang/zig" "$ZIGDIR/zig"
cp -r "$TMP/site/ziglang/lib" "$ZIGDIR/lib"
rm -rf "$TMP"
echo "installed zig $($ZIGDIR/zig version) at $ZIGDIR"
