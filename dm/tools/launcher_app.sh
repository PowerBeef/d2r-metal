#!/bin/bash
# Builds D2R Metal Launcher.app from dm/launcher (docs/LAUNCHER.md): swift build, then the app bundle.
# Usage: launcher_app.sh [output dir]   (default $DM_ROOT/out/launcher)  → <output dir>/D2R Metal Launcher.app
# Signing: ad-hoc by default (a local test build). DM_LAUNCHER_SIGN="Developer ID Application: <name> (<team>)" signs it for distribution, with the
# hardened runtime and a secure timestamp (release_mac.sh then notarizes and staples it). The launcher holds no game content and runs nothing from the
# game: it downloads the packs and runs dm/tools/install.py with the tools pack's Python.
# Version: DM_LAUNCHER_VERSION (default 1.0), build number DM_LAUNCHER_BUILD (default: the commit count, monotonic like the release builds).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); DM=$HERE/..; ROOT=${DM_ROOT:-$HOME/dm-work}
OUT=${1:-$ROOT/out/launcher}; NAME="D2R Metal Launcher"; BID=${DM_LAUNCHER_BUNDLE_ID:-org.d2rmetal.launcher}
VER=${DM_LAUNCHER_VERSION:-1.0}; BUILD=${DM_LAUNCHER_BUILD:-$(git -C "$DM" rev-list --count HEAD)}
swift build -c release --package-path "$DM/launcher" > /dev/null || { echo "🔴 swift build failed (swift build -c release --package-path dm/launcher)"; exit 2; }
BIN="$(swift build -c release --package-path "$DM/launcher" --show-bin-path)/D2RMetalLauncher"
STG="$OUT/.launcher.new"; APP="$STG/$NAME.app"
trap 'rm -rf "$STG"' EXIT
rm -rf "$STG"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/D2RMetalLauncher"
ICON_KEY=$("$DM/brand/icon.sh" "$APP/Contents/Resources")   # the project's icon (dm/brand), as the game app's
# the brand art the window draws (Brand.swift reads Resources/Brand laid out as dm/brand): the emblem at the size it is shown, the wordmark, Cinzel
BR="$APP/Contents/Resources/Brand"; mkdir -p "$BR/out" "$BR/fonts"
sips -Z 512 "$DM/brand/out/emblem.png" --out "$BR/out/emblem.png" > /dev/null; cp "$DM/brand/out/wordmark.png" "$BR/out/"
cp "$DM/brand/fonts/Cinzel-wght.ttf" "$DM/brand/fonts/OFL.txt" "$BR/fonts/"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>$NAME</string>
<key>CFBundleDisplayName</key><string>$NAME</string>
<key>CFBundleIdentifier</key><string>$BID</string>
<key>CFBundleExecutable</key><string>D2RMetalLauncher</string>
${ICON_KEY}
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VER</string>
<key>CFBundleVersion</key><string>$BUILD</string>
<key>LSMinimumSystemVersion</key><string>27.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
<key>CFBundleURLTypes</key><array><dict>
  <key>CFBundleURLName</key><string>$BID</string>
  <key>CFBundleURLSchemes</key><array><string>d2rmetal</string></array>
</dict></array>
</dict></plist>
PL
if [ -n "${DM_LAUNCHER_SIGN:-}" ]; then
  codesign --force --options runtime --timestamp -s "$DM_LAUNCHER_SIGN" "$APP" || { echo "🔴 Developer ID signing failed"; exit 3; }
else
  codesign --force -s - "$APP" > /dev/null 2>&1 || { echo "🔴 ad-hoc signing failed"; exit 3; }
fi
codesign --verify --strict "$APP"
mkdir -p "$OUT"; rm -rf "${OUT:?}/$NAME.app"; mv "$APP" "$OUT/"
echo "LAUNCHER $OUT/$NAME.app (version $VER build $BUILD, $( [ -n "${DM_LAUNCHER_SIGN:-}" ] && echo 'Developer ID' || echo ad-hoc ), $(du -sh "$OUT/$NAME.app" | cut -f1))"
