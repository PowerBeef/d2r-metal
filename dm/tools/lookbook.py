#!/usr/bin/env python3
# 画质对照集：用画面闸门那几页（dm/verify/shotgate_pages 里不要录制输入的页，golden.py 同一套触发、键序与确定时钟）给一个画面变体截一组图，
# 再与「2 倍超采样参照」比：参照 = 同一宿主按 2 倍出图分辨率（DM_GAME_RES）截图、再缩回 1 倍，当作抗锯齿 / 放大 / 纹理过滤要逼近的目标。
#   shoot <名> [--env K=V]… [--res 宽x高] [--dmhost 宿主] [页…]   截一组图到 $DM_ROOT/run/lookbook/<名>/（附 meta.json：环境、宿主、提交号）
#   ref [--scale 2] [--dmhost 宿主] [页…]                          截 2 倍分辨率一组（ref2x），缩回 1 倍存成 ref
#   compare <甲> <乙> [页…] [--stable 名1,名2]                    逐页：乙相对甲的 FLIP（整页与 3D 画面区）、两边的边缘清晰度（亮度拉普拉斯方差）；末行 RESULT {json}
#                                                                  --stable：同一变体截两次里变了的像素（自己会动的东西）不算
# 正对照：同一宿主同一环境截两次应完全相同（compare 报 FLIP 0）；负对照：关掉一个效果（DM_NVN_SKIP_FS=<键>）应在 FLIP 里看得出
# 截图是由游戏程序画出来的画面：只放本机（$DM_ROOT），不入库
# Picture-quality look set: takes one set of screenshots of a rendering variant with the screenshot-gate pages (the pages in dm/verify/shotgate_pages that need no
# recorded input; the same triggers, key scripts and deterministic clock as golden.py), and compares them with a "2x supersampled reference": the same host captured
# at twice the output resolution (DM_GAME_RES) and scaled back to 1x, the target anti-aliasing / upscaling / texture filtering should get close to.
#   shoot <name> [--env K=V]... [--res WxH] [--dmhost host] [page...]   one set into $DM_ROOT/run/lookbook/<name>/ (with meta.json: environment, host, commit)
#   ref [--scale 2] [--dmhost host] [page...]                           one set at 2x resolution (ref2x), scaled back to 1x and kept as ref
#   compare <a> <b> [page...] [--stable n1,n2]                          per page: FLIP of b against a (whole page and the 3D viewport) and the edge sharpness of both
#                                                                       (variance of the luma Laplacian); last line RESULT {json}; --stable ignores the pixels that changed
#                                                                       between two captures of one variant (things that move on their own)
# Positive control: the same host and environment captured twice must be identical (compare reports FLIP 0); negative control: switching one effect off
# (DM_NVN_SKIP_FS=<key>) must show in FLIP
# The screenshots are pictures drawn by the game program: kept on this machine only ($DM_ROOT), never committed
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(DM, 'verify')); sys.path.insert(0, HERE)
import golden   # noqa: E402  # pyright: ignore[reportMissingImports]
import imgdiff  # noqa: E402

OUT = os.path.join(golden.scenario.DM_ROOT, 'run', 'lookbook')
BASE_W, BASE_H = 1600, 900   # 宿主缺省出图分辨率（front_game_res）| the host's default output resolution (front_game_res)
FRONT = ('loading', 'charselect', 'newchar')   # 前端页：没有局内 3D 画面，画质对照不看 | front-end pages: no in-game 3D picture, not part of the look set
LB_PAGES = golden.LB_PAGES   # the look set's own 3D pages and their recipe live in golden.py (its FLIP sets shoot them too)


def pages(names):
    names = list(names or [])
    lb = [p for p in LB_PAGES if not names or p in names]
    gate = [n for n in names if n not in LB_PAGES]
    if names and not gate: return lb   # only look-set pages asked for: the screenshot-gate pages are not listed (nor their unavailable notes)
    return [p for p in golden.shot_pages(gate) if p not in FRONT and (not names or p in names)] + lb


def spec(p):
    return dict(host=LB_PAGES[p], viewport=LB_PAGES[p]['viewport']) if p in LB_PAGES else golden.page_spec(p)


def take_shot(host, p, png, extra=None):
    """One page through golden.take_shot (both the screenshot-gate pages and the look set's own pages); extra wins over the page's own env, so a
    variant can set DM_BRIGHT_K."""
    return golden.take_shot(host, p, png, extra)


