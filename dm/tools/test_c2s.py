"""c2s.py 的单测：标签表格式（包号不重复、标签是我们自己的短句、长度规则合法）、按包号汇总与长度核对、某拍附近的包、表外包号。 | Unit tests for c2s.py: the label table's format (no repeated number, labels are our own short phrases, valid length rules), the per-number summary and length check, the packets near a tick, numbers missing from the table."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import c2s  # noqa: E402


def row(tick, pkt, n=None, sys_class=False):
    r = dict(tick=tick, unit=None if sys_class else 1, len=n if n is not None else len(pkt) // 2, pkt=pkt)
    if sys_class: r['q'] = 'sys'
    return r


class C2sTest(unittest.TestCase):
    def test_table_format(self):
        seen = set()
        with open(c2s.TABLE, encoding='utf-8') as f:
            for ln in f:
                if ln.startswith('#') or ln.startswith('class\t') or not ln.strip(): continue
                k, op, lab, rule = ln.rstrip('\n').split('\t')
                self.assertIn(k, ('cmd', 'sys')); self.assertRegex(op, r'^0x[0-9a-f]{2}$')
                self.assertNotIn((k, op), seen); seen.add((k, op))
                self.assertTrue(0 < len(lab) <= 48 and lab.isascii() and not re.search(r'[A-Z][a-z]+[A-Z]', lab), lab)   # 人话，不是函数名 | plain words, not a function name
                self.assertRegex(rule, r'^(\d+|var|\?)$')
        self.assertGreater(len(seen), 50)

    def test_summary_and_length_check(self):
        t = {('cmd', 0x30): ('test action', '5'), ('sys', 0x6d): ('heartbeat', '13')}
        rows = [row(1, '3001020304'), row(2, '30010203040506', 7), row(3, '6d' + '00' * 12, sys_class=True)]
        res = {(r['cls'], r['op']): r for r in c2s.summary(rows, t)}
        self.assertEqual(res[('cmd', 0x30)]['count'], 2); self.assertEqual(res[('cmd', 0x30)]['mismatch'], [7])
        self.assertEqual(res[('sys', 0x6d)]['mismatch'], [])
        rows.append(row(4, '6400'))
        self.assertFalse({(r['cls'], r['op']): r for r in c2s.summary(rows, t)}[('cmd', 0x64)]['known'])

    def test_near(self):
        t = {('cmd', 0x30): ('test action', '5')}
        rows = [row(k, '3001020304') for k in (1, 5, 9, 10, 11)]
        self.assertEqual([ln.split()[0] for ln in c2s.near(rows, 10, 1, t)], ['9', '10'])
        self.assertIn('test action', c2s.near(rows, 10, 0, t)[0])

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'c2s.jsonl'; p.write_text('\n'.join(json.dumps(r) for r in [row(3, '6f' + '00' * 4, sys_class=True)]) + '\n')
            out = subprocess.run([sys.executable, str(HERE / 'c2s.py'), 'summary', d], capture_output=True, text=True, check=False)
            self.assertIn('RESULT', out.stdout.splitlines()[-1])


if __name__ == '__main__':
    unittest.main()
