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
    def __init__(self, port, fail_cmd=None, mem_errors=0):
        self.port, self.fail_cmd, self.seq, self.in_game, self.got, self.mem_errors = port, fail_cmd, 0, False, [], mem_errors
        self.area, self.taps = 1, 3   # the hero's area (warp moves it); monsters left to tap
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
                if r['cmd'] == 'state': out = dict(in_game=self.in_game, hp=40, hp_max=40, dead=False, area=self.area, panel='AutoMap')
                if r['cmd'] == 'warp': self.area = int(r['level'])
                if r['cmd'] == 'live' and r.get('do') == 'list': out = dict(hooks=2, list=[dict(hook=0, name='frame', hits=120, matched=120, recorded=0), dict(hook=1, name='fence', hits=0, matched=0, recorded=0, acted=0)][int(r.get('from', 0)):])
                if r['cmd'] == 'boom': self.ev('crash', sig=11)
                if r['cmd'] == 'mem': out = dict(at='0x7100000010', hex='02' + '00' * (int(r.get('len', 8)) - 1))   # as the host: len bytes, 8 without it
                ok = r['cmd'] != self.fail_cmd
                if r['cmd'] == 'tap_unit': self.taps -= 1; ok = self.taps >= 0   # then none is left on screen
                if r['cmd'] == 'mem' and self.mem_errors > 0: self.mem_errors -= 1; ok, out = False, {}   # the first reads fail, as through a pointer still null
                rep = dict(id=r['id'], cmd=r['cmd'], ok=ok, **out, **({} if ok else {'err': 'no pointer at 0x0' if r['cmd'] == 'mem' else 'nope'}))
                self.s.sendall(b'[dm] RE ' + json.dumps(rep).encode() + b'\n')


