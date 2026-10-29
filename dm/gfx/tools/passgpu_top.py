#!/usr/bin/env python3
# Per-pass GPU time from a DM_NVN_PASSGPU record (columns: the DM_NVN_PASSGPU comment in nvn_mtl.m), ranked by mean timeline increment inc_ms.
# Passes are grouped by shape (kind, size, colour / depth format, load actions) and their ordinal among same-shape passes in the frame, so a pass that comes
# and goes (a panel, an effect) does not shift every later pass the way the plain in-frame index did. A segment of a bench / scenario run is cut by its
# marks (events.ndjson "mark" events: --from town --to wild) or by frame numbers; @gpu_ms per frame (the run's nvn.tsv or frames.tsv) is the cross-check:
# the increments of one frame sum to the GPU's busy time, which @gpu_ms (the sum of the frame's command buffer spans) should match within queueing gaps.
# Under DM_NVN_PASSGPU_SERIAL=1 passes no longer overlap, so a pass's own span (span_ms) is its exclusive cost; compare those only with each other.
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
    rows = [r for r in (l.rstrip('\n').split('\t') for l in open(path, encoding='utf-8')) if len(r) >= 13][1:]   # a run cut short leaves a partial last row
    inc = collections.defaultdict(dict); span = collections.defaultdict(dict); draws = collections.defaultdict(list); per = collections.defaultdict(float); gpass = collections.defaultdict(dict)
    seen = collections.defaultdict(collections.Counter)
    for r in rows:
        f = int(r[0])
        if not f0 < f <= f1: continue
        shape = (r[2], r[5], r[6], r[7], r[8], r[9]); k = shape + (seen[f][shape],); seen[f][shape] += 1
        inc[k][f] = float(r[4]); span[k][f] = float(r[3]); draws[k].append(int(r[10])); per[f] += float(r[4])
        if len(r) > 13: gpass[k][f] = r[13]
    if not per: sys.exit(f'🔴 no passes between frames {f0} and {f1}')
    frames = sorted(per); n = len(frames); g = gpu_ms(run); both = [f for f in frames if f in g]
    vals = sorted(per.values())
    print(f'frames {frames[0]}..{frames[-1]} ({n}); increments per frame p50 {S.median(vals):.2f} / p95 {vals[int(n * .95)]:.2f} ms'
          + (f'; @gpu_ms p50 {S.median(g[f] for f in both):.2f} ms on the same frames' if both else ''))
    # Mean over every frame of the segment (a pass absent in a frame costs 0 there), so the table sums to the mean busy time per frame
    cost = {k: sum(v.values()) / n for k, v in inc.items()}; tot = sum(cost.values())
    print(f'pass groups {len(cost)}; mean per frame {tot:.2f} ms')
    print('rank  mean_ms  share  present  span_ms  draws  kind  size        fmt0  dfmt  load   #')
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
    print('RESULT ' + json.dumps(dict(name='passgpu', frames=n, groups=len(cost), mean_ms=round(tot, 3), top=[[round(cost[k], 3)] + list(k) for k in ranked[:5]])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
