#!/usr/bin/env python3
# 夹具成没成：起跑时 (9,3) 有物品、(5,3) 空；收尾时那件物品在 (5,3)、(6,3) 空、没拿在手上
# Did the fixture work: at the start (9,3) holds an item and (5,3) is empty; at the end that item is at (5,3), (6,3) is empty, nothing in hand
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parents[2]))
import lib
r = lib.load(sys.argv[1])
ticks = sorted(r._state_by_tick)
first, last = r.items_at(ticks[0]), r.items_at(ticks[-1])
src = [i for i in first.values() if i.get("x") == 9 and i.get("y") == 3 and i.get("mode") == 0]
lib.precondition(len(src) == 1, "初始 (9,3) 有一件物品")
lib.precondition(not any(i.get("x") == 5 and i.get("y") == 3 and i.get("mode") == 0 for i in first.values()), "初始 (5,3) 为空")
if src:
    end = last.get(src[0]["id"]) or {}
    lib.check(end.get("x") == 5 and end.get("y") == 3 and end.get("mode") == 0, "收尾时那件物品在 (5,3)")
lib.check(not any(i.get("x") == 6 and i.get("y") == 3 and i.get("mode") == 0 for i in last.values()), "收尾时 (6,3) 为空")
lib.check(r.cursor_at(ticks[-1]) is None, "收尾时手上没有物品")
sys.exit(lib.main())
