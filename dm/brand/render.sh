#!/bin/bash
# Renders the brand pieces of brand.html with headless Chrome at 2x (its own throwaway profile, never the user's), transparent where the
# piece is: out/<piece>.png, except the app icon's layers (icon-iron, icon-frame, icon-rune), which go into AppIcon.icon/Assets/; the
# wordmark and lockup are trimmed to their content. After a change, run it, then check the icon with icon.sh. Needs Google Chrome and Pillow
# (the workspace venv). Usage: dm/brand/render.sh [piece ...]   (no pieces = all)
set -eu
B=$(cd "$(dirname "$0")" && pwd); CHROME=${CHROME:-'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'}
[ -x "$CHROME" ] || { echo "🔴 Google Chrome not found (set CHROME=<path>)"; exit 1; }
PIECES=("$@"); [ ${#PIECES[@]} -gt 0 ] || PIECES=(emblem wordmark lockup icon-iron icon-frame icon-rune icon-flat)
size() { case "$1" in wordmark) echo 760,150;; lockup) echo 1000,330;; *) echo 512,512;; esac; }
P=$(mktemp -d); trap 'rm -rf "$P"' EXIT; mkdir -p "$B/out"
for p in "${PIECES[@]}"; do
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --user-data-dir="$P" --force-device-scale-factor=2 \
    --default-background-color=00000000 --window-size="$(size "$p")" --virtual-time-budget=4000 \
    --screenshot="$B/out/$p.png" "file://$B/brand.html#$p" > /dev/null 2>&1
  case "$p" in icon-iron|icon-frame|icon-rune) mv "$B/out/$p.png" "$B/AppIcon.icon/Assets/${p#icon-}.png"; echo "$p → AppIcon.icon/Assets/${p#icon-}.png"; continue;; esac
  case "$p" in wordmark|lockup) python3 -I -c '
import sys; from PIL import Image
im = Image.open(sys.argv[1]); a = im.getchannel("A").point(lambda v: 255 if v > 6 else 0); x0, y0, x1, y1 = a.getbbox(); m = 12
im.crop((max(x0 - m, 0), max(y0 - m, 0), min(x1 + m, im.width), min(y1 + m, im.height))).save(sys.argv[1], optimize=True)' "$B/out/$p.png";; esac
  echo "$p $(sips -g pixelWidth -g pixelHeight "$B/out/$p.png" | awk '/pixel/{printf "%s ", $2}')$(( $(stat -f %z "$B/out/$p.png") / 1024 )) KB"
done
