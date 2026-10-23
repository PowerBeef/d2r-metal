#!/usr/bin/env python3
# 浸泡报告（soak.sh 末尾调；Mac 浸泡同口径）：读 DM_SOAK 表（diag.c soak_main：秒 拍号 句柄 存活线程 线程对象 malloc_MiB footprint_MiB）与同一局的宿主日志，
# 按四项判据出 SOAK OK|RED，末行 RESULT {json} 给脚本 / 代理读。阈值都可用环境变量改：
#   ① 拍号增长：局内（拍号 ≥ 0）各采样点拍号严格递增，平均 ≥ SOAK_MIN_TPS 拍/秒（缺省 20；游戏 25 拍/秒）
#   ② 内存斜率：第 SOAK_WARM_S 秒（缺省 300）之后局内 footprint 对时间的最小二乘斜率 ≤ SOAK_MAX_SLOPE MiB/分（缺省 1.0）
#   ③ 句柄 / 线程：预热后句柄数极差 ≤ SOAK_MAX_HANDLES（缺省 8）、存活线程极差 ≤ SOAK_MAX_THREADS（缺省 2）
#   ④ 帧时间 p95：宿主日志「计时 至帧 N： 帧时间 … p95 X」各段分前后两半，后半中位数 ≤ 前半中位数 × (1 + SOAK_P95_REL) + SOAK_P95_ABS ms（缺省 0.15、0.5）
#   数据不够（预热后局内采样少于 3 点、计时段少于 4 段）该项判红并写明原因
# 用法：soak_report.py <soak.tsv> <宿主日志>   退出码 0 = SOAK OK，1 = SOAK RED，2 = 用法 / 读不了文件
# Soak report (called at the end of soak.sh; same basis as the Mac soak): reads the DM_SOAK table (diag.c soak_main: seconds, tick, handles, live threads, thread objects, malloc_MiB, footprint_MiB) and the host log of the same run,
# and prints SOAK OK|RED by four criteria, with a last line RESULT {json} for scripts / agents to read. Every threshold can be changed with an environment variable:
#   1 ticks advance: in a run (tick >= 0) the tick number strictly increases between samples, on average >= SOAK_MIN_TPS ticks/s (default 20; the game runs 25 ticks/s)
#   2 memory slope: after second SOAK_WARM_S (default 300), the least-squares slope of the in-run footprint over time <= SOAK_MAX_SLOPE MiB/min (default 1.0)
#   3 handles / threads: after warm-up, the range of the handle count <= SOAK_MAX_HANDLES (default 8) and of live threads <= SOAK_MAX_THREADS (default 2)
#   4 frame time p95: the host log's "计时 至帧 N： 帧时间 ... p95 X" segments are split into a first and second half; the second half's median <= first half's median × (1 + SOAK_P95_REL) + SOAK_P95_ABS ms (defaults 0.15, 0.5)
#   Not enough data (fewer than 3 in-run samples after warm-up, fewer than 4 timing segments) makes that item red with the reason
# Usage: soak_report.py <soak.tsv> <host log>   exit code 0 = SOAK OK, 1 = SOAK RED, 2 = usage / unreadable file
import json
import os
import re
import statistics
import sys


def env_f(name, default):
    try: return float(os.environ.get(name) or str(default))
    except ValueError: return float(default)


def slope(xs, ys):   # 最小二乘斜率 | least-squares slope
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True)) / den if den else 0.0


