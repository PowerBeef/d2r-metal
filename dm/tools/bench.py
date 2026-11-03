#!/usr/bin/env python3
# 性能基准：按固定路线（dm/verify/bench/*.scn，场景格式，mark 切段）跑 N 遍，逐帧上屏间隔（DM_FRAMES）按段出统计，写 bench.json；A/B 交替跑并做显著性判断，结论记进账本。
#   run <路线> [--backend cli|mac-app] [--reps 3] [--env K=V …] [--host 宿主可执行文件] [--out bench.json] [--cool 秒]
#   ab  <路线> [--a-env "K=V …"] [--b-env "K=V …"] [--a-host 路径] [--b-host 路径] [--reps 6] [--metric nvn_ms.p50|gpu_ms.p50|cpu_ms.p95|present.p95_ms|…] [--out ab.json]
#        A、B 按 ABBA 交替（抵消发热 / 频率漂移），每段按每遍的指标做 Mann-Whitney U（小样本精确）与 B−A 中位数差的自助法 95% 区间；
#        区间整个在 0 一侧且 p < 0.05 才算有差别；结论追加到 $DM_ROOT/run/bench/ledger.tsv
#   show <bench.json>    打印各段摘要
#   profile <路线> [--ms 4] [--hitch-ms 25]   跑一遍并对全部客体线程采样（DM_SAMPLE=<ms>:*）：按段、按线程角色（主线程 / 任务线程 / 读写池 / 音频 / 其他）出在跑占比与最热的游戏函数
#        （名字来自本机的引擎符号表 guestsyms.py），再把超过 --hitch-ms 的帧（nvn.tsv 的帧时间）各自那段时间里在跑的函数列出来；写 profile.json
#   每遍之前看温控（Mac：pmset -g therm；过热就等，最多 --cool 秒）与游戏模式（gamepolicyctl），都记进结果；末行 RESULT {json}
# Performance benchmark: runs a fixed route (dm/verify/bench/*.scn, scenario format, segments cut by mark) N times, reports per-segment statistics of the per-frame present intervals (DM_FRAMES), writes bench.json; A/B runs interleaved with a significance judgement, the conclusion recorded in a ledger.
#   run <route> [--backend cli|mac-app] [--reps 3] [--env K=V ...] [--host host executable] [--out bench.json] [--cool seconds]
#   ab  <route> [--a-env "K=V ..."] [--b-env "K=V ..."] [--a-host path] [--b-host path] [--reps 6] [--metric nvn_ms.p50|gpu_ms.p50|cpu_ms.p95|present.p95_ms|...] [--out ab.json]
#        A and B alternate as ABBA (cancelling thermal / clock drift); per segment, a Mann-Whitney U on the per-run metric (exact for small samples) and a bootstrap 95% interval of the B-A median difference;
#        a difference counts only when the interval lies entirely on one side of 0 and p < 0.05; the conclusion is appended to $DM_ROOT/run/bench/ledger.tsv
#   show <bench.json>    print the per-segment summary
#   profile <route> [--ms 4] [--hitch-ms 25]  one run with all guest threads sampled (DM_SAMPLE=<ms>:*): per segment and thread role (main / job workers / IO pool / audio / other), the running share and the hottest game functions
#        (names from the local engine symbol table, guestsyms.py); then, for every frame longer than --hitch-ms (frame time in nvn.tsv), the functions running during it; writes profile.json
#   baseline save [--host host] [--name N]    keeps the current host (default this tree's dm/host/dmhost) with everything it runs on as $DM_ROOT/run/bench/baseline/<N, default the rev>/:
#        host/dmhost and its tables, the rewrite output it was built against (product/) and its shader library (msl/b-<chain>/, APFS clones), meta.json; current points at it
#   regress <route> [--reps 6] [--metrics cpu_ms.p50,nvn_ms.p50,proc_cpu_ms.p50]   baseline host (A, on its own rewrite output and shader library) against the current host (B) in ABBA order,
#        each metric judged as in ab, then Holm-corrected over every segment x metric; RED only when B is significantly slower after Holm; conclusions go to the ledger.
#        Refused before any run: a baseline without that snapshot is ⚪ (exit 3); --reps too few to reach significance over the comparisons is red (exit 2, it names the --reps needed).
#        gpu_ms is not in the defaults: on a fast Mac the GPU lowers its clock when it has room, so it is clock-scaled (--metrics gpu_ms.p50 still works)
#   run / ab / regress --keep: a .keep file in the result folder, so workspace_prune.py never takes it
#   {dir} in an env value (--env, --a-env, --b-env) becomes the run's own folder, e.g. DM_NVN_PASSGPU={dir}/passgpu.tsv: one file per run and variant
#   run / ab / profile / regress --picture rec|orig|rec75|phone: the picture (dm/tools/picture.py). rec, the default, is this Mac's Recommended in full screen
#        (cli: every picture key pinned + DM_TEST_FULLSCREEN_AT=1; mac-app: the test copy's own Recommended); orig is the host's own defaults, windowed.
#        --env / --a-env / --b-env win over it. The picture goes into bench.json / ab.json / regress.json and the ledger's route column (<route>@<picture>)
#   Before each run the thermal state (Mac: pmset -g therm; wait while hot, at most --cool seconds) and Game Mode (gamepolicyctl) are checked and recorded in the result; last line RESULT {json}
import argparse
import collections
import datetime
import glob
import hashlib
import itertools
import json
import math
import os
import platform
import random
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmevents  # noqa: E402
import picture  # noqa: E402
import scenario  # noqa: E402
import workspace_prune  # noqa: E402

