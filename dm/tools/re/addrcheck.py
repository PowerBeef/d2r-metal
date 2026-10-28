#!/usr/bin/env python3
# Game-address check: every `main+0x...` the sources cite must land where the game's code says it does.
#   The sources (dm/host, dm/gfx, dm/ios/Sources, dm/tools, dm/verify) cite game functions by module offset. This resolves each
#   cited code address against the function table guestsyms.py derives from the game program (.eh_frame) and the names exported
#   from the private engine-analysis database ($DM_ROOT/re/export/main.names.tsv), so a mistyped or stale address shows up
#   before someone follows it into the wrong function.
#   Red: a code address that is not 4-byte aligned, or that lies in no known function and is not a known name or label either.
#   Not checked: test files (test_*, t_*) and lines that give a usage example (made-up addresses).
#   Listed only: addresses outside the code range (data, the key page and patch area appended by the rewriter).
#   Prints addresses, file:line citations and names from the local export only; nothing derived from the game goes to git.
#   Without the symbol table or the export (no game files / no database on this machine) it reports unavailable, not red.
# Usage: addrcheck.py [--symbols <guestsyms table.tsv>] [--export <main.names.tsv>] [--root <dm dir>] [--list]
#   last lines: READDR OK|RED ..., RESULT {json}; exit 0 = OK or unavailable, 1 = RED
import argparse
import bisect
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..', '..'))
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
CITE = re.compile(r'\bmain\+0x([0-9a-fA-F]{3,})\b')
EXAMPLE = re.compile(r'e\.g\.|[Ee]xample|例|如 ')   # usage examples cite made-up addresses
DIRS = ('host', 'gfx', 'ios/Sources', 'tools', 'verify')


def citations(root):
    """{offset: ['file:line', ...]} for every main+0x... in the tracked sources under the checked directories."""
    out = subprocess.run(['git', '-C', root, 'grep', '-nIE', r'main\+0x[0-9a-fA-F]{3,}', '--', *DIRS], capture_output=True, text=True, check=False).stdout
    cites = {}
    for line in out.splitlines():
        try: f, n, text = line.split(':', 2)
        except ValueError: continue
        if os.path.basename(f).startswith(('test_', 't_')) or EXAMPLE.search(text): continue   # fixtures and usage examples
        for m in CITE.finditer(text): cites.setdefault(int(m.group(1), 16), []).append(f'{f}:{n}')
    return cites


def load_functions(path):
    """Sorted [(start, size, name)] of module main from a guestsyms.py table (module, start, size, name, notes)."""
    fns = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith(('#', 'module\t')): continue
            c = line.rstrip('\n').split('\t')
            if len(c) >= 4 and c[0] == 'main': fns.append((int(c[1], 16), int(c[2], 16), c[3]))
    return sorted(fns)


def load_export(path):
    """{offset: (name, kind)} from the engine-analysis export (offset, name, kind, confidence, source)."""
    names = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith(('#', 'offset\t')): continue
            c = line.rstrip('\n').split('\t')
            if len(c) >= 3: names.setdefault(int(c[0], 16), (c[1], c[2]))
    return names


def default_symbols():
    """guestsyms.py's table for the current touch build output; built on the spot when the build output is there but the table is not."""
    sys.path.insert(0, os.path.join(DM, 'tools'))
    import guestsyms   # pyright: ignore[reportMissingImports]
    prod = guestsyms.product_dir(); path = guestsyms.table_path(prod)
    if not os.path.exists(path) and prod and os.path.exists(os.path.join(prod, 'main.patched.bin')): guestsyms.build(prod)
    return path


def check(cites, fns, names):
    starts = [s for s, _, _ in fns]
    lo, hi = (fns[0][0], max(s + n for s, n, _ in fns)) if fns else (0, 0)
    rows = []
    for off in sorted(cites):
        i = bisect.bisect_right(starts, off) - 1
        fn = fns[i] if i >= 0 and off <= fns[i][0] + fns[i][1] else None   # the end itself counts: a return address after a final call
        known = names.get(off)
        if not lo <= off < hi: kind, ok = 'data', True
        elif off % 4: kind, ok = 'misaligned', False
        elif fn or known: kind, ok = 'code', True
        else: kind, ok = 'unknown', False
        where = (names.get(fn[0], (fn[2],))[0] or fn[2] or f'sub_{fn[0]:x}') + (f'+{off - fn[0]:#x}' if off != fn[0] else '') if fn else (known[0] if known else '')
        rows.append(dict(offset=off, kind=kind, ok=ok, where=where, cites=cites[off]))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description='Check the main+0x... addresses cited in the sources against the game program and the engine-analysis export.')
    ap.add_argument('--symbols', help='guestsyms.py table (default: the one for the current touch build output)')
    ap.add_argument('--export', default=os.path.join(DM_ROOT, 're', 'export', 'main.names.tsv'))
    ap.add_argument('--root', default=DM)
    ap.add_argument('--list', action='store_true', help='print every citation, not only the red ones')
    a = ap.parse_args(argv)
    sym = a.symbols or default_symbols()
    missing = [p for p in (sym, a.export) if not os.path.exists(p)]
    if missing:
        print(f'⚪ READDR unavailable: no {" / ".join(missing)} (guestsyms.py build; dm/tools/re/re.sh export)')
        print('RESULT ' + json.dumps(dict(ok=None, unavailable=missing)))
        return 0
    rows = check(citations(a.root), load_functions(sym), load_export(a.export))
    bad = [r for r in rows if not r['ok']]
    for r in rows if a.list else bad:
        print(f"{'  ' if r['ok'] else '  RED '}main+{r['offset']:#x}  {r['kind']:10s} {r['where'] or '-'}  ({', '.join(r['cites'][:3])}{' …' if len(r['cites']) > 3 else ''})")
    counts = {k: sum(r['kind'] == k for r in rows) for k in ('code', 'data', 'misaligned', 'unknown')}
    print(f"READDR {'RED' if bad else 'OK'} {len(rows)} addresses: {counts['code']} in code, {counts['data']} outside the code range, {len(bad)} red")
    print('RESULT ' + json.dumps(dict(ok=not bad, n=len(rows), **counts, red=[f"main+{r['offset']:#x}" for r in bad][:50])))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
