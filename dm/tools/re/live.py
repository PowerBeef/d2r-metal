#!/usr/bin/env python3
# Reader for the live hooks' output (dm/host/dm_live.c, DM_LIVE=1): the records a run wrote to DM_LIVE_OUT and its <out>.hooks.tsv, summarised
# per hook with the guest symbol table (guestsyms.py: the dynamic symbols and the engine-analysis export's names).
#   summary <live.bin> [--json]     per hook: records, threads, call rate, callers, return durations, each capture's most frequent values
#   dump <live.bin> [--hook N] [--limit N]   one line per record
#   blocks --out F [--only main+0x…-main+0x…]   every basic block's first instruction (offsets, hex), for `live do=cover file=F`
#   trace-spec --out F [--only …] [--records N]   one count-only entry hook per function, for `live do=load file=F` (the call-trace mode)
#   counts <counts.tsv> [--top N]   the census `live do=counts` wrote: functions that ran, by calls, named
#   cover <cov file> [--top N]      what `live do=cover_dump` wrote: functions that ran, with how many of their blocks
#   cover-diff <a> <b>              blocks that ran in b and not in a, by function (what only the second action runs)
#   run <scenario.scn> [--name N] [--window]   run a scenario with the hooks on (headless CLI host) into the run store, then report
#   report [run]                    everything a run holds: hooks with typed arguments, watchpoints, the census, coverage files, observed edges
#   table [run]                     every hook on one line: hits per phase (each counts*.tsv), records, threads, callers, values
#   edges [run]                     computed call sites resolved by what was seen: (call site, function called, count) → edges.tsv
#   propose [run] <a> <b> --label L   functions whose blocks ran only in b, appended to re/live/proposals.tsv as naming leads
#   view [run] main+0x… [--cov F]   one function's decompiled text with each line's coverage, hits and values → re/live/view/<run>/<offset>.html
#   cost [run]                      cost=1 hooks: instructions and cycles a call ran on its thread (net of the hooks), per call and in all; with
#                                   phase markers (ab=1 hooks, g_live_ab) each hook's on / off difference, paired by neighbouring phases
#   ab [run] [--frames N]           an in-run comparison (DM_NVN_UB phases): translate-thread CPU and every @metric of nvn.tsv, paired differences
#   draws [run] [--by v0|…|caller|fs] [--hook H] [--top N]   the tagged-draw log (tag=1 hooks): share of draws tied to a call, draws per tag value, callers of the rest
#   writes [run] [--tick A[-B]] [--watch N]       watch records with the game tick they fell in: who wrote this value at tick N
#   replay <recording> --hooks F [--name N]       replay a recorded session (replay.py record) with these hooks and watches armed from the start
# A run is a folder of $DM_ROOT/re/live/runs (its name, `latest`, or a path). blocks and trace-spec come from the snapshot (re.sh live-index)
# when it exists; without it they read the product's rewritten image and need the project venv's capstone
# The output can name game functions (the export's names) and carries runtime values: it stays in the run folder or under $DM_ROOT/re
import argparse
import bisect
import collections
import datetime
import html
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import guestsyms  # noqa: E402
import live_index  # noqa: E402

DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
LIVE = os.path.join(DM_ROOT, 're', 'live')
RUNS = os.path.join(LIVE, 'runs')

HDR = struct.Struct('<8sQQQII')          # dm_live_hdr: magic, base, size, timer_hz, rec_size, version
REC = struct.Struct('<IIQQQ4Q4QQ')       # dm_live_rec: hook, tid, t, lr, sp, x[4], cap[4], aux
RET = 0x80000000
WATCH = 0x40000000
TICK = 0x20000000   # a tick marker from the host: cap[0] = the tick number
AB = 0x10000000     # a phase marker: cap[0] = g_live_ab's new value


def read(path):
    with open(path, 'rb') as f: b = f.read()
    if len(b) < HDR.size: raise SystemExit(f'{path}: too short for a header')
    magic, base, size, hz, rs, ver = HDR.unpack_from(b, 0)
    if magic != b'DMLIVE01' or rs != REC.size: raise SystemExit(f'{path}: not a live hooks file (magic {magic!r}, record {rs} bytes)')
    hdr = {'base': base, 'size': size, 'timer_hz': hz, 'version': ver}
    every = [REC.unpack_from(b, o) for o in range(HDR.size, len(b) - REC.size + 1, REC.size)]
    recs = [r for r in every if r[0] not in (TICK, AB)]   # the markers go to the header: [(time, tick)] and [(time, value)], by time
    marks = sorted((r[2], r[9]) for r in every if r[0] == TICK); hdr['tick_t'] = [m[0] for m in marks]; hdr['tick_n'] = [m[1] for m in marks]
    hdr['ab'] = sorted((r[2], r[9]) for r in every if r[0] == AB)
    hooks = {}
    tsv = path + '.hooks.tsv'
    if os.path.exists(tsv):
        with open(tsv, encoding='utf-8') as f:
            for line in f:
                p = line.rstrip('\n').split('\t')
                if p and p[0].isdigit(): hooks[int(p[0])] = {'name': p[1], 'at': p[2], 'kind': p[3], 'spec': p[4] if len(p) > 4 else ''}
    return hdr, recs, hooks


def where(addr, hdr):
    off = addr - hdr['base']
    if 0 <= off < hdr['size']:
        w = f'main+{off:#x}'; n = guestsyms.lookup(w)
        return f'{w} {n}' if n else w
    return f'{addr:#x}'


def pct(v, q): return v[min(len(v) - 1, int(len(v) * q))] if v else 0


def tick_at(hdr, t):
    """The game tick a record's time fell in (the last marker at or before it); None before the first marker or in a run without markers."""
    i = bisect.bisect_right(hdr.get('tick_t') or [], t) - 1
    return hdr['tick_n'][i] if i >= 0 else None


def boot_ci(xs, seed=1, n=4000):
    """A 95% bootstrap interval of the mean."""
    import random
    r = random.Random(seed); k = len(xs); ms = sorted(sum(r.choice(xs) for _ in range(k)) / k for _ in range(n))
    return ms[int(n * .025)], ms[int(n * .975) - 1]


def paired(phases, key):
    """Mean on-phase value less the mean of its neighbouring off phases, per on phase: [(difference)]. phases = [(on, {key: value})] in order."""
    out = []
    for i, (on, v) in enumerate(phases):
        if not on or key not in v: continue
        adj = [phases[j][1][key] for j in (i - 1, i + 1) if 0 <= j < len(phases) and not phases[j][0] and key in phases[j][1]]
        if adj: out.append(v[key] - sum(adj) / len(adj))
    return out


