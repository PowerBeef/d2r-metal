#!/usr/bin/env python3
"""The boot list a machine learns, over two launches (dm/host/dm_bootlist.c, the boot prefetch in hle_tact.c, the animation pre-decode in hle_granny.c).

A scenario is one launch, so this runs enter_game twice on the headless host with one list path:
  launch 1: no list yet. The host must note the game's blocks and write a list (event bootlist_written, placed > 0).
  launch 2: the list is in use. Nearly all of it must be handed to the game already decoded, the animation pre-decode must get
            hits, and the list must still fit (no rewrite).
Both launches must pass the scenario's own checks (no red line). Last line: BOOTLIST OK | BOOTLIST RED <why>. Exit 0 / 1.

  bootlist_check.py [--keep]     --keep leaves the run folders' list in place (it is in a temp folder that is removed otherwise)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def launch(path):
    out = subprocess.run([sys.executable, os.path.join(REPO, 'dm', 'tools', 'scenario.py'), 'run', os.path.join(HERE, 'scenarios', 'enter_game.scn'),
                          '--backend', 'cli', '--headless', '--env', 'DM_BOOT_LIST=' + path + ' DM_BOOT_LEARN=1'], capture_output=True, text=True, cwd=REPO).stdout
    m = re.search(r'"dir": "([^"]+)"', out)
    ok = '"verdict": "OK"' in out
    log = open(os.path.join(m.group(1), 'host.log'), errors='replace').read() if m else ''
    evs = []
    if m and os.path.exists(os.path.join(m.group(1), 'events.ndjson')):
        for line in open(os.path.join(m.group(1), 'events.ndjson'), errors='replace'):
            try: evs.append(json.loads(line))
            except ValueError: pass
    return ok, log, evs, (m.group(1) if m else '?')


def main():
    keep = '--keep' in sys.argv[1:]
    tmp = tempfile.mkdtemp(prefix='bootlist_check.')
    path = os.path.join(tmp, 'bootlist_user.tsv')
    red = []
    ok1, log1, ev1, d1 = launch(path)
    if not ok1: red.append(f'launch 1 failed its scenario ({d1})')
    for _ in range(50):   # the list is written by a background thread after the menu; the scenario plays on for seconds, but be patient
        if os.path.exists(path): break
        time.sleep(0.1)
    rows = sum(1 for line in open(path) if not line.startswith('#')) if os.path.exists(path) else 0
    w = [e for e in ev1 if e.get('ev') == 'bootlist_written']
    if not rows: red.append(f'launch 1 wrote no list ({d1})')
    elif not w or not w[0].get('ok') or w[0].get('placed', 0) < 0.9 * w[0].get('blocks', 1): red.append(f'launch 1 placed too few blocks: {w[:1]} ({d1})')
    print(f'launch 1: {rows} blocks learned ({d1})')
    if rows:
        ok2, log2, ev2, d2 = launch(path)
        if not ok2: red.append(f'launch 2 failed its scenario ({d2})')
        if 'the list this machine learned' not in log2: red.append(f'launch 2 did not load the learned list ({d2})')
        m = re.search(r'DM_TACT_PREFETCH：命中 (\d+)', log2); hits = int(m.group(1)) if m else 0
        g = re.search(r'Granny 预解码（到选角）：候选 (\d+) 段、后台解完 (\d+)、失败 (\d+)；命中 (\d+)', log2); ghits = int(g.group(4)) if g else 0
        b = [e for e in ev2 if e.get('ev') == 'bootlist']
        if hits < 0.9 * rows: red.append(f'launch 2 was handed {hits} of {rows} listed blocks ({d2})')
        if not ghits: red.append(f'launch 2: the animation pre-decode had no hit ({d2})')
        if g and int(g.group(3)): red.append(f'launch 2: {g.group(3)} animation pre-decodes failed ({d2})')
        if not b or b[0].get('in_use') != 1: red.append(f'launch 2: no bootlist event with the learned list in use ({d2})')
        elif b[0].get('write'): red.append(f'launch 2 rewrote a list one launch old: {b[0]} ({d2})')
        print(f'launch 2: {hits} of {rows} blocks handed over decoded, {ghits} animation segments pre-decoded ({d2})')
    if not keep: shutil.rmtree(tmp, ignore_errors=True)
    print('BOOTLIST RED ' + '; '.join(red) if red else 'BOOTLIST OK')
    return 1 if red else 0


if __name__ == '__main__':
    sys.exit(main())
