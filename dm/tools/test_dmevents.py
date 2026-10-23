"""dmevents.py 的单测：合成的事件文件与夹着「[dm] EV」行的日志；不跑游戏。 | Unit tests for dmevents.py: a synthetic event file and a log with "[dm] EV" lines mixed in; the game is not run."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import dmevents  # noqa: E402


def ev(seq, t, name, **kw):
    return json.dumps(dict(seq=seq, t=t, ev=name, **kw), ensure_ascii=False)


GOOD = [ev(0, 0.0, 'start', v=1), ev(1, 1.2, 'main_menu'), ev(2, 3.4, 'game_enter'), ev(3, 4.4, 'loading_done', tick=16), ev(4, 9.0, 'pause_open', tick=130)]


class DmeventsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dmevents ')
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, lines, name='ev.ndjson'):
        p = self.dir / name; p.write_text('\n'.join(lines) + '\n', encoding='utf-8'); return str(p)

    def cli(self, *args):
        p = subprocess.run([sys.executable, str(TOOLS / 'dmevents.py'), *args], capture_output=True, text=True, check=False)
        return p.returncode, p.stdout

    def test_log_lines_and_plain_lines(self):
        log = ['[dm] UIMSG [1.25s] PanelManager OpenPanel MainMenuButtonRibbon', '[dm] EV ' + GOOD[1], 'boot: 版本 1', '[dm] EV {broken', '[dm] EV ' + GOOD[2]]
        self.assertEqual([e['ev'] for e in dmevents.load(self.write(log, 'log.txt'))], ['main_menu', 'game_enter'])
        self.assertEqual(len(dmevents.load('\n'.join(GOOD))), 5)   # 也收文本 | text is accepted too

    def test_expect_order_and_within(self):
        f = self.write(GOOD)
        self.assertEqual(self.cli('expect', f, 'main_menu', 'game_enter', 'loading_done', '--no-red')[0], 0)
        rc, out = self.cli('expect', f, 'loading_done', 'game_enter')   # game_enter 在 loading_done 之前：顺序不对 | game_enter is before loading_done: wrong order
        self.assertEqual(rc, 1); self.assertIn('EVENTS RED missing game_enter after loading_done', out)
        self.assertEqual(self.cli('expect', f, 'pause_open', '--within', '5')[0], 1)
        self.assertEqual(self.cli('expect', f, 'pause_open', '--within', '10')[0], 0)

    def test_red_guard_and_why(self):
        bad = GOOD[:3] + [ev(3, 3.9, 'red', code='0123abcd', src='hle_fs.c:484', msg='【红】存档提交拒绝'), ev(4, 4.0, 'crash', sig=11, pc='0x10', where='main+0x40', addr='0x0')]
        f = self.write(bad)
        self.assertEqual(self.cli('expect', f, 'main_menu', 'game_enter')[0], 0)   # 不加 --no-red 只看顺序 | without --no-red only the order counts
        rc, out = self.cli('expect', f, 'main_menu', 'game_enter', '--no-red')
        self.assertEqual(rc, 1); self.assertIn('crash sig=11 at main+0x40', out)   # crash 比 red 优先报 | crash is reported before red
        rc, out = self.cli('expect', f, 'loading_done', '--no-red')
        self.assertIn('missing loading_done; crash sig=11', out)
        self.assertEqual(self.cli('why', self.write(GOOD))[0], 1)

    def test_summary(self):
        rc, out = self.cli('summary', self.write(GOOD + [ev(5, 20.0, 'exit', why='ticks_full', code=0)]))
        res = json.loads(out.splitlines()[-1][7:])
        self.assertEqual((rc, res['events'], res['milestones']['loading_done'], res['counts']['start']), (0, 6, 4.4, 1))

    def test_follow_growing_file(self):
        f = self.dir / 'grow.ndjson'; f.write_text(GOOD[0] + '\n', encoding='utf-8')
        def append():
            time.sleep(0.5)
            with open(f, 'a', encoding='utf-8') as h: h.write(GOOD[1] + '\n' + GOOD[2][:10]); h.flush(); time.sleep(0.3); h.write(GOOD[2][10:] + '\n')
        threading.Thread(target=append).start()
        rc, out = self.cli('follow', str(f), 'game_enter', '--timeout', '5')
        self.assertEqual((rc, json.loads(out)['ev']), (0, 'game_enter'))
        self.assertEqual(self.cli('follow', str(f), 'pause_open', '--timeout', '0.5')[0], 1)
        g = self.write([GOOD[0], ev(1, 1.0, 'fatal', code='x', src='a.c:1', msg='FATAL x')], 'fatal.ndjson')
        self.assertEqual(self.cli('follow', g, 'main_menu', '--timeout', '3')[0], 2)


if __name__ == '__main__':
    unittest.main()
