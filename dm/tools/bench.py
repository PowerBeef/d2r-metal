#!/usr/bin/env python3
# 性能基准：按固定路线（dm/verify/bench/*.scn，场景格式，mark 切段）跑 N 遍，逐帧上屏间隔（DM_FRAMES）按段出统计，写 bench.json；A/B 交替跑并做显著性判断，结论记进账本。
#   run <路线> [--backend cli|mac-app] [--reps 3] [--env K=V …] [--host 宿主可执行文件] [--out bench.json] [--cool 秒]
#   ab  <路线> [--a-env "K=V …"] [--b-env "K=V …"] [--a-host 路径] [--b-host 路径] [--reps 6] [--metric nvn_ms.p50|gpu_ms.p50|cpu_ms.p95|present.p95_ms|…] [--out ab.json]
#        A、B 按 ABBA 交替（抵消发热 / 频率漂移），每段按每遍的指标做 Mann-Whitney U（小样本精确）与 B−A 中位数差的自助法 95% 区间；
#        区间整个在 0 一侧且 p < 0.05 才算有差别；结论追加到 $DM_ROOT/run/bench/ledger.tsv
#   show <bench.json>    打印各段摘要
#   每遍之前看温控（Mac：pmset -g therm；过热就等，最多 --cool 秒）与游戏模式（gamepolicyctl），都记进结果；末行 RESULT {json}
# Performance benchmark: runs a fixed route (dm/verify/bench/*.scn, scenario format, segments cut by mark) N times, reports per-segment statistics of the per-frame present intervals (DM_FRAMES), writes bench.json; A/B runs interleaved with a significance judgement, the conclusion recorded in a ledger.
#   run <route> [--backend cli|mac-app] [--reps 3] [--env K=V ...] [--host host executable] [--out bench.json] [--cool seconds]
#   ab  <route> [--a-env "K=V ..."] [--b-env "K=V ..."] [--a-host path] [--b-host path] [--reps 6] [--metric nvn_ms.p50|gpu_ms.p50|cpu_ms.p95|present.p95_ms|...] [--out ab.json]
#        A and B alternate as ABBA (cancelling thermal / clock drift); per segment, a Mann-Whitney U on the per-run metric (exact for small samples) and a bootstrap 95% interval of the B-A median difference;
#        a difference counts only when the interval lies entirely on one side of 0 and p < 0.05; the conclusion is appended to $DM_ROOT/run/bench/ledger.tsv
#   show <bench.json>    print the per-segment summary
#   Before each run the thermal state (Mac: pmset -g therm; wait while hot, at most --cool seconds) and Game Mode (gamepolicyctl) are checked and recorded in the result; last line RESULT {json}
import argparse
import datetime
import itertools
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
import sys
import time
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmevents  # noqa: E402
import scenario  # noqa: E402

DM_ROOT = scenario.DM_ROOT
PRESENT = ('mean_ms', 'p50_ms', 'p95_ms', 'p99_ms', 'max_ms', 'slow_share', 'fps')
# 逐帧计时（DM_NVN_TSV 的 @ 行，hle_nvn.c）：帧间隔 / 出帧工作 / NVN 调用 / GPU / 翻译录制 / 翻译提交 / 出帧线程 CPU / 取图等待。苹果上帧率常被垂直同步钉死，这些不受刷新率量化，A/B 主要看它们
# Per-frame timing (the @ rows of DM_NVN_TSV, hle_nvn.c): frame interval / frame work / NVN calls / GPU / translation record / translation submit / frame-thread CPU / image-acquire wait. On Apple platforms the frame rate is often pinned by vsync; these are not quantized by the refresh rate, and A/B mainly looks at them
SERIES = ('frame_ms', 'work_ms', 'nvn_ms', 'gpu_ms', 'rec_ms', 'submit_ms', 'cpu_ms', 'wait_ms')
STATS = ('mean', 'p50', 'p95', 'p99')
METRICS = tuple(f'present.{m}' for m in PRESENT) + tuple(f'{s}.{t}' for s in SERIES for t in STATS)
SLOW_MS = 20.8   # 超过这个算慢帧（60 Hz 下两个刷新以上，同 perf.txt 的档） | above this a frame is slow (more than two refreshes at 60 Hz, as the perf.txt buckets)


