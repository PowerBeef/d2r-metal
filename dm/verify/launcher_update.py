#!/usr/bin/env python3
"""Checks a launcher update end to end on a copy of a player's install, before a release goes out (docs/LAUNCHER.md "Testing a release").

  1. Clones the install root (APFS clones: instant, no extra space) to $DM_ROOT/run/launcher_update/<time>/root and moves the paths in the
     copy's state.json to it. The player's install, game app, saves and /Applications are only read, and are checked unchanged at the end.
  2. Runs the launcher's own executable on the copy, as its Update button does: DM_LAUNCHER_ROOT=<copy>, DM_LAUNCHER_FEED=<feed>,
     DM_LAUNCHER_BUNDLE_ID=org.d2rmobile.mac.updatetest (its own Metal cache and settings), DM_LAUNCHER_AUTORUN=update. It downloads and
     checks the release's packs, runs the install engine and reports back.
  3. Checks the engine's steps, state.json, the new app (version, icon, signature, host, paths) and the previous version kept for revert.
  4. Checks the updated game: opened directly it hands over to the launcher (an inert stand-in under a test id the game is told to look for
     with DM_TEST_LAUNCHER_ID, never the installed launcher, which would open on the player's install); a scenario (enter_game) runs in game on it.
  5. The hard cases, on the copy: a Repair stopped as Stop does (the engine gets SIGINT): nothing of it keeps running and the lock is free;
     a Repair whose launcher is killed outright: the engine stops its tools by itself, and the next run sweeps what was left and resumes;
     a Repair while the game runs: refused until the game quits; an unreachable feed: said plainly, nothing changed.
  6. Goes back to the previous version (the engine's revert, as the gear menu does) and checks the old app is back.
  7. Cleans up: the copy's apps unregistered, the copy deleted (--keep keeps it), the test bundle's caches and settings removed; the player's
     install, the installed launcher and what macOS opens for the launcher's id are checked unchanged.
With --clean-mac the launcher runs in a sandbox that hides Homebrew, /usr/local, MacPorts, the python.org framework, pyenv, conda, Xcode and
the Command Line Tools (what a Mac without them sees), the copy's shader cache is emptied first so the update translates every shader again,
and a Repair then redoes every step but unpacking in the same sandbox: proof that the packs carry everything they need.

Usage: launcher_update.py --feed file:///…/manifest.json [--root <install root>] [--launcher <D2R Metal Launcher.app>] [--clean-mac] [--keep] [--no-game]
                          [--no-hard]   (skips step 5)
Last lines: LAUNCHER-UPDATE OK|RED, then RESULT {json}. Needs the workspace venv for the scenario step.
"""
import argparse, datetime, fcntl, glob, hashlib, json, os, plistlib, shutil, signal, subprocess, sys, tempfile, time, urllib.request
from typing import Any

DM = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DM_ROOT = os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work'))
TEST_ID = 'org.d2rmobile.mac.updatetest'
STAND_IN_ID = 'org.d2rmetal.launcher.handovertest'   # the stand-in launcher's id (DM_TEST_LAUNCHER_ID): never the real launcher's
LAUNCHER_ID = 'org.d2rmetal.launcher'
APP = 'D2R Metal.app'
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
checks = []
# what a Mac without Homebrew, /usr/local, MacPorts, the python.org framework, pyenv, conda, Xcode or the Command Line Tools would see
# (sandbox-exec profile)
HOME = os.path.expanduser('~')
CLEAN_MAC = ('(version 1)(allow default)\n(deny file-read* file-write* process-exec (subpath "/opt/homebrew") (subpath "/usr/local") (subpath "/opt/local") '
             '(subpath "/Library/Frameworks/Python.framework") (regex #"^/Applications/Xcode[^/]*\\.app/") (subpath "/Library/Developer") '
             f'(subpath "{HOME}/.pyenv") (subpath "{HOME}/miniconda3") (subpath "{HOME}/anaconda3") (subpath "{HOME}/miniforge3") (subpath "/opt/miniconda3") '
             '(subpath "/opt/anaconda3"))')


