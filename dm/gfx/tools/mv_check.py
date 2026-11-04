#!/usr/bin/env python3
# Offline check of the motion vectors (dm/gfx/nvn/nvn_mv.inc). A run with DM_MV=1 DM_MV_DUMP=<first>:<count>:<prefix> writes <prefix><frame>_mv.raw and
# <prefix><frame>_col.raw; this warps each frame's previous picture onto it with the frame's velocities and compares: if the vectors are right, the warped
# picture is closer to the current one than the unwarped previous picture. It only looks at pixels that changed: a wall that stays still says nothing.
#   mv_check.py <prefix> [--box x0,y0,x1,y1] [--png <dir>]    --box: fractions of the content; --png: writes the velocity and error pictures
# The last line is RESULT {json}; exit 0 when the warp is better overall.
import argparse, glob, json, os, re, sys
import numpy as np


def load(path):
    with open(path, 'rb') as f:
        w, h, fmt, row = (int(x) for x in f.readline().split())
        return w, h, fmt, row, f.read()


def rg11b10(d, w, h, row):
    # R11G11B10F to float RGB, or its luma; a coarse decode is enough to compare two frames
    a = np.frombuffer(d, dtype=np.uint32).reshape(h, row // 4)[:, :w]
    def f11(v):
        e = (v >> 6) & 31; m = v & 63
        return np.where(e == 0, m / 64.0 * 2.0 ** -14, (1 + m / 64.0) * 2.0 ** (e.astype(np.int32) - 15))
    def f10(v):
        e = (v >> 5) & 31; m = v & 31
        return np.where(e == 0, m / 32.0 * 2.0 ** -14, (1 + m / 32.0) * 2.0 ** (e.astype(np.int32) - 15))
    return np.stack([f11(a & 0x7ff), f11((a >> 11) & 0x7ff), f10((a >> 22) & 0x3ff)], -1)


def bilinear(img, x, y):
    h, w = img.shape[:2]
    x0 = np.clip(np.floor(x).astype(int), 0, w - 2); y0 = np.clip(np.floor(y).astype(int), 0, h - 2)
    fx = np.clip(x - x0, 0, 1)[..., None]; fy = np.clip(y - y0, 0, 1)[..., None]
    return (img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx) * (1 - fy) + (img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx) * fy


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('prefix'); ap.add_argument('--box'); ap.add_argument('--png'); ap.add_argument('--cw', type=int); ap.add_argument('--ch', type=int)
    a = ap.parse_args()
    frames = sorted(int(re.search(r'(\d+)_mv\.raw$', p).group(1)) for p in glob.glob(a.prefix + '*_mv.raw'))
    rows = []; last = None
    for fr in frames:
        mp = f'{a.prefix}{fr}_mv.raw'; cp = f'{a.prefix}{fr}_col.raw'
        if not os.path.exists(cp): continue
        w, h, _, row, d = load(cp); col = rg11b10(d, w, h, row)
        mw, mh, _, mrow, md = load(mp); vel = np.frombuffer(md, dtype=np.float16).reshape(mh, mrow // 2)[:, :mw * 2].reshape(mh, mw, 2).astype(np.float32)
        cw = a.cw or w; ch = a.ch or h
        if last is not None and last[0] == fr - 1:
            prev = last[1]
            ys, xs = np.mgrid[0:ch, 0:cw]
            x = xs + 0.5 + vel[:ch, :cw, 0] * cw - 0.5; y = ys + 0.5 + vel[:ch, :cw, 1] * ch - 0.5
            warped = bilinear(prev[:ch, :cw], x, y)
            cur = col[:ch, :cw]; base = np.abs(cur - prev[:ch, :cw]).mean(-1); err = np.abs(cur - warped).mean(-1)
            m = np.ones((ch, cw), bool)
            if a.box:
                x0, y0, x1, y1 = (float(v) for v in a.box.split(','))
                m[:] = False; m[int(y0 * ch):int(y1 * ch), int(x0 * cw):int(x1 * cw)] = True
            ch_m = m & (base > np.percentile(base[m], 50))
            sp = np.hypot(vel[:ch, :cw, 0] * cw, vel[:ch, :cw, 1] * ch)
            rows.append(dict(frame=fr, base=float(base[ch_m].mean()), warped=float(err[ch_m].mean()), speed_px_p50=float(np.percentile(sp, 50)), speed_px_p99=float(np.percentile(sp, 99)), bad=int(np.isnan(vel).sum())))
            if a.png:
                from PIL import Image
                os.makedirs(a.png, exist_ok=True)
                s = max(float(np.percentile(sp, 99.5)), 0.5)
                v8 = np.clip(np.stack([vel[:ch, :cw, 0] * cw / s * 0.5 + 0.5, vel[:ch, :cw, 1] * ch / s * 0.5 + 0.5, np.full((ch, cw), 0.5)], -1) * 255, 0, 255).astype(np.uint8)
                Image.fromarray(v8).save(f'{a.png}/vel_{fr}.png')
                tm = lambda c: np.clip(c / (1 + c) * 1.6, 0, 1) ** (1 / 2.2)
                Image.fromarray((tm(cur) * 255).astype(np.uint8)).save(f'{a.png}/col_{fr}.png')
                k = max(float(np.percentile(base, 99)), 1e-4)
                e8 = np.clip(np.concatenate([base, err], 1) / k * 255, 0, 255).astype(np.uint8)
                Image.fromarray(e8).save(f'{a.png}/err_{fr}.png')
        last = (fr, col)
    if not rows:
        print('RESULT ' + json.dumps(dict(ok=False, why='no pair of consecutive frames'))); return 2
    for r in rows: print(f"frame {r['frame']}: no warp {r['base']:.5f}  warped {r['warped']:.5f}  ({r['warped'] / max(r['base'], 1e-9):.2f}x)  speed p50 {r['speed_px_p50']:.2f} p99 {r['speed_px_p99']:.2f} px  bad {r['bad']}")
    b = float(np.mean([r['base'] for r in rows])); w = float(np.mean([r['warped'] for r in rows])); ok = w < b and not any(r['bad'] for r in rows)
    print('RESULT ' + json.dumps(dict(ok=ok, frames=len(rows), base=round(b, 6), warped=round(w, 6), ratio=round(w / max(b, 1e-9), 3))))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
