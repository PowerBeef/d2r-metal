"""Unit tests for workspace_prune.py: which entries under run/ are prune candidates. A temporary $DM_ROOT, no real workspace is touched."""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import workspace_prune  # noqa: E402

OLD = time.time() - 30 * 86400


class PruneTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='prune ')
        self.root = Path(self.temp.name)
        self.saved = workspace_prune.ROOT
        workspace_prune.ROOT = str(self.root)

    def tearDown(self):
        workspace_prune.ROOT = self.saved
        self.temp.cleanup()

    def put(self, rel, text='x'):
        p = self.root / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text, encoding='utf-8'); return p

    def age_all(self, fresh=()):
        """Every file, folder and link 30 days old (bottom-up, so folder times stay old), except the trees named in fresh."""
        for d, dirs, files in os.walk(self.root, topdown=False):
            for n in files + dirs:
                os.utime(os.path.join(d, n), (OLD, OLD), follow_symlinks=False)
        for rel in fresh:
            for d, dirs, files in os.walk(self.root / rel):
                for n in files + dirs: os.utime(os.path.join(d, n), None, follow_symlinks=False)
            os.utime(self.root / rel, None)

    def test_only_old_run_folders_and_old_baselines(self):
        for rel in ('run/bench/ledger.tsv', 'run/bootprof/history.tsv', 'run/ios/smoke_ledger.tsv', 'run/ios/devlock_hist.tsv',
                    'run/ios/battery.log', 'run/ios/device_low_battery', 'run/release/1.1.2.log'):   # ledgers, histories, markers: plain files at area level
            self.put(rel)
        (self.root / 'run/ios/macsmoke.queue').mkdir(parents=True); (self.root / 'run/ios/device.lock').mkdir(parents=True)   # lock and queue folders
        self.put('run/bench/old_run/a')
        self.put('run/bench/kept_run/.keep', ''); self.put('run/bench/kept_run/b')
        self.put('run/chipaware/.keep', ''); self.put('run/chipaware/x.json'); self.put('run/chipaware/sub/y')
        self.put('run/bench/baseline/aaa/host/dmhost'); self.put('run/bench/baseline/bbb/host/dmhost')
        os.symlink('aaa', self.root / 'run/bench/baseline/current')
        self.put('run/scenario/new_run/c')
        self.age_all(fresh=('run/scenario/new_run',))
        got = {os.path.relpath(p, self.root): why for p, why in workspace_prune.candidates(7)}
        self.assertEqual(sorted(got), ['run/bench/baseline/bbb', 'run/bench/old_run'], got)
        self.assertEqual(got['run/bench/baseline/bbb'], 'older bench baseline')

    def test_keep_and_fresh_baselines(self):   # a .keep in a baseline keeps it; a baseline younger than --days stays; with no current link every old one goes
        self.put('run/bench/baseline/old1/host/dmhost'); self.put('run/bench/baseline/old2/.keep', ''); self.put('run/bench/baseline/new1/host/dmhost')
        self.age_all(fresh=('run/bench/baseline/new1',))
        got = sorted(os.path.relpath(p, self.root) for p, _ in workspace_prune.candidates(7))
        self.assertEqual(got, ['run/bench/baseline/old1'])


if __name__ == '__main__':
    unittest.main()