# ---------- 统计 | Statistics ----------
def pct(xs, q):
    if not xs: return float('nan')
    s = sorted(xs); k = (len(s) - 1) * q / 100; lo = math.floor(k); hi = math.ceil(k)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def seg_stats(ivs):
    """一段的逐帧上屏间隔 → 统计。0 = 没有读数（不算），负数 = 被系统丢弃。 | Per-frame present intervals of one segment -> statistics. 0 = no reading (ignored), negative = dropped by the system."""
    v = [x for x in ivs if x > 0]; dropped = sum(1 for x in ivs if x < 0)
    if not v: return dict(frames=0, dropped=dropped)
    mean = statistics.fmean(v)
    return dict(frames=len(v), dropped=dropped, mean_ms=round(mean, 3), p50_ms=round(pct(v, 50), 3), p95_ms=round(pct(v, 95), 3), p99_ms=round(pct(v, 99), 3),
                max_ms=round(max(v), 3), slow_share=round(sum(1 for x in v if x > SLOW_MS) / len(v), 4), fps=round(1000 / mean, 2))


def mann_whitney(a, b):
    """双侧 Mann-Whitney U：返回 (U, p)。组合数 ≤ 20000 时对合并样本做精确置换（并列自然处理），否则正态近似（带并列校正）。 | Two-sided Mann-Whitney U: returns (U, p). Exact permutation over the pooled sample when the number of combinations is ≤ 20000 (ties handled naturally), otherwise the normal approximation (with tie correction)."""
    n1, n2 = len(a), len(b)
    if not n1 or not n2: return float('nan'), 1.0
    pooled = list(a) + list(b)
    order = sorted(range(len(pooled)), key=lambda i: pooled[i]); ranks = [0.0] * len(pooled); i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and pooled[order[j + 1]] == pooled[order[i]]: j += 1
        for k in range(i, j + 1): ranks[order[k]] = (i + j) / 2 + 1
        i = j + 1
    def u_of(idx): r = sum(ranks[k] for k in idx); return r - n1 * (n1 + 1) / 2
    u = u_of(range(n1)); mu = n1 * n2 / 2
    if math.comb(n1 + n2, n1) <= 20000:
        dev = abs(u - mu); hits = total = 0
        for idx in itertools.combinations(range(n1 + n2), n1):
            total += 1; hits += abs(u_of(idx) - mu) >= dev - 1e-9
        return u, hits / total
    ties = {}
    for x in pooled: ties[x] = ties.get(x, 0) + 1
    n = n1 + n2; var = n1 * n2 / 12 * ((n + 1) - sum(t ** 3 - t for t in ties.values()) / (n * (n - 1)))
    if var <= 0: return u, 1.0
    z = (abs(u - mu) - 0.5) / math.sqrt(var)
    return u, math.erfc(z / math.sqrt(2))


def boot_median_diff(a, b, rounds=4000, seed=7):
    """B − A 中位数差与自助法 95% 区间（各组各自重抽样，固定种子）。 | B - A median difference with a bootstrap 95% interval (each group resampled separately, fixed seed)."""
    rnd = random.Random(seed)
    d = statistics.median(b) - statistics.median(a)
    ds = sorted(statistics.median(rnd.choices(b, k=len(b))) - statistics.median(rnd.choices(a, k=len(a))) for _ in range(rounds))
    return d, ds[int(0.025 * rounds)], ds[int(0.975 * rounds) - 1]


# ---------- 环境 | Environment ----------
def sh(*cmd):
    try: return subprocess.run(cmd, capture_output=True, text=True, timeout=20, check=False).stdout.strip()
    except (OSError, subprocess.SubprocessError): return ''


def thermal():
    """Mac：pmset -g therm 的告警级别与 CPU 限速；nominal = 没有告警且不限速。 | Mac: the warning level and CPU speed limit from pmset -g therm; nominal = no warning and no throttling."""
    t = sh('pmset', '-g', 'therm')
    lim = re.search(r'CPU_Speed_Limit\s*=\s*(\d+)', t)
    warn = 'No thermal warning level has been recorded' not in t and re.search(r'thermal warning level', t, re.I) is not None
    limit = int(lim.group(1)) if lim else 100
    return dict(nominal=not warn and limit >= 100, cpu_speed_limit=limit, warning=warn)


