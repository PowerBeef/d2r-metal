#!/usr/bin/env python3
# Per-pass GPU time from a DM_NVN_PASSGPU record (columns: the DM_NVN_PASSGPU comment in nvn_mtl.m), ranked by mean timeline increment inc_ms.
# Passes are grouped by shape (kind, size, colour / depth format, load actions) and their ordinal among same-shape passes in the frame, so a pass that comes
# and goes (a panel, an effect) does not shift every later pass the way the plain in-frame index did. A segment of a bench / scenario run is cut by its
# marks (events.ndjson "mark" events: --from town --to wild) or by frame numbers; @gpu_ms per frame (the run's nvn.tsv or frames.tsv) is the cross-check:
# the increments of one frame sum to the GPU's busy time, which @gpu_ms (the sum of the frame's command buffer spans) should match within queueing gaps.
# A pass's own span is its exclusive cost (shown as excl_ms) only when the record's first line says serial=1 barrier_level=0 (DM_NVN_PASSGPU_SERIAL=1 holds the
# pass barriers at level 0) and its spans don't overlap (overlap = 1 - sum(inc_ms) / sum(span), at most 2 %); compare those only with each other.
# Older records, which have no such line, are never exclusive: rank them by mean_ms. Clear-only render passes (no draws) are left out of the overlap: their start
# stamp (after a vertex stage they don't have) is written before their start barrier waits, so their span overlaps the previous pass even when nothing runs in parallel.
# Usage: passgpu_top.py <run dir | passgpu.tsv> [--from MARK|FRAME] [--to MARK|FRAME] [--top N] [--tsv OUT [--join FRAME]]   (old form: passgpu_top.py <dir> [first frame] [N])
import argparse, collections, json, os, statistics as S, sys


def marks(run):
    """Mark name → frame number from the run's events.ndjson (empty if there is none)"""
    out = {}
    try:
        for line in open(os.path.join(run, 'events.ndjson'), encoding='utf-8'):
            try: e = json.loads(line)
            except ValueError: continue
            if e.get('ev') == 'mark' and 'frame' in e: out[e['name']] = int(e['frame'])
    except OSError: pass
    return out


def frame_of(v, mk, default):
    if v is None: return default
    if v.lstrip('-').isdigit(): return int(v)
    if v not in mk: sys.exit(f'🔴 no mark "{v}" in events.ndjson (marks: {", ".join(mk) or "none"})')
    return mk[v]


def meta(path):
    """key=value pairs of the record's first line when it starts with '#' (pg_on in nvn_mtl.m writes barrier_level and serial; empty for older records)"""
    try:
        with open(path, encoding='utf-8') as f: first = f.readline()
    except OSError: return {}
    if not first.startswith('#'): return {}
    return dict(kv.split('=', 1) for kv in first[1:].split() if '=' in kv)


def gpu_ms(run):
    """Frame → @gpu_ms from the run's per-frame NVN table (bench writes nvn.tsv; older runs frames.tsv)"""
    g = {}
    for name in ('nvn.tsv', 'frames.tsv'):
        try:
            for line in open(os.path.join(run, name), encoding='utf-8'):
                p = line.rstrip('\n').split('\t')
                if len(p) == 3 and p[0].isdigit() and p[1] == '@gpu_ms': g[int(p[0])] = float(p[2])
        except OSError: continue
    return g


