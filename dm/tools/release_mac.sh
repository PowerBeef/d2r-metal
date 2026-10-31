#!/bin/bash
# Builds a launcher release on this Mac (GitHub has no macOS 27 runners; docs/LAUNCHER.md): the tools pack, the runtime pack and the signed, notarized
# D2R Metal Launcher, then a draft GitHub release holding them with the signed manifest. Publishing the draft stays the maintainer's step (on github.com,
# or gh release edit v<version> --draft=false), followed by release_mac.sh --check-latest.
# Usage: release_mac.sh <version> [--unsigned] [--no-upload] [--notes <file>] [--out <dir>] [--min-build <n>] [--replace-draft] [--update-nuget-pins]
#        release_mac.sh --check-latest
#   <version>     e.g. 1.0.0 (tag v<version>). Built from the committed HEAD in a temporary worktree: uncommitted changes and the three files ./setup.sh
#                 rewrites never enter a release. The build number is the commit count (monotonic, what update checks compare); an upload needs HEAD
#                 on origin/main and a build above every published release's.
#   --unsigned    a local test release: ad-hoc signatures, no notarization, no team in the manifest (a test launcher then checks the checksums, and
#                 the manifest signature when the release key is in this keychain); DM_LAUNCHER_FEED=file://<out>/manifest.json points a launcher
#                 at it. Implies --no-upload
#   --no-upload   build everything into <out> and stop before GitHub (a signed dry run still notarizes: it uploads the app to Apple's notary service)
#   --min-build   the oldest launcher build that may install this release (default: LAUNCHER_MIN_BUILD in dm/tools/install.py)
#   --replace-draft  delete a draft v<version> left by an earlier run (never a published release) before uploading
#   --update-nuget-pins  record the NuGet packages of this build in dm/tools/release_nuget.tsv instead of refusing a change
#   --check-latest   after publishing: GitHub's "latest release" (the stable channel and the in-game notice read it) must be a D2R Metal release
# Needs: Xcode 27 (clang, make, swift, codesign, notarytool, stapler, actool), Homebrew spirv-cross (the pinned version and checksum) and zstd, git,
# gh, and the network (the pinned Python build, .NET SDK, wheels and NuGet packages, hactool's source). Signing uses the keychain's "Developer ID
# Application" identity of team FK2D8X36G2; the manifest signature the release key in the login keychain (dm/tools/manifest_sign.swift, made once
# with its keygen); notarization the notarytool keychain profile DM_NOTARY_PROFILE (default d2r-notary), made once with:
#   xcrun notarytool store-credentials d2r-notary --apple-id <apple id> --team-id FK2D8X36G2   (it asks for an app-specific password)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); DM=$HERE/..; REPO=$(cd "$DM/.." && pwd); ROOT=${DM_ROOT:-$HOME/dm-work}
TEAM=FK2D8X36G2; GH_REPO=PowerBeef/d2r-metal; PROFILE=${DM_NOTARY_PROFILE:-d2r-notary}; LAUNCHER_ID=org.d2rmetal.launcher
VERSION=; UNSIGNED=; UPLOAD=1; NOTES=; OUT=; MIN_BUILD=; REPLACE=; NUGET_UPDATE=; CHECK_LATEST=
while [ $# -gt 0 ]; do case $1 in
  --unsigned) UNSIGNED=1; UPLOAD=;; --no-upload) UPLOAD=;; --notes) NOTES=$2; shift;; --out) OUT=$2; shift;; --min-build) MIN_BUILD=$2; shift;;
  --replace-draft) REPLACE=1;; --update-nuget-pins) NUGET_UPDATE=1;; --check-latest) CHECK_LATEST=1;;
  -*) echo "unknown option $1" >&2; exit 2;; *) VERSION=$1;; esac; shift; done
say() { echo "[$(date +%T)] $*"; }
die() { echo "🔴 $*" >&2; exit 1; }
pin() { awk -F'\t' -v k="$1" '$1 == k { print $2 }' "$HERE/release_pins.txt"; }
sha() { shasum -a 256 "$1" | cut -c1-64; }
size() { stat -f %z "$1"; }

