#!/usr/bin/env bash
# ============================================================================
# Build a standalone, double-clickable AetherOS application.
#
#   ./tools/make_app.sh
#
# Produces a single self-contained AetherOS.html and, where the platform
# supports it, a launcher that opens it in an app-style window (no tabs, no
# address bar, no browser chrome).  Nothing needs to be installed: no Electron,
# no Node, no network.
#
# Why not Electron for this?  Electron is 200 MB and needs a per-platform build
# plus code signing before anyone will run it.  For an OS that already runs in
# any browser engine, the honest "standalone app" is one HTML file plus a
# chrome-less launcher.  `npm start` still gives the Electron build for anyone
# who wants a native window and the extra host APIs.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

echo "==> building the WebAssembly image"
./tools/build_wasm.sh

echo "==> bundling into a single file"
node tools/bundle_standalone.js

OUT="$ROOT/dist/AetherOS.html"
[ -f "$OUT" ] || { echo "bundle failed" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Launchers.  Each one opens the HTML in an app-style window.
# ---------------------------------------------------------------------------

# Windows: a .cmd that opens the file in Edge/Chrome's app mode, which gives a
# window with no tabs, no address bar and no menu -- indistinguishable from a
# native app window for this purpose.
cat > "$ROOT/dist/AetherOS.cmd" <<'WIN'
@echo off
REM AetherOS -- open the single-file build in a chrome-less app window.
setlocal
set "APP=%~dp0AetherOS.html"
if not exist "%APP%" (
  echo AetherOS.html is missing next to this launcher.
  pause
  exit /b 1
)
set "BROWSER="
if exist "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" set "BROWSER=%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"
if not defined BROWSER if exist "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" set "BROWSER=%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"
if not defined BROWSER if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" set "BROWSER=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if not defined BROWSER if exist "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe" set "BROWSER=%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"
if defined BROWSER (
  start "" "%BROWSER%" --app="file:///%APP:\=/%" --window-size=1320,800 --disable-features=Translate
) else (
  start "" "%APP%"
)
endlocal
WIN

# macOS: a double-clickable .command
cat > "$ROOT/dist/AetherOS.command" <<'MAC'
#!/bin/bash
# AetherOS -- open the single-file build in a chrome-less app window.
cd "$(dirname "$0")"
APP="$PWD/AetherOS.html"
for b in "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
         "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge" \
         "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"; do
  if [ -x "$b" ]; then exec "$b" --app="file://$APP" --window-size=1320,800; fi
done
open "$APP"     # Safari, full browser chrome, but it runs
MAC
chmod +x "$ROOT/dist/AetherOS.command"

# Linux: a .desktop entry plus a launcher script.
cat > "$ROOT/dist/aether-os.sh" <<'LNX'
#!/bin/bash
# AetherOS -- open the single-file build in a chrome-less app window.
cd "$(dirname "$0")"
APP="$PWD/AetherOS.html"
for b in google-chrome chromium chromium-browser microsoft-edge brave-browser; do
  if command -v "$b" >/dev/null 2>&1; then
    exec "$b" --app="file://$APP" --window-size=1320,800
  fi
done
xdg-open "$APP"
LNX
chmod +x "$ROOT/dist/aether-os.sh"

cat > "$ROOT/dist/AetherOS.desktop" <<'DSK'
[Desktop Entry]
Type=Application
Name=AetherOS
Comment=An operating system that boots inside a window
Exec=sh -c 'cd "$(dirname "%k")" 2>/dev/null; ./aether-os.sh'
Icon=applications-system
Terminal=false
Categories=System;Utility;
DSK

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
echo "standalone app ready:"
for f in AetherOS.html AetherOS.cmd AetherOS.command aether-os.sh AetherOS.desktop; do
  if [ -f "$ROOT/dist/$f" ]; then
    printf '  %-20s %8s bytes\n' "dist/$f" "$(stat -c%s "$ROOT/dist/$f")"
  fi
done
echo
echo "  Windows : double-click dist\\AetherOS.cmd"
echo "  macOS   : double-click dist/AetherOS.command"
echo "  Linux   : run dist/aether-os.sh   (or install AetherOS.desktop)"
echo
echo "  The .html alone also works: double-click it in any browser."