DM_ROOT = scenario.DM_ROOT
PRESENT = ('mean_ms', 'p50_ms', 'p95_ms', 'p99_ms', 'max_ms', 'slow_share', 'fps')
# 逐帧计时（DM_NVN_TSV 的 @ 行，hle_nvn.c）：帧间隔 / 出帧工作 / NVN 调用 / GPU / 翻译录制 / 翻译提交 / 出帧线程 CPU / 取图等待。苹果上帧率常被垂直同步钉死，这些不受刷新率量化，A/B 主要看它们
# Per-frame timing (the @ rows of DM_NVN_TSV, hle_nvn.c): frame interval / frame work / NVN calls / GPU / translation record / translation submit / frame-thread CPU / image-acquire wait. On Apple platforms the frame rate is often pinned by vsync; these are not quantized by the refresh rate, and A/B mainly looks at them
SERIES = ('frame_ms', 'work_ms', 'nvn_ms', 'gpu_ms', 'rec_ms', 'submit_ms', 'cpu_ms', 'wait_ms', 'draws', 'binds', 'cmdmem_cb',   # draws / binds / cmdmem_cb 是逐帧计数（条 / 次） | draws / binds / cmdmem_cb are per-frame counts
          'proc_cpu_ms', 'sub_cpu_ms', 'pso_new', 'lib_new', 'rt_load_mb',   # 全进程 CPU / 翻译线程提交 CPU / 本帧同步建管线数 / 建着色器库数 / 渲染目标读回量 | whole-process CPU / translation-thread submit CPU / pipelines and shader libraries built synchronously this frame / render-target load traffic
          'bindskip',   # 录制时滤掉的重复状态绑定（条 / 帧，hle_nvn.c bshadow） | repeated state binds filtered while recording (per frame, hle_nvn.c bshadow)
          'proc_mj')    # 进程耗电（mJ / 帧，内核能量账） | process energy (mJ per frame, the kernel's energy account)
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


def holm(rows, alpha=0.05):
    """Holm's step-down correction over every row that carries a p: sorted by p, the row of rank k (from 0) stays significant while p <= alpha / (m - k);
    after the first that fails, every later one fails too. Sets row['holm'] True / False; rows without p (too few runs) are left as they are."""
    ps = sorted((r for r in rows if isinstance(r.get('p'), (int, float))), key=lambda r: r['p']); m = len(ps); ok = True
    for k, r in enumerate(ps):
        ok = ok and r['p'] <= alpha / (m - k); r['holm'] = ok
    return rows


def min_reps(m, alpha=0.05):
    """The fewest runs per side (2..20) at which the exact two-sided Mann-Whitney test can reach Holm's strictest level alpha / m: its smallest p is 2 / C(2n, n)."""
    return next((n for n in range(2, 21) if 2 / math.comb(2 * n, n) <= alpha / m), 21)


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


