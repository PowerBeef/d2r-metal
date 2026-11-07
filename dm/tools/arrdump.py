#!/usr/bin/env python3
"""Read the files DM_NVN_ARRDUMP=<frame>:<dir> writes: the raw blocks of the compressed texture arrays the game copies its terrain materials into.

Usage:
  arrdump.py list <dir>                              one line per array: serial, format, size, levels, layers, bytes
  arrdump.py png <file> <layer> <out.png> [--level N]    one layer of one level, decoded
  arrdump.py sheet <file> <out.png> [--level N] [--layers a,b-c] [--cell 128]   a contact sheet, numbered, of those layers (default: all)
  arrdump.py nonzero <file> <layer> [--level N]      the fraction of blocks that aren't all zero, and their bounding box in pixels

The dumps hold the player's own game assets: keep them and anything made from them out of the repository.
"""
import argparse, io, os, re, struct, sys

NAME = re.compile(r'arr_(\d+)_pf(\d+)_(\d+)x(\d+)_m(\d+)_l(\d+)\.bin$')
FORMATS = {130: ('DXT1', 8), 131: ('DXT1', 8), 134: ('DXT5', 16), 135: ('DXT5', 16), 140: ('ATI1', 8), 142: ('ATI2', 16)}   # MTLPixelFormat -> FourCC, bytes a block


def shape(path):
    m = NAME.search(os.path.basename(path))
    if not m: sys.exit(f'arrdump: {path}: not a DM_NVN_ARRDUMP file name')
    serial, pf, w, h, levels, layers = map(int, m.groups())
    if pf not in FORMATS: sys.exit(f'arrdump: {path}: format {pf} is not handled')
    return dict(serial=serial, pf=pf, w=w, h=h, levels=levels, layers=layers, four=FORMATS[pf][0], block=FORMATS[pf][1])


def level_size(s, level):
    w, h = max(s['w'] >> level, 1), max(s['h'] >> level, 1)
    return w, h, (w + 3) // 4 * ((h + 3) // 4) * s['block']


def read_layer(path, s, layer, level):
    if not 0 <= level < s['levels'] or not 0 <= layer < s['layers']: sys.exit(f'arrdump: level {level} layer {layer} is outside {s["levels"]} levels, {s["layers"]} layers')
    off = sum(level_size(s, i)[2] * s['layers'] for i in range(level))
    w, h, n = level_size(s, level)
    with open(path, 'rb') as f:
        f.seek(off + layer * n)
        return w, h, f.read(n)


def decode(s, w, h, blocks):
    from PIL import Image
    head = b'DDS ' + struct.pack('<7I44x', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(blocks), 0, 1)
    head += struct.pack('<2I4s5I', 32, 4, s['four'].encode(), 0, 0, 0, 0, 0) + struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    return Image.open(io.BytesIO(head + blocks)).convert('RGB')


def layer_list(text, count):
    if not text: return list(range(count))
    out = []
    for part in text.split(','):
        a, _, b = part.partition('-')
        out += range(int(a), int(b or a) + 1)
    return out


def nonzero(s, w, h, blocks):
    n, bw, bh = 0, (w + 3) // 4, (h + 3) // 4
    x0, y0, x1, y1 = bw, bh, -1, -1
    for y in range(bh):
        for x in range(bw):
            if any(blocks[(y * bw + x) * s['block']:(y * bw + x + 1) * s['block']]):
                n += 1; x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return n / (bw * bh), (x0 * 4, y0 * 4, (x1 + 1) * 4, (y1 + 1) * 4) if n else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('op', choices=('list', 'png', 'sheet', 'nonzero')); ap.add_argument('args', nargs='+')
    ap.add_argument('--level', type=int, default=0); ap.add_argument('--layers', default=''); ap.add_argument('--cell', type=int, default=128)
    a = ap.parse_args()
    if a.op == 'list':
        for n in sorted(os.listdir(a.args[0])):
            if NAME.search(n):
                s = shape(n); print(f"{s['serial']}\tpf{s['pf']}\t{s['w']}x{s['h']}\t{s['levels']} levels\t{s['layers']} layers\t{os.path.getsize(os.path.join(a.args[0], n))} bytes\t{n}")
        return
    s = shape(a.args[0])
    if a.op == 'png':
        w, h, b = read_layer(a.args[0], s, int(a.args[1]), a.level); decode(s, w, h, b).save(a.args[2]); return
    if a.op == 'nonzero':
        w, h, b = read_layer(a.args[0], s, int(a.args[1]), a.level); f, box = nonzero(s, w, h, b); print(f'layer {a.args[1]} level {a.level}: {w}x{h}, {f:.3f} of blocks not zero, box {box}'); return
    from PIL import Image, ImageDraw
    ids = layer_list(a.layers, s['layers']); cols = max(1, min(16, len(ids))); rows = (len(ids) + cols - 1) // cols
    out = Image.new('RGB', (cols * a.cell, rows * a.cell), (40, 0, 40)); d = ImageDraw.Draw(out)
    for i, l in enumerate(ids):
        w, h, b = read_layer(a.args[0], s, l, a.level); x, y = i % cols * a.cell, i // cols * a.cell
        out.paste(decode(s, w, h, b).resize((a.cell, a.cell)), (x, y)); d.text((x + 3, y + 2), str(l), fill=(255, 255, 0))
    out.save(a.args[1])


if __name__ == '__main__':
    main()
