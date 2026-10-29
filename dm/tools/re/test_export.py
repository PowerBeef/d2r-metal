"""Unit tests for export.py's provenance rules (classify): curated, bulk, seeded and analysis names; no Ghidra, no game files."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export  # noqa: E402  # pyright: ignore[reportMissingImports]

CONF = {0x100: 'medium'}
EVIDENCE = {0x100: 'probed'}
AUTO = {0x200: ('Foo_Bar', 'medium', 'string'), 0x300: ('Baz_Qux', 'high', 'vtable'), 0x350: ('Old_Guess', 'low', 'vtable')}
SEEDED = {(0x200, 'Foo_Bar'): 'autoname', (0x360, 'Gone_Name'): 'autoname', (0x400, 'type_Widget'): 'reflect',
          (0x410, 'g_tables_v1'): 'tables', (0x420, 'pfn_nvnFoo'): 'nvn', (0x430, 'vt_Widget'): 'widget vtable'}


def classify(off, name, src):
    return export.classify(off, name, src, CONF, EVIDENCE, AUTO, SEEDED)


class ClassifyTest(unittest.TestCase):
    def test_curated_name_keeps_its_confidence_and_evidence(self):
        self.assertEqual(classify(0x100, 'Any_Name', 'seed'), ('medium', 'seed', 'probed'))

    def test_bulk_name_keeps_its_own_confidence_and_rule(self):
        self.assertEqual(classify(0x200, 'Foo_Bar', 'seed'), ('medium', 'auto:string', 'derived'))
        self.assertEqual(classify(0x300, 'Baz_Qux', 'seed'), ('high', 'auto:vtable', 'derived'))

    def test_bulk_row_demoted_or_dropped(self):
        self.assertEqual(classify(0x350, 'Old_Guess', 'seed'), ('low', 'auto:vtable', 'derived'))     # demoted to low after seed wrote it
        self.assertEqual(classify(0x360, 'Gone_Name', 'seed'), ('low', 'auto:dropped', 'derived'))   # row gone, name still in the database

    def test_bulk_row_does_not_apply_to_hand_names(self):
        self.assertEqual(classify(0x200, 'Hand_Name', 'manual'), ('high', 'manual', 'static'))
        self.assertEqual(classify(0x200, 'Foo_Bar', 'manual'), ('high', 'manual', 'static'))          # same name set by hand: the analyst vouches

    def test_seeded_names_by_kind(self):
        self.assertEqual(classify(0x400, 'type_Widget', 'seed'), ('high', 'seed', 'derived'))
        self.assertEqual(classify(0x410, 'g_tables_v1', 'seed'), ('high', 'seed', 'derived'))
        self.assertEqual(classify(0x420, 'pfn_nvnFoo', 'seed'), ('high', 'seed', 'static'))
        self.assertEqual(classify(0x430, 'vt_Widget', 'seed'), ('high', 'seed', 'static'))
        self.assertEqual(classify(0x440, '_INIT_3', 'seed'), ('high', 'seed', 'static'))               # the loader's own symbols

    def test_analysis_names(self):
        self.assertEqual(classify(0x500, 'memcpy', 'analysis'), ('analysis', 'analysis', 'static'))


if __name__ == '__main__':
    unittest.main()
