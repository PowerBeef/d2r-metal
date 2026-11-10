#!/usr/bin/env python3
# Write a live run's findings back into the analysis project as evidence (re.sh ingest [run]; the query server stopped; $DM_ROOT/re/venv):
#   - a `[live: <run>]` comment on each hooked function that recorded something: calls, rate, callers, argument and capture values, return time
#   - a computed-call reference from each call site the run resolved to the function it reached (live.py edges), so xrefs and call graphs show it
#   - a `dm-live` bookmark on each function coverage saw run, with its blocks run of blocks known, per coverage file
#   - for a feature map run (featuremap.py): `[live: <run>] runs during: <action or group>` on each function that ran only during actions
#   - with --unknown ID: the private unknowns ledger's row gets its status and a resolution carrying `[probed: <run>, <product>]`
# Nothing here names or types anything: names stay a person's decision (live.py propose writes leads to review). Re-running replaces the same
# run's comments. --dry-run prints what would change. The run's records carry runtime values: everything stays under $DM_ROOT/re
import argparse
import collections
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent))
DM_ROOT = Path(os.environ.get('DM_ROOT') or Path.home() / 'dm-work')
LEDGER = DM_ROOT / 're' / 'notes' / 'unknowns.tsv'


def comment_lines(run_id, h):
    """The comment for one hook's summary (live.py summary's entry), as lines."""
    out = [f"{h['records']} records" + (f", {h['per_s']}/s" if h.get('per_s') else '') + f", {h['threads']} threads ({h.get('kind', '?')} hook {h.get('name', '')})".rstrip()]
    if h.get('callers'): out.append('callers: ' + '; '.join(f"{c['at'].split(' ')[0]} ×{c['n']}" for c in h['callers'][:4]))
    for a in h.get('args', []):
        if a['values']: out.append(f"{a['reg']} {a['name']} ({a['type']}): " + ', '.join(f"{v['v']}×{v['n']}" for v in a['values']))
    for i, c in enumerate(h.get('caps', [])):
        nm = (h.get('cap_names') or [])[i] if i < len(h.get('cap_names') or []) else f'cap {i}'
        out.append(f'{nm}: ' + ', '.join(f"{v['v']}×{v['n']}" for v in c))
    if 'dur_us' in h: out.append(f"returns after p50 {h['dur_us']['p50']} us, p95 {h['dur_us']['p95']} us; x0 " + ', '.join(f"{v['v']}×{v['n']}" for v in h['ret_x0'][:3]))
    return [f'[live: {run_id}] {x}' for x in out]


def merge_comment(old, run_id, lines):
    """The old comment without this run's earlier lines, then the new ones."""
    keep = [x for x in (old or '').split('\n') if x and not x.startswith(f'[live: {run_id}]')]
    return '\n'.join(keep + lines)


def ledger_update(path, uid, status, text, run_id, product):
    """Set one ledger row's status and resolution (with the probed tag); returns the new row or raises."""
    if status not in ('resolved', 'contradicted'): raise SystemExit('--status is resolved or contradicted')
    lines = Path(path).read_text(encoding='utf-8').split('\n'); hit = None
    for i, ln in enumerate(lines):
        c = ln.split('\t')
        if c and c[0] == uid:
            c += [''] * (8 - len(c)); c[2] = status; c[7] = f'{text.strip()} [probed: {run_id}, {product}]'; lines[i] = '\t'.join(c); hit = lines[i]
    if hit is None: raise SystemExit(f'no row {uid} in {path}')
    Path(path).write_text('\n'.join(lines), encoding='utf-8')
    return hit


def gather(run):
    """What the run has to write: (comments {offset: lines}, edges [(site, target, count)], coverage {offset: [text]}), read without Ghidra."""
    import live
    import live_index
    rid = os.path.basename(run); comments, cov = {}, collections.defaultdict(list)
    if os.path.exists(os.path.join(run, 'live.bin')):
        for h in live.summary(os.path.join(run, 'live.bin'), quiet=True)['hooks']:
            m = re.fullmatch(r'main\+0x([0-9a-f]+)', h.get('at', ''))
            if m and (h['records'] or h['returns']): comments[int(m.group(1), 16)] = comment_lines(rid, h)
    edges = live.edges(run, quiet=True)
    ix = live_index.Index()
    fp = os.path.join(run, 'features.tsv')   # a feature map run (featuremap.py): when each function runs
    if os.path.exists(fp):
        with open(fp, encoding='utf-8') as fh:
            for ln in fh.read().split('\n')[1:]:
                c = ln.split('\t')
                if len(c) > 3 and c[0].startswith('main+'):
                    comments.setdefault(int(c[0][5:], 16), []).append(f"[live: {rid}] runs during: {c[2]}" + ('' if c[3] == '1' else ' (a branch: the function also runs while idle)'))
    for f in sorted(os.listdir(run)):
        if f.startswith('cov') and f.endswith('.tsv'):
            for func, n in live.by_function(live.cov_read(os.path.join(run, f))).items():
                cov[func].append(f'{rid}/{f}: {n} of {len(ix.leaders(func)) or "?"} blocks ran')
    return rid, comments, edges, cov


