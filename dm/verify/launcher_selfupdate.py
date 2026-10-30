#!/usr/bin/env python3
"""Checks the launcher updating itself from a release, before the release goes out (docs/LAUNCHER.md "Testing a release").

  1. Makes an "old" launcher: the release's own launcher (from its zip) with its build number set to 1 and an ad-hoc signature, in a throwaway
     folder ($DM_ROOT/run/launcher_selfupdate/<time>/Apps.noindex, out of app search), with an empty test install root beside it.
  2. A tampered release first: the same manifest with a wrong checksum for the launcher. The old launcher must refuse it and stay as it is.
  3. The real release: run with DM_LAUNCHER_AUTORUN=selfupdate, the old launcher must download and check the new one, swap it into its own
     place, keep itself as the previous launcher, and restart into the new one (the test switches carry over, so the new one opens on the test
     root). Checks: the app in place is the release's build, its signature is valid, its program is the release's, the old one is kept.
  4. Cleans up and checks that the installed launcher is untouched and is still the one macOS opens for the launcher's bundle id.

Usage: launcher_selfupdate.py --feed file:///…/manifest.json [--launcher /Applications/D2R Metal Launcher.app] [--keep]
Last lines: LAUNCHER-SELFUPDATE OK|RED, then RESULT {json}.
"""
import argparse, datetime, hashlib, json, os, plistlib, shutil, signal, subprocess, sys, tempfile, time, urllib.parse, urllib.request

DM_ROOT = os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work'))
LAUNCHER_ID = 'org.d2rmetal.launcher'
NAME = 'D2R Metal Launcher.app'
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
checks = []


def check(name, ok, detail=''):
    checks.append({'check': name, 'ok': bool(ok), 'detail': detail})
    print(f"{'✓' if ok else '✗'} {name}" + (f': {detail}' if detail else ''), flush=True)
    return ok


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()


def info(app):
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: return plistlib.load(f)


def pids(pattern):
    r = subprocess.run(['pgrep', '-f', pattern], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def stop(pattern, wait=10):
    for p in pids(pattern): os.kill(p, signal.SIGTERM)
    for _ in range(wait):
        if not pids(pattern): return
        time.sleep(1)


def opens_for_id():
    r = subprocess.run(['osascript', '-e', f'POSIX path of (path to application id "{LAUNCHER_ID}")'], capture_output=True, text=True)
    return r.stdout.strip().rstrip('/')


def run_old(old, root, feed):
    exe = os.path.join(old, 'Contents', 'MacOS', 'D2RMetalLauncher')
    env = dict(os.environ, DM_LAUNCHER_ROOT=root, DM_LAUNCHER_FEED=feed, DM_LAUNCHER_AUTORUN='selfupdate')
    p = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=600)
    return p.returncode, next((l for l in p.stdout.splitlines() if l.startswith('LAUNCHER-AUTORUN')), p.stdout[-300:])


