#!/bin/bash
# Builds D2R Metal Launcher.app from dm/launcher (docs/LAUNCHER.md): swift build, then the app bundle.
# Usage: launcher_app.sh [output dir]   (default $DM_ROOT/out/launcher)  → <output dir>/D2R Metal Launcher.app
# Signing: ad-hoc by default (a local test build). DM_LAUNCHER_SIGN="Developer ID Application: <name> (<team>)" signs it for distribution, with the
# hardened runtime and a secure timestamp (release_mac.sh then notarizes and staples it). The launcher holds no game content and runs nothing from the
# game: it downloads the packs and runs dm/tools/install.py with the tools pack's Python.
# Version: DM_LAUNCHER_VERSION (default 1.0), build number DM_LAUNCHER_BUILD (release_mac.sh passes the commit count). The default build is 0: a
# development build never looks newer than a release (LaunchServices would prefer it for the launcher's id and its d2rmetal:// link) and never
# replaces itself with one. DM_LAUNCHER_APP_ID gives a test build another bundle id (not DM_LAUNCHER_BUNDLE_ID, which the running launcher reads as
# the game app's id).
# Errors go to stderr, so a caller that silences the output still shows them.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); DM=$HERE/..; ROOT=${DM_ROOT:-$HOME/dm-work}
OUT=${1:-$ROOT/out/launcher}; NAME="D2R Metal Launcher"; BID=${DM_LAUNCHER_APP_ID:-org.d2rmetal.launcher}
VER=${DM_LAUNCHER_VERSION:-1.0}; BUILD=${DM_LAUNCHER_BUILD:-0}
die() { echo "🔴 $*" >&2; exit "${2:-1}"; }
swift build -c release --package-path "$DM/launcher" > /dev/null || die "swift build failed (swift build -c release --package-path dm/launcher)" 2
BIN="$(swift build -c release --package-path "$DM/launcher" --show-bin-path)/D2RMetalLauncher"
STG="$OUT/.launcher.new"; APP="$STG/$NAME.app"
trap 'rm -rf "$STG"' EXIT
rm -rf "$STG"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/D2RMetalLauncher"
ICON_KEY=$("$DM/brand/icon.sh" "$APP/Contents/Resources")   # the project's icon (dm/brand), as the game app's
# the project's license and the third-party notices (docs/THIRD_PARTY_NOTICES.md: what each component is and where its text is)
LIC="$APP/Contents/Resources/Licenses"; mkdir -p "$LIC"; cp "$DM/../LICENSE" "$DM/../docs/THIRD_PARTY_NOTICES.md" "$LIC/"
for f in "$LIC/LICENSE" "$LIC/THIRD_PARTY_NOTICES.md"; do [ -s "$f" ] || die "a license text is missing: $f"; done
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
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
<key>NSHumanReadableCopyright</key><string>D2R Metal, MIT License. Diablo II: Resurrected is not included: it is built from your own copy.</string>
<key>CFBundleURLTypes</key><array><dict>
  <key>CFBundleURLName</key><string>$BID</string>
  <key>CFBundleURLSchemes</key><array><string>d2rmetal</string></array>
</dict></array>
</dict></plist>
PL
if [ -n "${DM_LAUNCHER_SIGN:-}" ]; then
  codesign --force --options runtime --timestamp -s "$DM_LAUNCHER_SIGN" "$APP" || die "Developer ID signing failed" 3
else
  codesign --force -s - "$APP" > /dev/null 2>&1 || die "ad-hoc signing failed" 3
fi
codesign --verify --strict "$APP" || die "the signature does not verify" 3
mkdir -p "$OUT"; rm -rf "${OUT:?}/$NAME.app"; mv "$APP" "$OUT/"
echo "LAUNCHER $OUT/$NAME.app (version $VER build $BUILD, $( [ -n "${DM_LAUNCHER_SIGN:-}" ] && echo 'Developer ID' || echo ad-hoc ), $(du -sh "$OUT/$NAME.app" | cut -f1))"
