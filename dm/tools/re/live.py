#!/usr/bin/env python3
# Reader for the live hooks' output (dm/host/dm_live.c, DM_LIVE=1): the records a run wrote to DM_LIVE_OUT and its <out>.hooks.tsv, summarised
# per hook with the guest symbol table (guestsyms.py: the dynamic symbols and the engine-analysis export's names).
#   summary <live.bin> [--json]     per hook: records, threads, call rate, callers, return durations, each capture's most frequent values
#   dump <live.bin> [--hook N] [--limit N]   one line per record
# The output can name game functions (the export's names) and carries runtime values: it stays in the run folder or under $DM_ROOT/re
import argparse
import collections
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import guestsyms  # noqa: E402

HDR = struct.Struct('<8sQQQII')          # dm_live_hdr: magic, base, size, timer_hz, rec_size, version
REC = struct.Struct('<IIQQQ4Q4QQ')       # dm_live_rec: hook, tid, t, lr, sp, x[4], cap[4], aux
RET = 0x80000000


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
    by = collections.defaultdict(list)
    for r in recs: by[r[0]].append(r)
    out = {'file': path, 'records': len(recs), 'hooks': []}
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
        hid = r[0] & ~RET
        if hook is not None and hid != hook: continue
        kind = 'ret' if r[0] & RET else 'hit'
        extra = f" dur {r[13] / hz * 1e6:.1f} us" if r[0] & RET else ' cap ' + ' '.join(f'{c:#x}' for c in r[9:13])
        print(f"{(r[2] - t0) / hz:10.6f} s  hook {hid} {hooks.get(hid, {}).get('name', '?')} {kind} tid {r[1]}  x0 {r[5]:#x} x1 {r[6]:#x}  from {where(r[3], hdr)}{extra}")
        n += 1
        if n >= limit: break


def main(argv):
    ap = argparse.ArgumentParser(prog='live.py', description='Read the live hooks output (dm_live.c).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('summary'); s.add_argument('file'); s.add_argument('--json', action='store_true')
    d = sub.add_parser('dump'); d.add_argument('file'); d.add_argument('--hook', type=int); d.add_argument('--limit', type=int, default=50)
    a = ap.parse_args(argv)
    if a.cmd == 'summary': summary(a.file, a.json)
    else: dump(a.file, a.hook, a.limit)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
