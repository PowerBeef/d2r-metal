#!/bin/bash
# Builds a launcher release on this Mac (GitHub has no macOS 27 runners; docs/LAUNCHER.md): the tools pack, the runtime pack and the signed, notarized
# D2R Metal Launcher, then a draft GitHub release holding them. Publishing the draft stays the maintainer's step (on github.com, or
# gh release edit v<version> --draft=false).
# Usage: release_mac.sh <version> [--unsigned] [--no-upload] [--notes <file>] [--out <dir>]
#   <version>     e.g. 1.0.0 (tag v<version>). Built from the committed HEAD in a temporary worktree: uncommitted changes and the three files ./setup.sh
#                 rewrites never enter a release. The build number is the commit count (monotonic, what update checks compare).
#   --unsigned    a local test release: ad-hoc signatures, no notarization, no team in the manifest (the launcher then checks the checksums only);
#                 DM_LAUNCHER_FEED=file://<out>/manifest.json points a launcher at it. Implies --no-upload
#   --no-upload   build everything into <out> and stop before GitHub
# Needs: Xcode 27 (clang, make, swift, codesign, notarytool, stapler), .NET 10, Homebrew spirv-cross at the pinned version, git, gh, and the network
# (the Python build, the pip wheels, hactool's source). Signing uses the keychain's "Developer ID Application" identity of team FK2D8X36G2;
# notarization the notarytool keychain profile DM_NOTARY_PROFILE (default d2r-notary), made once with:
#   xcrun notarytool store-credentials d2r-notary --apple-id <apple id> --team-id FK2D8X36G2   (it asks for an app-specific password)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); DM=$HERE/..; REPO=$(cd "$DM/.." && pwd); ROOT=${DM_ROOT:-$HOME/dm-work}
TEAM=FK2D8X36G2; GH_REPO=PowerBeef/d2r-metal; PROFILE=${DM_NOTARY_PROFILE:-d2r-notary}
VERSION=; UNSIGNED=; UPLOAD=1; NOTES=; OUT=
while [ $# -gt 0 ]; do case $1 in
  --unsigned) UNSIGNED=1; UPLOAD=;; --no-upload) UPLOAD=;; --notes) NOTES=$2; shift;; --out) OUT=$2; shift;;
  -*) echo "unknown option $1" >&2; exit 2;; *) VERSION=$1;; esac; shift; done
