#!/usr/bin/env python3
"""Why a run's frames cost what they cost: the reasons behind a bench run's numbers, from its per-frame records (DM_NVN_TSV, nvn.tsv), its marks and the whole-process sampler (DM_SAMPLER,
prof.tsv: dm/host/dm_sampler.c). `bench.py profile` writes those and runs this; it also works on any run folder that has them. For each segment between two marks:

  budget    what each stage takes against the frame's time. The GPU lowers its clock until its work fills the frame, so its figure is never headroom: bench.py headroom measures that.
  late      every frame shown late or not at all, with what stood out in it: the stage that jumped against the segment's median, what the frame built or loaded, and what the threads were
            running then. Grouped by cause, with the time lost to each.
  threads   CPU per drawn frame for each thread role, exact from the system's clock. Samples are checked against it: a share of samples far from the clock means the table below can't be trusted.
  hot       functions per thread role in ms per drawn frame, self and inclusive, with a 95% range from the sample count; then a roll-up by subsystem (the prefix of a function's name).

  bench_why.py <run dir> [--vs <other run dir>] [--top N] [--json FILE] [--folded FILE]

--vs compares two runs: the stages' medians and the functions whose cost moved by more than their sampling ranges allow. --folded writes one line per distinct stack for a flame graph tool.
Function names come from the private engine analysis through guestsyms.py: the output stays on this machine, like the analysis itself. Last line: RESULT {json}. Pinned by test_bench_why.py.
"""
import argparse
import collections
import json
import math
import os
import statistics as S
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
try:
    import guestsyms  # noqa: E402
except ImportError:
    guestsyms = None

STAGES = (('game thread CPU', 'cpu_ms'), ('NVN calls', 'nvn_ms'), ('translation CPU', 'sub_cpu_ms'), ('whole process CPU', 'proc_cpu_ms'), ('GPU, at its clock', 'gpu_ms'), ('present queue wait', 'pq_wait_ms'),
          ('drawable wait', 'drawable_ms'), ('uploads', 'upload_ms'), ('page protection', 'mprotect_ms'))
COUNTS = ('draws', 'binds', 'bindskip', 'passes', 'rt_load_mb', 'rt_store_mb', 'faults', 'wakes', 'proc_mj')
JUMP_STAGES = (('the game thread', 'cpu_ms'), ('translation', 'sub_wall_ms'), ('the GPU', 'gpu_ms'), ('the present queue', 'pq_wait_ms'), ('a drawable', 'drawable_ms'), ('uploads', 'upload_ms'), ('page protection', 'mprotect_ms'))
LATE_FACTOR = 1.25      # a frame shown after this many of its periods is late: one refresh late is 1.5 at the 30 / 60 of frame generation and 2 at real 60
JUMP_MIN_MS = 2.0       # a stage must rise this much over its median to count as a reason
AGREE_MAX = 0.35        # samples and the CPU clock may differ this much per role before the hot table is flagged


def load_frames(run):
    frames = collections.defaultdict(dict); path = os.path.join(run, 'nvn.tsv')
    if not os.path.exists(path): return frames
    with open(path, encoding='utf-8') as f:
        for ln in f:
            a = ln.rstrip('\n').split('\t')
            if len(a) == 3 and a[1].startswith('@') and a[0].isdigit():
                try: frames[int(a[0])][a[1][1:]] = float(a[2])
                except ValueError: pass
    return frames


def load_marks(run):
    out = []; path = os.path.join(run, 'events.ndjson')
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            for ln in f:
                if '"mark"' not in ln: continue
                try: e = json.loads(ln)
                except ValueError: continue
                if e.get('ev') == 'mark' and 'frame' in e: out.append((e['name'], int(e['frame']), float(e['t'])))
    return out


def load_prof(path):
    """prof.tsv -> ticks (times), names {tid: name}, samples [(t, tid, frames)], cpu {tid: [(t, us)]}"""
    ticks, names, samples, cpu = [], {}, [], collections.defaultdict(list)
    if not os.path.exists(path): return ticks, names, samples, cpu
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            a = ln.rstrip('\n').split('\t')
            if a[0] == 'K' and len(a) == 2: ticks.append(float(a[1]))
            elif a[0] == 'T' and len(a) == 3: names[a[1]] = a[2]
            elif a[0] == 'S' and len(a) >= 3 and ticks: samples.append((ticks[-1], a[1], a[2:]))
            elif a[0] == 'C' and len(a) == 4: cpu[a[2]].append((float(a[1]), int(a[3])))
    return ticks, names, samples, cpu


