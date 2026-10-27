#!/usr/bin/env python3
# 原包精灵图的 DXT5 + 块线性编解码（构建时生成贴图用：键帽 kbd_glyphs.py、前端右栏 gen_fe_rightpanel.py；原先在储藏箱加宽贴图 gen_widestash_art.py 里）。
# 全部做成原包同款 SpA1 v61 = DXT5 + 块线性：原始 RGBA（v31）在大图上读出成排花点（本机宿主实测）。块线性换算用本仓的 nvn_tex.c / nvn_deswizzle.c。
# DXT5 + block-linear encoding and decoding of the original package's sprites (for textures generated at build time: key caps kbd_glyphs.py, front-end right panel gen_fe_rightpanel.py; formerly in the stash-widening art gen_widestash_art.py).
# Everything is made in the original package's SpA1 v61 format = DXT5 + block-linear: raw RGBA (v31) reads back with rows of garbage dots on large images (measured on the local host). The block-linear conversion uses this tree's nvn_tex.c / nvn_deswizzle.c.
import ctypes as C, io, os, struct, subprocess, tempfile
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.join(HERE, '..', '..')

class Desc(C.Structure): _fields_ = [('target', C.c_int), ('format', C.c_int), ('flags', C.c_int), ('samples', C.c_int), ('width', C.c_uint32), ('height', C.c_uint32),
                                     ('depth', C.c_uint32), ('levels', C.c_uint32), ('stride', C.c_uint32), ('pkg_block_h', C.c_int)]
class Layout(C.Structure): _fields_ = [('size', C.c_uint64), ('layer_size', C.c_uint64), ('align', C.c_uint32), ('layers', C.c_uint32), ('bpb', C.c_uint32), ('tileH', C.c_uint), ('tileD', C.c_uint)]

def lib():
    # 同 romfs_art：允许用本仓源码预编的库，Windows 不必另装系统 clang。
    # As in romfs_art: allow a library built from this tree, without system clang on Windows.
    p = os.environ.get('DM_DESWIZZLE_LIB')
    win = ['-Wl,/EXPORT:nvn_tex_layout', '-Wl,/EXPORT:nvn_deswizzle', '-Wl,/EXPORT:nvn_swizzle'] if os.name == 'nt' else ['-fPIC']   # 同 romfs_art.py | same as romfs_art.py
    if not p:
        t = tempfile.mkdtemp(); p = os.path.join(t, 'bl.dll' if os.name == 'nt' else 'bl.dylib')
        subprocess.run(['clang', '-O2', '-shared', *win, '-I' + os.path.join(DM, 'gfx', 'nvn'), '-I' + os.path.join(DM, 'gfx', 'tex'), '-o', p,
                        os.path.join(DM, 'gfx', 'tex', 'nvn_deswizzle.c'), os.path.join(DM, 'gfx', 'nvn', 'nvn_tex.c')], check=True)
    L = C.CDLL(p); L.nvn_tex_layout.argtypes = [C.POINTER(Desc), C.POINTER(Layout)]
    L.nvn_deswizzle.argtypes = [C.POINTER(Desc), C.c_uint, C.c_uint, C.c_void_p, C.c_void_p, C.c_size_t]
    L.nvn_swizzle.argtypes = [C.POINTER(Desc), C.c_uint, C.c_uint, C.c_void_p, C.c_size_t, C.c_void_p]
    return L

def desc(b, w, h): return Desc(1, 0x44, 0, 0, w, h, 1, 1, 0, b[23] + 1)   # 0x44 = DXT5；块高参数取原图头第 23 字节（同解码处） | 0x44 = DXT5; the block height parameter is taken from byte 23 of the original image's header (same as at the decode site)

