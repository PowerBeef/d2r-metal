"""install.py unit tests (the launcher's install engine): game-file checks, folder layouts, stamps, app replacement and revert, on synthetic files only
(no game content, no Mac needed: CI runs it on Linux; the steps that run the real tools are covered by the end-to-end install in docs/LAUNCHER.md)"""
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import install   # noqa: E402


def args(root, **kw):
    a = dict(root=str(root), runtime=None, tools=None, app_dir=None, bundle_id=None, json=True, game=None, keys=None, source=None, force=False)
    a.update(kw); return types.SimpleNamespace(**a)


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='install test ')   # a space in the path, like Application Support
        self.root = Path(self.tmp.name) / 'Install Root'

    def tearDown(self):
        self.tmp.cleanup()

    def ctx(self, **kw):
        c = install.Ctx(args(self.root, **kw)); c.json = False
        c.event = lambda **_: None
        self.addCleanup(c.log.close)
        return c

    def test_game_check_names_the_problem(self):
        c = self.ctx()
        self.assertIn('ExeFS/main is missing', install.game_ok(c) or '')
        exe = Path(c.game) / 'ExeFS'; exe.mkdir(parents=True)
        for n in install.EXEFS_SHA: (exe / n).write_bytes(b'not the game')
        self.assertIn('not from version 1.0.34.0', install.game_ok(c) or '')

    def test_extracted_folder_layouts(self):
        base = Path(self.tmp.name)
        for layout in ('flat', 'flat/Update', 'flat2/data/switch/3.3.94012/Update'):
            for part in ('ExeFS', 'RomFS'): (base / layout / part).mkdir(parents=True, exist_ok=True)
        self.assertEqual(install.find_update(str(base / 'flat')), str(base / 'flat'))
        self.assertEqual(install.find_update(str(base / 'flat2')), str(base / 'flat2/data/switch/3.3.94012/Update'))
        self.assertIsNone(install.find_update(str(base / 'nowhere')))

    def test_unpack_refuses_without_input(self):
        c = self.ctx()
        with self.assertRaises(install.Fail) as e: install.step_unpack(c, args(self.root))
        self.assertIn('no game files yet', str(e.exception))
        with self.assertRaises(install.Fail) as e: install.step_unpack(c, args(self.root, game=['/x.nsp']))
        self.assertIn('prod.keys', str(e.exception))

    def test_tree_stamp_follows_content_only(self):
        d = Path(self.tmp.name) / 'src'; (d / 'sub').mkdir(parents=True)
        (d / 'a.py').write_text('x = 1'); (d / 'sub' / 'b.py').write_text('y = 2'); (d / 'sub' / '__pycache__').mkdir(); (d / 'sub' / '__pycache__' / 'b.pyc').write_bytes(b'junk')
        s1 = install.tree_sha([str(d)])
        (d / 'sub' / '__pycache__' / 'b.pyc').write_bytes(b'other junk'); os.utime(d / 'a.py', (0, 0))
        self.assertEqual(install.tree_sha([str(d)]), s1)            # bytecode and times don't count
        (d / 'a.py').write_text('x = 2')
        self.assertNotEqual(install.tree_sha([str(d)]), s1)

    def test_info_plist(self):
        c = self.ctx(bundle_id='org.example.test')
        p = install.info_plist(c, {'version': '1.2.0', 'build': 412, 'macos_min': '27.0', 'update_url': 'https://github.com/x/y/releases/latest/download/latest_mac.json'})
        self.assertEqual((p['CFBundleIdentifier'], p['CFBundleVersion'], p['CFBundleExecutable']), ('org.example.test', '412', 'D2RMobile'))
        self.assertTrue(p['DMLauncherManaged'])
        self.assertIn('SWUpdateURL', p)
        self.assertNotIn('SWUpdateURL', install.info_plist(c, {'version': '1', 'build': 1}))

    def fake_app(self, where, managed_=True, marker='a'):
        app = Path(where) / install.APP_NAME; (app / 'Contents').mkdir(parents=True)
        with open(app / 'Contents' / 'Info.plist', 'wb') as f: plistlib.dump({'DMLauncherManaged': managed_, 'M': marker}, f)
        return app

    def marker(self, app):
        with open(Path(app) / 'Contents' / 'Info.plist', 'rb') as f: return plistlib.load(f)['M']

    def test_install_app_keeps_previous_and_refuses_foreign(self):
        apps = Path(self.tmp.name) / 'Apps'; c = self.ctx(app_dir=str(apps))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's1', marker='old')))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's2', marker='new')))
        self.assertEqual(self.marker(apps / install.APP_NAME), 'new')
        self.assertEqual(self.marker(Path(c.build) / 'previous' / install.APP_NAME), 'old')
        # revert swaps them back, and again
        c.state['release'], c.state['previous_release'] = {'version': '2'}, {'version': '1'}
        self.assertEqual(install.cmd_revert(c, None), 0)
        self.assertEqual(self.marker(apps / install.APP_NAME), 'old')
        self.assertEqual(c.state['release'], {'version': '1'})
        self.assertEqual(install.cmd_revert(c, None), 0)
        self.assertEqual(self.marker(apps / install.APP_NAME), 'new')
        # an app the launcher did not install, or a link, is never replaced
        other = Path(self.tmp.name) / 'Other'
        self.fake_app(other, managed_=False)
        with self.assertRaises(install.Fail): install.install_app(self.ctx(app_dir=str(other)), str(self.fake_app(Path(self.tmp.name) / 's3')))
        link = Path(self.tmp.name) / 'Linked'; link.mkdir(); (link / install.APP_NAME).symlink_to(apps / install.APP_NAME)
        self.assertFalse(install.managed(str(link / install.APP_NAME)))
        with self.assertRaises(install.Fail): install.install_app(self.ctx(app_dir=str(link)), str(self.fake_app(Path(self.tmp.name) / 's4')))

    def test_state_survives_a_new_run(self):
        c = self.ctx(); c.state['steps']['product'] = 'r49 abc'; c.save_state()
        self.assertEqual(json.loads(Path(c.state_path).read_text())['steps']['product'], 'r49 abc')
        self.assertEqual(self.ctx().state['steps']['product'], 'r49 abc')


if __name__ == '__main__':
    unittest.main()
