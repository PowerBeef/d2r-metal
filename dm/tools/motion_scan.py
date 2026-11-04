#!/usr/bin/env python3
"""Measures motion and animation in a run's shown frames, so that faults a still picture hides are caught by a number instead of by someone watching.

Reads a DM_FRAME_REC recording (nvn_framerec.inc: <prefix>.raw and <prefix>.tsv) or any video file, and a DM_MV_TRACE file (nvn_mv.inc). Reports:
  cadence   time between shown frames, late ones, and the generated / real order (recordings only)
  repeats   frames identical to the one before
  world     how far the picture moves between shown frames while the hero walks: steps that go backwards are visible judder
  halfway   frame generation: whether each generated frame sits half way between its two real neighbours
  limbs     the hero's animation (--trace). The camera follows her, so her body stays still on the screen and what moves in her area is her limbs. A run of frames with next to no limb
            motion while the camera moves is a frozen pose, which is what a "slide show" walk looks like. From the velocity target, so no picture or video compression is involved

  motion_scan.py <prefix | video> [--trace file] [--fps N] [--sheet out.png] [--clip out.mp4] [--clip-range first,count] [--freeze-max N]
  motion_scan.py --trace file       the limbs alone, with no frames

--sheet writes 16 consecutive frames of the first walk, cropped on the hero. --clip writes frames as an H.264 video at their own rate.
The last line is RESULT {json}; exit 0 when nothing is flagged, 1 otherwise. dm/verify/test_motion_scan.py pins the detectors; dm/verify/motion_gate.py runs them on the game, with a control.

A pixel check of the legs' area was tried first and dropped: it did not flag clips whose hero was known to be frozen (healthy runs dip just the same whenever her legs cross).
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

STILL_PX = 0.5          # picture motion below this (in pixels of the frame) counts as standing still
CAM_WALK_PX = 1.0       # camera motion per drawn frame above which the hero counts as walking (limbs)
FROZEN_LIMB_PX = 1.0    # a drawn frame's fastest limb pixels (95th percentile) below this count as a frozen pose. Measured on an M6, town walk: healthy frames 2.7-17 px, frozen ones 0.0-0.1
FREEZE_MAX = 3          # the longest run of frozen frames allowed. Healthy runs had at most 1; the fixed clock (DM_DETCLOCK), which does freeze the pose, had 16 and 33


class Frames:
    """Frames from a recording (memory-mapped) or a video (decoded on demand). get(i) -> H x W x 3 uint8."""
    def __init__(self, path, fps=None):
        self.rows = None
        if os.path.exists(path + '.raw'):
            with open(path + '.tsv', encoding='utf-8') as f:
                head = f.readline().split(); lines = [l.rstrip('\n').split('\t') for l in f]
            self.w, self.h = int(head[2]), int(head[4])
            keys = lines[0]; self.rows = [dict(zip(keys, l)) for l in lines[1:] if len(l) == len(keys)]   # a run that quits mid-recording leaves part of a row and of a frame
            raw = np.memmap(path + '.raw', dtype=np.uint8, mode='r'); size = self.h * self.w * 4
            self.data = raw[:len(raw) // size * size].reshape(-1, self.h, self.w, 4); self.n = min(len(self.data), len(self.rows)); self.fps = fps or 60.0
            return
        out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries', 'stream=width,height,avg_frame_rate', '-of', 'csv=p=0', path], capture_output=True, text=True, check=True).stdout.strip().split(',')
        self.w, self.h = int(out[0]), int(out[1]); num, den = out[2].split('/'); self.fps = fps or float(num) / float(den)
        raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True).stdout
        self.data = np.frombuffer(raw, dtype=np.uint8).reshape(-1, self.h, self.w, 3); self.n = len(self.data)

    def get(self, i):
        return np.asarray(self.data[i][..., :3])


def phase_shift(a, b):
    """The whole-picture motion from b to a, in pixels, by phase correlation, with a sub-pixel peak."""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    A = np.fft.fft2((a - a.mean()) * win); B = np.fft.fft2((b - b.mean()) * win)
    C = A * np.conj(B); C /= np.abs(C) + 1e-9
    c = np.fft.ifft2(C).real
    y, x = np.unravel_index(np.argmax(c), c.shape); h, w = c.shape

    def sub(m, o, p):
        d = m - 2 * o + p
        return 0.5 * (m - p) / d if d else 0.0
    return (x if x < w / 2 else x - w) + sub(c[y, (x - 1) % w], c[y, x], c[y, (x + 1) % w]), (y if y < h / 2 else y - h) + sub(c[(y - 1) % h, x], c[y, x], c[(y + 1) % h, x])


def world_steps(fr, step=4):
    """Picture motion between consecutive frames, in pixels of the frame. The interface band and the frame's edges are left out; every step-th pixel is enough."""
    h, w = fr.h, fr.w
    y0, y1, x0, x1 = int(h * .08), int(h * .78), int(w * .12), int(w * .88)
    prev = None; out = [(0.0, 0.0)]
    for i in range(fr.n):
        g = fr.get(i)[y0:y1:step, x0:x1:step, 1].astype(np.float32)
        if prev is not None:
            dx, dy = phase_shift(g, prev); out.append((dx * step, dy * step))
        prev = g
    return out


