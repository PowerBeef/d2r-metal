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
import picture  # noqa: E402

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


    def regress_out(self, argv, segs, go=None):
        """bench.py regress over fake runs whose segments come from segs(run number): (exit code, output, regress.json). go(attempt) gives a run's (result, code)."""
        n = [0]; tries = [0]
        class FakeRunner:
            def __init__(self, route, backend, out_dir, headless): self.on_started = None
            def go(self): tries[0] += 1; return go(tries[0]) if go else ({}, 0)
        def fake_segments(d): n[0] += 1; return segs(n[0])
        with tempfile.TemporaryDirectory() as t:
            self.snapshotted(t); host = Path(t, 'dmhost'); host.write_bytes(b'x'); out = io.StringIO()
            with mock.patch.object(bench.scenario, 'Runner', FakeRunner), mock.patch.object(bench.scenario, 'game_lock', lambda *a: None), \
                 mock.patch.object(bench, 'wait_cool', lambda s: (None, 0)), mock.patch.object(bench, 'thermal', lambda: None), \
                 mock.patch.object(bench, 'environment', lambda: {'git': {'rev': 'x'}}), mock.patch.object(bench, 'DM_ROOT', t), mock.patch.object(bench, 'BASE', t), \
                 mock.patch.object(bench, 'current_host', lambda: str(host)), mock.patch.object(bench, 'segments', fake_segments), contextlib.redirect_stdout(out):
                rc = bench.main(['regress', ROUTE, '--reps', '6'] + argv)
            with open(next(Path(t, 'run', 'bench').glob('town_loop_regress_*/regress.json')), encoding='utf-8') as f: rj = json.load(f)
        return rc, out.getvalue(), rj

    def test_regress_not_judged(self):   # a requested comparison with no p (too few runs had a value) never reads OK: grey, naming it
        def seg(i, present=False):   # values that interleave between A and B: no difference
            v = 1.0 + (i % 3) * 0.01
            return dict({k: {'p50': v} for k in ('cpu_ms', 'nvn_ms', 'proc_cpu_ms')}, present={'p95_ms': v} if present else {})
        rc, out, rj = self.regress_out([], lambda i: {'town': seg(i), 'wild': seg(i)})
        self.assertEqual(rc, 0, out); self.assertIn('BENCH-REGRESS OK', out); self.assertEqual((rj['verdict'], rj['not_judged']), ('OK', {}))
        rc, out, rj = self.regress_out(['--metrics', 'cpu_ms.p50,present.p95_ms'], lambda i: {'town': seg(i), 'wild': seg(i)})   # present.* empty, as at Recommended
        self.assertEqual(rc, 3, out); self.assertIn('⚪ BENCH-REGRESS town_loop: not judged', out); self.assertIn('present.p95_ms (town, wild)', out)
        self.assertNotIn('BENCH-REGRESS OK', out); self.assertEqual(rj['not_judged'], {'present.p95_ms': ['town', 'wild']})
        rc, out, _ = self.regress_out(['--metrics', 'present.p95_ms'], lambda i: {'town': seg(i, True), 'wild': seg(i, True)})
        self.assertEqual(rc, 0, out)
        rc, out, rj = self.regress_out([], lambda i: {'town': seg(i)})   # a route segment missing from every run
        self.assertEqual(rc, 3, out); self.assertEqual(rj['not_judged'], {m: ['wild'] for m in ('cpu_ms.p50', 'nvn_ms.p50', 'proc_cpu_ms.p50')})
        def slow(i): return {'town': dict(cpu_ms={'p50': (2.0 if ('ABBA' * 3)[i - 1] == 'B' else 1.0) + i / 1000})}   # B clearly slower (run i follows the ABBA order)
        rc, out, rj = self.regress_out(['--metrics', 'cpu_ms.p50,present.p95_ms'], lambda i: {**slow(i), 'wild': seg(i)})   # a slowdown is red even with a comparison not judged
        self.assertEqual((rc, rj['verdict']), (1, 'RED'), out); self.assertIn('town cpu_ms.p50 +', out); self.assertIn('not judged: present.p95_ms (town, wild)', out)
        self.assertEqual(bench.not_judged([], ['cpu_ms.p50'], []), {'cpu_ms.p50': ['(no segment)']})

    def test_regress_disturbed(self):   # a run the desk still disturbed after its redos is a not_judged reason: grey (exit 3), never OK; a slowdown stays red
        def seg(i):
            v = 1.0 + (i % 3) * 0.01
            return {'town': {k: {'p50': v} for k in ('cpu_ms', 'nvn_ms', 'proc_cpu_ms')}, 'wild': {k: {'p50': v} for k in ('cpu_ms', 'nvn_ms', 'proc_cpu_ms')}}
        dist = dict(disturbed=[dict(t=1.0, step=1, what='desk other_app by com.example')])
        with mock.patch.object(bench, 'REDO', bench.REDO):   # main() sets the module's REDO from --redo: put it back afterwards
            rc, out, rj = self.regress_out(['--redo', '0'], seg, go=lambda k: (dist, 0) if k == 3 else ({}, 0))
        self.assertEqual((rc, rj['verdict']), (3, 'UNAVAILABLE'), out); self.assertIn('⚪ BENCH-REGRESS town_loop: not judged, 1 runs disturbed', out)
        self.assertEqual(rj['not_judged'], {'disturbed': ['03B: desk other_app by com.example']}); self.assertNotIn('BENCH-REGRESS OK', out)

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


    def test_picture_env(self):
        self.assertEqual(bench.picture_env('rec', 'cli', {}), {**picture.REC, 'DM_TEST_FULLSCREEN_AT': '1'})
        self.assertEqual(bench.picture_env('rec', 'mac-app', {}), {'DM_TEST_VIDEO_FIRSTBOOT': '1'})
        self.assertEqual(bench.picture_env('orig', 'cli', {}), {})
        got = bench.picture_env('rec', 'cli', {'DM_TEST_WINSIZE': '1280x720'})
        self.assertEqual(got['DM_TEST_WINSIZE'], '1280x720'); self.assertNotIn('DM_TEST_FULLSCREEN_AT', got)

    def test_picture_reaches_host(self):   # run, ab, profile and regress all hand the picture to the host; regress's A runs also get the baseline's snapshot
        seen = []
        class FakeRunner:
            def __init__(self, route, backend, out_dir, headless): seen.append((out_dir, dict(os.environ))); self.on_started = None
            def go(self): return {}, 0
        with tempfile.TemporaryDirectory() as t:
            host = Path(t, 'dmhost'); host.write_bytes(b'x')
            base = os.path.join(t, 'baseline'); os.makedirs(base); self.snapshotted(base)
            patches = [mock.patch.object(bench.scenario, 'Runner', FakeRunner), mock.patch.object(bench.scenario, 'game_lock', lambda *a: None),
                       mock.patch.object(bench, 'wait_cool', lambda s: (None, 0)), mock.patch.object(bench, 'thermal', lambda: None),
                       mock.patch.object(bench, 'environment', lambda: {'git': {'rev': 'x'}}), mock.patch.object(bench, 'DM_ROOT', t), mock.patch.object(bench, 'BASE', base),
                       mock.patch.object(bench, 'current_host', lambda: str(host)), mock.patch.object(bench, 'attribute', lambda d, h: dict(threads={}, segments={}, hitches=[]))]
            with contextlib.ExitStack() as st, contextlib.redirect_stdout(io.StringIO()):
                for pt in patches: st.enter_context(pt)
                def call(argv, want=0):
                    seen.clear(); rc = bench.main(argv); self.assertEqual(rc, want, argv); self.assertNotIn('DM_GAME_RES', os.environ); return list(seen)
                rec = {k: v for k, v in picture.REC.items()}
                [(_, got)] = call(['run', ROUTE, '--reps', '1'])
                self.assertEqual((got['DM_GAME_RES'], got['DM_TEST_FULLSCREEN_AT']), ('2560x1440', '1')); self.assertLessEqual(rec.items(), got.items())
                [(_, got)] = call(['run', ROUTE, '--reps', '1', '--env', 'DM_GAME_RES=1600x900'])
                self.assertEqual(got['DM_GAME_RES'], '1600x900')
                [(_, got)] = call(['run', ROUTE, '--reps', '1', '--picture', 'orig'])
                self.assertNotIn('DM_AA_MODE', got)
                runs = call(['ab', ROUTE, '--reps', '1', '--b-env', 'DM_AA_MODE=1'])
                self.assertEqual(len(runs), 2); self.assertTrue(all(got['DM_NVN_ANISO'] == '16' for _, got in runs))
                self.assertEqual([got['DM_AA_MODE'] for _, got in runs], ['2', '1'])
                [(_, got)] = call(['profile', ROUTE])
                self.assertEqual((got['DM_SAMPLE'], got['DM_SHADOW_SIZE']), ('4:*', '4096'))
                runs = call(['regress', ROUTE, '--reps', '6'], 3)   # the fake runs measure nothing, so no comparison is judged: grey, not OK
                self.assertEqual(len(runs), 12)
                for d, got in runs:
                    self.assertEqual(got['DM_ENV_SHADOWS'], '1', d)
                    if d.endswith('A'): self.assertEqual((got['SCN_PRODUCT'], got['DM_MSL_DIR']), (os.path.join(os.path.realpath(base), 'r1', 'product'), os.path.join(os.path.realpath(base), 'r1', 'msl')))
                    else: self.assertNotIn('SCN_PRODUCT', got)
                rj = json.load(open(next(Path(t, 'run', 'bench').glob('town_loop_regress_*/regress.json')), encoding='utf-8'))
                self.assertEqual(rj['picture'], 'rec')


    def test_dir_token(self):   # {dir} in an env value becomes the run's own folder (DM_NVN_PASSGPU={dir}/passgpu.tsv), per variant and run
        seen = []
        class FakeRunner:
            def __init__(self, route, backend, out_dir, headless): seen.append((out_dir, dict(os.environ).get('DM_NVN_PASSGPU'))); self.on_started = None   # read through a copy: a test is not a reader in dm/switches.tsv
            def go(self): return {}, 0
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()), \
             mock.patch.object(bench.scenario, 'Runner', FakeRunner), mock.patch.object(bench.scenario, 'game_lock', lambda *a: None), \
             mock.patch.object(bench, 'wait_cool', lambda s: (None, 0)), mock.patch.object(bench, 'thermal', lambda: None), \
             mock.patch.object(bench, 'environment', lambda: {'git': {'rev': 'x'}}), mock.patch.object(bench, 'DM_ROOT', t):
            self.assertEqual(bench.main(['ab', ROUTE, '--reps', '1', '--a-env', 'DM_NVN_PASSGPU={dir}/pg.tsv', '--b-env', 'DM_NVN_PASSGPU={dir}/pg.tsv']), 0)
        self.assertEqual(len(seen), 2)
        for d, v in seen: self.assertEqual(v, os.path.join(d, 'pg.tsv'))
        self.assertNotIn('DM_NVN_PASSGPU', os.environ)


