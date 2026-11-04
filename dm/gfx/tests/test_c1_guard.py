"""c1_guard.py: integer conversions that can wrap prove nothing, so an index built from them is not "proven bounded" and its shader goes on the c1 list.
Hand-written MSL fragments with hand-worked expectations; no shader library needed. SPAN 512 = 256-byte data rounded up plus the 256-byte gap (indices 0..31 fit)."""
import hashlib
import os
import sys
import unittest
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools')
sys.path.insert(0, TOOLS)
import c1_guard as G   # noqa: E402

SPAN = 512


def ok(src, index):
    return G.proven(index, G.definitions_of(src), SPAN)


class Conversions(unittest.TestCase):
    def test_int_of_unbounded_uint_is_unbounded(self):
        # a uint known only as >= 0 can be >= 2^31: int() wraps it negative, and % keeps the sign
        self.assertFalse(ok('uint _3 = as_type<uint>(in.v); int _4 = int(_3);', '_4 % 16'))
        self.assertFalse(ok('uint _3 = as_type<uint>(in.v); int _4 = int(_3);', 'min(_4, 15)'))

    def test_implicit_signed_declaration_of_unbounded_uint(self):
        self.assertFalse(ok('uint _3 = as_type<uint>(in.v); int _4 = _3;', '_4 % 16'))

    def test_bounded_values_still_prove(self):
        self.assertTrue(ok('uint _3 = as_type<uint>(in.v); uint _4 = _3 % 16u;', '_4'))
        self.assertTrue(ok('uint _3 = as_type<uint>(in.v); int _4 = int(_3 & 15u);', '_4'))
        self.assertTrue(ok('int _4 = int(_3) & 15;', '_4'))

    def test_short_wraps_above_its_range(self):
        self.assertFalse(ok('uint _3 = as_type<uint>(in.v) % 40000u; short _5 = short(_3);', '_5 % 16'))
        self.assertTrue(ok('uint _3 = as_type<uint>(in.v) % 100u; short _5 = short(_3);', '_5 % 16'))

    def test_call_bounds(self):
        call = G.Interval.call
        self.assertEqual(call('int', [(0, None)]), G.UNBOUNDED)
        self.assertEqual(call('int', [(0, 2**31 - 1)]), (0, 2**31 - 1))
        self.assertEqual(call('int', [(0, 2**31)]), G.UNBOUNDED)
        self.assertEqual(call('uint', [(-1, 5)]), (0, None))
        self.assertEqual(call('ushort', [(0, 70000)]), (0, None))
        self.assertEqual(call('uint', [(3, 7)]), (3, 7))


class Header(unittest.TestCase):
    def test_header_names_chain_and_guard_hash(self):
        with open(G.__file__, 'rb') as f: sha = hashlib.sha256(f.read()).hexdigest()[:12]
        h = G.header('/x/b-abc', 'T')
        self.assertTrue(h.startswith('# chain b-abc '), h)
        self.assertTrue(h.endswith(' guard ' + sha + '\n'), h)


if __name__ == '__main__':
    unittest.main()
