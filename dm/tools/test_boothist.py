"""boothist.py 的单测：从合成日志（启动时间线表 + 事件行）取阶段、记账、按前后两组比，不跑游戏。 | Unit tests for boothist.py: stages from a synthetic log (boot timeline table + event lines), recording, comparing the two latest groups; the game is not run."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parent / 'boothist.py'


def log(scale):
    rows = [f'[dm] ⓪ 进程创建 → 宿主 main（dyld）  {0.10 * scale:.3f} {0.10 * scale:.3f}', f'[dm] ⑥ → MainMenuButtonRibbon  {0.80 * scale:.3f} {1.20 * scale:.3f}']
    evs = [dict(seq=1, t=0.03 * scale, ev='boot', stage='host_loaded'), dict(seq=2, t=1.25 * scale, ev='main_menu'), dict(seq=3, t=3.3 * scale, ev='loading_done')]
    return '\n'.join(rows + ['[dm] EV ' + json.dumps(e) for e in evs]) + '\n'


class BoothistTest(unittest.TestCase):
    def run_tool(self, root, *args):
        p = subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, env=dict(os.environ, DM_ROOT=root), check=False)
        return p.returncode, p.stdout

    def test_record_and_show(self):
        with tempfile.TemporaryDirectory() as d:
            files = []
            for i, sc in enumerate([1.0, 1.0, 1.0, 1.3, 1.3, 1.3]):   # 后三次全都慢 30% | the last three are all 30% slower
                f = Path(d) / f'r{i}.log'; f.write_text(log(sc), encoding='utf-8'); files.append(str(f))
            rc, out = self.run_tool(d, 'record', *files, '--label', 't')
            self.assertEqual(rc, 0); self.assertIn('recorded 6 runs', out)
            rc, out = self.run_tool(d, 'show', '--label', 't', '--last', '3')
            res = json.loads(out.splitlines()[-1][7:])
            st = {s['stage']: s for s in res['stages']}
            self.assertEqual(sorted(st), sorted(['prof ⓪', 'prof ⑥', 'host_loaded', 'main_menu', 'loading_done']))
            self.assertAlmostEqual(st['loading_done']['recent'], 4.29); self.assertAlmostEqual(st['loading_done']['before'], 3.3)
            self.assertTrue(st['loading_done']['slower']); self.assertFalse(st['host_loaded']['slower'])   # 9 ms：不到 50 ms 不算 | 9 ms: under 50 ms does not count
            self.assertEqual(rc, 1)
            self.assertEqual(self.run_tool(d, 'show', '--label', 'other')[0], 0)


if __name__ == '__main__':
    unittest.main()
