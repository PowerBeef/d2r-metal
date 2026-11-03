"""Unit tests for pso_seed.py: the byte-level key normaliser against hand-made rows and against the C one (one hash over t_pso_skey's random keys),
the row check (pso_skey_check's port), normalise and diff on synthetic manifests; no game files, nothing is run."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pso_seed  # noqa: E402

PSO_NORM_FNV = 0xe615b40ab55121b6   # the same constant as dm/gfx/tests/t_pso_skey.m
M64 = (1 << 64) - 1


def blend(target, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a):
    """blend_st as its 8 bytes (bit fields 3, 7, 7, 7, 7, 3, 3 from the low bit)."""
    v = target | src_rgb << 3 | dst_rgb << 10 | src_a << 17 | dst_a << 24 | eq_rgb << 31 | eq_a << 34
    return struct.pack('<Q', v)


def pat(i):
    return blend(i, 5, 6, 2, 1, 1, 1)


def row(cfmt, blend_en, chmask, blends, lmap, merged=0, vs=b'aaaaaaaaaaaaaaaa_b', fs=b''):
    b = bytearray(pso_seed.SKEY_LEN)
    b[0:len(vs)] = vs; b[24:24 + len(fs)] = fs; b[50] = merged
    struct.pack_into('<8I', b, 104, *cfmt); struct.pack_into('<II', b, 148, blend_en, chmask)
    b[160:224] = b''.join(blends); b[416:424] = bytes(lmap)
    return bytes(b)


def n1():
    return row([70, 70, 0, 0, 0, 0, 0, 0], 0x5, 0xffffffff, [pat(i) for i in range(8)], [3] * 8)


def n2():
    return row([70, 70, 70, 0, 0, 0, 0, 0], 0x7, 0x87654321, [pat(i) for i in range(8)], [0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff], merged=1)


class Xorshift:
    def __init__(self, x=0x9e3779b97f4a7c15): self.x = x

    def next(self):
        x = self.x; x ^= (x << 13) & M64; x ^= x >> 7; x ^= (x << 17) & M64; self.x = x
        return x


def gen(g):
    """One random row exactly as t_pso_skey.m gen() builds it."""
    b = bytearray(pso_seed.SKEY_LEN); b[0:18] = b'aaaaaaaaaaaaaaaa_b'
    r = g.next(); merged = r & 1; b[50] = merged
    for i in range(8): struct.pack_into('<I', b, 104 + 4 * i, 70 + i if (r >> (8 + i)) & 1 else 0)
    struct.pack_into('<I', b, 148, g.next() & 0xffffffff); struct.pack_into('<I', b, 152, g.next() & 0xffffffff)
    for i in range(8): struct.pack_into('<Q', b, 160 + 8 * i, g.next())
    r = g.next()
    for i in range(8):
        v = (r >> (8 * i)) & 0xff
        b[416 + i] = (0xff if v % 9 == 8 else v % 9) if merged else v
    return bytes(b)


class NormTest(unittest.TestCase):
    def test_n1_normal_pass(self):
        out = pso_seed.norm_row(n1())
        want = row([70, 70, 0, 0, 0, 0, 0, 0], 0x1, 0xff, [pat(0)] + [bytes(8)] * 7, [0] * 8)
        self.assertEqual(out, want)
        self.assertEqual(pso_seed.norm_row(out), out)

    def test_n2_merged_pass(self):
        out = pso_seed.norm_row(n2())
        want = row([70, 70, 70, 0, 0, 0, 0, 0], 0x3, 0x21, [pat(0), pat(1)] + [bytes(8)] * 6, [0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff], merged=1)
        self.assertEqual(out, want)
        self.assertEqual(pso_seed.norm_row(out), out)

    def test_hash_shared_with_c(self):
        g, h = Xorshift(), 0xcbf29ce484222325
        for _ in range(5000):
            n = pso_seed.norm_row(gen(g))
            self.assertEqual(pso_seed.norm_row(n), n)
            for c in n: h = ((h ^ c) * 0x100000001b3) & M64
        self.assertEqual(h, PSO_NORM_FNV)


class CheckTest(unittest.TestCase):
    def test_check_row(self):
        for raw, merged in ((n1(), 0), (n2(), 1)):
            why, h = pso_seed.check_row(raw.hex())
            self.assertIsNone(why); self.assertEqual(h, pso_seed.norm_row(raw).hex())
        b = bytearray(n1()); b[86] = b[87] = 0xff
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'pad')
        b = bytearray(n2()); b[51] = 1; b[52] = 2
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'kind')
        b = bytearray(n2()); b[416] = 9
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'lmap')
        b = bytearray(n1()); b[416] = 9   # an unmerged row's lmap is cleared, not refused
        self.assertIsNone(pso_seed.check_row(b.hex())[0])
        b = bytearray(n1()); b[0] = 0
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'key')
        self.assertEqual(pso_seed.check_row('00')[0], 'parse')

    def test_normalise(self):
        bad = bytearray(n1()); bad[86] = 0xff
        lines = ['# old comment', f'R\t{n1().hex()}', 'S\tssss_b\t0', f'R\t{bad.hex()}', 'C\t', pso_seed.BG_MARK, f'R\t{pso_seed.norm_row(n1()).hex()}', 'C\tcccc_b']
        out, dropped, kept = pso_seed.normalise(lines)
        self.assertEqual(out, [f'R\t{pso_seed.norm_row(n1()).hex()}', 'S\tssss_b\t0', pso_seed.BG_MARK, 'C\tcccc_b'])
        self.assertEqual([(t, w) for t, w, _ in dropped], [('R', 'pad'), ('C', 'empty'), ('R', 'duplicate')])
        self.assertEqual(kept, {'S': 1, 'R': 1, 'C': 1})


class DiffTest(unittest.TestCase):
    def test_diff(self):
        with tempfile.TemporaryDirectory() as d:
            raw = n1(); other = row([80, 0, 0, 0, 0, 0, 0, 0], 0, 0xf, [bytes(8)] * 8, [0] * 8)
            seed = Path(d) / 'seed.tsv'; seed.write_text(f'# c\nS\tssss_b\t0\nR\t{pso_seed.norm_row(raw).hex()}\n', encoding='utf-8')
            m = Path(d) / 'm.tsv'
            m.write_text(f'S\tssss_b\t0\nS\ttttt_b\t1\nR\t{raw.hex()}\nR\t{other.hex()}\nR\t{other.hex()}\nC\tcccc_b\n', encoding='utf-8')
            p = subprocess.run([sys.executable, str(HERE / 'pso_seed.py'), 'diff', str(m), '--seed', str(seed)], capture_output=True, text=True, check=False)
            self.assertEqual(p.returncode, 0, p.stderr)
            lines = p.stdout.splitlines()
            self.assertIn('DIFF S 1 R 1 C 1', lines)   # the raw n1 row equals the seed's normalised one; the repeated row counts once
            res = json.loads(lines[-1][len('RESULT '):])
            self.assertEqual([(r['tag'], r['vs']) for r in res['rows']], [('S', 'tttt_b'), ('R', 'aaaaaaaaaaaaaaaa_b'), ('C', 'cccc_b')])


if __name__ == '__main__':
    os.environ.setdefault('PYTHONDONTWRITEBYTECODE', '1')
    unittest.main()