def shoot(name, env, res, host, names):
    """One set of a variant: its env (and DM_GAME_RES=res) go to every shot as the caller's env; the process environment is left alone."""
    d = os.path.join(OUT, name); os.makedirs(d, exist_ok=True)
    extra = dict(env, **({'DM_GAME_RES': res} if res else {}))
    bad = 0
    for p in pages(names):
        png = os.path.join(d, p + '.png')
        ok, why = take_shot(host, p, png, extra)
        print(f"LOOKBOOK shot {name}/{p} {'OK' if ok else 'RED ' + why}", flush=True); bad += not ok
    with open(os.path.join(d, 'meta.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(name=name, env=extra, res=res, host=host, sha=golden.head()), f, indent=1)
    return bad


def make_ref(scale, host, names):
    big = f'{BASE_W * scale}x{BASE_H * scale}'
    bad = shoot('ref2x', {}, big, host, names)
    d = os.path.join(OUT, 'ref'); os.makedirs(d, exist_ok=True)
    for p in pages(names):
        src = os.path.join(OUT, 'ref2x', p + '.png')
        if os.path.exists(src): Image.open(src).convert('RGB').resize((BASE_W, BASE_H), Image.Resampling.LANCZOS).save(os.path.join(d, p + '.png'))
    with open(os.path.join(d, 'meta.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(name='ref', from_res=big, scale=scale, host=host, sha=golden.head()), f, indent=1)
    return bad


def sharpness(img, keep=None):
    """亮度拉普拉斯方差（只算 keep 里的像素）：同一处画面里越大越清晰（只用来比同一场景）。 | Variance of the luma Laplacian (only over the pixels in keep):
    larger is crisper within the same scene (only for comparing one scene)."""
    y = img @ np.array([0.2126, 0.7152, 0.0722])
    lap = -4 * y[1:-1, 1:-1] + y[:-2, 1:-1] + y[2:, 1:-1] + y[1:-1, :-2] + y[1:-1, 2:]
    if keep is not None: lap = lap[keep[1:-1, 1:-1]]
    return float(lap.var() * 1e4) if lap.size else 0.0


def crop(img, vp):
    h, w = img.shape[:2]; x0, y0, x1, y1 = vp
    return img[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]


def unstable(s1, s2, p, shape):
    """同一变体截两次（s1 / s2）里变了的像素（会自己动的：小动物、火光闪动、定时文字），外扩 4 像素；比较时不算这些。 | Pixels that changed between two
    captures of the same variant (s1 / s2: things that move on their own -- critters, flicker, timed text), grown by 4 pixels; excluded from comparisons."""
    f1, f2 = os.path.join(OUT, s1, p + '.png'), os.path.join(OUT, s2, p + '.png')
    if not (os.path.exists(f1) and os.path.exists(f2)): return np.zeros(shape[:2], bool)
    return golden.unstable_mask(imgdiff.load(f1), imgdiff.load(f2))


def compare(a, b, names, stable=None):
    rows = []
    for p in pages(names):
        fa, fb = os.path.join(OUT, a, p + '.png'), os.path.join(OUT, b, p + '.png')
        if not (os.path.exists(fa) and os.path.exists(fb)): print(f'⚪ {p}: missing in {a} or {b}'); continue
        ia, ib = imgdiff.load(fa), imgdiff.load(fb)
        if ia.shape != ib.shape: ib = np.asarray(Image.open(fb).convert('RGB').resize((ia.shape[1], ia.shape[0]), Image.Resampling.LANCZOS), dtype=np.float64) / 255.0
        vp = spec(p).get('viewport', [0, 0, 1, 1])
        whole = imgdiff.flip(ia, ib)
        keep = ~unstable(stable[0], stable[1], p, ia.shape) if stable else np.ones(ia.shape[:2], bool)
        view = crop(whole, vp)[crop(keep, vp)]
        if not view.size: view = crop(whole, vp).ravel()
        r = dict(page=p, flip_mean=round(float(whole[keep].mean() if keep.any() else whole.mean()), 5), flip_view_mean=round(float(view.mean()), 5),
                 flip_view_p95=round(float(np.percentile(view, 95)), 5), unstable_share=round(float(1 - keep.mean()), 4),
                 sharp_a=round(sharpness(crop(ia, vp), crop(keep, vp)), 2), sharp_b=round(sharpness(crop(ib, vp), crop(keep, vp)), 2))
        rows.append(r)
        print(f"{p:16s} FLIP whole {r['flip_mean']:.4f}  3D mean {r['flip_view_mean']:.4f} p95 {r['flip_view_p95']:.4f}  sharpness {a} {r['sharp_a']:7.2f} {b} {r['sharp_b']:7.2f}  ignored {r['unstable_share']:.1%}")
    if rows: print(f"LOOKBOOK {a} vs {b}: mean 3D FLIP {np.mean([r['flip_view_mean'] for r in rows]):.4f} over {len(rows)} pages")
    print('RESULT ' + json.dumps(dict(a=a, b=b, pages=rows)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog='lookbook.py', description='Picture-quality look set: capture variants and score them against a 2x supersampled reference.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('shoot'); s.add_argument('name'); s.add_argument('pages', nargs='*'); s.add_argument('--env', action='append', default=[]); s.add_argument('--res'); s.add_argument('--dmhost', default=os.path.join(DM, 'host', 'dmhost'))
    r = sub.add_parser('ref'); r.add_argument('pages', nargs='*'); r.add_argument('--scale', type=int, default=2); r.add_argument('--dmhost', default=os.path.join(DM, 'host', 'dmhost'))
    c = sub.add_parser('compare'); c.add_argument('a'); c.add_argument('b'); c.add_argument('pages', nargs='*')
    c.add_argument('--stable', help='two captures of one variant, "name1,name2": pixels that differ between them are ignored (things that move on their own)')
    a = ap.parse_args(argv)
    if a.cmd == 'shoot': return 1 if shoot(a.name, dict(e.split('=', 1) for e in a.env), a.res, a.dmhost, a.pages) else 0
    if a.cmd == 'ref': return 1 if make_ref(a.scale, a.dmhost, a.pages) else 0
    return compare(a.a, a.b, a.pages, a.stable.split(',') if a.stable else None)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