def check(name, ok, detail=''):
    checks.append({'check': name, 'ok': bool(ok), 'detail': detail})
    print(f"{'✓' if ok else '✗'} {name}" + (f': {detail}' if detail else ''), flush=True)
    return ok


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()


def unsigned_sha(binary):
    """The checksum of a Mach-O binary without its code signature (signing the app rewrites the one in the binary). Removing a signature leaves
    __LINKEDIT's vmsize at what the old one needed, larger for a Developer ID signature than an ad-hoc one, so that field is zeroed too"""
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'bin'); shutil.copy2(binary, c)
        subprocess.run(['codesign', '--remove-signature', c], capture_output=True)
        with open(c, 'r+b') as f:
            head = f.read(32)
            if head[:4] == b'\xcf\xfa\xed\xfe':   # a thin 64-bit Mach-O (the host is arm64 only)
                off = 32
                for _ in range(int.from_bytes(head[16:20], 'little')):
                    f.seek(off); cmd, size = int.from_bytes(f.read(4), 'little'), int.from_bytes(f.read(4), 'little')
                    if cmd == 0x19 and f.read(16).rstrip(b'\0') == b'__LINKEDIT':   # LC_SEGMENT_64: segname, vmaddr, then vmsize
                        f.seek(off + 32); f.write(bytes(8))
                    off += size
        return sha256(c)


def plist(app):
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: return plistlib.load(f)


def pids(pattern):
    """Processes whose command line holds this text (a full path: never a partial name that other programs share)"""
    r = subprocess.run(['pgrep', '-f', '-x', '.*' + pattern.replace('.', '\\.') + '.*'], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split() if int(p) != os.getpid()]


def running(pattern): return bool(pids(pattern))


def wait_for(cond, timeout, step=0.25):
    """Polls cond until it is true or timeout seconds pass; its last value"""
    end = time.monotonic() + timeout
    while True:
        v = cond()
        if v or time.monotonic() > end: return v
        time.sleep(step)


def quit_app(bundle_id, pattern, wait=15):
    """Asks an app to quit (by bundle id) and waits until no process matches pattern (a path in its command line); one that is still there (the
    quit went to another copy with the same id) gets SIGTERM, then SIGKILL: a test never leaves its game running"""
    subprocess.run(['osascript', '-e', f'tell application id "{bundle_id}" to quit'], capture_output=True)
    if wait_for(lambda: not running(pattern), wait): return
    for sig, t in ((signal.SIGTERM, 10), (signal.SIGKILL, 5)):
        for p in pids(pattern): os.kill(p, sig)
        if wait_for(lambda: not running(pattern), t): return


def snapshot(root):
    """What must not change in the player's own install: its state, its game app, the saves, what /Applications holds"""
    st = json.load(open(os.path.join(root, 'build', 'state.json')))
    app = st.get('app') or ''
    saves = os.path.expanduser('~/Library/Application Support/D2R Mobile/save')
    listing = sorted((os.path.relpath(p, saves), os.path.getsize(p), int(os.path.getmtime(p)))
                     for p in glob.glob(os.path.join(saves, '**', '*'), recursive=True) if os.path.isfile(p))
    return {'state': sha256(os.path.join(root, 'build', 'state.json')), 'app': app,
            'app_plist': sha256(os.path.join(app, 'Contents', 'Info.plist')) if os.path.isdir(app) else None,
            'app_host': sha256(os.path.join(app, 'Contents', 'MacOS', 'D2RMobile')) if os.path.isdir(app) else None,
            'saves': listing, 'applications': sorted(n for n in os.listdir('/Applications') if 'D2R' in n)}


def file_sha(path): return sha256(path) if os.path.isfile(path) else None


def opens_for_id(bundle_id):
    """The app LaunchServices opens for a bundle id (asked through AppKit; AppleScript's `path to application id` would start it)"""
    code = f'import AppKit; print(NSWorkspace.shared.urlForApplication(withBundleIdentifier: "{bundle_id}")?.path ?? "")'
    return subprocess.run(['swift', '-'], input=code, capture_output=True, text=True, timeout=180).stdout.strip().rstrip('/')


