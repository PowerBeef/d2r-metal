#!/usr/bin/env python3
# Synthetic-log cases for smoke_mac.sh's Frame Drops step check (dm/ios/tools/s3_steps.py); no game content or Apple SDK needed.
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 's3_steps.py'


def perf(c16, c25, c33, disc, fps=None):
    if fps is None:   # the frame rate the [perf] line prints: shown frames over their intervals (16.667 / 25 / 33.4 ms)
        ms = c16 * 16.667 + c25 * 25.0 + c33 * 33.4
        fps = (c16 + c25 + c33) * 1000 / ms if ms else 60.0
    return f'[dm] [perf] 温控 0 3D 0.75 帧率 {fps:.1f} 三档 {c16}/{c25}/{c33} 丢弃 {disc}'


def step(r10, r60):
    return f'[dm] 3D 比例控制：最近 10 秒掉帧 {r10:.1f}%、60 秒 {r60:.1f}% → 降到第 1 档（3D 比例 0.70）'


class S3StepsTest(unittest.TestCase):
    def judge(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'player.txt'
            path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
            out = subprocess.run([sys.executable, '-I', str(SCRIPT), str(path)], check=True, capture_output=True, text=True).stdout.split()
            return int(out[0]), int(out[1])

    def test_drops_split_across_two_full_windows_are_a_good_step(self):
        # 32 drops of 33.4 ms in the controller's 10 s span (5.3%), 16 in each window: each window is about 2.7% drops at 58.4 fps
        self.assertEqual(self.judge([perf(600, 0, 0, 0), perf(584, 0, 16, 0), step(5.3, 0.0), perf(584, 0, 16, 0)]), (0, 0))

    def test_full_windows_with_no_drops_are_a_bad_step(self):
        self.assertEqual(self.judge([perf(600, 0, 0, 0), perf(601, 0, 0, 0), step(22.5, 0.0), perf(600, 0, 0, 0)]), (1, 0))

    def test_late_count_bound(self):
        # Under 28 late or not-shown frames across both windows the 10 s rule cannot have fired; from 28 on it can
        self.assertEqual(self.judge([perf(590, 0, 13, 0), step(5.1, 0.0), perf(590, 0, 12, 2)]), (1, 0))
        self.assertEqual(self.judge([perf(590, 0, 14, 0), step(5.1, 0.0), perf(590, 0, 12, 2)]), (0, 0))
        self.assertEqual(self.judge([perf(590, 10, 4, 0), step(5.1, 0.0), perf(590, 10, 3, 0)]), (1, 0))

    def test_sixty_second_rule_is_left_alone(self):
        self.assertEqual(self.judge([perf(600, 0, 0, 0), step(0.5, 1.6), perf(600, 0, 0, 0)]), (0, 0))

    def test_blank_windows_are_a_bad_step(self):
        self.assertEqual(self.judge([perf(0, 0, 0, 599, 59.9), step(90.5, 0.0), perf(0, 0, 0, 600, 60.0)]), (1, 0))

    def test_partly_covered_windows_are_not_counted(self):
        self.assertEqual(self.judge([perf(491, 0, 0, 109, 60.0), step(32.2, 0.0), perf(0, 0, 0, 600, 60.0)]), (0, 0))

    def test_no_window_after_the_step_is_not_judged(self):
        self.assertEqual(self.judge([perf(600, 0, 0, 0), step(22.5, 0.0)]), (0, 1))

    def test_windows_above_sixty_fps_are_not_full(self):
        # 72 fps pacing (a 144 Hz screen): the late-frame bound is worked out for 60 fps, so these windows are not judged as full
        self.assertEqual(self.judge([perf(720, 0, 0, 0, 72.0), step(6.0, 0.0), perf(720, 0, 0, 0, 72.0)]), (0, 0))

    def test_missing_log(self):
        out = subprocess.run([sys.executable, '-I', str(SCRIPT), '/nonexistent/player.txt'], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ['0', '0'])


if __name__ == '__main__':
    unittest.main()
