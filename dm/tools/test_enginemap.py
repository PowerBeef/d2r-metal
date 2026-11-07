#!/usr/bin/env python3
"""enginemap.py on a small made-up tree: a complete map passes; a removed row, a new cut with no row, a row citing what no longer exists and a settled assumption all go red."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enginemap

NSO = """
CUTS = {'b': [(0x1000, 'aa', 'bb', 'why')]}
OBS = {'b': [(0x2000, 1, 'cc', 'why')]}
PARAMS = {'b': [(0x3000, 'a', 0x3004, 'b', 8, 'KONE', 'why')]}
SWITCHES = {'b': [(0x4000, 'dd', 'KONE', 'why')]}
KEYS = {'KONE': 500}
"""
RULES = "\nRULES = [(one_rule, lambda name, key: True)]\n"
ROW = """### EC-001 A row
- **Layer:** binary rule
- **Engine:** does a thing
- **Ours:** does another
- **Why:** a reason
- **Setting:** none
- **Switch:** DM_ONE
- **Test:** quick_one, t_one
- **Commit:** {sha}
- **Measured:** none: nothing to measure
- **Status:** {status}
- **Covers:** {covers}
"""
ALL = 'main+0x1000, main+0x2000, main+0x3000, main+0x4000, KONE, one_rule, 0123456789abcdef, renderer+0x664c'


class Map(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = self.tmp.name
        for path, text in (('dm/tools/patch_nso.py', NSO), ('dm/gfx/tools/dmsh_rules.py', RULES), ('dm/gfx/nvn/nvn_mtl.m', 'x = "0123456789abcdef";\n'),
                           ('dm/host/dm_video.c', 'rcfg_put(cfg, 0x664c, 4, 9);\n'), ('dm/verify/fork_gate.sh', 'step quick_one 120 -- true\n'),
                           ('dm/switches.tsv', 'DM_ONE\tgfx\tx\ty\n'), ('dm/host/tests/t_one.c', 'int main(void) { return 0; }\n')):
            full = os.path.join(self.root, path); os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, 'w', encoding='utf-8') as f: f.write(text)
        for args in (['init', '-q'], ['-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '--allow-empty', '-m', 'x']):
            subprocess.run(['git', '-C', self.root, *args], check=True)
        self.sha = subprocess.run(['git', '-C', self.root, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        self.doc = os.path.join(self.root, 'doc.md')

    def tearDown(self):
        self.tmp.cleanup()

    def red(self, row=None, ledger=None, before=''):
        text = '# Map\n\n' + before + '## Register\n\n' + (row if row is not None else ROW.format(sha=self.sha, status='confirmed', covers=ALL))
        with open(self.doc, 'w', encoding='utf-8') as f: f.write(text)
        return enginemap.check(self.root, self.doc, ledger)[3]

    def test_complete(self):
        self.assertEqual(self.red(), [])

    def test_removed_row(self):
        self.assertTrue(any('main+0x1000' in r and 'no row' in r for r in self.red(ROW.format(sha=self.sha, status='confirmed', covers=ALL.replace('main+0x1000, ', '')))))
        self.assertTrue(any('no rows' in r for r in self.red('')))

    def test_new_cut_without_a_row(self):
        path = os.path.join(self.root, 'dm/tools/patch_nso.py')
        with open(path, encoding='utf-8') as f: text = f.read()
        with open(path, 'w', encoding='utf-8') as f: f.write(text.replace("[(0x1000, 'aa', 'bb', 'why')]", "[(0x1000, 'aa', 'bb', 'why'), (0x5000, 'ee', 'ff', 'why')]"))
        self.assertTrue(any('main+0x5000' in r for r in self.red()))

    def test_new_shader_key_without_a_row(self):
        with open(os.path.join(self.root, 'dm/gfx/nvn/nvn_mv.inc'), 'w', encoding='utf-8') as f: f.write('x = "fedcba9876543210";\n')
        self.assertTrue(any('fedcba9876543210' in r for r in self.red()))

    def test_row_cites_what_is_gone(self):
        row = ROW.format(sha=self.sha, status='confirmed', covers=ALL + ', main+0x9000')
        self.assertTrue(any('main+0x9000' in r for r in self.red(row)))
        row = ROW.format(sha='deadbeef', status='confirmed', covers=ALL)
        self.assertTrue(any('deadbeef' in r for r in self.red(row)))
        row = ROW.format(sha=self.sha, status='confirmed', covers=ALL).replace('DM_ONE', 'DM_GONE')
        self.assertTrue(any('DM_GONE' in r for r in self.red(row)))
        row = ROW.format(sha=self.sha, status='confirmed', covers=ALL).replace('t_one', 't_gone')
        self.assertTrue(any('t_gone' in r for r in self.red(row)))
        self.assertTrue(any('EC-009' in r for r in self.red(before='See EC-009.\n\n')))

    def test_assumptions(self):
        ledger = os.path.join(self.root, 'ledger.tsv')
        with open(ledger, 'w', encoding='utf-8') as f: f.write('u-a-1\tx\topen\tq\nu-a-2\tx\tresolved\tq\n')
        self.assertEqual(self.red(ROW.format(sha=self.sha, status='assumes:u-a-1', covers=ALL), ledger), [])
        self.assertTrue(any('resolved' in r for r in self.red(ROW.format(sha=self.sha, status='assumes:u-a-2', covers=ALL), ledger)))
        self.assertTrue(any('missing' in r for r in self.red(ROW.format(sha=self.sha, status='assumes:u-a-3', covers=ALL), ledger)))
        self.assertEqual(self.red(ROW.format(sha=self.sha, status='assumes:u-a-2', covers=ALL)), [])   # no ledger at hand: the id's form only
        self.assertTrue(any('unknown id' in r for r in self.red(ROW.format(sha=self.sha, status='assumes:nonsense', covers=ALL))))

    def test_format(self):
        row = ROW.format(sha=self.sha, status='confirmed', covers=ALL).replace('- **Why:** a reason\n', '')
        self.assertTrue(any('no Why' in r for r in self.red(row)))
        row = ROW.format(sha=self.sha, status='confirmed', covers=ALL).replace('binary rule', 'magic')
        self.assertTrue(any('layer' in r for r in self.red(row)))
        two = ROW.format(sha=self.sha, status='confirmed', covers=ALL) + ROW.format(sha=self.sha, status='confirmed', covers='none')
        self.assertTrue(any('twice' in r for r in self.red(two)))


class RealMap(unittest.TestCase):
    def test_repo_map_is_current(self):
        root = enginemap.ROOT
        self.assertEqual(enginemap.check(root, os.path.join(root, 'docs/ENGINE_CHANGES.md'), None)[3], [])


if __name__ == '__main__':
    unittest.main()
