"""Unit tests for scenario.py's scheme-neutral steps: the `schemes` header, `only=` on a step, the run's scheme (--scheme / the header), what each
step sends in each scheme, and a step a scheme cannot do ending the run as not judged (exit 3) rather than red. A fake host, no game."""
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


class Host:
    """Replies OK to every command, records them, answers await_ui with a point and opens the inventory on PLUS."""
    def __init__(self, port):
        self.port, self.seq, self.got = port, 0, []
        threading.Thread(target=self.run, daemon=True).start()

    def ev(self, kind, **kw):
        self.seq += 1; self.s.sendall(b'[dm] EV ' + json.dumps(dict(seq=self.seq, t=self.seq / 10, ev=kind, **kw)).encode() + b'\n')

    def run(self):
        self.s = socket.create_connection(('127.0.0.1', self.port)); self.ev('start', v=1)
        buf = b''
        while True:
            d = self.s.recv(4096)
            if not d: return
            buf += d
            *lines, buf = buf.split(b'\n')
            for line in lines:
                r = json.loads(line); self.got.append(r); out = {}
                if r['cmd'] == 'press' and r.get('keys') == 'PLUS': self.ev('panel', act='OpenPanel', name='PlayerInventoryExpansionLayout')
                if r['cmd'] == 'await_ui': out = dict(x=0.5, y=0.4)
                self.s.sendall(b'[dm] RE ' + json.dumps(dict(id=r['id'], cmd=r['cmd'], ok=True, **out)).encode() + b'\n')


class Backend:
    name = 'cli'
    def __init__(self): self.host = None
    def start(self):
        srv, port = dmctl.bind(); self.host = Host(port)
        return dmctl.Ctl(server=srv, connect_timeout=5)
    def alive(self): return False
    def wait_exit(self, timeout): return 0
    def kill(self): pass
    def exit_code(self): return 0
    def stop(self): pass
    def shot_path(self, p): return p
    def event_file(self): return None


class SchemeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='scenario schemes ')
        self.dir = Path(self.temp.name)
        self.saved = os.environ.pop('SCN_SCHEME', None)

    def tearDown(self):
        os.environ.pop('SCN_SCHEME', None)
        if self.saved is not None: os.environ['SCN_SCHEME'] = self.saved
        self.temp.cleanup()

    def scn(self, text, name='t.scn'):
        p = self.dir / name; p.write_text(text, encoding='utf-8'); return str(p)

    def run_scn(self, text, scheme=None):
        if scheme: os.environ['SCN_SCHEME'] = scheme
        else: os.environ.pop('SCN_SCHEME', None)
        r = scenario.Runner(self.scn(text), 'cli', str(self.dir / 'out'), True)
        b = Backend(); r.backend = b   # type: ignore[assignment]
        res, code = r.go()
        return res, code, [(g['cmd'], {k: v for k, v in g.items() if k not in ('id', 'cmd')}) for g in (b.host.got if b.host else [])], r

    def test_parse(self):
        meta, steps, _ = scenario.parse(self.scn('schemes pc touch\nback only=pc,touch\nopen inventory\nuse_unit 1 150 hold=2\nkey return only=pc\n'))
        self.assertEqual(meta['schemes'], ['pc', 'touch'])
        self.assertEqual((steps[0]['op'], steps[0]['only'], steps[0]['kw']), ('back', ['pc', 'touch'], {}))
        self.assertEqual((steps[1]['kw'], steps[1]['only']), ({'what': 'inventory'}, []))
        self.assertEqual(steps[2]['kw'], {'type': '1', 'cls': '150', 'hold': '2'})
        self.assertEqual(steps[3]['kw'], {'name': 'return'})
        self.assertEqual(scenario.parse(self.scn('ui_click Settings\n'))[1][0]['op'], 'menu_pick')   # the old name still parses
        for bad in ('schemes pc mouse\n', 'schemes\n', 'back only=pc,phone\n'):
            with self.assertRaises(scenario.ScenarioError, msg=bad): scenario.parse(self.scn(bad, 'b.scn'))

    def test_scheme_of_the_run(self):
        _, _, _, r = self.run_scn('schemes touch pc\nping\n')
        self.assertEqual((r.scheme, r.scheme_name()), ('touch', 'touch'))   # the header's first scheme
        _, _, _, r = self.run_scn('schemes touch pc\nping\n', 'pc')
        self.assertEqual((r.scheme, r.meta['env'].get('DM_PC_MODE')), ('pc', '1'))   # --scheme pc runs with the PC interface set
        _, _, _, r = self.run_scn('env DM_PC_MODE=1\nping\n')
        self.assertEqual((r.scheme, r.scheme_name()), (None, 'pc'))   # no header: the scenario's own env decides
        _, _, _, r = self.run_scn('ping\n')
        self.assertEqual(r.scheme_name(), 'controller')

    def test_a_scheme_the_scenario_is_not_written_for_is_skipped(self):
        res, code, got, _ = self.run_scn('schemes pc\nping\n', 'controller')
        self.assertEqual((res['verdict'], code, got), ('SKIP', 0, []))

    def test_only_steps(self):
        res, code, got, _ = self.run_scn('schemes controller pc\npress A only=pc\npress B\n')
        self.assertEqual((res['verdict'], code), ('OK', 0))
        self.assertEqual([g for g in got if g[0] == 'press'], [('press', {'keys': 'B'})])

    def test_steps_in_the_controller_scheme(self):
        res, code, got, _ = self.run_scn('schemes controller\nopen inventory\nback\nuse_unit 1 150\n')
        self.assertEqual((res['verdict'], code), ('OK', 0), res)
        self.assertEqual([g for g in got if g[0] in ('press', 'act')], [('press', {'keys': 'PLUS'}), ('press', {'keys': 'B'}), ('act', {'do': 'use_unit', 'type': 1, 'cls': 150})])

    def test_steps_in_the_pc_scheme_off_the_mac_app(self):   # the cli host: PC keys and clicks go into the PC layer (kbm), units through the host's act
        res, code, got, _ = self.run_scn('schemes pc\nopen inventory\nback\nkey return\nmenu_pick Settings\nuse_unit 1 150 btn=right hold=1\n')
        self.assertEqual((res['verdict'], code), ('OK', 0), res)
        sent = [g for g in got if g[0] in ('kbm', 'act')]
        self.assertEqual(sent[:3], [('kbm', {'key': 73}), ('kbm', {'key': 27}), ('kbm', {'key': 13})])
        self.assertEqual(sent[3:5], [('kbm', {'btn': 'left', 'x': 0.5, 'y': 0.4, 'up': 0}), ('kbm', {'btn': 'left', 'x': 0.5, 'y': 0.4, 'up': 1})])
        self.assertEqual(sent[5], ('act', {'do': 'use_unit', 'type': 1, 'cls': 150, 'btn': 'right', 'hold': 1}))

    def test_steps_in_the_touch_scheme(self):
        res, code, got, _ = self.run_scn('schemes touch\nmenu_pick Settings\n')
        self.assertEqual((res['verdict'], code), ('OK', 0), res)
        self.assertIn(('tap_ui', {'name': 'Settings', 'timeout': 10}), got)

    def test_a_step_the_scheme_cannot_do_is_not_judged(self):
        for text in ('schemes controller\npress B\nkey return\npress A\n', 'schemes controller\nmenu_pick Settings\n', 'schemes controller\nuse_unit 1 150 btn=right\n'):
            res, code, got, _ = self.run_scn(text)
            self.assertEqual((res['verdict'], code), ('UNAVAILABLE', 3), text)
            self.assertNotIn(('press', {'keys': 'A'}), got)   # the run stops at the step it cannot do

    def test_an_earlier_red_step_stays_red(self):
        res, code, _, _ = self.run_scn('schemes controller\nexpect panel name=Never timeout=0.2\nkey return\n')
        self.assertEqual((res['verdict'], code), ('RED', 1))


if __name__ == '__main__':
    unittest.main()