def role(name):
    """A thread's name -> what it does. The game's unnamed Thread_0x… are its job workers"""
    n = name.split(' ', 1)[1] if name.startswith('0x') and ' ' in name else name
    if n in ('主线程', 'main'): return 'game main'
    if n.startswith('Thread_0x'): return 'game workers'
    if 'AIO' in n: return 'game io'
    if n.startswith('FMOD'): return 'audio'
    if n == 'nvn-translate': return 'translation'
    if n.startswith('nvn-present'): return 'present'
    if n.startswith('llvm-worker') or 'compile' in n.lower(): return 'shader compile'
    return 'other'


_fn = {}
def fn_of(frame):
    """A frame -> its function: a name from the engine analysis, or the module offset; host frames are symbols already"""
    if frame in _fn: return _fn[frame]
    out = frame
    if frame.startswith(('main+0x', 'sdk+0x')) and guestsyms:
        try: sym = guestsyms.lookup(frame)
        except Exception: sym = ''
        if sym: out = sym.split('+0x')[0].split('  [')[0].replace(' (medium)', '').replace(' (low)', '')
    _fn[frame] = out
    return out


def subsystem(fn):
    if fn.startswith(('main+0x', 'sdk+0x', 'fn_')): return '(unnamed game code)'
    if '_' in fn and fn[0].isupper(): return fn.split('_', 1)[0]
    return '(host and system)'


def pct(v, q):
    if not v: return 0.0
    s = sorted(v); return s[min(len(s) - 1, int(len(s) * q))]


def segments(frames, marks):
    if not frames: return []
    if not marks: return [('all', min(frames), max(frames) + 1)]
    out = []
    for (name, f0, _), (_, f1, _) in zip(marks, marks[1:]):
        if name != 'end' and f1 > f0: out.append((name, f0, f1))
    last = marks[-1]
    if last[0] != 'end' and max(frames) > last[1]: out.append((last[0], last[1], max(frames) + 1))
    return out


def budget(frames, f0, f1):
    fs = [frames[f] for f in range(f0, f1) if f in frames]
    shown = [r['presented_ms'] for r in fs if r.get('presented_ms', 0) > 0]
    period = S.median(shown) if shown else 1000.0 / 60
    stages = {key: dict(label=label, p50=round(S.median(v), 3), p95=round(pct(v, .95), 3), max=round(max(v), 3)) for label, key in STAGES if (v := [r[key] for r in fs if key in r])}
    counts = {key: round(S.median(v), 3) for key in COUNTS if (v := [r[key] for r in fs if key in r])}
    return dict(frames=len(fs), period_ms=round(period, 3), stages=stages, counts=counts)


def late_frames(frames, f0, f1, bud, samples, names):
    period = bud['period_ms']; med = {k: S.median(v) for _, k in JUMP_STAGES if (v := [frames[f][k] for f in range(f0, f1) if f in frames and k in frames[f]])}
    out = []
    for f in range(f0, f1):
        r = frames.get(f)
        if not r or 'presented_ms' not in r: continue
        iv = r['presented_ms']
        if iv >= 0 and iv <= period * LATE_FACTOR: continue
        # the frame shown late was held up by itself or by the one before it
        best, jump = None, 0.0; near = [r] + ([frames[f - 1]] if f - 1 in frames else [])
        for label, k in JUMP_STAGES:
            d = max((x.get(k, 0.0) - med.get(k, 0.0) for x in near), default=0.0)
            if d > jump: best, jump = label, d
        built = sum(x.get('pso_new', 0) + x.get('lib_new', 0) for x in near); up = max(x.get('upload_ms', 0) for x in near); fl = max(x.get('faults', 0) for x in near)
        if built: cause = 'pipelines built'
        elif jump >= JUMP_MIN_MS and best: cause = best
        else: cause = 'nothing in the records: the system or the display'
        t = r.get('t_s'); hot = collections.Counter()
        if t is not None:
            lo, hi = t - 2 * period / 1000, t + period / 1000
            for ts, tid, st in samples:
                if lo <= ts <= hi: hot[f"{role(names.get(tid, tid))}: {fn_of(next((x for x in st if x.startswith(('main+0x', 'sdk+0x'))), st[0]))}"] += 1
        out.append(dict(frame=f, shown_ms=round(iv, 1), lost_ms=round(max(iv - period, 0.0), 1) if iv >= 0 else round(period, 1), cause=cause, jump_ms=round(jump, 1), built=int(built), upload_ms=round(up, 1), faults=int(fl), running=[[k, n] for k, n in hot.most_common(4)]))
    return out


