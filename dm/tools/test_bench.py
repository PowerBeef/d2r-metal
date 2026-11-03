"""bench.py 的单测：统计（Mann-Whitney U 精确 / 近似、自助法区间、段统计）与按 mark 切段，用合成数据，不跑游戏。 | Unit tests for bench.py: statistics (exact / approximate Mann-Whitney U, bootstrap interval, segment statistics) and cutting segments by mark, on synthetic data; the game is not run."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

ROUTE = str(Path(__file__).resolve().parents[1] / 'verify' / 'bench' / 'town_loop.scn')
CHAIN = 'add73ceeb333'


def fake_tree(root, chain=CHAIN, fixups='v1'):
    """A host, its gfx tables, a rewrite output and a shader library laid out as in the repository and the workspace."""
    r = Path(root)
    for rel, data in (('host/dmhost', b'\x00..' + chain.encode() + b'..\x00'), ('gfx/nvn/shader_map.tsv', b'map\n'), ('product/main.fixups.tsv', fixups.encode()),
                      (f'msl/b-{CHAIN}/x.metallib', b'lib')):
        (r / rel).parent.mkdir(parents=True, exist_ok=True); (r / rel).write_bytes(data)
    return str(r / 'host' / 'dmhost'), str(r / 'product'), str(r / 'msl')


def raise_if_called(*a, **k):
    raise AssertionError('must not be called')


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
            rows = ['frame\tname\tvalue'] + [f'{f}\t@gpu_ms\t{5.0 if f < 160 else 7.0}' for f in range(100, 220)] + [f'{f}\t@cpu_ms\t2.0' for f in range(100, 220)] + [f'{f}\t@proc_cpu_ms\t9.5' for f in range(100, 220)] + [f'{f}\t@pso_new\t{1 if f == 170 else 0}' for f in range(100, 220)] + ['150\tnvnDrawElements\t12']
            (p / 'nvn.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
            seg = bench.segments(str(p))
        self.assertEqual(sorted(seg), ['town', 'wild'])
        self.assertEqual((seg['town']['frames'], seg['town']['gpu_ms']['p50'], seg['wild']['gpu_ms']['p50']), (60, 5.0, 7.0))
        self.assertEqual((seg['town']['present']['frames'], seg['wild']['present']['p50_ms']), (60, 33.3))
        self.assertEqual(bench.metric(seg['wild'], 'gpu_ms.p50'), 7.0); self.assertEqual(bench.metric(seg['town'], 'present.slow_share'), 0.0)
        self.assertIsNone(bench.metric(seg['town'], 'rec_ms.p95'))
        self.assertEqual((bench.metric(seg['town'], 'proc_cpu_ms.p50'), bench.metric(seg['wild'], 'pso_new.p99') > 0), (9.5, True))   # 全进程 CPU 与同步建管线进了汇总 | whole-process CPU and synchronous pipeline builds are summarised
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


    def test_baseline_snapshot(self):   # the snapshot keeps the host, its tables, the rewrite output and the shader library; saving twice into one folder never nests
        with tempfile.TemporaryDirectory() as t:
            host, prod, msl = fake_tree(t); d = os.path.join(t, 'base', 'r1')
            ok, why = bench.snapshot(host, prod, msl, CHAIN, '3.3.94012-r49', d, git={'rev': 'r1', 'dirty': False})
            self.assertTrue(ok, why)
            for rel in ('host/dmhost', 'gfx/nvn/shader_map.tsv', 'product/main.fixups.tsv', f'msl/b-{CHAIN}/x.metallib'): self.assertTrue(os.path.isfile(os.path.join(d, rel)), rel)
            meta = json.load(open(os.path.join(d, 'meta.json'), encoding='utf-8'))
            self.assertEqual((meta['chain'], meta['product'], meta['rev']), (CHAIN, '3.3.94012-r49', 'r1'))
            self.assertEqual(bench.baseline_env(d), {'SCN_PRODUCT': os.path.join(d, 'product'), 'DM_MSL_DIR': os.path.join(d, 'msl')})
            Path(prod, 'main.fixups.tsv').write_text('v2', encoding='utf-8')
            ok, why = bench.snapshot(host, prod, msl, CHAIN, '3.3.94012-r49', d, git={'rev': 'r1', 'dirty': True})
            self.assertTrue(ok, why)
            self.assertEqual(Path(d, 'product', 'main.fixups.tsv').read_text(encoding='utf-8'), 'v2')
            self.assertFalse(os.path.exists(os.path.join(d, 'product', 'product')))
            self.assertFalse(os.path.exists(d + '.new') or os.path.exists(d + '.old'))
            other, _, _ = fake_tree(os.path.join(t, 'other'), chain='0123456789ab')
            before = sorted(os.listdir(d))
            self.assertEqual(bench.snapshot(other, prod, msl, CHAIN, '3.3.94012-r49', d), (False, 'host was built from another shader chain'))
            self.assertEqual(sorted(os.listdir(d)), before)
            self.assertFalse(bench.snapshot(host, prod, os.path.join(t, 'nolib'), CHAIN, '3.3.94012-r49', d)[0])
            os.remove(os.path.join(d, 'meta.json')); self.assertIsNone(bench.baseline_env(d))

    def test_regress_unsnapshotted(self):   # a kept host without its rewrite output / shader library is grey before any run starts
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'r1', 'host').mkdir(parents=True); Path(t, 'r1', 'host', 'dmhost').write_bytes(b'x'); os.symlink('r1', os.path.join(t, 'current'))
            out = io.StringIO()
            with mock.patch.object(bench, 'BASE', t), mock.patch.object(bench.scenario, 'game_lock', lambda *a: None), \
                 mock.patch.object(bench.scenario, 'Runner', raise_if_called), mock.patch.object(bench, 'current_host', raise_if_called), contextlib.redirect_stdout(out):
                rc = bench.main(['regress', ROUTE, '--reps', '6'])
        self.assertEqual(rc, 3, out.getvalue()); self.assertIn('no rewrite output / shader library snapshot', out.getvalue())


    def test_holm_and_min_reps(self):
        rows = [dict(p=p) for p in (0.001, 0.01, 0.02, 0.04)]; bench.holm(rows)
        self.assertEqual([r['holm'] for r in rows], [True, True, True, True])
        rows = [dict(p=p) for p in (0.04, 0.03, 0.02, 0.01)] + [dict(verdict='not enough runs')]; bench.holm(rows)   # in any order; a row without p is left alone
        self.assertEqual([r.get('holm') for r in rows], [False, False, False, True, None]); self.assertNotIn('holm', rows[4])
        self.assertEqual([bench.min_reps(m) for m in (1, 4, 6, 8)], [4, 5, 5, 6])   # 2/70 = 0.0286; 2/252 = 0.0079 <= 0.0125 and <= 0.00833; 2/924 = 0.0022 <= 0.00625

    def snapshotted(self, t):
        host, prod, msl = fake_tree(os.path.join(t, 'src'))
        ok, why = bench.snapshot(host, prod, msl, CHAIN, '3.3.94012-r49', os.path.join(t, 'r1'), git={'rev': 'r1'}); self.assertTrue(ok, why)
        os.symlink('r1', os.path.join(t, 'current'))

    def test_regress_runs_guard(self):   # too few runs for the comparisons regress makes: refused before any run, naming the reps it needs
        for argv, want in ((['--reps', '4'], 'use --reps 5'), (['--metrics', 'cpu_ms.p50,nvn_ms.p50,gpu_ms.p50,proc_cpu_ms.p50', '--reps', '5'], 'use --reps 6'),
                           (['--metrics', 'cpu_ms.p50,nope.p50'], 'unknown metric')):
            with tempfile.TemporaryDirectory() as t:
                self.snapshotted(t); out = io.StringIO()
                with mock.patch.object(bench, 'BASE', t), mock.patch.object(bench.scenario, 'game_lock', lambda *a: None), \
                     mock.patch.object(bench.scenario, 'Runner', raise_if_called), mock.patch.object(bench, 'current_host', raise_if_called), contextlib.redirect_stdout(out):
                    rc = bench.main(['regress', ROUTE] + argv)
            self.assertEqual(rc, 2, out.getvalue()); self.assertIn(want, out.getvalue())


if __name__ == '__main__':
    unittest.main()