def typed(at, kind, caps, ent, ix, spec=''):
    """What the snapshot says about a hook's values: the function's arguments by register (with their most frequent values when the hook sits
    on its first instruction), and a name for each capture that reads through an argument typed as a pointer to a known structure."""
    m = re.fullmatch(r'main\+0x([0-9a-f]+)', at or '')
    if not m or ix.db is None: return {}
    off = int(m.group(1), 16); f = ix.function(off)
    if not f: return {}
    out = {'function': f[2], 'entry': f[0] == off}; args = ix.args(f[0])
    # type=x0:Unit,x1:Game in the hook's spec (the engine ignores it): what a register points at, where the map's prototype doesn't say
    for hint in (spec.split('type=')[1].split()[0].split(',') if 'type=' in spec else []):
        rg, _, tn = hint.partition(':')
        if rg[:1] == 'x' and rg[1:].isdigit() and tn: args[int(rg[1:])] = (args.get(int(rg[1:]), (rg, ''))[0], tn + ' *')
    if f[0] == off and kind != 'insn':
        out['args'] = [{'reg': f'x{r}', 'name': n, 'type': t, 'values': [{'v': f'{v:#x}', 'n': k} for v, k in collections.Counter(x[5 + r] for x in ent).most_common(4)]}
                       for r, (n, t) in sorted(args.items()) if r < 4]
        labels = []
        for c in caps:
            cm = re.fullmatch(r'\[x(\d+)([+-]0x[0-9a-f]+)?\]\.[bhwq]', c)
            lab = ix.field(args[int(cm.group(1))][1], int(cm.group(2) or '0', 16)) if cm and int(cm.group(1)) in args else None
            labels.append(lab or c)
        out['cap_names'] = labels
    return out


def summary(path, as_json=False, quiet=False, top=25, top_values=5):
    hdr, recs, hooks = read(path); ix = live_index.Index()
    hz = hdr['timer_hz'] or 1
    by = collections.defaultdict(list); watch = collections.defaultdict(list)
    for r in recs: (watch[(r[0] & 0xff, r[9] & ~7)] if r[0] & WATCH else by[r[0]]).append(r)   # a watch slot is reused: its records go by slot and address
    out = {'file': path, 'records': len(recs), 'hooks': [], 'watches': []}
    for slot, _ in sorted(watch):   # cap[0] the address, cap[1] the writing instruction, cap[2] the value after
        w = watch[(slot, _)]
        out['watches'].append({'watch': slot, 'addr': f'{w[0][9]:#x}', 'writes': sum(1 for r in w if r[13]), 'reads': sum(1 for r in w if not r[13]), 'threads': len({r[1] for r in w}),
                               'writers': [{'at': where(pc, hdr), 'n': n} for pc, n in collections.Counter(r[10] for r in w).most_common(8)],
                               'values': [{'v': f'{v:#x}', 'n': n} for v, n in collections.Counter(r[11] for r in w).most_common(8)]})
    for hid in sorted({h & ~RET for h in by} | set(hooks)):
        ent, ret = by.get(hid, []), by.get(hid | RET, [])
        h = {'hook': hid, **hooks.get(hid, {}), 'records': len(ent), 'returns': len(ret), 'threads': len({r[1] for r in ent + ret})}
        ts = sorted(r[2] for r in ent)
        if len(ts) > 1:
            gaps = sorted((b - a) / hz * 1e3 for a, b in zip(ts, ts[1:]))
            h['per_s'] = round((len(ts) - 1) / ((ts[-1] - ts[0]) / hz), 2) if ts[-1] > ts[0] else None
            h['gap_ms'] = {'p50': round(pct(gaps, .5), 3), 'p95': round(pct(gaps, .95), 3)}
        h['callers'] = [{'at': where(lr, hdr), 'n': n} for lr, n in collections.Counter(r[3] for r in ent).most_common(5)]
        if ret:
            d = sorted(r[13] / hz * 1e6 for r in ret)
            h['dur_us'] = {'p50': round(pct(d, .5), 2), 'p95': round(pct(d, .95), 2), 'max': round(d[-1], 2)}
            h['ret_x0'] = [{'v': f'{v:#x}', 'n': n} for v, n in collections.Counter(r[5] for r in ret).most_common(5)]
        spec = hooks.get(hid, {}).get('spec', ''); caps = [c for c in (spec.split('cap=')[1].split()[0].split(',') if 'cap=' in spec else []) if c][:4]
        if ret and re.search(r'(^| )cost=1', spec):   # a cost hook's return records: cap 0, 1 = net instructions and cycles of the call
            ins = sorted(r[9] for r in ret); cyc = sorted(r[10] for r in ret)
            h['cost'] = {'calls': len(ret), 'instr': {'p50': pct(ins, .5), 'p95': pct(ins, .95), 'mean': round(sum(ins) / len(ins), 1), 'total': sum(ins)},
                         'cycles': {'p50': pct(cyc, .5), 'mean': round(sum(cyc) / len(cyc), 1), 'total': sum(cyc)}}
        if caps and caps[0].startswith('str:'):   # cap=str:<source>: the four slots hold up to 32 bytes of text
            texts = collections.Counter(struct.pack('<4Q', *r[9:13]).split(b'\0')[0].decode('utf-8', 'replace') for r in ent)
            h['caps'] = [[{'v': t, 'n': n} for t, n in texts.most_common(top_values)]]; h['texts'] = len(texts)
        else:
            h['caps'] = [[{'v': f'{v:#x}', 'n': n} for v, n in collections.Counter(r[9 + i] for r in ent).most_common(top_values)] for i in range(len(caps))]
        h.update(typed(h.get('at', ''), h.get('kind', ''), caps, ent, ix, spec))
        out['hooks'].append(h)
    if as_json: print(json.dumps(out, indent=1)); return out
    if quiet: return out
    print(f"{path}: {len(recs)} records, module at {hdr['base']:#x}, timer {hz} Hz")
    for w in out['watches']:
        print(f"watch {w['watch']} on {w['addr']}: {w['writes']} writes, {w['reads']} reads, {w['threads']} threads")
        for c in w['writers']: print(f"    accessed by {c['at']}  ×{c['n']}")
        print(f"    values {', '.join(f'{x['v']}×{x['n']}' for x in w['values'])}")
    shown = sorted((h for h in out['hooks'] if h['records'] or h['returns']), key=lambda h: -(h['records'] + h['returns']))
    if len(shown) > top: print(f'{len(shown)} hooks recorded; the {top} with most records (summary --json has all):')
    for h in shown[:top]:
        print(f"hook {h['hook']} {h.get('name', '?')} ({h.get('at', '?')}, {h.get('kind', '?')}): {h['records']} records, {h['returns']} returns, {h['threads']} threads"
              + (f", {h['per_s']}/s, gap p50 {h['gap_ms']['p50']} ms p95 {h['gap_ms']['p95']} ms" if h.get('per_s') else ''))
        for c in h['callers']: print(f"    called from {c['at']}  ×{c['n']}")
        if 'dur_us' in h: print(f"    duration p50 {h['dur_us']['p50']} us, p95 {h['dur_us']['p95']} us, max {h['dur_us']['max']} us; x0 at return {', '.join(f'{x['v']}×{x['n']}' for x in h['ret_x0'])}")
        if 'cost' in h: print(f"    cost per call: {h['cost']['instr']['p50']} instructions (p95 {h['cost']['instr']['p95']}), {h['cost']['cycles']['p50']} cycles; {h['cost']['instr']['total']} instructions in {h['cost']['calls']} calls")
        for a in h.get('args', []):
            if a['values']: print(f"    {a['reg']} {a['name']} ({a['type']}): {', '.join(f'{x['v']}×{x['n']}' for x in a['values'])}")
        for i, c in enumerate(h['caps']): print(f"    cap {(h.get('cap_names') or [])[i] if i < len(h.get('cap_names') or []) else i}: {', '.join(f'{x['v']}×{x['n']}' for x in c)}")
    return out


