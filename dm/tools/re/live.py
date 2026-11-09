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
# blocks and trace-spec read the product's rewritten image and need capstone: run them with the project venv's python
# The output can name game functions (the export's names) and carries runtime values: it stays in the run folder or under $DM_ROOT/re
import argparse
import collections
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import guestsyms  # noqa: E402

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


def summary(path, as_json=False):
    hdr, recs, hooks = read(path)
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
        ncap = len([c for c in (hooks.get(hid, {}).get('spec', '').split('cap=')[1].split()[0].split(',') if 'cap=' in hooks.get(hid, {}).get('spec', '') else []) if c])
        h['caps'] = [[{'v': f'{v:#x}', 'n': n} for v, n in collections.Counter(r[9 + i] for r in ent).most_common(5)] for i in range(min(ncap, 4))]
        out['hooks'].append(h)
    if as_json: print(json.dumps(out, indent=1)); return out
    print(f"{path}: {len(recs)} records, module at {hdr['base']:#x}, timer {hz} Hz")
    for w in out['watches']:
        print(f"watch {w['watch']} on {w['addr']}: {w['writes']} writes, {w['threads']} threads")
        for c in w['writers']: print(f"    written by {c['at']}  ×{c['n']}")
        print(f"    values {', '.join(f'{x['v']}×{x['n']}' for x in w['values'])}")
    for h in out['hooks']:
        print(f"hook {h['hook']} {h.get('name', '?')} ({h.get('at', '?')}, {h.get('kind', '?')}): {h['records']} records, {h['returns']} returns, {h['threads']} threads"
              + (f", {h['per_s']}/s, gap p50 {h['gap_ms']['p50']} ms p95 {h['gap_ms']['p95']} ms" if h.get('per_s') else ''))
        for c in h['callers']: print(f"    called from {c['at']}  ×{c['n']}")
        if 'dur_us' in h: print(f"    duration p50 {h['dur_us']['p50']} us, p95 {h['dur_us']['p95']} us, max {h['dur_us']['max']} us; x0 at return {', '.join(f'{x['v']}×{x['n']}' for x in h['ret_x0'])}")
        for i, c in enumerate(h['caps']): print(f"    cap {i}: {', '.join(f'{x['v']}×{x['n']}' for x in c)}")
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
    fns = [(s_, n) for s_, n, _, _ in guestsyms.load().get('main', [])]
    if only:
        lo, hi = (int(x.split('+')[-1], 16) for x in only.split('-'))
        fns = [f for f in fns if lo <= f[0] < hi]
    if not fns: raise SystemExit('no functions: build the symbol table first (python3 dm/tools/guestsyms.py build)')
    return fns


def image():
    with open(os.path.join(guestsyms.product_dir(), 'main.patched.bin'), 'rb') as f: return f.read()


def blocks(out, only=None):
    import capstone   # the project venv has it
    import live_index
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
    a = ap.parse_args(argv)
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
