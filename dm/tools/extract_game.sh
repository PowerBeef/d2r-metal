#!/bin/bash
# 从你自己的游戏包里取出本项目要的两部分（更新后的程序文件与资源），放进工作区。
# 用法：extract_game.sh <游戏包 .xci 或 .nsp> [<游戏包> …] <prod.keys>
#   游戏包：你自己导出的《暗黑破坏神 II：狱火重生》Switch 版，本体与 3.3.94012 的更新都要有：可以在同一个包里（.xci 常见的「1G+1U」），
#   也可以分开给（本体一个 .nsp、更新一个 .nsp）。eShop 格式的 .nsp 里带的票据（.tik）会自动取出标题密钥，不用另备 title.keys。
#   prod.keys：从你自己的主机导出的密钥文件，放最后。都只在本机读，不拷贝、不上传。
# 产物：$DM_ROOT/data/switch/3.3.94012/Update/{ExeFS,RomFS}（DM_ROOT 缺省 ~/dm-work）。已有这个目录时不覆盖。
# 需要：hactool（开源的 Switch 文件解包工具，https://github.com/SciresM/hactool ，按它的说明 make 出来）。
#   放进 PATH，或用 HACTOOL=<路径> 指给本脚本。磁盘要留出约 20 GB（中间文件 9 GB，做完自动删）。
# Extract the two parts this project needs (the updated program files and the assets) from your own copy of the game and put them into the workspace.
# Usage: extract_game.sh <game dump .xci or .nsp> [<game dump> ...] <prod.keys>
#   Game dump: your own dump of the Switch version of Diablo II: Resurrected; it must contain both the base game and the 3.3.94012 update, either in one package
#   (the common "1G+1U" form for .xci) or as separate packages (one .nsp for the base game, one for the update). Title keys are taken from the tickets (.tik) in eShop-style .nsp files; no title.keys needed.
#   prod.keys: the key file dumped from your own console, given last. Everything is only read on this machine; nothing is copied or uploaded.
# Output: $DM_ROOT/data/switch/3.3.94012/Update/{ExeFS,RomFS} (DM_ROOT defaults to ~/dm-work). An existing directory is not overwritten.
# Requires: hactool (an open-source tool for unpacking Switch files, https://github.com/SciresM/hactool ; build it with make following its instructions).
#   Put it on PATH, or point this script at it with HACTOOL=<path>. Keep about 20 GB of disk free (9 GB of intermediate files, deleted automatically when done).
set -u
[ $# -ge 2 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
KEYS=${!#}; PKGS=("${@:1:$#-1}"); DM_ROOT=${DM_ROOT:-$HOME/dm-work}; OUT=$DM_ROOT/data/switch/3.3.94012/Update
die() { echo "✗ $*" >&2; exit 1; }
HT=${HACTOOL:-$(command -v hactool || true)}; [ -n "$HT" ] && [ -x "$HT" ] || die "没找到 hactool：装好后放进 PATH，或设 HACTOOL=<路径>（见本文件开头）"
for PKG in "${PKGS[@]}"; do [ -f "$PKG" ] || die "没有这个游戏包：$PKG"; done; [ -f "$KEYS" ] || die "没有这个密钥文件：$KEYS"
[ ! -e "$OUT" ] || die "已有 ${OUT}，不覆盖。要重做先自己把它移走"
# hactool 只从 $HOME/.switch/title.keys 读标题密钥：给它一个临时 HOME，里面的 title.keys = 你原有的那份 + 从票据取的，不动你自己的 ~/.switch
# hactool only reads title keys from $HOME/.switch/title.keys: give it a temporary HOME whose title.keys = your existing one + the keys taken from the tickets, leaving your own ~/.switch untouched
ht() { HOME="$T/home" "$HT" -k "$KEYS" --disablekeywarns --suppresskeys "$@"; }
T=$DM_ROOT/build/cache/dm/extract.$$; mkdir -p "$T/nca" "$T/home/.switch" || die "建不了中间目录 $T"; trap 'rm -rf "$T"' EXIT
[ -f "$HOME/.switch/title.keys" ] && cat "$HOME/.switch/title.keys" > "$T/home/.switch/title.keys"
ln -s "$(cd "$(dirname "$KEYS")" && pwd)/$(basename "$KEYS")" "$T/home/.switch/prod.keys"   # 只是链接，免得 hactool 报「prod.keys 不存在」盖住真正的错 | only a link, so hactool does not warn "prod.keys does not exist" over the real error
say() { echo "[$(date +%T)] $*"; }
# 拆不开时：hactool 嫌密钥文件里某一行长度不对，报的是那行的值；按值找回是哪个键，免得只看到一串十六进制
# When unpacking fails: if hactool rejects a line of the key file for its length it reports that line's value; find which key it is by the value, rather than showing only a hex string
keyhint() { local v n; v=$(sed -n 's/.*Key (\([0-9a-fA-F]*\)) must be.*/\1/p' "$1" | head -1); [ -n "$v" ] || return 0
  n=$(grep -i "=[[:space:]]*$v[[:space:]]*$" "$KEYS" | head -1 | sed 's/[[:space:]]*=.*//')
  echo "（密钥文件里 ${n:-有一行} 长度不对，hactool 不接受：删掉这行或换一份密钥文件 | ${n:-a line} in the key file has the wrong length and hactool refuses it: remove that line or use another key file）"; }

say "拆游戏包（约 9 GB）"
n=0
for PKG in "${PKGS[@]}"; do
  n=$((n + 1)); L=$T/unpack$n.log
  case "$PKG" in
    *.xci|*.XCI) ht -t xci --securedir="$T/nca" "$PKG" > "$L" 2>&1 || die "游戏包拆不开（密钥不对或文件损坏），日志 $L：$(tail -1 "$L")$(keyhint "$L")";;
    *.nsp|*.NSP) ht -t pfs0 --outdir="$T/nca" "$PKG" > "$L" 2>&1 || die "游戏包拆不开（密钥不对或文件损坏）：$(tail -1 "$L")$(keyhint "$L")";;
    *) die "只认 .xci 或 .nsp：$PKG";;
  esac
