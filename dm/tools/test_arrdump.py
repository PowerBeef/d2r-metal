#!/usr/bin/env python3
"""arrdump.py on made-up dumps: file names give the shape, a layer is found by level and index, and nonzero sees where the data is."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arrdump


class ArrDump(unittest.TestCase):
    def test_shape_and_layers(self):
        with tempfile.TemporaryDirectory() as d:
            # BC4 (8 bytes a block), 16 x 16 with 2 levels and 3 layers: level 0 = 16 blocks a layer, level 1 = 4
            path = os.path.join(d, 'arr_7_pf140_16x16_m2_l3.bin')
            l0 = [bytes([i + 1]) * 8 * 16 for i in range(3)]
            l1 = [bytes([i + 10]) * 8 * 4 for i in range(3)]
            with open(path, 'wb') as f: f.write(b''.join(l0 + l1))
            s = arrdump.shape(path)
            self.assertEqual((s['serial'], s['pf'], s['w'], s['h'], s['levels'], s['layers'], s['block']), (7, 140, 16, 16, 2, 3, 8))
            self.assertEqual(arrdump.read_layer(path, s, 2, 0)[2], l0[2])
            self.assertEqual(arrdump.read_layer(path, s, 1, 1), (8, 8, l1[1]))

    def test_nonzero_corner(self):
        s = arrdump.shape('arr_1_pf140_16x16_m1_l1.bin')
        blocks = bytearray(8 * 16)
        blocks[0:8] = b'\xff' * 8   # only the top-left block holds data
        frac, box = arrdump.nonzero(s, 16, 16, bytes(blocks))
        self.assertEqual((frac, box), (1 / 16, (0, 0, 4, 4)))
        self.assertEqual(arrdump.nonzero(s, 16, 16, bytes(8 * 16)), (0.0, None))

    def test_decode(self):
        s = arrdump.shape('arr_1_pf140_8x8_m1_l1.bin')
        img = arrdump.decode(s, 8, 8, b'\xff\xff' + b'\x00' * 6 + b'\xff\xff' + b'\x00' * 6 + b'\xff\xff' + b'\x00' * 6 + b'\xff\xff' + b'\x00' * 6)
        self.assertEqual(img.size, (8, 8)); self.assertEqual(img.getpixel((0, 0))[0], 255)

    def test_refuses_other_names(self):
        with self.assertRaises(SystemExit): arrdump.shape('picture.bin')


if __name__ == '__main__':
    unittest.main()
