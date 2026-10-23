#!/usr/bin/env python3
# 事件流（DM_EVENTS，dm/verify/event_stream.md）的读取与判定：既读 NDJSON 事件文件，也读夹着「[dm] EV {json}」行的日志（DM_EVENTS=log、日志端口收下来的那种）。
#   summary <文件>                 时间线（里程碑各在第几秒）、各类计数、红 / 致命 / 崩溃明细；末行 RESULT {json}
#   expect <文件> <事件>… [--no-red] [--within 秒]
#                                  按给定顺序依次出现（后一个在前一个之后）；--no-red 时有 red / fatal / crash / watchdog 即红；--within = 最后一个的 t 上限
#                                  打 EVENTS OK|RED 判定行（dmtest.py 认得），退出码 0 / 1
#   why <文件>                     一行说清为什么红：第一个 crash / fatal / watchdog，没有就第一个 red；都没有不打印、退出码 1
#   follow <文件> <事件> [--timeout 秒]
#                                  文件边写边读，等到这个事件出现退出码 0；先见 crash / fatal / watchdog 退出码 2；超时 1
# 作为模块：load(路径或文本) → 事件列表；parse_line(行) → 事件或 None
# Reading and judging the event stream (DM_EVENTS, dm/verify/event_stream.md): reads an NDJSON event file as well as a log with "[dm] EV {json}" lines mixed in (DM_EVENTS=log, as collected from the log port).
#   summary <file>                 timeline (which second each milestone came at), counts per type, red / fatal / crash details; last line RESULT {json}
#   expect <file> <event>... [--no-red] [--within SEC]
#                                  the events appear in the given order (each after the previous one); with --no-red any red / fatal / crash / watchdog is red; --within = upper bound on t of the last one
#                                  prints an EVENTS OK|RED verdict line (dmtest.py understands it), exit code 0 / 1
#   why <file>                     one line saying why it went red: the first crash / fatal / watchdog, else the first red; prints nothing and exits 1 when there is none
#   follow <file> <event> [--timeout SEC]
#                                  reads the file while it is being written, exit code 0 once the event appears; 2 if crash / fatal / watchdog comes first; 1 on timeout
# As a module: load(path or text) -> list of events; parse_line(line) -> event or None
import argparse
import collections
import json
import os
import sys
import time

MILESTONES = ('main_menu', 'game_enter', 'loading_done', 'pause_open', 'game_leave', 'quit', 'shutdown', 'exit')
BAD = ('crash', 'fatal', 'watchdog')


def parse_line(line):
    s = line.strip()
    i = s.find('[dm] EV {')
    if i >= 0: s = s[i + 8:]
    if not s.startswith('{'): return None
    try: e = json.loads(s)
    except ValueError: return None
    return e if isinstance(e, dict) and 'ev' in e else None


def load(src):
    if '\n' in src or not os.path.exists(src): text = src
    else:
        with open(src, encoding='utf-8', errors='replace') as f: text = f.read()
    return [e for e in map(parse_line, text.splitlines()) if e]


def why(events):
    e = next((e for e in events if e['ev'] in BAD), None) or next((e for e in events if e['ev'] == 'red'), None)
    if not e: return ''
    if e['ev'] == 'crash': return f"crash sig={e.get('sig')} at {e.get('where') or e.get('pc')} (t={e['t']})"
    if e['ev'] == 'watchdog': return f"watchdog: stalled {e.get('stalled_s')} s at {e.get('stage')} (t={e['t']})"
    return f"{e['ev']} {e.get('code', '')} {e.get('src', '')}: {e.get('msg', '')} (t={e['t']})"


def summary(events):
    """返回 (里程碑首次出现的秒数, 各类计数, 前 20 个坏事件)。 | Returns (seconds of each milestone's first appearance, counts per type, the first 20 bad events)."""
    first: dict[str, float] = {}
    for e in events:
        if e['ev'] in MILESTONES and e['ev'] not in first: first[e['ev']] = e['t']
    return first, dict(collections.Counter(e['ev'] for e in events)), [e for e in events if e['ev'] in BAD or e['ev'] == 'red'][:20]


def expect(events, want, no_red, within):
    """返回 (是否通过, 说明)。 | Returns (passed, explanation)."""
    at, last_seq = [], -1
    for w in want:
        e = next((e for e in events if e['ev'] == w and e['seq'] > last_seq), None)
        if not e: return False, f"missing {w}" + (f" after {want[len(at) - 1]}" if at else '') + (f"; {r}" if (r := why(events)) else '')
        at.append((w, e['t'])); last_seq = e['seq']
    if within and at and at[-1][1] > within: return False, f"{at[-1][0]} at {at[-1][1]} s > {within:g} s"
    if no_red and (w := why(events)): return False, w
    return True, ' '.join(f'{w}@{t:g}s' for w, t in at)


def follow(path, want, timeout):
    end, pos, buf = time.time() + timeout, 0, ''
    while time.time() < end:
        try:
            with open(path, encoding='utf-8', errors='replace') as f: f.seek(pos); chunk = f.read(); pos = f.tell()
        except OSError: chunk = ''
        buf += chunk
        *lines, buf = buf.split('\n')
        for line in lines:
            e = parse_line(line)
            if not e: continue
            if e['ev'] == want: print(json.dumps(e, ensure_ascii=False)); return 0
            if e['ev'] in BAD: print(json.dumps(e, ensure_ascii=False)); return 2
        if not chunk: time.sleep(0.2)
    return 1


def main(argv):
    ap = argparse.ArgumentParser(prog='dmevents.py', description='Read and check DM_EVENTS streams (dm/verify/event_stream.md).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('summary'); p.add_argument('file')
    p = sub.add_parser('expect'); p.add_argument('file'); p.add_argument('events', nargs='+'); p.add_argument('--no-red', action='store_true'); p.add_argument('--within', type=float, default=0)
    p = sub.add_parser('why'); p.add_argument('file')
    p = sub.add_parser('follow'); p.add_argument('file'); p.add_argument('event'); p.add_argument('--timeout', type=float, default=60)
    a = ap.parse_args(argv)
    if a.cmd == 'follow': return follow(a.file, a.event, a.timeout)
    events = load(a.file)
    if a.cmd == 'why':
        w = why(events)
        if w: print(w)
        return 0 if w else 1
    if a.cmd == 'summary':
        first, counts, bad = summary(events)
        print(f"事件 | events: {len(events)}  " + '  '.join(f'{k}@{v:g}s' for k, v in first.items()))
        print('计数 | counts: ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))
        for e in bad: print('  ' + json.dumps(e, ensure_ascii=False))
        print('RESULT ' + json.dumps(dict(name='events', events=len(events), milestones=first, counts=counts, bad=bad), ensure_ascii=False))
        return 0
    ok, detail = expect(events, a.events, a.no_red, a.within)
    print(f"EVENTS {'OK' if ok else 'RED'} {detail}")
    print('RESULT ' + json.dumps(dict(name='events', verdict='OK' if ok else 'RED', detail=detail, want=a.events), ensure_ascii=False))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
