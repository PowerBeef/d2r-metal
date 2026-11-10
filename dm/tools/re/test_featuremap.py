"""Unit tests for the feature map (featuremap.py) on synthetic data: the plan parser, the tag of a function from the actions it ran in, and the
per-action sets from coverage files (an action's blocks less the idle windows', by function, with the functions that never run while idle);
no game, no snapshot."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import featuremap  # noqa: E402
import live  # noqa: E402


class FeatureMapTest(unittest.TestCase):
    def test_plan(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'x.plan')
            Path(p).write_text('# a plan\nsave hero\nenv A=1 B=2\nsetup\n  enter_game\n  sleep 2\naction ui/inventory\n  open inventory\n  # a comment\n  back\naction idle_mid\n  sleep 4\n', encoding='utf-8')
            self.assertEqual(featuremap.parse_plan(p), ('hero', 'A=1 B=2', ['enter_game', 'sleep 2'], [('ui/inventory', ['open inventory', 'back']), ('idle_mid', ['sleep 4'])]))
            Path(p).write_text('save x\nstick 0 1\n', encoding='utf-8')
            with self.assertRaises(SystemExit): featuremap.parse_plan(p)   # a step before setup or an action
            Path(p).write_text('setup\n  enter_game\n', encoding='utf-8')
            with self.assertRaises(SystemExit): featuremap.parse_plan(p)   # no action

    def test_tags(self):
        self.assertEqual(featuremap.tag_of(['ui/inventory']), 'ui/inventory')
        self.assertEqual(featuremap.tag_of(['ui/inventory', 'ui/pause']), 'ui/* (2)')
        self.assertEqual(featuremap.tag_of(['ui/inventory', 'move/walk']), 'ui/inventory+move/walk')
        self.assertEqual(featuremap.tag_of(['walk', 'run']), 'walk+run')
        self.assertEqual(featuremap.tag_of(['a/1', 'b/2', 'c/3', 'a/4']), 'shared: a x2, b x1, c x1')

    def test_features(self):
        """Functions at 0x1000 (runs while idle, one more block in the walk), 0x2000 (inventory only), 0x3000 (both panels)."""
        fns = {0x1000: 0x100, 0x2000: 0x100, 0x3000: 0x100}
        old = live.by_function, featuremap.guestsyms.lookup
        def by_function(offs):
            out = {}
            for o in offs:
                for f, n in fns.items():
                    if f <= o < f + n: out[f] = out.get(f, 0) + 1
            return out
        live.by_function = by_function; featuremap.guestsyms.lookup = lambda w: {'main+0x2000': 'Inv_Open+0x0  [note]'}.get(w, '')
        try:
            with tempfile.TemporaryDirectory() as d:
                def cov(name, offs): Path(d, name).write_text(''.join(f'{o:x}\t1\n' for o in offs), encoding='utf-8')
                cov('cov_00_idle_start.tsv', [0x1000, 0x1010]); cov('cov_01_ui~inventory.tsv', [0x1000, 0x2000, 0x2010, 0x3000])
                cov('cov_02_ui~pause.tsv', [0x1010, 0x3000, 0x3020]); cov('cov_03_move~walk.tsv', [0x1000, 0x1020]); cov('cov_04_idle_end.tsv', [0x1000])
                order, base, rows = featuremap.write_features(d)
                self.assertEqual((order, len(base)), (['ui/inventory', 'ui/pause', 'move/walk'], 2))
                got = {r[0]: (r[1], r[2], r[3], r[4]) for r in rows}
                self.assertEqual(got[0x2000], ('Inv_Open', 'ui/inventory', 1, [2, 0, 0]))
                self.assertEqual(got[0x3000], ('', 'ui/* (2)', 1, [1, 2, 0]))
                self.assertEqual(got[0x1000], ('', 'move/walk', 0, [0, 0, 1]))   # a branch of code that runs anyway
                text = Path(d, 'features.tsv').read_text(encoding='utf-8').split('\n')
                self.assertEqual(text[0], 'func\tname\ttag\tnew\tui/inventory\tui/pause\tmove/walk')
                old_merged = featuremap.MERGED; featuremap.MERGED = os.path.join(d, 'merged.tsv')
                try:
                    featuremap.merge(d); featuremap.merge(d)   # the second merge replaces the run's rows
                    self.assertEqual(len(Path(featuremap.MERGED).read_text(encoding='utf-8').strip().split('\n')), 4)
                    self.assertEqual(featuremap.lookup(0x2000)[0][:3], ('ui/inventory', True, 'ui/inventory:2'))
                finally: featuremap.MERGED = old_merged
        finally: live.by_function, featuremap.guestsyms.lookup = old


if __name__ == '__main__':
    unittest.main()
