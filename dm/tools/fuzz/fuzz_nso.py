#!/usr/bin/env python3
"""Fuzz dm/verify/nsoimg.py's decode, the NSO decoder the verification and analysis tools read the game's modules through (guestsyms, nso_fnmap, the
equivalence checks). Seeds: an NSO0 with three small segments, text LZ4-compressed, ro and data stored, and the same with every segment compressed.
Clean rejection: nsoimg.Error (a ValueError). The image must have the length its segments and bss lay out; any other exception, a wrong length, or an
allocation beyond fuzzlib.allowed() is a finding. Usage: see fuzzlib.py. Adapted from nxnative (github.com/seedds/nxnative, MIT, (c) 2026 seedds)."""
import struct
import sys

import fuzzlib
import lz4.block
import nsoimg

HARNESS = 'nso'
fuzzlib.guard(nsoimg)


def nso(segments, compress):
    """NSO0: +0xc flags (bit i: segment i compressed), +0x10 + 0x10 i {file offset, memory offset, size}, +0x3c bss, +0x40 module ID, +0x60 stored sizes"""
    head, body, mem, stored = bytearray(0x100), bytearray(), 0, []
    head[:4] = b'NSO0'
    for i, seg in enumerate(segments):
        raw = lz4.block.compress(seg, store_size=False) if compress[i] else seg
        struct.pack_into('<III', head, 0x10 + 0x10 * i, 0x100 + len(body), mem, len(seg))
        body += raw; stored.append(len(raw)); mem += (len(seg) + 0xfff) & ~0xfff
    struct.pack_into('<I', head, 0xc, sum(1 << i for i, c in enumerate(compress) if c))
    struct.pack_into('<I', head, 0x3c, 0x1000); head[0x40:0x60] = bytes(range(32)); struct.pack_into('<III', head, 0x60, *stored)
    return bytes(head + body)


def seeds():
    text = bytes.fromhex('fd7bbfa9fd030091') * 32 + bytes.fromhex('c0035fd6')   # stp / mov / ret
    ro, data = b'fuzz ro segment\0' * 8, struct.pack('<4Q', 1, 2, 3, 4)
    return {'text-lz4': nso([text, ro, data], (1, 0, 0)), 'all-lz4': nso([text, ro, data], (1, 1, 1))}


def check(data):
    try: img = nsoimg.decode(data)
    except nsoimg.Error: return   # the declared rejection
    _, mo, sz = struct.unpack_from('<III', data, 0x30); bss = struct.unpack_from('<I', data, 0x3c)[0]
    if len(img) != mo + sz + bss: raise fuzzlib.broken('nsoimg.py:decode', f'the image is {len(img):#x} bytes, the segments and bss lay out {mo + sz + bss:#x}')


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
