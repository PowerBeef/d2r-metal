"""passgpu_top.py: a DM_NVN_PASSGPU record's span column is an exclusive cost only when the record says it was made serialised (barrier level 0)
and its passes really did not overlap. Small hand-written records with closed-form expectations; no game or GPU needed; temp files go only under run/gfx."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # workspace root (game files, build outputs and run artifacts are all under it)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import passgpu_top   # noqa: E402

HEAD = 'frame\tidx\tkind\tgpu_ms\tinc_ms\tw\th\tfmt0\tdfmt\tload\tdraws\tt0_ns\tt1_ns\tpass\n'


class PassgpuTop(unittest.TestCase):
    def run_top(self, first, span, inc, clear=None):
        """Writes a record of frames 301 and 302 (one 1600x900 render pass each, plus a clear-only pass (span, inc) when clear is given) and returns
        (stdout, RESULT dict)"""
        root = Path(DM_ROOT) / 'run/gfx'
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='passgpu_test_', dir=root) as tmp:
            rows = ''.join(f'{f}\t0\tR\t{span}\t{inc}\t1600\t900\t70\t0\tC\t10\t{f * 1000}\t{f * 1000 + 400}\t{f - 300}\n' for f in (301, 302))
            if clear: rows += ''.join(f'{f}\t1\tR\t{clear[0]}\t{clear[1]}\t1600\t900\t53\t0\tC\t0\t{f * 1000 + 100}\t{f * 1000 + 500}\t{f - 300}\n' for f in (301, 302))
            (Path(tmp) / 'passgpu.tsv').write_text((first + '\n' if first else '') + HEAD + rows)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(passgpu_top.main([tmp]), 0)
        text = out.getvalue()
        res = [l for l in text.splitlines() if l.startswith('RESULT ')]
        self.assertEqual(len(res), 1, text)
        return text, json.loads(res[0][len('RESULT '):])

    def test_a_serial_level0_no_overlap_is_exclusive(self):
        text, r = self.run_top('# barrier_level=0 serial=1', 0.4, 0.4)
        self.assertIs(r['exclusive'], True, text)
        self.assertEqual(r['barrier_level'], 0)
        self.assertEqual(r['overlap'], 0.0)
        self.assertTrue(any('excl_ms' in l and l.startswith('rank') for l in text.splitlines()), text)
        self.assertNotIn('not an exclusive cost', text)

    def test_b_level1_overlapping_is_not_exclusive(self):
        text, r = self.run_top('# barrier_level=1 serial=1', 0.5, 0.4)
        self.assertIs(r['exclusive'], False, text)
        self.assertEqual(r['barrier_level'], 1)
        self.assertEqual(r['overlap'], 0.2)
        self.assertIn('not an exclusive cost', text)
        self.assertFalse(any('excl_ms' in l for l in text.splitlines()), text)

    def test_c_old_record_without_header_is_never_exclusive(self):
        text, r = self.run_top(None, 0.5, 0.4)
        self.assertIs(r['exclusive'], False, text)
        self.assertIsNone(r['barrier_level'])
        self.assertEqual(r['groups'], 1)
        self.assertEqual(r['mean_ms'], 0.4)
        self.assertIn('barrier level unknown', text)

    def test_d_serial_header_but_overlap_is_refused(self):
        text, r = self.run_top('# barrier_level=0 serial=1', 0.5, 0.4)
        self.assertIs(r['exclusive'], False, text)
        self.assertEqual(r['barrier_level'], 0)
        self.assertIn('record says serial but passes overlap 20 %: not an exclusive cost', text)

    def test_e_clear_only_passes_are_left_out_of_the_overlap(self):
        # A pass with no draws writes its start stamp (after a vertex stage it doesn't have) before its start barrier waits, so its span overlaps the
        # previous pass even when passes run one after another (seen on a serialised town_loop record: 6 % "overlap", all from such passes)
        text, r = self.run_top('# barrier_level=0 serial=1', 0.4, 0.4, clear=(0.33, 0.0002))
        self.assertIs(r['exclusive'], True, text)
        self.assertEqual(r['overlap'], 0.0)
        # ... and their span is not shown as an exclusive cost: '-' in the excl_ms column, the drawing pass keeps its own
        rows = [l.split() for l in text.splitlines() if l[:4].strip().isdigit()]
        self.assertEqual(len(rows), 2, text)
        by_draws = {row[5]: row for row in rows}   # rank, mean_ms, share, present, excl_ms, draws, ...
        self.assertEqual(by_draws['0'][4], '-', text)
        self.assertEqual(by_draws['10'][4], '0.400', text)
        self.assertNotIn('0.330', text)
        self.assertIn("clear-only passes (no draws): excl_ms '-'", text)

    def test_f_clear_only_span_shown_when_not_exclusive(self):
        # Outside an exclusive record the column is span_ms, which never claims an exclusive cost: clear-only rows keep their span there
        text, r = self.run_top('# barrier_level=1 serial=0', 0.5, 0.4, clear=(0.33, 0.0002))
        self.assertIs(r['exclusive'], False, text)
        self.assertIn('0.330', text)
        self.assertNotIn("excl_ms '-'", text)


if __name__ == '__main__':
    unittest.main()
