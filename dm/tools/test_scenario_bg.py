"""Unit tests for scenario.py's background / front test runs: the `front` header, the DM_TEST_BGRUN value each run gets, the disturbance detector
(synthetic streams and small copies of real event streams), and the redo / "not judged" path with a fake host. The game is not run."""
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import dmtest  # noqa: E402
import scenario  # noqa: E402
from test_scenario import FakeBackend  # noqa: E402


def ev(seq, t, kind, **kw):
    return dict(seq=seq, t=t, ev=kind, **kw)


# Small copies of real event streams (only the event types the detector reads) with the step marks read from each run's port.log.
# inactive_present_cli_20261103_123537_96463: the RED run; another app took the front during `sleep 22` (resign at 15.4 s, from before `cause` existed)
INACTIVE_PRESENT = dict(
    events=[ev(0, 0.005, 'start'), ev(6, 0.141, 'app', state='active'), ev(7, 0.168, 'app', state='visible'), ev(31, 2.515, 'game_enter'),
            ev(66, 3.314, 'loading_done'), ev(69, 15.381, 'app', state='resign')],
    marks=[7, 66, 68, 69, 69, 72, 72, 75, 75], ops=['enter_game', 'stick', 'sleep', 'app', 'sleep', 'app', 'expect', 'log', 'expect'], fs_at=())
# town_loop_ab_20261103_121536 reps 02B and 01A (bench, DM_TEST_FULLSCREEN_AT=1): the real full-screen Space switch, occluded about 1.2 s, visible about 1.7 s
TOWN_02B = dict(
    events=[ev(0, 0.006, 'start'), ev(5, 0.134, 'app', state='visible'), ev(6, 0.148, 'app', state='active'), ev(17, 1.171, 'app', state='occluded'),
            ev(32, 1.698, 'app', state='visible')],
    marks=[6, 70, 72, 74, 76], ops=['enter_game', 'sleep', 'mark', 'walk_to', 'walk_to'], fs_at=(1.0,))
TOWN_01A = dict(
    events=[ev(0, 0.339, 'start'), ev(5, 0.513, 'app', state='active'), ev(6, 0.542, 'app', state='visible'), ev(16, 1.552, 'app', state='occluded'),
            ev(31, 2.074, 'app', state='visible')],
    marks=[6, 70, 72, 74, 76], ops=['enter_game', 'sleep', 'mark', 'walk_to', 'walk_to'], fs_at=(1.0,))
# quit_with_name_sheet_mac_mac-app_20261103_120338_57312: the name sheet takes the key state (resign during the `sleep 2` after `fe "0 skey 36"`), then system_quit
NAME_SHEET = dict(
    events=[ev(0, 0.131, 'start'), ev(5, 0.226, 'app', state='active'), ev(6, 0.256, 'app', state='visible'), ev(20, 1.435, 'main_menu'),
            ev(56, 7.791, 'app', state='resign'), ev(57, 9.698, 'quit', why='x'), ev(58, 9.700, 'app', state='active')],
    marks=[7, 20, 27, 33, 33, 37, 39, 39, 39, 43, 43, 43, 53, 53, 53, 53, 56],
    ops=['expect', 'sleep', 'press', 'sleep', 'press', 'press', 'expect_state', 'sleep', 'press', 'expect_state', 'sleep', 'press', 'expect_state', 'sleep', 'fe', 'sleep', 'system_quit'], fs_at=())


def det(fx, front=True):
    return scenario.disturbed(fx['events'], fx['marks'], fx['ops'], front, fx['fs_at'])


class BgRunTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='scenario bg ')
        self.dir = Path(self.temp.name)
        self.saved = os.environ.get('DM_TEST_BGRUN'); os.environ.pop('DM_TEST_BGRUN', None)

    def tearDown(self):
        if self.saved is None: os.environ.pop('DM_TEST_BGRUN', None)
        else: os.environ['DM_TEST_BGRUN'] = self.saved
        self.temp.cleanup()

    def scn(self, text, name='t.scn'):
        p = self.dir / name; p.write_text(text, encoding='utf-8'); return str(p)

    # ---- the header and the value each run gets
    def test_front_header(self):
        meta, _, _ = scenario.parse(self.scn('front visibility timing\nping\n'))
        self.assertEqual(meta['front'], 'visibility timing')
        self.assertEqual(scenario.parse(self.scn('ping\n'))[0]['front'], '')
        for bad in ('front\nping\n', 'front focus\nenv DM_TEST_BGRUN=1\nping\n'):
            with self.assertRaises(scenario.ScenarioError, msg=bad): scenario.parse(self.scn(bad, 'b.scn'))
        out = io.StringIO()
        with contextlib.redirect_stdout(out): rc = scenario.main(['check', self.scn('front\nping\n', 'c.scn')])
        self.assertEqual(rc, 1); self.assertIn('SCENARIO-CHECK c.scn RED', out.getvalue())

    def test_bgrun_value(self):
        def run(text, backend='cli', headless=False, env=None):
            if env is None: os.environ.pop('DM_TEST_BGRUN', None)
            else: os.environ['DM_TEST_BGRUN'] = env
            r = scenario.Runner(self.scn(text), backend, str(self.dir / 'out'), headless)
            return r.bgrun, r.front, r.backend.bgrun
        self.assertEqual(run('ping\n'), ('1', False, '1'))                         # a windowed cli run is a background run by default
        self.assertEqual(run('ping\n', 'mac-app'), ('1', False, '1'))              # so is the Mac App
        self.assertEqual(run('front focus\nping\n'), ('0', True, '0'))             # the front header
        self.assertEqual(run('ping\n', env='0'), ('0', True, '0'))                 # --front / bench: DM_TEST_BGRUN=0 in the runner's environment
        self.assertEqual(run('env DM_TEST_BGRUN=1\nping\n', env='0'), ('1', False, '1'))   # the scenario's env wins over the runner's
        self.assertEqual(run('ping\n', headless=True), (None, False, None))       # headless: nothing set
        self.assertEqual(run('window 0\nping\n'), (None, False, None))
        self.assertEqual(run('ping\n', 'device'), (None, False, None))
        self.assertEqual(run('ping\n', 'ios-mac'), ('0', True, '0'))              # the iOS package on the Mac takes the front anyway: always a front run
        self.assertEqual(run('ping\n', 'ios-mac', env='1'), ('0', True, '0'))
        # a capture of the presented picture (graphical analysis, README images) is a front run: the real game window, in front and in real full screen
        self.assertEqual(run('env DM_TEST_PRESENT_SHOT=10:{run}/x.png\nping\n'), ('0', True, '0'))
        self.assertEqual(run('env DM_TEST_PRESENT_SHOT=10:{run}/x.png\nping\n', 'mac-app', env='1'), ('0', True, '0'))

    # ---- the detector, synthetic streams
    def test_detector_front(self):
        base = [ev(0, 0.1, 'start'), ev(1, 0.2, 'app', state='active')]
        ok = lambda evs, marks, ops, fs=(): scenario.disturbed(base + evs, marks, ops, True, fs)['disturbed']   # noqa: E731
        self.assertEqual(ok([ev(5, 4.0, 'app', state='resign', cause='sheet')], [1, 3], ['fe', 'sleep']), [])
        self.assertEqual(ok([ev(5, 8.0, 'app', state='resign', cause='sheet')], [1, 3, 4], ['sleep', 'sleep', 'sleep']), [])   # a sheet is the run's own, wherever
        self.assertEqual(ok([ev(5, 4.0, 'app', state='occluded')], [1, 3, 6], ['app', 'expect', 'sleep']), [])   # `app cover`, occluded during the next expect
        self.assertEqual(len(ok([ev(5, 9.0, 'app', state='occluded')], [1, 3, 4], ['app', 'expect', 'sleep'])), 1)   # two steps later: not requested
        fs = [ev(3, 1.20, 'window', full=True), ev(4, 1.25, 'app', state='occluded'), ev(5, 1.7, 'app', state='visible'), ev(6, 1.75, 'desk', what='space')]
        self.assertEqual(ok(fs, [2], ['sleep']), [])                                   # the real full-screen Space switch
        self.assertEqual(len(ok([ev(5, 5.0, 'desk', what='space')], [2], ['sleep'])), 1)
        # the previous front run's full-screen Space closing during this launch (before the first step and the first full-screen window): not a disturbance
        self.assertEqual(ok([ev(2, 0.22, 'desk', what='space'), ev(3, 1.20, 'window', full=True)], [4], ['sleep']), [])
        self.assertEqual(ok([ev(2, 0.22, 'desk', what='space'), ev(3, 1.20, 'window', full=True)], [1, 8], ['enter_game', 'sleep']), [])   # the first step began at launch
        self.assertEqual(len(ok([ev(4, 0.22, 'desk', what='space'), ev(9, 1.20, 'window', full=True)], [1, 3, 8], ['sleep', 'sleep', 'sleep'])), 1)   # in the second step: judged
        self.assertEqual(len(ok([ev(2, 0.22, 'desk', what='other_app', by='x'), ev(3, 1.20, 'window', full=True)], [4], ['sleep'])), 1)   # only a Space switch
        self.assertEqual(len(ok([ev(3, 1.20, 'window', full=True), ev(4, 8.0, 'desk', what='space')], [5], ['sleep'])), 1)   # after the full-screen window (past its grace): judged
        d = ok([ev(5, 5.0, 'desk', what='other_app', by='com.example')], [2], ['sleep'])
        self.assertEqual((len(d), d[0]['step']), (1, 1)); self.assertIn('other_app', d[0]['what'])
        self.assertEqual(len(ok([ev(5, 5.0, 'app', state='resign', cause='app')], [2], ['sleep'])), 1)
        self.assertEqual(ok([ev(5, 5.0, 'app', state='resign', cause='minimize')], [2], ['sleep']), [])
        self.assertEqual(ok([ev(5, 5.0, 'quit'), ev(6, 5.1, 'desk', what='other_app', by='x')], [4], ['quit']), [])   # after the quit started: consequences
        never = scenario.disturbed([ev(0, 0.1, 'start'), ev(1, 0.2, 'app', state='active', emu=True)], [1, 3], ['enter_game', 'sleep'], True)['disturbed']
        self.assertEqual([x['what'] for x in never], ['never active'])               # an emulated active does not count in front

    def test_detector_background(self):
        evs = [ev(0, 0.1, 'start'), ev(1, 0.2, 'app', state='active', emu=True)]
        r = scenario.disturbed(evs + [ev(2, 1.0, 'desk', what='front', by='self')], [2], ['sleep'], False)
        self.assertEqual((len(r['took_front']), r['front_by_user'], r['disturbed']), (1, [], []))   # within 3 s of launch
        r = scenario.disturbed(evs + [ev(5, 9.0, 'desk', what='front', by='self')], [2, 4], ['fe', 'sleep'], False)
        self.assertEqual(len(r['took_front']), 1)                                            # inside a requesting step's reach
        r = scenario.disturbed(evs + [ev(5, 30.0, 'desk', what='front', by='self'), ev(6, 31.0, 'desk', what='other_app'), ev(7, 31.5, 'app', state='occluded')], [2, 4], ['sleep', 'sleep'], False)
        self.assertEqual((r['took_front'], len(r['front_by_user']), r['disturbed']), ([], 1, []))   # the user's own activation: only noted

    # ---- the detector on copies of real streams
    def test_detector_fixtures(self):
        d = det(INACTIVE_PRESENT)['disturbed']
        self.assertEqual([(x['step'], x['what']) for x in d], [(3, 'app resign cause=?')])
        self.assertEqual(det(TOWN_02B)['disturbed'], []); self.assertEqual(det(TOWN_01A)['disturbed'], [])
        self.assertEqual(len(det(dict(TOWN_02B, fs_at=()))['disturbed']), 1)              # without the full-screen switch the same stream would be disturbed (occluded)
        self.assertEqual(det(NAME_SHEET)['disturbed'], [])

    # ---- redo and "not judged"
    def run_front(self, disturb):
        """A front scenario on the fake host; disturb[i] = whether attempt i sees another app take the front. Returns (res, code, stdout)."""
        path = self.scn('front focus\nping\n'); attempts = iter(disturb)
        def make(out):
            r = scenario.Runner(path, 'cli', out or str(self.dir / 'out'), False)
            fb = FakeBackend(); orig = fb.start; bad = next(attempts)
            def start():
                c = orig(); assert fb.host is not None
                c.wait_for('main_menu', 5); fb.host.ev('app', state='active')
                if bad: fb.host.ev('desk', what='other_app', by='com.example')
                c.wait_for('desk' if bad else 'app', 5)
                return c
            fb.start = start   # type: ignore[method-assign]
            r.backend = fb   # type: ignore[assignment]
            return r
        out = io.StringIO()
        with contextlib.redirect_stdout(out): res, code = scenario.run_scenario(path, 'cli', None, False, None, 2, make=make)
        return res, code, out.getvalue()

    def test_redo_then_ok(self):
        res, code, text = self.run_front([True, False])
        self.assertEqual((code, res['verdict']), (0, 'OK'), text)
        self.assertIn('SCENARIO t DISTURBED desk other_app by com.example (redo 1/2)', text)
        self.assertIn('· STEP  1 ok', text); self.assertIn('· SCENARIO t OK', text)
        sc = dmtest.Scan()
        for n, line in enumerate(text.splitlines(), 1): sc.line(line, n)
        self.assertEqual([(v['name'], v['verdict']) for v in sc.verdicts], [('SCENARIO t', 'OK')])   # only the judged attempt's bare line counts
        self.assertEqual(sc.unavailable, [])

    def test_disturbed_every_time_is_not_judged(self):
        res, code, text = self.run_front([True, True, True])
        self.assertEqual((code, res['verdict']), (3, 'UNAVAILABLE'), text)
        self.assertEqual(text.count('DISTURBED'), 2)
        sc = dmtest.Scan()
        for n, line in enumerate(text.splitlines(), 1): sc.line(line, n)
        self.assertEqual(sc.verdicts, [])
        self.assertEqual(len(sc.unavailable), 1); self.assertIn('not judged: disturbed', sc.unavailable[0])

    def test_front_skip(self):   # a front scenario for another backend (mfx3d_dyn: front, requires ios-mac) is skipped cleanly, its verdict line printed bare once
        path = self.scn('front visibility\nrequires ios-mac device\nping\n')
        out = io.StringIO()
        with contextlib.redirect_stdout(out): res, code = scenario.run_scenario(path, 'cli', None, False, None, 2, make=lambda o: scenario.Runner(path, 'cli', o or str(self.dir / 'out'), False))
        text = out.getvalue()
        self.assertEqual((code, res['verdict']), (0, 'SKIP'), text)
        self.assertIn('· SCENARIO t SKIP', text); self.assertEqual([ln for ln in text.splitlines() if ln.startswith('SCENARIO t SKIP')][:1], [res['line']])

    def test_final_code(self):
        self.assertEqual(scenario.final_rc([0, 3, 1]), 1)   # RED wins over not judged
        self.assertEqual(scenario.final_rc([0, 3]), 3)
        self.assertEqual(scenario.final_rc([0, 0]), 0)
        self.assertEqual(scenario.final_rc([]), 0)


if __name__ == '__main__':
    unittest.main()