def run_once(route, backend, out_dir, env, host, cool, mpt=False):
    env = {k: v.replace('{dir}', out_dir) for k, v in env.items()}   # {dir} in a value = this run's own folder (DM_NVN_PASSGPU={dir}/passgpu.tsv)
    th0, waited = wait_cool(cool)
    saved = {k: os.environ.get(k) for k in list(env) + ['DM_FRAMES', 'DM_STATEREP', 'SCN_DMHOST', 'DM_NVN_TSV', 'DM_NVN_TSV_CALLS']}
    os.environ.update(env); os.environ['DM_FRAMES'] = os.path.join(out_dir, 'frames.tsv'); os.environ['DM_STATEREP'] = '1'
    os.environ['DM_NVN_TSV'] = os.path.join(out_dir, 'nvn.tsv'); os.environ['DM_NVN_TSV_CALLS'] = '0'
    if host: os.environ['SCN_DMHOST'] = host
    try:
        os.makedirs(out_dir, exist_ok=True)
        runner = scenario.Runner(route, backend, out_dir, False); lst: dict[str, Any] = {}
        if mpt:   # 系统的 Metal 读数（metalperftrace listen）陪跑整遍 | the system's Metal readings (metalperftrace listen) for the whole run
            import gpucap   # noqa: PLC0415
            def attach(b):
                pid = b.pid()
                if pid: lst['l'] = gpucap.Listener(pid)
            runner.on_started = attach
        res, code = runner.go()
        if 'l' in lst: lst['mpt'] = lst.pop('l').stop(os.path.join(out_dir, 'metalperftrace.json'))
        if code == 0 and os.path.exists(os.path.join(out_dir, 'events.ndjson')):   # 启动各阶段记进历史账本（boothist.py） | boot stages go into the history ledger (boothist.py)
            import boothist   # noqa: PLC0415
            boothist.record([os.path.join(out_dir, 'events.ndjson')], f'bench:{backend}')
    finally:
        for k, v in saved.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
    return dict(ok=code == 0, reason=res.get('reason'), thermal_before=th0, cooled_s=waited, thermal_after=thermal(), segments=segments(out_dir), metalperftrace=lst.get('mpt'), dir=out_dir)


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
    """K=V pairs separated by spaces; shell quoting keeps a value with spaces whole (DM_MAC_APP='…/D2R Metal.app')"""
    out = {}
    for it in items or []:
        for kv in shlex.split(it):
            if '=' in kv: k, v = kv.split('=', 1); out[k] = v
    return out


def picture_env(name, backend, user_env):
    """The picture a run uses (dm/tools/picture.py): 'rec' (default) = this Mac's Recommended in full screen (cli: every picture key pinned +
    DM_TEST_FULLSCREEN_AT=1; mac-app: the test copy takes its own Recommended, DM_TEST_VIDEO_FIRSTBOOT=1), rec75 / phone likewise, 'orig' = the
    host's own defaults, windowed. The caller's env wins; it may size or switch the window itself (DM_TEST_WINSIZE / DM_TEST_FULLSCREEN_AT)."""
    return picture.env(name, backend, user_env)


def cmd_run(a):
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S'); name = os.path.splitext(os.path.basename(a.route))[0]
    base = os.path.join(DM_ROOT, 'run', 'bench', f'{name}_{stamp}')
    env = picture_env(a.picture, a.backend, parse_env(a.env)); reps = []
    for i in range(a.reps):
        print(f'== 第 {i + 1}/{a.reps} 遍 | run {i + 1}/{a.reps}', flush=True)
        reps.append(run_once(a.route, a.backend, os.path.join(base, f'r{i + 1}'), env, a.host, a.cool, a.mpt))
    res = dict(schema='bench/1', route=name, backend=a.backend, picture=a.picture, started=stamp, env=env, host=a.host, environment=environment(), reps=reps, summary=summarize(reps))
    out = a.out or os.path.join(base, 'bench.json')
    with open(out, 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1)
    keep(base, a)
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
    var = {'A': (picture_env(a.picture, a.backend, parse_env([a.a_env])), a.a_host), 'B': (picture_env(a.picture, a.backend, parse_env([a.b_env])), a.b_host)}
    order = ''.join('ABBA' for _ in range(math.ceil(a.reps / 2)))[:2 * a.reps]   # 每个变体 reps 遍 | reps runs per variant
    reps = {'A': [], 'B': []}
    for i, v in enumerate(order, 1):
        print(f'== {i}/{len(order)} 变体 | variant {v}', flush=True)
        env, host = var[v]; reps[v].append(run_once(a.route, a.backend, os.path.join(base, f'{i:02d}{v}'), env, host, a.cool))
    rows = compare(reps, a.metric)
    envinfo = environment()
    res = dict(schema='bench-ab/1', route=name, backend=a.backend, picture=a.picture, started=stamp, order=order, a=dict(env=var['A'][0], host=a.a_host), b=dict(env=var['B'][0], host=a.b_host),
               environment=envinfo, rows=rows, reps=reps)
    out = a.out or os.path.join(base, 'ab.json')
    with open(out, 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1)
    keep(base, a)
    ledger = ledger_write(stamp, envinfo, f'{name}@{a.picture}', a.metric, a.a_env or a.a_host or '-', a.b_env or a.b_host or '-', rows)
    ok = all(r['ok'] for v in reps.values() for r in v)
    print(f"BENCH-AB {'OK' if ok else 'RED'} {name} → {out}（账本 | ledger {ledger}）")
    print('RESULT ' + json.dumps(dict(name='bench-ab', verdict='OK' if ok else 'RED', route=name, rows=[{k: r.get(k) for k in ('segment', 'diff', 'ci95', 'p', 'verdict')} for r in rows], file=out), ensure_ascii=False))
    return 0 if ok else 1


