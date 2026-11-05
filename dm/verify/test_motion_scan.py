#!/usr/bin/env python3
"""dm/tools/motion_scan.py on synthetic motion: a known camera step is measured, a frame out of place reads as a reversal, a generated frame is judged by how close to half way it sits,
and the limbs' trace flags a frozen pose only past the limit."""
import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import motion_scan as M  # noqa: E402


class Fake:
    """Frames that are one noise texture, moved by the given offsets; rows mark generated and real frames."""
    def __init__(self, offsets, kinds=None):
        self.w, self.h = 640, 360; self.fps = 60.0; self.n = len(offsets); self.offsets = offsets
        rng = np.random.default_rng(7); t = rng.integers(0, 255, (self.h // 4, self.w // 4), dtype=np.uint8)
        self.tex = np.kron(t, np.ones((4, 4), dtype=np.uint8))
        self.rows = [dict(kind=k, target_s=f'{i / 60:.6f}') for i, k in enumerate(kinds)] if kinds else None

    def get(self, i):
        dx, dy = self.offsets[i]; g = np.roll(np.roll(self.tex, dy, 0), dx, 1)
        return np.stack([g, g, g], -1)


def write_trace(path, values, cam=8.0):
    with open(path, 'w', encoding='utf-8') as f:
        f.write('frame\tt_ms\tcam_px\thero_px\tlimb_p50\tlimb_p95\n')
        for i, v in enumerate(values): f.write(f'{i}\t{i * 16.7:.2f}\t{cam}\t7000\t{v / 2}\t{v}\n')


class MotionScanTest(unittest.TestCase):
    def test_even_steps_are_measured_and_clean(self):
        fr = Fake([(8 * i, 4 * i) for i in range(40)]); res, steps = M.scan(fr)
        self.assertAlmostEqual(steps[5][0], 8, delta=1); self.assertAlmostEqual(steps[5][1], 4, delta=1)
        self.assertEqual(res['world']['reversals'], 0); self.assertEqual(res['flags'], [])

    def test_a_frame_out_of_place_reads_as_reversals(self):
        o = [(8 * i, 0) for i in range(40)]
        for i in range(4, 36, 4): o[i], o[i + 1] = o[i + 1], o[i]
        res, _ = M.scan(Fake(o)); self.assertGreater(res['world']['reversals'], 0); self.assertIn('reversals', res['flags'])

    def test_repeated_frames_are_counted(self):
        o = [(8 * i, 0) for i in range(20)]; o[10] = o[9]; o[15] = o[14]
        self.assertEqual(M.scan(Fake(o))[0]['repeats'], 2)

    def test_halfway(self):
        n = 40; kinds = ['real' if i % 2 == 0 else 'gen' for i in range(n)]
        good = Fake([(8 * i, 0) for i in range(n)], kinds)   # whole steps of the scan's sampling (world_steps: every 4th pixel)
        self.assertLess(M.scan(good)[0]['halfway']['off_median'], 0.05)
        late = Fake([(16 * (i // 2) + (12 if i % 2 else 0), 0) for i in range(n)], kinds)   # generated at 3/4 of the way, not 1/2
        self.assertGreater(M.scan(late)[0]['halfway']['off_median'], 0.3)

    def test_latency(self):
        n = 120; o = [(0, 0) for _ in range(n)]
        for i in range(60, n): o[i] = (8 * (i - 59), 0)   # the picture starts moving at frame 60
        kinds = ['real'] * n; fr = Fake(o, kinds); _, steps = M.scan(fr)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'e.ndjson')
            with open(p, 'w', encoding='utf-8') as f:
                f.write('{"ev":"frame_rec_start","t":10.0,"media_s":0.0}\n{"ev":"mark","name":"in1","t":10.9}\n')   # media 0.9 s, frame 54: motion 6 frames on, at 1.0 s
            r = M.latency_scan(fr, steps, p)
        self.assertEqual(r['trials'], 1); self.assertAlmostEqual(r['mean_ms'], 100, delta=2)

    def test_detail(self):
        n = 40; kinds = ['real' if i % 2 == 0 else 'gen' for i in range(n)]; fr = Fake([(8 * i, 0) for i in range(n)], kinds); plain = fr.get
        _, steps = M.scan(fr); self.assertAlmostEqual(M.detail_scan(fr, steps)['median'], 1.0, delta=.05)
        def soft(i):   # a generated frame blurred sideways keeps less of its detail
            a = plain(i)
            return ((a.astype(np.uint16) + np.roll(a, 4, 1)) // 2).astype(np.uint8) if kinds[i] == 'gen' else a
        fr.get = soft; self.assertLess(M.detail_scan(fr, steps)['median'], .8)

    def test_solid(self):
        rng = np.random.default_rng(3); w, h = 400, 300
        base = rng.integers(60, 180, (h, w, 3), dtype=np.uint8)
        def picture(step, staff=255):   # the ground slid by step pixels; the staff stays put on the screen, as the hero does
            a = np.dstack([np.roll(base, step, 1), np.full((h, w), 255, np.uint8)])
            if staff == 255: a[100:190, 196:204, :3] = 10
            return a
        mv = np.zeros((h, w, 2), np.float16); mv[..., 0] = 20 / w; mv[100:190, 196:204, 0] = 0.0   # 20 pixels a frame: this pixel was 20 to the right last frame
        def put(d, name, arr):
            with open(os.path.join(d, name), 'wb') as f: f.write(f'{w} {h} 0 {w * arr.shape[2] * arr.dtype.itemsize}\n'.encode()); f.write(arr.tobytes())
        with tempfile.TemporaryDirectory() as d:
            put(d, 'd_1_real.raw', picture(20)); put(d, 'd_2_real.raw', picture(0)); put(d, 'd_2_mv.raw', mv)
            put(d, 'd_2_comp.raw', picture(10))
            r = M.solid_scan(os.path.join(d, 'd_')); self.assertEqual(r['pairs'], 1); self.assertLess(r['lost'], 0.02); self.assertLess(r['pale'], 0.02)
            put(d, 'd_2_comp.raw', picture(10, staff=0))   # the staff is gone from the generated frame
            self.assertGreater(M.solid_scan(os.path.join(d, 'd_'))['lost'], 0.5)
            c = picture(10); c[100:190, 210:218, :3] = (c[100:190, 210:218, :3] * 0.6).astype(np.uint8)   # a pale copy beside it
            self.assertGreater(M.solid_scan(os.path.join(d, 'd_'))['pale'] if put(d, 'd_2_comp.raw', c) is None else 0, 0.1)

    def test_frozen_pose_is_flagged_past_the_limit_only(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't.tsv')
            write_trace(p, [6.0] * 50); r = M.limbs_scan(p); self.assertFalse(r['flagged']); self.assertEqual(r['frozen'], 0); self.assertTrue(r['judged'])
            write_trace(p, [6.0] * 20 + [0.0] * M.FREEZE_MAX + [6.0] * 20); self.assertFalse(M.limbs_scan(p)['flagged'])
            write_trace(p, [6.0] * 20 + [0.0] * (M.FREEZE_MAX + 1) + [6.0] * 20); r = M.limbs_scan(p); self.assertTrue(r['flagged']); self.assertEqual(r['longest_frozen_run'], M.FREEZE_MAX + 1)

    def test_standing_still_is_not_judged(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't.tsv'); write_trace(p, [0.0] * 50, cam=0.0); r = M.limbs_scan(p)
            self.assertFalse(r['flagged']); self.assertFalse(r['judged'])


if __name__ == '__main__':
    unittest.main()
