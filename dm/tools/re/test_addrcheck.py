"""Unit tests for addrcheck.py: a throwaway git tree cites made-up main+0x... addresses against a synthetic symbol table and export; no game files are read."""
from pathlib import Path
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(Path(__file__).resolve().parent))
import addrcheck  # noqa: E402

SYMBOLS = 'module\tstart\tsize\tname\tnotes\nmain\t0x1000\t0x100\t\t\nmain\t0x1400\t0x40\tfoo\t\nmain\t0x3000\t0x10\t\t\nsdk\t0x1000\t0x100\tbar\t\n'
EXPORT = '# names\noffset\tname\tkind\tconfidence\tsource\n0x1000\tGame_Tick\tfunction\thigh\tseed\n0x2800\tlbl_in_gap\tlabel\tmedium\tauto\n'
SOURCES = {
    'host/a.c': 'x(); // main+0x1010 inside Game_Tick; main+0x1440 is the end of foo (a return address)\n',
    'host/b.c': '// table at main+0x9000 (data)\n// main+0x1002 is a typo\n// main+0x2000 lies in no function\n// main+0x2800 is a known label\n',
    'tools/test_x.py': "FAKE = 'main+0x2004'   # fixture\n",
    'tools/usage.py': '# usage, e.g. main+0x2008\n',
    'verify/c.md': 'see sdk+0x1000 (another module, not checked)\n',
}


class AddrcheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='addrcheck ')
        root = Path(self.tmp.name)
        for f, t in SOURCES.items():
            (root / f).parent.mkdir(parents=True, exist_ok=True); (root / f).write_text(t, encoding='utf-8')
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
        (root / 'sym.tsv').write_text(SYMBOLS, encoding='utf-8'); (root / 'names.tsv').write_text(EXPORT, encoding='utf-8')
        self.root = root

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = addrcheck.main(['--root', str(self.root), '--symbols', str(self.root / 'sym.tsv'), '--export', str(self.root / 'names.tsv'), *extra])
        return rc, out.getvalue()

    def test_citations_skip_fixtures_and_examples(self):
        cites = addrcheck.citations(str(self.root))
        self.assertEqual(sorted(cites), [0x1002, 0x1010, 0x1440, 0x2000, 0x2800, 0x9000])
        self.assertEqual(cites[0x1010], ['host/a.c:1'])

    def test_verdicts(self):
        rc, out = self.run_tool('--list')
        self.assertEqual(rc, 1)
        rows = {r['offset']: r for r in addrcheck.check(addrcheck.citations(str(self.root)), addrcheck.load_functions(str(self.root / 'sym.tsv')), addrcheck.load_export(str(self.root / 'names.tsv')))}
        self.assertEqual((rows[0x1010]['kind'], rows[0x1010]['where']), ('code', 'Game_Tick+0x10'))   # export name wins over the empty table name
        self.assertEqual((rows[0x1440]['kind'], rows[0x1440]['where']), ('code', 'foo+0x40'))        # the function end counts
        self.assertEqual(rows[0x1002]['kind'], 'misaligned')
        self.assertEqual(rows[0x2000]['kind'], 'unknown')
        self.assertEqual((rows[0x2800]['kind'], rows[0x2800]['ok']), ('code', True))                 # a known label in a gap
        self.assertEqual(rows[0x9000]['kind'], 'data')
        result = json.loads(out.strip().splitlines()[-1][len('RESULT '):])
        self.assertEqual(sorted(result['red']), ['main+0x1002', 'main+0x2000'])
        self.assertIn('READDR RED 6 addresses', out)

    def test_unavailable_without_export(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = addrcheck.main(['--root', str(self.root), '--symbols', str(self.root / 'sym.tsv'), '--export', str(self.root / 'missing.tsv')])
        self.assertEqual(rc, 0)
        self.assertTrue(out.getvalue().startswith('⚪ READDR unavailable'))


if __name__ == '__main__':
    unittest.main()
