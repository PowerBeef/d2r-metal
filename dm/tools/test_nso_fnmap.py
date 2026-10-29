"""Unit test for nso_fnmap.py on two synthetic builds (no game files): a function that moved, a global that moved, the verdict line."""
from pathlib import Path
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nso_fnmap  # noqa: E402  # pyright: ignore[reportMissingImports]

RET, ADD_10, ADD_410 = 0xd65f03c0, 0x91004021, 0x91104021   # ret; add x1, x1, #0x10; add x1, x1, #0x410
ADRP_2 = 0xd0000001                                          # adrp x1, +2 pages


def image(b_at, add):
    """A memory image with a MOD0 pointer, an eh_frame_hdr table and two functions: A at 0x1000 (adrp + add to a global, then
    movz w0, #1; ret) and B at b_at (movz w2, #7; movz w3, #9; ret)."""
    img = bytearray(0x4000)
    struct.pack_into('<I', img, 4, 0x100)                    # MOD0 at 0x100
    struct.pack_into('<i', img, 0x110, 0x200 - 0x100)        # eh_frame_hdr at 0x200, relative to MOD0
    img[0x200:0x204] = bytes([1, 0x1b, 0x03, 0x3b])
    funcs = [(0x1000, [ADRP_2, add, 0x52800020, RET]), (b_at, [0x528000e2, 0x52800123, RET])]
    struct.pack_into('<I', img, 0x208, len(funcs))
    for i, (at, words) in enumerate(funcs):
        fde = 0x300 + 16 * i
        struct.pack_into('<ii', img, 0x20c + 8 * i, at - 0x200, fde - 0x200)
        struct.pack_into('<iI', img, fde + 8, at - (fde + 8), 4 * len(words))
        for k, w in enumerate(words): struct.pack_into('<I', img, at + 4 * k, w)
    return bytes(img)


class FnmapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='fnmap ')
        d = Path(self.tmp.name)
        self.old, self.new = d / 'old.bin', d / 'new.bin'
        self.old.write_bytes(image(0x1100, ADD_10))           # global at 0x3010
        self.new.write_bytes(image(0x1140, ADD_410))          # B moved by 0x40, the global to 0x3410

    def tearDown(self):
        self.tmp.cleanup()

    def run_map(self, *offs):
        out = io.StringIO()
        with redirect_stdout(out): rc = nso_fnmap.main([str(self.old), str(self.new), *offs])
        return rc, out.getvalue()

    def test_moved_function_and_global(self):
        rc, out = self.run_map('0x1104', '0x1004', '0x3010')
        self.assertEqual(rc, 0, out)
        self.assertIn('0x1104 -> 0x1144  code  exact', out)    # B moved by 0x40
        self.assertIn('0x1004 -> 0x1004  code  exact', out)    # A unchanged once the add immediate is masked
        self.assertIn('0x3010 -> 0x3410  data  1/1', out)       # decoded from A's adrp + add in the new build
        self.assertTrue(out.splitlines()[-1].startswith('FNMAP OK 3 offsets, 0 red'))

    def test_unmappable_global_is_red(self):
        rc, out = self.run_map('0x3800')                       # nothing computes it
        self.assertEqual(rc, 1)
        self.assertIn('【红】 0x3800: data', out)
        self.assertTrue(out.splitlines()[-1].startswith('FNMAP RED 1 offsets, 1 red'))


if __name__ == '__main__':
    unittest.main()