def main():
    ap = argparse.ArgumentParser(prog='ingest.py')
    ap.add_argument('run', nargs='?'); ap.add_argument('--dry-run', action='store_true'); ap.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '6g'))
    ap.add_argument('--unknown'); ap.add_argument('--status', default='resolved'); ap.add_argument('--resolution')
    a = ap.parse_args()
    import live
    run = live.resolve_run(a.run); rid, comments, edges, cov = gather(run)
    meta = json.load(open(os.path.join(run, 'run.json'), encoding='utf-8')) if os.path.exists(os.path.join(run, 'run.json')) else {}
    if meta and not meta.get('index_matches', True): raise SystemExit(f"run {rid} was made on product {meta.get('product')}, the snapshot is for {meta.get('index_product')}: re.sh live-index first")
    print(f'run {rid}: {len(comments)} function comments, {len(edges)} computed-call edges, {len(cov)} functions with coverage')
    if a.unknown:
        if not a.resolution: raise SystemExit('--unknown needs --resolution "what the run showed"')
        if a.dry_run: print(f'  would set {a.unknown} to {a.status}')
        else:
            ledger_update(LEDGER, a.unknown, a.status, a.resolution, rid, meta.get('product', '?'))
            chk = subprocess.run([sys.executable, str(DM_ROOT / 're' / 'bin' / 'unknowns.py'), 'check'], capture_output=True, text=True, check=False)
            print(f'  ledger: {a.unknown} {a.status}; unknowns.py check: {(chk.stdout + chk.stderr).strip().splitlines()[-1] if (chk.stdout + chk.stderr).strip() else chk.returncode}')
    if a.dry_run:
        for off, lines in list(comments.items())[:3]: print(f'  would comment main+{off:#x}:', *lines, sep='\n    ')
        print('INGEST OK (dry run)'); return 0
    import ghidra_db
    ghidra_db.start(a.maxmem)
    import pyghidra
    from ghidra.program.model.listing import BookmarkType, CodeUnit
    from ghidra.program.model.symbol import RefType, SourceType
    from ghidra.util.task import TaskMonitor
    project = ghidra_db.open_project(); nc = ne = nb = 0
    with pyghidra.program_context(project, '/main.elf') as program:
        lst, refs, bm = program.getListing(), program.getReferenceManager(), program.getBookmarkManager()
        with pyghidra.transaction(program, f'dm live ingest {rid}'):
            for off, lines in comments.items():
                at = ghidra_db.addr(program, off); old = lst.getComment(CodeUnit.PRE_COMMENT, at) or ''; new = merge_comment(old, rid, lines)
                if new != old: lst.setComment(at, CodeUnit.PRE_COMMENT, new); nc += 1
            for site, target, _ in edges:
                frm, to = ghidra_db.addr(program, site), ghidra_db.addr(program, target)
                if any(r.getToAddress().equals(to) for r in refs.getReferencesFrom(frm)): continue
                refs.addMemoryReference(frm, to, RefType.COMPUTED_CALL, SourceType.USER_DEFINED, 0); ne += 1
            for func, texts in cov.items():
                bm.setBookmark(ghidra_db.addr(program, func), BookmarkType.NOTE, 'dm-live', '; '.join(texts)[:300]); nb += 1
        program.save(f'dm live ingest {rid}', TaskMonitor.DUMMY)
    project.close()
    with open(os.path.join(run, 'ingested.json'), 'w', encoding='utf-8') as f: json.dump(dict(comments=nc, edges=ne, bookmarks=nb), f)
    print(f'INGEST OK: {nc} comments set, {ne} references added, {nb} bookmarks set')
    return 0


if __name__ == '__main__':
    sys.exit(main())
