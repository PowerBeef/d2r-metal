"""tickdiff.py 的单测：相同、第一个不同的拍与哪几格、逐单位差异、之后又相同、新开一局分段、没有共同的拍。 | Unit tests for tickdiff.py: identical, the first divergent tick and its slots, per-unit differences, identical again afterwards, a new game as a new segment, no common ticks."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tickdiff  # noqa: E402

H0 = ['%016x' % i for i in range(6)]


def digest(tick, h=None, n=(1, 2, 0, 0, 3), bad=0):
    return json.dumps(dict(tick=tick, n=list(n), frames=tick * 2, h=h or H0, bad=bad, reads=99, game='0x1', dt=tick / 25))


def unit(tick, t, uid, **kw):
    u = dict(type=t, cls=7, id=uid, mode=1, act=1, seed=0, rlo=5, rhi=6, x=100, y=200, xf=0, yf=0, it=None, st=[[6, 64]])
    u.update(kw); return json.dumps(dict(t=tick, u=u))


class TickdiffTest(unittest.TestCase):
    def write(self, d, name, lines):
        p = Path(d) / name; p.write_text('\n'.join(lines) + '\n', encoding='utf-8'); return str(p)

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(HERE / 'tickdiff.py'), *args], capture_output=True, text=True, check=False)

    def test_identical(self):
        with tempfile.TemporaryDirectory() as d:
            a = self.write(d, 'a.jsonl', [digest(t) for t in range(1, 6)])
            b = self.write(d, 'b.jsonl', [digest(t).replace('"dt": 0.', '"dt": 9.') for t in range(1, 6)])   # 只给人看的字段不比 | fields for people only are not compared
            r = self.run_cli(a, b)
            self.assertEqual(r.returncode, 0, r.stdout); self.assertIn('TICKDIFF OK 5', r.stdout)

    def test_first_divergence_with_units(self):
        hm = list(H0); hm[1] = 'ffffffffffffffff'
        a = [digest(1), unit(1, 1, 10), digest(2), unit(2, 1, 10), digest(3), unit(3, 1, 10), digest(4), unit(4, 1, 10)]
        b = [digest(1), unit(1, 1, 10), digest(2), unit(2, 1, 10), digest(3, hm, n=(1, 2, 0, 1, 3)), unit(3, 1, 10, x=101, st=[[6, 60]]), unit(3, 3, 77),
             digest(4), unit(4, 1, 10)]
        with tempfile.TemporaryDirectory() as d:
            r = tickdiff.compare(tickdiff.load(self.write(d, 'a.jsonl', a)), tickdiff.load(self.write(d, 'b.jsonl', b)))
        f = r['first']
        self.assertFalse(r['same']); self.assertEqual((f['tick'], f['slots'], f['n']), (3, ['monster'], {'missile': [0, 1]}))
        self.assertEqual((r['diverged'], r['reconverged']), (1, 4))
        self.assertIn('  monster id 10 cls 7: x 100→101, stat 6 64→60', f['units'])
        self.assertIn('+ missile id 77 cls 7 (only in B)', f['units'])

    def test_segments_and_cli_red(self):
        a = [digest(1), digest(2), digest(1), digest(2)]   # 第二局拍号从 1 重来 | the second game restarts at tick 1
        b = [digest(1), digest(2), digest(1), digest(2, bad=3)]
        with tempfile.TemporaryDirectory() as d:
            pa, pb = self.write(d, 'a.jsonl', a), self.write(d, 'b.jsonl', b)
            r = tickdiff.compare(tickdiff.load(pa), tickdiff.load(pb))
            self.assertEqual((r['first']['segment'], r['first']['tick'], r['first']['bad']), (1, 2, [0, 3]))
            out = self.run_cli(pa, pb)
            self.assertEqual(out.returncode, 1); self.assertIn('first divergent tick 2', out.stdout); self.assertIn('DM_TICKS_FULL=1', out.stdout)
            self.assertTrue(out.stdout.splitlines()[-1].startswith('RESULT '))

    def test_no_common_ticks(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.run_cli(self.write(d, 'a.jsonl', [digest(1)]), self.write(d, 'b.jsonl', [digest(1).replace('"tick": 1', '"tick": 9')]), '--to', '5')
        self.assertEqual(out.returncode, 2); self.assertIn('no common ticks', out.stdout)


if __name__ == '__main__':
    unittest.main()