def tick_range(text):
    if not text: return None
    a, _, b = str(text).partition('-'); return int(a), int(b or a)


def dump(path, hook=None, limit=50, tick=None):
    hdr, recs, hooks = read(path); hz = hdr['timer_hz'] or 1; n = 0; tr = tick_range(tick)
    recs.sort(key=lambda r: r[2]); t0 = recs[0][2] if recs else 0
    for r in recs:
        tk = tick_at(hdr, r[2]); tt = '' if tk is None else f' tick {tk}'
        if tr and (tk is None or not tr[0] <= tk <= tr[1]): continue
        if r[0] & WATCH:
            if hook is None: print(f"{(r[2] - t0) / hz:10.6f} s{tt}  watch {r[0] & 0xff} tid {r[1]}  {r[9]:#x} = {r[11]:#x}  by {where(r[10], hdr)}")
            continue
        hid = r[0] & ~RET
        if hook is not None and hid != hook: continue
        kind = 'ret' if r[0] & RET else 'hit'
        extra = f" dur {r[13] / hz * 1e6:.1f} us" if r[0] & RET else ' cap ' + ' '.join(f'{c:#x}' for c in r[9:13])
        print(f"{(r[2] - t0) / hz:10.6f} s{tt}  hook {hid} {hooks.get(hid, {}).get('name', '?')} {kind} tid {r[1]}  x0 {r[5]:#x} x1 {r[6]:#x}  from {where(r[3], hdr)}{extra}")
        n += 1
        if n >= limit: break


def functions(only=None):
    """main's functions (start, size) from the guest symbol table, optionally within main+0x…-main+0x…"""
    ix = live_index.Index()
    fns = [(r[0], r[1]) for r in ix.db.execute('select func, max(off) + 4 - func from blocks group by func order by func')] if ix.db is not None and ix.one('select 1 from blocks limit 1') else \
        [(s_, n) for s_, n, _, _ in guestsyms.load().get('main', [])]
    if only:
        lo, hi = (int(x.split('+')[-1], 16) for x in only.split('-'))
        fns = [f for f in fns if lo <= f[0] < hi]
    if not fns: raise SystemExit('no functions: build the symbol table first (python3 dm/tools/guestsyms.py build)')
    return fns


def image():
    with open(os.path.join(guestsyms.product_dir(), 'main.patched.bin'), 'rb') as f: return f.read()


def blocks(out, only=None):
    ix = live_index.Index()
    if ix.db is not None and ix.one('select 1 from blocks limit 1'):   # the snapshot has them: no disassembly
        lo, hi = (int(x.split('+')[-1], 16) for x in only.split('-')) if only else (0, 1 << 40)
        rows = ix.db.execute('select off from blocks where func >= ? and func < ? order by off', (lo, hi)).fetchall()
        with open(out, 'w', encoding='utf-8') as f: f.writelines(f'{r[0]:x}\n' for r in rows)
        print(f'BLOCKS {len(rows)} → {out} (from the snapshot)'); return
    import capstone   # the project venv has it
    import quiet
    quiet.wait_quiet('live.py blocks')   # seconds of full CPU: never beside a gate's game tiers or a benchmark
    img = image(); md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); n = 0
    with open(out, 'w', encoding='utf-8') as f:
        for s_, size in functions(only):
            lead, _ = live_index.scan_function(((ad, mn, ops) for ad, _, mn, ops in md.disasm_lite(img[s_:s_ + size], s_)), s_, s_ + size)
            for x in lead: f.write(f'{x:x}\n')
            n += len(lead)
    print(f'BLOCKS {n} → {out}')


def trace_spec(out, only=None, records=0):
    fns = functions(only)
    with open(out, 'w', encoding='utf-8') as f:
        for s_, _ in fns: f.write(f'at=main+{s_:#x} ' + (f'max={records} rate=0\n' if records else 'count=1 rate=0\n'))
    print(f'TRACE-SPEC {len(fns)} functions → {out}')


def counts(path, top=40):
    rows = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if p[0].isdigit(): rows.append((int(p[3]), p[1], int(p[6])))
    ran = [r for r in rows if r[0]]; total = sum(r[0] for r in ran)
    print(f'{path}: {len(rows)} functions hooked, {len(ran)} ran, {total} calls, {sum(1 for r in rows if not r[2])} disarmed (over their rate)')
    for n, at, armed in sorted(ran, reverse=True)[:top]: print(f"{n:12d}  {at} {guestsyms.lookup(at)}{'' if armed else '  (disarmed: a lower bound)'}")


def cov_read(path):
    with open(path, encoding='utf-8') as f: return {int(line.split('\t')[0], 16) for line in f if line.strip()}