def main(argv):
    ap = argparse.ArgumentParser(description=(__doc__ or '').split('\n')[0])
    ap.add_argument('--feed', required=True, help='the release: its manifest.json URL (file:// for a local test release)')
    ap.add_argument('--launcher', default='/Applications/' + NAME, help='the installed launcher, checked untouched')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args(argv)

    with urllib.request.urlopen(a.feed) as r: m = json.load(r)
    la = m['launcher']; zip_url = la.get('url') or urllib.parse.urljoin(a.feed, la['name'])
    print(f"== the launcher updating itself to {la['version']} ({la['build']})", flush=True)
    real_exe = os.path.join(a.launcher, 'Contents', 'MacOS', 'D2RMetalLauncher')
    real_sha = sha256(real_exe) if os.path.exists(real_exe) else None
    real_proc = f'{a.launcher}/Contents/MacOS/'
    real_was_running = bool(pids(real_proc))
    if real_was_running: subprocess.run(['osascript', '-e', f'tell application id "{LAUNCHER_ID}" to quit'], capture_output=True); time.sleep(2)

    run = os.path.join(DM_ROOT, 'run', 'launcher_selfupdate', datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    apps, root = os.path.join(run, 'Apps.noindex'), os.path.join(run, 'root')
    old = os.path.join(apps, NAME); os.makedirs(apps); os.makedirs(root)
    new_proc = f'{old}/Contents/MacOS/'
    try:
        # 1. the release's launcher, unpacked (the reference), and an "old" copy of it
        ref = os.path.join(run, 'ref')
        with tempfile.TemporaryDirectory() as d:
            z = os.path.join(d, la['name'])
            with urllib.request.urlopen(zip_url) as r, open(z, 'wb') as f: shutil.copyfileobj(r, f)
            check("the release's launcher zip", sha256(z) == la['sha256'], la['name'])
            subprocess.run(['ditto', '-x', '-k', z, ref], check=True)
        ref_app = os.path.join(ref, NAME)
        subprocess.run(['ditto', ref_app, old], check=True)
        pl = info(old); pl['CFBundleVersion'] = '1'; pl['CFBundleShortVersionString'] = '0.0.1'
        with open(os.path.join(old, 'Contents', 'Info.plist'), 'wb') as f: plistlib.dump(pl, f)
        subprocess.run(['codesign', '--force', '--deep', '-s', '-', old], check=True, capture_output=True)
        check('an old launcher (build 1)', info(old)['CFBundleVersion'] == '1')

        # 2. a tampered release: refused, nothing changed
        bad = os.path.join(run, 'bad'); os.makedirs(bad)
        mb = json.loads(json.dumps(m)); mb['launcher']['sha256'] = '0' * 64; mb['launcher']['url'] = zip_url
        json.dump(mb, open(os.path.join(bad, 'manifest.json'), 'w'))
        code, line = run_old(old, root, 'file://' + os.path.join(bad, 'manifest.json'))
        check('a launcher with a wrong checksum is refused', code != 0 and ' RED ' in line and 'damaged' in line and info(old)['CFBundleVersion'] == '1', line[:200])
        stop(new_proc)

        # 3. the real release
        code, line = run_old(old, root, a.feed)
        check('the old launcher installs the new one', code == 0 and ' OK ' in line, line[:200])
        up = []
        for _ in range(20):
            up = pids(new_proc)
            if up: break
            time.sleep(0.5)
        check('it restarts into the new launcher', bool(up), f'pid {up[0]}' if up else 'no new process')
        time.sleep(3); still = pids(new_proc)
        check('the new launcher keeps running', bool(still))
        stop(new_proc)
        check('the app in place is the release\'s build', info(old)['CFBundleVersion'] == str(la['build']) and info(old)['CFBundleShortVersionString'] == la['version'],
              f"{info(old)['CFBundleShortVersionString']} ({info(old)['CFBundleVersion']})")
        sig = subprocess.run(['codesign', '--verify', '--deep', '--strict', old], capture_output=True, text=True)
        check('its signature', sig.returncode == 0, sig.stderr.strip()[:200])
        check("its program is the release's", sha256(os.path.join(old, 'Contents', 'MacOS', 'D2RMetalLauncher')) == sha256(os.path.join(ref_app, 'Contents', 'MacOS', 'D2RMetalLauncher')))
        prev = os.path.join(root, 'build', 'launcher-previous.noindex', NAME)
        check('the old launcher is kept', os.path.isdir(prev) and info(prev)['CFBundleVersion'] == '1')
        check('nothing left half-way', not os.path.exists(os.path.join(root, 'downloads', 'launcher.noindex'))
              and not os.path.exists(os.path.join(apps, '.D2R Metal Launcher.update.app')))
    except Exception as e:   # noqa: BLE001 -- any failure is a red check, and the clean-up below still runs
        check('ran to the end', False, f'{type(e).__name__}: {e}')
    finally:
        # 4. clean up; the installed launcher untouched and still the one macOS opens
        stop(new_proc)
        for app in (old, os.path.join(root, 'build', 'launcher-previous.noindex', NAME), os.path.join(run, 'ref', NAME)):
            if os.path.isdir(app): subprocess.run([LSREGISTER, '-u', app], capture_output=True)
        if not a.keep: shutil.rmtree(run, ignore_errors=True)
        if os.path.isdir(a.launcher): subprocess.run([LSREGISTER, '-f', a.launcher], capture_output=True)
        check('the installed launcher untouched', real_sha is None or sha256(real_exe) == real_sha)
        check('macOS still opens the installed launcher', real_sha is None or opens_for_id() == a.launcher.rstrip('/'), opens_for_id())
        if real_was_running: subprocess.run(['open', a.launcher])

    ok = all(c['ok'] for c in checks)
    print(f"LAUNCHER-SELFUPDATE {'OK' if ok else 'RED'} 1 → {la['build']}" + (f' (kept in {run})' if a.keep else ''))
    print('RESULT ' + json.dumps({'name': 'launcher-selfupdate', 'verdict': 'OK' if ok else 'RED', 'to': la['build'], 'checks': checks}))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
