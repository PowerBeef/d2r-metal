#!/usr/bin/env python3
"""bench_why.py on a made-up run: a late frame's cause comes from the stage that jumped or what it built, ms per frame from ticks and samples, samples checked against the CPU clock, and two runs compared."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_why as W  # noqa: E402

PERIOD = 100.0 / 3


def make_run(d, slow=None, hot_share=0.5, n=200, extra=None):
    """200 frames at 33.3 ms in one segment; slow = {frame: {column: value}}. Game main runs hot_share of the ticks in fn A, the translation thread a quarter"""
    os.makedirs(d, exist_ok=True); slow = slow or {}
    with open(os.path.join(d, 'nvn.tsv'), 'w', encoding='utf-8') as f:
        f.write('# kind\nframe\tname\tvalue\n')
        for i in range(n):
            r = dict(t_s=i * PERIOD / 1000, presented_ms=PERIOD, cpu_ms=2.0, nvn_ms=1.5, sub_cpu_ms=2.0, sub_wall_ms=2.5, proc_cpu_ms=10.0, gpu_ms=20.0, pq_wait_ms=0.0, drawable_ms=0.0, upload_ms=0.0, mprotect_ms=0.0, draws=1500, faults=0, pso_new=0, lib_new=0)
            r.update(slow.get(i, {}))
            for k, v in r.items(): f.write(f'{i}\t@{k}\t{v}\n')
    with open(os.path.join(d, 'events.ndjson'), 'w', encoding='utf-8') as f:
        f.write(json.dumps(dict(ev='mark', name='go', frame=0, t=0.0)) + '\n' + json.dumps(dict(ev='mark', name='end', frame=n - 1, t=(n - 1) * PERIOD / 1000)) + '\n')
    ticks = n * 8; tick_s = n * PERIOD / 1000 / ticks
    with open(os.path.join(d, 'prof.tsv'), 'w', encoding='utf-8') as f:
        f.write('# dm_sampler 1 ms=4\nT\t1\t0x1001 主线程\nT\t2\tnvn-translate\n')
        cpu1 = cpu2 = 0
        for i in range(ticks):
            t = i * tick_s; f.write(f'K\t{t:.4f}\n')
            if i % int(1 / hot_share) == 0: f.write('S\t1\tNvn_Bind+0x4\tRender_Draw+0x8\n' if not extra else f'S\t1\t{extra}\n'); cpu1 += tick_s * 1e6
            if i % 4 == 0: f.write('S\t2\tx_submit\n'); cpu2 += tick_s * 1e6
            if i % 16 == 0: f.write(f'C\t{t:.4f}\t1\t{int(cpu1)}\nC\t{t:.4f}\t2\t{int(cpu2)}\n')


class WhyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.d = os.path.join(self.tmp.name, 'a'); W._fn.clear(); W.guestsyms = None

    def tearDown(self): self.tmp.cleanup()

    def seg(self, **kw):
        make_run(self.d, **kw); return W.analyse(self.d)[0]['segments']['go']

    def test_clean_run_has_no_late_frames(self):
        s = self.seg(); self.assertEqual(s['late'], []); self.assertEqual(s['budget']['frames'], 199); self.assertAlmostEqual(s['budget']['period_ms'], PERIOD, delta=.01)

    def test_causes(self):
        s = self.seg(slow={50: dict(presented_ms=50.0, gpu_ms=45.0), 80: dict(presented_ms=50.0, cpu_ms=30.0), 110: dict(presented_ms=66.7, pso_new=3, gpu_ms=45.0), 140: dict(presented_ms=50.0), 170: dict(presented_ms=-1, upload_ms=12.0)})
        got = {x['frame']: x['cause'] for x in s['late']}
        self.assertEqual(got, {50: 'the GPU', 80: 'the game thread', 110: 'pipelines built', 140: 'nothing in the records: the system or the display', 170: 'uploads'})
        self.assertAlmostEqual(s['causes']['the GPU']['lost_ms'], 16.7, delta=.1)

    def test_previous_frame_counts(self):   # a frame is shown late because of the one before it
        s = self.seg(slow={60: dict(cpu_ms=30.0), 61: dict(presented_ms=50.0)})
        self.assertEqual([x['cause'] for x in s['late']], ['the game thread'])

    def test_one_tick_late_at_real_60_and_under_frame_generation(self):
        s = self.seg(slow={50: dict(presented_ms=PERIOD * 1.25 + 0.5)}); self.assertEqual(len(s['late']), 1)
        s = self.seg(slow={50: dict(presented_ms=PERIOD * 1.2)}); self.assertEqual(s['late'], [])

    def test_ms_per_frame_and_clock(self):
        s = self.seg()
        m = s['hot']['game main']; self.assertAlmostEqual(m['ms'], PERIOD / 2, delta=1.0)
        self.assertEqual(m['self'][0]['fn'], 'Nvn_Bind+0x4'); self.assertAlmostEqual(s['hot']['translation']['ms'], PERIOD / 4, delta=.5)
        self.assertAlmostEqual(s['cpu_clock']['game main'], PERIOD / 2, delta=1.0); self.assertAlmostEqual(s['samples_vs_clock']['game main'], 1.0, delta=.1)

    def test_samples_off_the_clock_are_flagged(self):
        make_run(self.d); p = os.path.join(self.d, 'prof.tsv')
        with open(p, encoding='utf-8') as f: rows = [l for l in f if not l.startswith('S\t1')]   # the clock says game main ran, and no sample saw it but a few
        with open(p, 'w', encoding='utf-8') as f: f.writelines(rows[:len(rows) // 2] + ['S\t1\tx\n'] * 20 + rows[len(rows) // 2:])
        self.assertTrue(W.analyse(self.d)[0]['flags'])

    def test_compare_reports_only_what_moved(self):
        make_run(self.d, hot_share=.5); other = os.path.join(self.tmp.name, 'b'); make_run(other, hot_share=.25)
        a, _, _ = W.analyse(self.d); b, _, _ = W.analyse(other)
        out = W.compare(a, b, 8)['go']['moved']
        self.assertEqual([r['role'] for r in out], ['game main']); self.assertLess(out[0]['d_ms'], -5)

    def test_role_and_subsystem(self):
        self.assertEqual(W.role('0x1001 主线程'), 'game main'); self.assertEqual(W.role('0x1004 Thread_0x1'), 'game workers'); self.assertEqual(W.role('nvn-present-link'), 'present'); self.assertEqual(W.role('llvm-worker-0'), 'shader compile')
        self.assertEqual(W.subsystem('Nvn_Bind'), 'Nvn'); self.assertEqual(W.subsystem('main+0x10'), '(unnamed game code)'); self.assertEqual(W.subsystem('memcpy'), '(host and system)')

    def test_folded(self):
        make_run(self.d); _, smp, names = W.analyse(self.d); p = os.path.join(self.tmp.name, 'f'); W.folded(smp, names, p)
        with open(p, encoding='utf-8') as f: first = f.readline()
        self.assertTrue(first.startswith('game main;Render_Draw+0x8;Nvn_Bind+0x4 '))


if __name__ == '__main__':
    unittest.main()