class FakeRunner:
    """Stands in for scenario.Runner in run_once: each go() takes the next outcome of OUTCOMES ('ok', 'red' or 'disturbed'), records the
    DM_TEST_BGRUN it saw and leaves a marker file naming its attempt in its run folder."""
    OUTCOMES: list = []
    seen: list = []

    def __init__(self, route, backend, out, headless, metal=None):
        self.out, self.on_started = out, None

    def go(self):
        what = FakeRunner.OUTCOMES.pop(0); FakeRunner.seen.append((self.out, os.environ.get('DM_TEST_BGRUN')))
        Path(self.out, 'attempt.txt').write_text(f'{len(FakeRunner.seen)} {what}\n', encoding='utf-8')
        Path(self.out, 'events.ndjson').write_text(json.dumps(dict(seq=1, t=0.2, ev='window', cw=1280, ch=720, full=True, emu=False, active=True)) + '\n', encoding='utf-8')
        res = dict(reason=what, disturbed=[dict(t=1.0, step=1, what='desk other_app by com.example')] if what == 'disturbed' else [])
        return res, 1 if what == 'red' else 0


class BenchRedoTest(unittest.TestCase):
    """bench.py runs in front (DM_TEST_BGRUN=0); a disturbed rep is redone in its own slot after cooling, its folder renamed .disturbedK, then not judged."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bench redo '); self.dir = Path(self.temp.name)
        self.saved = (bench.scenario.Runner, bench.wait_cool, bench.thermal, bench.DM_ROOT, bench.environment, bench.ledger_write, sys.modules.get('boothist'))
        self.records: list = []
        bench.scenario.Runner = FakeRunner   # type: ignore[misc]
        bench.wait_cool = lambda s: (dict(nominal=True), 0.0)   # type: ignore[assignment]
        bench.thermal = lambda: dict(nominal=True)   # type: ignore[assignment]
        bench.DM_ROOT = str(self.dir)
        bench.environment = lambda: dict(git=dict(rev='test', dirty=False), bgrun=0)   # type: ignore[assignment]
        bench.ledger_write = lambda *a: 'ledger'   # type: ignore[assignment]
        sys.modules['boothist'] = types.SimpleNamespace(record=lambda files, label: self.records.append(files))   # type: ignore[assignment]
        FakeRunner.seen = []
        os.environ.pop('DM_TEST_BGRUN', None)

    def tearDown(self):
        (bench.scenario.Runner, bench.wait_cool, bench.thermal, bench.DM_ROOT, bench.environment, bench.ledger_write, bh) = self.saved
        if bh is None: sys.modules.pop('boothist', None)
        else: sys.modules['boothist'] = bh
        self.temp.cleanup()

    def once(self, outcomes, redo=2):
        FakeRunner.OUTCOMES = list(outcomes)
        out = str(self.dir / 'r1')
        with contextlib.redirect_stdout(io.StringIO()):
            return bench.run_once('route.scn', 'cli', out, {}, None, 0, redo=redo), out

    def test_disturbed_then_ok_is_accepted(self):
        r, out = self.once(['disturbed', 'ok'])
        self.assertTrue(r['ok']); self.assertEqual((r['attempts'], r['disturbed']), (2, []))
        self.assertEqual(Path(out + '.disturbed1', 'attempt.txt').read_text(encoding='utf-8'), '1 disturbed\n')
        self.assertEqual(Path(out, 'attempt.txt').read_text(encoding='utf-8'), '2 ok\n')   # the same out_dir: DM_FRAMES and DM_NVN_TSV stay valid
        self.assertEqual([s[1] for s in FakeRunner.seen], ['0', '0'])                          # every attempt in front
        self.assertNotIn('DM_TEST_BGRUN', os.environ)                                         # restored afterwards
        self.assertEqual(len(self.records), 1)                                                # boot history only for the accepted rep
        self.assertEqual((r['window']['cw'], r['window']['full']), (1280, True))               # the rep's last window state: in front, real full screen or not

    def test_disturbed_every_time(self):
        r, out = self.once(['disturbed'] * 3)
        self.assertFalse(r['ok']); self.assertEqual(r['attempts'], 3); self.assertEqual(len(r['disturbed']), 1)
        self.assertTrue(Path(out + '.disturbed1').is_dir() and Path(out + '.disturbed2').is_dir())
        self.assertEqual(self.records, [])
        self.assertEqual(bench.rep_verdict([r]), 'NOT_JUDGED')

    def test_rep_verdict(self):
        ok, red, dist = dict(ok=True, disturbed=[]), dict(ok=False, disturbed=[]), dict(ok=False, disturbed=[dict(what='x')])
        self.assertEqual(bench.rep_verdict([ok, ok]), 'OK')
        self.assertEqual(bench.rep_verdict([ok, dist]), 'NOT_JUDGED')
        self.assertEqual(bench.rep_verdict([dist, red]), 'RED')   # a real failure wins

    def test_cmd_run_not_judged(self):
        FakeRunner.OUTCOMES = ['ok'] + ['disturbed'] * 3
        a = types.SimpleNamespace(route='town.scn', backend='cli', reps=2, env=None, host=None, out=str(self.dir / 'bench.json'), cool=0, mpt=False, picture='rec')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf): rc = bench.cmd_run(a)
        self.assertEqual(rc, 3); self.assertIn('⚪ BENCH town not judged: 1 of 2 runs disturbed', buf.getvalue())
        self.assertEqual(json.loads(Path(a.out).read_text(encoding='utf-8'))['verdict'], 'NOT_JUDGED')

    def test_ab_slot_order_after_redo(self):
        FakeRunner.OUTCOMES = ['ok', 'disturbed', 'ok', 'ok', 'ok']   # the second slot (B) is redone once in place
        a = types.SimpleNamespace(route='town.scn', backend='cli', reps=2, a_env='', b_env='', a_host=None, b_host=None, metric='gpu_ms.p50', out=str(self.dir / 'ab.json'), cool=0, picture='rec')
        with contextlib.redirect_stdout(io.StringIO()): rc = bench.cmd_ab(a)
        self.assertEqual(rc, 0)
        slots = [os.path.basename(o) for o, _ in FakeRunner.seen]
        self.assertEqual(slots, ['01A', '02B', '02B', '03B', '04A'])   # ABBA kept: the redo runs in its own slot before the next one
        res = json.loads(Path(a.out).read_text(encoding='utf-8'))
        self.assertEqual(res['order'], 'ABBA'); self.assertEqual([r['attempts'] for r in res['reps']['B']], [2, 1])


if __name__ == '__main__':
    unittest.main()
