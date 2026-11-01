#!/usr/bin/env python3
"""Keeps the workspace ($DM_ROOT, default ~/dm-work) from filling the disk: lists what is safe to remove and, with --apply, moves it to the Trash.

What goes (each regenerable or done with):
  - shader libraries of older translation-chain versions (build/cache/metal_shader_cache/b-<V>, all but the version dm/gfx/nvn/dmsh_chain.h names);
  - rewrite products of older revisions (build/cache/dm/out/<P>-<kind>, all but dm/PRODUCT's);
  - test runs older than --days (run/<area>/<entry>: scenarios, captures, measurements, gate results, ...), never run/bench/baseline;
  - local test releases older than --days (release/<version>), never the newest one, the release cache or the notes;
  - Mac App test copies older than a day (out/macapp_test, out/macapp-<name>; never out/macapp, the player's App).
The game files (data/), the engine-analysis database (re/), the Python environment and the current outputs are never touched.

Usage: workspace_prune.py [--apply] [--days N] [--quiet]
  without --apply it only lists; the last line is "PRUNE <total> GB in use, <n> GB to free"
  --apply moves everything listed into ~/.Trash/dm-work prune <date> (one folder; recoverable until the Trash is emptied)
  --quiet prints only the last line (the session-start check uses it)
Standard library only.
"""
import argparse, os, re, shutil, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ROOT = os.path.expanduser(os.environ.get('DM_ROOT') or '~/dm-work')
DAY = 86400


def size(p):   # bytes on disk (allocated blocks; APFS clones count in full)
    if os.path.islink(p): return 0
    if os.path.isfile(p): return os.lstat(p).st_blocks * 512
    n = 0
    for d, _, files in os.walk(p):
        for f in files:
            try: n += os.lstat(os.path.join(d, f)).st_blocks * 512
            except OSError: pass
    return n


def newest(p):   # the newest mtime inside p (a run folder counts as recent while anything in it is)
    t = os.lstat(p).st_mtime
    if os.path.isdir(p) and not os.path.islink(p):
        for d, dirs, files in os.walk(p):
            for f in dirs + files:
                try: t = max(t, os.lstat(os.path.join(d, f)).st_mtime)
                except OSError: pass
    return t


def current_chain():
    try: m = re.search(r'#define DMSH_CHAIN "([0-9a-f]{12})"', open(os.path.join(REPO, 'dm', 'gfx', 'nvn', 'dmsh_chain.h'), encoding='utf-8').read())
    except OSError: m = None
    return m.group(1) if m else None


def candidates(days):
    now = time.time(); out = []   # (path, why)
    shaders = os.path.join(ROOT, 'build', 'cache', 'metal_shader_cache'); chain = current_chain()
    if chain and os.path.isdir(os.path.join(shaders, 'b-' + chain)):   # never prune without knowing which version is current
        for n in sorted(os.listdir(shaders)):
            if n.startswith('b-') and n != 'b-' + chain: out.append((os.path.join(shaders, n), 'older shader library version'))
    products = os.path.join(ROOT, 'build', 'cache', 'dm', 'out')
    try: prod = open(os.path.join(REPO, 'dm', 'PRODUCT'), encoding='utf-8').read().strip()
    except OSError: prod = None
    if prod and os.path.isdir(products):
        for n in sorted(os.listdir(products)):
            if not n.startswith(prod + '-'): out.append((os.path.join(products, n), 'older rewrite product'))
    run = os.path.join(ROOT, 'run'); keep = {os.path.join(run, 'bench', 'baseline')}
    if os.path.isdir(run):
        for area in sorted(os.listdir(run)):
            a = os.path.join(run, area)
            if not os.path.isdir(a) or os.path.islink(a): continue
            latest = os.path.realpath(os.path.join(a, 'latest')) if os.path.islink(os.path.join(a, 'latest')) else None
            for n in sorted(os.listdir(a)):
                p = os.path.join(a, n)
                if p in keep or os.path.realpath(p) == latest or os.path.islink(p): continue
                if now - newest(p) > days * DAY: out.append((p, 'test run older than %d days' % days))
    rel = os.path.join(ROOT, 'release')
    if os.path.isdir(rel):
        vers = [n for n in os.listdir(rel) if re.match(r'^\d+\.\d+\.\d+', n) and os.path.isdir(os.path.join(rel, n))]
        top = max(vers, key=lambda n: os.lstat(os.path.join(rel, n)).st_mtime) if vers else None
        for n in sorted(vers):
            p = os.path.join(rel, n)
            if n != top and now - newest(p) > days * DAY: out.append((p, 'local release older than %d days' % days))
    apps = os.path.join(ROOT, 'out')
    if os.path.isdir(apps):
        for n in sorted(os.listdir(apps)):
            p = os.path.join(apps, n)
            if (n == 'macapp_test' or n.startswith('macapp-')) and now - newest(p) > DAY: out.append((p, 'Mac App test copy older than a day'))
    return out


def main():
    ap = argparse.ArgumentParser(description='Lists what is safe to remove from the workspace; --apply moves it to the Trash.')
    ap.add_argument('--apply', action='store_true'); ap.add_argument('--days', type=int, default=7); ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    if not os.path.isdir(ROOT): print('PRUNE no workspace at %s' % ROOT); return 0
    total = size(ROOT); items = [(p, why, size(p)) for p, why in candidates(a.days)]; free = sum(s for _, _, s in items)
    if not a.quiet:
        for p, why, s in sorted(items, key=lambda x: -x[2]): print('  %7.2f GB  %s  (%s)' % (s / 1e9, os.path.relpath(p, ROOT), why))
    if a.apply and items:
        trash = os.path.join(os.path.expanduser('~/.Trash'), 'dm-work prune ' + time.strftime('%Y-%m-%d %H%M%S'))
        for p, _, _ in items:
            dst = os.path.join(trash, os.path.relpath(p, ROOT)); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.move(p, dst)
        if not a.quiet: print('moved %d item(s) to %s' % (len(items), trash))
    print('PRUNE %.1f GB in use, %.1f GB %s' % (total / 1e9, free / 1e9, 'moved to the Trash' if a.apply else 'to free (run with --apply)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
