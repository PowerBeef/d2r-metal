#!/usr/bin/env python3
"""Fuzz dm/tools/dmx.py's blte, the decoder of the BLTE frames the game's data archives store files in. Seeds: a frame of one stored ('N') block and one
zlib ('Z') block. Clean rejection: ValueError. Usage: see fuzzlib.py."""
import struct
import sys
import zlib

import fuzzlib
import dmx

HARNESS = 'dmx'


def frame(blocks):
    """BLTE: magic, header length at +4, flags at +8, block count at +9 (3 bytes), 24-byte entries {encoded size, decoded size, MD5}, then the blocks"""
    enc = [b'N' + d if m == 'N' else b'Z' + zlib.compress(d) for m, d in blocks]
    head = bytearray(b'BLTE') + struct.pack('>I', 12 + 24 * len(enc)) + b'\x0f' + len(enc).to_bytes(3, 'big')
    for e, (_, d) in zip(enc, blocks): head += struct.pack('>II', len(e), len(d)) + bytes(16)
    return bytes(head) + b''.join(enc)


def seeds():
    return {'stored-and-zlib': frame([('N', b'plain block ' * 4), ('Z', b'zlib block ' * 16)]), 'one-stored': frame([('N', b'x' * 40)])}


def check(data):
    try: dmx.blte(data)
    except ValueError: return   # the declared rejection


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
