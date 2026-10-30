#!/bin/bash
# 把 Mac 宿主包成能双击起的 macOS App：mac_app.sh [输出目录]   产出 <输出目录>/D2R Metal.app（缺省 $DM_ROOT/out/macapp）
# 包里有：宿主程序、改写后的游戏程序（产物目录，种类 DM_MAC_PRODUCT，缺省 touch）、有界面覆盖层就带上（DM_MAC_OVERLAY=<目录>）。
# DM_MAC_LINK=1：不带派生物的出法——改写后的游戏程序、着色器库、界面覆盖层都是从游戏内容派生的，公开发布的包不能带；
#   这样出的包里只有宿主程序和 Resources/paths.txt（各项指向本机构建产物的路径），只在构建它的这台机器上能跑。
# 版本：给人看的版本号 DM_MAC_VERSION（缺省 1.0，与 iOS 包同叫法）；构建号 DM_MAC_BUILD（缺省取当前月日时分，同 iOS 包格式）。产物代号与提交号写在包里的 Resources/build.txt。
# 更新检查：DM_MAC_UPDATE_URL=<版本文件地址>（如下载站的 latest_mac.json）写进 Info.plist 的 SWUpdateURL，App 启动后读它、比包内构建号新就在主菜单提示；不给 = 不检查（公开版 / 自己构建的包）。
# 游戏资源缺省不进包（DM_MAC_ROMFS_COPY=1 可带自己的资源）：第一次启动请玩家选资源文件夹（host/app_mac.m）。签名缺省用本地临时签名；要 iCloud 同步用 DM_MAC_ICLOUD=1（见下）。
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); DM=$HERE/..; ROOT=${DM_ROOT:-$HOME/dm-work}
OUT=${1:-$ROOT/out/macapp}; KIND=${DM_MAC_PRODUCT:-touch}; BID=${DM_MAC_BUNDLE_ID:-org.d2rmobile.mac}
P=$(bash "$HERE/product.sh" path "$KIND"); [ -f "$P/main.patched.bin" ] || { echo "🔴 没有产物 ${P}（先跑 product.sh build）"; exit 2; }
NAME="D2R Metal"
# DM_MAC_ICLOUD=1：用付费开发者的 provisioning profile（带 iCloud 能力的 Mac profile）签名，出的 App 能 iCloud 同步存档。
# 把 profile 放进签名目录（缺省 ~/dm-work/mac-sign，DM_MAC_SIGN_DIR 可换）即可：团队、包标识、entitlements、证书全从 profile 里读，
# 签名身份按内嵌证书的 SHA-1 指纹在钥匙串精确匹配；包标识决定 iCloud 容器（iOS 包的 profile = 与手机同步，自定标识 = 两台 Mac 互同步）。
# 不设时照旧：本机临时签名，不需要开发者账号。
ICLOUD=${DM_MAC_ICLOUD:-}; PF=; TMPD=
if [ -n "$ICLOUD" ]; then
  SIGN_DIR=${DM_MAC_SIGN_DIR:-$ROOT/mac-sign}
  PF=$(ls -t "$SIGN_DIR"/*.provisionprofile "$SIGN_DIR"/*.mobileprovision 2>/dev/null | head -1)
  [ -n "$PF" ] || { echo "🔴 签名目录里没有 profile：把带 iCloud 能力的 .provisionprofile 放进 ${SIGN_DIR}（DM_MAC_SIGN_DIR 可换目录）"; exit 2; }
  TMPD=$(mktemp -d "${TMPDIR:-/tmp}/dm_mac_sign.XXXXXX") || { echo "🔴 建不了临时目录"; exit 3; }
  trap 'rm -rf "$TMPD"' EXIT
  security cms -D -i "$PF" > "$TMPD/p.plist" 2>/dev/null || { echo "🔴 解不开 $(basename "$PF")（不是有效的 provisioning profile）"; exit 3; }
  TEAM=$(/usr/libexec/PlistBuddy -c "Print :ApplicationIdentifierPrefix:0" "$TMPD/p.plist" 2>/dev/null || true)
  APPID=$(/usr/libexec/PlistBuddy -c "Print :Entitlements:com.apple.application-identifier" "$TMPD/p.plist" 2>/dev/null || true)
  [ -n "$APPID" ] || APPID=$(/usr/libexec/PlistBuddy -c "Print :Entitlements:application-identifier" "$TMPD/p.plist" 2>/dev/null || true)
  [ -n "$TEAM" ] && [ -n "$APPID" ] || { echo "🔴 读不出 $(basename "$PF") 的团队/包标识"; exit 3; }
  BID=${APPID#*.}
  # 平台要 OSX、必须带 iCloud 容器、不能过期
  plutil -extract Platform json -o - "$TMPD/p.plist" 2>/dev/null | grep -q '"OSX"' || { echo "🔴 $(basename "$PF") 不是 Mac 平台的 profile"; exit 3; }
  /usr/libexec/PlistBuddy -c "Print :Entitlements:com.apple.developer.icloud-container-identifiers:0" "$TMPD/p.plist" > /dev/null 2>&1 || { echo "🔴 $(basename "$PF") 不带 iCloud 容器"; exit 3; }
  EXP=$(/usr/libexec/PlistBuddy -c "Print :ExpirationDate" "$TMPD/p.plist" 2>/dev/null || true)
  EXPS=$(date -j -f "%a %b %d %T %Z %Y" "$EXP" "+%s" 2>/dev/null || echo 0)
  [ "$EXPS" -gt "$(date +%s)" ] || { echo "🔴 $(basename "$PF") 已过期（${EXP}）：重新下载一份放进签名目录"; exit 3; }
  echo "签名用 profile：$(basename "$PF")（包标识 ${BID}，到期 ${EXP}）"
fi
make -C "$DM/host" > /dev/null
MIN=$(make -s -C "$DM/host" print-macos-min)   # minimum system version: the same number the host is compiled with (MACOS_MIN in host/Makefile)
# The whole package is first built in the staging folder STG inside the output folder (together with the link-style overlay) and only moved into the output folder after codesign --verify passes:
# the old package used to be deleted first, so a failure at any later step left half a package (the /Applications link pointed at an App that could not start), with the overlay already deleted and being rebuilt
DST="$OUT/$NAME.app"; STG="$OUT/.mac_app.new"; APP="$STG/$NAME.app"
trap 'rm -rf "$STG" ${TMPD:+"$TMPD"}' EXIT
rm -rf "$STG"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$DM/host/dmhost" "$APP/Contents/MacOS/D2RMobile"
LINK=${DM_MAC_LINK-1}; [ "$LINK" != 0 ] || LINK=; RES="$APP/Contents/Resources"; : > "$RES/paths.txt"
if [ -n "$LINK" ]; then echo "product=$P" >> "$RES/paths.txt"; else cp -R "$P" "$RES/product"; fi
cp "$DM/gfx/nvn/shader_map.tsv" "$APP/Contents/Resources/shader_map.tsv"
cp "$DM/gfx/nvn/pso_manifest.tsv" "$APP/Contents/Resources/pso_manifest.tsv"   # 管线预热的种子清单：图形后端按「与映射表同目录」找它（两种出法都带；漏带的包开局边画边现编管线——的日志里现编了 102 条） | seed list for pipeline warm-up: the graphics backend looks for it "in the same directory as the mapping table" (both ways of building carry it; a package without it compiles pipelines on the fly while drawing at the start of a game - 102 of them in one log)
[ ! -f "$DM/gfx/nvn/c1_full.tsv" ] || cp "$DM/gfx/nvn/c1_full.tsv" "$APP/Contents/Resources/c1_full.tsv"   # 图形后端按「与映射表同目录」找它 | The graphics backend looks for it "in the same directory as the mapping table"
# 编好的着色器库（Mac 版，dmsh_build.py 出的）：只带运行要用的 .metallib 与反射 .json，源码 .metal 不带
# The compiled shader library (Mac variant, produced by dmsh_build.py): only the .metallib and reflection .json needed at run time are included, not the .metal sources
V=$(sed -n 's/.*DMSH_CHAIN "\(.*\)".*/\1/p' "$DM/gfx/nvn/dmsh_chain.h")
MSL=${DM_MSL_DIR:-$ROOT/build/cache/metal_shader_cache}/b-$V; MSLR=$MSL
[ -d "$MSLR" ] || { echo "🔴 没有着色器库 ${MSL}（先跑 dm/gfx/tools/dmsh_build.py）"; exit 2; }
if [ -n "$LINK" ]; then echo "msl=$(dirname "$MSL")" >> "$RES/paths.txt"
else mkdir -p "$RES/msl/b-$V"; rsync -a --include='*.metallib' --include='*.json' --exclude='*' "$MSLR/" "$RES/msl/b-$V/"; fi
# The two game-derived tables the host reads at start (dm/host/dm_tables.h; make writes them to dm/host/tables beside dmhost): carried like the overlay,
# beside the package in the link style, inside it otherwise
[ -f "$DM/host/tables/hud_exp.tsv" ] && [ -f "$DM/host/tables/bright_levels.tsv" ] || { echo "🔴 no game tables in $DM/host/tables (make -C dm/host tables, with the game files in place)"; exit 2; }
if [ -n "$LINK" ]; then rm -rf "$STG/tables.local"; cp -R "$DM/host/tables" "$STG/tables.local"; echo "tables=$OUT/tables.local" >> "$RES/paths.txt"
else cp -R "$DM/host/tables" "$RES/tables"; fi
# 界面覆盖层（自定义页、珍宝匣、秘境等我们的页面都在里面）：从游戏资源现生成（约 3 秒）；DM_MAC_OVERLAY=<目录> 用现成的，=none 不带（原版界面）
# UI overlay (the Custom page, the trove, rifts and our other pages all live in it): generated on the spot from the game resources (about 3 seconds); DM_MAC_OVERLAY=<dir> uses an existing one, =none leaves it out (original UI)
if [ "${DM_MAC_OVERLAY:-}" != none ]; then
  # OVD = 覆盖层最终的位置，OVW = 这次写到哪：自带的在包里；链接式的写进暂存目录，最后随包换到 $OUT | OVD = the overlay's final location, OVW = where this run writes it: inside the package when bundled; the link style writes into the staging folder and is moved to $OUT with the package at the end
  OVD="$RES/romfs_overlay"; OVW=$OVD; [ -z "$LINK" ] || { OVD="$OUT/romfs_overlay.local"; OVW="$STG/romfs_overlay.local"; echo "romfs_overlay=$OVD" >> "$RES/paths.txt"; }
  # 基础覆盖层与开关组合成套更新，防止复用输出目录时残留旧组合；外部覆盖层也照带同名兄弟目录。
  # Refresh the base overlay and the feature combinations together, so no stale combination survives when an output directory is reused; an external overlay brings its same-named sibling directory too.
  if [ -n "${DM_MAC_OVERLAY:-}" ]; then
    [ "$(cd "$DM_MAC_OVERLAY" && pwd -P)" != "$(mkdir -p "$OVD" && cd "$OVD" && pwd -P)" ] || { echo "DM_MAC_OVERLAY must differ from the output overlay directory"; exit 2; }
  fi
  rm -rf "$OVW" "${OVW}_variants"
  if [ -n "${DM_MAC_OVERLAY:-}" ]; then
    cp -R "$DM_MAC_OVERLAY" "$OVW"
    [ ! -d "${DM_MAC_OVERLAY%/}_variants" ] || cp -R "${DM_MAC_OVERLAY%/}_variants" "${OVW}_variants"
  else DM_OVERLAY_MAC=1 python3 "$DM/ios/tools/gen_romfs_overlay.py" "${DM_ROMFS_SRC:-$ROOT/data/switch/3.3.94012/Update/RomFS}" "$OVW" > /dev/null || { echo "🔴 界面覆盖层生成失败"; exit 2; }; fi
  [ -z "$LINK" ] || [ ! -d "${OVW}_variants" ] || echo "romfs_overlay_variants=${OVD}_variants" >> "$RES/paths.txt"
fi
# DM_MAC_ROMFS_COPY=1：把自己的游戏资源拷进包（Resources/romfs），启动时 app_mac.m 优先用它，不再问资源文件夹；只给自己用，不许分发
# DM_MAC_ROMFS_COPY=1: copy your own game resources into the package (Resources/romfs); at launch app_mac.m prefers them and no longer asks for the resource folder. For your own use only, never distribute it
if [ "${DM_MAC_ROMFS_COPY:-0}" = 1 ]; then
  ROM=${DM_ROMFS_SRC:-$ROOT/data/switch/3.3.94012/Update/RomFS}
  [ -f "$ROM/data/build_manifest.psv" ] || { echo "🔴 DM_MAC_ROMFS_COPY=1 但资源里没有清单：$ROM/data/build_manifest.psv"; exit 2; }
  ditto "$ROM" "$RES/romfs"
fi
# The App icon: by default the project's own, from dm/brand/icon.sh (the layered icon of dm/brand/AppIcon.icon when Xcode's actool is there,
# else the flattened one). DM_MAC_ICON_SOURCE=<image> uses another picture instead, an opaque full-bleed square (from macOS 26 on the system
# rounds the corners itself; a picture with transparent margins was shown shrunk inside a gray frame on macOS 27); =none builds without an icon.
ICON_SRC=${DM_MAC_ICON_SOURCE:-}; ICON_KEY=
if [ -z "$ICON_SRC" ]; then
  if [ -x "$DM/brand/icon.sh" ]; then ICON_KEY=$("$DM/brand/icon.sh" "$RES")
  else echo "note: dm/brand/icon.sh is missing (a tree with only part of the sources); building without an icon"; fi
elif [ "$ICON_SRC" != none ]; then
  [ -f "$ICON_SRC" ] || { echo "🔴 DM_MAC_ICON_SOURCE points at a missing image: $ICON_SRC"; exit 2; }
  ICONSET="$RES/AppIcon.iconset"; mkdir "$ICONSET"
  for size in 16 32 128 256 512; do
    sips -s format png -z "$size" "$size" "$ICON_SRC" --out "$ICONSET/icon_${size}x${size}.png" > /dev/null
    twice=$((size * 2))
    sips -s format png -z "$twice" "$twice" "$ICON_SRC" --out "$ICONSET/icon_${size}x${size}@2x.png" > /dev/null
  done
  iconutil -c icns "$ICONSET" -o "$RES/AppIcon.icns"
  rm -r "$ICONSET"
  ICON_KEY="<key>CFBundleIconFile</key><string>AppIcon</string>"
fi
if [ -z "$LINK" ] && [ -n "$(find "$RES" -type l -print -quit)" ]; then
  echo "🔴 自带资源的包里不许有符号链接（换到别的机器就断）"; exit 3
fi
[ -s "$RES/paths.txt" ] || rm -f "$RES/paths.txt"
VER=${DM_MAC_VERSION:-1.0}; BUILD=${DM_MAC_BUILD:-$(date +%m%d%H%M)}
echo "版本 ${VER} 构建 ${BUILD} 产物 $(basename "$P") 提交 $(git -C "$DM" rev-parse --short HEAD 2>/dev/null || echo 未知)" > "$RES/build.txt"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>$NAME</string>
<key>CFBundleDisplayName</key><string>$NAME</string>
<key>CFBundleIdentifier</key><string>$BID</string>
<key>CFBundleExecutable</key><string>D2RMobile</string>
${ICON_KEY}
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VER</string>
<key>CFBundleVersion</key><string>$BUILD</string>
<key>LSMinimumSystemVersion</key><string>$MIN</string>
${DM_MAC_UPDATE_URL:+<key>SWUpdateURL</key><string>$DM_MAC_UPDATE_URL</string>
}<key>NSHighResolutionCapable</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
<key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
PL
# 自检：管线预热的种子清单在包里、与映射表同目录、不是空的
# Self-check: the seed list for pipeline warm-up is in the package, in the same directory as the mapping table, and not empty
[ -s "$RES/pso_manifest.tsv" ] && [ -s "$RES/shader_map.tsv" ] || { echo "🔴 包里缺管线预热种子清单或着色器映射表（Resources/pso_manifest.tsv、shader_map.tsv）"; exit 3; }
# 自检：带进包的着色器库记的目标系统不许高于包声明的最低系统（抽查头、中、尾各一个与一个 _bf；链接式的包不带库，不查）
# Self-check: the target system recorded in the shader libraries carried in the package must not be higher than the minimum system the package declares (spot-check one each from the head, middle and tail plus one _bf; a link-style package carries no libraries and is not checked)
if [ -z "$LINK" ]; then ( cd "$RES/msl/b-$V" && for f in $(ls | grep '_b\.metallib$' | sed -n '1p;1000p;$p') $(ls | grep '_bf\.metallib$' | head -1); do
  LV=$(xcrun -sdk macosx metal-objdump -d "$f" 2>/dev/null | sed -n 's/.*target triple = ".*-apple-macosx\([0-9]*\.[0-9]*\).*/\1/p' | head -1)
  [ -n "$LV" ] && [ "$(printf '%s\n%s\n' "$LV" "$MIN" | sort -V | tail -1)" = "$MIN" ] || { echo "🔴 着色器库 ${f} 的目标系统是 macOS ${LV:-读不到}，高于包声明的最低系统 ${MIN}"; exit 3; }
done ) || exit 3; fi
# 自检：程序本体里记的最低系统版本要与 Info.plist 写的相等（不等时老系统上双击只会得到一句「不能与此版本的 macOS 配合使用」）；只许有 arm64 一个架构
# Self-check: the minimum system version recorded in the executable must equal the one written in Info.plist (if they differ, double-clicking on an older system only yields "cannot be used with this version of macOS"); only the single arm64 architecture is allowed
BIN_MIN=$(otool -l "$APP/Contents/MacOS/D2RMobile" | awk '$1=="minos"{print $2; exit}'); PL_MIN=$(/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$APP/Contents/Info.plist")
[ -n "$BIN_MIN" ] && [ "$BIN_MIN" = "$PL_MIN" ] || { echo "🔴 最低系统版本对不上：程序本体 ${BIN_MIN:-读不到}，Info.plist ${PL_MIN}"; exit 3; }
ARCHS=$(lipo -archs "$APP/Contents/MacOS/D2RMobile"); [ "$ARCHS" = arm64 ] || { echo "🔴 程序本体的架构不是只有 arm64：${ARCHS}"; exit 3; }
python3 "$HERE/mac_bundle.py" "$APP/Contents/MacOS/D2RMobile" "$DM/host/dmhost"
if [ -z "$ICLOUD" ]; then
  codesign --force -s - "$APP" > /dev/null 2>&1 || { echo "🔴 本地签名失败"; exit 3; }
else
  # 嵌入 profile，按其 entitlements 用内嵌证书对应的钥匙串身份签
  cp "$PF" "$APP/Contents/embedded.provisionprofile"
  plutil -extract Entitlements xml1 -o "$TMPD/ent.plist" "$TMPD/p.plist" 2>/dev/null || { echo "🔴 读不出 profile 的 entitlements"; exit 3; }
  CERT_SHA=$(plutil -extract DeveloperCertificates.0 raw "$TMPD/p.plist" 2>/dev/null | tr -d '\n' | base64 -d 2>/dev/null | openssl x509 -noout -fingerprint -sha1 2>/dev/null | sed 's/.*=//; s/://g' | tr 'A-Z' 'a-z')
  [ -n "$CERT_SHA" ] || { echo "🔴 读不出 profile 内嵌证书的指纹"; exit 3; }
  ID=$(security find-identity -v -p codesigning 2>/dev/null | awk -v h="$CERT_SHA" 'tolower($2) == h { $1 = $2 = ""; sub(/^ */, ""); gsub(/^"|"$/, ""); print; exit }')
  [ -n "$ID" ] || { echo "🔴 钥匙串里没有与 profile 证书（指纹 ${CERT_SHA}）对应的签名身份：Xcode → Settings → Accounts 登录该团队会自动装"; exit 3; }
  # 不加 --options runtime：宿主要运行时写入并执行改写后的游戏代码页，Hardened Runtime 会把这种页 SIGKILL
  codesign --force -s "$ID" --entitlements "$TMPD/ent.plist" "$APP" > /dev/null 2>&1 || { echo "🔴 团队证书签名失败"; exit 3; }
  codesign -d --entitlements :- "$APP" 2>/dev/null | plutil -p - 2>/dev/null | grep -q "icloud-container-identifiers" || { echo "🔴 签出来的包没有 iCloud 权限"; exit 3; }
  echo "签名：${ID}（iCloud 容器 $(/usr/libexec/PlistBuddy -c "Print :com.apple.developer.icloud-container-identifiers:0" "$TMPD/ent.plist" 2>/dev/null || echo "?")）"
fi
codesign --verify --deep --strict "$APP"
# 验过了，换上去：先删同名的旧包（链接式的还有旧覆盖层与它的组合目录，免得留下上一套多出来的），再把暂存目录里的逐项挪进输出目录（同一个盘内只是改名）
# Verified, so swap it in: first delete the old package of the same name (for the link style also the old overlay and its combinations directory, so nothing extra from the previous set survives), then move each item of the staging folder into the output folder (a rename on the same disk)
[ -z "$LINK" ] || [ "${DM_MAC_OVERLAY:-}" = none ] || rm -rf "$OUT/romfs_overlay.local" "$OUT/romfs_overlay.local_variants"
for e in "$STG"/*; do rm -rf "${OUT:?}/${e##*/}"; mv "$e" "$OUT/" || { echo "🔴 新包换不到 ${OUT}（${e##*/}）"; exit 3; }; done
APP=$DST
echo "MACAPP ${APP}（版本 ${VER} 构建 ${BUILD}，最低系统 macOS ${MIN}，产物 $(basename "$P")，$(du -sh "$APP" | cut -f1)${LINK:+，不带派生物：只能在本机跑}）"
