"""assertlib.py 新加部分的单测：事件流查询（面板、存档、坏事件）、日志红标守卫、消毒器报告与 Metal 校验层消息；触屏局目录那部分由 touchcases/test_lib.py 测。 | Unit tests for the parts added to assertlib.py: event-stream queries (panels, saves, bad events), the log red-marker guard, sanitizer reports and Metal validation messages; the touch run-directory part is tested by touchcases/test_lib.py."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import assertlib  # noqa: E402


def ev(seq, kind, **kw):
    return json.dumps(dict(seq=seq, t=seq / 10, ev=kind, **kw), ensure_ascii=False)


class AssertlibTest(unittest.TestCase):
    def test_events(self):
        lines = [ev(1, 'panel', act='OpenPanel', name='A'), ev(2, 'panel', act='OpenPanel', name='B'), ev(3, 'save', result='commit', n=1),
                 ev(4, 'panel', act='ClosePanel', name='A'), ev(5, 'red', code='abc', src='x.c:1', msg='【红】x'), ev(6, 'panel', act='OpenExclusivePanel', name='C'),
                 ev(7, 'save', result='rejected', errno=28)]
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'events.ndjson'; p.write_text('\n'.join(lines) + '\n', encoding='utf-8')
            e = assertlib.events(p)
        self.assertEqual(e.panels_open(upto_seq=3), {'A', 'B'})
        self.assertEqual(e.panels_open(upto_seq=4), {'B'})
        self.assertEqual(e.panels_open(), {'C'})
        self.assertEqual([s['n'] for s in e.saves()], [1])
        self.assertEqual(len(e.bad()), 1); self.assertEqual(e.bad(allow_codes=['abc']), [])
        rej = e.first('save', result='rejected'); assert rej is not None
        self.assertEqual(rej['errno'], 28)

    def test_logred(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'host.log'
            p.write_text('[dm] 正常一行\n[dm] 【红】存档提交拒绝\nboot: 🔴 显卡不支持\n[dm] FATAL boom\n[dm] CRASH sig=11 pc=main+0x10\n[dm] 【红】看门狗：第 15 秒起无进展\n[dm] FATALITY 不是红标\n', encoding='utf-8')
            self.assertEqual([n for n, _ in assertlib.logred(p)], [2, 3, 4, 5, 6])
            self.assertEqual([n for n, _ in assertlib.logred(p, allow=['显卡', '看门狗'])], [2, 4, 5])
            r = subprocess.run([sys.executable, str(HERE / 'assertlib.py'), 'logred', str(p), '--allow', '.'], capture_output=True, text=True, check=False)
            self.assertEqual(r.returncode, 0); self.assertIn('LOGRED OK', r.stdout)
            r = subprocess.run([sys.executable, str(HERE / 'assertlib.py'), 'logred', str(p)], capture_output=True, text=True, check=False)
            self.assertEqual(r.returncode, 1); self.assertIn('LOGRED RED 5', r.stdout)

    def test_sanitizers(self):
        log = ('[dm] 正常一行\n'
               'loader.c:279:100: runtime error: store to misaligned address 0x0001134168b4 for type \'uint64_t *\'\n'
               'SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior loader.c:279:100 \n'
               '==66356==ERROR: AddressSanitizer: global-buffer-overflow on address 0x0001047dd2f0 at pc 0x00010439e7ac\n'
               'READ of size 8 at 0x0001047dd2f0 thread T0\n'
               'SUMMARY: AddressSanitizer: global-buffer-overflow hle_nvn.c:95 in nvn_init_slots\n'
               + 'WARNING: ThreadSanitizer: data race (pid=1)\n  Write of size 8 at 0x1 by thread T2:\nSUMMARY: ThreadSanitizer: data race nvn_mtl.m:897 in f\n' * 2
               + 'dm_vault.c:82:9: runtime error: load of misaligned address 0x00011 for type \'uint16_t\'\n')
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'host.log'; p.write_text(log, encoding='utf-8')
            rep = assertlib.sanitizers(p)
            self.assertEqual(rep[0], ('ThreadSanitizer', 'data race nvn_mtl.m:897 in f', 2))
            self.assertEqual(sorted(k for k, _, _ in rep), ['AddressSanitizer', 'ThreadSanitizer', 'UndefinedBehaviorSanitizer', 'UndefinedBehaviorSanitizer'])
            self.assertIn(('UndefinedBehaviorSanitizer', 'undefined-behavior loader.c:279:100 ', 1), rep)   # runtime error 行与摘要行只算一份 | the runtime error line and its summary count once
            self.assertTrue(any(w.startswith('load of misaligned address 0x…') for _, w, _ in rep))   # 没有摘要行的那份按 runtime error 行记 | the one without a summary line is counted by its runtime error line
            r = subprocess.run([sys.executable, str(HERE / 'assertlib.py'), 'sanitizers', str(p)], capture_output=True, text=True, check=False)
            self.assertEqual(r.returncode, 1); self.assertIn('SANITIZERS RED 5', r.stdout)
            p.write_text('[dm] 正常一行\n', encoding='utf-8')
            self.assertEqual(assertlib.sanitizers(p), [])

    def test_metal_validation(self):
        head = '2026-10-23 07:54:08.699 dmhost[69902:2124497] '
        log = (head + 'Metal API Validation Enabled\n'
               + (head + 'Compute Command Encoder Set Compute Pipeline State Validation\nRedundant call to setComputePipelineState.\n') * 3
               + head + 'Render Command Encoder Set Cull Mode Validation.\nPrevious setCullMode was unused.\n'
               + '[dm] [nvn] 一行\n'
               + "-[MTLDebugRenderCommandEncoder validateDraw:]:1234: failed assertion `Draw Errors Validation 0x1234'\n")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'host.log'; p.write_text(log, encoding='utf-8')
            rep = assertlib.metal_validation(p)
            self.assertEqual(rep[0], ('Compute Command Encoder Set Compute Pipeline State Validation', 'Redundant call to setComputePipelineState.', 3))
            self.assertEqual(len(rep), 3); self.assertIn('0x…', rep[-1][1])
            r = subprocess.run([sys.executable, str(HERE / 'assertlib.py'), 'metal', str(p)], capture_output=True, text=True, check=False)
            self.assertEqual(r.returncode, 1); self.assertIn('METALVAL RED 5', r.stdout)
            r = subprocess.run([sys.executable, str(HERE / 'assertlib.py'), 'metal', str(p), '--lint'], capture_output=True, text=True, check=False)
            self.assertEqual(r.returncode, 0); self.assertIn('METALVAL OK 5', r.stdout)


if __name__ == '__main__':
    unittest.main()