def stand_in_launcher(real, where, scratch):
    """A copy of the launcher under its own test id, held on its preview screen (LSEnvironment: DM_LAUNCHER_PREVIEW, a scratch root), so it
    opens as a real app but reads and changes nothing: the game, told that id with DM_TEST_LAUNCHER_ID, opens it for the hand-over, and the
    installed launcher never starts on the player's install during the test (LaunchServices keeps preferring the copy in /Applications for
    the real id, whatever the build number). A bare script would not do: the game waits for the launcher to finish launching"""
    app = os.path.join(where, os.path.basename(real))
    subprocess.run(['ditto', real, app], check=True)
    pl = plist(app); pl['CFBundleIdentifier'] = STAND_IN_ID; pl.pop('CFBundleURLTypes', None)   # nor does it claim d2rmetal://
    pl['LSEnvironment'] = {'DM_LAUNCHER_PREVIEW': 'empty', 'DM_LAUNCHER_ROOT': scratch}
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'wb') as f: plistlib.dump(pl, f)
    subprocess.run(['codesign', '--force', '--deep', '-s', '-', app], check=True, capture_output=True)
    subprocess.run([LSREGISTER, '-f', app], capture_output=True)
    return app


def lock_free(root):
    """The root's engine lock can be taken (no engine holds it)"""
    try:
        with open(os.path.join(root, 'build', '.engine.lock'), 'a+') as f:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB); fcntl.flock(f, fcntl.LOCK_UN); return True
    except BlockingIOError: return False
    except OSError: return True


def step_started(root, step, since):
    """The latest engine log (newer than since) shows step starting"""
    for e in engine_events(root, since):
        if e.get('event') == 'step' and e.get('step') == step and e.get('state') == 'start': return True
    return False


def rebase(value: Any, old: str, new: str) -> Any:
    if isinstance(value, str): return new + value[len(old):] if value.startswith(old) else value
    if isinstance(value, dict): return {k: rebase(v, old, new) for k, v in value.items()}
    if isinstance(value, list): return [rebase(v, old, new) for v in value]
    return value


def engine_events(root, since=0):
    logs = sorted((l for l in glob.glob(os.path.join(root, 'logs', 'install-*.log')) if os.path.getmtime(l) >= since), key=os.path.getmtime)
    if not logs: return []
    evs = []
    for line in open(logs[-1], errors='replace'):
        if line.startswith('{'):
            try: evs.append(json.loads(line))
            except ValueError: pass
    return evs