done

# 票据（.tik，eShop 格式的 .nsp 才有）里的标题密钥：按签名类型定出票据数据的起点，再取 密钥种类（+0x141）、权利 ID（+0x160）、密钥（+0x40）。
# 通用票据（种类 0）的密钥能直接用；个人票据（种类 1）绑定导出它的那台主机，取不出来——你的 ~/.switch/title.keys 里已有这个标题的密钥时照样能拆。
# Title keys from the tickets (.tik, only in eShop-style .nsp files): the signature type gives where the ticket data starts, then read the key type (+0x141), the rights ID (+0x160) and the key (+0x40).
# A common ticket (type 0) holds a key that can be used directly; a personalized ticket (type 1) is bound to the console it was dumped from and cannot be read - extraction still works if your ~/.switch/title.keys has the key for that title.
tk=0
for tik in "$T"/nca/*.tik; do
  [ -f "$tik" ] || continue
  case "$(od -An -tx4 -N4 "$tik" | tr -d ' ')" in
    00010000|00010003) base=$((0x240));;   # RSA-4096 签名 | RSA-4096 signature
    00010001|00010004) base=$((0x140));;   # RSA-2048 签名 | RSA-2048 signature
    00010002|00010005) base=$((0x80));;    # ECDSA 签名 | ECDSA signature
    *) say "跳过认不出签名类型的票据 $(basename "$tik") | skipping a ticket with an unknown signature type"; continue;;
  esac
  kind=$(od -An -tu1 -j $((base + 0x141)) -N1 "$tik" | tr -d ' ')
  rid=$(od -An -tx1 -j $((base + 0x160)) -N16 "$tik" | tr -d ' \n')
  if [ "$kind" != 0 ]; then say "票据 $(basename "$tik") 是个人票据（绑定主机），取不出标题密钥 | personalized ticket, its title key cannot be read"; continue; fi
  echo "$rid = $(od -An -tx1 -j $((base + 0x40)) -N16 "$tik" | tr -d ' \n')" >> "$T/home/.switch/title.keys"; tk=$((tk + 1))
done
[ $tk = 0 ] || say "从游戏包的票据取到 $tk 个标题密钥"

# 找出本体与更新的程序内容：按每个内容文件自己报的类型与程序号认，不靠文件名。
# 更新的程序内容报的也是本体的程序号；区别是它的资源段是「补丁」格式（BKTR），要叠在本体上才读得出
# Find the program content of the base game and of the update: identify them by the type and program id each content file reports, not by file name.
# The update's program content also reports the base game's program id; the difference is that its asset section is in "patch" format (BKTR) and can only be read when layered on top of the base game
BASE=; UPD=
for f in "$T"/nca/*.nca; do
  case "$f" in *.cnmt.nca) continue;; esac
  info=$(ht -t nca "$f" 2>&1)
  echo "$info" | grep -q "Content Type: *Program" || continue
  echo "$info" | grep -q "^Title ID: *0100726014352000" || continue
  if echo "$info" | grep -q "BKTR"; then UPD=$f; else BASE=$f; fi
done
[ -n "$BASE" ] || die "游戏包里没找到这个游戏的本体（本体与更新分开的话，两个包都要给）"
[ -n "$UPD" ] || die "游戏包里没有更新：本项目要 3.3.94012 的更新，只有本体不行（更新是单独的包的话，一起给上）"

say "取程序文件与资源（约 9 GB）"
mkdir -p "$OUT.part" || die "建不了 $OUT.part"
ht -t nca --basenca="$BASE" --exefsdir="$OUT.part/ExeFS" --romfsdir="$OUT.part/RomFS" "$UPD" > "$T/extract.log" 2>&1 || { rm -rf "$OUT.part"; die "更新内容取不出来：$(tail -1 "$T/extract.log")"; }

# 版本核对：三个程序文件的校验值必须是 3.3.94012 的
# Version check: the checksums of the three program files must be those of 3.3.94012
ok=1
while read -r want name; do
  got=$(shasum -a 256 "$OUT.part/ExeFS/$name" 2> /dev/null | cut -c1-64)
  if [ "$got" = "$want" ]; then echo "  ✓ $name"; else echo "  ✗ $name 不是 3.3.94012 的（${got:-文件不在}）"; ok=0; fi
done <<SUMS
55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e main
1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017 sdk
67a38fcf89cd13e040a021eb3d157072f38ebf6680a907874c35c7b231b5d4e2 subsdk0
SUMS
[ -f "$OUT.part/RomFS/data/build_manifest.psv" ] || { echo "  ✗ 资源里没有 data/build_manifest.psv"; ok=0; }
[ "$ok" = 1 ] || { rm -rf "$OUT.part"; die "版本不对：游戏包里的更新不是 3.3.94012"; }
mv "$OUT.part" "$OUT" || die "改名到 $OUT 没成"
say "好了：${OUT}。接着在仓库根目录运行 ./setup.sh"