def by_function(offs):
    import bisect
    fns = sorted((s_, n) for s_, n, _, _ in guestsyms.load().get('main', [])); starts = [f[0] for f in fns]; per = collections.Counter()
    for o in offs:
        i = bisect.bisect_right(starts, o) - 1
        if i >= 0 and o < fns[i][0] + fns[i][1]: per[fns[i][0]] += 1
    return per


def cover(path, top=40):
    offs = cov_read(path); per = by_function(offs)
    print(f'{path}: {len(offs)} blocks ran in {len(per)} functions')
    for s_, n in per.most_common(top): print(f'{n:6d} blocks  main+{s_:#x} {guestsyms.lookup(f"main+{s_:#x}")}')


def cover_diff(a, b, top=60):
    only = cov_read(b) - cov_read(a); per = by_function(only)
    print(f'{len(only)} blocks ran in {b} and not in {a}, in {len(per)} functions')
    for s_, n in per.most_common(top): print(f'{n:6d} blocks  main+{s_:#x} {guestsyms.lookup(f"main+{s_:#x}")}')


# ---- the run store: $DM_ROOT/re/live/runs/<time>_<name>/ (private: runtime values and game function names) ----
def resolve_run(arg=None):
    """A run folder from its name, `latest` (or nothing), or a path."""
    if arg and os.path.isdir(arg): return os.path.abspath(arg)
    if arg and arg != 'latest':
        p = os.path.join(RUNS, arg)
        if os.path.isdir(p): return p
        raise SystemExit(f'no run {arg} in {RUNS}')
    runs = sorted(d for d in (os.listdir(RUNS) if os.path.isdir(RUNS) else []) if os.path.isdir(os.path.join(RUNS, d)))
    if not runs: raise SystemExit(f'no runs in {RUNS} yet (live.py run <scenario>)')
    return os.path.join(RUNS, runs[-1])


def new_run(name):
    d = os.path.join(RUNS, f"{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}_{re.sub(r'[^A-Za-z0-9_-]', '_', name)}"); os.makedirs(d, exist_ok=True); return d


def run_meta(d, **more):
    """run.json: what produced the run, so a result can be tied to a build and the snapshot it was read with."""
    repo = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    sha = subprocess.run(['git', '-C', repo, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, check=False).stdout.strip()
    prod = os.path.basename(guestsyms.product_dir()); im = live_index.Index().meta()
    meta = dict(id=os.path.basename(d), when=datetime.datetime.now().isoformat(timespec='seconds'), host=sha, product=prod,
                index_product=im.get('product'), index_matches=im.get('product') == prod, **more)
    with open(os.path.join(d, 'run.json'), 'w', encoding='utf-8') as f: json.dump(meta, f, indent=1)
    return meta


def cmd_run(scn, name=None, window=False):
    d = new_run(name or os.path.splitext(os.path.basename(scn))[0])
    repo = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    cmd = [sys.executable, os.path.join(repo, 'dm', 'tools', 'scenario.py'), 'run', scn, '--backend', 'cli', '--out', d, '--env', f'DM_LIVE=1 DM_LIVE_OUT={d}/live.bin'] + ([] if window else ['--headless'])
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)
    with open(os.path.join(d, 'scenario.log'), 'w', encoding='utf-8') as f: f.write(p.stdout + p.stderr)
    verdict = next((ln for ln in reversed(p.stdout.splitlines()) if ln.startswith('SCENARIO ')), 'no verdict line')
    with open(scn, encoding='utf-8') as f: text = f.read()
    run_meta(d, scenario=os.path.abspath(scn), scenario_text=text, verdict=verdict)
    print(verdict); print(f'RUN {os.path.basename(d)} → {d}')
    if p.returncode == 0: report(d)
    return p.returncode


def edges(d, quiet=False):
    """Calls the code makes through a register, resolved by what was seen: a recorded call whose return address follows a computed call site
    names that site's target. Writes edges.tsv (site, target function, count) in the run folder."""
    path = os.path.join(d, 'live.bin')
    if not os.path.exists(path): return []
    hdr, recs, hooks = read(path); ix = live_index.Index(); seen = collections.Counter()
    for r in recs:
        if r[0] & (RET | WATCH): continue
        h = hooks.get(r[0]); m = re.fullmatch(r'main\+0x([0-9a-f]+)', (h or {}).get('at', ''))
        if not m or h.get('kind') == 'insn': continue
        site = r[3] - hdr['base'] - 4; c = ix.call(site) if 0 <= site < hdr['size'] else None
        if c and c[1] is None: seen[(site, int(m.group(1), 16))] += 1
    rows = sorted(((s_, t, n) for (s_, t), n in seen.items()), key=lambda x: -x[2])
    with open(os.path.join(d, 'edges.tsv'), 'w', encoding='utf-8') as f:
        f.write('site\ttarget\tcount\n'); f.writelines(f'main+{s_:#x}\tmain+{t:#x}\t{n}\n' for s_, t, n in rows)
    if not quiet:
        print(f'{len(rows)} computed call sites resolved → {os.path.join(d, "edges.tsv")}')
        for s_, t, n in rows[:20]: print(f'{n:8d}  main+{s_:#x} {guestsyms.lookup(f"main+{s_:#x}")}  →  main+{t:#x} {guestsyms.lookup(f"main+{t:#x}")}')
    return rows


