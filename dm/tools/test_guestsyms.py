"""guestsyms.py 与 crashlog.py 的单测：手工拼一个小模块映像（MOD0、两个 FDE 的 .eh_frame_hdr、一个动态符号），合成 crash.json；不读游戏文件。 | Unit tests for guestsyms.py and crashlog.py: a hand-built small module image (MOD0, an .eh_frame_hdr with two FDEs, one dynamic symbol) and a synthetic crash.json; no game files are read."""
import io
import json
import os
import contextlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import crashlog  # noqa: E402
import guestsyms  # noqa: E402


def image():
    img = bytearray(0x1000)
    struct.pack_into('<I', img, 4, 0x10)                        # MOD0 偏移 | MOD0 offset
    img[0x10:0x14] = b'MOD0'
    dyn, ehs = 0x400, 0x200
    struct.pack_into('<6i', img, 0x14, dyn - 0x10, 0, 0, ehs - 0x10, ehs + 0x40 - 0x10, 0)
    img[ehs:ehs + 4] = bytes([1, 0x1b, 0x03, 0x3b])             # 版本 + 三种编码 | version + three encodings
    struct.pack_into('<i', img, ehs + 4, 0)                     # eh_frame_ptr（不用） | eh_frame_ptr (unused)
    struct.pack_into('<I', img, ehs + 8, 2)                     # FDE 个数 | FDE count
    fdes = [(0x800, 0x300, 0x40), (0x900, 0x340, 0x80)]         # (函数起点, FDE 位置, 长度) | (function start, FDE location, length)
    for i, (start, fde, ln) in enumerate(fdes):
        struct.pack_into('<ii', img, ehs + 12 + 8 * i, start - ehs, fde - ehs)
        struct.pack_into('<IIiI', img, fde, 0x10, 0, 0, ln)    # 长度、CIE 指针、pc_begin、pc_range | length, CIE pointer, pc_begin, pc_range
    symtab, strtab = 0x600, 0x700
    for i, (t, v) in enumerate([(6, symtab), (5, strtab), (10, 0x20), (0, 0)]): struct.pack_into('<qQ', img, dyn + 16 * i, t, v)
    struct.pack_into('<IBBHQQ', img, symtab + 24, 1, 0x12, 0, 1, 0x900, 0x80)   # 第 1 个符号：FUNC，定义在 0x900 | symbol 1: FUNC, defined at 0x900
    img[strtab:strtab + 16] = b'\0_Z3foov\0'.ljust(16, b'\0')
    return bytes(img)


class GuestsymsTest(unittest.TestCase):
    def setUp(self):
        guestsyms._RE_NAMES = {}   # noqa: SLF001 -- 不读本机真实的引擎分析库导出 | do not read this machine's real engine-analysis export

    def tearDown(self):
        guestsyms._RE_NAMES = None   # noqa: SLF001

    def test_functions_and_dynsyms(self):
        img = image()
        self.assertEqual(guestsyms.functions(img), [(0x800, 0x40), (0x900, 0x80)])
        self.assertEqual(guestsyms.dynsyms(img), {0x900: '_Z3foov'})
        self.assertEqual(guestsyms.demangle(['_Z3foov']).get('_Z3foov'), 'foo()')

    def test_lookup(self):
        guestsyms._TABLE = {'main': [(0x800, 0x40, '', 'note'), (0x900, 0x80, 'foo()', '')]}   # noqa: SLF001
        try:
            self.assertEqual(guestsyms.lookup('main+0x810'), 'fn_800+0x10  [note]')
            self.assertEqual(guestsyms.lookup('main+0x97f'), 'foo()+0x7f')
            self.assertEqual(guestsyms.lookup('main+0x880'), '')        # 两个函数之间 | between two functions
            self.assertEqual(guestsyms.lookup('sdk+0x10'), '')
        finally:
            guestsyms._TABLE = None   # noqa: SLF001

    def test_lookup_re_names(self):
        guestsyms._TABLE = {'main': [(0x800, 0x40, '', ''), (0x900, 0x80, 'foo()', ''), (0xa00, 0x10, '', '')]}   # noqa: SLF001
        guestsyms._RE_NAMES = {'main': {0x800: ('PanelManager_Open', 'high'), 0x900: ('ignored', 'high'), 0xa00: ('maybe_x', 'low')}}   # noqa: SLF001
        try:
            self.assertEqual(guestsyms.lookup('main+0x804'), 'PanelManager_Open+0x4')
            self.assertEqual(guestsyms.lookup('main+0x900'), 'foo()+0x0')      # 动态符号名优先 | the dynamic-symbol name wins
            self.assertEqual(guestsyms.lookup('main+0xa04'), 'maybe_x (low)+0x4')
        finally:
            guestsyms._TABLE = guestsyms._RE_NAMES = None   # noqa: SLF001

    def test_re_names_file(self):
        with tempfile.TemporaryDirectory() as d:
            old, guestsyms.DM_ROOT, guestsyms._RE_NAMES = guestsyms.DM_ROOT, d, None   # noqa: SLF001
            try:
                os.makedirs(os.path.join(d, 're', 'export'))
                Path(guestsyms.re_names_path('main')).write_text('# x\noffset\tname\tkind\tconfidence\tsource\n0x800\tf_a\tfunction\thigh\tseed\n0x2f05458\tg_ui_root\tlabel\thigh\tseed\n', encoding='utf-8')
                self.assertEqual(guestsyms.re_names(), {'main': {0x800: ('f_a', 'high')}})
            finally:
                guestsyms.DM_ROOT, guestsyms._RE_NAMES = old, None   # noqa: SLF001

    def test_crashlog_show(self):
        guestsyms._TABLE = {'main': [(0x800, 0x40, 'bar', '')]}   # noqa: SLF001
        try:
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / 'crash.json'
                p.write_text(json.dumps(dict(v=1, sig=11, code=2, addr='0x8', pc='0x1000', pc_where='main+0x804', lr='0x0', bt=[dict(pc='0x2000', where=None)], tick=5, stage='x', host_base='0x0')), encoding='utf-8')
                with contextlib.redirect_stdout(io.StringIO()): res = crashlog.show(str(p), '/nonexistent')
            self.assertEqual((res['signal'], res['tick']), ('SIGSEGV', 5))
            self.assertEqual(res['frames'][0], dict(pc='0x1000', where='main+0x804', name='bar+0x4'))
            self.assertEqual(res['frames'][1]['where'], None)
        finally:
            guestsyms._TABLE = None   # noqa: SLF001


if __name__ == '__main__':
    unittest.main()
