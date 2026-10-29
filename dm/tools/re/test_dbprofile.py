"""Unit tests for dbprofile.py: stamps taken when a step runs, OK / stale against now, remedies, unavailable or unreadable profile; synthetic files only."""
from pathlib import Path
import importlib
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(Path(__file__).resolve().parent))

NAMES = '# offset\tname\tconfidence\tsource\n0x10\t{}\thigh\ttest\n'


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dbprofile ')
        root = Path(self.tmp.name)
        nso = root / 'data/switch/3.3.94012/Update/ExeFS/main'
        nso.parent.mkdir(parents=True)
        nso.write_bytes(b'NSO0' + bytes(0x3c) + bytes(range(32)) + bytes(64))
        (root / 're/export').mkdir(parents=True)
        (root / 're/names.tsv').write_text(NAMES.format('Foo_Bar'), encoding='utf-8')
        os.environ['DM_ROOT'] = str(root)
        import dbprofile  # pyright: ignore[reportMissingImports]
        self.p = importlib.reload(dbprofile)
        self.root = root

    def tearDown(self):
        os.environ.pop('DM_ROOT', None)
        self.tmp.cleanup()

    def run_check(self):
        out = io.StringIO()
        with redirect_stdout(out): rc = self.p.check()
        return rc, out.getvalue()

    def seed(self):
        self.p.stamp('seed', (*self.p.SEED_INPUTS, 'seed.py'))

    def test_unavailable_without_profile(self):
        rc, out = self.run_check()
        self.assertEqual(rc, 0)
        self.assertTrue(out.startswith('⚪ PROFILE unavailable'))

    def test_unreadable_profile_is_unavailable(self):
        self.p.PROFILE.write_text('{"inputs": ', encoding='utf-8')
        rc, out = self.run_check()
        self.assertEqual(rc, 0)
        self.assertIn('⚪ PROFILE unavailable', out)
        self.assertTrue(out.splitlines()[-1].startswith('RESULT '))

    def test_ok_then_stale_after_names_change(self):
        self.seed()
        prof = self.p.write({'exported': 'test'})
        self.assertEqual(prof['inputs']['nso_build_id'], bytes(range(32)).hex())
        self.assertIsNone(prof['inputs']['main_elf'])                      # missing files are null, not an empty hash
        self.assertIn('names_tsv', prof['stamps'])
        self.assertIn('main_elf', prof['unstamped'])                        # no import stamp in this workspace
        self.assertEqual(self.run_check()[0], 0)
        (self.root / 're/names.tsv').write_text(NAMES.format('Foo_Baz'), encoding='utf-8')
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn('changed inputs.names_tsv', out)
        self.assertIn('re.sh seed && re.sh export', out)
        self.assertIn('PROFILE STALE 1 change(s)', out)

    def test_export_without_reseed_stays_stale(self):
        self.seed()
        (self.root / 're/names.tsv').write_text(NAMES.format('Foo_Baz'), encoding='utf-8')   # edited after seed ran
        self.p.write({'exported': 'test'})                                                    # export only
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn('changed inputs.names_tsv', out)
        self.seed()
        self.p.write({'exported': 'test'})
        self.assertEqual(self.run_check()[0], 0)

    def test_tool_change_names_its_own_step(self):
        self.seed()
        prof = self.p.write({'exported': 'test'})
        prof['tools']['reflect.py'] = 'an older blob'
        self.p.PROFILE.write_text(json.dumps(prof), encoding='utf-8')
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn('re.sh reflect, then re.sh seed && re.sh export', out)
        self.assertEqual(set(self.p.REMEDY) >= set(self.p.TOOLS) | set(self.p.INPUTS) | {'nso_build_id', 'ghidra', 'analysis'}, True)


if __name__ == '__main__':
    unittest.main()