def runs_of(flags):
    """[(first index, length)] of the runs of true values."""
    out = []; start = None
    for i, f in enumerate(flags):
        if f and start is None: start = i
        if not f and start is not None: out.append((start, i - start)); start = None
    if start is not None: out.append((start, len(flags) - start))
    return out


def limbs_scan(path, freeze_max=FREEZE_MAX):
    """The hero's limb motion per drawn frame while the camera moves (a DM_MV_TRACE file), and the longest run of frames with next to none."""
    with open(path, encoding='utf-8') as f:
        keys = f.readline().rstrip('\n').split('\t'); rows = [dict(zip(keys, l.rstrip('\n').split('\t'))) for l in f if l.strip()]
    walking = [float(r['limb_p95']) for r in rows if float(r['cam_px']) > CAM_WALK_PX and int(r['hero_px']) > 0]
    frozen = [v < FROZEN_LIMB_PX for v in walking]; longest = max((n for _, n in runs_of(frozen)), default=0)
    return dict(frames=len(rows), walking=len(walking), frozen=int(sum(frozen)), frozen_share=round(sum(frozen) / max(1, len(walking)), 4), longest_frozen_run=longest,
                limb_median_px=round(float(np.median(walking)), 2) if walking else None, flagged=bool(walking) and longest > freeze_max, judged=bool(walking))


def world_scan(steps):
    """Evenness of the picture's motion while it moves: the spread of the step, and steps against the direction of travel."""
    mags = np.array([np.hypot(*s) for s in steps]); moving = mags > STILL_PX
    w = [(i, n) for i, n in runs_of(list(moving)) if n >= 10]
    spread = []; back = 0; total = 0
    for i, n in w:
        m = mags[i:i + n]; spread.append(float(m.std() / max(m.mean(), 1e-6)))
        for k in range(i + 1, i + n):
            total += 1
            if steps[k][0] * steps[k - 1][0] + steps[k][1] * steps[k - 1][1] < 0: back += 1
    return dict(walks=len(w), moving_frames=int(moving.sum()), step_median_px=round(float(np.median(mags[moving])), 2) if moving.any() else 0.0, step_spread=round(float(np.median(spread)), 3) if spread else None, reversals=back, reversal_share=round(back / max(1, total), 4))


def halfway_scan(fr, steps):
    """Each generated frame's step against the real frame's step after it: equal when the generated frame sits half way."""
    if not fr.rows: return None
    errs = []
    for i in range(1, fr.n - 1):
        if fr.rows[i]['kind'] != 'gen' or fr.rows[i + 1]['kind'] != 'real': continue
        a, b = steps[i], steps[i + 1]
        if np.hypot(*a) + np.hypot(*b) < STILL_PX * 2: continue
        errs.append(np.hypot(a[0] - b[0], a[1] - b[1]) / (np.hypot(a[0] + b[0], a[1] + b[1]) + 1e-6))
    return dict(pairs=len(errs), off_median=round(float(np.median(errs)), 3), off_p95=round(float(np.percentile(errs, 95)), 3)) if errs else dict(pairs=0)


