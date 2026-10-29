#!/usr/bin/env python3
"""Fuzz dm/verify/d2save.py's checkers of a character file (check_d2s) and a shared-stash file (check_d2i): every scenario run checks its save with them, so
any bytes must end in a list of problems (empty = valid), never in an exception. Seeds: a valid .d2s (header, version, size, checksum) and a .d2i of two sections.
Usage: see fuzzlib.py."""
import struct
import sys

import fuzzlib
import d2save

HARNESS = 'd2save'


def d2s(n=0x40):
    b = bytearray(n); struct.pack_into('<III', b, 0, d2save.MAGIC, sorted(d2save.VERSIONS)[0], n)
    struct.pack_into('<I', b, 0xc, d2save.d2s_sum(bytes(b)))
    return bytes(b)


def d2i():
    out = b''
    for gold in (0, 1234):
        sec = bytearray(0x40); struct.pack_into('<IIIII', sec, 0, d2save.MAGIC, 0, sorted(d2save.VERSIONS)[0], gold, 0x40); out += bytes(sec)
    return out


def seeds():
    return {'d2s': d2s(), 'd2i': d2i()}


def check(data):
    for f in (d2save.check_d2s, d2save.check_d2i):
        bad = f(data)
        if not isinstance(bad, list): raise fuzzlib.broken(f'd2save.py:{f.__name__}', f'returned {type(bad).__name__}, not a list of problems')


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