def pack(L, hdr0, lin, w, h, fw, frames=1):
    """线性 DXT5 块 → 块线性，套原图头（改显示宽、宽、帧数、数据字节数）。 | Linear DXT5 blocks → block-linear, with the original image's header attached (display width, width, frame count, data byte count changed)."""
    d = desc(hdr0, w, h); lay = Layout(); L.nvn_tex_layout(C.byref(d), C.byref(lay)); dst = C.create_string_buffer(lay.size); src = C.create_string_buffer(lin, len(lin))
    assert L.nvn_swizzle(C.byref(d), 0, 0, src, (w + 3) // 4 * 16, dst) == 0
    hdr = bytearray(hdr0[:40]); struct.pack_into('<HI', hdr, 6, fw, w); struct.pack_into('<I', hdr, 12, h); struct.pack_into('<H', hdr, 20, frames); struct.pack_into('<I', hdr, 0x20, lay.size)
    return bytes(hdr) + dst.raw

def bc3(img):
    """RGBA → 线性 DXT5 块（行优先）。颜色：块内像素投到主轴上取两端；透明度：最大 / 最小 8 级。 | RGBA → linear DXT5 blocks (row-major). Color: pixels in the block are projected onto the principal axis and the two ends taken; alpha: max / min, 8 levels."""
    a = np.asarray(img.convert('RGBA'), dtype=np.float64); h, w = a.shape[:2]; H, W = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    p = np.zeros((H, W, 4)); p[:h, :w] = a; p[h:, :] = p[h - 1:h, :] if h < H else 0; p[:, w:] = p[:, w - 1:w] if w < W else 0
    blk = p.reshape(H // 4, 4, W // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4); rgb, al = blk[:, :, :3], blk[:, :, 3]
    mu = rgb.mean(1, keepdims=True); c = rgb - mu; cov = np.einsum('npi,npj->nij', c, c); v = np.ones((len(blk), 3))
    for _ in range(8): v = np.einsum('nij,nj->ni', cov, v); v /= np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-9)
    t = np.einsum('npi,ni->np', c, v); e0 = rgb[np.arange(len(blk)), t.argmax(1)]; e1 = rgb[np.arange(len(blk)), t.argmin(1)]
    def q565(e): r, g, b_ = (np.round(e[:, 0] * 31 / 255).astype(int), np.round(e[:, 1] * 63 / 255).astype(int), np.round(e[:, 2] * 31 / 255).astype(int)); return r << 11 | g << 5 | b_
    def un565(c_): return np.stack([(c_ >> 11 & 31) * 255 / 31, (c_ >> 5 & 63) * 255 / 63, (c_ & 31) * 255 / 31], 1)
    c0, c1 = q565(e0), q565(e1); dm = c0 < c1; c0, c1 = np.where(dm, c1, c0), np.where(dm, c0, c1)
    p0, p1 = un565(c0), un565(c1); pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    ci = ((rgb[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1).argmin(2); ci[c0 == c1] = 0
    a0, a1 = al.max(1).round().astype(int), al.min(1).round().astype(int)
    ap = np.stack([a0, a1] + [((7 - k) * a0 + k * a1) / 7 for k in range(1, 7)], 1); ai = np.abs(al[:, :, None] - ap[:, None, :]).argmin(2); ai[a0 == a1] = 0
    out = bytearray()
    for n in range(len(blk)):
        ab = sum(int(ai[n, k]) << (3 * k) for k in range(16)); cb = sum(int(ci[n, k]) << (2 * k) for k in range(16))
        out += struct.pack('<BB', a0[n], a1[n]) + ab.to_bytes(6, 'little') + struct.pack('<HHI', c0[n], c1[n], cb)
    return bytes(out)

def decode(L, b):
    """原包 v61 精灵图 → RGBA（解块线性后套 DDS 头交给 PIL 解 DXT5）。 | Original package v61 sprite → RGBA (after un-block-linearizing, a DDS header is attached and PIL decodes the DXT5)."""
    w, h = struct.unpack_from('<II', b, 8); d = desc(b, w, h); pitch = (w + 3) // 4 * 16; lin = C.create_string_buffer(pitch * ((h + 3) // 4)); src = C.create_string_buffer(b[40:], len(b) - 40)
    assert L.nvn_deswizzle(C.byref(d), 0, 0, src, lin, pitch) == 0
    hdr = [124, 0x81007, h, w, len(lin), 0, 1] + [0] * 11 + [32, 4, struct.unpack('<I', b'DXT5')[0], 0, 0, 0, 0, 0] + [0x1000, 0, 0, 0, 0]
    return Image.open(io.BytesIO(b'DDS ' + struct.pack('<31I', *hdr) + lin.raw)).convert('RGBA')