def compare(reps, m):
    """A / B 各遍按段取指标 m：Mann-Whitney + 中位数差的自助区间，判「没差别 / B 慢 / B 快」。 | Per segment, metric m over the A / B runs: Mann-Whitney + bootstrap interval of the median difference, judged "no difference / B slower / B faster"."""
    rows = []
    segs = sorted({n for v in reps.values() for r in v if r['ok'] for n in r['segments']})
    for n in segs:
        va = [x for x in (metric(r['segments'].get(n, {}), m) for r in reps['A'] if r['ok']) if x is not None]
        vb = [x for x in (metric(r['segments'].get(n, {}), m) for r in reps['B'] if r['ok']) if x is not None]
        if len(va) < 2 or len(vb) < 2: rows.append(dict(segment=n, metric=m, verdict='not enough runs')); continue
        u, p = mann_whitney(va, vb); d, lo, hi = boot_median_diff(va, vb)
        worse = not m.endswith('.fps')   # 除了 fps，越大越差 | except fps, larger is worse
        sig = p < 0.05 and (lo > 0 or hi < 0)
        verdict = 'no difference' if not sig else ('B slower' if (d > 0) == worse else 'B faster')
        rows.append(dict(segment=n, metric=m, a=va, b=vb, median_a=statistics.median(va), median_b=statistics.median(vb), diff=round(d, 4), ci95=[round(lo, 4), round(hi, 4)], u=u, p=round(p, 4), verdict=verdict))
        print(f"  {n:<10} {m}: A {statistics.median(va):.3f} B {statistics.median(vb):.3f}  B−A {d:+.3f} [{lo:+.3f}, {hi:+.3f}]  p {p:.3f}  → {verdict}")
    return rows


def ledger_write(stamp, envinfo, name, m, a_label, b_label, rows):
    ledger = os.path.join(DM_ROOT, 'run', 'bench', 'ledger.tsv'); os.makedirs(os.path.dirname(ledger), exist_ok=True)
    new = not os.path.exists(ledger)
    with open(ledger, 'a', encoding='utf-8') as f:
        if new: f.write('时刻 | time\t提交 | rev\t路线 | route\t段 | segment\t指标 | metric\tA\tB\t每组遍数 | runs\tA 中位 | median A\tB 中位 | median B\tB−A\t区间 | CI95\tp\t结论 | verdict\n')
        rev = envinfo['git']['rev']
        for r in rows:
            if 'diff' not in r: continue
            verdict = r['verdict'] + (f" (Holm {'ok' if r['holm'] else 'n.s.'})" if 'holm' in r else '')
            f.write(f"{stamp}\t{rev}\t{name}\t{r['segment']}\t{m}\t{a_label}\t{b_label}\t{len(r['a'])}/{len(r['b'])}\t{r['median_a']:.4f}\t{r['median_b']:.4f}\t{r['diff']:+.4f}\t[{r['ci95'][0]:+.4f}, {r['ci95'][1]:+.4f}]\t{r['p']:.4f}\t{verdict}\n")
    return ledger


# ---------- 采样归因与卡顿 | sampled attribution and hitches ----------
WAITS = ('__psynch', '__semwait', 'semaphore_', 'mach_msg', '__ulock', 'kevent', '__select', '__workq', '__wait4', 'nanosleep', '__nanosleep', 'usleep', '__sleep', '_pthread_cond_wait')
SAMPLE_RE = re.compile(r'\[dm\] SAMPLE #\d+ \[([\d.]+)s\] thr=(0x[0-9a-f]+) pc=(\S+)')


def role(name):
    """线程名 → 角色（按名字粗分；无名的 Thread_0x… 是游戏的任务线程）。 | Thread name -> role (coarsely by name; the unnamed Thread_0x... are the game's job workers)."""
    if name in ('主线程', 'main'): return 'main'
    if name.startswith('Thread_0x'): return 'workers'
    if 'AIO' in name: return 'io'
    if name.startswith('FMOD'): return 'audio'
    return 'other'


