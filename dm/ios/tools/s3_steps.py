#!/usr/bin/env python3
# Frame Drops steps in a host log (smoke_mac.sh's player path), judged by the [perf] windows around each step-down line: the one before it and the
# one it falls in, across which the controller (hle_oe.c scale3d_ctl_step) decided it. Prints "bad notjudged".
# - A window is blank when 99% or more of its presents were not shown (the line's discarded count), and full when under 1% were and it ran at 58 to
#   61.5 fps (the 60 fps pacing of the Macs this runs on).
# - A step is bad when both neighbours are blank (nothing on screen: the controller must not judge such windows), or when both are full, the 60 s
#   rate it printed is 1.5% or less and fewer than FULL_LATE_MAX frames across the two windows were late (over 20.8 ms: the 25 and 33 ms buckets)
#   or not shown. The controller's 10 s span lies inside the two windows but rarely lines up with one, so their drops are counted together: two full
#   windows at 60 fps leave at most about 0.7 s of late time, so the span holds at least about 558 frames and the 10 s rule needs at least 28 drops
#   in it, each one a late (over 25.25 ms) or not-shown frame of the two windows. The 60 s rule can fire legitimately at about 59 fps, so a step
#   that printed more than 1.5% is left alone.
# - A step with no window after it is not judged, nor is one in partly covered windows (occlusion flicker of the iOS-on-Mac window is a test
#   artefact).
# Usage: s3_steps.py <log>   (a missing log reads as no steps: "0 0")
import re
import sys

PERF = re.compile(r"\[perf\].*帧率 ([0-9.]+) 三档 (\d+)/(\d+)/(\d+) 丢弃 (\d+)")
STEP = re.compile(r"3D 比例控制：最近 10 秒掉帧 ([0-9.]+)%、60 秒 ([0-9.]+)% → 降到第")
FULL_LATE_MAX = 28


def window(m):
    fps = float(m.group(1)); c16, c25, c33, disc = map(int, m.groups()[1:]); total = c16 + c25 + c33 + disc
    return {'blank': disc >= 0.99 * total, 'full': disc < 0.01 * total and 58.0 <= fps <= 61.5, 'late': c25 + c33 + disc}


def judge(lines):
    ev = []
    for line in lines:
        m = PERF.search(line)
        if m: ev.append(('p', window(m))); continue
        m = STEP.search(line)
        if m: ev.append(('s', float(m.group(2))))
    bad = nj = 0
    for i, (kind, val) in enumerate(ev):
        if kind != 's': continue
        pre = next((w for k, w in reversed(ev[:i]) if k == 'p'), None); post = next((w for k, w in ev[i + 1:] if k == 'p'), None)
        if not pre or not post: nj += 1; continue
        bad += (pre['blank'] and post['blank']) or (pre['full'] and post['full'] and val <= 1.5 and pre['late'] + post['late'] < FULL_LATE_MAX)
    return bad, nj


def main():
    try: lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
    except (OSError, IndexError): lines = []
    print(*judge(lines))


if __name__ == '__main__':
    main()
