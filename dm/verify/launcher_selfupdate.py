#!/usr/bin/env python3
"""Checks the launcher updating itself from a release, before the release goes out (docs/LAUNCHER.md "Testing a release").

  1. Makes an "old" launcher: the release's own launcher (from its zip) with its build number set to 1 and an ad-hoc signature, in a throwaway
     folder ($DM_ROOT/run/launcher_selfupdate/<time>/Apps.noindex, out of app search), with an empty test install root beside it.
  2. Tampered releases first, each refused with the old launcher left as it is: a wrong checksum for the launcher; a manifest whose signature
     does not verify; and for a signed release, a manifest re-signed with the release key that names no team and carries an ad-hoc launcher
     (the old launcher is then signed with the release's Developer ID too, so it pins its own team).
  3. The real release: run with DM_LAUNCHER_AUTORUN=selfupdate, the old launcher must download and check the new one, swap it into its own
     place, keep itself as the previous launcher, and restart into the new one (the test switches carry over, so the new one opens on the test
     root). Checks: the app in place is the release's build, its signature is valid, its program is the release's, the old one is kept.
  4. Cleans up and checks that the installed launcher is untouched and is still the one macOS opens for the launcher's bundle id.

Usage: launcher_selfupdate.py --feed file:///…/manifest.json [--launcher /Applications/D2R Metal Launcher.app] [--keep]
Last lines: LAUNCHER-SELFUPDATE OK|RED, then RESULT {json}.
"""
import argparse, base64, datetime, hashlib, json, os, plistlib, shutil, signal, subprocess, sys, tempfile, time, urllib.parse, urllib.request

DM_ROOT = os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work'))
LAUNCHER_ID = 'org.d2rmetal.launcher'
NAME = 'D2R Metal Launcher.app'
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
SIGNER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools', 'manifest_sign.swift')
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
    """Processes whose command line holds this text (a full path: never a partial name that other programs share)"""
    r = subprocess.run(['pgrep', '-f', '-x', '.*' + pattern.replace('.', '\\.') + '.*'], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split() if int(p) != os.getpid()]


def wait_for(cond, timeout, step=0.25):
    end = time.monotonic() + timeout
    while True:
        v = cond()
        if v or time.monotonic() > end: return v
        time.sleep(step)


def developer_id(team):
    out = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'], capture_output=True, text=True).stdout
    return next((l.split('"')[1] for l in out.splitlines() if 'Developer ID Application' in l and f'({team})' in l), None)


def stop(pattern, wait=10):
    for p in pids(pattern): os.kill(p, signal.SIGTERM)
    wait_for(lambda: not pids(pattern), wait)


def opens_for_id():
    """The app LaunchServices opens for the launcher's bundle id (asked through AppKit; AppleScript's `path to application id` would start it)"""
    code = f'import AppKit; print(NSWorkspace.shared.urlForApplication(withBundleIdentifier: "{LAUNCHER_ID}")?.path ?? "")'
    r = subprocess.run(['swift', '-'], input=code, capture_output=True, text=True, timeout=180)
    return r.stdout.strip().rstrip('/')


