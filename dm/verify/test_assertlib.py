"""assertlib.py 新加部分的单测：事件流查询（面板、存档、坏事件）与日志红标守卫；触屏局目录那部分由 touchcases/test_lib.py 测。 | Unit tests for the parts added to assertlib.py: event-stream queries (panels, saves, bad events) and the log red-marker guard; the touch run-directory part is tested by touchcases/test_lib.py."""
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


if __name__ == '__main__':
    unittest.main()
