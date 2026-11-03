#!/usr/bin/env python3
# Pipeline warm-up seed tool (dm/gfx/nvn/pso_manifest.tsv): reads pipeline manifests (the seed, a run's pso_user.tsv) at the byte level of the
# pso_skey rows the graphics layer writes (dm/gfx/nvn/nvn_pso_skey.h), with the same normalisation (pso_key_norm).
#   diff <manifest>... --seed FILE   the distinct normalised S / R / C rows of the manifests that the normalised seed lacks:
#                                    'DIFF S n R n C n', then RESULT {json} with each row's shader keys
# Every row form (S<TAB>shader key<TAB>stage, R<TAB>pso_skey hex, C<TAB>compute shader key) and the byte offsets below follow nvn_pso_skey.h;
# norm_row and pso_key_norm must change together (t_pso_skey and test_pso_seed.py check both against one hash).
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
SEED = os.path.join(DM, 'gfx', 'nvn', 'pso_manifest.tsv')

SKEY_LEN = 432   # sizeof(pso_skey)
# pso_skey byte offsets (little-endian): vs 0:24, fs 24:48, vs_stage 48, fs_stage 49, merged 50, fz 51, fz_desc kind 52, fbmask 53, fx 54:86,
# padding 86:88, then pso_key at 88: vs pointer 88:96, fs pointer 96:104, cfmt 104:136 (8 x u32), blend_en 148, chmask 152, blend 160:224 (8 x 8 bytes),
# lmap 416:424, fsfn pointer 424:432
O_MERGED, O_CFMT, O_BLEND_EN, O_CHMASK, O_BLEND, O_LMAP = 50, 104, 148, 152, 160, 416


def norm_row(b: bytes) -> bytes:
    """The rule of pso_key_norm on a 432-byte row: the write mask and blend state of an output with no attachment (normal pass) or with no
    physical attachment mapped to it (merged pass), and the blend factors of an output whose blending is off, are cleared; a normal pass gets
    an all-zero lmap."""
    b = bytearray(b)
    merged = b[O_MERGED]
    cfmt = struct.unpack_from('<8I', b, O_CFMT)
    en_in, ch_in = struct.unpack_from('<II', b, O_BLEND_EN)
    en = ch = 0
    bl = [bytes(8)] * 8
    for i in range(8):
        if not cfmt[i]: continue
        lo = b[O_LMAP + i] if merged else i
        if lo > 7: continue
        ch |= ch_in & (0xf << (4 * lo))
        if en_in >> lo & 1:
            en |= 1 << lo
            bl[lo] = bytes(b[O_BLEND + 8 * lo:O_BLEND + 8 * lo + 8])
    struct.pack_into('<II', b, O_BLEND_EN, en, ch)
    b[O_BLEND:O_BLEND + 64] = b''.join(bl)
    if not merged: b[O_LMAP:O_LMAP + 8] = bytes(8)
    return bytes(b)


def cstr(b: bytes) -> str:
    return b.split(b'\0', 1)[0].decode('ascii', 'replace')


def row_keys(tag, val):
    """(vs, fs) shader keys of a row ('' when absent)."""
    if tag == 'R':
        b = bytes.fromhex(val)
        return cstr(b[0:24]), cstr(b[24:48])
    return val, ''


def read_rows(path):
    """[(tag, value, stage)] of a manifest; comments and blank lines skipped. R values are normalised hex, rows that do not parse are left out."""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'): continue
            c = line.split('\t')
            if c[0] == 'R' and len(c) > 1:
                try: b = bytes.fromhex(c[1])
                except ValueError: continue
                if len(b) == SKEY_LEN: out.append(('R', norm_row(b).hex(), None))
            elif c[0] in ('S', 'C') and len(c) > 1 and c[1]:
                out.append((c[0], c[1], c[2] if len(c) > 2 else None))
    return out


def row_id(tag, val):
    return f'{tag}\t{val}'


def diff(manifests, seed):
    have = {row_id(t, v) for t, v, _ in read_rows(seed)}
    seen, rows = set(), []
    for m in manifests:
        for t, v, _ in read_rows(m):
            k = row_id(t, v)
            if k in have or k in seen: continue
            seen.add(k)
            vs, fs = row_keys(t, v)
            rows.append(dict(tag=t, vs=vs, fs=fs))
    return rows


def count(rows):
    n = {'S': 0, 'R': 0, 'C': 0}
    for r in rows: n[r['tag']] += 1
    return n


def main(argv):
    ap = argparse.ArgumentParser(prog='pso_seed.py', description='Pipeline warm-up seed tool (dm/gfx/nvn/pso_manifest.tsv).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('diff', help='rows of the manifests that the seed lacks')
    d.add_argument('manifests', nargs='+'); d.add_argument('--seed', default=SEED)
    a = ap.parse_args(argv)
    if a.cmd == 'diff':
        rows = diff(a.manifests, a.seed); n = count(rows)
        for r in rows: print(f"{r['tag']}\t{r['vs']}\t{r['fs']}")
        print(f"DIFF S {n['S']} R {n['R']} C {n['C']}")
        print('RESULT ' + json.dumps(dict(name='pso_seed diff', counts=n, rows=rows)))
        return 0
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