def samples(log):
    """host.log 的全线程采样 → (线程名表, [(秒, 句柄, 在跑, 最内层游戏函数)])。 | All-thread samples from host.log -> (thread name map, [(seconds, handle, running, innermost game function)])."""
    sys.path.insert(0, HERE); import guestsyms   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    names, out, cur = {}, [], None
    with open(log, encoding='utf-8', errors='replace') as f:
        for ln in f:
            if ln.startswith('[dm] SAMPLE-THR '):
                h, _, nm = ln[16:].strip().partition(' '); names[int(h, 16)] = nm; continue
            m = SAMPLE_RE.match(ln)
            if m: cur = [float(m.group(1)), int(m.group(2), 16), not m.group(3).startswith(WAITS), m.group(3)]; continue
            if cur and ln.startswith('[dm] bt:'):
                frames = [cur[3]] + ln[8:].split()
                game = next((x for x in frames if x.startswith(('main+0x', 'sdk+0x'))), None)
                sym = guestsyms.lookup(game) if game else ''
                fn = (sym.split('+0x')[0] if sym else game) if game else 'host:' + cur[3].split('+0x')[0]   # 没名字的游戏函数留模块偏移 | game functions without a name keep their module offset
                out.append((cur[0], cur[1], cur[2], fn)); cur = None
    return names, out


def attribute(run_dir, hitch_ms):
    names, smp = samples(os.path.join(run_dir, 'host.log'))
    evs = dmevents.load(os.path.join(run_dir, 'events.ndjson')) if os.path.exists(os.path.join(run_dir, 'events.ndjson')) else []
    marks = [(e['t'], e.get('name')) for e in evs if e['ev'] == 'mark']
    def table(t0, t1):
        by: dict[str, Any] = {}
        for t, h, run, fn in smp:
            if not t0 <= t < t1: continue
            r = by.setdefault(role(names.get(h, '?')), dict(samples=0, running=0, hot=collections.Counter()))
            r['samples'] += 1
            if run: r['running'] += 1; r['hot'][fn] += 1
        return {k: dict(samples=v['samples'], running_pct=round(100 * v['running'] / v['samples'], 1) if v['samples'] else 0,
                        hot=[[f, n] for f, n in v['hot'].most_common(6)]) for k, v in sorted(by.items())}
    segs = {name: table(t0, t1) for (t0, name), (t1, _) in zip(marks, marks[1:], strict=False) if name != 'end'}
    nv = nvn_rows(os.path.join(run_dir, 'nvn.tsv')); hitches = []
    for f, row in sorted(nv.items()):
        ms, t = row.get('frame_ms'), row.get('t_s')
        if ms is None or t is None or ms <= hitch_ms: continue
        hot = collections.Counter(fn for ts, h, run, fn in smp if run and t - ms / 1000 <= ts <= t)
        hitches.append(dict(frame=f, ms=round(ms, 1), t=round(t, 3), running=[[k, n] for k, n in hot.most_common(6)]))
    return dict(threads={f'{h:#x}': names[h] for h in sorted(names)}, segments=segs, hitches=hitches)


def cmd_profile(a):
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S'); name = os.path.splitext(os.path.basename(a.route))[0]
    out = os.path.join(DM_ROOT, 'run', 'bench', f'{name}_profile_{stamp}')
    r = run_once(a.route, a.backend, out, picture_env(a.picture, a.backend, {'DM_SAMPLE': f'{a.ms}:*'}), a.host, a.cool)
    prof = attribute(out, a.hitch_ms); prof.update(route=name, picture=a.picture, ms=a.ms, hitch_ms=a.hitch_ms, ok=r['ok'], segments_timing=r['segments'])
    with open(os.path.join(out, 'profile.json'), 'w', encoding='utf-8') as f: json.dump(prof, f, ensure_ascii=False, indent=1)
    for seg, tab in prof['segments'].items():
        print(f'== {seg}')
        for rl, v in tab.items(): print(f"  {rl:<8} 在跑 | running {v['running_pct']:5.1f}%  " + '; '.join(f'{fn} {n}' for fn, n in v['hot'][:4]))
    print(f"  卡顿 | hitches > {a.hitch_ms} ms: {len(prof['hitches'])}" + ''.join(f"\n    帧 | frame {h['frame']} {h['ms']} ms: " + '; '.join(f'{fn} {n}' for fn, n in h['running'][:4]) for h in prof['hitches'][:8]))
    print(f"BENCH-PROFILE {'OK' if r['ok'] else 'RED'} {name} → {os.path.join(out, 'profile.json')}")
    print('RESULT ' + json.dumps(dict(name='bench-profile', verdict='OK' if r['ok'] else 'RED', route=name, hitches=len(prof['hitches']), file=os.path.join(out, 'profile.json')), ensure_ascii=False))
    return 0 if r['ok'] else 1


# ---------- 对留存基线的回归 A/B | regression A/B against a kept baseline ----------
BASE = os.path.join(DM_ROOT, 'run', 'bench', 'baseline')


def current_host():
    h = os.path.join(scenario.DM, 'host', 'dmhost')
    if subprocess.run(['make', '-s', '-C', os.path.dirname(h), 'dmhost'], capture_output=True, check=False).returncode: raise scenario.ScenarioError('host build failed')
    return h


