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
#   edges [run]                     computed call sites resolved by what was seen: (call site, function called, count) → edges.tsv
#   propose [run] <a> <b> --label L   functions whose blocks ran only in b, appended to re/live/proposals.tsv as naming leads
#   view [run] main+0x… [--cov F]   one function's decompiled text with each line's coverage, hits and values → re/live/view/<run>/<offset>.html
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


def read(path):
    with open(path, 'rb') as f: b = f.read()
    if len(b) < HDR.size: raise SystemExit(f'{path}: too short for a header')
    magic, base, size, hz, rs, ver = HDR.unpack_from(b, 0)
    if magic != b'DMLIVE01' or rs != REC.size: raise SystemExit(f'{path}: not a live hooks file (magic {magic!r}, record {rs} bytes)')
    hdr = {'base': base, 'size': size, 'timer_hz': hz, 'version': ver}
    recs = [REC.unpack_from(b, o) for o in range(HDR.size, len(b) - REC.size + 1, REC.size)]
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


def summary(path, as_json=False, quiet=False, top=25):
    hdr, recs, hooks = read(path); ix = live_index.Index()
    hz = hdr['timer_hz'] or 1
    by = collections.defaultdict(list); watch = collections.defaultdict(list)
    for r in recs: (watch[r[0] & 0xff] if r[0] & WATCH else by[r[0]]).append(r)
    out = {'file': path, 'records': len(recs), 'hooks': [], 'watches': []}
    for slot in sorted(watch):   # cap[0] the address, cap[1] the writing instruction, cap[2] the value after
        w = watch[slot]
        out['watches'].append({'watch': slot, 'addr': f'{w[0][9]:#x}', 'writes': len(w), 'threads': len({r[1] for r in w}),
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
        h['caps'] = [[{'v': f'{v:#x}', 'n': n} for v, n in collections.Counter(r[9 + i] for r in ent).most_common(5)] for i in range(len(caps))]
        h.update(typed(h.get('at', ''), h.get('kind', ''), caps, ent, ix, spec))
        out['hooks'].append(h)
    if as_json: print(json.dumps(out, indent=1)); return out
    if quiet: return out
    print(f"{path}: {len(recs)} records, module at {hdr['base']:#x}, timer {hz} Hz")
    for w in out['watches']:
        print(f"watch {w['watch']} on {w['addr']}: {w['writes']} writes, {w['threads']} threads")
        for c in w['writers']: print(f"    written by {c['at']}  ×{c['n']}")
        print(f"    values {', '.join(f'{x['v']}×{x['n']}' for x in w['values'])}")
    shown = sorted((h for h in out['hooks'] if h['records'] or h['returns']), key=lambda h: -(h['records'] + h['returns']))
    if len(shown) > top: print(f'{len(shown)} hooks recorded; the {top} with most records (summary --json has all):')
    for h in shown[:top]:
        print(f"hook {h['hook']} {h.get('name', '?')} ({h.get('at', '?')}, {h.get('kind', '?')}): {h['records']} records, {h['returns']} returns, {h['threads']} threads"
              + (f", {h['per_s']}/s, gap p50 {h['gap_ms']['p50']} ms p95 {h['gap_ms']['p95']} ms" if h.get('per_s') else ''))
        for c in h['callers']: print(f"    called from {c['at']}  ×{c['n']}")
        if 'dur_us' in h: print(f"    duration p50 {h['dur_us']['p50']} us, p95 {h['dur_us']['p95']} us, max {h['dur_us']['max']} us; x0 at return {', '.join(f'{x['v']}×{x['n']}' for x in h['ret_x0'])}")
        for a in h.get('args', []):
            if a['values']: print(f"    {a['reg']} {a['name']} ({a['type']}): {', '.join(f'{x['v']}×{x['n']}' for x in a['values'])}")
        for i, c in enumerate(h['caps']): print(f"    cap {(h.get('cap_names') or [])[i] if i < len(h.get('cap_names') or []) else i}: {', '.join(f'{x['v']}×{x['n']}' for x in c)}")
    return out


def dump(path, hook=None, limit=50):
    hdr, recs, hooks = read(path); hz = hdr['timer_hz'] or 1; n = 0
    t0 = recs[0][2] if recs else 0
    for r in recs:
        if r[0] & WATCH:
            if hook is None: print(f"{(r[2] - t0) / hz:10.6f} s  watch {r[0] & 0xff} tid {r[1]}  {r[9]:#x} = {r[11]:#x}  by {where(r[10], hdr)}")
            continue
        hid = r[0] & ~RET
        if hook is not None and hid != hook: continue
        kind = 'ret' if r[0] & RET else 'hit'
        extra = f" dur {r[13] / hz * 1e6:.1f} us" if r[0] & RET else ' cap ' + ' '.join(f'{c:#x}' for c in r[9:13])
        print(f"{(r[2] - t0) / hz:10.6f} s  hook {hid} {hooks.get(hid, {}).get('name', '?')} {kind} tid {r[1]}  x0 {r[5]:#x} x1 {r[6]:#x}  from {where(r[3], hdr)}{extra}")
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
    d = sub.add_parser('dump'); d.add_argument('file'); d.add_argument('--hook', type=int); d.add_argument('--limit', type=int, default=50)
    b = sub.add_parser('blocks'); b.add_argument('--out', required=True); b.add_argument('--only')
    t = sub.add_parser('trace-spec'); t.add_argument('--out', required=True); t.add_argument('--only'); t.add_argument('--records', type=int, default=0)
    c = sub.add_parser('counts'); c.add_argument('file'); c.add_argument('--top', type=int, default=40)
    v = sub.add_parser('cover'); v.add_argument('file'); v.add_argument('--top', type=int, default=40)
    x = sub.add_parser('cover-diff'); x.add_argument('a'); x.add_argument('b'); x.add_argument('--top', type=int, default=60)
    rn = sub.add_parser('run'); rn.add_argument('scenario'); rn.add_argument('--name'); rn.add_argument('--window', action='store_true')
    for nm in ('report', 'edges'): q = sub.add_parser(nm); q.add_argument('run', nargs='?')
    pr = sub.add_parser('propose'); pr.add_argument('args', nargs='+', help='[run] <a> <b>'); pr.add_argument('--label', required=True)
    vw = sub.add_parser('view'); vw.add_argument('args', nargs='+', help='[run] main+0x…'); vw.add_argument('--cov')
    a = ap.parse_args(argv)
    if a.cmd == 'run': return cmd_run(a.scenario, a.name, a.window)
    if a.cmd == 'report': report(resolve_run(a.run)); return 0
    if a.cmd == 'edges': edges(resolve_run(a.run)); return 0
    if a.cmd == 'propose': propose(resolve_run(a.args[0] if len(a.args) == 3 else None), a.args[-2], a.args[-1], a.label); return 0
    if a.cmd == 'view': view(resolve_run(a.args[0] if len(a.args) == 2 else None), a.args[-1], a.cov); return 0
    if a.cmd == 'summary': summary(a.file, a.json)
    elif a.cmd == 'dump': dump(a.file, a.hook, a.limit)
    elif a.cmd == 'blocks': blocks(a.out, a.only)
    elif a.cmd == 'trace-spec': trace_spec(a.out, a.only, a.records)
    elif a.cmd == 'counts': counts(a.file, a.top)
    elif a.cmd == 'cover': cover(a.file, a.top)
    else: cover_diff(a.a, a.b, a.top)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
