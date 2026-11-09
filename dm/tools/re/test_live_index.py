"""Unit tests for live_index.py's pure parts: block leaders and call sites from decoded instructions, the rewritten-address tables, the
schema; no Ghidra, no capstone, no game files."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import live_index  # noqa: E402  # pyright: ignore[reportMissingImports]


class ScanTest(unittest.TestCase):
    def test_leaders_and_calls(self):
        insns = [(0x100, 'cmp', 'x0, #5'), (0x104, 'b.eq', '#0x114'), (0x108, 'bl', '#0x4000'), (0x10c, 'blr', 'x8'), (0x110, 'ret', ''),
                 (0x114, 'cbz', 'x1, #0x100'), (0x118, 'b', '#0x9000'), (0x11c, 'br', 'x16')]
        lead, calls = live_index.scan_function(insns, 0x100, 0x120)
        self.assertEqual(lead, [0x100, 0x108, 0x114, 0x118, 0x11c])   # entry, after b.eq, its target, after cbz, after b; after ret = 0x114 already
        self.assertEqual(calls, [(0x108, 0x4000), (0x10c, None), (0x118, 0x9000), (0x11c, None)])   # a call, a computed call, a tail call, a computed jump

    def test_a_call_does_not_end_a_block(self):
        lead, _ = live_index.scan_function([(0, 'bl', '#0x40'), (4, 'mov', 'x0, x1'), (8, 'ret', '')], 0, 12)
        self.assertEqual(lead, [0])


class TablesTest(unittest.TestCase):
    def test_never_hook(self):
        with tempfile.TemporaryDirectory() as d:
            w = lambda n, t: (Path(d) / f'main.{n}.tsv').write_text(t, encoding='utf-8')
            w('gates', '0x3250000\tsvc\t134\t0x2f04\n'); w('cuts', '# BUILD_ID\tx\n0x8524\taa\tbb\tall\twhy\n')
            w('obs', '# BUILD_ID\tx\n0x15f0\t23\tdead\twhy\n'); w('params', '# BUILD_ID\tx\n0xe12c\t0xe130\tx19\tKRES1\twhy\n'); w('sites', '0xb40c\tx18\n0x8524\tx18\n')
            nh = live_index.read_never_hook(d)
            self.assertEqual(sorted(nh), [0x15f0, 0x2f00, 0x2f04, 0x8524, 0xb40c, 0xe12c, 0xe130])
            self.assertEqual(nh[0x8524], 'cut')   # a more specific table wins over the register-site list
            self.assertEqual(nh[0x15f0], 'observation gate 23')

    def test_schema_and_meta(self):
        with tempfile.TemporaryDirectory() as d:
            db = live_index.connect(Path(d) / 'i.sqlite'); live_index.set_meta(db, product='p-r1'); db.commit()
            self.assertEqual(db.execute("select value from meta where key = 'product'").fetchone()[0], 'p-r1')
            db.execute('insert into calls values(?, ?, ?)', (4, 0, None))
            self.assertEqual(db.execute('select count(*) from calls where target is null').fetchone()[0], 1)


if __name__ == '__main__':
    unittest.main()
