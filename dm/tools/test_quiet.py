"""Unit tests for quiet.py: a held lock makes a heavy tool wait, a stale or absent one does not, and the two overrides skip the wait."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quiet  # noqa: E402


class QuietTest(unittest.TestCase):
    def setUp(self):
        self.env = {k: os.environ.pop(k, None) for k in ('DM_QUIET_SKIP', 'XT_LOCK_HELD')}

    def tearDown(self):
        for k, v in self.env.items():
            if v is not None: os.environ[k] = v

    def lock(self, d, pid):
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'pid'), 'w') as f: f.write(str(pid))
        with open(os.path.join(d, 'owner'), 'w') as f: f.write('test holder')

    def test_no_lock_no_wait(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertIsNone(quiet.holder(os.path.join(t, 'none')))
            self.assertEqual(quiet.wait_quiet('x', lock=os.path.join(t, 'none')), 0.0)

    def test_stale_lock_no_wait(self):
        with tempfile.TemporaryDirectory() as t:
            self.lock(t, 2 ** 22 + 12345)   # no such process
            self.assertIsNone(quiet.holder(t))

    def test_held_lock_waits_until_released(self):
        with tempfile.TemporaryDirectory() as t:
            self.lock(t, os.getpid()); n = []
            def fake_sleep(s):
                n.append(s)
                if len(n) == 3: os.remove(os.path.join(t, 'pid'))   # released on the third look
            self.assertEqual(quiet.holder(t), (os.getpid(), 'test holder'))
            self.assertEqual(quiet.wait_quiet('x', lock=t, poll=1.0, sleep=fake_sleep), 3.0)

    def test_overrides(self):
        with tempfile.TemporaryDirectory() as t:
            self.lock(t, os.getpid())
            for k in ('DM_QUIET_SKIP', 'XT_LOCK_HELD'):
                os.environ[k] = '1'; self.assertEqual(quiet.wait_quiet('x', lock=t), 0.0); del os.environ[k]


if __name__ == '__main__':
    unittest.main()