def thread_cpu(cpu, names, t0, t1, nframes):
    """CPU per drawn frame by role, from the system's clock"""
    out = collections.Counter()
    for tid, rows in cpu.items():
        inside = [(t, us) for t, us in rows if t0 <= t <= t1]
        if len(inside) >= 2: out[role(names.get(tid, tid))] += (inside[-1][1] - inside[0][1]) / 1000 / max(nframes, 1)
    return {k: round(v, 3) for k, v in out.most_common()}


def hot_tables(samples, names, ticks, t0, t1, nframes):
    """Per role: ms per drawn frame by function, self and inclusive, with the 95% range"""
    tk = [t for t in ticks if t0 <= t < t1]
    if len(tk) < 2: return {}, 0.0
    tick_ms = (tk[-1] - tk[0]) / (len(tk) - 1) * 1000; k = tick_ms / max(nframes, 1)
    by = {}
    for ts, tid, st in samples:
        if not t0 <= ts < t1: continue
        r = by.setdefault(role(names.get(tid, tid)), dict(n=0, self=collections.Counter(), incl=collections.Counter(), guest=collections.Counter()))
        r['n'] += 1; fs = [fn_of(x) for x in st]
        r['self'][fs[0]] += 1
        for fn in set(fs): r['incl'][fn] += 1
        r['guest'][fn_of(next((x for x in st if x.startswith(('main+0x', 'sdk+0x'))), st[0]))] += 1
    def row(c, top): return [dict(fn=fn, ms=round(n * k, 3), range_ms=round(1.96 * math.sqrt(n) * k, 3), n=n) for fn, n in c.most_common(top)]
    out = {}
    for rl, r in sorted(by.items(), key=lambda kv: -kv[1]['n']):
        sub = collections.Counter()
        for fn, n in r['guest'].items(): sub[subsystem(fn)] += n
        out[rl] = dict(ms=round(r['n'] * k, 3), samples=r['n'], self=row(r['self'], 200), incl=row(r['incl'], 200), guest=row(r['guest'], 200), subsystems=[[s, round(n * k, 3)] for s, n in sub.most_common()])
    return out, tick_ms


def analyse(run):
    frames = load_frames(run); marks = load_marks(run); ticks, names, samples, cpu = load_prof(os.path.join(run, 'prof.tsv'))
    res = dict(run=run, has_samples=bool(samples), segments={})
    for name, f0, f1 in segments(frames, marks):
        bud = budget(frames, f0, f1)
        ts = [frames[f]['t_s'] for f in (f0, f1 - 1) if f in frames and 't_s' in frames[f]]
        t0, t1 = (ts[0], ts[1]) if len(ts) == 2 else (0, 0)
        late = late_frames(frames, f0, f1, bud, samples, names)
        causes = collections.defaultdict(lambda: [0, 0.0])
        for x in late: causes[x['cause']][0] += 1; causes[x['cause']][1] += x['lost_ms']
        hot, tick_ms = hot_tables(samples, names, ticks, t0, t1, bud['frames']) if samples and t1 > t0 else ({}, 0.0)
        clock = thread_cpu(cpu, names, t0, t1, bud['frames']) if cpu and t1 > t0 else {}
        agree = {}
        for rl, v in hot.items():
            c = clock.get(rl)
            if c and c > 0.2: agree[rl] = round(v['ms'] / c, 2)
        res['segments'][name] = dict(frames=(f0, f1), budget=bud, late=late, causes={k: dict(frames=v[0], lost_ms=round(v[1], 1)) for k, v in sorted(causes.items(), key=lambda kv: -kv[1][1])}, cpu_clock=clock, tick_ms=round(tick_ms, 3), hot=hot, samples_vs_clock=agree)
    res['flags'] = sorted({f'{n}/{rl} samples {x}x the CPU clock' for n, s in res['segments'].items() for rl, x in s['samples_vs_clock'].items() if abs(x - 1) > AGREE_MAX})
    return res, samples, names


