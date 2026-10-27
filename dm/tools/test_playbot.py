#!/usr/bin/env python3
# playbot.py 的单测（不跑游戏）：同种子同计划、每个意图都展开成场景运行器认得的步、生成的场景能解析、不含 msg 步（只走玩家的路）
# Unit test for playbot.py (no game run): same seed, same plan; every intent expands into steps the scenario runner knows; the generated scenario parses; no msg steps (player paths only)
import os
import random
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import playbot   # noqa: E402
import scenario  # noqa: E402


class T(unittest.TestCase):
    def test_plan_is_seeded(self):
        self.assertEqual(playbot.plan(7, 20), playbot.plan(7, 20))
        self.assertNotEqual([n for n, _ in playbot.plan(7, 20)], [n for n, _ in playbot.plan(8, 20)])

    def test_every_intent_parses(self):
        r = random.Random(1)
        intents = [(name, g(r)) for _, name, g in playbot.INTENTS]
        with tempfile.NamedTemporaryFile('w', suffix='.scn', delete=False, encoding='utf-8') as f:
            f.write(playbot.scenario_text('t', 'warlock', intents, 300)); path = f.name
        try:
            meta, steps, checks = scenario.parse(path)
            self.assertEqual(steps[0]['op'], 'enter_game')
            self.assertFalse([s for s in steps if s['op'] in ('msg', 'tap', 'ui_dump', 'crash')], 'player paths only')
            self.assertIn('no_event', [c['op'] for c in checks])
            self.assertGreaterEqual(sum(1 for s in steps if s['op'] == 'expect_state'), len(intents))   # 每个意图后都有心跳 | a heartbeat after every intent
        finally:
            os.unlink(path)

    def test_heartbeat_follows_each_intent(self):
        steps = playbot.expand(playbot.plan(3, 5))
        self.assertEqual(sum(1 for s in steps if s == playbot.HEARTBEAT[-1]), 5)


if __name__ == '__main__':
    unittest.main()