[ -n "$VERSION" ] || { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
OUT=${OUT:-$ROOT/release/$VERSION}; CACHE=$ROOT/release/cache
say() { echo "[$(date +%T)] $*"; }
die() { echo "🔴 $*" >&2; exit 1; }
pin() { awk -F'\t' -v k="$1" '$1 == k { print $2 }' "$HERE/release_pins.txt"; }
sha() { shasum -a 256 "$1" | cut -c1-64; }
size() { stat -f %z "$1"; }

ID=
if [ -z "$UNSIGNED" ]; then
  ID=$(security find-identity -v -p codesigning | sed -n "s/.*\"\(Developer ID Application: .*($TEAM)\)\".*/\1/p" | head -1)
  [ -n "$ID" ] || die "no Developer ID Application identity of team $TEAM in the keychain"
  xcrun notarytool history --keychain-profile "$PROFILE" > /dev/null 2>&1 || die "no notarytool profile $PROFILE (see the top of this file)"
fi
sign() {   # sign <file>… : Developer ID with a secure timestamp, or ad-hoc in a test release (codesign's "replacing existing signature" notes are dropped)
  local err; err=$(mktemp)
  if [ -n "$ID" ]; then codesign --force --timestamp -s "$ID" "$@" > /dev/null 2> "$err"; else codesign --force -s - "$@" > /dev/null 2> "$err"; fi \
    || { grep -v 'replacing existing signature' "$err" >&2; rm -f "$err"; return 1; }
  rm -f "$err"
}

# ---- a clean tree at HEAD ----
COMMIT=$(git -C "$REPO" rev-parse --short HEAD); BUILD=$(git -C "$REPO" rev-list --count HEAD)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/d2r-release.XXXXXX"); WT=$WORK/src
cleanup() { git -C "$REPO" worktree remove --force "$WT" > /dev/null 2>&1 || true; rm -rf "$WORK"; }
trap cleanup EXIT
git -C "$REPO" worktree add --detach "$WT" HEAD > /dev/null
mkdir -p "$OUT" "$CACHE"
say "release $VERSION build $BUILD from $COMMIT → $OUT${UNSIGNED:+ (unsigned test release)}"

# ---- tools pack (versioned apart; rebuilt only for a new tools_version) ----
TV=$(pin tools_version); TOOLS_NAME=d2r-metal-tools-$TV.tar.gz; TC=$CACHE/tools-$TV${UNSIGNED:+-unsigned}; T=$TC/pack
if [ ! -f "$TC/$TOOLS_NAME" ]; then
  say "tools pack $TV"
  rm -rf "$TC"; mkdir -p "$T/bin" "$T/licenses" "$WORK/dl"
  curl -fsSL "$(pin python_url)" -o "$WORK/dl/python.tgz"
  [ "$(sha "$WORK/dl/python.tgz")" = "$(pin python_sha256)" ] || die "the Python build does not match its pinned checksum"
  tar -xzf "$WORK/dl/python.tgz" -C "$T"
  # shellcheck disable=SC2046
  "$T/python/bin/python3" -m pip install -q --no-cache-dir --disable-pip-version-check --root-user-action=ignore --only-binary :all: $(pin pip)
  "$T/python/bin/python3" -c 'import lz4, capstone, numpy, PIL; assert capstone.__version__.startswith("5")' || die "the pinned packages do not import"
  cp "$T/python/lib/python3.14/LICENSE.txt" "$T/licenses/Python.txt" 2>/dev/null || true
  dotnet publish "$WT/dm/gfx/tools/dmsh_xlate" -c Release -r osx-arm64 --self-contained true -o "$T/dmsh_xlate" -v quiet > /dev/null
  "$T/dmsh_xlate/dmsh_xlate" --source-sha > /dev/null || die "the self-contained translator does not start"
  cp "$WT/dm/third_party/ryujinx/LICENSE.txt" "$T/licenses/Ryujinx.txt"
  DR=$(dirname "$(dirname "$(readlink -f "$(command -v dotnet)")")")/libexec; [ -d "$DR" ] || DR=${DOTNET_ROOT:-}
  for f in LICENSE.txt ThirdPartyNotices.txt; do [ ! -f "$DR/$f" ] || cp "$DR/$f" "$T/licenses/dotnet-$f"; done
  [ "$(brew list --versions spirv-cross | awk '{print $2}')" = "$(pin spirv_cross_version)" ] || die "Homebrew spirv-cross is not $(pin spirv_cross_version)"
  cp -L "$(brew --prefix)/bin/spirv-cross" "$T/bin/spirv-cross"; cp "$(brew --prefix spirv-cross)/LICENSE" "$T/licenses/SPIRV-Cross.txt" 2>/dev/null || true
  otool -L "$T/bin/spirv-cross" | tail -n +2 | grep -v -E '^\s+/usr/lib/|/System/' && die "spirv-cross links libraries outside the system"
  git clone -q https://github.com/SciresM/hactool.git "$WORK/hactool" && git -C "$WORK/hactool" checkout -q "$(pin hactool_commit)"
  (cd "$WORK/hactool" && cp config.mk.template config.mk && make -s > "$WORK/hactool.log" 2>&1) || { tail -20 "$WORK/hactool.log"; die "hactool did not build"; }
  cp "$WORK/hactool/hactool" "$T/bin/hactool"; cp "$WORK/hactool/LICENSE" "$T/licenses/hactool.txt"
  # every Mach-O of the pack carries the release's signature (the launcher checks python3's)
  find "$T" -type f \( -perm -u+x -o -name '*.so' -o -name '*.dylib' \) -print0 | while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q Mach-O; then sign "$f"; fi; done
  (cd "$T" && COPYFILE_DISABLE=1 tar -czf "$TC/$TOOLS_NAME" .)
fi

# ---- runtime pack: the host with the source-form chain these tools produce, the install scripts, the deswizzle library ----
say "runtime pack"
RT=$WORK/runtime; mkdir -p "$RT/host" "$RT/lib" "$RT/app" "$RT/repo"
( cd "$WT"
  PATH="$T/bin:$PATH" XLATE="$T/dmsh_xlate/dmsh_xlate" "$T/python/bin/python3" -E -s dm/gfx/tools/dmsh_chain.py --update-lock --runtime > /dev/null
  "$T/python/bin/python3" -E -s dm/gfx/tools/dmsh_chain.py --header > /dev/null
  make -s -C dm/host dmhost ROMFS=/nonexistent > /dev/null 2>&1 || die "the host did not build (make -C dm/host dmhost)"
  clang -O2 -shared -fPIC -Idm/gfx/nvn -Idm/gfx/tex -mmacosx-version-min=27.0 -o "$RT/lib/libnvn_deswizzle.dylib" dm/gfx/tex/nvn_deswizzle.c dm/gfx/nvn/nvn_tex.c )
CHAIN=$("$T/python/bin/python3" -E -s "$WT/dm/gfx/tools/dmsh_chain.py")
cp "$WT/dm/host/dmhost" "$RT/host/D2RMobile"
[ "$(lipo -archs "$RT/host/D2RMobile")" = arm64 ] || die "the host is not arm64 only"
grep -q -F "$CHAIN" "$RT/host/D2RMobile" || die "the host does not carry chain $CHAIN"   # not strings | grep -q: with pipefail, grep's early exit fails the pipe
# the scripts the install steps run, from the tracked files only (dm/ios/tools/overlay_contract.py lists the overlay's inputs; they must all be here)
( cd "$WT" && git ls-files -z dm/tools dm/gfx/tools dm/gfx/tex dm/ios/tools dm/ios/Sources dm/ios/NativeHUD dm/ios/docs dm/i18n dm/data \
    dm/PRODUCT dm/switches.tsv dm/verify/nsoimg.py 'dm/gfx/nvn/*.tsv' 'dm/gfx/nvn/*.h' dm/gfx/nvn/nvn_tex.c \
  | grep -z -v -E '^dm/tools/(re|fuzz|lldb|git-hooks)/|/test_[^/]*$|^dm/gfx/tools/dmsh_xlate/(bin|obj)/' | rsync -a --from0 --files-from=- . "$RT/repo/" )
cp "$WT/dm/gfx/tools/dmsh_chain.lock" "$RT/repo/dm/gfx/tools/"; cp "$WT/dm/gfx/nvn/dmsh_chain.h" "$RT/repo/dm/gfx/nvn/"
"$T/python/bin/python3" -E -s -c 'import os, sys; sys.path.insert(0, sys.argv[1] + "/dm/ios/tools"); import overlay_contract as o
miss = [f for f in o.SOURCE_INPUTS if not os.path.exists(os.path.join(sys.argv[1], f))]
sys.exit("overlay inputs missing from the runtime pack: " + ", ".join(miss) if miss else 0)' "$RT/repo"
[ "$("$T/python/bin/python3" -E -s "$RT/repo/dm/gfx/tools/dmsh_chain.py")" = "$CHAIN" ] || die "the pack's chain differs from the host's"
ICON=$DM/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png; IS=$WORK/AppIcon.iconset; mkdir "$IS"
for s in 16 32 128 256 512; do sips -s format png -z $s $s "$ICON" --out "$IS/icon_${s}x${s}.png" > /dev/null; sips -s format png -z $((s*2)) $((s*2)) "$ICON" --out "$IS/icon_${s}x${s}@2x.png" > /dev/null; done
iconutil -c icns "$IS" -o "$RT/app/AppIcon.icns"
sign "$RT/host/D2RMobile" "$RT/lib/libnvn_deswizzle.dylib"
RUNTIME_NAME=d2r-metal-runtime-$VERSION.tar.gz; LAUNCHER_ZIP=D2R-Metal-Launcher-$VERSION.zip; DMG=D2R-Metal-Launcher-$VERSION.dmg
UPDATE_URL=https://github.com/$GH_REPO/releases/latest/download/latest_mac.json
write_manifest() {   # write_manifest <file> <with assets: 0|1>
  "$T/python/bin/python3" -E -s - "$1" "$2" <<PY
import json, os, sys
out, full = sys.argv[1], sys.argv[2] == '1'
def asset(name, path, version, url=None):
    import hashlib; h = hashlib.sha256(open(path, 'rb').read()).hexdigest() if full else '0'
    a = {'name': name, 'version': version, 'sha256': h, 'size': os.path.getsize(path) if full else 0}
    if url: a['url'] = url
    return a
notes = open('$NOTES').read() if '$NOTES' else 'D2R Metal $VERSION'
m = {'schema': 1, 'version': '$VERSION', 'build': $BUILD, 'commit': '$COMMIT', 'date': '$(date +%F)', 'notes': notes, 'macos_min': '27.0',
     'update_url': '$UPDATE_URL', 'chain': '$CHAIN', 'product': '$(head -1 "$WT/dm/PRODUCT")'}
if not '$UNSIGNED': m['team_id'] = '$TEAM'
if full:
    tools_url = open('$TC/url.txt').read().strip() if os.path.exists('$TC/url.txt') else None
    m['runtime'] = asset('$RUNTIME_NAME', '$OUT/$RUNTIME_NAME', '$VERSION')
    m['tools'] = asset('$TOOLS_NAME', '$OUT/$TOOLS_NAME', '$TV', tools_url)
    m['launcher'] = dict(asset('$LAUNCHER_ZIP', '$OUT/$LAUNCHER_ZIP', '$VERSION'), build=$BUILD, min_build=0)
else:
    m['runtime'] = {'name': '$RUNTIME_NAME', 'version': '$VERSION', 'sha256': '0', 'size': 0}
    m['tools'] = {'name': '$TOOLS_NAME', 'version': '$TV', 'sha256': '0', 'size': 0}
json.dump(m, open(out, 'w'), indent=1, ensure_ascii=False)
PY
}
write_manifest "$RT/manifest.json" 0   # the pack's own copy: what install.py reads (version, build, update URL)
(cd "$RT" && COPYFILE_DISABLE=1 tar -czf "$OUT/$RUNTIME_NAME" .)
cp "$TC/$TOOLS_NAME" "$OUT/$TOOLS_NAME"

# ---- the launcher: signed with the hardened runtime, notarized, stapled; a zip (self-update) and a DMG (download) ----
say "launcher"
DM_LAUNCHER_VERSION=$VERSION DM_LAUNCHER_BUILD=$BUILD DM_LAUNCHER_SIGN=$ID DM_ROOT=$ROOT bash "$WT/dm/tools/launcher_app.sh" "$WORK/launcher" > /dev/null
APP="$WORK/launcher/D2R Metal Launcher.app"
notarize() { xcrun notarytool submit "$1" --keychain-profile "$PROFILE" --wait | tail -3; }
if [ -n "$ID" ]; then
  ditto -c -k --keepParent "$APP" "$WORK/notarize.zip"; say "notarizing the app"; notarize "$WORK/notarize.zip"; xcrun stapler staple "$APP" > /dev/null
fi
ditto -c -k --keepParent "$APP" "$OUT/$LAUNCHER_ZIP"
mkdir "$WORK/dmg"; cp -R "$APP" "$WORK/dmg/"; ln -s /Applications "$WORK/dmg/Applications"
hdiutil create -quiet -volname "D2R Metal Launcher" -srcfolder "$WORK/dmg" -ov -format UDZO "$OUT/$DMG"
if [ -n "$ID" ]; then
  sign "$OUT/$DMG"; say "notarizing the disk image"; notarize "$OUT/$DMG"; xcrun stapler staple "$OUT/$DMG" > /dev/null
  spctl -a -t open --context context:primary-signature -v "$OUT/$DMG" 2>&1 | tail -1
fi

# ---- the release's description and the in-game notice ----
write_manifest "$OUT/manifest.json" 1
printf '{"build": %s, "note": "D2R Metal %s", "note_en": "D2R Metal %s", "url": "d2rmetal://update"}\n' "$BUILD" "$VERSION" "$VERSION" > "$OUT/latest_mac.json"
say "built: $(cd "$OUT" && ls | tr '\n' ' ')"
[ -n "$UPLOAD" ] || { say "not uploaded (--no-upload / --unsigned)"; exit 0; }

gh release view "v$VERSION" --repo "$GH_REPO" > /dev/null 2>&1 && die "release v$VERSION already exists on GitHub"
ASSETS=("$OUT/$DMG" "$OUT/$LAUNCHER_ZIP" "$OUT/$RUNTIME_NAME" "$OUT/manifest.json" "$OUT/latest_mac.json")
[ -f "$TC/url.txt" ] || ASSETS+=("$OUT/$TOOLS_NAME")   # a tools pack an earlier release carries is not uploaded again (the manifest points there)
gh release create "v$VERSION" --repo "$GH_REPO" --draft --target "$(git -C "$REPO" rev-parse HEAD)" --title "D2R Metal $VERSION" \
  --notes-file "${NOTES:-/dev/null}" "${ASSETS[@]}"
[ -f "$TC/url.txt" ] || echo "https://github.com/$GH_REPO/releases/download/v$VERSION/$TOOLS_NAME" > "$TC/url.txt"   # later releases point at this copy
say "draft release v$VERSION created: review it on github.com and publish it there"
