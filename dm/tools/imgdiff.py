#!/usr/bin/env python3
# 两张截图的感知差异（LDR-FLIP，Andersson 等 2020「FLIP: A Difference Evaluator for Alternating Images」）：按人眼在给定观看距离下能看出的差别打分，0 = 看不出、1 = 最大。
# 颜色一路：sRGB → YCxCz，按三个对立通道的对比敏感度做空间滤波，转回 Lab 做 Hunt 调整，取 HyAB 距离并压缩；特征一路：亮度上的边缘 / 点特征差；两路逐像素合成。
# 照 NVIDIA 参考实现的公式与常数写（numpy + PIL，没用 scipy），不与参考实现逐位相同；用来比同一处画面的两张截图（A/B、改动前后），不比游戏真值图（不入库）。
# 用法：imgdiff.py <参照图> <被测图> [--ppd 67] [--max-mean 阈值] [--map 误差图.png]
#   打平均 / 中位 / p95 / 最大 FLIP；给了 --max-mean 时按平均值判 IMGDIFF OK|RED（退出码 0 / 1）；末行 RESULT {json}
# Perceptual difference of two screenshots (LDR-FLIP, Andersson et al. 2020, "FLIP: A Difference Evaluator for Alternating Images"): scores what the eye can tell apart at a given viewing distance, 0 = indistinguishable, 1 = maximum.
# Color pipeline: sRGB -> YCxCz, spatially filtered with the contrast sensitivity of the three opponent channels, back to Lab with the Hunt adjustment, HyAB distance, compressed; feature pipeline: edge / point feature differences on luminance; combined per pixel.
# Written from the formulas and constants of NVIDIA's reference implementation (numpy + PIL, no scipy), not bit-identical to it; for comparing two screenshots of the same view (A/B, before / after a change), not game ground-truth images (not committed).
# Usage: imgdiff.py <reference> <test> [--ppd 67] [--max-mean threshold] [--map error_map.png]
#   Prints mean / median / p95 / max FLIP; with --max-mean, judges IMGDIFF OK|RED by the mean (exit code 0 / 1); last line RESULT {json}
import argparse
import json
import math
import sys

import numpy as np
from PIL import Image

QC, QF, PC, PT = 0.7, 0.5, 0.4, 0.95
REF_WHITE = None


def srgb2lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


M_RGB2XYZ = np.array([[10135552 / 24577794, 8788810 / 24577794, 4435075 / 24577794],
                      [2613072 / 12288897, 8788810 / 12288897, 887015 / 12288897],
                      [1425312 / 73733382, 8788810 / 73733382, 70074185 / 73733382]])
M_XYZ2RGB = np.linalg.inv(M_RGB2XYZ)
WHITE = M_RGB2XYZ @ np.ones(3)   # D65 参照白 | D65 reference white


def lin2xyz(img): return np.einsum('ij,jhw->ihw', M_RGB2XYZ, img)
def xyz2lin(img): return np.einsum('ij,jhw->ihw', M_XYZ2RGB, img)


def xyz2ycxcz(xyz):
    x, y, z = (xyz[i] / WHITE[i] for i in range(3))
    return np.stack([116 * y - 16, 500 * (x - y), 200 * (y - z)])


def ycxcz2xyz(ycc):
    y = (ycc[0] + 16) / 116
    return np.stack([(ycc[1] / 500 + y) * WHITE[0], y * WHITE[1], (y - ycc[2] / 200) * WHITE[2]])


def xyz2lab(xyz):
    d = 6 / 29
    def f(t): return np.where(t > d ** 3, np.cbrt(t), t / (3 * d * d) + 4 / 29)
    fx, fy, fz = (f(xyz[i] / WHITE[i]) for i in range(3))
    return np.stack([116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)])


def conv(img, k):
    """二维卷积（对称边界填充，同尺寸输出；FFT）。 | 2-D convolution (symmetric boundary padding, same-size output; FFT)."""
    r = k.shape[0] // 2
    p = np.pad(img, r, mode='symmetric')
    h, w = p.shape
    fk = np.fft.rfft2(k, s=(h, w))
    out = np.fft.irfft2(np.fft.rfft2(p) * fk, s=(h, w))
    return out[2 * r:2 * r + img.shape[0], 2 * r:2 * r + img.shape[1]]


def csf_kernels(ppd):
    """三个对立通道（亮度 / 红绿 / 蓝黄）的对比敏感度空间滤波核。 | Contrast-sensitivity spatial filters of the three opponent channels (achromatic / red-green / blue-yellow)."""
    params = {'A': (1, 0.0047, 0, 1e-5), 'RG': (1, 0.0053, 0, 1e-5), 'BY': (34.1, 0.04, 13.5, 0.025)}
    rmax = math.ceil(3 * math.sqrt(0.04 / (2 * math.pi ** 2)) * ppd)
    ax = np.arange(-rmax, rmax + 1) / ppd
    xx, yy = np.meshgrid(ax, ax); z = xx ** 2 + yy ** 2
    out = []
    for a1, b1, a2, b2 in params.values():
        g = a1 * math.sqrt(math.pi / b1) * np.exp(-math.pi ** 2 * z / b1) + a2 * math.sqrt(math.pi / b2) * np.exp(-math.pi ** 2 * z / b2)
        out.append(g / g.sum())
    return out


