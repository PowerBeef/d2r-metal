#!/bin/bash
# 生成触屏用例的夹具存档：跑 touchcases/_fixtures/<名>（触点脚本把物品摆到用例要的格子），在第 <存盘拍> 拍让游戏自己存一次盘（DM_TEST_SAVE_AT），
# 断言过了就把这一局提交的存档导出成 $DM_ROOT/data/switch/save_ref_touch_<名>/0000000000000001/0（只放本机，不入库）。用例目录里的 save 文件指向它。
# 用法：touch_fixture.sh <名> [存盘拍=900]   末行 TOUCHFIX <名> OK|RED
# Generates a fixture save for the touch cases: runs touchcases/_fixtures/<name> (a touch script that puts items where the cases need them), has the game save once at tick <save tick> (DM_TEST_SAVE_AT),
# and when the assertions pass exports the save this run committed as $DM_ROOT/data/switch/save_ref_touch_<name>/0000000000000001/0 (local only, never committed). A case directory's save file points at it.
# Usage: touch_fixture.sh <name> [save tick=900]   last line TOUCHFIX <name> OK|RED
set -u
HERE=$(cd "$(dirname "$0")" && pwd); NAME=${1:?usage: touch_fixture.sh <name> [save tick]}; AT=${2:-900}
DMR=${DM_ROOT:-$HOME/dm-work}; RR=$(mktemp -d "$DMR/run/verify/dm/touchfix_${NAME}_XXXX") || exit 2
DST="$DMR/data/switch/save_ref_touch_$NAME/0000000000000001/0"
DM_TOUCH_KEEP=1 DM_TOUCH_RUNROOT="$RR" DM_TEST_SAVE_AT="$AT" bash "$HERE/touchchain.sh" "_fixtures/$NAME" > "$RR/chain.log" 2>&1; rc=$?
RUN=$(ls -d "$RR"/_fixtures/"$NAME"_* 2>/dev/null | head -1)
if [ "$rc" -ne 0 ] || [ -z "$RUN" ]; then tail -5 "$RR/chain.log"; echo "TOUCHFIX $NAME RED（touchchain 退出码 | exit code $rc；$RR）"; exit 1; fi
grep -q '"result":"commit"' "$RUN/events.ndjson" || { echo "TOUCHFIX $NAME RED 第 $AT 拍没有存档提交 | no save commit at tick $AT（$RUN）"; exit 1; }
python3 "$HERE/d2save.py" "$RUN/user/0" | tail -1 | grep -q '^D2SAVE OK' || { echo "TOUCHFIX $NAME RED 导出的存档不合法 | the exported save is invalid"; exit 1; }
rm -rf "$DST.tmp" && mkdir -p "$(dirname "$DST")" && cp -R "$RUN/user/0" "$DST.tmp" && rm -rf "$DST" && mv "$DST.tmp" "$DST" || { echo "TOUCHFIX $NAME RED 拷不过去 | copy failed"; exit 1; }
rm -rf "$RR"
echo "TOUCHFIX $NAME OK → $DST"
