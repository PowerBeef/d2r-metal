#!/usr/bin/env python3
"""soak_report.py 的单测：合成的浸泡表 + 宿主日志，四项判据各造一个坏例，期望值按规则手算。 | Unit test for soak_report.py: a synthetic soak table + host log, one bad case per criterion, expected values computed by hand from the rules."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'soak_report.py')
HEAD = '秒\t拍号\t句柄\t存活线程\t线程对象\tmalloc_MiB\tfootprint_MiB\n'


def rows(n=30, tps=25, leak=0.0, thread_step=0, stall_at=None, rejoin_every=None):   # 每 60 秒一行；前 1 行未进局（拍号 -1）；rejoin_every：每隔几行出局再进一局（拍号从头数） | one row every 60 s; the first row is before entering the run (tick -1); rejoin_every: every so many rows leave and enter another run (ticks count from the start)
    out, tick = [HEAD, '60\t-1\t40\t20\t30\t500.0\t1500.0\n'], 0
    for i in range(2, n + 2):
        t = i * 60
        if rejoin_every and i % rejoin_every == 0: tick = 0
        if stall_at is None or i < stall_at: tick += tps * 60
        out.append(f'{t}\t{tick}\t40\t{20 + (thread_step * i if t >= 300 else 0)}\t30\t500.0\t{1500.0 + leak * i:.1f}\n')
    return ''.join(out)


def log(p95s):
    return ''.join(f'[dm] [nvn] 计时 至帧 {300 * (i + 1)}： 帧时间 p50 16.67 / p95 {v:.2f} / 最大 30 ms\n' for i, v in enumerate(p95s))


class SoakReportTests(unittest.TestCase):
    def run_tool(self, tsv, text, **env):
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, 'soak.tsv'), os.path.join(d, 'boot.txt')
            with open(a, 'w') as f: f.write(tsv)
            with open(b, 'w') as f: f.write(text)
            r = subprocess.run([sys.executable, TOOL, a, b], capture_output=True, text=True, env=dict(os.environ, **env), check=False)
        res = json.loads(next(line for line in r.stdout.splitlines() if line.startswith('RESULT '))[7:])
        return r.returncode, res, {c['name']: c['ok'] for c in res['checks']}

    def test_healthy_soak_is_ok(self):
        rc, res, ok = self.run_tool(rows(), log([18.0] * 10))
        self.assertEqual((rc, res['verdict']), (0, 'OK'))
        self.assertEqual(ok, dict(ticks=True, memory=True, handles=True, p95=True))

    def test_leak_is_red(self):   # 每分钟 +3 MiB > 上限 1 | +3 MiB per minute > limit 1
        rc, res, ok = self.run_tool(rows(leak=3.0), log([18.0] * 10))
        self.assertEqual((rc, ok['memory'], ok['ticks']), (1, False, True))

    def test_rejoins_are_not_a_tick_regression(self):   # 出局再进：新一局拍号从头数，每局里照样递增 → 通过（Mac 试玩机器人浸泡的「保存并退出再进」） | leave and enter again: the new run counts ticks from the start and they still rise within each run -> OK (the Mac playtest-bot soak's "save and exit, enter again")
        rc, res, ok = self.run_tool(rows(rejoin_every=7), log([18.0] * 10))
        self.assertEqual((rc, ok['ticks']), (0, True))
        self.assertGreater(next(c for c in res['checks'] if c['name'] == 'ticks')['runs'], 3)

    def test_stall_inside_one_of_several_runs_is_red(self):   # 分了几局也照样抓局内停滞 | a stall inside one of several runs is still caught
        rc, _, ok = self.run_tool(rows(rejoin_every=7, stall_at=24), log([18.0] * 10))
        self.assertEqual((rc, ok['ticks']), (1, False))

    def test_tick_stall_is_red(self):
        rc, _, ok = self.run_tool(rows(stall_at=20), log([18.0] * 10))
        self.assertEqual((rc, ok['ticks']), (1, False))

    def test_thread_growth_is_red(self):   # 预热后每分钟多 1 条线程，极差远超 2 | one more thread per minute after warm-up, range far above 2
        rc, _, ok = self.run_tool(rows(thread_step=1), log([18.0] * 10))
        self.assertEqual((rc, ok['handles']), (1, False))

    def test_p95_regression_is_red(self):   # 前半 18 → 后半 22 > 18 × 1.15 + 0.5 = 21.2 | first half 18 -> second half 22 > 18 × 1.15 + 0.5 = 21.2
        rc, res, ok = self.run_tool(rows(), log([18.0] * 5 + [22.0] * 5))
        self.assertEqual((rc, ok['p95']), (1, False))
        self.assertAlmostEqual(next(c for c in res['checks'] if c['name'] == 'p95')['limit_ms'], 21.2)

    def test_threshold_from_environment(self):   # 放宽斜率上限后同一份泄漏数据通过 | with a looser slope limit the same leaking data passes
        rc, _, ok = self.run_tool(rows(leak=3.0), log([18.0] * 10), SOAK_MAX_SLOPE='5')
        self.assertEqual((rc, ok['memory']), (0, True))

    def test_not_enough_data_is_red(self):
        rc, _, ok = self.run_tool(HEAD + '60\t-1\t40\t20\t30\t500.0\t1500.0\n', log([18.0] * 2))
        self.assertEqual(rc, 1)
        self.assertFalse(any(ok.values()))


if __name__ == '__main__':
    unittest.main()
