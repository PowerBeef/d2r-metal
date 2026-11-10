"""Unit tests for the live harness's report side (live.py, ingest.py) on synthetic data: record decoding, typed arguments and captures from a
hand-built snapshot, computed call sites resolved from recorded callers, the annotated view's line states, the write-back's comment merge and
the ledger update; no game, no Ghidra."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ingest  # noqa: E402
import live  # noqa: E402
import live_index  # noqa: E402

BASE = 0x100000000


def rec(hook, tid, t, lr, x=(0, 0, 0, 0), cap=(0, 0, 0, 0), aux=0):
    return live.REC.pack(hook, tid, t, lr, 0, *x, *cap, aux)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.d = self.tmp.name
        self.old_live = live.LIVE, live_index.LIVE
        live.LIVE = self.d; live_index.LIVE = Path(self.d)
        db = live_index.connect(Path(self.d) / 'index.sqlite')
        db.execute("insert into functions values(0x1000, 0x40, 'Thing_Update', 'void', 'ANALYSIS')")
        db.execute("insert into functions values(0x2000, 0x20, 'Caller', 'void', 'ANALYSIS')")
        db.execute("insert into params values(0x1000, 0, 'param_1', 'long', 'x0:8')")
        db.execute("insert into structs values('Thing', '/dm', 16)")
        db.execute("insert into fields values('Thing', 4, 'mode', 'dword', 4)")
        db.executemany('insert into blocks values(?, ?)', [(0x1000, 0x1000), (0x1010, 0x1000), (0x1020, 0x1000)])
        db.execute('insert into calls values(0x2008, 0x2000, null)'); db.execute('insert into calls values(0x2010, 0x2000, 0x1000)')
        db.commit(); db.close()
        self.run = os.path.join(self.d, 'runs', 'r1'); os.makedirs(self.run)
        with open(os.path.join(self.run, 'live.bin'), 'wb') as f:
            f.write(live.HDR.pack(b'DMLIVE01', BASE, 0x10000, 1000, live.REC.size, 1))
            for i in range(4): f.write(rec(0, 7, 1000 * i, BASE + 0x200c, x=(0x500 + (i & 1), 0, 0, 0), cap=(3, 0, 0, 0)))   # called from after the computed site
            f.write(rec(0, 7, 5000, BASE + 0x2014, x=(0x500, 0, 0, 0), cap=(9, 0, 0, 0)))                                          # and once from the direct call
            f.write(rec(0 | live.RET, 7, 5010, BASE + 0x2014, x=(1, 0, 0, 0), aux=10))
            f.write(rec(live.WATCH | 1, 8, 6000, 0, cap=(0x9000, BASE + 0x1014, 0x2a, 0)))
        with open(os.path.join(self.run, 'live.bin.hooks.tsv'), 'w') as f:
            f.write('id\tname\tat\tkind\tspec\n0\tupd\tmain+0x1000\tret\tat=main+0x1000 kind=ret cap=[x0+0x4].w type=x0:Thing\n')

    def tearDown(self):
        live.LIVE, live_index.LIVE = self.old_live; self.tmp.cleanup()


class ReportTest(Fixture):
    def test_summary_types_and_watch(self):
        s = live.summary(os.path.join(self.run, 'live.bin'), quiet=True); h = s['hooks'][0]
        self.assertEqual((h['records'], h['returns'], h['function'], h['entry']), (5, 1, 'Thing_Update', True))
        self.assertEqual(h['args'][0]['type'], 'Thing *')                      # the spec's type hint wins over the map's untyped prototype
        self.assertEqual(h['args'][0]['values'][0], {'v': '0x500', 'n': 3})
        self.assertEqual(h['cap_names'], ['Thing.mode'])                       # [x0+0x4] through a Thing pointer
        self.assertEqual(h['caps'][0][0], {'v': '0x3', 'n': 4})
        self.assertEqual(h['dur_us']['max'], 10000.0)                          # 10 ticks at 1000 Hz
        w = s['watches'][0]
        self.assertEqual((w['watch'], w['addr'], w['writes'], w['values'][0]['v']), (1, '0x9000', 1, '0x2a'))
        self.assertTrue(w['writers'][0]['at'].startswith('main+0x1014'))

    def test_edges_only_computed_sites(self):
        rows = live.edges(self.run, quiet=True)
        self.assertEqual(rows, [(0x2008, 0x1000, 4)])                           # the direct call at 0x2010 is not an observed edge
        self.assertIn('main+0x2008\tmain+0x1000\t4', Path(self.run, 'edges.tsv').read_text())

    def test_view_rows(self):
        decomp = {'lines': [{'n': 1, 'text': 'void f(void)', 'addrs': [0x1000]}, {'n': 2, 'text': '{', 'addrs': []},
                            {'n': 3, 'text': '  a();', 'addrs': [0x1014]}, {'n': 4, 'text': '  b();', 'addrs': [0x1024, 0x1000]}]}
        rows = live.view_rows(decomp, [0x1000, 0x1010, 0x1020], {0x1000, 0x1020}, {0x1000: 5}, {0x1000: 'x0: 1'})
        self.assertEqual([r[1] for r in rows], ['ran', 'none', 'not', 'ran'])   # 0x1014 is in the block at 0x1010, which did not run
        self.assertEqual([(r[2], r[3]) for r in rows], [(5, 'x0: 1'), (0, ''), (0, ''), (0, '')])   # a hook's numbers once, on the first line
        self.assertEqual([r[1] for r in live.view_rows(decomp, [0x1000], None, {}, {})], ['none'] * 4)   # no coverage in the run

    def test_resolve_run(self):
        os.makedirs(os.path.join(self.d, 'runs', 'r2')); old = live.RUNS; live.RUNS = os.path.join(self.d, 'runs')
        try:
            self.assertTrue(live.resolve_run().endswith('r2')); self.assertTrue(live.resolve_run('r1').endswith('r1'))
            with self.assertRaises(SystemExit): live.resolve_run('nope')
        finally: live.RUNS = old


class IngestTest(Fixture):
    def test_comment_and_merge(self):
        h = live.summary(os.path.join(self.run, 'live.bin'), quiet=True)['hooks'][0]
        lines = ingest.comment_lines('r1', h)
        self.assertTrue(all(x.startswith('[live: r1] ') for x in lines))
        self.assertTrue(any('Thing.mode: 0x3×4' in x for x in lines)); self.assertTrue(any('callers: main+0x200c ×4' in x for x in lines))
        merged = ingest.merge_comment('[dm] a hand note\n[live: r1] old\n[live: r0] kept', 'r1', ['[live: r1] new'])
        self.assertEqual(merged, '[dm] a hand note\n[live: r0] kept\n[live: r1] new')

    def test_ledger_update(self):
        p = os.path.join(self.d, 'unknowns.tsv')
        with open(p, 'w', encoding='utf-8') as f: f.write('id\tarea\tstatus\tquestion\traised_in\tsettle_by\tblocks\tresolution\nu-x-01\tx\topen\tq?\tn.md\tprobe\t[idea] y\t\n')
        row = ingest.ledger_update(p, 'u-x-01', 'resolved', 'seen 4 times', 'r1', 'p-r1')
        self.assertEqual(row.split('\t')[2], 'resolved'); self.assertTrue(row.endswith('seen 4 times [probed: r1, p-r1]'))
        with self.assertRaises(SystemExit): ingest.ledger_update(p, 'u-x-99', 'resolved', 't', 'r1', 'p')


if __name__ == '__main__':
    unittest.main()
