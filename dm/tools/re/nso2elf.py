#!/usr/bin/env python3
# NSO → 标准 ELF64 AArch64 共享对象（ET_DYN，虚拟地址 = 模块偏移），给 Ghidra 等反汇编器用现成的 ELF 加载器读。
#   段：每个 NSO 段一个 PT_LOAD（text RX / rodata R / data+bss RW），PT_DYNAMIC 指向 MOD0 的动态段，PT_GNU_EH_FRAME 指向 .eh_frame_hdr。
#   节：由动态段各 DT_* 项与 MOD0 推出（.rela.dyn .rela.plt .hash .gnu.hash .dynsym .dynstr .eh_frame_hdr .eh_frame .dynamic .got.plt .got .init_array .plt .bss），
#       其余部分按所在段补成 .text / .rodata / .data；各节互不重叠。Ghidra 只对叫 .eh_frame* 的块跑展开表分析、只把 .got* 当只读，所以节名要有。
#   字节不改：重定位由加载器按 .rela.* 自己做；输出文件是由游戏程序推出来的数据，只放 $DM_ROOT/re/in，不入库。
#   用法：nso2elf.py <NSO 文件> <输出 .elf>
# NSO → a standard ELF64 AArch64 shared object (ET_DYN, virtual address = module offset), so Ghidra and other disassemblers can read it with their stock ELF loader.
#   Segments: one PT_LOAD per NSO segment (text RX / rodata R / data+bss RW), PT_DYNAMIC on MOD0's dynamic section, PT_GNU_EH_FRAME on .eh_frame_hdr.
#   Sections: derived from the dynamic section's DT_* entries and MOD0 (.rela.dyn .rela.plt .hash .gnu.hash .dynsym .dynstr .eh_frame_hdr .eh_frame .dynamic .got.plt .got .init_array .plt .bss);
#       the rest of each segment is filled in as .text / .rodata / .data; no two sections overlap. Ghidra only runs its unwind-table analysis on blocks named .eh_frame* and only treats .got* as read-only, so the names matter.
#   Bytes are unchanged: the loader applies the relocations itself from .rela.*; the output is data derived from the game program, kept only in $DM_ROOT/re/in and never committed.
#   Usage: nso2elf.py <NSO file> <output .elf>
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'verify'))
import nsoimg  # noqa: E402

FILE_BASE = 0x1000          # 映像在文件里的起点：文件偏移 = FILE_BASE + 虚拟地址，满足页对齐同余 | Where the image starts in the file: file offset = FILE_BASE + virtual address, keeping the page-alignment congruence
PLT_STUB = (0x90000010, 0x9f00001f)      # adrp x16, … （值, 掩码） | adrp x16, ... (value, mask)
BR_X17 = 0xd61f0220
SHT = dict(NULL=0, PROGBITS=1, STRTAB=3, RELA=4, HASH=5, DYNAMIC=6, NOBITS=8, DYNSYM=11, INIT_ARRAY=14, GNU_HASH=0x6ffffff6)
SHF_W, SHF_A, SHF_X, SHF_INFO_LINK = 1, 2, 4, 0x40


def segments(nso):
    """[(内存偏移, 大小)]×3 与 bss 大小，取自 NSO 头。 | [(memory offset, size)] x3 and the bss size, from the NSO header."""
    segs = [struct.unpack_from('<III', nso, 0x10 + i * 0x10)[1:] for i in range(3)]
    return segs, struct.unpack_from('<I', nso, 0x3c)[0]


def dynamic(img):
    """MOD0 → (动态段偏移, 字节数含 DT_NULL, {tag: 值})。 | MOD0 -> (dynamic section offset, size in bytes including DT_NULL, {tag: value})."""
    m0 = nsoimg.mod0(img)
    dyn = m0 + struct.unpack_from('<i', img, m0 + 4)[0]
    tags, o = {}, dyn
    while True:
        t, v = struct.unpack_from('<qQ', img, o); o += 16
        if t == 0: break
        tags.setdefault(t, v)
    return dyn, o - dyn, tags


def gnu_hash_size(img, at, nsyms):
    """.gnu.hash 的字节数：头 16 + 布隆 8×n + 桶 4×n + 链（symoffset 之后每个符号 4 字节）。 | Size of .gnu.hash: 16-byte header + 8×bloom + 4×buckets + chain (4 bytes per symbol from symoffset on)."""
    nb, symoff, bloom, _ = struct.unpack_from('<IIII', img, at)
    return 16 + 8 * bloom + 4 * nb + 4 * max(0, nsyms - symoff)


def eh_frame_range(img, ehs):
    """.eh_frame_hdr 里的 eh_frame_ptr（pcrel sdata4）给起点，逐条走 CIE/FDE 到长度 0 的结束标记。 | The eh_frame_ptr in .eh_frame_hdr (pcrel sdata4) gives the start; walk the CIE/FDE records up to the zero-length terminator."""
    if img[ehs + 1] != 0x1b: return None
    start = ehs + 4 + struct.unpack_from('<i', img, ehs + 4)[0]
    o = start
    while o + 4 <= len(img):
        ln = struct.unpack_from('<I', img, o)[0]; o += 4
        if ln == 0: return start, o - start
        if ln == 0xffffffff: return None   # 64 位长度的记录：AArch64 上不会出现 | 64-bit length records do not occur on AArch64
        o += ln
    return None


