#!/bin/bash
# Puts the project's app icon into an app bundle's Resources folder and prints the Info.plist keys for it. With Xcode's actool,
# AppIcon.icon (the layered Liquid Glass icon: iron, bronze frame, molten rune) compiles to Assets.car plus an AppIcon.icns
# fallback; without it (no Xcode), the flattened out/icon-flat.png becomes AppIcon.icns alone.
# Usage: dm/brand/icon.sh <Resources dir>
set -eu
B=$(cd "$(dirname "$0")" && pwd); RES=$1; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if xcrun --find actool > /dev/null 2>&1 && xcrun actool "$B/AppIcon.icon" --compile "$T" --platform macosx --minimum-deployment-target 27.0 \
     --app-icon AppIcon --output-partial-info-plist "$T/partial.plist" > "$T/actool.log" 2>&1 && [ -f "$T/Assets.car" ] && [ -f "$T/AppIcon.icns" ]; then
  cp "$T/Assets.car" "$T/AppIcon.icns" "$RES/"
  echo "<key>CFBundleIconFile</key><string>AppIcon</string><key>CFBundleIconName</key><string>AppIcon</string>"
else
  mkdir "$T/AppIcon.iconset"
  for s in 16 32 128 256 512; do
    sips -s format png -z $s $s "$B/out/icon-flat.png" --out "$T/AppIcon.iconset/icon_${s}x${s}.png" > /dev/null
    sips -s format png -z $((s * 2)) $((s * 2)) "$B/out/icon-flat.png" --out "$T/AppIcon.iconset/icon_${s}x${s}@2x.png" > /dev/null
  done
  iconutil -c icns "$T/AppIcon.iconset" -o "$RES/AppIcon.icns"
  echo "<key>CFBundleIconFile</key><string>AppIcon</string>"
fi