def clone(src, dst):
    """Copies the folder src to dst (which must not exist): an APFS clone (cp -cR, blocks shared until either side changes), else a plain copy."""
    if subprocess.run(['cp', '-cR', src, dst], capture_output=True, check=False).returncode == 0: return
    shutil.rmtree(dst, ignore_errors=True); shutil.copytree(src, dst, symlinks=True)


def snapshot(src_host, product_dir, msl_root, chain, product, d, git=None):
    """Keeps a host with everything it runs on as baseline folder d: the host and the tables it reads beside itself (host/tables, ../gfx/nvn/*.tsv,
    nvn_mtl.m exe_rel), the rewrite output it was built against (product/) and its shader library (msl/b-<chain>/), plus meta.json.
    The whole snapshot is built in d.new and renamed into place at the end, so saving twice into one folder never nests a copy inside an old one.
    Returns (ok, d or the reason); on a refusal d is left as it was."""
    lib = os.path.join(msl_root, 'b-' + chain)
    if not os.path.isdir(lib): return False, f'shader library {lib} is missing (./setup.sh)'
    if not os.path.isdir(product_dir): return False, f'rewrite output {product_dir} is missing (./setup.sh)'
    with open(src_host, 'rb') as f:
        if chain.encode() not in f.read(): return False, 'host was built from another shader chain'
    t = d + '.new'; shutil.rmtree(t, ignore_errors=True)
    os.makedirs(os.path.join(t, 'host')); os.makedirs(os.path.join(t, 'gfx', 'nvn')); os.makedirs(os.path.join(t, 'msl'))
    shutil.copy2(src_host, os.path.join(t, 'host', 'dmhost'))
    if os.path.isdir(os.path.join(os.path.dirname(src_host), 'tables')):   # the game-derived tables it reads beside itself (dm/host/dm_tables.h)
        shutil.copytree(os.path.join(os.path.dirname(src_host), 'tables'), os.path.join(t, 'host', 'tables'))
    for x in glob.glob(os.path.join(os.path.dirname(src_host), '..', 'gfx', 'nvn', '*.tsv')): shutil.copy2(x, os.path.join(t, 'gfx', 'nvn', os.path.basename(x)))   # shader map, c1 list...
    clone(product_dir, os.path.join(t, 'product')); clone(lib, os.path.join(t, 'msl', 'b-' + chain))
    with open(src_host, 'rb') as f: sha = hashlib.sha256(f.read()).hexdigest()
    meta = dict(rev=(git or {}).get('rev'), dirty=(git or {}).get('dirty'), product=product, chain=chain, saved=datetime.datetime.now().isoformat(timespec='seconds'), host_sha256=sha)
    with open(os.path.join(t, 'meta.json'), 'w', encoding='utf-8') as f: json.dump(meta, f, indent=1)
    old = d + '.old'; shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(d): os.rename(d, old)
    os.rename(t, d); shutil.rmtree(old, ignore_errors=True)
    return True, d


def baseline_meta(d):
    try:
        with open(os.path.join(d, 'meta.json'), encoding='utf-8') as f: return json.load(f)
    except (OSError, ValueError): return None


def baseline_env(d):
    """The environment that runs a kept host on its own rewrite output and shader library; None when the baseline has no such snapshot."""
    m = baseline_meta(d)
    if not m or not m.get('chain') or not os.path.isdir(os.path.join(d, 'product')) or not os.path.isdir(os.path.join(d, 'msl', 'b-' + m['chain'])): return None
    return {'SCN_PRODUCT': os.path.join(d, 'product'), 'DM_MSL_DIR': os.path.join(d, 'msl')}


def current_product():
    with open(os.path.join(scenario.DM, 'PRODUCT'), encoding='utf-8') as f: return f.read().strip()


