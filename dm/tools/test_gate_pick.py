#!/usr/bin/env python3
"""gate_pick.py: docs stay at quick and unit, host code asks for core, graphics and the apps for full, a scenario by its requires line;
the change is read from a made-up git repository, setup.sh's rewrites left out."""
import json
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
        self.assertEqual(self.tiers('dm/verify/motion_gate.py'), ['quick', 'unit', 'core'])   # no rule names it
        self.assertEqual(self.tiers('dm/verify/fork_gate.sh'), ['quick', 'unit', 'core'])

    def test_full(self):
        for f in ('dm/gfx/nvn/nvn_mtl.m', 'dm/gfx/tests/nvn_replay.m', 'dm/ios/Sources/x.m', 'dm/host/front_mac.m', 'dm/host/dm_video.c',
                  'dm/host/dm_kbm.c', 'dm/host/dm_ctl.c', 'dm/host/Makefile',
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


class Passed(unittest.TestCase):
    def test_only_a_clean_full_run_of_that_commit(self):
        with tempfile.TemporaryDirectory() as runs:
            def run(name, **kw):
                os.makedirs(os.path.join(runs, name))
                s = dict(sha='abc1234', verdict='OK', uncommitted=0, tiers=['quick', 'unit', 'core', 'full']); s.update(kw)
                with open(os.path.join(runs, name, 'summary.json'), 'w') as f: json.dump(s, f)
            sha = 'abc1234' + '0' * 33
            self.assertIsNone(g.passed(sha, runs))
            run('20261107_100000_red', verdict='RED')
            run('20261107_110000_dirty', uncommitted=3)              # a run of the tree before the commit
            run('20261107_120000_core', tiers=['quick', 'unit', 'core'])
            run('20261107_130000_other', sha='def5678')
            self.assertIsNone(g.passed(sha, runs))
            run('20261107_090000_good')
            self.assertEqual(os.path.basename(g.passed(sha, runs)), '20261107_090000_good')
            run('20261107_140000_good')                                # the newest wins
            self.assertEqual(os.path.basename(g.passed(sha, runs)), '20261107_140000_good')


class Lint(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.dm = os.path.join(self.tmp.name, 'dm')
        for f in ('tools/a.py', 'verify/scenarios/picture/mac_x.scn', 'verify/b.sh'):
            os.makedirs(os.path.dirname(os.path.join(self.dm, f)), exist_ok=True); open(os.path.join(self.dm, f), 'w').close()  # noqa: SIM115
        self.gate = os.path.join(self.dm, 'verify', 'fork_gate.sh')

    def tearDown(self):
        self.tmp.cleanup()

    def lint(self, body):
        with open(self.gate, 'w') as f: f.write(body)
        return g.lint(self.gate, self.dm)

    def test_good(self):
        n, problems = self.lint('step quick_a 60 -- python3 "$DM/tools/a.py" check   # a comment naming "$DM/tools/gone.py"\n'
                                "    step full_b 900 -- bash -c 'scn_run \"$@\"' _ \"$DM/verify/scenarios/picture/\"mac_*.scn\n"
                                '    for r in x y; do step "long_$r" 60 -- bash "$DM/verify/b.sh" "$DM/verify/bench/$r.scn"; done\n')
        self.assertEqual((n, problems), (3, []))

    def test_missing_path_and_glob(self):
        n, problems = self.lint('step quick_a 60 -- python3 "$DM/tools/gone.py"\nstep full_b 60 -- ls "$DM/verify/scenarios/picture/"ios_*.scn\n')
        self.assertEqual(len(problems), 2, problems)
        self.assertIn('$DM/tools/gone.py names nothing', problems[0])

    def test_build_output_may_be_missing(self):   # a clean checkout has no dmhost_tsan until the long tier builds it
        subprocess.run(['git', 'init', '-q', self.tmp.name], check=True)
        with open(os.path.join(self.tmp.name, '.gitignore'), 'w') as f: f.write('dm/host/dmhost_*\n')
        n, problems = self.lint('step long_x 60 -- python3 "$DM/tools/a.py" --dmhost "$DM/host/dmhost_tsan"\n')
        self.assertEqual(problems, [])
        n, problems = self.lint('step long_x 60 -- python3 "$DM/tools/gone.py"\n')
        self.assertEqual(len(problems), 1)

    def test_duplicate_name(self):
        n, problems = self.lint('step quick_a 60 -- true\nstep quick_a 60 -- true\n')
        self.assertEqual(problems, ['line 2: step quick_a already at line 1'])

    def test_the_real_gate(self):
        n, problems = g.lint(os.path.join(g.ROOT, 'dm', 'verify', 'fork_gate.sh'), os.path.join(g.ROOT, 'dm'))
        self.assertGreater(n, 40); self.assertEqual(problems, [])


if __name__ == '__main__':
    unittest.main(verbosity=1)