def plt_range(img, text_end, njmp):
    """text 末尾那一串 16 字节 PLT 桩（adrp x16 … br x17）往前再加 32 字节的 PLT0；桩数须与 JMPREL 条数一致。 | The run of 16-byte PLT stubs (adrp x16 ... br x17) at the end of text, plus the 32-byte PLT0 before it; the stub count must match the JMPREL entries."""
    w = lambda o: struct.unpack_from('<I', img, o)[0]
    is_stub = lambda o: o >= 0 and (w(o) & PLT_STUB[1]) == PLT_STUB[0] and w(o + 12) == BR_X17
    o = text_end - 16
    while o >= 0 and is_stub(o): o -= 16
    first = o + 16
    if njmp == 0 or (text_end - first) // 16 != njmp: return None
    return first - 32, text_end - first + 32


def got_end(start, limit, relocated):
    """从 start 起连续的「被重定位的 8 字节槽」到哪为止（GOT 的每一项都被重定位）。 | How far the run of relocated 8-byte slots starting at start goes (every GOT entry is relocated)."""
    a = start
    while a < limit and a in relocated: a += 8
    return a


def layout(path):
    """→ (映像不含 bss, 装载段表, 节表, (动态段, 字节数), (.eh_frame_hdr, 字节数))。节表项 = (名字, 类型, 标志, 地址, 大小, link 名, info, 对齐, entsize)。 | -> (image without bss, load segment table, section table, (dynamic, size), (.eh_frame_hdr, size)). A section entry = (name, type, flags, address, size, link name, info, alignment, entsize)."""
    nso = Path(path).read_bytes()
    if nso[:4] != b'NSO0': raise ValueError(f'{path} is not an NSO')
    img = nsoimg.load(path)
    segs, bss = segments(nso)
    (t_off, t_sz), (r_off, r_sz), (d_off, d_sz) = segs
    m0 = nsoimg.mod0(img)
    ehs, ehe = (m0 + struct.unpack_from('<i', img, m0 + k)[0] for k in (16, 20))
    dyn, dyn_sz, tags = dynamic(img)
    nhash = struct.unpack_from('<I', img, tags[4] + 4)[0] if 4 in tags else 0
    rel = nsoimg.relocs(img)
    njmp = tags.get(2, 0) // 24
    known = []                                          # (名字, 类型, 标志, 地址, 大小, link, info, 对齐, entsize) | (name, type, flags, address, size, link, info, align, entsize)
    def add(name, typ, flags, addr, size, link='', info=0, align=8, ent=0):
        if addr is not None and size: known.append((name, SHT[typ], flags, addr, size, link, info, align, ent))
    add('.rela.dyn', 'RELA', SHF_A, tags.get(7), tags.get(8, 0), '.dynsym', 0, 8, 24)
    add('.rela.plt', 'RELA', SHF_A | SHF_INFO_LINK, tags.get(23), tags.get(2, 0), '.dynsym', '.got.plt', 8, 24)
    if 4 in tags:
        nb = struct.unpack_from('<I', img, tags[4])[0]
        add('.hash', 'HASH', SHF_A, tags[4], 4 * (2 + nb + nhash), '.dynsym', 0, 4, 4)
    if 0x6ffffef5 in tags: add('.gnu.hash', 'GNU_HASH', SHF_A, tags[0x6ffffef5], gnu_hash_size(img, tags[0x6ffffef5], nhash), '.dynsym', 0, 8)
    if 6 in tags and nhash: add('.dynsym', 'DYNSYM', SHF_A, tags[6], 24 * nhash, '.dynstr', 1, 8, 24)
    add('.dynstr', 'STRTAB', SHF_A, tags.get(5), tags.get(10, 0), '', 0, 1)
    add('.eh_frame_hdr', 'PROGBITS', SHF_A, ehs, ehe - ehs, '', 0, 4)
    eh = eh_frame_range(img, ehs)
    if eh: add('.eh_frame', 'PROGBITS', SHF_A, eh[0], eh[1], '', 0, 8)
    add('.dynamic', 'DYNAMIC', SHF_A | SHF_W, dyn, dyn_sz, '.dynstr', 0, 8, 16)
    if 3 in tags:
        gp = tags[3]
        add('.got.plt', 'PROGBITS', SHF_A | SHF_W, gp, 8 * (3 + njmp), '', 0, 8, 8)
        g0 = gp + 8 * (3 + njmp)
        ia = tags.get(25, d_off + d_sz)
        g1 = got_end(g0, ia, {r[0] for r in rel})
        add('.got', 'PROGBITS', SHF_A | SHF_W, g0, g1 - g0, '', 0, 8, 8)
    add('.init_array', 'INIT_ARRAY', SHF_A | SHF_W, tags.get(25), tags.get(27, 0), '', 0, 8, 8)
    add('.fini_array', 'INIT_ARRAY', SHF_A | SHF_W, tags.get(26), tags.get(28, 0), '', 0, 8, 8)
    plt = plt_range(img, t_off + t_sz, njmp)
    if plt: add('.plt', 'PROGBITS', SHF_A | SHF_X, plt[0], plt[1], '', 0, 16, 16)
    # 每段剩下的空隙补成该段的通用节；已知节必须完整落在某一段里且互不重叠
    # Fill each segment's remaining gaps with that segment's generic section; known sections must lie wholly inside one segment and must not overlap
    secs = []
    for base, size, gen, typ, flags in ((t_off, t_sz, '.text', 'PROGBITS', SHF_A | SHF_X), (r_off, r_sz, '.rodata', 'PROGBITS', SHF_A),
                                        (d_off, d_sz, '.data', 'PROGBITS', SHF_A | SHF_W)):
        inside = sorted((k for k in known if base <= k[3] < base + size), key=lambda k: k[3])
        for k in inside:
            if k[3] + k[4] > base + size: raise ValueError(f'{k[0]} crosses the end of its segment')
        cur, n = base, 0
        for k in inside + [None]:
            nxt = k[3] if k else base + size
            if nxt < cur: raise ValueError(f'{k[0]} overlaps the section before it')
            if nxt > cur:
                secs.append((gen + (f'.{n}' if n else ''), SHT[typ], flags, cur, nxt - cur, '', 0, 16 if flags & SHF_X else 8, 0)); n += 1
            if k: secs.append(k); cur = k[3] + k[4]
    stray = [k[0] for k in known if k not in secs]
    if stray: raise ValueError(f'sections outside every segment: {stray}')
    bss_at = d_off + d_sz
    secs.append(('.bss', SHT['NOBITS'], SHF_A | SHF_W, bss_at, bss, '', 0, 16, 0))
    loads = [(t_off, t_sz, t_sz, 5), (r_off, r_sz, r_sz, 4), (d_off, d_sz, d_sz + bss, 6)]   # (地址, 文件大小, 内存大小, 权限 R=4 W=2 X=1) | (address, file size, memory size, flags R=4 W=2 X=1)
    return img[:d_off + d_sz], loads, secs, (dyn, dyn_sz), (ehs, ehe - ehs)