def cmd_baseline(a):
    if a.name and (not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', a.name) or a.name == 'current' or a.name.endswith(('.new', '.old'))):
        print(f'🔴 BENCH-BASELINE bad name {a.name!r} (letters, digits, . _ -; not current, not *.new / *.old)'); return 2
    src = os.path.abspath(a.host) if a.host else current_host()
    git = environment()['git']; name = a.name or git.get('rev') or 'unknown'; d = os.path.join(BASE, name)
    chain = workspace_prune.current_chain()
    if not chain: print('🔴 BENCH-BASELINE no DMSH_CHAIN in dm/gfx/nvn/dmsh_chain.h (./setup.sh)'); return 1
    product_dir = subprocess.run(['bash', os.path.join(scenario.DM, 'tools', 'product.sh'), 'path', 'touch'], capture_output=True, text=True, check=False).stdout.strip()
    msl_root = os.environ.get('DM_MSL_DIR') or os.path.join(DM_ROOT, 'build', 'cache', 'metal_shader_cache')
    os.makedirs(BASE, exist_ok=True)
    ok, why = snapshot(src, product_dir, msl_root, chain, current_product(), d, git)
    if not ok: print(f'🔴 BENCH-BASELINE {why}'); return 1
    cur = os.path.join(BASE, 'current')
    if os.path.lexists(cur): os.remove(cur)
    os.symlink(name, cur)
    print(f"BENCH-BASELINE OK {name} → {d} (rev {git.get('rev')}{' dirty' if git.get('dirty') else ''}, product {current_product()}, chain b-{chain}); current → {name}"); return 0


def cmd_regress(a):
    base = os.path.join(BASE, 'current', 'host', 'dmhost')
    if not os.path.exists(base): print('⚪ BENCH-REGRESS no baseline (bench.py baseline save)'); return 3
    bdir = os.path.realpath(os.path.join(BASE, 'current')); bname = os.path.basename(bdir); benv = baseline_env(bdir)
    if benv is None:
        print(f'⚪ BENCH-REGRESS the kept baseline {bname} has no rewrite output / shader library snapshot (bench.py baseline save again)'); return 3
    metrics = [m for m in a.metrics.split(',') if m]; bad = [m for m in metrics if m not in METRICS]
    if bad or not metrics: print(f'🔴 BENCH-REGRESS unknown metric {bad or a.metrics!r}'); return 2
    # Runs guard: the route's segments x the metrics are the comparisons Holm corrects for; with too few runs no row could ever be significant
    segs = [st['kw'].get('name') for st in scenario.parse(a.route)[1] if st['op'] == 'mark' and st['kw'].get('name') != 'end']
    ncmp = len(metrics) * max(len(segs), 1); need = min_reps(ncmp)
    if a.reps < need:
        print(f'🔴 BENCH-REGRESS --reps {a.reps} cannot reach significance over {ncmp} comparisons (smallest p {2 / math.comb(2 * a.reps, a.reps):.4f} > {0.05 / ncmp:.4f}); use --reps {need}'); return 2
    bmeta = baseline_meta(bdir) or {}
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S'); name = os.path.splitext(os.path.basename(a.route))[0]
    out = os.path.join(DM_ROOT, 'run', 'bench', f'{name}_regress_{stamp}')
    hosts = {'A': os.path.realpath(base), 'B': current_host()}
    sides = dict(baseline=dict(name=bname, rev=bmeta.get('rev'), product=bmeta.get('product'), chain=bmeta.get('chain')),
                 current=dict(rev=environment()['git'].get('rev'), product=current_product(), chain=workspace_prune.current_chain()))
    print(f"baseline {bname}: {sides['baseline']['product']} b-{sides['baseline']['chain']}; current: {sides['current']['product']} b-{sides['current']['chain']}", flush=True)
    order = ''.join('ABBA' for _ in range(math.ceil(a.reps / 2)))[:2 * a.reps]; reps: dict[str, list] = {'A': [], 'B': []}
    for i, v in enumerate(order, 1):
        print(f'== {i}/{len(order)} 变体 | variant {v}', flush=True)
        reps[v].append(run_once(a.route, a.backend, os.path.join(out, f'{i:02d}{v}'), picture_env(a.picture, a.backend, benv if v == 'A' else {}), hosts[v], a.cool))
    keep(out, a)
    envinfo = environment(); by = {m: compare(reps, m) for m in metrics}; rows = [r for rr in by.values() for r in rr]
    holm(rows)   # one family: every segment x metric of this regression
    for m, rr in by.items(): ledger_write(stamp, envinfo, f'{name}@{a.picture}', m, f'baseline {bname}', 'current', rr)
    for r in rows:
        if 'holm' in r: print(f"  {r['segment']:<10} {r['metric']}: {r['verdict']}, p {r['p']:.3f} → Holm {'ok' if r['holm'] else 'n.s.'}")
    slower = [r for r in rows if r.get('verdict') == 'B slower' and r.get('holm')]; ok = all(r['ok'] for v in reps.values() for r in v)
    with open(os.path.join(out, 'regress.json'), 'w', encoding='utf-8') as f: json.dump(dict(route=name, picture=a.picture, hosts=hosts, sides=sides, order=order, rows=rows, environment=envinfo), f, ensure_ascii=False, indent=1)
    verdict = 'RED' if slower or not ok else 'OK'
    print(f"BENCH-REGRESS {verdict} {name}: {len(slower)} significantly slower after Holm" + ''.join(f"\n  {r['segment']} {r['metric']} +{r['diff']}" for r in slower))
    print('RESULT ' + json.dumps(dict(name='bench-regress', verdict=verdict, route=name, slower=[(r['segment'], r['metric'], r['diff']) for r in slower]), ensure_ascii=False))
    return 1 if verdict == 'RED' else 0


def keep(folder, a):
    """--keep: a .keep file in the result folder, so workspace_prune.py never takes it (A/B evidence cited in a commit or the README)."""
    if getattr(a, 'keep', False) and os.path.isdir(folder):
        open(os.path.join(folder, '.keep'), 'w', encoding='utf-8').close()


def main(argv):
    ap = argparse.ArgumentParser(prog='bench.py', description='Fixed-route benchmarks over the control port (dm/verify/bench).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run'); r.add_argument('route'); r.add_argument('--backend', default='cli', choices=('cli', 'mac-app')); r.add_argument('--reps', type=int, default=3)
    r.add_argument('--env', action='append'); r.add_argument('--host'); r.add_argument('--out'); r.add_argument('--cool', type=float, default=120)
    r.add_argument('--mpt', action='store_true', help='also record metalperftrace listen (on-glass intervals, lateness, skipped frames) per run')
    r.add_argument('--keep', action='store_true', help='write .keep into the result folder so workspace_prune.py never takes it')
    b = sub.add_parser('ab'); b.add_argument('route'); b.add_argument('--backend', default='cli', choices=('cli', 'mac-app')); b.add_argument('--reps', type=int, default=6)
    b.add_argument('--a-env', default=''); b.add_argument('--b-env', default=''); b.add_argument('--a-host'); b.add_argument('--b-host'); b.add_argument('--metric', default='nvn_ms.p50', choices=METRICS)
    b.add_argument('--out'); b.add_argument('--cool', type=float, default=120); b.add_argument('--keep', action='store_true', help='write .keep into the result folder')
    s = sub.add_parser('show'); s.add_argument('file')
    s.add_argument('--metric', action='append', default=[], help='ab.json：按这些指标重比 A / B（可多给，或逗号分隔）| ab.json: compare A / B again on these metrics (repeat or comma-separate)')
    pr = sub.add_parser('profile'); pr.add_argument('route'); pr.add_argument('--backend', default='cli', choices=('cli', 'mac-app')); pr.add_argument('--ms', type=int, default=4)
    pr.add_argument('--hitch-ms', type=float, default=25); pr.add_argument('--host'); pr.add_argument('--cool', type=float, default=120)
    bl = sub.add_parser('baseline'); bl.add_argument('op', choices=('save',)); bl.add_argument('--host'); bl.add_argument('--name', help='baseline folder name (default: the git revision)')
    rg = sub.add_parser('regress'); rg.add_argument('route'); rg.add_argument('--backend', default='cli', choices=('cli',), help='the kept host runs on the command-line host only'); rg.add_argument('--reps', type=int, default=6)
    rg.add_argument('--metrics', default='cpu_ms.p50,nvn_ms.p50,proc_cpu_ms.p50', help='comma-separated; gpu_ms is left out by default: on a fast Mac it is clock-scaled'); rg.add_argument('--cool', type=float, default=120); rg.add_argument('--keep', action='store_true', help='write .keep into the result folder')
    for sp in (r, b, pr, rg):
        sp.add_argument('--picture', default='rec', choices=('rec', 'orig', 'rec75', 'phone'),
                        help="rec (default): this Mac's Recommended in full screen (dm/tools/picture.py); orig: the host's own defaults, windowed; rec75 / phone: those sets")
    a = ap.parse_args(argv)
    held = scenario.game_lock(__file__, argv, f"bench {a.cmd} {os.path.basename(getattr(a, 'route', ''))} ~{getattr(a, 'reps', 1) * 2} min", a.cmd in ('run', 'ab', 'profile', 'regress'))   # 开窗跑局先拿 Mac 游戏锁 | runs that open a window take the Mac game lock first
    if held is not None: return held
    if a.cmd == 'run': return cmd_run(a)
    if a.cmd == 'ab': return cmd_ab(a)
    if a.cmd == 'profile': return cmd_profile(a)
    if a.cmd == 'baseline': return cmd_baseline(a)
    if a.cmd == 'regress': return cmd_regress(a)
    with open(a.file, encoding='utf-8') as f: res = json.load(f)
    if res.get('schema') == 'bench-ab/1':   # 一次 A / B 的各遍都存着全部指标：换指标重比不必重跑 | an A / B keeps every metric of every run: comparing on another metric needs no rerun
        ms = [m for x in (a.metric or ['nvn_ms.p50,cpu_ms.p50,gpu_ms.p50,proc_cpu_ms.p50']) for m in x.split(',') if m]
        bad = [m for m in ms if m not in METRICS]
        if bad: print(f'🔴 不认识的指标 | unknown metrics: {bad}'); return 2
        for m in ms: compare(res['reps'], m)
        return 0
    show(res)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
