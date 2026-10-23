#!/usr/bin/env python3
# 逐拍状态（DM_TICKS，格式见 dm/verify/tick_state_format.md）的两份对照：找第一个不同的拍，说出哪几格不同；两边都带逐单位明细（DM_TICKS_FULL=1）时再说是哪几个单位、哪些字段。
#   tickdiff.py <A> <B> [--from 拍] [--to 拍] [--units N]   A / B 是 ticks.jsonl 或含它的局目录；比 n / h / bad 三项（game、dt、reads、frames 只给人看，不比）
#   一份里拍号回落（新开一局）就另起一段，两边按段对齐、段内按拍号对齐；只比两边都有的拍
#   判定行 TICKDIFF OK|RED，末行 RESULT {json}；退出码 0 = 相同、1 = 有不同、2 = 读不了 / 没有共同的拍
#   作为模块：compare(a_rows, b_rows) → 结果 dict（explore.py 等用）
# Two-way comparison of per-tick state (DM_TICKS, format in dm/verify/tick_state_format.md): finds the first tick that differs and names the slots that differ; when both sides carry per-unit detail (DM_TICKS_FULL=1) it also names the units and the fields.
#   tickdiff.py <A> <B> [--from tick] [--to tick] [--units N]   A / B are ticks.jsonl files or run folders containing one; compares n / h / bad (game, dt, reads, frames are for people only and not compared)
#   When the tick number falls back within one file (a new game), a new segment starts; the two sides are aligned by segment, and by tick number within a segment; only ticks present on both sides are compared
#   Verdict line TICKDIFF OK|RED, last line RESULT {json}; exit code 0 = identical, 1 = differences, 2 = unreadable / no common ticks
#   As a module: compare(a_rows, b_rows) -> result dict (used by explore.py and others)
import argparse
import json
import os
import sys
from typing import Any

SLOTS = ('player', 'monster', 'object', 'missile', 'item', 'seed')
UNIT_FIELDS = ('cls', 'mode', 'act', 'seed', 'rlo', 'rhi', 'x', 'y', 'xf', 'yf', 'it')


def load(path):
    """[(段号, 拍号, 摘要行, {(type, id): 单位})]：摘要行与其后的逐单位行归在一起。 | [(segment, tick, digest row, {(type, id): unit})]: a digest row together with the per-unit rows after it."""
    if os.path.isdir(path): path = os.path.join(path, 'ticks.jsonl')
    rows, seg, last = [], 0, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line: continue
            try: x = json.loads(line)
            except ValueError as e: raise ValueError(f'{path}:{n}: not JSON ({e})') from e
            if 'tick' in x and 'h' in x:
                if last is not None and x['tick'] <= last: seg += 1   # 拍号回落：新的一局 | tick number fell back: a new game
                last = x['tick']; rows.append((seg, x['tick'], x, {}))
            elif 'u' in x and rows and x.get('t') == rows[-1][1]:
                u = x['u']; rows[-1][3][(u.get('type'), u.get('id'))] = u
    return rows


def _stats(u):
    return {(a >> 16, a & 0xffff): b for a, b in (u.get('st') or [])}


def unit_diff(ua, ub, limit=20):
    """两边单位表的差异，人能读的几行：只在一边的单位、同一单位不同的字段与属性。 | Differences between the two unit tables as readable lines: units on one side only, and the fields and stats that differ for the same unit."""
    out = []
    for key in sorted(set(ua) | set(ub), key=lambda k: (k[0] if k[0] is not None else 99, k[1] if k[1] is not None else 0)):
        t, uid = key; name = SLOTS[t] if isinstance(t, int) and 0 <= t < 5 else f'type {t}'
        a, b = ua.get(key), ub.get(key)
        if a is None or b is None:
            u = a or b or {}
            out.append(f"{'-' if b is None else '+'} {name} id {uid} cls {u.get('cls')} (only in {'A' if b is None else 'B'})")
        else:
            diffs = [f'{f} {a.get(f)}→{b.get(f)}' for f in UNIT_FIELDS if a.get(f) != b.get(f)]
            sa, sb = _stats(a), _stats(b)
            diffs += [f'stat {s}{f"/{layer}" if layer else ""} {sa.get((layer, s))}→{sb.get((layer, s))}' for layer, s in sorted(set(sa) | set(sb)) if sa.get((layer, s)) != sb.get((layer, s))]
            if diffs: out.append(f"  {name} id {uid} cls {a.get('cls')}: " + ', '.join(diffs[:8]) + (f' (+{len(diffs) - 8})' if len(diffs) > 8 else ''))
        if len(out) >= limit: out.append('  …'); break
    return out