def table(d, callers=2):
    """Every hook of a run on one line: its hits in each counts*.tsv the run dumped (a phase each), records, threads, top callers and values.
    For runs that arm one hook per question (a check list): the name carries the question's id."""
    phases = sorted((f for f in os.listdir(d) if f.startswith('counts') and f.endswith('.tsv')), key=lambda f: os.path.getmtime(os.path.join(d, f)))
    hits = collections.defaultdict(dict); names = {}
    for ph in phases:
        with open(os.path.join(d, ph), encoding='utf-8') as f:
            for line in f:
                p = line.rstrip('\n').split('\t')
                if p[0].isdigit(): hits[int(p[0])][ph] = (int(p[3]), int(p[4])); names[int(p[0])] = (p[2], p[1])
    summ = {h['hook']: h for h in summary(os.path.join(d, 'live.bin'), quiet=True, top_values=6)['hooks']} if os.path.exists(os.path.join(d, 'live.bin')) else {}
    print('hook\tname\tat\t' + '\t'.join(ph[7:-4] or 'counts' for ph in phases) + '\trecords\tthreads\tdetail')
    for hid in sorted(names, key=lambda i: names[i][0]):
        h = summ.get(hid, {}); det = []
        for c in h.get('callers', [])[:callers]: det.append(f"from {c['at']} ×{c['n']}")
        for a in h.get('args', []):
            if a['values']: det.append(f"{a['reg']}: " + ' '.join(f"{v['v']}×{v['n']}" for v in a['values'][:4]))
        for i, c in enumerate(h.get('caps', [])): det.append(f"{(h.get('cap_names') or [])[i] if i < len(h.get('cap_names') or []) else 'cap'}: " + ' '.join(f"{v['v']}×{v['n']}" for v in c))
        if 'dur_us' in h: det.append(f"dur p50 {h['dur_us']['p50']} p95 {h['dur_us']['p95']} max {h['dur_us']['max']} us")
        cells = [(f"{hits[hid][ph][0]}" + (f"/{hits[hid][ph][1]}m" if hits[hid][ph][1] != hits[hid][ph][0] else '')) if ph in hits[hid] else '-' for ph in phases]
        print(f"{hid}\t{names[hid][0]}\t{names[hid][1]}\t" + '\t'.join(cells) + f"\t{h.get('records', 0)}\t{h.get('threads', 0)}\t" + ' | '.join(det))


def report(d):
    meta = {}
    if os.path.exists(os.path.join(d, 'run.json')):
        with open(os.path.join(d, 'run.json'), encoding='utf-8') as f: meta = json.load(f)
        print(f"run {meta.get('id')}: host {meta.get('host')}, product {meta.get('product')}" + ('' if meta.get('index_matches') else '  ⚠️ the snapshot was built for another product: re.sh live-index') + f"; {meta.get('verdict', '')}")
    out = {}
    if os.path.exists(os.path.join(d, 'live.bin')): out['summary'] = summary(os.path.join(d, 'live.bin')); out['edges'] = len(edges(d))
    for f in sorted(os.listdir(d)):
        if f.startswith('counts') and f.endswith('.tsv'): counts(os.path.join(d, f), 15)
        if f.startswith('cov') and f.endswith('.tsv'): cover(os.path.join(d, f), 10)
    with open(os.path.join(d, 'summary.json'), 'w', encoding='utf-8') as f: json.dump(out, f)
    return out


