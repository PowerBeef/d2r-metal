#!/usr/bin/env python3
# 存档合法性：按游戏自己载入存档时的拒收规则查一个存档目录（递归找 .d2s / .d2i），看游戏下次开机会不会认它。
#   .d2s：头 0xAA55AA55、版本 0x68 / 0x69（+4）、大小字段（+8）等于文件长、校验和（+0x0c）自洽（算法同 savecmp.py：校验和字段置 0 后 sum = (sum << 1 | sum >> 31) + 字节）
#   .d2i：一段接一段（每段头 0xAA55AA55、+8 版本 0x68 / 0x69、+0x0c 金币 ≤ 2,500,000、+0x10 段长 ≥ 0x40），各段正好铺满整个文件
#   .d2s.meta：能按 JSON 读
# 用法：d2save.py <存档目录>... [--json]   退出码 0 = 全部合法；1 = 有不合法的；3 = 没有 .d2s（射程不足）
# 末行：D2SAVE OK|RED {json}
# Save validity: checks a save directory (searching .d2s / .d2i recursively) against the rejection rules the game applies when it loads a save, i.e. whether the game would accept it on its next boot.
#   .d2s: header 0xAA55AA55, version 0x68 / 0x69 (+4), size field (+8) equal to the file length, checksum (+0x0c) self-consistent (same algorithm as savecmp.py: with the checksum field zeroed, sum = (sum << 1 | sum >> 31) + byte)
#   .d2i: section after section (each with header 0xAA55AA55, version 0x68 / 0x69 at +8, gold at +0x0c ≤ 2,500,000, section length at +0x10 ≥ 0x40), the sections exactly covering the whole file
#   .d2s.meta: readable as JSON
# Usage: d2save.py <save directory>... [--json]   exit code 0 = all valid; 1 = something invalid; 3 = no .d2s (insufficient coverage)
# Last line: D2SAVE OK|RED {json}
import json
import os
import struct
import sys

MAGIC = 0xAA55AA55
VERSIONS = (0x68, 0x69)
GOLD_MAX = 2_500_000


def d2s_sum(b):
    """校验和字段置 0 后逐字节 sum = (sum << 1 | sum >> 31) + byte（32 位）。 | With the checksum field zeroed, per byte sum = (sum << 1 | sum >> 31) + byte (32-bit)."""
    z = bytearray(b); z[0x0c:0x10] = b'\0\0\0\0'; s = 0
    for c in z: s = (((s << 1) | (s >> 31)) + c) & 0xFFFFFFFF
    return s


def check_d2s(b):
    """返回问题列表（空 = 合法）。 | Returns the list of problems (empty = valid)."""
    if len(b) < 0x10: return [f'too short ({len(b)} bytes)']
    magic, ver, size, csum = struct.unpack_from('<IIII', b, 0)
    bad = []
    if magic != MAGIC: bad.append(f'header {magic:#x}')
    if ver not in VERSIONS: bad.append(f'version {ver:#x}')
    if size != len(b): bad.append(f'size field {size} != file length {len(b)}')
    if csum != d2s_sum(b): bad.append(f'checksum {csum:#010x} != {d2s_sum(b):#010x}')
    return bad


def check_d2i(b):
    bad, off, n = [], 0, 0
    while off < len(b):
        if off + 0x14 > len(b): bad.append(f'section {n} at {off:#x}: truncated header'); break
        magic, _flag, ver, gold, size = struct.unpack_from('<IIIII', b, off)
        if magic != MAGIC: bad.append(f'section {n} at {off:#x}: header {magic:#x}'); break
        if ver not in VERSIONS: bad.append(f'section {n}: version {ver:#x}')
        if gold > GOLD_MAX: bad.append(f'section {n}: gold {gold} > {GOLD_MAX}')
        if size < 0x40 or off + size > len(b): bad.append(f'section {n} at {off:#x}: length {size} (file {len(b)})'); break
        off += size; n += 1
    if not n and not bad: bad.append('no sections')
    return bad


def check_dir(root):
    """返回 [(相对路径, 问题列表)]；只看已提交的存档，跳过 .prev / .new / .re 这类旁边的副本。 | Returns [(relative path, problems)]; only the committed save, skipping side copies such as .prev / .new / .re."""
    out = []
    for r, ds, fs in os.walk(root):
        ds[:] = sorted(d for d in ds if not d.endswith(('.prev', '.new', '.re')))
        for f in sorted(fs):
            p = os.path.join(r, f); rel = os.path.relpath(p, root)
            if f.endswith('.d2s'): out.append((rel, check_d2s(open(p, 'rb').read())))
            elif f.endswith('.d2i'): out.append((rel, check_d2i(open(p, 'rb').read())))
            elif f.endswith('.d2s.meta'):
                try: json.load(open(p, encoding='utf-8')); out.append((rel, []))
                except (OSError, ValueError) as e: out.append((rel, [f'meta not JSON: {e}']))
    return out


def main(argv):
    dirs = [a for a in argv if not a.startswith('--')]
    if not dirs: print(__doc__ or 'usage: d2save.py <save dir>...'); return 2
    res, bad, nd2s = [], 0, 0
    for d in dirs:
        for rel, probs in check_dir(d):
            nd2s += rel.endswith('.d2s'); bad += bool(probs)
            res.append(dict(dir=d, file=rel, ok=not probs, problems=probs))
            if probs: print(f'  ✗ {os.path.join(d, rel)}: ' + '; '.join(probs))
    verdict = 'RED' if bad else 'OK' if nd2s else 'UNAVAIL'
    summary = dict(files=len(res), d2s=nd2s, bad=bad)
    if '--json' in argv: print(json.dumps(res, ensure_ascii=False))
    print(f'D2SAVE {verdict} ' + json.dumps(summary))
    return 1 if bad else 0 if nd2s else 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