def environment() -> dict[str, Any]:
    rev = sh('git', '-C', scenario.DM, 'rev-parse', '--short=12', 'HEAD'); dirty = bool(sh('git', '-C', scenario.DM, 'status', '--porcelain', '--untracked-files=no'))
    return dict(git=dict(rev=rev, dirty=dirty), model=sh('sysctl', '-n', 'hw.model'), os=f"{platform.system()} {platform.mac_ver()[0]}",
                cpu=sh('sysctl', '-n', 'machdep.cpu.brand_string'), game_mode=re.sub(r'\x1b\[[0-9;]*m', '', sh('xcrun', 'gamepolicyctl', 'game-mode', 'status')).splitlines()[:2])


def wait_cool(limit_s):
    t0 = time.time(); th = thermal()
    while not th['nominal'] and time.time() - t0 < limit_s:
        print(f"  温控未回落，等 10 秒 | thermal not nominal ({th}), waiting 10 s", flush=True); time.sleep(10); th = thermal()
    return th, round(time.time() - t0, 1)


# ---------- 跑一遍 | One run ----------
def series_stats(v):
    v = [x for x in v if x >= 0]
    return dict(mean=round(statistics.fmean(v), 4), p50=round(pct(v, 50), 4), p95=round(pct(v, 95), 4), p99=round(pct(v, 99), 4)) if v else {}


def nvn_rows(path):
    """DM_NVN_TSV 的 @ 计时行 → {帧号: {指标: 值}}。 | The @ timing rows of DM_NVN_TSV -> {frame: {metric: value}}."""
    out: dict[int, dict[str, float]] = {}
    if not os.path.exists(path): return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) != 3 or not p[1].startswith('@'): continue
            try: out.setdefault(int(p[0]), {})[p[1][1:]] = float(p[2])
            except ValueError: pass
    return out


def segments(run_dir):
    evs = dmevents.load(os.path.join(run_dir, 'events.ndjson')) if os.path.exists(os.path.join(run_dir, 'events.ndjson')) else []
    marks = [(e['t'], e.get('name'), e.get('frame')) for e in evs if e['ev'] == 'mark']
    nv = nvn_rows(os.path.join(run_dir, 'nvn.tsv'))
    rows = []
    fp = os.path.join(run_dir, 'frames.tsv')
    if os.path.exists(fp):
        with open(fp, encoding='utf-8') as f:
            next(f, None)
            for line in f:
                p = line.split('\t')
                if len(p) == 2:
                    try: rows.append((float(p[0]), float(p[1])))
                    except ValueError: pass
    out = {}
    for (t0, name, f0), (t1, _, f1) in zip(marks, marks[1:], strict=False):
        if name == 'end': continue
        seg: dict[str, Any] = dict(seconds=round(t1 - t0, 3), present=seg_stats([iv for t, iv in rows if t0 <= t < t1]))
        if f0 is not None and f1 is not None:
            frames = [nv[f] for f in range(f0, f1) if f in nv]
            seg['frames'] = len(frames)
            for sname in SERIES:
                st = series_stats([fr[sname] for fr in frames if sname in fr])
                if st: seg[sname] = st
        out[name] = seg
    return out


def metric(seg, path):
    """「序列.统计」取值（present.p95_ms、work_ms.p95 …），没有返回 None。 | Value of "series.stat" (present.p95_ms, work_ms.p95 ...), None when absent."""
    a, b = path.split('.', 1)
    v = seg.get(a, {}).get(b) if isinstance(seg.get(a), dict) else None
    return v if isinstance(v, (int, float)) else None


def run_once(route, backend, out_dir, env, host, cool):
    th0, waited = wait_cool(cool)
    saved = {k: os.environ.get(k) for k in list(env) + ['DM_FRAMES', 'DM_STATEREP', 'SCN_DMHOST', 'DM_NVN_TSV', 'DM_NVN_TSV_CALLS']}
    os.environ.update(env); os.environ['DM_FRAMES'] = os.path.join(out_dir, 'frames.tsv'); os.environ['DM_STATEREP'] = '1'
    os.environ['DM_NVN_TSV'] = os.path.join(out_dir, 'nvn.tsv'); os.environ['DM_NVN_TSV_CALLS'] = '0'
    if host: os.environ['SCN_DMHOST'] = host
    try:
        os.makedirs(out_dir, exist_ok=True)
        res, code = scenario.Runner(route, backend, out_dir, False).go()
    finally:
        for k, v in saved.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
    return dict(ok=code == 0, reason=res.get('reason'), thermal_before=th0, cooled_s=waited, thermal_after=thermal(), segments=segments(out_dir), dir=out_dir)


