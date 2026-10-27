#!/usr/bin/env python3
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parents[1]))
import lib
r = lib.load(sys.argv[1])
ticks = sorted(r._state_by_tick)
lib.precondition(any(i.get("x") == 8 and i.get("y") == 0 and i.get("mode") == 0 and i.get("page") == 0 for i in r.items_at(ticks[0]).values()), "初始背包 (8,0) 有物品（术士存档的魔盒）")
lib.check(any(a == "OpenPanel" and p == "HoradricCubeLayout" for _, a, p in r._panel_events), "双击魔盒后魔盒面板打开（界面消息 OpenPanel HoradricCubeLayout）")
lib.check(all(r.cursor_at(t) is None for t in ticks), "双击全程 cursor 为空（没拿起）")
lib.check(not any(op == 0x19 for _, op, _ in r.c2s_ops()), "双击不发 0x19 拿起")
sys.exit(lib.main())