def main(argv):
    ap = argparse.ArgumentParser(description=(__doc__ or '').split('\n')[0])
    ap.add_argument('--feed', required=True, help='the release to update to: its manifest.json URL (file:// for a local test release)')
    ap.add_argument('--root', default=os.path.expanduser('~/Library/Application Support/D2R Metal'), help="the player's install root")
    ap.add_argument('--launcher', default='/Applications/D2R Metal Launcher.app', help='the launcher that runs the jobs (default: the installed one)')
    ap.add_argument('--installed', default='/Applications/D2R Metal Launcher.app', help='the installed launcher, checked untouched')
    ap.add_argument('--keep', action='store_true', help='keep the copy afterwards')
    ap.add_argument('--no-game', action='store_true', help='skip the in-game checks (handoff, scenario)')
    ap.add_argument('--clean-mac', action='store_true', help='run the launcher where Homebrew, /usr/local, Xcode and the Command Line Tools are '
                    'hidden, translate every shader again and redo every step but unpacking')
    ap.add_argument('--no-hard', action='store_true', help='skip the stop, kill, running-game and offline cases')
    a = ap.parse_args(argv)

    with urllib.request.urlopen(a.feed) as r: target = json.load(r)
    tv, tb = target['version'], target['build']
    real = os.path.abspath(a.root)
    old_state = json.load(open(os.path.join(real, 'build', 'state.json')))
    ov = (old_state.get('release') or {}).get('version')
    print(f'== updating a copy of {real} from {ov} to {tv} ({tb})', flush=True)
    before = snapshot(real)
    launcher_exe = os.path.join(a.installed, 'Contents', 'MacOS', 'D2RMetalLauncher')
    launcher_sha = file_sha(launcher_exe)
    launcher_proc = f'{a.installed}/Contents/MacOS/'
    launcher_was_running = running(launcher_proc)
    if launcher_was_running: quit_app(LAUNCHER_ID, launcher_proc)

    run = os.path.join(DM_ROOT, 'run', 'launcher_update', datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    root = os.path.join(run, 'root'); os.makedirs(run)
    try:
        # 1. the copy: APFS clones of every file, then state.json pointed at the copy
        t0 = time.time()
        subprocess.run(['cp', '-c', '-R', real, root], check=True)
        sp = os.path.join(root, 'build', 'state.json'); st = rebase(json.load(open(sp)), real, root)
        json.dump(st, open(sp, 'w'), indent=1)
        check('copy of the install', os.path.isdir(os.path.join(root, 'data')) and st.get('app', '').startswith(root), f'{time.time() - t0:.0f} s')

        # 2. the launcher's Update, on the copy
        exe = os.path.join(a.launcher, 'Contents', 'MacOS', 'D2RMetalLauncher')
        if a.clean_mac:
            check('the sandbox hides Homebrew', subprocess.run(['sandbox-exec', '-p', CLEAN_MAC, '/bin/ls', '/opt/homebrew'], capture_output=True).returncode != 0)
            shutil.rmtree(os.path.join(root, 'build', 'cache', 'metal_shader_cache'), ignore_errors=True)   # the copy's: every shader is translated again

        def launcher(job, feed=None, sandbox=None):
            env = dict(os.environ, DM_LAUNCHER_ROOT=root, DM_LAUNCHER_FEED=feed or a.feed, DM_LAUNCHER_BUNDLE_ID=TEST_ID, DM_LAUNCHER_AUTORUN=job)
            t0 = time.time()
            p = subprocess.run((['sandbox-exec', '-p', CLEAN_MAC] if (a.clean_mac if sandbox is None else sandbox) else []) + [exe], env=env,
                               capture_output=True, text=True, timeout=2400)
            line = next((l for l in p.stdout.splitlines() if l.startswith('LAUNCHER-AUTORUN')), '')
            return p.returncode == 0 and ' OK ' in line + ' ', f'{line or p.stdout[-300:]} ({time.time() - t0:.0f} s)'

        ok, detail = launcher('update')
        check("the launcher's update" + (' (clean Mac)' if a.clean_mac else ''), ok, detail)

        # 3. what the update did
        evs = engine_events(root)
        steps = {e['step']: e['state'] for e in evs if e.get('event') == 'step' and e.get('state') in ('done', 'kept', 'fail')}
        check('engine steps', steps.get('unpack') == 'kept' and steps.get('app') == 'done' and 'fail' not in steps.values(),
              ', '.join(f'{k} {v}' for k, v in steps.items()))
        if a.clean_mac:
            missing = [l.strip()[:160] for l in open(sorted(glob.glob(os.path.join(root, 'logs', 'install-*.log')), key=os.path.getmtime)[-1], errors='replace')
                       if any(w in l for w in ('Library not loaded', '/opt/homebrew', '/usr/local/', 'xcrun', 'CommandLineTools'))]
            fresh = len(glob.glob(os.path.join(root, 'build', 'cache', 'metal_shader_cache', 'b-*', '*_b.metal')))
            check('every shader translated again, nothing from outside the packs', steps.get('shaders') in ('done', 'kept') and fresh > 6000 and not missing,
                  f'{fresh} shader sources' + (f'; {missing[0]}' if missing else ''))
        st = json.load(open(sp)); new_app = os.path.join(root, 'Game.noindex', APP)
        check('state.json', (st.get('release') or {}).get('version') == tv and (st.get('release') or {}).get('build') == tb
              and (st.get('previous_release') or {}).get('version') == ov and st.get('app') == new_app
              and st.get('packs', {}).get('runtime', '').startswith(os.path.join(root, 'releases', tv)) and st.get('bundle_id') == TEST_ID,
              f"release {(st.get('release') or {}).get('version')}, previous {(st.get('previous_release') or {}).get('version')}")
        info = plist(new_app); res = os.path.join(new_app, 'Contents', 'Resources')
        check('new app: version and identity', info.get('CFBundleShortVersionString') == tv and info.get('CFBundleVersion') == str(tb)
              and info.get('CFBundleIdentifier') == TEST_ID and info.get('DMLauncherManaged') is True,
              f"{info.get('CFBundleShortVersionString')} ({info.get('CFBundleVersion')})")
        check('new app: icon', info.get('CFBundleIconName') == 'AppIcon' and os.path.isfile(os.path.join(res, 'Assets.car'))
              and os.path.isfile(os.path.join(res, 'AppIcon.icns')))
        sig = subprocess.run(['codesign', '--verify', '--deep', '--strict', new_app], capture_output=True, text=True)
        check('new app: signature', sig.returncode == 0, sig.stderr.strip()[:200])
        host = os.path.join(st['packs']['runtime'], 'host', 'D2RMobile')
        check("new app: the release's game host", unsigned_sha(os.path.join(new_app, 'Contents', 'MacOS', 'D2RMobile')) == unsigned_sha(host),
              'compared without signatures (the install signs the app it assembles)')
        paths = dict(l.strip().split('=', 1) for l in open(os.path.join(res, 'paths.txt')) if '=' in l)
        check('new app: paths into the copy', paths and all(v.startswith(root) and os.path.exists(v) for v in paths.values()), ', '.join(sorted(paths)))
        prev = os.path.join(root, 'build', 'previous.noindex', APP)
        check('previous version kept', os.path.isdir(prev) and plist(prev).get('CFBundleShortVersionString') == ov,
              plist(prev).get('CFBundleShortVersionString') if os.path.isdir(prev) else 'missing')

        # 4. the updated game
        if not a.no_game:
            log = os.path.expanduser('~/Library/Logs/D2R Mobile/host.log'); m0 = os.path.getmtime(log) if os.path.exists(log) else 0
            stand_dir = os.path.join(run, 'Apps.noindex'); os.makedirs(stand_dir)
            stand = stand_in_launcher(a.launcher, stand_dir, os.path.join(run, 'stand-in-root'))
            check('a stand-in launcher is registered for the hand-over', wait_for(lambda: opens_for_id(STAND_IN_ID) == stand, 20, 1), opens_for_id(STAND_IN_ID))
            subprocess.run(['open', new_app], check=True, env=dict(os.environ, DM_TEST_LAUNCHER_ID=STAND_IN_ID))   # open hands its environment to the app
            stand_up = wait_for(lambda: running(f'{stand}/Contents/MacOS/'), 15)
            game_gone = wait_for(lambda: not running(f'{new_app}/Contents/MacOS/'), 10)
            check('opened directly, the game hands over to the launcher', stand_up and game_gone and not running(launcher_proc)
                  and (os.path.getmtime(log) if os.path.exists(log) else 0) == m0,
                  f"stand-in {'opened' if stand_up else 'not opened'}, game {'gone' if game_gone else 'still running'}, installed launcher "
                  f"{'started (it must not)' if running(launcher_proc) else 'not started'}")
            for p in pids(f'{stand}/Contents/MacOS/'): os.kill(p, signal.SIGKILL)
            subprocess.run([LSREGISTER, '-u', stand], capture_output=True); shutil.rmtree(stand_dir, ignore_errors=True)
            subprocess.run([LSREGISTER, '-f', a.installed], capture_output=True)
            if not game_gone: quit_app(TEST_ID, f'{new_app}/Contents/MacOS/')
            out = os.path.join(run, 'scenario')
            sc = subprocess.run([sys.executable, os.path.join(DM, 'tools', 'scenario.py'), 'run', os.path.join(DM, 'verify', 'scenarios', 'enter_game.scn'),
                                 '--backend', 'mac-app', '--out', out], env=dict(os.environ, DM_MAC_APP=new_app, DM_TEST_VIDEO_FIRSTBOOT='1'),
                                capture_output=True, text=True, timeout=900)
            res_line = next((l for l in reversed(sc.stdout.splitlines()) if l.startswith('RESULT')), '')
            check('a game scenario on the updated app (enter_game)', sc.returncode == 0 and '"verdict": "OK"' in res_line, res_line[:160])

        if a.clean_mac:   # the new release's Repair, in the sandbox (same build: the previous version stays)
            ok, detail = launcher('repair')
            rep = {e['step']: e['state'] for e in engine_events(root) if e.get('event') == 'step' and e.get('state') in ('done', 'kept', 'fail')}
            check('Repair redoes every step but unpacking (clean Mac)', ok and 'fail' not in rep.values() and len(rep) == 9,
                  detail + ' · ' + ', '.join(f'{k} {v}' for k, v in rep.items()))

        # 5. the hard cases (the new release's engine: before going back)
        if not a.no_hard:
            env = dict(os.environ, DM_LAUNCHER_ROOT=root, DM_LAUNCHER_FEED=a.feed, DM_LAUNCHER_BUNDLE_ID=TEST_ID, DM_LAUNCHER_AUTORUN='repair')
            engine_pat = f'install.py repair --root {root} '
            tools_pat = root + '/'
            app_before = sha256(os.path.join(new_app, 'Contents', 'Info.plist'))

            # Stop: the engine gets SIGINT, as the Stop button and a quit send it
            t0 = time.time(); out = open(os.path.join(run, 'stop.txt'), 'w')
            lp = subprocess.Popen([exe], env=env, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            started = wait_for(lambda: step_started(root, 'shaders', t0), 300, 1)
            eng = pids(engine_pat)
            for p in eng: os.kill(p, signal.SIGINT)
            lp.wait(timeout=120); out.close()
            line = next((l for l in open(os.path.join(run, 'stop.txt'), errors='replace') if l.startswith('LAUNCHER-AUTORUN')), '')
            left = wait_for(lambda: not pids(engine_pat) and not [p for p in pids(tools_pat) if p != lp.pid], 30) and True
            check('Stop during a repair: everything stops, the lock is free', started and bool(eng) and 'Stopped' in line and left and lock_free(root),
                  f"{'shaders started' if started else 'never reached the shaders'}, {len(eng)} engine(s) stopped; {line[:120]}")

            # the launcher killed outright: its engine notices, stops its tools and exits; the next run sweeps and resumes
            t0 = time.time()
            lp = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
            started = wait_for(lambda: step_started(root, 'shaders', t0), 300, 1)
            os.kill(lp.pid, signal.SIGKILL); lp.wait()
            gone = wait_for(lambda: not pids(engine_pat) and not pids(tools_pat), 40)
            check('the launcher killed during a repair: its engine and tools stop by themselves', started and gone and lock_free(root),
                  'stopped' if gone else f'still running: {pids(engine_pat) + pids(tools_pat)}')
            py = os.path.join(st['packs']['tools'], 'python', 'bin', 'python3')
            rs = subprocess.run([py, '-I', '-B', os.path.join(st['packs']['runtime'], 'repo', 'dm', 'tools', 'install.py'), 'install', '--root', root,
                                 '--runtime', st['packs']['runtime'], '--tools', st['packs']['tools'], '--app-dir', os.path.join(root, 'Game.noindex'),
                                 '--bundle-id', TEST_ID, '--json'], capture_output=True, text=True, timeout=1800)
            evs = [json.loads(l) for l in rs.stdout.splitlines() if l.startswith('{')]
            leftovers = glob.glob(os.path.join(root, 'build', '**', '*.part'), recursive=True) + glob.glob(os.path.join(root, 'build', 'tmp', '*'))
            check('the next run resumes and leaves nothing behind', rs.returncode == 0 and any(e.get('event') == 'done' for e in evs) and not leftovers,
                  ', '.join(f"{e['step']} {e['state']}" for e in evs if e.get('event') == 'step' and e.get('state') in ('done', 'kept', 'fail'))
                  + (f'; left: {leftovers[:2]}' if leftovers else ''))

            # the game running: a repair waits for it to quit, and changes nothing meanwhile
            saves = os.path.join(run, 'saves'); os.makedirs(saves, exist_ok=True)
            subprocess.run(['open', '-n', '--env', 'DM_FROM_LAUNCHER=1', '--env', f'DM_APP_SUPPORT={saves}', new_app], check=True)
            up = wait_for(lambda: running(f'{new_app}/Contents/MacOS/'), 20)
            ok, detail = launcher('repair', sandbox=False)
            check('a repair while the game runs is refused until it quits', up and not ok and 'the game is running' in detail
                  and sha256(os.path.join(new_app, 'Contents', 'Info.plist')) == app_before, detail[:160])
            quit_app(TEST_ID, f'{new_app}/Contents/MacOS/', 30)

            # an unreachable feed: said plainly, nothing changes
            state_before = sha256(sp)
            ok, detail = launcher('update', feed='https://127.0.0.1:9/manifest.json', sandbox=False)
            check('an unreachable feed is said plainly, nothing changes', not ok and "can't be reached" in detail and sha256(sp) == state_before, detail[:160])
        # 6. last: back to the previous version, as the gear menu does (the installed release's engine; after it, the copy runs the older release)
        py = os.path.join(st['packs']['tools'], 'python', 'bin', 'python3')
        rv = subprocess.run([py, '-E', '-s', os.path.join(st['packs']['runtime'], 'repo', 'dm', 'tools', 'install.py'), 'revert', '--root', root,
                             '--app-dir', os.path.join(root, 'Game.noindex'), '--json'], capture_output=True, text=True, timeout=300)
        st2 = json.load(open(sp))
        check('back to the previous version', rv.returncode == 0 and plist(new_app).get('CFBundleShortVersionString') == ov
              and (st2.get('release') or {}).get('version') == ov and plist(prev).get('CFBundleShortVersionString') == tv,
              f"app {plist(new_app).get('CFBundleShortVersionString')}, kept {plist(prev).get('CFBundleShortVersionString') if os.path.isdir(prev) else '-'}")
    except Exception as e:   # noqa: BLE001 -- any failure is a red check, and the clean-up below still runs
        check('ran to the end', False, f'{type(e).__name__}: {e}')
    finally:
        # 7. clean up; the player's own install must not have changed
        for app in glob.glob(os.path.join(root, '**', '*.app'), recursive=True):
            subprocess.run([LSREGISTER, '-u', app], capture_output=True)
        if os.path.realpath(a.launcher) != os.path.realpath(a.installed):   # a launcher run from elsewhere registered itself: the installed one again
            subprocess.run([LSREGISTER, '-u', a.launcher], capture_output=True)
            if os.path.isdir(a.installed): subprocess.run([LSREGISTER, '-f', a.installed], capture_output=True)
        cache = subprocess.run(['getconf', 'DARWIN_USER_CACHE_DIR'], capture_output=True, text=True).stdout.strip()
        if cache and os.path.isdir(os.path.join(cache, TEST_ID)): shutil.rmtree(os.path.join(cache, TEST_ID), ignore_errors=True)
        subprocess.run(['defaults', 'delete', TEST_ID], capture_output=True)
        if not a.keep: shutil.rmtree(run, ignore_errors=True)
        after = snapshot(real)
        check("the player's install untouched", after == before,
              ', '.join(k for k in before if before[k] != after[k]) or 'state, app, saves, /Applications')
        check('the installed launcher untouched, and still the one macOS opens', file_sha(launcher_exe) == launcher_sha
              and (launcher_sha is None or opens_for_id(LAUNCHER_ID) == a.installed.rstrip('/')), opens_for_id(LAUNCHER_ID))
        if launcher_was_running: subprocess.run(['open', a.installed])

    ok = all(c['ok'] for c in checks)
    print(f"LAUNCHER-UPDATE {'OK' if ok else 'RED'} {ov} → {tv}" + (f' (copy kept in {run})' if a.keep else ''))
    print('RESULT ' + json.dumps({'name': 'launcher-update', 'verdict': 'OK' if ok else 'RED', 'from': ov, 'to': tv, 'checks': checks}))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