def report(tsv, log):
    rows = []
    for line in open(tsv, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 7 or not re.match(r'^\d', f[0]): continue   # 表头（中文列名）与残行不算 | the header (Chinese column names) and partial lines do not count
        rows.append(dict(t=float(f[0]), tick=int(f[1]), handles=int(f[2]), threads=int(f[3]), objs=int(f[4]), malloc=float(f[5]), fp=float(f[6])))
    text = open(log, encoding='utf-8', errors='replace').read()
    p95 = [float(m.group(1)) for m in re.finditer(r'计时 至帧 \d+： 帧时间 p50 [\d.]+ / p95 ([\d.]+)', text)]
    warm = env_f('SOAK_WARM_S', 300)
    game = [r for r in rows if r['tick'] >= 0]
    late = [r for r in game if r['t'] >= warm]
    checks = []

    def check(key, ok, zh, en, **data):
        checks.append(dict(name=key, ok=bool(ok), detail=en, **data))
        print(f"{'✓' if ok else '✗'} {zh} | {en}")

    # ① 拍号增长 | 1 ticks advance
    min_tps = env_f('SOAK_MIN_TPS', 20)
    if len(game) < 2:
        check('ticks', False, f'① 拍号增长：局内采样只有 {len(game)} 点，判不了', f'1 ticks advance: only {len(game)} in-run samples, cannot judge')
    else:
        mono = all(b['tick'] > a['tick'] for a, b in zip(game, game[1:], strict=False))
        tps = (game[-1]['tick'] - game[0]['tick']) / max(1e-9, game[-1]['t'] - game[0]['t'])
        check('ticks', mono and tps >= min_tps, f'① 拍号增长：{"严格递增" if mono else "有停滞 / 倒退"}，平均 {tps:.1f} 拍/秒（下限 {min_tps:g}）',
              f'1 ticks advance: {"strictly increasing" if mono else "stalled / went back"}, {tps:.1f} ticks/s on average (minimum {min_tps:g})', ticks_per_s=round(tps, 2), monotonic=mono)
    # ② 内存斜率 | 2 memory slope
    max_slope = env_f('SOAK_MAX_SLOPE', 1.0)
    if len(late) < 3:
        check('memory', False, f'② 内存斜率：第 {warm:g} 秒后局内采样只有 {len(late)} 点（要 ≥ 3）', f'2 memory slope: only {len(late)} in-run samples after {warm:g} s (need >= 3)')
    else:
        s_fp = slope([r['t'] / 60 for r in late], [r['fp'] for r in late]); s_ml = slope([r['t'] / 60 for r in late], [r['malloc'] for r in late])
        check('memory', s_fp <= max_slope, f'② 内存斜率：footprint {s_fp:+.2f} MiB/分（上限 {max_slope:g}），malloc {s_ml:+.2f} MiB/分',
              f'2 memory slope: footprint {s_fp:+.2f} MiB/min (maximum {max_slope:g}), malloc {s_ml:+.2f} MiB/min', footprint_mib_per_min=round(s_fp, 3), malloc_mib_per_min=round(s_ml, 3))
    # ③ 句柄 / 线程 | 3 handles / threads
    mh, mt = env_f('SOAK_MAX_HANDLES', 8), env_f('SOAK_MAX_THREADS', 2)
    if len(late) < 3:
        check('handles', False, '③ 句柄 / 线程：预热后采样不足', '3 handles / threads: not enough samples after warm-up')
    else:
        dh = max(r['handles'] for r in late) - min(r['handles'] for r in late); dt = max(r['threads'] for r in late) - min(r['threads'] for r in late)
        check('handles', dh <= mh and dt <= mt, f'③ 句柄 / 线程：句柄极差 {dh}（上限 {mh:g}），存活线程极差 {dt}（上限 {mt:g}）',
              f'3 handles / threads: handle range {dh} (maximum {mh:g}), live-thread range {dt} (maximum {mt:g})', handle_range=dh, thread_range=dt)
    # ④ 帧时间 p95 | 4 frame time p95
    rel, ab = env_f('SOAK_P95_REL', 0.15), env_f('SOAK_P95_ABS', 0.5)
    if len(p95) < 4:
        check('p95', False, f'④ 帧时间 p95：日志里只有 {len(p95)} 段计时（要 ≥ 4）', f'4 frame time p95: only {len(p95)} timing segments in the log (need >= 4)')
    else:
        h = len(p95) // 2; a, b = statistics.median(p95[:h]), statistics.median(p95[h:]); lim = a * (1 + rel) + ab
        check('p95', b <= lim, f'④ 帧时间 p95：前半 {a:.2f} ms → 后半 {b:.2f} ms（上限 {lim:.2f}）', f'4 frame time p95: first half {a:.2f} ms -> second half {b:.2f} ms (limit {lim:.2f})',
              first_half_ms=round(a, 3), second_half_ms=round(b, 3), limit_ms=round(lim, 3))
    ok = all(c['ok'] for c in checks)
    print(f"SOAK {'OK' if ok else 'RED'}（{sum(c['ok'] for c in checks)}/{len(checks)} 通过） | {sum(c['ok'] for c in checks)}/{len(checks)} passed")
    print('RESULT ' + json.dumps(dict(name='soak', verdict='OK' if ok else 'RED', samples=len(rows), in_run_samples=len(game), timing_segments=len(p95), checks=checks), ensure_ascii=False))
    return 0 if ok else 1


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit('用法 | usage: soak_report.py <soak.tsv> <host log>')
    try: sys.exit(report(sys.argv[1], sys.argv[2]))
    except OSError as e:
        print(f'SOAK RED 读不了文件 | cannot read file: {e}'); sys.exit(2)
