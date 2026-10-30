"""install.py unit tests (the launcher's install engine): game-file checks, folder layouts, stamps, app replacement and revert, on synthetic files only
(no game content, no Mac needed: CI runs it on Linux; the steps that run the real tools are covered by the end-to-end install in docs/LAUNCHER.md)"""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
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
        self.assertEqual(self.marker(Path(c.build) / 'previous.noindex' / install.APP_NAME), 'old')
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

    def test_failure_names_the_error_in_english(self):
        tail = ['[12:00:00] 从游戏包的票据取到 1 个标题密钥 | took 1 title keys from the tickets in the game packages',
                '✗ 没找到本体（两个包都要给） | the base game is missing (give both packages)；hactool 读内容文件时报错 | hactool error: Error: bad key!']
        self.assertEqual(install.failure('extract_game.sh', 1, tail),
                         'the base game is missing (give both packages); hactool error: Error: bad key! (extract_game.sh exited with 1)')
        self.assertEqual(install.failure('x.py', 2, ['plain line', 'last line']), 'plain line | last line (x.py exited with 2)')

    def test_extract_merges_title_keys_once(self):
        # A title.keys that already holds a ticket's key (saved from the same package) must not reach hactool twice: it refuses the whole list.
        # A fake hactool unpacks one ticket and fails the content step on a repeated rights ID, as the real one does; made-up keys only
        t = Path(self.tmp.name); home = t / 'home'; (home / '.switch').mkdir(parents=True)
        rid, key = bytes.fromhex('0100000000001000000000000000000a'), bytes(range(16))
        tik = bytearray(0x2c0); tik[0:4] = b'\x04\x00\x01\x00'; tik[0x180:0x190] = key; tik[0x2a0:0x2b0] = rid   # RSA-2048 common ticket
        (t / 'fake.tik').write_bytes(bytes(tik))
        (home / '.switch' / 'title.keys').write_text(f'{rid.hex().upper()} = {key.hex()}\nfeed0000000000000000000000000001 = {"11" * 16}')
        (t / 'game.nsp').write_bytes(b'PFS0'); (t / 'prod.keys').write_text(f'header_key = {"00" * 32}\n')
        ht = t / 'hactool'
        ht.write_text('#!/bin/bash\nfor a in "$@"; do case "$a" in --outdir=*) out=${a#--outdir=};; esac; done\n'
                      'case " $* " in\n  *" pfs0 "*) cp "$FAKE_TIK" "$out/x.tik"; : > "$out/a.nca";;\n'
                      '  *" nca "*) [ -z "$(sed -n "s/[[:space:]]*=.*//p" "$HOME/.switch/title.keys" | tr A-Z a-z | sort | uniq -d)" ] || '
                      '{ echo "Error: Rights ID already has a corresponding titlekey!"; exit 1; };;\nesac\n')
        ht.chmod(0o755)
        env = dict(os.environ, HOME=str(home), HACTOOL=str(ht), DM_ROOT=str(t / 'ws'), FAKE_TIK=str(t / 'fake.tik'))
        r = subprocess.run(['bash', str(Path(__file__).with_name('extract_game.sh')), str(t / 'game.nsp'), str(t / 'prod.keys')],
                           env=env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn('took 1 title keys', r.stdout)
        self.assertIn('the base game is not in the game package', r.stderr)   # the fake holds no program content: the script gets this far
        self.assertNotIn('hactool error', r.stderr)

    def test_install_app_moves_an_app_from_its_old_folder(self):
        # an install made before the launcher kept the game in its own folder: the copy in the old folder becomes the previous version
        old_dir = Path(self.tmp.name) / 'Applications'; c = self.ctx(app_dir=str(Path(self.tmp.name) / 'Game'))
        c.state['app'] = str(self.fake_app(old_dir, marker='old'))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's1', marker='new')))
        self.assertEqual(self.marker(Path(c.app_dir) / install.APP_NAME), 'new')
        self.assertEqual(self.marker(Path(c.build) / 'previous.noindex' / install.APP_NAME), 'old')
        self.assertFalse((old_dir / install.APP_NAME).exists())
        # an app at the old place that the launcher did not install stays where it is
        mine = self.fake_app(Path(self.tmp.name) / 'Mine', managed_=False, marker='mine'); c.state['app'] = str(mine)
        shutil.rmtree(Path(c.app_dir) / install.APP_NAME)
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's2', marker='newer')))
        self.assertEqual(self.marker(mine), 'mine')

    def test_game_folder_is_the_default(self):
        self.assertEqual(self.ctx().app_dir, str(self.root / 'Game.noindex'))

    def test_previous_from_an_older_engine_moves_out_of_app_search(self):
        c = self.ctx(); self.fake_app(Path(c.build) / 'previous', marker='kept')
        self.assertEqual(self.marker(install.previous_app(c)), 'kept')
        self.assertFalse((Path(c.build) / 'previous').exists())

    def test_state_survives_a_new_run(self):
        c = self.ctx(); c.state['steps']['product'] = 'r49 abc'; c.save_state()
        self.assertEqual(json.loads(Path(c.state_path).read_text())['steps']['product'], 'r49 abc')
        self.assertEqual(self.ctx().state['steps']['product'], 'r49 abc')


if __name__ == '__main__':
    unittest.main()