def cadence_scan(fr):
    if not fr.rows: return None
    t = [float(r['target_s']) for r in fr.rows[:fr.n]]; t = [x for x in t if x > 0]; kinds = [r['kind'] for r in fr.rows[:fr.n]]
    if len(t) < 2: return dict(frames=fr.n, generated=kinds.count('gen'))
    dt = np.diff(t) * 1000; tick = float(np.median(dt)); same = sum(1 for a, b in zip(kinds, kinds[1:]) if a == b and a == 'gen')
    return dict(frames=fr.n, generated=kinds.count('gen'), interval_median_ms=round(tick, 2), late=int((dt > tick * 1.5).sum()), late_share=round(float((dt > tick * 1.5).mean()), 4), longest_ms=round(float(dt.max()), 1), generated_twice_in_a_row=same)


def repeats_scan(fr, step=4):
    same = 0; prev = None
    for i in range(fr.n):
        g = fr.get(i)[::step, ::step, 1]
        if prev is not None and np.array_equal(g, prev): same += 1
        prev = g
    return same


def first_walk(steps, need=30):
    run = 0
    for i, s in enumerate(steps):
        run = run + 1 if np.hypot(*s) > STILL_PX else 0
        if run == need: return i - need + 1
    return None


def write_sheet(fr, start, box, path):
    from PIL import Image, ImageDraw   # noqa: PLC0415
    cx, cy, bw, _ = box; ch = bw * 5 // 4; x0 = max(0, cx - bw // 2); y0 = max(0, cy - ch * 2 // 3)
    o = Image.new('RGB', (240 * 8, 300 * 2))
    for k in range(16):
        i = min(start + k, fr.n - 1); im = Image.fromarray(fr.get(i)[y0:y0 + ch, x0:x0 + bw]).resize((240, 300))
        ImageDraw.Draw(im).text((4, 4), f"{i} {fr.rows[i]['kind'] if fr.rows else ''}", fill=(255, 255, 0)); o.paste(im, ((k % 8) * 240, (k // 8) * 300))
    o.save(path)


def write_clip(fr, first, count, path):
    p = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{fr.w}x{fr.h}', '-r', str(round(fr.fps)), '-i', '-', '-vf', 'scale=1920:-2:flags=lanczos', '-c:v', 'libx264', '-preset', 'medium', '-crf', '19', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', path], stdin=subprocess.PIPE)
    for i in range(first, min(first + count, fr.n)): p.stdin.write(fr.get(i).tobytes())
    p.stdin.close(); p.wait()


def scan(fr):
    steps = world_steps(fr)
    res = dict(frames=fr.n, size=[fr.w, fr.h], fps=round(fr.fps, 2), cadence=cadence_scan(fr), repeats=repeats_scan(fr), world=world_scan(steps), halfway=halfway_scan(fr, steps))
    res['flags'] = ['reversals'] if res['world']['reversal_share'] > 0.02 else []
    return res, steps


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', nargs='?'); ap.add_argument('--trace'); ap.add_argument('--fps', type=float); ap.add_argument('--sheet'); ap.add_argument('--clip'); ap.add_argument('--clip-range'); ap.add_argument('--freeze-max', type=int, default=FREEZE_MAX)
    a = ap.parse_args(argv)
    if not a.source and not a.trace: ap.error('a recording, a video or --trace is needed')
    res, steps, fr = dict(flags=[]), None, None
    if a.source:
        fr = Frames(a.source, a.fps); res, steps = scan(fr)
    if a.trace:
        res['limbs'] = limbs_scan(a.trace, a.freeze_max)
        if res['limbs']['flagged']: res['flags'].append('frozen_pose')
    if a.sheet and fr:
        s = first_walk(steps)
        if s is not None: write_sheet(fr, s + 10, (fr.w // 2, fr.h * 49 // 100, fr.w * 12 // 100, 0), a.sheet)
    if a.clip and fr:
        f, c = (int(x) for x in a.clip_range.split(',')) if a.clip_range else (0, fr.n)
        write_clip(fr, f, c, a.clip)
    print('RESULT ' + json.dumps(res))
    return 1 if res['flags'] else 0


if __name__ == '__main__':
    sys.exit(main())
