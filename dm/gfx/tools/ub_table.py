#!/usr/bin/env python3
# 同进程交替测量（DM_NVN_UB，nvn_mtl.m）出表：读宿主日志里的「UB 上屏 相号 名 帧 N 均 X ms 2拍 a 3拍 b 4拍以上 c 丢弃 d 翻译每帧 CPU y ms …」行，
# 每个变体相与它前后相邻的基准相配对（偶数相 = 基准）——配对差抵消温控 / 频率随时间的漂移——按变体出：相数、平均上屏间隔、对基准的差（中位数 + 自助法 95% 区间）、
# 慢帧占比（3 拍及以上，即 > 20.8 ms；2 拍 = 正常的 16.7 ms 帧）、翻译线程每帧 CPU。区间整个在 0 的一侧才算有差别。末行 RESULT {json}。
# 用法：ub_table.py <宿主日志> [跳过前几相=1]   （第一相常含热身，缺省丢掉）
# Table for the same-process alternating measurement (DM_NVN_UB, nvn_mtl.m): reads the host log's "UB 上屏 <phase> <name> 帧 N 均 X ms 2拍 a 3拍 b 4拍以上 c 丢弃 d 翻译每帧 CPU y ms ..." lines,
# pairs each variant phase with its adjacent baseline phases before and after (even phases = baseline) -- the paired difference cancels thermal / clock drift over time -- and for each variant prints: phase count, mean present interval, difference to the baseline (median + bootstrap 95% interval),
# share of slow frames (3 display ticks or more, i.e. > 20.8 ms; 2 ticks = a normal 16.7 ms frame), and translation-thread CPU per frame. A difference counts only when the whole interval is on one side of 0. Last line RESULT {json}.
# Usage: ub_table.py <host log> [phases to skip at the start=1]   (the first phase often includes warm-up, dropped by default)
import json
import random
import re
import statistics
import sys

LINE = re.compile(r'UB 上屏 (\d+) (\S+) 帧 (\d+) 均 ([\d.]+) ms 2拍 (\d+) 3拍 (\d+) 4拍以上 (\d+) 丢弃 (\d+) 翻译每帧 CPU ([\d.]+) ms')


def parse(text):
    out = []
    for m in LINE.finditer(text):
        ph, name, n, mean, c2, c3, c4, drop, cpu = m.groups()
        n = int(n)
        out.append(dict(phase=int(ph), name=name, frames=n, mean_ms=float(mean), slow=(int(c3) + int(c4)) / n if n else 0.0, drop=int(drop), cpu_ms=float(cpu)))
    return out


def ci95(xs, rounds=2000, seed=1):   # 中位数的自助法 95% 区间（固定种子，结果可重复） | bootstrap 95% interval of the median (fixed seed, reproducible)
    if len(xs) < 2: return (xs[0], xs[0]) if xs else (0.0, 0.0)
    rnd = random.Random(seed)
    meds = sorted(statistics.median(rnd.choices(xs, k=len(xs))) for _ in range(rounds))
    return meds[int(0.025 * rounds)], meds[int(0.975 * rounds) - 1]


def table(phases):
    by = {p['phase']: p for p in phases}
    res = {}
    for p in phases:
        if p['name'] == 'base': continue
        nb = [by[q] for q in (p['phase'] - 1, p['phase'] + 1) if q in by and by[q]['name'] == 'base']
        if not nb: continue
        r = res.setdefault(p['name'], dict(phases=0, mean=[], diff=[], slow=[], cpu=[], dcpu=[]))
        r['phases'] += 1; r['mean'].append(p['mean_ms']); r['slow'].append(p['slow']); r['cpu'].append(p['cpu_ms'])
        r['diff'].append(p['mean_ms'] - statistics.fmean(b['mean_ms'] for b in nb))
        r['dcpu'].append(p['cpu_ms'] - statistics.fmean(b['cpu_ms'] for b in nb))
    base = [p for p in phases if p['name'] == 'base']
    return base, res


def main(argv):
    if len(argv) < 2: sys.exit('用法 | usage: ub_table.py <host log> [phases to skip=1]')
    skip = int(argv[2]) if len(argv) > 2 else 1
    phases = [p for p in parse(open(argv[1], encoding='utf-8', errors='replace').read()) if p['phase'] >= skip]
    if not phases: print('UB 没有「UB 上屏」行 | no "UB 上屏" lines'); print('RESULT ' + json.dumps(dict(name='ub_table', verdict='EMPTY', variants=[]))); return 1
    base, res = table(phases)
    print(f"基准 | base: {len(base)} 相 phases，上屏间隔 | present interval {statistics.fmean(b['mean_ms'] for b in base):.3f} ms，慢帧 | slow frames {100 * statistics.fmean(b['slow'] for b in base):.1f}%" if base else '基准 | base: 0')
    print(f"{'变体 | variant':<24} {'相':>3} {'间隔ms':>8} {'差ms':>8} {'95% 区间':>18} {'慢帧':>6} {'CPU差ms':>8}  结论 | verdict")
    variants = []
    for name, r in sorted(res.items()):
        d = statistics.median(r['diff']); lo, hi = ci95(r['diff']); dc = statistics.median(r['dcpu'])
        verdict = '变慢 | slower' if lo > 0 else '变快 | faster' if hi < 0 else '无差别 | no difference'
        print(f"{name:<24} {r['phases']:>3} {statistics.fmean(r['mean']):8.3f} {d:+8.3f} [{lo:+7.3f}, {hi:+7.3f}] {100 * statistics.fmean(r['slow']):5.1f}% {dc:+8.3f}  {verdict}")
        variants.append(dict(name=name, phases=r['phases'], mean_ms=round(statistics.fmean(r['mean']), 4), diff_ms=round(d, 4), ci95_ms=[round(lo, 4), round(hi, 4)],
                             slow_share=round(statistics.fmean(r['slow']), 4), cpu_diff_ms=round(dc, 4), verdict=verdict.split(' | ')[1]))
    print('RESULT ' + json.dumps(dict(name='ub_table', verdict='OK', base_phases=len(base), variants=variants), ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
