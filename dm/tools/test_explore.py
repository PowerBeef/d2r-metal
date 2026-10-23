"""explore.py 的单测：同种子同操作串、生成的场景能被场景运行器解析、ddmin 缩到最短且守预算、红因指纹抹掉数字。 | Unit tests for explore.py: same seed same sequence, generated scenarios parse in the scenario runner, ddmin shrinks to the shortest and keeps to its budget, red fingerprints blank out numbers."""
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import explore  # noqa: E402
import scenario  # noqa: E402


class ExploreTest(unittest.TestCase):
    def test_actions_seeded_and_parse(self):
        self.assertEqual(explore.actions(7, 50), explore.actions(7, 50))
        self.assertNotEqual(explore.actions(7, 50), explore.actions(8, 50))
        with tempfile.TemporaryDirectory() as d:
            for seed in range(20):   # 生成的每种步骤都得是运行器认得的写法 | every generated step must be in a form the runner accepts
                p = Path(d) / f's{seed}.scn'
                p.write_text(explore.scenario_text(f'x{seed}', 'warlock', explore.actions(seed, 30), 'abc12345', 300), encoding='utf-8')
                meta, steps, checks = scenario.parse(str(p))
                self.assertEqual((meta['save'], len(checks)), ('warlock', 2))
                self.assertGreaterEqual(len(steps), 31)
                self.assertFalse({st['op'] for st in steps} & {'quit', 'crash', 'app', 'background', 'system_quit', 'open_pause', 'close_pause', 'expect', 'expect_state', 'tap_ui', 'await_ui'})

    def test_ddmin(self):
        items = list('abcdefghijklmnop')
        calls = []
        def fails(sub):
            calls.append(len(sub)); s = ''.join(sub); return 'c' in s and 'k' in s and s.index('c') < s.index('k')
        small, runs = explore.ddmin(items, fails, 100)
        self.assertEqual(small, ['c', 'k']); self.assertEqual(runs, len(calls))
        small, runs = explore.ddmin(items, fails, 3)
        self.assertLessEqual(runs, 3); self.assertTrue(fails(small))
        self.assertEqual(explore.ddmin(items, lambda _sub: True, 10), ([], 1))   # 空串也红：毛病不在操作里 | red even when empty: the fault is not in the actions

    def test_signature(self):
        self.assertIsNone(explore.signature(dict(verdict='OK')))
        crash = dict(verdict='RED', reason='step 3 (tap 0.1 0.2): crash SIGSEGV at main+0x455aa4 fn_455a80+0x24; tap: no reply')
        self.assertEqual(explore.signature(crash), 'crash SIGSEGV at main+0x455aa4 fn_455a80+0x24')
        host = dict(verdict='RED', reason='step 1 (enter_game): crash SIGSEGV at dm_custom_ui_pump (in dmhost) (dm_custom.c:193); main_menu: not seen')
        self.assertEqual(explore.signature(host), 'crash SIGSEGV at dm_custom_ui_pump')
        red = dict(verdict='RED', reason='check: no_red', checks=[dict(check='no_red', ok=False, detail='{"seq": 9, "ev": "red", "code": "1a2b3c4d", "msg": "【红】x 12"}')])
        self.assertEqual(explore.signature(red), 'red 1a2b3c4d')
        a = dict(verdict='RED', checks=[dict(check='no_red_log', ok=False, detail='812: [dm] 【红】驻留缺失 #3：贴图 0x1234 分配 4096 B')])
        b = dict(verdict='RED', checks=[dict(check='no_red_log', ok=False, detail='97: [dm] 【红】驻留缺失 #1：贴图 0xabcd 分配 8192 B')])
        self.assertEqual(explore.signature(a), explore.signature(b))


if __name__ == '__main__':
    unittest.main()