# GitHub's latest release is what the stable channel and the in-game notice read: it must be one of ours (v<version>, with a manifest). A release of
# README images or anything else must be a pre-release, or every launcher's stable channel would find no manifest there
check_latest() {
  local tag; tag=$(gh api "repos/$GH_REPO/releases/latest" --jq .tag_name 2>/dev/null) || die "GitHub has no latest release yet: the stable channel finds nothing"
  [[ $tag == v* ]] || die "GitHub's latest release is $tag, not a D2R Metal release: mark it as a pre-release (gh release edit $tag --repo $GH_REPO --prerelease)"
  gh api "repos/$GH_REPO/releases/latest" --jq '.assets[].name' | grep -qx manifest.json || die "the latest release ($tag) has no manifest.json"
  gh api "repos/$GH_REPO/releases/latest" --jq '.assets[].name' | grep -qx manifest.json.sig || die "the latest release ($tag) has no manifest.json.sig"
  say "GitHub's latest release is $tag, with a signed manifest"
}
[ -z "$CHECK_LATEST" ] || { check_latest; exit 0; }
[ -n "$VERSION" ] || { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || die "the version must look like 1.2.3 (it names files and the launcher's folders)"
OUT=${OUT:-$ROOT/release/$VERSION}; CACHE=$ROOT/release/cache

# Every Mach-O in a pack loads only macOS's own libraries or the pack's own files (@rpath / @loader_path resolved; dm/tools/macho_deps.py): players
# install nothing (no Homebrew, Xcode or runtimes), so a library from the build Mac would fail on theirs. Prints the offenders
self_contained() { /usr/bin/python3 -I "$HERE/macho_deps.py" "$1"; }
need_file() { [ -s "$1" ] || die "a license text is missing: $1"; }   # a pack never ships without the license texts of what it holds

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
SIGNKEY=1   # the manifest signature: required for a release, made for a test release whenever the key is here
swift "$HERE/manifest_sign.swift" public > /dev/null 2>&1 || { [ -n "$UNSIGNED" ] || die "no release key in the keychain (swift dm/tools/manifest_sign.swift keygen, once)"; SIGNKEY=; }

# ---- a clean tree at HEAD ----
COMMIT=$(git -C "$REPO" rev-parse --short HEAD); BUILD=$(git -C "$REPO" rev-list --count HEAD)
if [ -n "$UPLOAD" ]; then
  git -C "$REPO" fetch -q origin main || die "cannot fetch origin/main"
  git -C "$REPO" merge-base --is-ancestor HEAD origin/main || die "HEAD ($COMMIT) is not on origin/main: push it first (a release is built from the public trunk)"
  PUBLISHED=$(gh api "repos/$GH_REPO/releases?per_page=30" --jq '[.[] | select(.draft | not) | select(.tag_name | startswith("v")) | .tag_name] | .[]' | while read -r t; do
    curl -fsSL "https://github.com/$GH_REPO/releases/download/$t/manifest.json" 2>/dev/null | /usr/bin/python3 -I -c 'import json, sys; print(json.load(sys.stdin)["build"])' 2>/dev/null || true
  done | sort -n | tail -1)
  [ "$BUILD" -gt "${PUBLISHED:-0}" ] || die "build $BUILD is not above the newest published release's ($PUBLISHED): updates would not see it"
  # a release of anything else marked "latest" would hide this one from the stable channel
  OTHER=$(gh api "repos/$GH_REPO/releases?per_page=30" --jq '.[] | select(.draft | not) | select(.prerelease | not) | select(.tag_name | startswith("v") | not) | .tag_name')
  [ -z "$OTHER" ] || die "these releases are not D2R Metal releases and not pre-releases, so GitHub may call one of them latest: $OTHER (gh release edit <tag> --prerelease)"
fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/d2r-release.XXXXXX"); WT=$WORK/src
cleanup() { git -C "$REPO" worktree remove --force "$WT" > /dev/null 2>&1 || true; rm -rf "$WORK"; }
trap cleanup EXIT
git -C "$REPO" worktree add --detach "$WT" HEAD > /dev/null
mkdir -p "$OUT" "$CACHE"
MIN_BUILD=${MIN_BUILD:-$(sed -n 's/^LAUNCHER_MIN_BUILD = \([0-9][0-9]*\).*/\1/p' "$WT/dm/tools/install.py")}
[[ $MIN_BUILD =~ ^[0-9]+$ ]] || die "no minimum launcher build (--min-build, or LAUNCHER_MIN_BUILD in dm/tools/install.py)"
say "release $VERSION build $BUILD from $COMMIT → $OUT${UNSIGNED:+ (unsigned test release)}; launchers from build $MIN_BUILD"

# ---- tools pack (versioned apart; rebuilt only for a new tools_version) ----
# Its cache is keyed by everything that goes into it: the pins, the wheel and NuGet pins, and the translator's sources. A different key under a
# tools_version already built means an input changed without a version bump, which would give players two different packs of one version
TV=$(pin tools_version); TOOLS_NAME=d2r-metal-tools-$TV.tar.gz; TC=$CACHE/tools-$TV${UNSIGNED:+-unsigned}; T=$TC/pack
KEY=$({ cat "$HERE/release_pins.txt" "$HERE/release_requirements.txt" "$HERE/release_nuget.tsv"; git -C "$WT" rev-parse HEAD:dm/gfx/tools/dmsh_xlate HEAD:dm/third_party/ryujinx; } \
     | shasum -a 256 | cut -c1-16)
if [ -f "$TC/key" ] && [ "$(cat "$TC/key")" != "$KEY" ]; then
  die "the tools pack's inputs changed since tools pack $TV was built: bump tools_version in dm/tools/release_pins.txt"
fi
if [ ! -f "$TC/$TOOLS_NAME" ] || [ "$(sha "$TC/$TOOLS_NAME")" != "$(cat "$TC/$TOOLS_NAME.sha256" 2>/dev/null)" ]; then
  say "tools pack $TV"
  rm -rf "$TC"; mkdir -p "$T/bin" "$T/licenses/python" "$T/licenses/python-packages" "$WORK/dl"
  curl -fsSL "$(pin python_url)" -o "$WORK/dl/python.tgz"
  [ "$(sha "$WORK/dl/python.tgz")" = "$(pin python_sha256)" ] || die "the Python build does not match its pinned checksum"
  tar -xzf "$WORK/dl/python.tgz" -C "$T"
  "$T/python/bin/python3" -m pip install -q --no-cache-dir --disable-pip-version-check --root-user-action=ignore --only-binary :all: --no-deps \
    --require-hashes -r "$HERE/release_requirements.txt" || die "the pinned Python packages did not install (a wheel whose hash is not pinned?)"
  "$T/python/bin/python3" -c 'import lz4, capstone, numpy, PIL; assert capstone.__version__.startswith("5")' || die "the pinned packages do not import"
  # the license texts: CPython's and those of the libraries built into it (from the same build's full archive), python-build-standalone's own,
  # and each wheel's (Pillow's and NumPy's list the libraries their wheels bundle)
  curl -fsSL "$(pin python_licenses_url)" -o "$WORK/dl/python-full.tar.zst"
  [ "$(sha "$WORK/dl/python-full.tar.zst")" = "$(pin python_licenses_sha256)" ] || die "the Python full archive does not match its pinned checksum"
  command -v zstd > /dev/null || die "zstd is needed to read the Python build's license texts (brew install zstd)"
  zstd -dcq "$WORK/dl/python-full.tar.zst" | tar -x -C "$WORK/dl" python/licenses
  cp "$WORK/dl/python/licenses/"*.txt "$T/licenses/python/"; need_file "$T/licenses/python/LICENSE.cpython.txt"; need_file "$T/licenses/python/LICENSE.openssl-3.txt"
  curl -fsSL "$(pin python_build_license_url)" -o "$T/licenses/python-build-standalone.txt"
  [ "$(sha "$T/licenses/python-build-standalone.txt")" = "$(pin python_build_license_sha256)" ] || die "python-build-standalone's license text does not match its pin"
  for d in "$T/python/lib/python3.14/site-packages/"*.dist-info; do
    n=$(basename "$d" | sed 's/-[0-9].*//' | tr 'A-Z' 'a-z'); [ "$n" != pip ] || continue
    mkdir -p "$T/licenses/python-packages/$n"
    find "$d" -maxdepth 2 -type f \( -iname 'LICEN[CS]E*' -o -path '*/licenses/*' \) -exec cp {} "$T/licenses/python-packages/$n/" \;
    [ -n "$(ls "$T/licenses/python-packages/$n")" ] || die "the $n wheel carries no license text"
  done
  # what players never run is left out: pip, IDLE, pydoc, Tk and Tcl, and the console scripts pip wrote (their first line names this Mac's
  # build folder)
  P=$T/python; rm -rf "$P/lib/python3.14/site-packages/pip" "$P/lib/python3.14/site-packages/pip-"*.dist-info "$P/lib/python3.14/idlelib" \
    "$P/lib/python3.14/tkinter" "$P/lib/python3.14/turtledemo" "$P/lib/python3.14/turtle.py" "$P/lib/python3.14/pydoc_data" \
    "$P/lib/python3.14/lib-dynload/_tkinter"* "$P/lib/tcl"* "$P/lib/tk"* "$P/lib/itcl"* "$P/lib/thread"* "$P/lib/libtcl"* "$P/lib/libtk"* \
    "$P/bin/pip"* "$P/bin/idle"* "$P/bin/pydoc"* "$P/bin/f2py" "$P/bin/numpy-config"
  if grep -rIlF "$T" "$P/bin" > /dev/null 2>&1; then die "a script in python/bin names the build folder: $(grep -rIlF "$T" "$P/bin" | head -3)"; fi
  "$T/python/bin/python3" -c 'import lz4, capstone, numpy, PIL.Image, json, hashlib, ssl' || die "the trimmed Python no longer imports what the install steps need"
  # the shader translator, compiled ahead of time (NativeAOT) with Microsoft's own .NET SDK (pinned, cached across releases): one native
  # program that loads only macOS's libraries, so players get no .NET runtime at all; the SDK is needed only here, on the release Mac
  DN=$CACHE/dotnet-sdk-$(pin dotnet_sdk_version)
  if [ ! -x "$DN/dotnet" ]; then
    curl -fsSL "$(pin dotnet_sdk_url)" -o "$WORK/dl/dotnet.tgz"
    [ "$(shasum -a 512 "$WORK/dl/dotnet.tgz" | cut -c1-128)" = "$(pin dotnet_sdk_sha512)" ] || die "the .NET SDK does not match its pinned checksum"
    rm -rf "$DN.part"; mkdir -p "$DN.part"; tar -xzf "$WORK/dl/dotnet.tgz" -C "$DN.part"; mv "$DN.part" "$DN"
  fi
  DOTNET_ROOT=$DN DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 NUGET_PACKAGES=$CACHE/nuget \
    "$DN/dotnet" publish "$WT/dm/gfx/tools/dmsh_xlate" -c Release -r osx-arm64 -p:PublishAot=true -p:InvariantGlobalization=true -p:StripSymbols=true \
    -o "$T/dmsh_xlate" -v quiet > "$WORK/xlate.log" 2>&1 || { tail -20 "$WORK/xlate.log"; die "the shader translator did not build with the .NET SDK"; }
  # the NuGet packages it restored are the pinned ones (NuGet records each package's sha512 beside it)
  NOW=$(cd "$CACHE/nuget" && find . -name '*.nupkg.sha512' | sort | while read -r f; do printf '%s\t%s\t%s\n' "$(echo "$f" | cut -d/ -f2)" "$(echo "$f" | cut -d/ -f3)" "$(cat "$f")"; done)
  if [ -n "$NUGET_UPDATE" ]; then
    { grep '^#' "$HERE/release_nuget.tsv"; echo "$NOW"; } > "$HERE/release_nuget.tsv.new" && mv "$HERE/release_nuget.tsv.new" "$HERE/release_nuget.tsv"
    die "dm/tools/release_nuget.tsv rewritten: review it, commit it with a new tools_version, and run again"
  fi
  [ "$NOW" = "$(grep -v '^#' "$HERE/release_nuget.tsv")" ] || { diff <(grep -v '^#' "$HERE/release_nuget.tsv") <(echo "$NOW") | head; die "the NuGet packages differ from dm/tools/release_nuget.tsv (above)"; }
  # AOT warnings stop the release, but one: Ryujinx's logger lists an object's public properties by reflection (DynamicObjectFormatter,
  # IL2075), which only its log text uses; the translator logs no such object, and all 6,927 shaders translate to the same bytes as with .NET
  AOTW=$(grep -E 'warning (IL|AOT)[0-9]+' "$WORK/xlate.log" | grep -v -E 'Ryujinx\.Common/Logging/Formatters/DynamicObjectFormatter\.cs.*warning IL2075' || true)
  [ -z "$AOTW" ] || { echo "$AOTW" | head; die "the native translator has AOT warnings (above)"; }
  rm -rf "$T/dmsh_xlate"/*.dSYM; [ "$(ls "$T/dmsh_xlate")" = dmsh_xlate ] || die "the native translator is not one program: $(ls "$T/dmsh_xlate" | head -3)"
  "$T/dmsh_xlate/dmsh_xlate" --source-sha > /dev/null || die "the self-contained translator does not start"
  cp "$WT/dm/third_party/ryujinx/LICENSE.txt" "$T/licenses/Ryujinx.txt"
  for f in LICENSE.txt ThirdPartyNotices.txt; do cp "$DN/$f" "$T/licenses/dotnet-$f"; done
  [ "$(brew list --versions spirv-cross | awk '{print $2}')" = "$(pin spirv_cross_version)" ] || die "Homebrew spirv-cross is not $(pin spirv_cross_version)"
  cp -L "$(brew --prefix)/bin/spirv-cross" "$T/bin/spirv-cross"; cp "$(brew --prefix spirv-cross)/LICENSE" "$T/licenses/SPIRV-Cross.txt"
  [ "$(sha "$T/bin/spirv-cross")" = "$(pin spirv_cross_sha256)" ] || die "Homebrew's spirv-cross is not the pinned build (its sha256 differs)"
  git clone -q https://github.com/SciresM/hactool.git "$WORK/hactool" && git -C "$WORK/hactool" checkout -q "$(pin hactool_commit)"
  [ "$(git -C "$WORK/hactool" rev-parse HEAD)" = "$(pin hactool_commit)" ] || die "hactool is not at the pinned commit"
  (cd "$WORK/hactool" && cp config.mk.template config.mk && make -s > "$WORK/hactool.log" 2>&1) || { tail -20 "$WORK/hactool.log"; die "hactool did not build"; }
  cp "$WORK/hactool/hactool" "$T/bin/hactool"; cp "$WORK/hactool/LICENSE" "$T/licenses/hactool.txt"; cp "$WORK/hactool/mbedtls/LICENSE" "$T/licenses/mbedtls.txt"
  [ ! -f "$WORK/hactool/mbedtls/apache-2.0.txt" ] || cp "$WORK/hactool/mbedtls/apache-2.0.txt" "$T/licenses/mbedtls-apache-2.0.txt"
  sed -n '1,/\*\//p' "$WORK/hactool/cJSON.c" > "$T/licenses/cJSON.txt"
  cp "$WT/LICENSE" "$T/licenses/LICENSE"; cp "$WT/docs/THIRD_PARTY_NOTICES.md" "$T/licenses/"
  for f in LICENSE THIRD_PARTY_NOTICES.md Ryujinx.txt dotnet-LICENSE.txt dotnet-ThirdPartyNotices.txt SPIRV-Cross.txt hactool.txt mbedtls.txt cJSON.txt \
           python-build-standalone.txt; do need_file "$T/licenses/$f"; done
  # every Mach-O of the pack carries the release's signature (the launcher checks python3's)
  find "$T" -type f \( -perm -u+x -o -name '*.so' -o -name '*.dylib' \) -print0 | while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q Mach-O; then sign "$f"; fi; done
  self_contained "$T" || die "the tools pack loads libraries from outside macOS and itself (above)"
  # written under .part and renamed, with its checksum beside it: an interrupted build is never taken for a finished pack
  (cd "$T" && COPYFILE_DISABLE=1 tar -czf "$TC/$TOOLS_NAME.part" .) && mv "$TC/$TOOLS_NAME.part" "$TC/$TOOLS_NAME"
  sha "$TC/$TOOLS_NAME" > "$TC/$TOOLS_NAME.sha256"; echo "$KEY" > "$TC/key"
fi

# ---- runtime pack: the host with the source-form chain these tools produce, the install scripts, the deswizzle library ----
say "runtime pack"
RT=$WORK/runtime; mkdir -p "$RT/host" "$RT/lib" "$RT/app" "$RT/repo" "$RT/licenses"
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
find "$RT/repo" -name __pycache__ -prune -exec rm -rf {} +   # the checks above imported from the pack: no bytecode ships
[ "$("$T/python/bin/python3" -E -s -B "$RT/repo/dm/gfx/tools/dmsh_chain.py")" = "$CHAIN" ] || die "the pack's chain differs from the host's"
"$WT/dm/brand/icon.sh" "$RT/app" > /dev/null   # the game app's icon (Assets.car + AppIcon.icns); install.py copies it into the app it assembles
[ -f "$RT/app/Assets.car" ] || die "the app icon did not compile (dm/brand/icon.sh needs Xcode's actool)"
cp "$WT/LICENSE" "$RT/licenses/LICENSE"; cp "$WT/docs/THIRD_PARTY_NOTICES.md" "$RT/licenses/"
cp "$WT/dm/third_party/bcdec/LICENSE" "$RT/licenses/bcdec.txt"; cp "$WT/dm/third_party/libdeflate/COPYING" "$RT/licenses/libdeflate.txt"
for f in LICENSE THIRD_PARTY_NOTICES.md bcdec.txt libdeflate.txt; do need_file "$RT/licenses/$f"; done
sign "$RT/host/D2RMobile" "$RT/lib/libnvn_deswizzle.dylib"
RUNTIME_NAME=d2r-metal-runtime-$VERSION.tar.gz; LAUNCHER_ZIP=D2R-Metal-Launcher-$VERSION.zip; DMG=D2R-Metal-Launcher-$VERSION.dmg
UPDATE_URL=https://github.com/$GH_REPO/releases/latest/download/latest_mac.json

# The tools pack an earlier release published is reused (players who have it download nothing), but only when that copy is really there: the
# release published (a draft's files can't be downloaded) and the file the same as the one built here. Otherwise this release carries it again
TOOLS_URL=
if [ -f "$TC/url.txt" ]; then
  u=$(cat "$TC/url.txt")
  if curl -fsSL "$u" -o "$WORK/tools-published.tgz" 2>/dev/null && [ "$(sha "$WORK/tools-published.tgz")" = "$(sha "$TC/$TOOLS_NAME")" ]; then TOOLS_URL=$u
  else say "the tools pack at $u is not published or differs: this release carries it"; fi
  rm -f "$WORK/tools-published.tgz"
fi
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
    m['runtime'] = asset('$RUNTIME_NAME', '$OUT/$RUNTIME_NAME', '$VERSION')
    m['tools'] = asset('$TOOLS_NAME', '$OUT/$TOOLS_NAME', '$TV', '$TOOLS_URL' or None)
    m['launcher'] = dict(asset('$LAUNCHER_ZIP', '$OUT/$LAUNCHER_ZIP', '$VERSION'), build=$BUILD, min_build=$MIN_BUILD)
else:
    m['runtime'] = {'name': '$RUNTIME_NAME', 'version': '$VERSION', 'sha256': '0', 'size': 0}
    m['tools'] = {'name': '$TOOLS_NAME', 'version': '$TV', 'sha256': '0', 'size': 0}
json.dump(m, open(out, 'w'), indent=1, ensure_ascii=False)
PY
}
write_manifest "$RT/manifest.json" 0   # the pack's own copy: what install.py reads (version, build, update URL)
self_contained "$RT" || die "the runtime pack loads libraries from outside macOS and itself (above)"
(cd "$RT" && COPYFILE_DISABLE=1 tar -czf "$OUT/$RUNTIME_NAME" .)
cp "$TC/$TOOLS_NAME" "$OUT/$TOOLS_NAME"

# ---- the launcher: signed with the hardened runtime, notarized, stapled; a zip (self-update) and a DMG (download) ----
say "launcher"
# notarize <file>: waits for Apple's verdict; anything but Accepted prints the notary log and stops the release
notarize() {
  local out id status
  out=$(xcrun notarytool submit "$1" --keychain-profile "$PROFILE" --wait --output-format json 2> "$WORK/notary.err") || true
  id=$(echo "$out" | /usr/bin/python3 -I -c 'import json, sys; print(json.load(sys.stdin).get("id", ""))' 2>/dev/null || true)
  status=$(echo "$out" | /usr/bin/python3 -I -c 'import json, sys; print(json.load(sys.stdin).get("status", ""))' 2>/dev/null || true)
  if [ "$status" != Accepted ]; then
    cat "$WORK/notary.err" >&2
    [ -z "$id" ] || xcrun notarytool log "$id" --keychain-profile "$PROFILE" >&2 || true
    die "notarization of $(basename "$1") ended as ${status:-an error} (the notary log is above)"
  fi
  say "notarized $(basename "$1") (submission $id)"
}
( unset DM_LAUNCHER_APP_ID   # a development override of the launcher's bundle id never reaches a release
  DM_LAUNCHER_VERSION=$VERSION DM_LAUNCHER_BUILD=$BUILD DM_LAUNCHER_SIGN=$ID DM_ROOT=$ROOT bash "$WT/dm/tools/launcher_app.sh" "$WORK/launcher" ) > /dev/null \
  || die "the launcher did not build (dm/tools/launcher_app.sh; its error is above)"
APP="$WORK/launcher/D2R Metal Launcher.app"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist")" = "$LAUNCHER_ID" ] || die "the launcher's bundle id is not $LAUNCHER_ID"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Contents/Info.plist")" = "$BUILD" ] || die "the launcher's build number is not $BUILD"
if [ -n "$ID" ]; then
  ditto -c -k --keepParent "$APP" "$WORK/notarize.zip"; say "notarizing the app"; notarize "$WORK/notarize.zip"; xcrun stapler staple "$APP" > /dev/null
  spctl -a -t exec -vv "$APP" 2>&1 | grep -q 'source=Notarized Developer ID' || { spctl -a -t exec -vv "$APP" >&2; die "Gatekeeper does not accept the stapled launcher"; }
fi
ditto -c -k --keepParent "$APP" "$OUT/$LAUNCHER_ZIP"
mkdir "$WORK/dmg"; cp -R "$APP" "$WORK/dmg/"; ln -s /Applications "$WORK/dmg/Applications"
hdiutil create -quiet -volname "D2R Metal Launcher" -srcfolder "$WORK/dmg" -ov -format UDZO "$OUT/$DMG"
if [ -n "$ID" ]; then
  sign "$OUT/$DMG"; say "notarizing the disk image"; notarize "$OUT/$DMG"; xcrun stapler staple "$OUT/$DMG" > /dev/null
  spctl -a -t open --context context:primary-signature -v "$OUT/$DMG" 2>&1 | tail -1
fi

# ---- the release's description, signed, and the in-game notice ----
write_manifest "$OUT/manifest.json" 1
if [ -n "$SIGNKEY" ]; then
  swift "$HERE/manifest_sign.swift" sign "$OUT/manifest.json" || die "the manifest could not be signed"
  swift "$HERE/manifest_sign.swift" verify "$OUT/manifest.json" "$OUT/manifest.json.sig" "$(swift "$HERE/manifest_sign.swift" public)" || die "the manifest signature does not verify"
else
  rm -f "$OUT/manifest.json.sig"; say "the manifest is not signed (no release key here): only test launchers accept it"
fi
printf '{"build": %s, "note": "D2R Metal %s", "note_en": "D2R Metal %s", "url": "d2rmetal://update"}\n' "$BUILD" "$VERSION" "$VERSION" > "$OUT/latest_mac.json"
say "built: $(cd "$OUT" && ls | tr '\n' ' ')"
[ -n "$UPLOAD" ] || { say "not uploaded (--no-upload / --unsigned)"; exit 0; }

if gh release view "v$VERSION" --repo "$GH_REPO" > /dev/null 2>&1; then
  [ "$(gh release view "v$VERSION" --repo "$GH_REPO" --json isDraft --jq .isDraft)" = true ] || die "release v$VERSION is already published on GitHub"
  [ -n "$REPLACE" ] || die "a draft v$VERSION exists (an earlier run?): --replace-draft deletes it first"
  gh release delete "v$VERSION" --repo "$GH_REPO" --yes --cleanup-tag || die "the old draft could not be deleted"
fi
ASSETS=("$OUT/$DMG" "$OUT/$LAUNCHER_ZIP" "$OUT/$RUNTIME_NAME" "$OUT/manifest.json" "$OUT/manifest.json.sig" "$OUT/latest_mac.json")
[ -n "$TOOLS_URL" ] || ASSETS+=("$OUT/$TOOLS_NAME")   # a tools pack an earlier published release carries is not uploaded again (the manifest points there)
gh release create "v$VERSION" --repo "$GH_REPO" --draft --target "$(git -C "$REPO" rev-parse HEAD)" --title "D2R Metal $VERSION" \
  --notes-file "${NOTES:-/dev/null}" "${ASSETS[@]}" || die "the draft is incomplete: run again with --replace-draft"
# later releases may point at this copy once it is published (checked then, above)
[ -n "$TOOLS_URL" ] || echo "https://github.com/$GH_REPO/releases/download/v$VERSION/$TOOLS_NAME" > "$TC/url.txt"
say "draft release v$VERSION created: review it on github.com, publish it there, then run release_mac.sh --check-latest"
