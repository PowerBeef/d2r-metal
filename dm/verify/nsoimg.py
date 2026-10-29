# NSO → 内存映像（与 patch_nso.py 同一解法）；验证工具共用。
# NSO → memory image (same decoding as patch_nso.py); shared by the verification tools.
import struct
import lz4.block

class Error(ValueError):
    """A file that is not a well-formed NSO: the one rejection decode / load raise (fuzzed by dm/tools/fuzz/fuzz_nso.py)."""


def decode(d):
    """NSO bytes -> the whole memory image (segments at their memory offsets, then bss). Checks the header's sizes against the file before
    allocating or copying, so a malformed or cut file ends in Error, not in a wrong-length image, a huge allocation or another exception."""
    if len(d) < 0x70 or d[:4] != b'NSO0': raise Error('not an NSO (no NSO0 header)')
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from('<I', d, 0x3c)[0]
    csz = struct.unpack_from('<III', d, 0x60)
    end = segs[2][1] + segs[2][2]
    for i, (fo, mo, sz) in enumerate(segs):
        if fo + csz[i] > len(d): raise Error(f'segment {i} runs past the end of the file')
        if mo + sz > end or (i and mo < segs[i - 1][1] + segs[i - 1][2]): raise Error(f'segment {i} lies outside the image or overlaps the one before')
    if end + bss > max(16 << 20, 256 * len(d)): raise Error(f'image of {end + bss:#x} bytes from a {len(d):#x}-byte file')   # LZ4 expands at most about 255 times
    img = bytearray(end + bss)
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if flags >> i & 1:
            try: raw = lz4.block.decompress(raw, uncompressed_size=sz)
            except lz4.block.LZ4BlockError as e: raise Error(f'segment {i}: {e}') from None
        if len(raw) != sz: raise Error(f'segment {i}: {len(raw)} bytes, the header says {sz}')
        img[mo:mo + sz] = raw
    return bytes(img)


def load(path):
    try: return decode(open(path, 'rb').read())
    except Error as e: raise Error(f'{path}: {e}') from None


def header(out_dir, mod):
    h = {}
    for line in open(f'{out_dir}/{mod}.fixups.tsv'):
        if not line.startswith('#'): break
        k, v = line[1:].split(); h[k] = int(v, 0)      # 0x 开头为偏移，十进制为槽号 | A 0x prefix means an offset, decimal means a slot number
    return h

def mod0(img):
    m0 = struct.unpack_from('<I', img, 4)[0]
    assert img[m0:m0 + 4] == b'MOD0'
    return m0

def func_starts(img, text_end):
    """eh_frame_hdr 里的函数起点（与 patch_nso.py 同法）。 | Function start addresses from eh_frame_hdr (same method as patch_nso.py)."""
    m0 = mod0(img)
    ehs = m0 + struct.unpack_from('<i', img, m0 + 16)[0]
    def rd(o, enc):
        fmt, app = enc & 0xF, enc & 0x70
        v, sz = {0x3: (struct.unpack_from('<I', img, o)[0], 4), 0xB: (struct.unpack_from('<i', img, o)[0], 4),
                 0x4: (struct.unpack_from('<Q', img, o)[0], 8)}[fmt]
        if app == 0x10: v += o
        if app == 0x30: v += ehs
        return v, sz
    _, penc, cenc, tenc = img[ehs:ehs + 4]
    o = ehs + 4
    _, s = rd(o, penc); o += s
    nfde, s = rd(o, cenc); o += s
    starts = set()
    for _ in range(nfde):
        a, s = rd(o, tenc); o += s
        _, s = rd(o, tenc); o += s
        starts.add(a)
    return sorted(x for x in starts if x < text_end)

def relocs(img):
    """[(偏移, 类型, 符号名或 None, 符号值或 None, addend)]：RELA + JMPREL 全部条目。符号值 None = 未定义（导入）。 | [(offset, type, symbol name or None, symbol value or None, addend)]: all RELA + JMPREL entries. Symbol value None = undefined (import)."""
    m0 = mod0(img)
    dyn = m0 + struct.unpack_from('<i', img, m0 + 4)[0]
    d = {}
    o = dyn
    while True:
        tag, val = struct.unpack_from('<qQ', img, o); o += 16
        if tag == 0: break
        d[tag] = val
    def cstr(p):
        e = img.index(b'\0', p); return img[p:e].decode('latin1')
    out = []
    for tab, sz in ((d.get(7), d.get(8, 0)), (d.get(23), d.get(2, 0))):
        if not tab: continue
        for i in range(sz // 24):
            off, info, add = struct.unpack_from('<QQq', img, tab + 24 * i)
            typ, si = info & 0xFFFFFFFF, info >> 32
            name = val = None
            if si:
                nm, _, _, shn, sv, _ = struct.unpack_from('<IBBHQQ', img, d[6] + 24 * si)
                name = cstr(d[5] + nm); val = sv if shn else None
            out.append((off, typ, name, val, add))
    return out