def run_old(old, root, feed):
    """Runs the old launcher until it ends; its output goes to a file, not a pipe, which the processes it starts could hold open"""
    exe = os.path.join(old, 'Contents', 'MacOS', 'D2RMetalLauncher')
    env = dict(os.environ, DM_LAUNCHER_ROOT=root, DM_LAUNCHER_FEED=feed, DM_LAUNCHER_AUTORUN='selfupdate')
    out = os.path.join(root, 'autorun.txt')
    with open(out, 'w') as f:
        p = subprocess.Popen([exe], env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
        try: code = p.wait(timeout=600)
        except subprocess.TimeoutExpired: p.kill(); code = -1
    text = open(out, errors='replace').read()
    return code, next((l for l in text.splitlines() if l.startswith('LAUNCHER-AUTORUN')), text[-300:])


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
        team = m.get('team_id'); ident = developer_id(team) if team else None
        # a signed release: the old launcher carries the release's Developer ID too, so it pins its own team as a player's would
        sign = ['codesign', '--force', '--deep', '--options', 'runtime', '--timestamp', '-s', ident] if ident else ['codesign', '--force', '--deep', '-s', '-']
        subprocess.run(sign + [old], check=True, capture_output=True)
        check('an old launcher (build 1)', info(old)['CFBundleVersion'] == '1', f"signed {'with ' + ident if ident else 'ad-hoc'}")

        # 2. tampered releases: refused, nothing changed
        def tampered(name, change, resign):
            d = os.path.join(run, name); os.makedirs(d)
            mb = json.loads(json.dumps(m)); mb['launcher']['url'] = zip_url; change(mb, d)
            mp = os.path.join(d, 'manifest.json'); json.dump(mb, open(mp, 'w'))
            if resign: subprocess.run(['swift', SIGNER, 'sign', mp], check=True, capture_output=True)
            else: open(mp + '.sig', 'w').write(base64.b64encode(b'\0' * 64).decode() + '\n')   # a signature that verifies nothing
            code, line = run_old(old, root, 'file://' + mp)
            stop(new_proc)
            return code, line
        has_key = subprocess.run(['swift', SIGNER, 'public'], capture_output=True).returncode == 0
        code, line = tampered('bad-checksum', lambda mb, d: mb['launcher'].update(sha256='0' * 64), resign=has_key)
        check('a launcher with a wrong checksum is refused', code != 0 and ' RED ' in line and 'damaged' in line and info(old)['CFBundleVersion'] == '1', line[:200])
        code, line = tampered('bad-signature', lambda mb, d: None, resign=False)
        check('a manifest whose signature does not verify is refused', code != 0 and ' RED ' in line and 'not signed by the project' in line
              and info(old)['CFBundleVersion'] == '1', line[:200])
        if ident and has_key:
            def adhoc_launcher(mb, d):   # the release's launcher re-signed ad-hoc, its checksum and size updated, the team left out
                x = os.path.join(d, 'x'); subprocess.run(['ditto', ref_app, os.path.join(x, NAME)], check=True)
                subprocess.run(['codesign', '--force', '--deep', '-s', '-', os.path.join(x, NAME)], check=True, capture_output=True)
                z = os.path.join(d, la['name']); subprocess.run(['ditto', '-c', '-k', '--keepParent', os.path.join(x, NAME), z], check=True)
                mb.pop('team_id', None); mb['launcher'].update(url='file://' + z, sha256=sha256(z), size=os.path.getsize(z))
            code, line = tampered('no-team', adhoc_launcher, resign=True)
            check('a signed manifest naming no team, with an ad-hoc launcher, is refused (the team is the launcher\'s own)', code != 0 and ' RED ' in line
                  and 'not signed by the project' in line and info(old)['CFBundleVersion'] == '1', line[:200])
        else:
            check('the team check (a signed release only)', True, 'skipped: ' + ('an unsigned release' if not team else 'no Developer ID identity or release key here'))

        # 3. the real release
        code, line = run_old(old, root, a.feed)
        check('the old launcher installs the new one', code == 0 and ' OK ' in line, line[:200])
        up = []
        for _ in range(20):
            up = pids(new_proc)
            if up: break
            time.sleep(0.5)
        check('it restarts into the new launcher', bool(up), f'pid {up[0]}' if up else 'no new process')
        still = not wait_for(lambda: not pids(new_proc), 3)   # still there after 3 s
        check('the new launcher keeps running', still)
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
        opener = opens_for_id()
        check('macOS still opens the installed launcher', real_sha is None or opener == a.launcher.rstrip('/'), opener)
        if real_was_running: subprocess.run(['open', a.launcher])

    ok = all(c['ok'] for c in checks)
    print(f"LAUNCHER-SELFUPDATE {'OK' if ok else 'RED'} 1 → {la['build']}" + (f' (kept in {run})' if a.keep else ''))
    print('RESULT ' + json.dumps({'name': 'launcher-selfupdate', 'verdict': 'OK' if ok else 'RED', 'to': la['build'], 'checks': checks}))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
