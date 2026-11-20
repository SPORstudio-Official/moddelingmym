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
