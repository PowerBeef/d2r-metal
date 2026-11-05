#!/usr/bin/env python3
"""Motion on the game itself: the temporal scaler and frame generation (DM_FG) on a walk, judged by dm/tools/motion_scan.py: shown frames in order and on time, generated frames half way, the
hero's limbs moving. A control runs the same walk on the fixed clock (DM_DETCLOCK), which freezes her pose whatever else is on: it must read as frozen, or a clean result says nothing.
Last line MOTION_GATE OK|RED {json}. Frames are recorded in the run's folder, 1280 x 720 of the middle of the picture, and removed afterwards."""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(DM, 'tools'))
import motion_scan  # noqa: E402
import picture  # noqa: E402

ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work'); OUT = os.path.join(ROOT, 'run', 'motion'); SCN = os.path.join(HERE, 'scenarios', 'picture', 'motion_walk.scn')
LATE_MAX = 0.01; OFF_MAX = 0.1; DETAIL_RANGE = (0.8, 1.25)   # a generated frame's detail against a real one's: nvn_fg.inc's sharpen brings it near 1; without it about 0.76


def run(name, env):
    d = os.path.join(OUT, name); shutil.rmtree(d, ignore_errors=True); os.makedirs(d); trace = os.path.join(d, 'trace.tsv')   # a run's folder holds its save: never reuse it
    e = ' '.join(f'{k}={v}' for k, v in {**picture.RECT, **env, 'DM_MV_TRACE': trace, 'DM_WATCH_STALL': '90'}.items())
    p = subprocess.run([sys.executable, os.path.join(DM, 'tools', 'scenario.py'), 'run', SCN, '--backend', 'cli', '--out', os.path.join(d, 'run'), '--env', e], capture_output=True, text=True)
    return p.returncode, d, trace


def main():
    bad = []
    rc, d, trace = run('fg', {'DM_FG': '1', 'DM_FRAME_REC': f"11:10:{os.path.join(OUT, 'fg', 'rec')}:640,360,1280,720"})
    if rc: bad.append('the frame generation run failed')
    rec = os.path.join(d, 'rec'); res = {}
    if not rc:
        res, _ = motion_scan.scan(motion_scan.Frames(rec)); res['limbs'] = motion_scan.limbs_scan(trace)
        c, h, l = res['cadence'], res['halfway'], res['limbs']
        if not l['judged']: bad.append('the hero never walked: nothing was judged')
        if l['flagged']: bad.append(f"frozen pose, {l['longest_frozen_run']} frames")
        if res['flags']: bad.append(f"picture motion: {res['flags']}")
        if c['generated'] < c['frames'] * 0.45: bad.append(f"only {c['generated']} of {c['frames']} shown frames were generated")
        if c['late_share'] > LATE_MAX: bad.append(f"{c['late_share']:.2%} of shown frames late")
        if res['repeats'] > c['frames'] * 0.01: bad.append(f"{res['repeats']} repeated frames")
        if not h.get('pairs') or h['off_median'] > OFF_MAX: bad.append(f'generated frames not half way: {h}')
        dt = res['detail']
        if not dt.get('pairs') or not DETAIL_RANGE[0] <= dt['median'] <= DETAIL_RANGE[1]: bad.append(f'generated frames not as sharp as real ones: {dt}')
    for f in (rec + '.raw',):
        if os.path.exists(f): os.remove(f)
    rc2, _, trace2 = run('control', {'DM_DETCLOCK': '16666667'})
    ctl = motion_scan.limbs_scan(trace2) if not rc2 else {}
    if rc2: bad.append('the control run failed')
    elif not ctl['flagged']: bad.append(f'the control (fixed clock) did not read as a frozen pose, so the check is blind: {ctl}')
    out = dict(fg=res, control=ctl, bad=bad)
    print(('MOTION_GATE RED ' if bad else 'MOTION_GATE OK ') + json.dumps(out))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