def report(res, top):
    for name, s in res['segments'].items():
        b = s['budget']; print(f"== {name}: {b['frames']} frames, {b['period_ms']:.2f} ms each")
        for k, v in b['stages'].items(): print(f"   {v['label']:<20} p50 {v['p50']:8.2f}  p95 {v['p95']:8.2f}  max {v['max']:8.2f} ms   {100 * v['p50'] / b['period_ms']:5.1f}% of the frame")
        print('   per frame: ' + ', '.join(f'{k} {v:g}' for k, v in b['counts'].items()))
        if s['cpu_clock']: print('   CPU per frame, system clock: ' + ', '.join(f'{k} {v:.2f} ms' for k, v in s['cpu_clock'].items()))
        print(f"   late or not shown: {len(s['late'])}" + ''.join(f"\n     {c}: {v['frames']} frames, {v['lost_ms']:.0f} ms lost" for c, v in s['causes'].items()))
        for x in sorted(s['late'], key=lambda x: -x['lost_ms'])[:5]:
            print(f"     frame {x['frame']}: {x['shown_ms']} ms, {x['cause']}" + (f" (+{x['jump_ms']} ms)" if x['jump_ms'] >= JUMP_MIN_MS else '') + (' | ' + '; '.join(f'{k} {n}' for k, n in x['running']) if x['running'] else ''))
        for rl, v in s['hot'].items():
            ag = s['samples_vs_clock'].get(rl); print(f"   {rl}: {v['ms']:.2f} ms per frame from {v['samples']} samples" + (f", {ag}x the clock" if ag else ''))
            print('     self:      ' + '; '.join(f"{r['fn']} {r['ms']:.2f}±{r['range_ms']:.2f}" for r in v['self'][:top]))
            print('     inclusive: ' + '; '.join(f"{r['fn']} {r['ms']:.2f}" for r in v['incl'][:top]))
            print('     by game code: ' + '; '.join(f"{a} {m:.2f}" for a, m in v['subsystems'][:6]))


def compare(a, b, top):
    out = {}
    for name in a['segments']:
        if name not in b['segments']: continue
        sa, sb = a['segments'][name], b['segments'][name]
        st = {k: [sa['budget']['stages'][k]['p50'], sb['budget']['stages'][k]['p50']] for k in sa['budget']['stages'] if k in sb['budget']['stages']}
        moved = []
        for rl in set(sa['hot']) | set(sb['hot']):
            ra = {r['fn']: r for r in sa['hot'].get(rl, {}).get('guest', [])}; rb = {r['fn']: r for r in sb['hot'].get(rl, {}).get('guest', [])}
            for fn in set(ra) | set(rb):
                x, y = ra.get(fn, dict(ms=0, range_ms=0)), rb.get(fn, dict(ms=0, range_ms=0)); d = y['ms'] - x['ms']
                if abs(d) > x['range_ms'] + y['range_ms'] and abs(d) >= 0.02: moved.append(dict(role=rl, fn=fn, a_ms=x['ms'], b_ms=y['ms'], d_ms=round(d, 3)))
        moved.sort(key=lambda r: -abs(r['d_ms'])); out[name] = dict(stages=st, moved=moved[:top * 3], late=[len(sa['late']), len(sb['late'])])
        print(f"== {name}: late or not shown {len(sa['late'])} -> {len(sb['late'])}")
        for k, (x, y) in st.items(): print(f"   {k:<14} {x:8.2f} -> {y:8.2f} ms")
        for r in moved[:top * 2]: print(f"   {r['role']}: {r['fn']} {r['a_ms']:.2f} -> {r['b_ms']:.2f} ms ({r['d_ms']:+.2f})")
    return out


def folded(samples, names, path):
    c = collections.Counter()
    for _, tid, st in samples: c[role(names.get(tid, tid)) + ';' + ';'.join(fn_of(x).replace(';', ':') for x in reversed(st))] += 1
    with open(path, 'w', encoding='utf-8') as f:
        for k, n in c.most_common(): f.write(f'{k} {n}\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run'); ap.add_argument('--vs'); ap.add_argument('--top', type=int, default=8); ap.add_argument('--json'); ap.add_argument('--folded')
    a = ap.parse_args(argv)
    res, samples, names = analyse(a.run)
    if not res['segments']: print(f'no per-frame records in {a.run} (nvn.tsv)'); print('RESULT ' + json.dumps(dict(name='bench_why', verdict='UNAVAILABLE'))); return 3
    if a.vs:
        other, _, _ = analyse(a.vs); res['compare'] = compare(res, other, a.top)
    else: report(res, a.top)
    for f in res['flags']: print(f'⚠️ {f}: the samples are off, so the hot tables are only a guide')
    if a.folded and samples: folded(samples, names, a.folded)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1)
    print('RESULT ' + json.dumps(dict(name='bench_why', verdict='OK', segments={k: dict(late=len(v['late']), causes=v['causes']) for k, v in res['segments'].items()}, flags=res['flags']), ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main())
