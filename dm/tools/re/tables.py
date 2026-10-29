#!/usr/bin/env python3
# 数据表列描述提取：游戏启动时把每张 excel 表（.txt）按一份列描述读进定长记录数组。列描述是调用方栈上的数组，每项 0x20 字节
#   {列名, 种类, 长度或位号, 记录内偏移, 回调}，种类 0 结束。读表函数 main+0x4d9040（w0 标志, x1 表名, x4 列描述, x5 记录长度）
#   这一版读预编译的 .bin（首字行数，其后整块记录）：在 main+0x4d96b0 处 x0 = 调用方分配器给的记录数组、w19 = 行数，随后整块拷入；
#   没有 .bin 时才走文本解析 main+0xde3450（x1 列描述, x2 记录数组, w3 行数, w4 记录长度）。几乎所有表都由 main+0x4db130 读（启动时
#   main+0x2410dc 调一次），列描述在它的栈上逐项拼出、不在全局，所以在真游戏里取：lldb 起无窗口 CLI 宿主，在读表入口记参数与整份列描述、在分配点记记录数组与行数，
#   main+0x4db130 返回时（main+0x2410e0）在 main 的数据段和三个表根对象（[main+0x2d64a28/a38/a48]）里找各记录数组的指针，得出每张表挂在哪。
#   输出 $DM_ROOT/re/export/tables.json（由游戏程序推出：只放本机，不入库）；seed 据此建记录结构体、给表指针起名。
#   用法：tables.py [--timeout 秒]（需已备好的工作区与 dm/host/dmhost；要真跑游戏：不能和查询服务器同时开）
# Data-table column descriptor extraction: at startup the game reads each excel table (.txt) into an array of fixed-size records, driven by a column descriptor array on the caller's stack. Each entry is 0x20 bytes
#   {column name, kind, length or bit number, offset in the record, callback}; kind 0 ends it. The table reader main+0x4d9040 (w0 flags, x1 table name, x4 columns, x5 record size)
#   loads the precompiled .bin in this build (first word = rows, then the records as one block): at main+0x4d96b0 x0 = the record array from the caller's allocator and w19 = rows, then the block is copied in;
#   only without a .bin does it go through the text parser main+0xde3450 (x1 columns, x2 record array, w3 rows, w4 record size). Almost every table is read by main+0x4db130 (called once at startup
#   from main+0x2410dc), which builds the column arrays on its stack entry by entry, not in globals, so they are taken from the real game: lldb starts the headless CLI host, records the arguments and whole column array at the reader's entry and the record array and rows at the allocation point,
#   and when main+0x4db130 returns (main+0x2410e0) searches main's data segment and the three table root objects ([main+0x2d64a28/a38/a48]) for pointers to each record array, to learn where each table hangs.
#   Output: $DM_ROOT/re/export/tables.json (derived from the game program: local only, never committed); seed builds record structures and names the table pointers from it.
#   Usage: tables.py [--timeout s] (needs the prepared workspace and dm/host/dmhost; it runs the real game, so not while the query server is up)
# pyright: reportAttributeAccessIssue=false
import argparse
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DM = HERE.parent.parent
DM_ROOT = Path(os.environ.get('DM_ROOT') or Path.home() / 'dm-work')
OUT = DM_ROOT / 're' / 'export' / 'tables.json'
READ, ALLOC, PARSE, BUILT = 0x4d9040, 0x4d96b0, 0xde3450, 0x2410e0   # 读表入口 / .bin 分配后 / 文本解析 / 总读表函数返回点 | reader entry / after the .bin allocation / text parser / return point of the master loader
ROOTS = (0x2d64a28, 0x2d64a38, 0x2d64a48)          # 表根对象指针（main+0x4da800 建，0x2700 字节） | table root object pointers (built by main+0x4da800, 0x2700 bytes)
ROOT_SZ = 0x2700
ENTRY, MAX_COLS = 0x20, 0x15e                      # 解析函数最多收 350 列 | the parser accepts at most 350 columns
# 种类 → (宽度, 说明)：宽度取自解析函数缺省清零那段（行里缺这一列时写的值） | kind → (width, description): widths from the parser's default-clear switch (the value written when a row lacks the column)
KINDS = {1: (0, 'text'), 2: (4, 'i32'), 3: (2, 'i16'), 4: (1, 'i8'), 5: (1, 'i8 (blank = keep)'), 6: (1, 'u8'), 7: (1, 'raw'),
         8: (4, 'u32'), 9: (4, 'code4'), 10: (4, 'lookup32'), 11: (4, 'lookup32 (-1)'), 12: (1, 'lookup8'), 13: (1, 'lookup8 (-1)'),
         14: (2, 'lookup16'), 15: (2, 'lookup16 (-1)'), 16: (1, 'lookup8'), 17: (2, 'lookup16'), 18: (4, 'lookup32'), 19: (3, 'code3'),
         20: (5, 'code4+1'), 21: (4, 'lookup32 (-1)'), 22: (2, 'lookup16 (-1)'), 23: (1, 'lookup8 (-1)'), 24: (2, 'lookup16'),
         25: (4, 'lookup32'), 26: (0, 'callback'), 27: (0, 'callback'), 28: (0, 'callback'), 29: (0, 'flag bit'), 30: (0, 'flag bit (inverted)')}


