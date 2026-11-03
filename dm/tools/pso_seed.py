#!/usr/bin/env python3
# Pipeline warm-up seed tool (dm/gfx/nvn/pso_manifest.tsv): reads pipeline manifests (the seed, a run's pso_user.tsv) at the byte level of the
# pso_skey rows the graphics layer writes (dm/gfx/nvn/nvn_pso_skey.h), with the same normalisation (pso_key_norm).
#   diff <manifest>... --seed FILE   the distinct normalised S / R / C rows of the manifests that the normalised seed lacks:
#                                    'DIFF S n R n C n', then RESULT {json} with each row's shader keys
#   normalise <in> [--out FILE] [--note TEXT]...   writes the layout line and a comment block, then the rows in their order, each checked and normalised as
#                                    pso_skey_check does; invalid rows and later duplicates are dropped ('DROPPED <tag> <reason> <hex…>'),
#                                    the background marker line is kept; 'KEPT S n R n C n', then RESULT {json}
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
HEAD = '# pso_skey v2 432'         # PSO_MANIFEST_HEAD
BG_MARK = '# warm: background'     # PSO_WARM_BG_MARK
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


def check_row(h):
    """pso_skey_check on a row's hex: (reason, hex) for a row the warm-up refuses, reason one of parse, pad, ptr, flags, fz, kind, key, lmap
    (the first failed check, in the C order); (None, canonical hex) otherwise: shader keys zero after their NUL, no fz_desc on an unspecialised
    row, the key normalised."""
    if len(h) != SKEY_LEN * 2: return 'parse', h
    try: b = bytearray(bytes.fromhex(h))
    except ValueError: return 'parse', h
    if any(b[86:88]): return 'pad', h
    if any(b[88:104]) or any(b[424:432]): return 'ptr', h
    if b[50] > 1 or b[51] > 1: return 'flags', h
    if b[51] and not b[50]: return 'fz', h
    if b[51] and b[52] != 1: return 'kind', h
    if not b[0] or 0 not in b[0:24] or 0 not in b[24:48]: return 'key', h
    if b[50] and any(x > 7 and x != 0xff for x in b[O_LMAP:O_LMAP + 8]): return 'lmap', h
    for s0 in (0, 24):
        z = b.index(0, s0, s0 + 24); b[z:s0 + 24] = bytes(s0 + 24 - z)
    if not b[51]: b[52:86] = bytes(34)
    return None, norm_row(bytes(b)).hex()


def parse_line(line):
    """(tag, value, stage, reason) of a manifest row: value is the canonical hex for R; reason is None for a valid row."""
    c = line.split('\t')
    if c[0] == 'S':
        return ('S', c[1] if len(c) > 1 else '', c[2] if len(c) > 2 else None, None if len(c) >= 3 and c[1] else 'fields' if len(c) < 3 else 'empty')
    if c[0] == 'C':
        return ('C', c[1] if len(c) > 1 else '', None, None if len(c) > 1 and c[1] else 'empty')
    if c[0] == 'R':
        why, h = check_row(c[1] if len(c) > 1 else '')
        return ('R', h, None, why)
    return (c[0], '', None, 'tag')


def row_id(tag, val):
    return f'{tag}\t{val}'


def normalise(lines):
    """(output lines, dropped [(tag, reason, value)], kept counts) for a manifest's lines."""
    out, dropped, seen, marked = [], [], set(), False
    kept = {'S': 0, 'R': 0, 'C': 0}
    for line in lines:
        line = line.rstrip('\r\n')
        if line == BG_MARK and not marked: out.append(line); marked = True; continue
        if not line or line.startswith('#'): continue
        tag, val, stage, why = parse_line(line)
        if why: dropped.append((tag, why, val)); continue
        k = row_id(tag, val)
        if k in seen: dropped.append((tag, 'duplicate', val)); continue
        seen.add(k); kept[tag] += 1
        out.append(f'S\t{val}\t{stage}' if tag == 'S' else f'{tag}\t{val}')
    return out, dropped, kept


NORM_NOTE = ['# Pipeline warm-up manifest. Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex, dm/gfx/nvn/nvn_pso_skey.h) / C<TAB>compute shader key.',
             '# Rows are checked and normalised (dm/tools/pso_seed.py normalise; t_pso_skey lints the shipped seed). The row order is the build order: do not sort.',
             f"# In the seed, rows after a '{BG_MARK}' line warm in the background: the first loading screen waits only for the rows before it."]


def cstr(b: bytes) -> str:
    return b.split(b'\0', 1)[0].decode('ascii', 'replace')


def row_keys(tag, val):
    """(vs, fs) shader keys of a row ('' when absent)."""
    if tag == 'R':
        b = bytes.fromhex(val)
        return cstr(b[0:24]), cstr(b[24:48])
    return val, ''


def read_rows(path):
    """[(tag, value, stage)] of a manifest's valid rows, R values as canonical hex; comments, blank lines and invalid rows are left out."""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'): continue
            tag, val, stage, why = parse_line(line)
            if not why: out.append((tag, val, stage))
    return out


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
    n = sub.add_parser('normalise', help='check, normalise and dedupe a manifest')
    n.add_argument('input'); n.add_argument('--out'); n.add_argument('--note', action='append', default=[], help='one more comment line (repeatable)')
    a = ap.parse_args(argv)
    if a.cmd == 'normalise':
        with open(a.input, encoding='utf-8', errors='replace') as f: lines = f.readlines()
        out, dropped, kept = normalise(lines)
        for tag, why, val in dropped: print(f'DROPPED {tag} {why} {val[:40]}')
        text = '\n'.join([HEAD] + NORM_NOTE + ['# ' + x for x in a.note] + out) + '\n'
        if a.out:
            tmp = a.out + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f: f.write(text)
            os.replace(tmp, a.out)
        else: sys.stdout.write(text)
        print(f"KEPT S {kept['S']} R {kept['R']} C {kept['C']}", file=sys.stdout if a.out else sys.stderr)
        why = {}
        for tag, w, _ in dropped: why[f'{tag} {w}'] = why.get(f'{tag} {w}', 0) + 1
        print('RESULT ' + json.dumps(dict(name='pso_seed normalise', kept=kept, dropped=why)), file=sys.stdout if a.out else sys.stderr)
        return 0
    if a.cmd == 'diff':
        rows = diff(a.manifests, a.seed); n = count(rows)
        for r in rows: print(f"{r['tag']}\t{r['vs']}\t{r['fs']}")
        print(f"DIFF S {n['S']} R {n['R']} C {n['C']}")
        print('RESULT ' + json.dumps(dict(name='pso_seed diff', counts=n, rows=rows)))
        return 0
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
