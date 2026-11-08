#!/usr/bin/env python3
"""gate_pick.py: docs stay at quick and unit, host code asks for core, graphics and the apps for full, a scenario by its requires line;
the change is read from a made-up git repository, setup.sh's rewrites left out."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_pick as g


class Pick(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, path, text='x\n'):
        p = os.path.join(self.root, path); os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w', encoding='utf-8').write(text)

    def tiers(self, *files):
        return g.pick(list(files), self.root)[0]

    def test_floor(self):
        self.assertEqual(self.tiers(), ['quick', 'unit'])
        self.assertEqual(self.tiers('README.md', 'docs/MAC.md', 'dm/gfx/README.md', 'dm/switches.tsv', '.github/workflows/tests.yml'), ['quick', 'unit'])
        self.assertEqual(self.tiers('dm/host/tests/t_video.c', 'dm/tools/test_gate_pick.py', 'dm/launcher/Sources/x.swift'), ['quick', 'unit'])

    def test_core(self):
        self.assertEqual(self.tiers('dm/host/dm_ctl.c'), ['quick', 'unit', 'core'])
        self.assertEqual(self.tiers('dm/verify/motion_gate.py'), ['quick', 'unit', 'core'])   # no rule names it
        self.assertEqual(self.tiers('dm/verify/fork_gate.sh'), ['quick', 'unit', 'core'])

    def test_full(self):
        for f in ('dm/gfx/nvn/nvn_mtl.m', 'dm/gfx/tests/nvn_replay.m', 'dm/ios/Sources/x.m', 'dm/host/front_mac.m', 'dm/host/dm_video.c',
                  'dm/tools/patch_nso.py', 'dm/verify/golden.py', 'dm/verify/scenarios/picture/rec.scn', 'dm/i18n/ui_text.tsv'):
            self.assertEqual(self.tiers(f, 'docs/MAC.md'), ['quick', 'unit', 'core', 'full'], f)
        self.assertEqual(g.classify('dm/gfx/tests/t_nvn.c')[0], 'unit')   # the other graphics tests are unit tests

    def test_scenario_by_requires(self):
        self.write('dm/verify/scenarios/a.scn', 'name a\nrequires mac-app\nenter_game\n')
        self.write('dm/verify/scenarios/b.scn', 'name b\nrequires cli mac-app   # both\nenter_game\n')
        self.write('dm/verify/scenarios/c.scn', 'name c\nenter_game\n')
        self.assertEqual(self.tiers('dm/verify/scenarios/a.scn')[-1], 'full')
        self.assertEqual(self.tiers('dm/verify/scenarios/b.scn')[-1], 'core')
        self.assertEqual(self.tiers('dm/verify/scenarios/c.scn')[-1], 'core')
        self.assertEqual(self.tiers('dm/verify/scenarios/gone.scn')[-1], 'core')   # deleted
        self.assertEqual(self.tiers('dm/verify/scenarios/saves/make.scn')[-1], 'core')   # a save generator, not a curated scenario

    def test_rules_name_known_tiers(self):
        for pattern, tier, why in g.RULES:
            self.assertIn(tier, g.ORDER + (g.SCENARIO,), pattern); self.assertTrue(why, pattern)
        self.assertEqual(g.RULES[-1][0], '*')

    def test_changed(self):
        def git(*a):
            subprocess.run(['git', '-C', self.root, *a], check=True, capture_output=True)
        git('init', '-q'); git('config', 'user.email', 't@t'); git('config', 'user.name', 't')
        for f in ('docs/a.md', 'dm/host/b.c', 'dm/gfx/tools/dmsh_chain.lock', 'dm/gfx/nvn/old.m'):
            self.write(f)
        git('add', '-A'); git('commit', '-qm', 'base'); git('tag', 'base')
        self.write('dm/host/b.c', 'y\n'); git('commit', '-qam', 'pushed? no: after the base')   # a commit not on the base
        self.write('docs/a.md', 'y\n')                         # an unstaged edit
        self.write('dm/gfx/tools/dmsh_chain.lock', 'y\n')      # setup.sh's rewrite: never part of a change
        self.write('dm/tools/new.py')                          # a new file
        os.remove(os.path.join(self.root, 'dm/gfx/nvn/old.m'))   # a deletion
        _, files = g.changed('base', self.root)
        self.assertEqual(files, ['dm/gfx/nvn/old.m', 'dm/host/b.c', 'dm/tools/new.py', 'docs/a.md'])
        self.assertEqual(g.pick(files, self.root)[0][-1], 'full')
        _, files = g.changed('no-such-ref', self.root)   # no merge base: the working tree against HEAD
        self.assertEqual(files, ['dm/gfx/nvn/old.m', 'dm/tools/new.py', 'docs/a.md'])


if __name__ == '__main__':
    unittest.main(verbosity=1)