def build(path):
    """NSO 文件 → (ELF 字节, 节表)。 | NSO file -> (ELF bytes, section table)."""
    img, loads, secs, (dyn, dyn_sz), (ehs, eh_sz) = layout(path)
    phdrs = [(1, fl, FILE_BASE + a, a, a, fs, ms, 0x1000) for a, fs, ms, fl in loads]
    phdrs.append((2, 6, FILE_BASE + dyn, dyn, dyn, dyn_sz, dyn_sz, 8))          # PT_DYNAMIC
    phdrs.append((0x6474e550, 4, FILE_BASE + ehs, ehs, ehs, eh_sz, eh_sz, 4))  # PT_GNU_EH_FRAME
    names = [''] + [s[0] for s in secs] + ['.shstrtab']
    shstr, name_off = bytearray(b'\0'), {}
    for n in names[1:]:
        name_off[n] = len(shstr); shstr += n.encode() + b'\0'
    index = {n: i for i, n in enumerate(names)}
    body = bytearray(FILE_BASE) + img
    shstr_at = len(body); body += shstr
    while len(body) % 8: body.append(0)
    shoff = len(body)
    sh = [struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for name, typ, flags, addr, size, link, info, align, ent in secs:
        li = index.get(link, 0) if link else 0
        ii = info if isinstance(info, int) else index.get(info, 0)
        sh.append(struct.pack('<IIQQQQIIQQ', name_off[name], typ, flags, addr, FILE_BASE + addr, size, li, ii, align, ent))
    sh.append(struct.pack('<IIQQQQIIQQ', name_off['.shstrtab'], SHT['STRTAB'], 0, 0, shstr_at, len(shstr), 0, 0, 1, 0))
    for s in sh: body += s
    ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8)
    ehdr = ident + struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, 64, shoff, 0, 64, 56, len(phdrs), 64, len(sh), len(sh) - 1)
    ph = b''.join(struct.pack('<IIQQQQQQ', *p) for p in phdrs)
    if 64 + len(ph) > FILE_BASE: raise ValueError('program headers do not fit before the image')
    body[0:64 + len(ph)] = ehdr + ph
    return bytes(body), secs


def main():
    if len(sys.argv) != 3: sys.exit('usage: nso2elf.py <NSO file> <output .elf>')
    try:
        elf, secs = build(sys.argv[1])
    except ValueError as e:
        sys.exit(f'🔴 {e}')
    Path(sys.argv[2]).write_bytes(elf)
    bid = Path(sys.argv[1]).read_bytes()[0x40:0x50].hex()
    print(f'NSO2ELF OK {sys.argv[2]} {len(elf)} bytes, {len(secs)} sections, build id {bid}')


if __name__ == '__main__':
    main()
