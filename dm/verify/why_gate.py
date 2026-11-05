#!/usr/bin/env python3
"""A control for dm/tools/bench_why.py on the game itself: known spikes on the game thread and the GPU (DM_TEST_LOAD) must come back named, with the spinning function in the hot table, and the samples must
agree with the CPU clock on the threads that matter. Runs in front, as benches do. Last line WHY_GATE OK|RED {json}."""
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.normpath(os.path.join(HERE, '..'))
ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
SPIKES = 8   # at least this many of each, over the route


def main():
    p = subprocess.run([sys.executable, os.path.join(DM, 'tools', 'bench.py'), 'profile', os.path.join(DM, 'verify', 'bench', 'town_loop.scn'), '--cool', '0', '--env', 'DM_TEST_LOAD=spike:90:60,gspike:157:12000'], capture_output=True, text=True)
    bad = []; last = next((l for l in reversed(p.stdout.splitlines()) if l.startswith('RESULT ')), None)
    if p.returncode or not last: bad.append(f'the run failed: {p.stdout[-300:]}')
    out = {}
    if not bad:
        with open(json.loads(last[7:])['file'], encoding='utf-8') as f: why = json.load(f)
        causes = {}; self_fns = set(); agree = {}
        for s in why['segments'].values():
            for k, v in s['causes'].items(): causes[k] = causes.get(k, 0) + v['frames']
            for rl, v in s['hot'].items():
                if rl == 'game main': self_fns |= {r['fn'] for r in v['self'][:30]}
            agree.update({k: v for k, v in s['samples_vs_clock'].items() if k in ('game main', 'translation', 'game workers')})
        out = dict(causes=causes, agree=agree)
        if causes.get('the game thread', 0) < SPIKES: bad.append(f'game thread spikes not named: {causes}')
        if causes.get('the GPU', 0) < SPIKES / 2: bad.append(f'GPU spikes not named: {causes}')
        if 'dm_test_load_spin' not in self_fns: bad.append('the spinning function is not in the game thread\'s hot table')
        off = {k: v for k, v in agree.items() if abs(v - 1) > 0.35}
        if off: bad.append(f'samples off the CPU clock: {off}')
    print(('WHY_GATE RED ' if bad else 'WHY_GATE OK ') + json.dumps(dict(out, bad=bad), ensure_ascii=False))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
