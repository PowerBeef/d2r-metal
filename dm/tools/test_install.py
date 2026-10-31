"""install.py unit tests (the launcher's install engine): game-file checks, folder layouts, stamps, app replacement and revert, the lock, stopping and
hung tools, clean-up, on synthetic files only (no game content, no Mac needed: CI runs it on Linux; the steps that run the real tools are covered by
the end-to-end install in docs/LAUNCHER.md)"""
import contextlib
import io
import json
import os
import re
import signal
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
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
        c.events = []
        c.event = lambda **ev: c.events.append(ev)
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

    def fake_app(self, where, managed_=True, marker='a', build=1, paths=None, chain=None):
        app = Path(where) / install.APP_NAME; (app / 'Contents' / 'Resources').mkdir(parents=True)
        with open(app / 'Contents' / 'Info.plist', 'wb') as f:
            plistlib.dump({'DMLauncherManaged': managed_, 'M': marker, 'CFBundleVersion': str(build), 'CFBundleShortVersionString': f'0.{build}'}, f)
        if paths: (app / 'Contents' / 'Resources' / 'paths.txt').write_text(''.join(f'{k}={v}\n' for k, v in paths.items()))
        if chain: (app / 'Contents' / 'Resources' / 'c1_full.tsv').write_text(f'# chain b-{chain} 2026-10-31\n')
        return app

    def marker(self, app):
        with open(Path(app) / 'Contents' / 'Info.plist', 'rb') as f: return plistlib.load(f)['M']

    def test_install_app_keeps_previous_and_refuses_foreign(self):
        apps = Path(self.tmp.name) / 'Apps'; c = self.ctx(app_dir=str(apps))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's1', marker='old', build=1)), 1)
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's2', marker='new', build=2)), 2)
        self.assertEqual(self.marker(apps / install.APP_NAME), 'new')
        self.assertEqual(self.marker(Path(c.build) / 'previous.noindex' / install.APP_NAME), 'old')
        # the state names what is installed, read from the apps themselves
        self.assertEqual((c.state['release']['build'], c.state['previous_release']['build']), (2, 1))
        # revert swaps them back, and again; the release reverted from is kept for the launcher
        self.assertEqual(install.cmd_revert(c, None), 0)
        self.assertEqual(self.marker(apps / install.APP_NAME), 'old')
        self.assertEqual((c.state['release']['build'], c.state['previous_release']['build'], c.state['reverted_from']), (1, 2, 2))
        self.assertEqual(install.cmd_revert(c, None), 0)
        self.assertEqual(self.marker(apps / install.APP_NAME), 'new')
        self.assertNotIn('reverted_from', c.state)
        # an app the launcher did not install, or a link, is never replaced
        other = Path(self.tmp.name) / 'Other'
        self.fake_app(other, managed_=False)
        with self.assertRaises(install.Fail): install.install_app(self.ctx(app_dir=str(other)), str(self.fake_app(Path(self.tmp.name) / 's3')), 1)
        link = Path(self.tmp.name) / 'Linked'; link.mkdir(); (link / install.APP_NAME).symlink_to(apps / install.APP_NAME)
        self.assertFalse(install.managed(str(link / install.APP_NAME)))
        with self.assertRaises(install.Fail): install.install_app(self.ctx(app_dir=str(link)), str(self.fake_app(Path(self.tmp.name) / 's4')), 1)

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
        c.state['app'] = str(self.fake_app(old_dir, marker='old', build=1))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's1', marker='new', build=2)), 2)
        self.assertEqual(self.marker(Path(c.app_dir) / install.APP_NAME), 'new')
        self.assertEqual(self.marker(Path(c.build) / 'previous.noindex' / install.APP_NAME), 'old')
        self.assertFalse((old_dir / install.APP_NAME).exists())
        # an app at the old place that the launcher did not install stays where it is
        mine = self.fake_app(Path(self.tmp.name) / 'Mine', managed_=False, marker='mine'); c.state['app'] = str(mine)
        shutil.rmtree(Path(c.app_dir) / install.APP_NAME)
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's2', marker='newer', build=3)), 3)
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


    def test_same_build_again_keeps_the_previous_version(self):
        # a repair (or a resumed install) of the build already installed replaces it, and the version before it stays the previous one
        apps = Path(self.tmp.name) / 'Apps'; c = self.ctx(app_dir=str(apps))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's1', marker='v1', build=1)), 1)
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's2', marker='v2', build=2)), 2)
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's3', marker='v2 again', build=2)), 2)
        self.assertEqual(self.marker(apps / install.APP_NAME), 'v2 again')
        self.assertEqual(self.marker(Path(c.build) / 'previous.noindex' / install.APP_NAME), 'v1')
        self.assertFalse((Path(c.build) / 'replaced.tmp').exists())

    def test_previous_packs_follow_the_previous_app(self):
        apps = Path(self.tmp.name) / 'Apps'; c = self.ctx(app_dir=str(apps))
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's1', build=1)), 1); c.state['packs'] = {'runtime': 'r1', 'tools': 't1'}
        install.install_app(c, str(self.fake_app(Path(self.tmp.name) / 's2', build=2)), 2); c.state['packs'] = {'runtime': 'r2', 'tools': 't2'}
        self.assertEqual(c.state['previous_packs'], {'runtime': 'r1', 'tools': 't1'})
        install.cmd_revert(c, None)
        self.assertEqual((c.state['packs']['runtime'], c.state['previous_packs']['runtime']), ('r1', 'r2'))

    def test_revert_cut_short_is_put_back(self):
        apps = Path(self.tmp.name) / 'Apps'; c = self.ctx(app_dir=str(apps)); tmp = Path(c.build) / 'revert.tmp'
        prev = Path(c.build) / 'previous.noindex'
        for case in ('after the first move', 'after the second move', 'only the copy'):
            for d in (apps, prev, tmp): shutil.rmtree(d, ignore_errors=True)
            if case == 'after the first move':      # installed → tmp done; previous still in place
                self.fake_app(prev, marker='prev'); shutil.move(str(self.fake_app(Path(self.tmp.name) / 'x1', marker='cur')), str(tmp))
            elif case == 'after the second move':   # previous → installed done; tmp not yet back
                self.fake_app(apps, marker='prev'); shutil.move(str(self.fake_app(Path(self.tmp.name) / 'x2', marker='cur')), str(tmp))
            else:
                shutil.move(str(self.fake_app(Path(self.tmp.name) / 'x3', marker='cur')), str(tmp))
            install.recover_revert(c)
            self.assertFalse(tmp.exists(), case)
            self.assertTrue((apps / install.APP_NAME).is_dir(), case)
            if case != 'only the copy':
                self.assertEqual({self.marker(apps / install.APP_NAME), self.marker(prev / install.APP_NAME)}, {'prev', 'cur'}, case)

    def test_romfs_sizes_match_the_host(self):
        src = (Path(__file__).resolve().parents[1] / 'host' / 'romfs_validate.c').read_text()
        names = re.findall(r'"(\d\d-\d{8}\.data)"', src.split('static const char *need[]', 1)[1].split('};', 1)[0])
        sizes = [int(x) for x in re.findall(r'\d+', src.split('static const long long size[]', 1)[1].split('};', 1)[0])]
        self.assertEqual(dict(zip(names, sizes)), install.ROMFS_SIZES)

    def test_romfs_check_finds_short_archives(self):
        data = Path(self.tmp.name) / 'data'; data.mkdir()
        self.assertIn('build_manifest.psv is missing', install.romfs_ok(str(data)))
        (data / 'build_manifest.psv').write_text('x')
        for name, size in install.ROMFS_SIZES.items():
            with open(data / name, 'wb') as f: f.truncate(size)   # sparse: no disk used
        self.assertIsNone(install.romfs_ok(str(data)))
        with open(data / '04-00000002.data', 'r+b') as f: f.truncate(1000)
        self.assertIn('04-00000002.data is damaged', install.romfs_ok(str(data)))
        with open(data / '04-00000002.data', 'r+b') as f: f.truncate(install.ROMFS_SIZES['04-00000002.data'])
        (data / '17-00000001.data').unlink()
        self.assertIn('is missing', install.romfs_ok(str(data)))

    def test_damaged_game_files_need_new_input(self):
        c = self.ctx(); (Path(c.game) / 'ExeFS').mkdir(parents=True)
        with self.assertRaises(install.Fail) as e: install.step_unpack(c, args(self.root))
        self.assertEqual(e.exception.code, 'game_damaged')
        self.assertTrue(Path(c.game).exists())   # nothing deleted without new files to replace it

    def test_one_engine_per_root(self):
        a, b = self.ctx(), self.ctx()
        self.assertTrue(a.take_lock())
        self.assertFalse(b.take_lock())
        a.lock.close()   # released (the kernel also releases it when the process dies, however it dies)
        self.assertTrue(b.take_lock()); b.lock.close()

    def test_busy_root_is_an_error_event(self):
        holder = self.ctx(); self.assertTrue(holder.take_lock()); self.addCleanup(holder.lock.close)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.handlers():
            rc = install.main(['revert', '--root', str(self.root), '--json'])
        ev = [json.loads(l) for l in out.getvalue().splitlines()]
        self.assertEqual(rc, 1)
        self.assertEqual(ev[-1]['code'], 'busy')

    @contextlib.contextmanager
    def handlers(self):
        old = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        try: yield
        finally:
            for s, h in old.items(): signal.signal(s, h)

    def test_stop_request_stops_the_whole_tool(self):
        # a stop (here SIGTERM to the engine) stops the tool and what it started, before the engine goes on; no process is left behind
        c = self.ctx(); c.open_log(); self.addCleanup(c.log.close)
        mark = Path(self.tmp.name) / 'pid'
        with self.handlers():
            for s in (signal.SIGINT, signal.SIGTERM): signal.signal(s, c.on_signal)
            threading.Timer(1.0, lambda: os.kill(os.getpid(), signal.SIGTERM)).start()
            t0 = time.monotonic()
            with self.assertRaises(install.Interrupted):
                c.run('x', ['bash', '-c', f'sleep 60 & echo $! > "{mark}"; echo started; wait'], env=dict(os.environ))
            self.assertLess(time.monotonic() - t0, 15)
        child = int(mark.read_text())
        time.sleep(0.2)
        self.assertFalse(install.pid_alive(child) and not self.zombie(child), 'the tool\'s own child was left running')

    def zombie(self, pid):
        r = subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)], capture_output=True, text=True)
        return r.stdout.strip().startswith('Z') or not r.stdout.strip()

    def test_signal_waits_for_a_critical_move(self):
        c = self.ctx(); done = []
        with self.handlers():
            signal.signal(signal.SIGINT, c.on_signal)
            with self.assertRaises(install.Interrupted):
                with c.critical():
                    os.kill(os.getpid(), signal.SIGINT); time.sleep(0.1)
                    done.append('the move finished')
        self.assertEqual(done, ['the move finished'])

    def test_a_silent_tool_counts_as_hung(self):
        c = self.ctx(); c.open_log(); self.addCleanup(c.log.close)
        old = install.QUIET_MIN; install.QUIET_MIN = {'hang': 1 / 60}; self.addCleanup(setattr, install, 'QUIET_MIN', old)
        threading.Thread(target=c.watch, args=(0,), daemon=True).start()
        t0 = time.monotonic()
        with self.assertRaises(install.Fail) as e: c.run('hang', ['sleep', '60'], env=dict(os.environ))
        self.assertEqual(e.exception.code, 'stalled')
        self.assertLess(time.monotonic() - t0, 20)

    def test_write_atomic_replaces_in_one_move(self):
        d = Path(self.tmp.name) / 'out'
        install.write_atomic(str(d), lambda p: (os.makedirs(p), Path(p, 'f').write_text('1')))
        held = open(d / 'f')   # an installed app reading the old copy keeps reading it
        install.write_atomic(str(d), lambda p: (os.makedirs(p), Path(p, 'f').write_text('2')))
        self.assertEqual((d / 'f').read_text(), '2'); self.assertEqual(held.read(), '1'); held.close()
        self.assertEqual(sorted(os.listdir(self.tmp.name)), ['out'])
        with self.assertRaises(RuntimeError): install.write_atomic(str(d), lambda p: (os.makedirs(p), (_ for _ in ()).throw(RuntimeError('cut'))))
        self.assertEqual((d / 'f').read_text(), '2')   # a failed rebuild leaves the finished one

    def test_sweep_clears_what_a_killed_run_left(self):
        c = self.ctx(); cache = Path(c.build) / 'cache' / 'dm'; cache.mkdir(parents=True)
        dead = subprocess.Popen(['true']); dead.wait()
        (cache / f'extract.{dead.pid}').mkdir(); (cache / f'extract.{os.getpid()}').mkdir()
        tmp = Path(self.tmp.name) / 'T'; (tmp / 'dm-keys.dead').mkdir(parents=True); (tmp / 'dm-keys.live').mkdir()
        (tmp / 'dm-keys.dead' / '.owner').write_text(str(dead.pid)); (tmp / 'dm-keys.live' / '.owner').write_text(str(os.getpid()))
        Path(c.game + '.part').mkdir(parents=True)
        for half in ('products/r49-touch-abc.part', 'overlay/x.old', 'tables/t.part'): (Path(c.build) / half).mkdir(parents=True)
        (Path(c.build) / 'c1_full.tsv.part').write_text('half')
        old = os.environ.get('TMPDIR'); os.environ['TMPDIR'] = str(tmp)
        try: install.sweep(c)
        finally:
            if old is None: del os.environ['TMPDIR']
            else: os.environ['TMPDIR'] = old
        self.assertEqual(sorted(os.listdir(cache)), [f'extract.{os.getpid()}'])
        self.assertEqual(sorted(os.listdir(tmp)), ['dm-keys.live'])
        self.assertFalse(Path(c.game + '.part').exists())
        self.assertEqual([p for p in Path(c.build).rglob('*') if p.name.endswith(('.part', '.old'))], [])

    def test_prune_keeps_what_the_apps_use(self):
        apps = Path(self.tmp.name) / 'Apps'; c = self.ctx(app_dir=str(apps)); b = Path(c.build)
        dirs = {n: b / n for n in ('products/r49-touch-aaaa', 'products/r49-touch-bbbb', 'products/r49-touch-cccc', 'overlay/o1', 'overlay/o2', 'overlay/o3',
                                   'tables/t1', 'tables/t2', 'cache/dm/out/3.3.94012-r49-touch', 'cache/dm/out/3.3.94012-r49-rules',
                                   'cache/metal_shader_cache/b-111111111111', 'cache/metal_shader_cache/b-222222222222', 'cache/metal_shader_cache/b-333333333333')}
        for d in dirs.values(): d.mkdir(parents=True)
        for v in ('0.9.6', '0.9.10', '0.9.11'): (self.root / 'releases' / v / 'runtime').mkdir(parents=True)
        for v in ('1', '2', '3'): (self.root / 'tools' / v).mkdir(parents=True)
        cas = b / 'cache' / 'metal_shader_cache' / 'cas'; (cas / 'index').mkdir(parents=True)
        for dg in ('d1', 'd2', 'old'): (cas / 'shader' / dg).mkdir(parents=True)
        (cas / 'index' / 'k1.json').write_text('{"digest": "d1"}'); (cas / 'index' / 'k2.json').write_text('{"digest": "d2"}')
        self.fake_app(apps, build=11, chain='222222222222', paths={'product': dirs['products/r49-touch-cccc'], 'romfs_overlay': dirs['overlay/o3'] / 'romfs_overlay', 'tables': dirs['tables/t2']})
        self.fake_app(b / 'previous.noindex', build=6, chain='111111111111', paths={'product': dirs['cache/dm/out/3.3.94012-r49-touch'], 'romfs_overlay': dirs['overlay/o1'] / 'romfs_overlay', 'tables': dirs['tables/t2']})
        c.state.update(app=str(apps / install.APP_NAME), packs={'runtime': str(self.root / 'releases/0.9.11/runtime'), 'tools': str(self.root / 'tools/3')},
                       previous_packs={'runtime': str(self.root / 'releases/0.9.6/runtime'), 'tools': str(self.root / 'tools/1')})
        logs = self.root / 'logs'; logs.mkdir()
        for i in range(25): (logs / f'install-202610{i:02d}-000000.log').write_text('x')
        install.prune(c)
        left = {n for n, d in dirs.items() if d.exists()}
        self.assertEqual(left, {'products/r49-touch-cccc', 'overlay/o1', 'overlay/o3', 'tables/t2', 'cache/dm/out/3.3.94012-r49-touch',
                                'cache/dm/out/3.3.94012-r49-rules', 'cache/metal_shader_cache/b-111111111111', 'cache/metal_shader_cache/b-222222222222'})
        self.assertEqual(sorted(os.listdir(self.root / 'releases')), ['0.9.11', '0.9.6'])
        self.assertEqual(sorted(os.listdir(self.root / 'tools')), ['1', '3'])
        self.assertEqual(sorted(os.listdir(cas / 'shader')), ['d1', 'd2'])
        self.assertEqual(len(os.listdir(logs)), install.KEEP_LOGS)

    def test_prune_never_touches_a_developer_workspace(self):
        c = self.ctx(); (Path(c.build) / 'cache' / 'dm' / 'venv').mkdir(parents=True); (self.root / 'releases' / 'x').mkdir(parents=True)
        install.prune(c)
        self.assertTrue((self.root / 'releases' / 'x').exists())

    def test_unexpected_errors_are_explained(self):
        self.assertEqual(install.unexpected(OSError(28, 'No space left on device'))[1], 'disk')
        self.assertEqual(install.unexpected(PermissionError(13, 'Permission denied', '/x'))[1], 'access')
        self.assertIn('ValueError', install.unexpected(ValueError('odd'))[0])

    def test_status_reports_game_files_and_apps(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out): install.main(['status', '--root', str(self.root), '--json'])
        s = json.loads(out.getvalue())
        self.assertEqual((s['game_files'], s['game_present'], s['app_present'], s['protocol']), (False, False, False, install.PROTOCOL))


if __name__ == '__main__':
    unittest.main()