def compare(a_rows, b_rows, lo=None, hi=None, units=20) -> dict[str, Any]:
    """对照两份逐拍记录。返回 dict：same、common（共同拍数）、first（第一个不同的拍：段、拍号、哪几格、n 的变化、单位差异）、diverged（之后不同的拍数）、reconverged（之后又全同的第一拍）。 | Compare two per-tick records. Returns a dict: same, common (number of common ticks), first (the first differing tick: segment, tick, slots, the change in n, unit differences), diverged (number of differing ticks from there on), reconverged (the first fully identical tick after that)."""
    bi = {(s, t): (d, u) for s, t, d, u in b_rows}
    common, first, diverged, reconv = 0, None, 0, None
    for s, t, da, ua in a_rows:
        if (lo is not None and t < lo) or (hi is not None and t > hi) or (s, t) not in bi: continue
        db, ub = bi[(s, t)]; common += 1
        slots = [SLOTS[i] for i in range(6) if da['h'][i] != db['h'][i]]
        n_diff = {SLOTS[i]: [da['n'][i], db['n'][i]] for i in range(5) if da['n'][i] != db['n'][i]}
        bad = da.get('bad') != db.get('bad')
        if slots or n_diff or bad:
            diverged += 1
            if first is None:
                first = dict(segment=s, tick=t, slots=slots, n=n_diff, bad=[da.get('bad'), db.get('bad')] if bad else None,
                             units=unit_diff(ua, ub, units) if ua and ub else None)
        elif first is not None and reconv is None: reconv = t
    return dict(same=first is None and common > 0, common=common, first=first, diverged=diverged, reconverged=reconv,
                a_ticks=len(a_rows), b_ticks=len(b_rows))


def main(argv):
    ap = argparse.ArgumentParser(prog='tickdiff.py', description='First divergent tick between two DM_TICKS records (dm/verify/tick_state_format.md).')
    ap.add_argument('a'); ap.add_argument('b'); ap.add_argument('--from', dest='lo', type=int); ap.add_argument('--to', dest='hi', type=int)
    ap.add_argument('--units', type=int, default=20, help='at most this many unit lines at the first divergent tick')
    a = ap.parse_args(argv)
    try: ra, rb = load(a.a), load(a.b)
    except (OSError, ValueError) as e:
        print(f'TICKDIFF RED 读不了 | unreadable: {e}'); print('RESULT ' + json.dumps(dict(name='tickdiff', verdict='RED', error=str(e)), ensure_ascii=False)); return 2
    r = compare(ra, rb, a.lo, a.hi, a.units)
    if not r['common']:
        print(f"TICKDIFF RED 没有共同的拍 | no common ticks (A {r['a_ticks']}, B {r['b_ticks']})")
        print('RESULT ' + json.dumps(dict(name='tickdiff', verdict='RED', **r), ensure_ascii=False)); return 2
    if r['same']:
        print(f"TICKDIFF OK {r['common']} 拍逐拍相同 | ticks identical (A {r['a_ticks']}, B {r['b_ticks']})")
    else:
        f = r['first']; assert f is not None   # 不相同且有共同的拍 ⇒ 一定有第一个不同的拍 | not identical with common ticks => there is a first divergent tick
        print(f"TICKDIFF RED 第一个不同的拍 | first divergent tick {f['tick']}" + (f" (段 | segment {f['segment']})" if f['segment'] else '') + f"：{', '.join(f['slots']) or '-'}"
              + (f"；数目 | counts {f['n']}" if f['n'] else '') + (f"；bad {f['bad'][0]}→{f['bad'][1]}" if f['bad'] else ''))
        print(f"  共同 {r['common']} 拍里有 {r['diverged']} 拍不同 | {r['diverged']} of {r['common']} common ticks differ" + (f"；拍 {r['reconverged']} 起又相同 | identical again from tick {r['reconverged']}" if r['reconverged'] else ''))
        if f['units']: [print(f'  {line}') for line in f['units']]
        elif f['units'] is None: print('  （没有逐单位明细：两边都用 DM_TICKS_FULL=1 记，才能说是哪几个单位 | no per-unit detail: record both sides with DM_TICKS_FULL=1 to name the units)')
    print('RESULT ' + json.dumps(dict(name='tickdiff', verdict='OK' if r['same'] else 'RED', **r), ensure_ascii=False))
    return 0 if r['same'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
