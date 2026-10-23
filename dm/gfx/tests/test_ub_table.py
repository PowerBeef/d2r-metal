#!/usr/bin/env python3
"""ub_table.py 的单测：合成「UB 上屏」行，期望值按规则手算（配对差 = 变体相 − 相邻基准相均值）。 | Unit test for ub_table.py: synthetic "UB 上屏" lines, expected values computed by hand from the rules (paired difference = variant phase − mean of adjacent baseline phases)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'ub_table.py')


def ub(phase, name, mean, cpu=2.0, c3=0):   # 135 帧 = 2 拍 + 3 拍；慢帧 = 3 拍及以上 | 135 frames = 2 ticks + 3 ticks; slow frames = 3 ticks or more
    return f'[dm] [nvn-mtl] UB 上屏 {phase} {name} 帧 135 均 {mean:.3f} ms 2拍 {135 - c3} 3拍 {c3} 4拍以上 0 丢弃 0 翻译每帧 CPU {cpu:.3f} ms 周期 1.00 M\n'


def run(text, *args):
    with tempfile.NamedTemporaryFile('w', suffix='.log', delete=False) as f: f.write(text)
    try: r = subprocess.run([sys.executable, TOOL, f.name, *args], capture_output=True, text=True, check=False)
    finally: os.unlink(f.name)
    return r.returncode, json.loads(next(line for line in r.stdout.splitlines() if line.startswith('RESULT '))[7:])


class UbTableTests(unittest.TestCase):
    def test_slower_variant_with_drift(self):
        # 基准随时间从 16.7 漂到 17.5（温控），变体始终比相邻基准慢 1.0 ms：配对差 = +1.0，区间在 0 右侧 → 变慢
        # The baseline drifts from 16.7 to 17.5 over time (thermal), the variant is always 1.0 ms slower than its neighbours: paired difference = +1.0, interval right of 0 -> slower
        text, ph = '', 0
        for _ in range(6):
            b0 = 16.7 + 0.08 * ph; text += ub(ph, 'base', b0); ph += 1
            b1 = 16.7 + 0.08 * (ph + 1); text += ub(ph, 'x', (b0 + b1) / 2 + 1.0, cpu=2.5, c3=27); ph += 1
        text += ub(ph, 'base', 16.7 + 0.08 * ph)
        rc, res = run(text, '0')
        self.assertEqual(rc, 0)
        v = res['variants'][0]
        self.assertEqual((v['name'], v['phases'], v['verdict']), ('x', 6, 'slower'))
        self.assertAlmostEqual(v['diff_ms'], 1.0, places=2)
        self.assertGreater(v['ci95_ms'][0], 0)
        self.assertAlmostEqual(v['cpu_diff_ms'], 0.5, places=3)
        self.assertAlmostEqual(v['slow_share'], 0.2)   # 27 / 135

    def test_noise_is_no_difference(self):   # 变体与基准交替 ±0.2：中位数差 0，区间跨 0 | variant alternates ±0.2 around the baseline: median difference 0, interval spans 0
        text = ub(0, 'base', 16.7)
        for i in range(8): text += ub(2 * i + 1, 'y', 16.7 + (0.2 if i % 2 else -0.2)) + ub(2 * i + 2, 'base', 16.7)
        rc, res = run(text, '0')
        v = res['variants'][0]
        self.assertEqual((rc, v['verdict']), (0, 'no difference'))
        self.assertLessEqual(v['ci95_ms'][0], 0); self.assertGreaterEqual(v['ci95_ms'][1], 0)

    def test_skip_first_phases_and_empty(self):
        self.assertEqual(run('no ub lines\n')[0], 1)
        text = ub(0, 'base', 30) + ub(1, 'z', 40) + ub(2, 'base', 16.7) + ub(3, 'z', 17.7) + ub(4, 'base', 16.7)
        rc, res = run(text)   # 缺省跳过第 0 相：基准剩 2 相，z 第 1 相只配第 2 相 | phase 0 skipped by default: 2 base phases left, phase 1 of z pairs only with phase 2
        self.assertEqual((rc, res['base_phases'], res['variants'][0]['phases']), (0, 2, 2))
        rc, res = run(text, '2')   # 跳过前两相：z 只剩第 3 相，差 = 17.7 − 16.7 | skip two phases: only phase 3 of z remains, difference = 17.7 − 16.7
        self.assertEqual((rc, res['variants'][0]['phases']), (0, 1))
        self.assertAlmostEqual(res['variants'][0]['diff_ms'], 1.0, places=3)


if __name__ == '__main__':
    unittest.main()
