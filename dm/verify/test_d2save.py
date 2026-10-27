#!/usr/bin/env python3
# d2save.py 的单测：用合成的字节（不含任何游戏内容）造合法的 .d2s / .d2i，再逐条弄坏，确认每条规则都会判不合法（正对照）。
# Unit test for d2save.py: build valid .d2s / .d2i from synthetic bytes (no game content at all), then break them one rule at a time and confirm each rule makes them invalid (positive controls).
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d2save  # noqa: E402


def d2s(n=0x300, ver=0x69):
    b = bytearray(n); struct.pack_into('<III', b, 0, d2save.MAGIC, ver, n)
    for i in range(0x40, n): b[i] = (i * 7) & 0xff
    struct.pack_into('<I', b, 0x0c, d2save.d2s_sum(b)); return bytes(b)


def section(gold=0, size=0x44, ver=0x69):
    b = bytearray(size); struct.pack_into('<IIIII', b, 0, d2save.MAGIC, 2, ver, gold, size); b[0x40:0x42] = b'JM'; return bytes(b)


bad = 0
def expect(cond, what):
    global bad
    print(f"{'✓' if cond else '✗'} {what}"); bad += not cond


good = d2s()
expect(not d2save.check_d2s(good), 'valid .d2s passes')
b = bytearray(good); b[0x100] ^= 1
expect(any('checksum' in p for p in d2save.check_d2s(bytes(b))), 'a flipped byte breaks the checksum')
b = bytearray(good); struct.pack_into('<I', b, 8, len(good) + 4); struct.pack_into('<I', b, 0x0c, d2save.d2s_sum(b))
expect(any('size field' in p for p in d2save.check_d2s(bytes(b))), 'a wrong size field is caught')
expect(any('version' in p for p in d2save.check_d2s(d2s(ver=0x60))), 'an unknown version is caught')
expect(any('header' in p for p in d2save.check_d2s(b'\0' * 0x40)), 'a missing header is caught')
stash = section() * 6 + section(gold=1234, size=0x94)
expect(not d2save.check_d2i(stash), 'valid .d2i passes')
expect(any('gold' in p for p in d2save.check_d2i(section(gold=2_500_001))), 'gold over the cap is caught')
expect(any('length' in p for p in d2save.check_d2i(stash[:-4])), 'a truncated last section is caught')
expect(any('header' in p for p in d2save.check_d2i(section() + b'\0' * 0x44)), 'a section without a header is caught')
with tempfile.TemporaryDirectory() as t:
    os.makedirs(os.path.join(t, 'A')); os.makedirs(os.path.join(t, 'A.prev'))
    open(os.path.join(t, 'A', 'A.d2s'), 'wb').write(good); open(os.path.join(t, 'A.prev', 'A.d2s'), 'wb').write(b'junk')
    open(os.path.join(t, 'A', 'A.d2s.meta'), 'w').write('{"last_modified_time":1}')
    expect(d2save.main([t]) == 0, 'a directory with a valid save passes and side copies (.prev) are skipped')
    open(os.path.join(t, 'A', 'A.d2s'), 'wb').write(good[:-1])
    expect(d2save.main([t]) == 1, 'a directory with a broken save fails')
with tempfile.TemporaryDirectory() as t:
    expect(d2save.main([t]) == 3, 'a directory without .d2s is insufficient coverage (3)')
print('OK' if not bad else f'RED {bad}')
sys.exit(1 if bad else 0)