def main(argv):
    ap = argparse.ArgumentParser(prog='passgpu_top.py', description='Per-pass GPU time from a DM_NVN_PASSGPU record, ranked.')
    ap.add_argument('run'); ap.add_argument('old', nargs='*', help=argparse.SUPPRESS)
    ap.add_argument('--from', dest='a'); ap.add_argument('--to', dest='b'); ap.add_argument('--top', type=int, default=15); ap.add_argument('--tsv'); ap.add_argument('--join', type=int)
    a = ap.parse_args(argv)
    if a.old: a.a = a.a or a.old[0]; a.top = int(a.old[1]) if len(a.old) > 1 else a.top
    path = a.run if a.run.endswith('.tsv') else os.path.join(a.run, 'passgpu.tsv'); run = os.path.dirname(path) or '.'
    mk = marks(run); f0 = frame_of(a.a, mk, 300); f1 = frame_of(a.b, mk, 1 << 62)
    with open(path, encoding='utf-8') as fh: rows = [r for r in (l.rstrip('\n').split('\t') for l in fh) if len(r) >= 13][1:]   # a run cut short leaves a partial last row
    inc = collections.defaultdict(dict); span = collections.defaultdict(dict); draws = collections.defaultdict(list); per = collections.defaultdict(float); gpass = collections.defaultdict(dict)
    seen = collections.defaultdict(collections.Counter); span_sum = inc_sum = 0.0
    for r in rows:
        f = int(r[0])
        if not f0 < f <= f1: continue
        shape = (r[2], r[5], r[6], r[7], r[8], r[9]); k = shape + (seen[f][shape],); seen[f][shape] += 1
        inc[k][f] = float(r[4]); span[k][f] = float(r[3]); draws[k].append(int(r[10])); per[f] += float(r[4])
        if r[2] != 'R' or int(r[10]) > 0: span_sum += float(r[3]); inc_sum += float(r[4])   # clear-only passes: see the header
        if len(r) > 13: gpass[k][f] = r[13]
    if not per: sys.exit(f'🔴 no passes between frames {f0} and {f1}')
    # The spans are exclusive costs only if the record was made serialised and its passes really ran one after another: then each pass's span equals its
    # timeline increment. At level 1 a pass's vertex work overlaps the previous pass's fragment work, so the spans sum to more than the busy time.
    m = meta(path); lv = m.get('barrier_level'); lv = int(lv) if lv is not None and lv.lstrip('-').isdigit() else None
    overlap = 1 - inc_sum / span_sum if span_sum > 0 else 0.0
    said_serial = m.get('serial') == '1' and lv == 0
    exclusive = said_serial and overlap <= 0.02
    col = 'excl_ms' if exclusive else 'span_ms'
    frames = sorted(per); n = len(frames); g = gpu_ms(run); both = [f for f in frames if f in g]
    vals = sorted(per.values())
    print(f'frames {frames[0]}..{frames[-1]} ({n}); increments per frame p50 {S.median(vals):.2f} / p95 {vals[int(n * .95)]:.2f} ms'
          + (f'; @gpu_ms p50 {S.median(g[f] for f in both):.2f} ms on the same frames' if both else ''))
    # Mean over every frame of the segment (a pass absent in a frame costs 0 there), so the table sums to the mean busy time per frame
    cost = {k: sum(v.values()) / n for k, v in inc.items()}; tot = sum(cost.values())
    print(f'pass groups {len(cost)}; mean per frame {tot:.2f} ms; overlap between passes {100 * overlap:.1f} %')
    if said_serial and not exclusive: print(f'record says serial but passes overlap {100 * overlap:.0f} %: not an exclusive cost')
    elif not exclusive: print(f'span_ms overlaps neighbouring passes (barrier level {"unknown" if lv is None else lv}): not an exclusive cost; rank by mean_ms')
    print(f'rank  mean_ms  share  present  {col}  draws  kind  size        fmt0  dfmt  load   #')
    ranked = sorted(cost, key=lambda k: -cost[k])
    for i, k in enumerate(ranked[:a.top], 1):
        kind, w, h, fmt0, dfmt, load, ordn = k
        print(f'{i:4}  {cost[k]:7.3f}  {100 * cost[k] / tot:4.1f}%  {len(inc[k]) / n:6.0%}  {S.mean(span[k].values()):7.3f}  {S.median(draws[k]):5.0f}'
              f'  {kind:4}  {w + "x" + h:10}  {fmt0:4}  {dfmt:4}  {load:5}  {ordn}')
    if a.tsv:
        # pass_at: the group's global pass number in frame --join (the pass number DM_NVN_PASSDUMP lines carry), to join a dump of that frame
        jf = a.join if a.join is not None else frames[-2 if n > 1 else -1]   # the last frame may be cut short when the run ends
        with open(a.tsv, 'w', encoding='utf-8') as f:
            f.write(f'rank\tmean_ms\tshare\tpresent\tspan_ms\tdraws\tkind\tw\th\tfmt0\tdfmt\tload\tordinal\tpass_at_{jf}\n')
            for i, k in enumerate(ranked, 1):
                kind, w, h, fmt0, dfmt, load, ordn = k
                f.write(f'{i}\t{cost[k]:.4f}\t{cost[k] / tot:.4f}\t{len(inc[k]) / n:.3f}\t{S.mean(span[k].values()):.4f}\t{S.median(draws[k]):.0f}'
                        f'\t{kind}\t{w}\t{h}\t{fmt0}\t{dfmt}\t{load}\t{ordn}\t{gpass[k].get(jf, "")}\n')
        print(f'table: {a.tsv}')
    print('RESULT ' + json.dumps(dict(name='passgpu', frames=n, groups=len(cost), mean_ms=round(tot, 3), barrier_level=lv, exclusive=exclusive, overlap=round(overlap, 3),
                                      top=[[round(cost[k], 3)] + list(k) for k in ranked[:5]])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
