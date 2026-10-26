"""nso2elf.py 的单测：手工拼一个不压缩的小 NSO（MOD0、动态段、一张重定位表、两个 PLT 桩、.eh_frame_hdr），转成 ELF 后核对段、节与字节；不读游戏文件。 | Unit tests for nso2elf.py: a hand-built small uncompressed NSO (MOD0, a dynamic section, relocation tables, two PLT stubs, an .eh_frame_hdr) is converted to ELF and its segments, sections and bytes are checked; no game files are read."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nso2elf  # noqa: E402

TEXT, RO, DATA, BSS = (0x000, 0x100), (0x100, 0x100), (0x200, 0x100), 0x40
DYN, GOTPLT, GOT, EHS, EHF = 0x200, 0x2a0, 0x2c8, 0x1b8, 0x1c8


def image():
    img = bytearray(DATA[0] + DATA[1])
    struct.pack_into('<I', img, 4, 0x8)                                   # MOD0 偏移 | MOD0 offset
    img[8:12] = b'MOD0'
    struct.pack_into('<6i', img, 12, DYN - 8, 0x300 - 8, 0x340 - 8, EHS - 8, EHS + 12 - 8, 0)
    for at in (0xc0,):                                                    # PLT0：stp x16, x30, [sp, #-16]! 等 | PLT0: stp x16, x30, [sp, #-16]! and so on
        struct.pack_into('<I', img, at, 0xa9bf7bf0)
    for at in (0xe0, 0xf0):                                               # PLT 桩：adrp x16 / ldr x17 / add x16 / br x17 | PLT stub: adrp x16 / ldr x17 / add x16 / br x17
        struct.pack_into('<IIII', img, at, 0x90000010, 0xf9400211, 0x91000210, 0xd61f0220)
    rela, jmp, hsh, sym, strt = 0x100, 0x118, 0x148, 0x160, 0x1a8
    struct.pack_into('<QQq', img, rela, GOT, 1027, 0x10)                 # R_AARCH64_RELATIVE
    for i in range(2): struct.pack_into('<QQq', img, jmp + 24 * i, GOTPLT + 24 + 8 * i, (1 + i) << 32 | 1026, 0)   # R_AARCH64_JUMP_SLOT
    struct.pack_into('<II', img, hsh, 1, 3)                               # nbucket = 1, nchain = 3
    names = b'\0foo\0bar\0'
    img[strt:strt + len(names)] = names
    for i, nm in enumerate((1, 5)):
        struct.pack_into('<IBBHQQ', img, sym + 24 * (1 + i), nm, 0x12, 0, 0, 0, 0)   # 未定义的函数导入 | undefined function import
    img[EHS:EHS + 4] = bytes([1, 0x1b, 0x03, 0x3b])
    struct.pack_into('<iI', img, EHS + 4, EHF - (EHS + 4), 0)            # eh_frame_ptr，FDE 个数 0 | eh_frame_ptr, FDE count 0
    tags = [(6, sym), (5, strt), (10, len(names)), (4, hsh), (7, rela), (8, 24), (23, jmp), (2, 48), (3, GOTPLT), (0, 0)]
    for i, (t, v) in enumerate(tags): struct.pack_into('<qQ', img, DYN + 16 * i, t, v)
    return bytes(img)


def nso(img):
    h = bytearray(0x100)
    h[0:4] = b'NSO0'
    for i, (mo, sz) in enumerate((TEXT, RO, DATA)):
        struct.pack_into('<III', h, 0x10 + 0x10 * i, 0x100 + mo, mo, sz)
        struct.pack_into('<I', h, 0x60 + 4 * i, sz)
    struct.pack_into('<I', h, 0x3c, BSS)
    return bytes(h) + img


def sections(elf):
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shnum, shstrndx = struct.unpack_from('<HH', elf, 0x3c)
    raw = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + 64 * i) for i in range(shnum)]
    stroff = raw[shstrndx][4]
    name = lambda o: elf[stroff + o:elf.index(b'\0', stroff + o)].decode()
    return {name(s[0]): s for s in raw[1:]}, [name(s[0]) for s in raw]


class Nso2Elf(unittest.TestCase):
    def convert(self, data):
        with tempfile.NamedTemporaryFile(suffix='.nso') as f:
            f.write(data); f.flush()
            return nso2elf.build(f.name)[0]

    def test_header_and_loads(self):
        img = image(); elf = self.convert(nso(img))
        self.assertEqual(elf[:4], b'\x7fELF')
        self.assertEqual(struct.unpack_from('<HH', elf, 16), (3, 183))          # ET_DYN, EM_AARCH64
        phnum, = struct.unpack_from('<H', elf, 0x38)
        ph = [struct.unpack_from('<IIQQQQQQ', elf, 64 + 56 * i) for i in range(phnum)]
        loads = [p for p in ph if p[0] == 1]
        self.assertEqual([(p[3], p[1]) for p in loads], [(0, 5), (0x100, 4), (0x200, 6)])
        for p in loads: self.assertEqual(elf[p[2]:p[2] + p[5]], img[p[3]:p[3] + p[5]])   # 字节原样 | bytes unchanged
        self.assertEqual(loads[2][6], DATA[1] + BSS)                                        # data 段带 bss | data segment includes bss
        self.assertIn((2, DYN), [(p[0], p[3]) for p in ph])                                 # PT_DYNAMIC
        self.assertIn((0x6474e550, EHS), [(p[0], p[3]) for p in ph])                        # PT_GNU_EH_FRAME

    def test_sections(self):
        elf = self.convert(nso(image()))
        sec, order = sections(elf)
        at = {n: (s[3], s[5]) for n, s in sec.items()}
        self.assertEqual(at['.plt'], (0xc0, 0x40))
        self.assertEqual(at['.text'], (0, 0xc0))
        self.assertEqual(at['.got.plt'], (GOTPLT, 0x28))
        self.assertEqual(at['.got'], (GOT, 8))
        self.assertEqual(at['.eh_frame_hdr'], (EHS, 12))
        self.assertEqual(at['.eh_frame'], (EHF, 4))
        self.assertEqual(at['.dynsym'], (0x160, 72))
        self.assertEqual(at['.bss'], (0x300, BSS))
        self.assertEqual(order[sec['.dynsym'][6]], '.dynstr')                               # sh_link
        self.assertEqual(order[sec['.rela.plt'][7]], '.got.plt')                            # sh_info
        spans = sorted((s[3], s[3] + s[5]) for n, s in sec.items() if s[2] & 2 and n != '.bss')
        for (_, a1), (b0, _) in zip(spans, spans[1:]): self.assertLessEqual(a1, b0)       # 互不重叠 | no overlaps
        self.assertEqual(spans[0][0], 0); self.assertEqual(spans[-1][1], DATA[0] + DATA[1])   # 铺满整个映像 | covers the whole image

    def test_not_nso(self):
        with self.assertRaises(ValueError): self.convert(b'NOPE' + bytes(0x200))


if __name__ == '__main__':
    unittest.main()
