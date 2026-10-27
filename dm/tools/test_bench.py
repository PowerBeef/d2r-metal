"""bench.py 的单测：统计（Mann-Whitney U 精确 / 近似、自助法区间、段统计）与按 mark 切段，用合成数据，不跑游戏。 | Unit tests for bench.py: statistics (exact / approximate Mann-Whitney U, bootstrap interval, segment statistics) and cutting segments by mark, on synthetic data; the game is not run."""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402


class BenchTest(unittest.TestCase):
    def test_mann_whitney_exact(self):
        u, p = bench.mann_whitney([1, 2, 3, 4], [5, 6, 7, 8])   # 完全分开：双侧精确 p = 2/C(8,4) = 2/70 | fully separated: exact two-sided p = 2/C(8,4) = 2/70
        self.assertEqual(u, 0); self.assertAlmostEqual(p, 2 / 70)
        self.assertAlmostEqual(bench.mann_whitney([1, 2, 3], [1, 2, 3])[1], 1.0)
        self.assertGreater(bench.mann_whitney([1, 3, 5, 7], [2, 4, 6, 8])[1], 0.5)
        self.assertAlmostEqual(bench.mann_whitney([5, 5, 5], [5, 5, 5])[1], 1.0)   # 全并列 | all tied

    def test_mann_whitney_normal(self):   # 组合数大于 20000 走正态近似；明显分开时 p 很小 | over 20000 combinations uses the normal approximation; clearly separated gives a small p
        a = [i * 0.1 for i in range(12)]; b = [5 + i * 0.1 for i in range(12)]
        _, p = bench.mann_whitney(a, b); self.assertLess(p, 0.001)
        _, p = bench.mann_whitney(a, [x + 0.05 for x in a]); self.assertGreater(p, 0.3)

    def test_bootstrap(self):
        d, lo, hi = bench.boot_median_diff([10, 10.2, 9.9, 10.1], [12, 12.1, 11.8, 12.2])
        self.assertAlmostEqual(d, 2.0, places=6); self.assertGreater(lo, 0); self.assertGreaterEqual(hi, lo)
        self.assertEqual(bench.boot_median_diff([1, 2, 3], [1, 2, 3]), bench.boot_median_diff([1, 2, 3], [1, 2, 3]))   # 固定种子 | fixed seed

    def test_seg_stats(self):
        s = bench.seg_stats([16.7] * 90 + [33.4] * 10 + [0, -1, -1])
        self.assertEqual((s['frames'], s['dropped'], s['slow_share']), (100, 2, 0.1))
        self.assertAlmostEqual(s['mean_ms'], 18.37, places=2); self.assertEqual(s['p50_ms'], 16.7)
        self.assertEqual(bench.seg_stats([0, 0])['frames'], 0)

    def test_segments_and_metric(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            ev = [dict(seq=1, t=1.0, ev='mark', name='town', frame=100), dict(seq=2, t=2.0, ev='mark', name='wild', frame=160), dict(seq=3, t=3.0, ev='mark', name='end', frame=220)]
            (p / 'events.ndjson').write_text(''.join(json.dumps(e) + '\n' for e in ev), encoding='utf-8')
            (p / 'frames.tsv').write_text('t\tiv_ms\n' + ''.join(f'{1 + i / 60:.4f}\t{16.667 if i < 60 else 33.3}\n' for i in range(120)), encoding='utf-8')
            rows = ['frame\tname\tvalue'] + [f'{f}\t@gpu_ms\t{5.0 if f < 160 else 7.0}' for f in range(100, 220)] + [f'{f}\t@cpu_ms\t2.0' for f in range(100, 220)] + ['150\tnvnDrawElements\t12']
            (p / 'nvn.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
            seg = bench.segments(str(p))
        self.assertEqual(sorted(seg), ['town', 'wild'])
        self.assertEqual((seg['town']['frames'], seg['town']['gpu_ms']['p50'], seg['wild']['gpu_ms']['p50']), (60, 5.0, 7.0))
        self.assertEqual((seg['town']['present']['frames'], seg['wild']['present']['p50_ms']), (60, 33.3))
        self.assertEqual(bench.metric(seg['wild'], 'gpu_ms.p50'), 7.0); self.assertEqual(bench.metric(seg['town'], 'present.slow_share'), 0.0)
        self.assertIsNone(bench.metric(seg['town'], 'rec_ms.p95'))
        summ = bench.summarize([dict(ok=True, segments=seg), dict(ok=True, segments=seg), dict(ok=False, segments={})])
        self.assertEqual((summ['town']['runs'], summ['wild']['gpu_ms.p50']), (2, 7.0))

    def test_attribute_segments_and_hitches(self):   # 合成的采样日志 + 逐帧计时：按段按线程角色出在跑占比与热点，长帧只列它那段时间里在跑的函数 | synthetic samples + per-frame timing: running share and hotspots per segment and thread role; a long frame lists only what ran during it
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            ev = [dict(seq=1, t=1.0, ev='mark', name='seg', frame=10), dict(seq=2, t=3.0, ev='mark', name='end', frame=70)]
            (p / 'events.ndjson').write_text(''.join(json.dumps(e) + '\n' for e in ev), encoding='utf-8')
            log = ['[dm] SAMPLE-THR 0x1001 主线程', '[dm] SAMPLE-THR 0x1005 Thread_0x0000000100000000', '[dm] SAMPLE-THR 0x1007 AIO Pool Thread']
            for i in range(20):
                t = 1.0 + i * 0.1
                log += [f'[dm] SAMPLE #{3 * i + 1} [{t:.3f}s] thr=0x1001 pc=main+0x100', '[dm] bt: main+0x200 dm_thread_enter+0x18']
                log += [f'[dm] SAMPLE #{3 * i + 2} [{t:.3f}s] thr=0x1005 pc=__psynch_cvwait+0x8 lr=x', '[dm] bt: sdk+0x10 main+0x300']
                log += [f'[dm] SAMPLE #{3 * i + 3} [{t:.3f}s] thr=0x1007 pc={"main+0x400" if 2.0 <= t < 2.1 else "__semwait_signal+0x8"}', '[dm] bt: main+0x500']
            (p / 'host.log').write_text('\n'.join(log) + '\n', encoding='utf-8')
            rows = ['frame\tname\tvalue'] + [f'{f}\t@frame_ms\t{80.0 if f == 40 else 16.7}' for f in range(10, 70)] + [f'{f}\t@t_s\t{1.0 + (f - 10) / 30 + (0.05 if f == 40 else 0):.3f}' for f in range(10, 70)]
            (p / 'nvn.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
            real = sys.modules.get('guestsyms'); sys.modules['guestsyms'] = types.SimpleNamespace(lookup=lambda a: '')   # 不靠本机的引擎符号表 | independent of the local engine symbol table
            try: prof = bench.attribute(str(p), 25)
            finally:
                if real: sys.modules['guestsyms'] = real
                else: sys.modules.pop('guestsyms', None)
        seg = prof['segments']['seg']
        self.assertEqual((seg['main']['running_pct'], seg['workers']['running_pct'], seg['io']['running_pct']), (100.0, 0.0, 5.0))
        self.assertEqual(seg['main']['hot'][0][1], 20)
        self.assertEqual([h['frame'] for h in prof['hitches']], [40])
        self.assertEqual(sorted(f for f, _ in prof['hitches'][0]['running']), ['main+0x100', 'main+0x400'])   # 那 80 ms 里两条线程在跑 | two threads were running during those 80 ms
        self.assertEqual(prof['threads']['0x1007'], 'AIO Pool Thread')
        self.assertEqual((bench.role('主线程'), bench.role('Thread_0x01'), bench.role('FMOD mixer thread'), bench.role('BGS Thread')), ('main', 'workers', 'audio', 'other'))


if __name__ == '__main__':
    unittest.main()