def summarize(reps):
    """每段每个指标取各遍的中位数。 | Per segment and metric, the median over the runs."""
    names = sorted({n for r in reps if r['ok'] for n in r['segments']})
    out: dict[str, Any] = {}
    for n in names:
        segs = [r['segments'][n] for r in reps if r['ok'] and n in r['segments']]
        row: dict[str, Any] = dict(runs=len(segs))
        for m in METRICS:
            vals = [x for x in (metric(sg, m) for sg in segs) if x is not None]
            if vals: row[m] = round(statistics.median(vals), 4)
        out[n] = row
    return out


def parse_env(items):
    out = {}
    for it in items or []:
        for kv in it.split():
            if '=' in kv: k, v = kv.split('=', 1); out[k] = v
    return out


def cmd_run(a):
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S'); name = os.path.splitext(os.path.basename(a.route))[0]
    base = os.path.join(DM_ROOT, 'run', 'bench', f'{name}_{stamp}')
    env = parse_env(a.env); reps = []
    for i in range(a.reps):
        print(f'== 第 {i + 1}/{a.reps} 遍 | run {i + 1}/{a.reps}', flush=True)
        reps.append(run_once(a.route, a.backend, os.path.join(base, f'r{i + 1}'), env, a.host, a.cool))
    res = dict(schema='bench/1', route=name, backend=a.backend, started=stamp, env=env, host=a.host, environment=environment(), reps=reps, summary=summarize(reps))
    out = a.out or os.path.join(base, 'bench.json')
    with open(out, 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1)
    show(res)
    ok = all(r['ok'] for r in reps)
    print(f"BENCH {'OK' if ok else 'RED'} {name} {sum(r['ok'] for r in reps)}/{len(reps)} 遍 | runs → {out}")
    print('RESULT ' + json.dumps(dict(name='bench', verdict='OK' if ok else 'RED', route=name, summary=res['summary'], file=out), ensure_ascii=False))
    return 0 if ok else 1


def show(res):
    nan = float('nan')
    for n, s in res['summary'].items():
        print(f"  {n:<10} {s.get('runs', 0)} 遍 | runs  {s.get('present.fps', nan):6.2f} fps  上屏 | present p95 {s.get('present.p95_ms', nan):6.2f} ms  slow {100 * s.get('present.slow_share', 0):.1f}%"
              f"  CPU p50 {s.get('cpu_ms.p50', nan):5.2f} p95 {s.get('cpu_ms.p95', nan):5.2f}  NVN p50 {s.get('nvn_ms.p50', nan):5.2f} p95 {s.get('nvn_ms.p95', nan):5.2f}  GPU p50 {s.get('gpu_ms.p50', nan):5.2f} p95 {s.get('gpu_ms.p95', nan):5.2f} ms")