class FakeBackend:
    name = 'cli'
    def __init__(self, fail_cmd=None, mem_errors=0): self.fail_cmd, self.host, self.mem_errors = fail_cmd, None, mem_errors
    def start(self):
        srv, port = dmctl.bind(); self.host = FakeHost(port, self.fail_cmd, self.mem_errors)
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

    def run_scn(self, text, fail_cmd=None, mem_errors=0):
        r = scenario.Runner(self.scn(text), 'cli', str(self.dir / 'out'), True)
        fb = FakeBackend(fail_cmd, mem_errors); r.backend = fb   # type: ignore[assignment]
        res, code = r.go()
        assert fb.host is not None
        return res, code, fb.host

    def test_run_ok_with_macro_and_expects(self):
        res, code, host = self.run_scn('enter_game\nexpect_state in_game=true hp>=1\npress PLUS\nexpect panel act=OpenPanel name=PlayerInventoryExpansionLayout timeout=2\ncheck no_red\ncheck event loading_done\n')
        self.assertEqual((code, res['verdict']), (0, 'OK'), res)
        self.assertEqual([g['cmd'] for g in host.got][:4], ['state', 'state', 'press', 'msg'])   # 第二个 state：看选角页是不是已经开着 | the second state: whether character select is already open
        self.assertTrue(os.path.exists(self.dir / 'out' / 'steps.tsv'))

    def test_goto_fight_and_live_checks(self):
        """goto warps by stop name or area id and waits for the area; fight spawns each class, taps what is left and presses the skill buttons;
        expect_live and remember live: read a hook's counters from `live do=list`."""
        res, code, host = self.run_scn('enter_game\ngoto a1_den settle=0\nexpect_state area=8\ngoto 40 settle=0\ngoto 40 settle=0\n'
                                       'fight cls=19,5 count=2 rounds=6 pace=0 skill=X,Y\nexpect_live frame hits>=60\nremember live:frame.hits as=f0\n'
                                       'expect_live frame hits=$f0 matched=120\nexpect_live fence hits=0 acted=0\nexpect_live 1 hits=0\n')
        self.assertEqual((code, res['verdict']), (0, 'OK'), res)
        got = host.got; self.assertEqual([g['level'] for g in got if g['cmd'] == 'warp'], [8, 40])   # the third goto finds the hero already there
        self.assertEqual([(g['cls'], g['count']) for g in got if g['cmd'] == 'spawn'], [(19, 2), (5, 2)])
        self.assertEqual(len([g for g in got if g['cmd'] == 'tap_unit']), 5)   # three found, then one miss of each class ends the fight
        self.assertEqual([g['keys'] for g in got if g['cmd'] == 'press' and g.get('keys') in ('X', 'Y')][1:], ['X', 'Y', 'X'])   # after enter_game's own X
        res, code, _ = self.run_scn('enter_game\nexpect_live frame hits=0 timeout=0.4\n')
        self.assertEqual(code, 1); self.assertIn('live hook frame: hits=0 (is 120)', res['reason'])
        res, code, _ = self.run_scn('enter_game\nexpect_live nosuch hits=0\n'); self.assertIn("no live hook 'nosuch'", res['reason'])
        res, code, _ = self.run_scn('enter_game\ngoto nowhere\n'); self.assertIn('neither an area id nor a stop', res['reason'])
        for bad in ('expect_live frame\n', 'expect_live hits=1\n', 'goto\n'):
            with self.assertRaises(scenario.ScenarioError): scenario.parse(self.scn(bad, 'bad.scn'))

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


    def test_parse_log_has_and_expect_mem(self):
        _, _, checks = scenario.parse(self.scn('check log_has "Shadow map: 4096 px" file=p.tsv min=2\ncheck log_has re="a=1 b"\ncheck log_has last="窗口：内容区" "x"\n'))
        self.assertEqual([(c['op'], c['args'], c['kw']) for c in checks], [('log_has', ['Shadow map: 4096 px'], {'file': 'p.tsv', 'min': '2'}),
                                                                           ('log_has', [], {'re': 'a=1 b'}), ('log_has', ['x'], {'last': '窗口：内容区'})])
        _, _, checks = scenario.parse(self.scn('check log_has "放在 \\(0,0\\)"\n'))   # double quotes keep the backslash
        self.assertEqual(checks[0]['args'], ['放在 \\(0,0\\)'])
        _, steps, _ = scenario.parse(self.scn('expect_mem main+0x10 1 chain=0x8 hex=02\nexpect_mem main+0x10 hex=0200\n'))
        self.assertEqual((steps[0]['op'], steps[0]['kw']), ('expect_mem', {'at': 'main+0x10', 'len': '1', 'chain': '0x8', 'hex': '02'}))
        self.assertEqual(steps[1]['kw']['len'], '2')   # without len the read is hex's byte count, not the control port's 8
        for bad in ('check log_has\n', 'check log_has "("\n', 'check log_has last="(" x\n', 'check log_has x min=two\n', 'check log_has a=1\n',
                    'check log_has x min=0\n', 'check log_has last="a" "b" min=2\n', 'check log_has x re=y\n',
                    'expect_mem main+0x10 hex=0\n', 'expect_mem hex=02\n', 'expect_mem main+0x10 1 2 hex=02\n',
                    'expect_mem main+0x10 4 hex=02\n', 'expect_mem main+0x10 len=x hex=02\n'):
            with self.assertRaises(scenario.ScenarioError, msg=bad): scenario.parse(self.scn(bad, 'b.scn'))

    def test_expect_mem(self):   # polls the control port's mem until the bytes match; hex and timeout are never sent to the host
        res, code, host = self.run_scn('expect_mem main+0x10 1 hex=02\n')
        self.assertEqual((code, res['verdict']), (0, 'OK'), res)
        mem = [g for g in host.got if g['cmd'] == 'mem'][0]
        self.assertEqual((mem['at'], mem['len']), ('main+0x10', 1)); self.assertNotIn('hex', mem); self.assertNotIn('timeout', mem)
        res, code, _ = self.run_scn('expect_mem main+0x10 1 hex=01 timeout=0.5\n')
        self.assertEqual((code, res['failed']['step']), (1, 1)); self.assertIn('want 01', res['reason'])
        res, code, host = self.run_scn('expect_mem main+0x10 hex=0200\n')   # len left out: it reads exactly the two bytes of hex
        self.assertEqual((code, res['verdict']), (0, 'OK'), res); self.assertEqual([g['len'] for g in host.got if g['cmd'] == 'mem'], [2])

    def test_expect_mem_polls_through_error_replies(self):   # an error reply (a pointer on the chain still null) is read again until the timeout, not red at once
        res, code, host = self.run_scn('expect_mem main+0x10 chain=0x8 hex=02 timeout=5\n', mem_errors=2)
        self.assertEqual((code, res['verdict']), (0, 'OK'), res); self.assertEqual(sum(g['cmd'] == 'mem' for g in host.got), 3)
        res, code, _ = self.run_scn('expect_mem main+0x10 chain=0x8 hex=02 timeout=0.5\n', mem_errors=100)
        self.assertEqual((code, res['failed']['step']), (1, 1)); self.assertIn('no pointer at 0x0', res['reason']); self.assertIn('want 02', res['reason'])

    def run_log(self, text, lines, files=None):
        log = self.dir / 'host.log'; log.write_text(''.join(x + '\n' for x in lines), encoding='utf-8')
        r = scenario.Runner(self.scn(text), 'cli', str(self.dir / 'out'), True)
        for name, body in (files or {}).items(): (self.dir / 'out' / name).write_text(body, encoding='utf-8')
        fb = FakeBackend(); fb.log_file = lambda: str(log); r.backend = fb   # type: ignore[assignment,method-assign]
        return r.go()

    def test_log_has(self):
        lines = ['Shadow map: 4096 px', 'x', 'Shadow map: 4096 px']
        res, code = self.run_log('ping\ncheck log_has "Shadow map: 4096 px" min=2\n', lines)
        self.assertEqual(code, 0, res); self.assertTrue(res['checks'][0]['detail'].startswith('2 lines'), res['checks'][0])
        res, code = self.run_log('ping\ncheck log_has "Shadow map: 4096 px" min=3\n', lines)
        self.assertEqual((code, res['checks'][0]['ok']), (1, False))
        res, code = self.run_log('ping\ncheck log_has "\\t@mfx3d\\t1" file=nvn.tsv min=2\n', lines, {'nvn.tsv': '5\t@mfx3d\t1\n6\t@mfx3d\t1\n'})
        self.assertEqual(code, 0, res)
        res, code = self.run_log('ping\ncheck log_has x file=nope.tsv\n', lines)
        self.assertEqual(code, 1); self.assertIn('nope.tsv', res['checks'][0]['detail'])

    def test_log_has_last(self):   # last= judges only the last line matching it: an earlier windowed layout line cannot pass a full-screen check
        full, win = '窗口：内容区 1280×720，游戏画面 1280×720 放在 (0,0)', '窗口：内容区 1280×658，游戏画面 1170×658 放在 (55,0)'
        scn = 'ping\ncheck log_has last="窗口：内容区" "内容区 1280×720，游戏画面 1280×720 放在 \\(0,0\\)"\n'
        res, code = self.run_log(scn, [full, win]); self.assertEqual(code, 1); self.assertIn('1170×658', res['checks'][0]['detail'])
        res, code = self.run_log(scn, [win, full]); self.assertEqual(code, 0, res)
        res, code = self.run_log(scn, ['nothing']); self.assertEqual(code, 1)

    def test_run_token_and_extra_env(self):
        self.assertEqual(scenario.scn_env({'A': '{run}/p.tsv', 'B': 'x'}, '/r'), {'A': '/r/p.tsv', 'B': 'x'})
        r = scenario.Runner(self.scn('env A=1\nping\n'), 'cli', str(self.dir / 'out'), True, extra_env={'A': '2', 'B': '3'})
        self.assertEqual(r.meta['env'], {'A': '2', 'B': '3'})


if __name__ == '__main__':
    unittest.main()