def feature_kernels(ppd):
    sd = 0.5 * 0.082 * ppd; r = math.ceil(3 * sd)
    x, y = np.meshgrid(np.arange(-r, r + 1), np.arange(-r, r + 1))
    g = np.exp(-(x ** 2 + y ** 2) / (2 * sd * sd))
    ks = {}
    for kind, raw in (("edge", -x * g), ("point", (x ** 2 / (sd * sd) - 1) * g)):
        neg, pos = -raw[raw < 0].sum(), raw[raw > 0].sum()
        gx = np.where(raw < 0, raw / neg, raw / pos)
        ks[kind] = (gx, gx.T)
    return ks


def hunt(lab):
    return np.stack([lab[0], 0.01 * lab[0] * lab[1], 0.01 * lab[0] * lab[2]])


def hyab(a, b):
    return np.abs(a[0] - b[0]) + np.sqrt((a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def flip(ref, test, ppd=67.0):
    """ref / test：H×W×3、0–1 的 sRGB；返回 H×W 的 FLIP 误差图。 | ref / test: H×W×3 sRGB in 0-1; returns the H×W FLIP error map."""
    ref, test = (np.moveaxis(srgb2lin(np.clip(i, 0, 1)), -1, 0) for i in (ref, test))
    yr, yt = xyz2ycxcz(lin2xyz(ref)), xyz2ycxcz(lin2xyz(test))
    ks = csf_kernels(ppd)
    def filtered_lab(ycc):
        f = np.stack([conv(ycc[i], ks[i]) for i in range(3)])
        lin = np.clip(xyz2lin(ycxcz2xyz(f)), 0, 1)
        return hunt(xyz2lab(lin2xyz(lin)))
    de = hyab(filtered_lab(yr), filtered_lab(yt)) ** QC
    green, blue = (np.array(c, float).reshape(3, 1, 1) for c in ((0, 1, 0), (0, 0, 1)))
    cmax = float(hyab(hunt(xyz2lab(lin2xyz(green))), hunt(xyz2lab(lin2xyz(blue))))[0, 0]) ** QC
    pcc = PC * cmax
    dec = np.where(de < pcc, PT / pcc * de, PT + (de - pcc) / (cmax - pcc) * (1 - PT))
    fk = feature_kernels(ppd)
    def feats(ycc, kind):
        lum = (ycc[0] + 16) / 116
        kx, ky = fk[kind]
        return np.hypot(conv(lum, kx), conv(lum, ky))
    df = np.maximum(np.abs(feats(yr, 'edge') - feats(yt, 'edge')), np.abs(feats(yr, 'point') - feats(yt, 'point')))
    df = (df / math.sqrt(2)) ** QF
    return np.clip(dec, 0, 1) ** (1 - np.clip(df, 0, 1))


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255.0


def main(argv):
    ap = argparse.ArgumentParser(prog='imgdiff.py', description='Perceptual difference (LDR-FLIP) of two screenshots.')
    ap.add_argument('ref'); ap.add_argument('test'); ap.add_argument('--ppd', type=float, default=67.0, help='pixels per degree of visual angle (67 = a 0.7 m view of a 24-inch 4K monitor)')
    ap.add_argument('--max-mean', type=float); ap.add_argument('--map', help='write the error map as a grayscale PNG')
    a = ap.parse_args(argv)
    r, t = load(a.ref), load(a.test)
    if r.shape != t.shape: print(f'IMGDIFF RED 尺寸不同 | sizes differ: {r.shape[1]}x{r.shape[0]} vs {t.shape[1]}x{t.shape[0]}'); return 1
    m = flip(r, t, a.ppd)
    st = dict(mean=float(m.mean()), median=float(np.median(m)), p95=float(np.percentile(m, 95)), max=float(m.max()))
    if a.map: Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8), 'L').save(a.map)
    print('FLIP ' + '  '.join(f'{k} {v:.4f}' for k, v in st.items()) + f'  ({r.shape[1]}x{r.shape[0]}, ppd {a.ppd:g})')
    verdict = None if a.max_mean is None else 'OK' if st['mean'] <= a.max_mean else 'RED'
    if verdict: print(f"IMGDIFF {verdict} mean {st['mean']:.4f} {'≤' if verdict == 'OK' else '>'} {a.max_mean:g}")
    print('RESULT ' + json.dumps(dict(name='imgdiff', verdict=verdict or 'INFO', ppd=a.ppd, **{k: round(v, 6) for k, v in st.items()})))
    return 1 if verdict == 'RED' else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