def cmd_ab(a):
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S'); name = os.path.splitext(os.path.basename(a.route))[0]
    base = os.path.join(DM_ROOT, 'run', 'bench', f'{name}_ab_{stamp}')
    var = {'A': (parse_env([a.a_env]), a.a_host), 'B': (parse_env([a.b_env]), a.b_host)}
    order = ''.join('ABBA' for _ in range(math.ceil(a.reps / 2)))[:2 * a.reps]   # 每个变体 reps 遍 | reps runs per variant
    reps = {'A': [], 'B': []}
    for i, v in enumerate(order, 1):
        print(f'== {i}/{len(order)} 变体 | variant {v}', flush=True)
        env, host = var[v]; reps[v].append(run_once(a.route, a.backend, os.path.join(base, f'{i:02d}{v}'), env, host, a.cool))
    rows = []
    segs = sorted({n for v in reps.values() for r in v if r['ok'] for n in r['segments']})
    for n in segs:
        va = [x for x in (metric(r['segments'].get(n, {}), a.metric) for r in reps['A'] if r['ok']) if x is not None]
        vb = [x for x in (metric(r['segments'].get(n, {}), a.metric) for r in reps['B'] if r['ok']) if x is not None]
        if len(va) < 2 or len(vb) < 2: rows.append(dict(segment=n, verdict='not enough runs')); continue
        u, p = mann_whitney(va, vb); d, lo, hi = boot_median_diff(va, vb)
        worse = not a.metric.endswith('.fps')   # 除了 fps，越大越差 | except fps, larger is worse
        sig = p < 0.05 and (lo > 0 or hi < 0)
        verdict = 'no difference' if not sig else ('B slower' if (d > 0) == worse else 'B faster')
        rows.append(dict(segment=n, metric=a.metric, a=va, b=vb, median_a=statistics.median(va), median_b=statistics.median(vb), diff=round(d, 4), ci95=[round(lo, 4), round(hi, 4)], u=u, p=round(p, 4), verdict=verdict))
        print(f"  {n:<10} {a.metric}: A {statistics.median(va):.3f} B {statistics.median(vb):.3f}  B−A {d:+.3f} [{lo:+.3f}, {hi:+.3f}]  p {p:.3f}  → {verdict}")
    envinfo = environment()
    res = dict(schema='bench-ab/1', route=name, backend=a.backend, started=stamp, order=order, a=dict(env=var['A'][0], host=a.a_host), b=dict(env=var['B'][0], host=a.b_host),
               environment=envinfo, rows=rows, reps=reps)
    out = a.out or os.path.join(base, 'ab.json')
    with open(out, 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1)
    ledger = os.path.join(DM_ROOT, 'run', 'bench', 'ledger.tsv'); os.makedirs(os.path.dirname(ledger), exist_ok=True)
    new = not os.path.exists(ledger)
    with open(ledger, 'a', encoding='utf-8') as f:
        if new: f.write('时刻 | time\t提交 | rev\t路线 | route\t段 | segment\t指标 | metric\tA\tB\t每组遍数 | runs\tA 中位 | median A\tB 中位 | median B\tB−A\t区间 | CI95\tp\t结论 | verdict\n')
        rev = envinfo['git']['rev']
        for r in rows:
            if 'diff' not in r: continue
            f.write(f"{stamp}\t{rev}\t{name}\t{r['segment']}\t{a.metric}\t{a.a_env or a.a_host or '-'}\t{a.b_env or a.b_host or '-'}\t{len(r['a'])}/{len(r['b'])}\t{r['median_a']:.4f}\t{r['median_b']:.4f}\t{r['diff']:+.4f}\t[{r['ci95'][0]:+.4f}, {r['ci95'][1]:+.4f}]\t{r['p']:.4f}\t{r['verdict']}\n")
    ok = all(r['ok'] for v in reps.values() for r in v)
    print(f"BENCH-AB {'OK' if ok else 'RED'} {name} → {out}（账本 | ledger {ledger}）")
    print('RESULT ' + json.dumps(dict(name='bench-ab', verdict='OK' if ok else 'RED', route=name, rows=[{k: r.get(k) for k in ('segment', 'diff', 'ci95', 'p', 'verdict')} for r in rows], file=out), ensure_ascii=False))
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(prog='bench.py', description='Fixed-route benchmarks over the control port (dm/verify/bench).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run'); r.add_argument('route'); r.add_argument('--backend', default='cli', choices=('cli', 'mac-app')); r.add_argument('--reps', type=int, default=3)
    r.add_argument('--env', action='append'); r.add_argument('--host'); r.add_argument('--out'); r.add_argument('--cool', type=float, default=120)
    b = sub.add_parser('ab'); b.add_argument('route'); b.add_argument('--backend', default='cli', choices=('cli', 'mac-app')); b.add_argument('--reps', type=int, default=6)
    b.add_argument('--a-env', default=''); b.add_argument('--b-env', default=''); b.add_argument('--a-host'); b.add_argument('--b-host'); b.add_argument('--metric', default='nvn_ms.p50', choices=METRICS)
    b.add_argument('--out'); b.add_argument('--cool', type=float, default=120)
    s = sub.add_parser('show'); s.add_argument('file')
    a = ap.parse_args(argv)
    if a.cmd == 'run': return cmd_run(a)
    if a.cmd == 'ab': return cmd_ab(a)
    with open(a.file, encoding='utf-8') as f: show(json.load(f))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
