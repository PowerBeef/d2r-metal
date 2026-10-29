"""scenario.py 的单测：解析（表头、位置参数、坏行）与运行（假宿主照协议回执、按命令发事件），不跑游戏。 | Unit tests for scenario.py: parsing (header, positional arguments, bad lines) and running (a fake host replies per the protocol and emits events per command); the game is not run."""
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import dmctl  # noqa: E402
import scenario  # noqa: E402


class FakeHost:
    """连到运行器监听口的假宿主：主菜单事件先发，X → 选角面板，LoadCharacter → 进局 + 读取画面撤下。 | A fake host that connects to the runner's listening socket: main menu event first, X -> character select panel, LoadCharacter -> game enter + loading screen gone."""
    def __init__(self, port, fail_cmd=None):
        self.port, self.fail_cmd, self.seq, self.in_game, self.got = port, fail_cmd, 0, False, []
        threading.Thread(target=self.run, daemon=True).start()

    def ev(self, kind, **kw):
        self.seq += 1; self.s.sendall(b'[dm] EV ' + json.dumps(dict(seq=self.seq, t=self.seq / 10, ev=kind, **kw)).encode() + b'\n')

    def run(self):
        self.s = socket.create_connection(('127.0.0.1', self.port)); self.ev('start', v=1); self.ev('main_menu')
        buf = b''
        while True:
            d = self.s.recv(4096)
            if not d: return
            buf += d
            *lines, buf = buf.split(b'\n')
            for line in lines:
                r = json.loads(line); self.got.append(r); out = {}
                if r['cmd'] == 'press' and r.get('keys') == 'X': self.ev('panel', act='OpenPanel', name='CharacterSelectPanel')
                if r['cmd'] == 'msg': self.in_game = True; self.ev('game_enter'); self.ev('loading_done', tick=16)
                if r['cmd'] == 'press' and r.get('keys') == 'PLUS': self.ev('panel', act='OpenPanel', name='PlayerInventoryExpansionLayout')
                if r['cmd'] == 'state': out = dict(in_game=self.in_game, hp=40, panel='AutoMap')
                if r['cmd'] == 'boom': self.ev('crash', sig=11)
                ok = r['cmd'] != self.fail_cmd
                rep = dict(id=r['id'], cmd=r['cmd'], ok=ok, **out, **({} if ok else {'err': 'nope'}))
                self.s.sendall(b'[dm] RE ' + json.dumps(rep).encode() + b'\n')


class FakeBackend:
    name = 'cli'
    def __init__(self, fail_cmd=None): self.fail_cmd, self.host = fail_cmd, None
    def start(self):
        srv, port = dmctl.bind(); self.host = FakeHost(port, self.fail_cmd)
        return dmctl.Ctl(server=srv, connect_timeout=5)
    def alive(self): return False
    def wait_exit(self, timeout): return 0 if timeout >= 0 else None
    def kill(self): pass
    def exit_code(self): return 0
    def stop(self): pass
    def shot_path(self, p): return p
    def event_file(self): return None


class ScenarioTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='scenario ')
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def scn(self, text, name='t.scn'):
        p = self.dir / name; p.write_text(text, encoding='utf-8'); return str(p)

    def test_parse(self):
        meta, steps, checks = scenario.parse(self.scn('name x\nsave empty\nenv DM_UIMSG=1 DM_WATCH=60\ntimeout 60  # 上限\n\npress PLUS 12\ntap_ui SkillButton3+2,0@SkillsTreePanel\nshot a.png\n'
                                                    'expect panel act=OpenPanel name=X timeout=5\nexpect_state hp>=1 panel!=Y\ncheck no_red allow=abc\ncheck exit 0\n'))
        self.assertEqual((meta['name'], meta['save'], meta['env'], meta['timeout']), ('x', 'empty', {'DM_UIMSG': '1', 'DM_WATCH': '60'}, 60))
        self.assertEqual(steps[0]['kw'], {'keys': 'PLUS', 'frames': '12'})
        self.assertEqual(steps[1]['kw'], {'name': 'SkillButton3+2,0@SkillsTreePanel'})
        self.assertEqual((steps[2]['op'], steps[2]['kw']), ('screenshot', {'path': 'a.png'}))
        self.assertEqual((steps[3]['args'], steps[3]['kw']), (['panel'], {'act': 'OpenPanel', 'name': 'X', 'timeout': '5'}))
        self.assertEqual(steps[4]['kw'], {'hp>': '1', 'panel!': 'Y'})
        self.assertEqual([c['op'] for c in checks], ['no_red', 'exit'])
        for bad in ('jump 1\n', 'press A 1 2\n', 'expect\n', 'sleep x\n', 'check nonsense\n', 'msg "unclosed\n'):
            with self.assertRaises(scenario.ScenarioError, msg=bad): scenario.parse(self.scn(bad, 'b.scn'))

    def run_scn(self, text, fail_cmd=None):
        r = scenario.Runner(self.scn(text), 'cli', str(self.dir / 'out'), True)
        fb = FakeBackend(fail_cmd); r.backend = fb   # type: ignore[assignment]
        res, code = r.go()
        assert fb.host is not None
        return res, code, fb.host

    def test_run_ok_with_macro_and_expects(self):
        res, code, host = self.run_scn('enter_game\nexpect_state in_game=true hp>=1\npress PLUS\nexpect panel act=OpenPanel name=PlayerInventoryExpansionLayout timeout=2\ncheck no_red\ncheck event loading_done\n')
        self.assertEqual((code, res['verdict']), (0, 'OK'), res)
        self.assertEqual([g['cmd'] for g in host.got][:4], ['state', 'state', 'press', 'msg'])   # 第二个 state：看选角页是不是已经开着 | the second state: whether character select is already open
        self.assertTrue(os.path.exists(self.dir / 'out' / 'steps.tsv'))

    def test_failed_command_and_failed_expect_are_red(self):
        res, code, _ = self.run_scn('press A\n', fail_cmd='press')
        self.assertEqual((code, res['failed']['step']), (1, 1))
        res, code, _ = self.run_scn('press A\nexpect pause_open timeout=0.5\n')
        self.assertEqual((code, res['failed']['step']), (1, 2))
        self.assertIn('pause_open', res['reason'])

    def test_try_step_failure_does_not_end_the_run(self):   # try 前缀：这一步不成只记 try，后面的步照走；同样的步不带 try 就判红 | try prefix: the failure is only noted as try and later steps run; the same step without try is red
        res, code, host = self.run_scn('try press A\npress B\n', fail_cmd='press')
        self.assertEqual(code, 1)   # 后面不带 try 的 press 照样红 | the later press without try is still red
        self.assertEqual(res['failed']['step'], 2)
        res, code, _ = self.run_scn('try expect pause_open timeout=0.5\npress A\n')
        self.assertEqual((code, res['verdict']), (0, 'OK'), res)
        self.assertIn('\ttry\t', (self.dir / 'out' / 'steps.tsv').read_text())

    def test_crash_event_is_red(self):   # a crash read in before the steps start: the expect step is red and says crash (CI once saw a timeout here)
        with tempfile.TemporaryDirectory() as d:
            r = scenario.Runner(self.scn('ping\nexpect loading_done timeout=2\n'), 'cli', d, True)
            fb = FakeBackend(); r.backend = fb   # type: ignore[assignment]
            orig = fb.start
            def start():
                c = orig()
                assert fb.host is not None
                c.wait_for('main_menu', 5); fb.host.ev('crash', sig=11)
                c.wait_for('crash', 5)   # read in before step 1 takes its mark, the order that used to lose it
                return c
            fb.start = start   # type: ignore[method-assign]
            res, code = r.go()
        self.assertEqual(code, 1); self.assertIn('crash', res['reason'])

    def test_log_scans(self):   # 日志里有消毒器报告：步骤都过了也判红；--metal-validation 档的环境进了场景、lint 只汇总 | a sanitizer report in the log is red even when every step passed; the --metal-validation level's environment reaches the scenario, lint only summarizes
        log = self.dir / 'host.log'
        log.write_text('[dm] 一行\nSUMMARY: AddressSanitizer: heap-use-after-free x.c:1 in f\n2026-10-23 07:54:08.699 dmhost[1:2] Render Command Encoder Set Cull Mode Validation.\nPrevious setCullMode was unused.\n', encoding='utf-8')
        r = scenario.Runner(self.scn('ping\n'), 'cli', str(self.dir / 'out'), True, metal='lint')
        self.assertEqual(r.meta['env'].get('MTL_DEBUG_LAYER_WARNING_MODE'), 'nslog')
        fb = FakeBackend(); fb.log_file = lambda: str(log); r.backend = fb   # type: ignore[assignment,method-assign]
        res, code = r.go()
        self.assertEqual((code, res['verdict']), (1, 'RED'))
        by = {c['check']: c for c in res['checks']}
        self.assertFalse(by['no sanitizer reports']['ok']); self.assertIn('heap-use-after-free', by['no sanitizer reports']['detail'])
        self.assertTrue(by['metal validation (lint)']['ok']); self.assertIn('1 messages', by['metal validation (lint)']['detail'])
        self.assertTrue((self.dir / 'out' / 'sanitizer.txt').exists() and (self.dir / 'out' / 'metal_validation.txt').exists())


if __name__ == '__main__':
    unittest.main()
