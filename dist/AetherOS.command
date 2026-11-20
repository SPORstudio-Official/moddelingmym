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