def capture(debugger, timeout):
    import lldb  # pyright: ignore[reportMissingImports]  # noqa: PLC0415 -- lldb 自己的 Python 里才有 | only inside lldb's own Python
    debugger.SetAsync(False)
    host = DM / 'host' / 'dmhost'
    prod = subprocess.run(['bash', str(DM / 'tools' / 'product.sh'), 'path', 'touch'], capture_output=True, text=True, check=True).stdout.strip()
    run = DM_ROOT / 'run' / 're_tables'
    (run / 'save').mkdir(parents=True, exist_ok=True); (run / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_ROMFS=os.environ.get('DM_ROMFS') or str(DM_ROOT / 'data/switch/3.3.94012/Update/RomFS'), DM_SAVE=str(run / 'save'),
               DM_TEMP=str(run / 'tmp'), DM_MSL_DIR=str(DM_ROOT / 'build/cache/metal_shader_cache'), DM_ROOT=str(DM_ROOT))
    for k in ('DM_WINDOW', 'DM_WATCH', 'DM_WATCH_STALL', 'DM_PROBE'): env.pop(k, None)
    target = debugger.CreateTarget(str(host))
    bp0 = target.BreakpointCreateByName('probe_install')
    li = lldb.SBLaunchInfo([prod, 'main', 'sdk'])
    li.SetEnvironmentEntries([f'{k}={v}' for k, v in env.items()], False)
    li.SetWorkingDirectory(str(DM / 'host'))
    err = lldb.SBError()
    proc = target.Launch(li, err)
    if not err.Success() or not proc.IsValid(): raise SystemExit(f'🔴 launch failed: {err}')
    ci = debugger.GetCommandInterpreter(); res = lldb.SBCommandReturnObject()
    ci.HandleCommand('process handle SIGUSR1 SIGUSR2 SIGALRM SIGPROF SIGPIPE SIGURG SIGVTALRM -s false -n false -p true', res)

    def rd(a, n):
        e = lldb.SBError(); b = proc.ReadMemory(a, n, e) if a else None
        return b if b is not None and e.Success() else None

    def u(a, n=8):
        b = rd(a, n); return int.from_bytes(b, 'little') if b else 0

    def cstr(a, cap=96):
        if not a: return ''
        e = lldb.SBError(); s = proc.ReadCStringFromMemory(a, cap, e)
        return s if e.Success() else ''

    if proc.GetState() != lldb.eStateStopped: raise SystemExit('🔴 did not stop at probe_install')
    mods = []
    n = target.FindFirstGlobalVariable('g_nmod').GetValueAsUnsigned(); arr = target.FindFirstGlobalVariable('g_mod')
    for i in range(n):
        m = arr.GetChildAtIndex(i)
        mods.append(((m.GetChildMemberWithName('name').GetSummary() or '').strip('"'), m.GetChildMemberWithName('base').GetValueAsUnsigned(), m.GetChildMemberWithName('size').GetValueAsUnsigned()))
    base, size = next((b, s) for nm, b, s in mods if nm == 'main')
    target.BreakpointDelete(bp0.GetID())
    bps = {target.BreakpointCreateByAddress(base + off).GetID(): off for off in (READ, ALLOC, PARSE, BUILT)}

    def where(a):
        return f'main+{a - base:#x}' if base <= a < base + size else (f'{a:#x}' if a else '')

    pending, tables, roots, t0 = {}, [], {}, time.time()
    while time.time() - t0 < timeout:
        proc.Continue()
        st = proc.GetState()
        if st in (lldb.eStateExited, lldb.eStateCrashed, lldb.eStateDetached): print(f'🔴 host ended early: state {st}'); break
        th = next((proc.GetThreadAtIndex(i) for i in range(proc.GetNumThreads()) if proc.GetThreadAtIndex(i).GetStopReason() == lldb.eStopReasonBreakpoint), None)
        if th is None: continue
        off = bps.get(th.GetStopReasonDataAtIndex(0))
        fr = th.GetFrameAtIndex(0)
        x = [fr.FindRegister(f'x{i}').GetValueAsUnsigned() for i in range(7)]
        lr = fr.FindRegister('lr').GetValueAsUnsigned()
        if off == READ:   # 列描述只在这次调用期间活着（调用方栈上）：入口就整份读走 | the column array lives only during this call (on the caller's stack): read it all at entry
            t = {'name': cstr(x[1]), 'flags': x[0] & 0xffffffff, 'rec_size': x[5] & 0xffffffff, 'caller': where(lr - 4), 'columns': [], 'rows': None, 'records': 0}
            for k in range(MAX_COLS):
                e = rd(x[4] + k * ENTRY, ENTRY)
                if not e: break
                p, kind, ln, fo, cb = struct.unpack('<QIIQQ', e)
                if kind == 0: break
                t['columns'].append({'name': cstr(p, 64), 'kind': kind, 'type': KINDS.get(kind, (0, '?'))[1], 'len': ln, 'offset': fo, 'callback': where(cb)})
            tables.append(t); pending[th.GetThreadID()] = t
        elif off in (ALLOC, PARSE):
            t = pending.pop(th.GetThreadID(), None)
            rows = fr.FindRegister('x19').GetValueAsUnsigned() if off == ALLOC else x[3]
            if t is not None: t.update(rows=rows & 0xffffffff, records=x[0] if off == ALLOC else x[2], path='bin' if off == ALLOC else 'txt')
        elif off == BUILT:
            print(f'loader returned after {len(tables)} tables; searching for the record arrays')
            want = {t['records']: t for t in tables if t['records']}
            regions = [('main', base, size)] + [(f'root{i}', u(base + r), ROOT_SZ) for i, r in enumerate(ROOTS) if u(base + r)]
            for rname, rb, rs in regions:
                for c in range(0, rs, 0x100000):
                    blk = rd(rb + c, min(0x100000, rs - c))
                    if not blk: continue
                    for q in range(0, len(blk) - 7, 8):
                        v = int.from_bytes(blk[q:q + 8], 'little')
                        if v in want:
                            nxt = int.from_bytes(blk[q + 8:q + 12], 'little') if q + 12 <= len(blk) else 0
                            prv = int.from_bytes(blk[q - 4:q], 'little') if q >= 4 else 0
                            want[v].setdefault('slots', []).append({'at': f'{rname}+{c + q:#x}', 'next_u32': nxt, 'prev_u32': prv})
            roots = {f'root{i}': f'main+{r:#x}' for i, r in enumerate(ROOTS)}
            # 查找类的列，+0x18 指向表根对象里那张被查的表的索引对象：换成「根+偏移」（seed 再对到表名）
            # For lookup columns, +0x18 points at the index object of the looked-up table inside a root object: turn it into "root+offset" (seed maps it to the table name)
            rbs = [(i, u(base + r)) for i, r in enumerate(ROOTS) if u(base + r)]
            for t in tables:
                for c in t['columns']:
                    if c['callback'].startswith('0x'):
                        a = int(c['callback'], 16); c['callback'] = next((f'root{i}+{a - rb:#x}' for i, rb in rbs if 0 <= a - rb < ROOT_SZ), 'heap')
            # 表根对象里其余的「指针 + 个数」对（合并后的表，如三张物品表拼成的一张，不在读表记录里）：留偏移、个数与指向的那块的记录 id 线索
            # The other "pointer + count" pairs in the root objects (merged tables, e.g. the one built from the three item tables, are not among the loads): keep the offset and count
            known = {s['at'] for t in tables for s in t.get('slots', [])}
            for i, r in enumerate(ROOTS):
                rb = u(base + r); blk = rd(rb, ROOT_SZ) if rb else None
                if not blk: continue
                roots[f'root{i}_ptr'] = f'{rb:#x}'
                roots[f'root{i}_other'] = [{'off': f'{q:#x}', 'count': int.from_bytes(blk[q + 8:q + 12], 'little')} for q in range(0, ROOT_SZ - 12, 8)
                                           if f'root{i}+{q:#x}' not in known and 0x100000000 < int.from_bytes(blk[q:q + 8], 'little') < 0x800000000000 and 0 < int.from_bytes(blk[q + 8:q + 12], 'little') < 100000]
            break
    else:
        print('🔴 timed out before the loader returned')
    proc.Kill()
    for t in tables: t['records'] = f'{t["records"]:#x}' if t.get('records') else ''   # 运行时地址只作参考 | runtime addresses are for reference only
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({'roots': roots, 'tables': tables}, ensure_ascii=False, indent=1))
    hung = sum(1 for t in tables if t.get('slots'))
    if tables and roots: sys.path.insert(0, str(HERE)); import dbprofile; dbprofile.stamp('tables', ('tables.py',))   # what this step used (re.sh profile)
    print(f'TABLES {"OK" if tables and roots else "RED"} {len(tables)} tables, {sum(len(t["columns"]) for t in tables)} columns, {hung} located → {OUT}')


def dmtables(debugger, command, result, internal_dict):   # noqa: ARG001
    a = argparse.ArgumentParser(prog='dmtables'); a.add_argument('--timeout', type=float, default=240)
    capture(debugger, a.parse_args(command.split()).timeout)


def __lldb_init_module(debugger, internal_dict):   # noqa: ARG001
    debugger.HandleCommand(f'command script add -o -f {Path(__file__).stem}.dmtables dmtables')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('--timeout', type=float, default=240)
    o = ap.parse_args()
    if subprocess.run(['bash', str(HERE / 're.sh'), 'status'], capture_output=True, text=True, check=False).stdout.startswith('serving'):
        sys.exit('🔴 the query server is up: stop it first (re.sh stop) — never run the game next to it')
    sys.exit(subprocess.call(['xcrun', 'lldb', '--batch', '-o', f'command script import {__file__}', '-o', f'dmtables --timeout {o.timeout}']))