def cost(d):
    """cost=1 hooks of a run: what each call ran on its own thread, net of the hooks (and of cost hooks nested inside it). With phase markers,
    each hook's difference between the phases where ab=1 hooks acted and their neighbours where they did not."""
    path = os.path.join(d, 'live.bin'); hdr, recs, hooks = read(path); hz = hdr['timer_hz'] or 1
    ids = [h for h, v in hooks.items() if re.search(r'(^| )cost=1', v.get('spec', ''))]
    if not ids: print('no cost=1 hook in this run'); return []
    span = (max(r[2] for r in recs) - min(r[2] for r in recs)) / hz if recs else 0; rows = []
    print('hook\tname\tat\tcalls\tinstr p50\tinstr p95\tinstr mean\tcycles p50\tinstr total\tinstr/s')
    for hid in ids:
        ret = [r for r in recs if r[0] == hid | RET]
        if not ret: print(f"{hid}\t{hooks[hid]['name']}\t{hooks[hid]['at']}\t0"); continue
        ins = sorted(r[9] for r in ret); cyc = sorted(r[10] for r in ret)
        rows.append({'hook': hid, 'name': hooks[hid]['name'], 'calls': len(ret), 'instr_p50': pct(ins, .5), 'instr_mean': sum(ins) / len(ins), 'instr_total': sum(ins), 'cycles_p50': pct(cyc, .5)})
        print(f"{hid}\t{hooks[hid]['name']}\t{hooks[hid]['at']}\t{len(ret)}\t{pct(ins, .5)}\t{pct(ins, .95)}\t{sum(ins) / len(ins):.1f}\t{pct(cyc, .5)}\t{sum(ins)}\t{sum(ins) / span if span else 0:.0f}")
    ab = hdr.get('ab') or []
    if len(ab) >= 4:   # phases between markers; the first 0.25 s of each left out (the switch lands within 20 ms, caches settle)
        print(f'{len(ab)} phase changes: on = the phases where ab=1 hooks acted; difference = on less the mean of the neighbouring off phases (95% interval)')
        for hid in ids:
            ph = []
            for (t0, v), (t1, _) in zip(ab, ab[1:]):
                rr = [r for r in recs if r[0] == hid | RET and t0 + hz // 4 <= r[2] < t1]
                d_ = {'calls_s': len(rr) / ((t1 - t0 - hz // 4) / hz)} if t1 - t0 > hz // 4 else {}
                if rr: d_['instr'] = sum(r[9] for r in rr) / len(rr); d_['cycles'] = sum(r[10] for r in rr) / len(rr); d_['instr_s'] = sum(r[9] for r in rr) / ((t1 - t0 - hz // 4) / hz)
                ph.append((bool(v), d_))
            line = f"{hid}\t{hooks[hid]['name']}"
            for key, label in (('instr', 'instructions a call'), ('cycles', 'cycles a call'), ('calls_s', 'calls a second'), ('instr_s', 'instructions a second')):
                df = paired(ph, key); off = [v[key] for on, v in ph if not on and key in v]
                if len(df) > 2 and off: lo, hi = boot_ci(df); line += f"\t{label}: off {sum(off) / len(off):.1f}, on {sum(df) / len(df):+.1f} [{lo:+.1f}, {hi:+.1f}] n={len(df)}"
            print(line)
    return rows


def ab(d, frames=180):
    """An in-run comparison driven by DM_NVN_UB (phases of N frames, base and each named class in turn): the translate thread's CPU and cycles a
    frame from the host log's phase lines, and every @metric of nvn.tsv by phase (frames 45 on), as paired differences with a 95% interval."""
    import statistics
    lp = os.path.join(d, 'host.log')
    if not os.path.exists(lp): raise SystemExit(f'no host.log in {d}')
    with open(lp, encoding='utf-8', errors='replace') as f: log = f.read()
    ph = {}
    for m in re.finditer(r'UB 上屏 (\d+) (\S+) 帧 \d+ 均 [\d.]+ ms 2拍 \d+ 3拍 \d+ 4拍以上 \d+ 丢弃 \d+ 翻译每帧 CPU ([\d.]+) ms 周期 ([\d.]+) M', log):
        ph[int(m.group(1))] = {'name': m.group(2), 'translate_cpu_ms': float(m.group(3)), 'translate_Mcycles': float(m.group(4))}
    if not ph: raise SystemExit('no DM_NVN_UB phase lines in the host log (run with DM_NVN_UB="<name>=@set:g_live_ab=1" and DM_NVN_TSV for the metrics)')
    tsv = collections.defaultdict(lambda: collections.defaultdict(list)); tp = os.path.join(d, 'nvn.tsv')
    if os.path.exists(tp):
        with open(tp, encoding='utf-8') as f:
            for line in f:
                c = line.rstrip('\n').split('\t')
                if len(c) == 3 and c[1].startswith('@') and int(c[0]) % frames >= 45: tsv[int(c[0]) // frames][c[1]].append(float(c[2]))
    for k in ph:
        for m, v in tsv.get(k, {}).items(): ph[k][m] = statistics.mean(v)
    names = sorted({v['name'] for v in ph.values()} - {'base'}); order = sorted(k for k in ph if k >= 2)
    keys = ['translate_cpu_ms', 'translate_Mcycles'] + sorted({m for k in ph for m in ph[k] if m.startswith('@')})
    print(f"{len(ph)} phases of {frames} frames; base phases {sum(1 for k in order if ph[k]['name'] == 'base')}")
    for key in keys:
        base = [ph[k][key] for k in order if ph[k]['name'] == 'base' and key in ph[k]]
        if not base: continue
        row = f'{key:20s} base {statistics.mean(base):10.3f}'
        for nm in names:
            df = paired([(ph[k]['name'] == nm, ph[k]) for k in order if ph[k]['name'] in ('base', nm)], key)
            if len(df) > 2: lo, hi = boot_ci(df); row += f' | {nm} {statistics.mean(df):+9.3f} [{lo:+.3f}, {hi:+.3f}] n={len(df)}'
        print(row)


def draws_read(d):
    path = os.path.join(d, 'live.bin.draws.tsv')
    if not os.path.exists(path): raise SystemExit(f'no draw log in {d} (arm a kind=ret tag=1 hook; the recorder then logs draws to live.bin.draws.tsv)')
    rows = []
    with open(path, encoding='utf-8') as f:
        cols = f.readline().rstrip('\n').split('\t')
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= len(cols): rows.append(dict(zip(cols, p)))
    return rows


def draws(d, by='v0', top=30, hook=None):
    """The tagged-draw log: how many draws were made inside a tagged call, the draws per value of one tag column (with the shader pairs they
    used), and for the draws outside any tagged call, the call sites in the game that made them (where the next tag hook should go)."""
    rows = draws_read(d); hooks = read(os.path.join(d, 'live.bin'))[2] if os.path.exists(os.path.join(d, 'live.bin')) else {}
    tagged = [r for r in rows if r['hook'] != '-1']; frames = {r['frame'] for r in rows}
    print(f'{len(rows)} draws in {len(frames)} frames; {len(tagged)} ({100 * len(tagged) / max(1, len(rows)):.1f}%) inside a tagged call')
    for hk, n in collections.Counter(r['hook'] for r in tagged).most_common():
        hr = [r for r in tagged if r['hook'] == hk]
        print(f"  hook {hk} {hooks.get(int(hk), {}).get('name', '?')}: {n} draws, {n / max(1, len(frames)):.1f} a frame; distinct v0 {len({r['v0'] for r in hr})}, v1 {len({r['v1'] for r in hr})}, v2 {len({r['v2'] for r in hr})}, v3 {len({r['v3'] for r in hr})}; shader pairs {len({(r['vs'], r['fs']) for r in hr})}")
    grp = collections.defaultdict(list); pick = None
    if hook is not None:   # --hook: one tag hook's draws only (a column means something else under each hook)
        pick = next((str(i) for i, h in hooks.items() if str(i) == str(hook) or h.get('name') == hook), str(hook))
    for r in tagged:
        if pick is None or r['hook'] == pick: grp[r.get(by, '')].append(r)
    print(f"by {by}{'' if pick is None else f' of hook {pick}'} ({len(grp)} values), most draws first:")
    for k, rr in sorted(grp.items(), key=lambda kv: -len(kv[1]))[:top]:
        sh = collections.Counter((r['vs'], r['fs']) for r in rr).most_common(3)
        print(f"  {k:>18s}  {len(rr):7d} draws  {sum(int(r['count']) * max(1, int(r['instances'])) for r in rr):10d} indices  shaders " + ', '.join(f'{v[:8]}/{f[:8]}×{n}' for (v, f), n in sh))
    rest = [r for r in rows if r['hook'] == '-1']
    if rest:
        print(f'{len(rest)} draws outside any tagged call, by the game call site that made them:')
        for c, n in collections.Counter(r['caller'] for r in rest).most_common(12): print(f"  {n:8d}  main+0x{c} {guestsyms.lookup(f'main+0x{c}')}")
    return {'draws': len(rows), 'tagged': len(tagged), 'frames': len(frames)}


def writes(d, tick=None, watch=None, limit=200):
    """Watch records with the game tick each fell in: who wrote (or read) the watched bytes at tick N, and the value then."""
    hdr, recs, _ = read(os.path.join(d, 'live.bin')); tr = tick_range(tick); n = 0
    for r in sorted((r for r in recs if r[0] & WATCH), key=lambda r: r[2]):
        tk = tick_at(hdr, r[2])
        if tr and (tk is None or not tr[0] <= tk <= tr[1]): continue
        if watch is not None and r[0] & 0xff != watch: continue
        print(f"tick {tk if tk is not None else '?'}\twatch {r[0] & 0xff}\t{'write' if r[13] else 'read'}\t{r[9]:#x} = {r[11]:#x}\tby {where(r[10], hdr)}\tcalled from {where(r[3], hdr)}\ttid {r[1]}")
        n += 1
        if n >= limit: print(f'… (--limit {limit})'); break
    if not n: print('no watch record' + (f' in ticks {tick}' if tick else '') + (' (and no tick markers: the run never entered a game?)' if not hdr.get('tick_t') else ''))
    return n


def cmd_replay(rec, hooks_file, name=None, dmhost=None):
    """Replay a recorded session (replay.py record: command packets by tick) with hooks and watches armed from the process's start: the same
    session can be asked a new question each time. The replay's per-tick state is compared with the recording, so a hook that changed the game
    shows as the first divergent tick."""
    sys.path.insert(0, os.path.dirname(HERE))
    import replay   # noqa: PLC0415
    import tickdiff   # noqa: PLC0415
    rec = os.path.abspath(rec); d = new_run(name or 'replay_' + os.path.basename(rec.rstrip('/')))
    os.environ.update(DM_LIVE='1', DM_LIVE_OUT=os.path.join(d, 'live.bin'), DM_LIVE_HOOKS='@' + os.path.abspath(hooks_file))
    rc, ticks = replay.replay(rec, dmhost or replay.default_host(), os.path.join(d, 'replay'))
    cmp_ = 'not compared (no per-tick state)'
    if os.path.exists(ticks) and os.path.exists(os.path.join(rec, 'ticks.jsonl')):
        c = tickdiff.compare(tickdiff.load(rec), tickdiff.load(ticks))
        cmp_ = f"identical to the recording over {c['common']} ticks" if c['same'] else f"differ from the recording first at tick {(c['first'] or {}).get('tick')} ({c['diverged']} of {c['common']} ticks): a hook changed the game, or the replay is not deterministic"
    with open(hooks_file, encoding='utf-8') as f: text = f.read()
    verdict = f'replay exit {rc}; ticks {cmp_}'
    run_meta(d, scenario=f'replay of {rec}', scenario_text=text, verdict=verdict)
    for nm in ('host.log',):
        if os.path.exists(os.path.join(d, 'replay', nm)): os.replace(os.path.join(d, 'replay', nm), os.path.join(d, nm))
    print(verdict); print(f'RUN {os.path.basename(d)} → {d}')
    if os.path.exists(os.path.join(d, 'live.bin')): report(d)
    return rc


def propose(d, a, b, label):
    """Functions with blocks that ran only in coverage file b: naming leads, appended to re/live/proposals.tsv for review (never written to the map by a tool)."""
    only = cov_read(os.path.join(d, b)) - cov_read(os.path.join(d, a)); per = by_function(only); ix = live_index.Index(); rows = []
    for s_, n in per.most_common():
        f = ix.function(s_); total = len(ix.leaders(s_)) or 1
        rows.append((s_, f[2] if f else '', n, total, label, os.path.basename(d)))
    p = os.path.join(LIVE, 'proposals.tsv'); new = not os.path.exists(p)
    with open(p, 'a', encoding='utf-8') as f:
        if new: f.write('# Leads from live runs, for review: a function whose blocks ran only during the labelled action. A lead, not a name\noffset\tname_now\tblocks_only_then\tblocks\tlabel\trun\n')
        f.writelines(f'{s_:#x}\t{nm}\t{n}\t{t}\t{lab}\t{run}\n' for s_, nm, n, t, lab, run in rows)
    print(f'{len(rows)} functions ran only during "{label}" → {p}')
    for s_, nm, n, t, _, _ in rows[:15]: print(f'{n:5d}/{t:<5d} main+{s_:#x} {nm or guestsyms.lookup(f"main+{s_:#x}")}')
    return rows


# ---- the annotated view: one function's decompiled text with what a run saw, line by line (a local file, never published) ----
VIEW_CSS = """body{background:#14110f;color:#d8cfc4;font:13px/1.5 ui-monospace,Menlo,monospace;margin:0;padding:16px 16px 48px}
h1{font:600 16px system-ui;margin:0 0 4px}p.meta{font:12px system-ui;color:#9a8f82;margin:0 0 12px}
table{border-collapse:collapse;width:100%}td{padding:0 8px;vertical-align:top;white-space:pre}
td.n{color:#6f665c;text-align:right;user-select:none}td.h{color:#c9a15a;text-align:right}td.v{color:#8fb39a;white-space:normal;font-size:12px}
tr.ran td.c{border-left:3px solid #5f9a6b}tr.not td.c{border-left:3px solid #8a4a43;color:#8d8377}tr.none td.c{border-left:3px solid transparent}
span.k{display:inline-block;padding:0 8px;margin-right:8px;font:12px system-ui}span.ran{border-left:3px solid #5f9a6b}span.not{border-left:3px solid #8a4a43}"""


def view_rows(decomp, leaders, ran, hits, values):
    """Each decompiled line with its state: 'ran' when the block of one of its instructions ran, 'not' when it has instructions and none did,
    'none' without instructions (or without coverage: ran is None); plus the hits and values of hooks on its instructions."""
    rows = []; shown = set()
    for ln in decomp['lines']:
        addrs = ln.get('addrs') or []
        if ran is None or not addrs: state = 'none'
        else:
            blk = {leaders[bisect.bisect_right(leaders, a) - 1] for a in addrs if leaders and a >= leaders[0]}
            state = 'ran' if blk & ran else 'not'
        fresh = [a for a in addrs if a not in shown and (a in hits or a in values)]; shown.update(fresh)   # a hooked instruction's numbers go on the first line that holds it
        h = sum(hits.get(a, 0) for a in fresh); v = '; '.join(values[a] for a in fresh if a in values)
        rows.append((ln['n'], state, h, v, ln['text']))
    return rows


def view(d, where_, cov=None):
    off = int(where_.split('+')[-1], 16); ix = live_index.Index(); f = ix.function(off)
    entry = f[0] if f else off; dj = os.path.join(LIVE, 'decomp', f'{entry:x}.json')
    if not os.path.exists(dj):   # the cache is built from the project, which only one process may hold
        repo = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
        p = subprocess.run(['bash', os.path.join(repo, 'dm', 'tools', 're', 're.sh'), 'live-decomp', f'main+{entry:#x}'], capture_output=True, text=True, check=False)
        if not os.path.exists(dj): raise SystemExit(f'no decompiled text for main+{entry:#x}: {(p.stdout + p.stderr).strip()[-300:]} (re.sh live-decomp needs the query server stopped)')
    with open(dj, encoding='utf-8') as fh: decomp = json.load(fh)
    covs = [cov] if cov else sorted(x for x in os.listdir(d) if x.startswith('cov') and x.endswith('.tsv'))
    ran = set().union(*(cov_read(os.path.join(d, c)) for c in covs)) if covs else None
    hits, values = {}, {}
    cp = os.path.join(d, 'counts.tsv')
    if os.path.exists(cp):
        with open(cp, encoding='utf-8') as fh:
            for line in fh:
                p = line.rstrip('\n').split('\t')
                if p[0].isdigit() and int(p[3]): hits[int(p[1].split('+')[-1], 16)] = int(p[3])
    if os.path.exists(os.path.join(d, 'live.bin')):
        for h in summary(os.path.join(d, 'live.bin'), quiet=True)['hooks']:
            m = re.fullmatch(r'main\+0x([0-9a-f]+)', h.get('at', ''))
            if not m: continue
            a = int(m.group(1), 16); hits[a] = max(hits.get(a, 0), h['records'])
            parts = [f"{x['reg']} {x['name']}: " + ', '.join(f"{v['v']}×{v['n']}" for v in x['values']) for x in h.get('args', [])]
            parts += [f"{(h.get('cap_names') or [])[i] if i < len(h.get('cap_names') or []) else 'cap'}: " + ', '.join(f"{v['v']}×{v['n']}" for v in c) for i, c in enumerate(h['caps'])]
            if 'dur_us' in h: parts.append(f"returns after p50 {h['dur_us']['p50']} µs")
            if parts: values[a] = ' | '.join(parts)
    rows = view_rows(decomp, ix.leaders(entry), ran, hits, values)
    nr = sum(1 for r in rows if r[1] == 'ran'); nn = sum(1 for r in rows if r[1] == 'not')
    out_dir = os.path.join(LIVE, 'view', os.path.basename(d)); os.makedirs(out_dir, exist_ok=True); out = os.path.join(out_dir, f'{entry:x}.html')
    body = '\n'.join(f'<tr class="{st}"><td class="n">{n}</td><td class="h">{h or ""}</td><td class="c">{html.escape(text)}</td><td class="v">{html.escape(v)}</td></tr>' for n, st, h, v, text in rows)
    title = f"main+{entry:#x} {decomp.get('name', '')}"
    with open(out, 'w', encoding='utf-8') as fh:
        fh.write(f'<!doctype html><meta charset="utf-8"><title>{html.escape(title)}</title><style>{VIEW_CSS}</style><h1>{html.escape(title)}</h1>'
                 f'<p class="meta">run {html.escape(os.path.basename(d))} · coverage: {html.escape(", ".join(covs) if covs else "none in this run")} · '
                 f'<span class="k ran">{nr} lines ran</span><span class="k not">{nn} did not</span>private: never share this file</p>'
                 f'<table><tr><td class="n">line</td><td class="h">hits</td><td class="c">decompiled</td><td class="v">seen</td></tr>{body}</table>')
    print(f'VIEW main+{entry:#x}: {len(rows)} lines, {nr} ran, {nn} did not, {sum(1 for r in rows if r[2])} with hits → {out}')
    return out


def main(argv):
    ap = argparse.ArgumentParser(prog='live.py', description='Read the live hooks output (dm_live.c).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('summary'); s.add_argument('file'); s.add_argument('--json', action='store_true')
    d = sub.add_parser('dump'); d.add_argument('file'); d.add_argument('--hook', type=int); d.add_argument('--limit', type=int, default=50); d.add_argument('--tick', help='N or A-B: only records in these game ticks')
    b = sub.add_parser('blocks'); b.add_argument('--out', required=True); b.add_argument('--only')
    t = sub.add_parser('trace-spec'); t.add_argument('--out', required=True); t.add_argument('--only'); t.add_argument('--records', type=int, default=0)
    c = sub.add_parser('counts'); c.add_argument('file'); c.add_argument('--top', type=int, default=40)
    v = sub.add_parser('cover'); v.add_argument('file'); v.add_argument('--top', type=int, default=40)
    x = sub.add_parser('cover-diff'); x.add_argument('a'); x.add_argument('b'); x.add_argument('--top', type=int, default=60)
    rn = sub.add_parser('run'); rn.add_argument('scenario'); rn.add_argument('--name'); rn.add_argument('--window', action='store_true')
    for nm in ('report', 'edges', 'table', 'cost'): q = sub.add_parser(nm); q.add_argument('run', nargs='?')
    q = sub.add_parser('ab'); q.add_argument('run', nargs='?'); q.add_argument('--frames', type=int, default=180)
    q = sub.add_parser('draws'); q.add_argument('run', nargs='?'); q.add_argument('--by', default='v0'); q.add_argument('--top', type=int, default=30); q.add_argument('--hook', help="one tag hook's draws only (its name or number)")
    q = sub.add_parser('writes'); q.add_argument('run', nargs='?'); q.add_argument('--tick'); q.add_argument('--watch', type=int); q.add_argument('--limit', type=int, default=200)
    q = sub.add_parser('replay'); q.add_argument('recording'); q.add_argument('--hooks', required=True); q.add_argument('--name'); q.add_argument('--dmhost')
    pr = sub.add_parser('propose'); pr.add_argument('args', nargs='+', help='[run] <a> <b>'); pr.add_argument('--label', required=True)
    vw = sub.add_parser('view'); vw.add_argument('args', nargs='+', help='[run] main+0x…'); vw.add_argument('--cov')
    a = ap.parse_args(argv)
    if a.cmd == 'run': return cmd_run(a.scenario, a.name, a.window)
    if a.cmd == 'report': report(resolve_run(a.run)); return 0
    if a.cmd == 'edges': edges(resolve_run(a.run)); return 0
    if a.cmd == 'table': table(resolve_run(a.run)); return 0
    if a.cmd == 'cost': cost(resolve_run(a.run)); return 0
    if a.cmd == 'ab': ab(resolve_run(a.run), a.frames); return 0
    if a.cmd == 'draws': draws(resolve_run(a.run), a.by, a.top, a.hook); return 0
    if a.cmd == 'writes': writes(resolve_run(a.run), a.tick, a.watch, a.limit); return 0
    if a.cmd == 'replay': return cmd_replay(a.recording, a.hooks, a.name, a.dmhost)
    if a.cmd == 'propose': propose(resolve_run(a.args[0] if len(a.args) == 3 else None), a.args[-2], a.args[-1], a.label); return 0
    if a.cmd == 'view': view(resolve_run(a.args[0] if len(a.args) == 2 else None), a.args[-1], a.cov); return 0
    if a.cmd == 'summary': summary(a.file, a.json)
    elif a.cmd == 'dump': dump(a.file, a.hook, a.limit, a.tick)
    elif a.cmd == 'blocks': blocks(a.out, a.only)
    elif a.cmd == 'trace-spec': trace_spec(a.out, a.only, a.records)
    elif a.cmd == 'counts': counts(a.file, a.top)
    elif a.cmd == 'cover': cover(a.file, a.top)
    else: cover_diff(a.a, a.b, a.top)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
