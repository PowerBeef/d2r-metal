#!/usr/bin/env python3
# 启动时间的历史账本：每次跑（真机 bootprof.sh、基准测试的每一遍、手动记）把启动各阶段到进程起点的秒数追加进 $DM_ROOT/run/bootprof/history.tsv，带 git 提交，跨时间比。
#   来源两种，有哪种记哪种：DM_BOOTPROF 的启动时间线表（⓪–⑥ 分段的累计秒）与事件流（boot / main_menu / game_enter / loading_done 的 t）
#   record <日志或 events.ndjson>… [--label 标签]    记一次（多个文件 = 多次）
#   show [--label 标签] [--last 5]                     每个阶段：最近 N 次的中位数、再往前 N 次的中位数、差；变慢超过 10% 且 50 ms 标「变慢」；末行 RESULT {json}
# History ledger of boot times: every run (device bootprof.sh, each benchmark run, recorded by hand) appends the seconds from process start to each boot stage to $DM_ROOT/run/bootprof/history.tsv, with the git revision, for comparison over time.
#   Two sources, whichever is present: the DM_BOOTPROF boot timeline table (cumulative seconds of segments ⓪–⑥) and the event stream (t of boot / main_menu / game_enter / loading_done)
#   record <log or events.ndjson>... [--label label]   record one run (several files = several runs)
#   show [--label label] [--last 5]                     per stage: median of the latest N runs, median of the N before, and the difference; slower by more than 10% and 50 ms is marked "slower"; last line RESULT {json}
import argparse
import datetime
import json
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmevents  # noqa: E402

DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
HIST = os.path.join(DM_ROOT, 'run', 'bootprof', 'history.tsv')
EV_STAGES = (('boot', 'host_loaded'), ('boot', 'module_inits'), ('boot', 'nnmain'), ('boot', 'front_end'), ('main_menu', None), ('game_enter', None), ('loading_done', None))
BOOTPROF = re.compile(r'\[dm\] ([⓪①②③④⑤⑥]b?) (.+?)\s{2,}([\d.]+)\s+([\d.]+)')


def stages(path):
    """一个文件里的启动阶段 → {阶段名: 进程起点以来的秒}（每阶段取第一次）。 | Boot stages in one file -> {stage: seconds since process start} (the first occurrence of each)."""
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f: text = f.read()
    for m in BOOTPROF.finditer(text):
        out.setdefault(f'prof {m.group(1)}', float(m.group(4)))
    evs = dmevents.load(text)
    for ev, st in EV_STAGES:
        e = next((x for x in evs if x['ev'] == ev and (st is None or x.get('stage') == st)), None)
        if e: out.setdefault(st or ev, float(e['t']))
    return out


def git_rev():
    try:
        rev = subprocess.run(['git', '-C', HERE, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True, timeout=10, check=False).stdout.strip()
        dirty = bool(subprocess.run(['git', '-C', HERE, 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True, timeout=10, check=False).stdout.strip())
        return rev + ('+' if dirty else '')
    except (OSError, subprocess.SubprocessError): return ''


def record(paths, label):
    os.makedirs(os.path.dirname(HIST), exist_ok=True)
    new = not os.path.exists(HIST); n = 0; rev = git_rev(); when = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    with open(HIST, 'a', encoding='utf-8') as f:
        if new: f.write('时刻 | time\t提交 | rev\t标签 | label\t次 | run\t阶段 | stage\t秒 | seconds\n')
        for i, p in enumerate(paths):
            st = stages(p)
            if not st: continue
            run = f'{when}#{i}'; n += 1
            for k, v in st.items(): f.write(f'{when}\t{rev}\t{label}\t{run}\t{k}\t{v:.3f}\n')
    return n


def load():
    rows = []
    if not os.path.exists(HIST): return rows
    with open(HIST, encoding='utf-8') as f:
        next(f, None)
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) == 6:
                try: rows.append(dict(time=p[0], rev=p[1], label=p[2], run=p[3], stage=p[4], s=float(p[5])))
                except ValueError: pass
    return rows


def show(label, last):
    rows = [r for r in load() if not label or r['label'] == label]
    runs = list(dict.fromkeys(r['run'] for r in rows))   # 按记录顺序 | in recording order
    recent, before = runs[-last:], runs[-2 * last:-last]
    out = []
    for stage in dict.fromkeys(r['stage'] for r in rows):
        a = [r['s'] for r in rows if r['stage'] == stage and r['run'] in recent]
        b = [r['s'] for r in rows if r['stage'] == stage and r['run'] in before]
        if not a: continue
        ma = statistics.median(a); mb = statistics.median(b) if b else None
        slower = mb is not None and ma - mb > 0.05 and ma > mb * 1.10
        out.append(dict(stage=stage, recent=round(ma, 3), runs=len(a), before=round(mb, 3) if mb is not None else None, diff=round(ma - mb, 3) if mb is not None else None, slower=slower))
        print(f"  {stage:<16} 最近 | recent {ma:7.3f} s（{len(a)} 次 | runs）" + (f"  之前 | before {mb:7.3f} s  差 | diff {ma - mb:+.3f} s" + ('  ← 变慢 | slower' if slower else '') if mb is not None else ''))
    print(f"BOOTHIST {'RED' if any(o['slower'] for o in out) else 'OK'} {len(runs)} 次记录 | runs recorded" + (f'（标签 | label {label}）' if label else ''))
    print('RESULT ' + json.dumps(dict(name='boothist', verdict='RED' if any(o['slower'] for o in out) else 'OK', label=label, runs=len(runs), stages=out), ensure_ascii=False))
    return 1 if any(o['slower'] for o in out) else 0


def main(argv):
    ap = argparse.ArgumentParser(prog='boothist.py', description='Boot-time history ledger ($DM_ROOT/run/bootprof/history.tsv).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record'); r.add_argument('files', nargs='+'); r.add_argument('--label', default='manual')
    s = sub.add_parser('show'); s.add_argument('--label'); s.add_argument('--last', type=int, default=5)
    a = ap.parse_args(argv)
    if a.cmd == 'record':
        n = record(a.files, a.label)
        print(f'BOOTHIST 记了 {n} 次 | recorded {n} runs → {HIST}')
        return 0 if n else 1
    return show(a.label, a.last)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
