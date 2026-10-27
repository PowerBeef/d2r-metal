#!/usr/bin/env python3
# 客户端命令包（DM_C2S_LOG / replay.py 录的 c2s.jsonl，格式见 dm/verify/command_packet_format.md）的可读视图：包号 → 人话标签与长度规则，
#   标签表是 dm/verify/c2s_opcodes.tsv（我们自己的话；长度 = 处理函数实际读到 / 校验的，var = 变长，? = 未知）。
#   c2s.py summary <c2s.jsonl|录制目录>             每个包号的条数、标签、见到的长度；固定长度对不上的标红
#   c2s.py near <c2s.jsonl|录制目录> <拍> [--span N]  拍 [拍−N, 拍] 里的包（replay.py 报第一个不同的拍时用它列现场）
#   判定行 C2S OK|RED，末行 RESULT {json}。作为模块：label(包号, 系统类) / summary(行) / near(行, 拍, 跨度)
# Readable view of client command packets (c2s.jsonl from DM_C2S_LOG / replay.py, format in dm/verify/command_packet_format.md): packet number -> plain-language label and length rule;
#   the label table is dm/verify/c2s_opcodes.tsv (our own words; length = what the handler actually reads / checks, var = variable, ? = unknown).
#   c2s.py summary <c2s.jsonl|recording dir>             count, label and lengths seen per packet number; a fixed length that does not match is red
#   c2s.py near <c2s.jsonl|recording dir> <tick> [--span N]  the packets in ticks [tick-N, tick] (replay.py lists them when it reports the first divergent tick)
#   Verdict line C2S OK|RED, last line RESULT {json}. As a module: label(number, system) / summary(rows) / near(rows, tick, span)
import argparse
import collections
import json
import os
import sys

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'verify', 'c2s_opcodes.tsv')


def table():
    """{(类, 包号): (标签, 长度规则)}；类 = 'cmd'（命令包）或 'sys'（系统类消息）。 | {(class, number): (label, length rule)}; class = 'cmd' (command packet) or 'sys' (system-class message)."""
    out = {}
    with open(TABLE, encoding='utf-8') as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip() or ln.startswith('class\t'): continue
            k, op, lab, ln_rule = (ln.rstrip('\n').split('\t') + ['', '', '', ''])[:4]
            out[(k, int(op, 16))] = (lab, ln_rule or '?')
    return out


def label(op, sys_class=False, t=None):
    t = t if t is not None else table()
    return t.get(('sys' if sys_class else 'cmd', op), (f'unknown {op:#04x}', '?'))[0]


def load(path):
    if os.path.isdir(path): path = os.path.join(path, 'c2s.jsonl')
    rows = []
    with open(path, encoding='utf-8') as f:
        for ln in f:
            if ln.strip(): rows.append(json.loads(ln))
    return rows


def summary(rows, t=None):
    """[{类, 包号, 条数, 标签, 规则, 长度表, 对不上}]；包号按类与号排序。 | [{class, number, count, label, rule, lengths, mismatch}] sorted by class and number."""
    t = t if t is not None else table()
    seen = collections.defaultdict(collections.Counter)
    for r in rows:
        seen[('sys' if r.get('q') == 'sys' else 'cmd', int(r['pkt'][:2], 16))][r['len']] += 1
    out = []
    for (k, op), lens in sorted(seen.items()):
        lab, rule = t.get((k, op), (f'unknown {op:#04x}', '?'))
        bad = sorted(n for n in lens if rule.isdigit() and n != int(rule))
        out.append(dict(cls=k, op=op, count=sum(lens.values()), label=lab, rule=rule, lengths=dict(sorted(lens.items())), mismatch=bad, known=(k, op) in t))
    return out


def near(rows, tick, span=5, t=None):
    """拍 [tick−span, tick] 里的包，一包一行：「拍 类 包号 标签 长度」。 | The packets in ticks [tick-span, tick], one line each: "tick class number label length"."""
    t = t if t is not None else table()
    out = []
    for r in rows:
        if tick - span <= r['tick'] <= tick:
            k = 'sys' if r.get('q') == 'sys' else 'cmd'; op = int(r['pkt'][:2], 16)
            out.append(f"{r['tick']:>6} {k} {op:#04x} {label(op, k == 'sys', t)} ({r['len']} bytes)")
    return out


def main(argv):
    ap = argparse.ArgumentParser(prog='c2s.py', description='Readable view of recorded client command packets.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('summary'); s.add_argument('file')
    n = sub.add_parser('near'); n.add_argument('file'); n.add_argument('tick', type=int); n.add_argument('--span', type=int, default=5)
    a = ap.parse_args(argv)
    try: rows, t = load(a.file), table()
    except (OSError, ValueError, KeyError) as e:
        print(f'C2S RED {e}'); return 2
    if a.cmd == 'near':
        for line in near(rows, a.tick, a.span, t): print(line)
        print('RESULT ' + json.dumps(dict(name='c2s-near', verdict='OK', tick=a.tick, span=a.span)))
        return 0
    res = summary(rows, t); bad = [r for r in res if r['mismatch']]
    for r in res:
        print(f"{r['cls']} {r['op']:#04x} ×{r['count']:<5} {r['label']:<40} 长度 | length {r['rule']:<4} 见到 | seen {r['lengths']}" + ('  【红】长度对不上 | length mismatch' if r['mismatch'] else '') + ('' if r['known'] else '  （表里没有 | not in the table）'))
    print(f"C2S {'RED' if bad else 'OK'} {len(rows)} 条 | packets，{len(res)} 种 | kinds，{len(bad)} 种长度对不上 | with a length mismatch")
    print('RESULT ' + json.dumps(dict(name='c2s-summary', verdict='RED' if bad else 'OK', packets=len(rows), kinds=len(res), mismatch=[f"{r['cls']}:{r['op']:#04x}" for r in bad])))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
