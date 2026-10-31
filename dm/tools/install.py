#!/usr/bin/env python3
# Install engine of the D2R Metal Launcher (docs/LAUNCHER.md): builds the Mac app on the player's own Mac from their own game files, with the prebuilt tools
# of a GitHub release, and on an update redoes only what the release changed. Standard library only: it runs with the tools pack's Python (python3 -I -B).
#
# The install root is a workspace like ~/dm-work (it is the DM_ROOT of every step): the game files under data/switch/3.3.94012/Update and the build outputs
# under build/, so the repository's own scripts (extract_game.sh, product.sh, dmx.py, dmsh_build.py, c1_guard.py, gen_romfs_overlay.py) run on it unchanged.
# Nothing here needs Xcode, Homebrew, .NET or a compiler: the shader chain is of the source form (dmsh_chain.py --update-lock --runtime) and the app
# compiles its libraries with macOS's own Metal compiler, warmed once by D2RMobile --warm-shaders.
#
#   install.py check   --root R --runtime P --tools T                     what this Mac has and lacks; changes nothing
#   install.py install --root R --runtime P --tools T [--game <pkg>… --keys <prod.keys> | --from <folder>] [--app-dir D] [--bundle-id ID] [--json]
#                      the whole pipeline, each step skipped when its stamp still matches: the first install, an update (the new release's packs) and a
#                      resume after an interruption are the same command; game files are needed only when the root has none yet (or damaged ones)
#   install.py repair  …the same arguments…   every step again; the previous version is kept unless the app's build changes
#   install.py revert  --root R [--app-dir D]                                swaps the app back to the one before the last install or update
#   (--app-dir: where D2R Metal.app goes; default R/Game.noindex, out of /Applications and app search, since the launcher starts the game)
#   install.py status  --root R [--json]                                     the state, and whether the game files and both apps are there (one JSON object)
# P = an unpacked runtime pack (manifest.json, host/D2RMobile, repo/dm/…, lib/libnvn_deswizzle.dylib), T = an unpacked tools pack (python/bin/python3,
# dmsh_xlate/dmsh_xlate, bin/spirv-cross, bin/hactool). --json prints one JSON event per line on stdout (what the launcher reads); the steps' own output
# goes to <R>/logs/install-<time>.log either way. Game packages are only read, in place; the keys too, through a temporary copy that unpacking deletes.
#
# One engine at a time per root (a lock the kernel drops with the process). Every tool runs in its own process group: a stop (SIGINT, SIGTERM, SIGHUP, or the
# launcher going away) stops the tool and everything it started before the engine exits, and the next run resumes. Swapping apps is never cut half-way.
import argparse, contextlib, datetime, errno, fcntl, glob, hashlib, importlib.util, json, os, platform, plistlib, re, shutil, signal, subprocess, sys
import threading, time, traceback

VERSION = '3.3.94012'
EXEFS_SHA = {'main': '55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e',   # the three program files of 1.0.34.0 (= setup.sh, extract_game.sh)
             'sdk': '1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017',
             'subsdk0': '67a38fcf89cd13e040a021eb3d157072f38ebf6680a907874c35c7b231b5d4e2'}
# The resource archives the game reads and their byte counts in 1.0.34.0: the same table as dm/host/romfs_validate.c (test_install.py checks they agree).
# A short or missing archive is a damaged install, found by size alone (a stat each), never by reading the 8 GB
ROMFS_SIZES = {'00-00000001.data': 1073741824, '00-00000002.data': 1073741821, '00-00000003.data': 1026342790, '00-00000004.data': 3664822,
               '00-00000005.data': 391990840, '00-00000006.data': 3794407, '01-00000001.data': 344377972, '01-00000002.data': 37189352,
               '02-00000001.data': 368762692, '02-00000002.data': 23206186, '03-00000001.data': 251620752, '03-00000002.data': 55071917,
               '04-00000001.data': 118831405, '04-00000002.data': 8915855, '05-00000001.data': 478931036, '05-00000002.data': 92048235,
               '17-00000001.data': 204495617, '17-00000002.data': 4496514}
APP_NAME = 'D2R Metal.app'
BUNDLE_ID = 'org.d2rmobile.mac'   # the data-bearing id: saves, settings and the Metal cache of earlier builds carry over
PROTOCOL = 2   # what the launcher may expect of this engine: 2 = lock, error codes, status fields, previous_packs / reverted_from in the state
LAUNCHER_MIN_BUILD = 0   # the oldest launcher build that drives this engine correctly: release_mac.sh's default --min-build
STEPS = ['unpack', 'product', 'packs', 'shaders', 'c1', 'overlay', 'tables', 'app', 'warm']
TYPICAL_S = {'unpack': 300, 'product': 15, 'packs': 20, 'shaders': 120, 'c1': 10, 'overlay': 20, 'tables': 5, 'app': 5, 'warm': 240}   # M6, for the ETA
QUIET_MIN = {'unpack': 30}   # minutes a tool may print nothing before it counts as hung (hactool is silent while it writes 9 GB); 10 for the others
# free space (GB) a run needs where the root is: unpacking packages takes ~20 GB of intermediate files on top of the ~9 GB of game files; an update or a
# repair writes new libraries, an overlay and an app (well under 3 GB); a folder given with --from is cloned (no space) on the same volume, else copied
NEED_GB_PACKAGES, NEED_GB = 30, 3
PY = ['-B', '-E', '-s']   # the steps' Python: no bytecode written into the packs (-E ignores PYTHONDONTWRITEBYTECODE), no PYTHON* variables, no user
                          # site; not -I, which would drop the script's own folder that these scripts import from
KEEP_LOGS = 20


class Fail(Exception):
    def __init__(self, msg, code=None):
        super().__init__(msg); self.code = code


class Interrupted(BaseException):
    """A stop request: SIGINT / SIGTERM / SIGHUP, or the launcher that started the engine is gone (not an Exception: nothing may swallow it)"""


CJK = re.compile(r'[　-〿㐀-鿿＀-￯]')


def english(line):
    """The English of a bilingual "Chinese | English" output line: each " | " part cut at its first Chinese character, empty parts dropped"""
    if not CJK.search(line): return line
    parts = (CJK.split(p, 1)[0].strip(' ✗🔴:;,') for p in line.split(' | '))
    return '; '.join(p for p in parts if p)


def failure(tool, code, tail):
    """What a failed tool says went wrong: its last error line ("✗" from the shell scripts, "🔴" or 【红】 from the Python ones), else its last lines"""
    errs = [l for l in tail if l.lstrip().startswith(('✗', '🔴')) or '【红】' in l]
    said = (english(errs[-1]) or errs[-1].strip()) if errs else ' | '.join(english(l) or l for l in tail[-3:] if l)
    return f'{said} ({tool} exited with {code})'


def group_alive(pgid):
    try: os.killpg(pgid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True


def stop_group(p, grace=10):
    """Stops a tool and everything it started (its own process group: start_new_session): SIGTERM, then SIGKILL after `grace` seconds"""
    for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, 5)):
        try: os.killpg(p.pid, sig)
        except ProcessLookupError: return
        end = time.monotonic() + wait
        while time.monotonic() < end:
            if p.poll() is not None and not group_alive(p.pid): return
            time.sleep(0.1)


def pid_alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True


class Ctx:
    def __init__(self, a):
        self.root = os.path.abspath(os.path.expanduser(a.root))
        self.runtime = os.path.abspath(os.path.expanduser(a.runtime)) if getattr(a, 'runtime', None) else ''   # '' = not given (check, revert, status)
        self.tools = os.path.abspath(os.path.expanduser(a.tools)) if getattr(a, 'tools', None) else ''
        # the game app lives in the root's Game.noindex folder by default: started through the launcher (as a Battle.net game), out of /Applications;
        # the .noindex suffix keeps it out of Spotlight and app search, which do index a plain folder under Application Support
        self.app_dir = os.path.abspath(os.path.expanduser(getattr(a, 'app_dir', None) or os.path.join(self.root, 'Game.noindex')))
        self.bundle_id = getattr(a, 'bundle_id', None) or BUNDLE_ID
        self.json = getattr(a, 'json', False)
        self.game = os.path.join(self.root, 'data', 'switch', VERSION, 'Update')
        self.build = os.path.join(self.root, 'build')
        self.state_path = os.path.join(self.build, 'state.json')
        self.log_path, self.log = None, None
        self.state = load_json(self.state_path) or {'steps': {}}
        self.state.setdefault('steps', {})
        self.proc, self.last_out, self.quiet_s, self.stalled = None, time.monotonic(), None, False
        self.held, self.pending, self.lock = 0, None, None

    def open_log(self):
        os.makedirs(os.path.join(self.root, 'logs'), exist_ok=True)
        self.log_path = os.path.join(self.root, 'logs', 'install-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.log')
        self.log = open(self.log_path, 'a', encoding='utf-8')

    def write(self, text):
        if self.log: self.log.write(text); self.log.flush()

    # -- output: JSON events for the launcher, plain lines for a person
    def event(self, **ev):
        ev.setdefault('t', round(time.time(), 3))
        self.write(json.dumps(ev, ensure_ascii=False) + '\n')
        try:
            if self.json: print(json.dumps(ev, ensure_ascii=False), flush=True)
            elif ev.get('event') in ('step', 'error', 'done', 'note'):
                print(f"[{datetime.datetime.now():%H:%M:%S}] {ev.get('step', '')} {ev.get('state', ev['event'])} {ev.get('msg', '')}".rstrip(), flush=True)
        except BrokenPipeError:   # the launcher is gone: the parent watch stops the run; the log keeps every event
            pass

    def save_state(self):
        os.makedirs(self.build, exist_ok=True)
        tmp = self.state_path + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f: json.dump(self.state, f, indent=1, ensure_ascii=False)
        os.replace(tmp, self.state_path)

    # -- one engine per root; stop requests
    def take_lock(self):
        """The root's engine lock (build/.engine.lock): flock is released by the kernel when the process ends, however it ends. False when another
        engine holds it"""
        os.makedirs(self.build, exist_ok=True)
        self.lock = open(os.path.join(self.build, '.engine.lock'), 'a+')
        try: fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: return False
        self.lock.seek(0); self.lock.truncate(); self.lock.write(f'{os.getpid()}\n'); self.lock.flush()
        return True

    def on_signal(self, sig, _frame):
        if self.held: self.pending = sig; return
        raise Interrupted(signal.Signals(sig).name)

    @contextlib.contextmanager
    def critical(self):
        """Moves that must not stop half-way (swapping apps, stopping a tool): a stop request waits until they are done"""
        self.held += 1
        try: yield
        finally:
            self.held -= 1
            if not self.held and self.pending:
                sig, self.pending = self.pending, None
                raise Interrupted(signal.Signals(sig).name)

    def watch(self, parent):
        """A thread: stops the engine when the launcher that started it is gone (the engine re-parents to launchd), and stops a tool that has printed
        nothing for longer than its step allows (a hang fails the step instead of waiting for ever)"""
        while True:
            time.sleep(1)
            if parent and os.getppid() != parent:
                os.kill(os.getpid(), signal.SIGTERM); return
            p = self.proc
            if p and self.quiet_s and not self.stalled and time.monotonic() - self.last_out > self.quiet_s:
                self.stalled = True; stop_group(p)

    # -- the packs
    @property
    def repo(self): return os.path.join(self.runtime, 'repo')
    def dm(self, *p): return os.path.join(self.repo, 'dm', *p)
    @property
    def python(self): return os.path.join(self.tools, 'python', 'bin', 'python3')
    @property
    def manifest(self): return (load_json(os.path.join(self.runtime, 'manifest.json')) if self.runtime else None) or {}

    def env(self, **extra):
        """The environment of every step: this root as DM_ROOT and only the packs' tools on PATH (no Homebrew, no Xcode)"""
        e = {k: v for k, v in os.environ.items() if k in ('HOME', 'USER', 'LOGNAME', 'TMPDIR', 'LANG', 'LC_ALL')}
        e.update(DM_ROOT=self.root, PATH=os.pathsep.join([os.path.join(self.tools, 'bin'), os.path.join(self.tools, 'python', 'bin'), '/usr/bin', '/bin', '/usr/sbin', '/sbin']),
                 XLATE=os.path.join(self.tools, 'dmsh_xlate', 'dmsh_xlate'), HACTOOL=os.path.join(self.tools, 'bin', 'hactool'), DM_PY=self.python,
                 PYTHONDONTWRITEBYTECODE='1', DEVELOPER_DIR='/nonexistent')   # an accidental xcrun fails loudly instead of using an Xcode that players don't have
        e.update(extra)
        return e

    def run(self, step, cmd, env=None, cwd=None, progress=None):
        """Runs one tool of a step in its own process group; its output goes to the log. progress(line) → (done, total) or None: lines that report
        progress become events. A stop request or a hang stops the whole group before this returns"""
        self.write(f'$ {" ".join(cmd)}\n')
        tool = next((os.path.basename(x) for x in cmd if x.endswith(('.py', '.sh'))), os.path.basename(cmd[0]))
        quiet = QUIET_MIN.get(step, 10)
        p = subprocess.Popen(cmd, env=env or self.env(), cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, errors='replace', start_new_session=True)
        self.proc, self.last_out, self.quiet_s, self.stalled = p, time.monotonic(), quiet * 60, False
        tail = []
        assert p.stdout is not None
        try:
            for line in p.stdout:
                self.last_out = time.monotonic()
                self.write(line); tail = (tail + [line.rstrip()])[-12:]
                got = progress(line) if progress else None
                if got: self.event(event='progress', step=step, done=got[0], total=got[1])
            p.wait()
        finally:
            self.proc = None
            with self.critical():
                if p.poll() is None or group_alive(p.pid): stop_group(p)   # stopped while the tool runs, or it left processes behind
        if self.stalled: raise Fail(f'{tool} stopped responding: nothing for {quiet} min (the log has its last lines); run it again', code='stalled')
        if p.returncode: raise Fail(failure(tool, p.returncode, tail))
        return tail

    def capture(self, cmd, env=None, timeout=600):
        """Runs a short tool whose output the engine reads; stdout and stderr both go to the log too"""
        self.write(f'$ {" ".join(cmd)}\n')
        tool = next((os.path.basename(x) for x in cmd if x.endswith(('.py', '.sh'))), os.path.basename(cmd[0]))
        p = subprocess.Popen(cmd, env=env or self.env(), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             errors='replace', start_new_session=True)
        try: out, err = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            with self.critical(): stop_group(p)
            raise Fail(f'{tool} stopped responding ({timeout // 60} min)', code='stalled')
        except BaseException:
            with self.critical(): stop_group(p)
            raise
        self.write(out[-20000:] + ('' if out.endswith('\n') or not out else '\n') + ''.join(f'stderr: {l}\n' for l in err.splitlines()[-40:]))
        return subprocess.CompletedProcess(cmd, p.returncode, out, err)


def read(path, binary=False):
    with open(path, 'rb' if binary else 'r', **({} if binary else {'encoding': 'utf-8', 'errors': 'replace'})) as f: return f.read()


def load_json(path):
    try:
        with open(path, encoding='utf-8') as f: return json.load(f)
    except (OSError, ValueError): return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def tree_sha(paths):
    """One digest over the contents of files and folders (sorted, by relative name): the stamp of a step whose inputs are those sources"""
    h = hashlib.sha256()
    for top in paths:
        files = [top] if os.path.isfile(top) else sorted(os.path.join(d, f) for d, _, fs in os.walk(top) for f in fs if not f.endswith('.pyc') and '__pycache__' not in d)
        for f in files:
            h.update(os.path.relpath(f, os.path.dirname(top)).encode() + b'\0'); h.update(read(f, binary=True)); h.update(b'\0')
    return h.hexdigest()[:16]


def os_build():
    return subprocess.run(['sysctl', '-n', 'kern.osversion'], capture_output=True, text=True).stdout.strip()


def existing(path):
    while not os.path.exists(path): path = os.path.dirname(path)
    return path


def free_gb(path):
    st = os.statvfs(existing(path)); return st.f_bavail * st.f_frsize / 2**30


def write_atomic(final, build):
    """Builds a folder or a file under final + '.part', then moves it into place: an interrupted build never looks finished, and the copy an
    installed app is using is replaced in one rename (the app keeps reading the files it has open), never rewritten in place"""
    part, old = final + '.part', final + '.old'
    for p in (part, old):
        if os.path.isdir(p) and not os.path.islink(p): shutil.rmtree(p)
        elif os.path.lexists(p): os.unlink(p)
    build(part)
    if os.path.lexists(final): os.rename(final, old)
    os.rename(part, final)
    if os.path.isdir(old): shutil.rmtree(old, ignore_errors=True)
    elif os.path.lexists(old): os.unlink(old)


# ---------- the steps: each returns (stamp: what it was built from, ran: whether it did any work) or raises Fail ----------

def game_ok(c):
    """Why the root's game files are not usable, or None when they are: the three program files of 1.0.34.0 and the resources (romfs_ok)"""
    for name, want in EXEFS_SHA.items():
        p = os.path.join(c.game, 'ExeFS', name)
        if not os.path.isfile(p): return f'ExeFS/{name} is missing'
        if sha256(p) != want: return f'ExeFS/{name} is not from version 1.0.34.0 ({VERSION})'
    return romfs_ok(os.path.join(c.game, 'RomFS', 'data'))


def romfs_ok(data):
    """Why a RomFS data folder is not usable, or None: the resource manifest and every archive the game reads, at its full size"""
    if not os.path.isfile(os.path.join(data, 'build_manifest.psv')): return 'RomFS/data/build_manifest.psv is missing'
    for name, size in ROMFS_SIZES.items():
        try: have = os.path.getsize(os.path.join(data, name))
        except OSError: return f'RomFS/data/{name} is missing'
        if have != size: return f'RomFS/data/{name} is damaged ({have:,} bytes instead of {size:,})'
    return None


def find_update(folder):
    """An extracted game folder → the folder that holds ExeFS and RomFS (the folder itself, its Update, or the dm-work layout under it)"""
    for cand in (folder, os.path.join(folder, 'Update'), os.path.join(folder, 'data', 'switch', VERSION, 'Update')):
        if os.path.isdir(os.path.join(cand, 'ExeFS')) and os.path.isdir(os.path.join(cand, 'RomFS')): return cand
    return None


def step_unpack(c, a):
    why = game_ok(c)
    if why is None: return 'game ' + EXEFS_SHA['main'][:16], False
    if os.path.exists(c.game):
        if not (a.source or a.game):
            raise Fail(f'your game files are damaged or incomplete ({why}): choose your game packages or an extracted folder again', code='game_damaged')
        with c.critical(): os.rename(c.game, c.game + '.damaged.' + str(os.getpid()))   # out of the way at once; deleted below
        shutil.rmtree(c.game + '.damaged.' + str(os.getpid()), ignore_errors=True)
    if a.source:
        src = find_update(os.path.abspath(os.path.expanduser(a.source)))
        if not src: raise Fail(f'{a.source} holds no extracted game files (a folder with ExeFS and RomFS)')
        os.makedirs(os.path.dirname(c.game), exist_ok=True)
        # cp -c clones on the same APFS volume (no data copied, instant); across volumes it is a normal copy. Both follow links (-L, symlinks=False):
        # the install keeps its own files, never a link into a folder or a disk that may go away
        def copy(part):
            if subprocess.run(['cp', '-c', '-R', '-L', src, part], capture_output=True).returncode:
                shutil.rmtree(part, ignore_errors=True); shutil.copytree(src, part, symlinks=False)
        write_atomic(c.game, copy)
    elif a.game:
        if not a.keys: raise Fail('the game packages need your prod.keys too (--keys)')
        c.run('unpack', ['bash', c.dm('tools', 'extract_game.sh'), *[os.path.abspath(os.path.expanduser(p)) for p in a.game], os.path.abspath(os.path.expanduser(a.keys))])
    else:
        raise Fail('no game files yet: give your game packages and prod.keys (--game … --keys …) or an extracted folder (--from …)', code='no_game')
    why = game_ok(c)
    if why: raise Fail(why, code='game_damaged')
    return 'game ' + EXEFS_SHA['main'][:16], True


def product_stamp(c):
    """What the rewrite depends on: the rewriter, its driver, the boot block lists it copies, the product name and the tools pack's Python"""
    return tree_sha([c.dm('tools', 'patch_nso.py'), c.dm('tools', 'product.sh'), c.dm('PRODUCT'), *sorted(glob.glob(c.dm('data', 'bootlist_*.tsv')))]) + \
        ' ' + os.path.basename(c.tools.rstrip('/'))


def product_dir(c, stamp=None):
    """The rewrite's output, one folder per stamp (build/products/<product>-touch-<digest>): a new release writes a new folder, so the one an installed
    app runs from is never rewritten under it"""
    p = read(c.dm('PRODUCT')).splitlines()[0].strip()
    return os.path.join(c.build, 'products', f'{p}-touch-{(stamp or product_stamp(c)).split()[0]}')


def step_product(c, a):
    stamp = product_stamp(c); d = product_dir(c, stamp); want = sha256(c.dm('tools', 'patch_nso.py'))
    def done(path):
        try: return read(os.path.join(path, '.complete')).strip() == want
        except OSError: return False
    if not a.force and done(d): return os.path.basename(d) + ' ' + want[:16], False
    write_atomic(d, lambda part: c.run('product', ['bash', c.dm('tools', 'product.sh'), 'build', 'touch'], env=c.env(DM_PRODUCT_DIR=part)))
    if not done(d): raise Fail('the rewrite did not complete')
    return os.path.basename(d) + ' ' + want[:16], True


def step_packs(c, a):
    d = os.path.join(c.build, 'cache', 'dm', 'dmsh')
    def have(): return [n for n in os.listdir(d) if not n.endswith('.part')] if os.path.isdir(d) else []
    if not a.force and len(have()) == 162: return 'packs 162', False
    os.makedirs(d, exist_ok=True)
    for n in os.listdir(d):
        if n.endswith('.part'): os.unlink(os.path.join(d, n))
    env = c.env(DMX_ROOT=os.path.join(c.game, 'RomFS', 'data'))
    r = c.capture([c.python, *PY, c.dm('tools', 'dmx.py'), 'list', 'data:data/hd/shaders/*.bin'], env=env)
    if r.returncode: raise Fail(f'cannot list the shader packs in the game files (dmx.py exited with {r.returncode}; the log has its output)')
    names = [n.split('\t')[0] for n in r.stdout.split('\n') if n.strip()]
    for i, n in enumerate(names):
        dest = os.path.join(d, os.path.basename(n))
        c.run('packs', [c.python, *PY, c.dm('tools', 'dmx.py'), 'get', n, dest + '.part'], env=env)
        os.replace(dest + '.part', dest)
        c.event(event='progress', step='packs', done=i + 1, total=len(names))
    if len(have()) != 162: raise Fail(f'{len(have())} shader packs instead of 162')
    return 'packs 162', True


def chain(c):
    r = c.capture([c.python, *PY, c.dm('gfx', 'tools', 'dmsh_chain.py')], timeout=120)
    v = r.stdout.strip()
    if not re.fullmatch(r'[0-9a-f]{12}', v): raise Fail('cannot read the shader chain version' + (f': {english(r.stderr.strip().splitlines()[-1])}' if r.stderr.strip() else ''))
    return v


def msl_root(c): return os.path.join(c.build, 'cache', 'metal_shader_cache')


def shaders_ok(c, v):
    d = os.path.join(msl_root(c), 'b-' + v); res = load_json(os.path.join(d, '_b_build.json'))
    if not res or any(r['st'] != 'ok' for r in res): return False
    have = set(os.listdir(d))
    return all(r['key'] + '_b.metal' in have and r['key'] + '_b.json' in have for r in res)


def step_shaders(c, a):
    v = chain(c)
    if not a.force and shaders_ok(c, v): return 'chain ' + v, False
    bad = c.capture([c.python, *PY, c.dm('gfx', 'tools', 'dmsh_chain.py'), '--check-tools'], timeout=300)
    if bad.returncode:
        said = (bad.stdout.strip() or bad.stderr.strip() or '?').splitlines()[0]
        raise Fail('the shader tools do not match this release: ' + (english(said) or said)[:200])
    n = os.cpu_count() or 8
    work = os.path.join(c.build, 'tmp'); os.makedirs(work, exist_ok=True)   # the translator's scratch folders, inside the root: the start sweep clears them
    c.run('shaders', [c.python, *PY, c.dm('gfx', 'tools', 'dmsh_build.py'), os.path.join(c.build, 'cache', 'dm', 'dmsh'), msl_root(c)],
          env=c.env(J=str(max(2, min(8, n // 2))), DMSH_PROGRESS='1', WORK=work),
          progress=lambda l: (lambda m: (int(m[1]), int(m[2])) if m else None)(re.match(r'DMSH (\d+)/(\d+)', l)))
    if not shaders_ok(c, v): raise Fail('some shaders were not translated (see the log)')
    return 'chain ' + v, True


def step_c1(c, a):
    v = chain(c); out = os.path.join(c.build, 'c1_full.tsv')
    stamp = f"chain {v} guard {sha256(c.dm('gfx', 'tools', 'c1_guard.py'))[:12]}"
    try: head = read(out).split('\n', 1)[0] + '\n'
    except OSError: head = ''
    if not a.force and c.state['steps'].get('c1') == stamp and head.startswith(f'# chain b-{v} '): return stamp, False
    write_atomic(out, lambda part: c.run('c1', [c.python, *PY, c.dm('gfx', 'tools', 'c1_guard.py'), os.path.join(msl_root(c), 'b-' + v), part]))
    return stamp, True


def overlay_dir(c, stamp): return os.path.join(c.build, 'overlay', stamp)


def load_overlay_contract(c):
    spec = importlib.util.spec_from_file_location('overlay_contract', c.dm('ios', 'tools', 'overlay_contract.py'))
    if not spec or not spec.loader: raise Fail('the runtime pack has no overlay_contract.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


def overlay_stamp(c):
    """The overlay's inputs: the generator and everything it reads (overlay_contract.SOURCE_INPUTS, which covers the texture code outside dm/ios),
    the interface art, the strings, the product name, and the deswizzle library it loads"""
    try: contract = load_overlay_contract(c).source_fingerprint(c.repo)
    except Exception as e:   # noqa: BLE001 -- a runtime pack without the contract's inputs
        raise Fail(f'the runtime pack is incomplete: {e}')
    lib = os.path.join(c.runtime, 'lib', 'libnvn_deswizzle.dylib')
    h = hashlib.sha256((tree_sha([c.dm('ios', 'tools'), c.dm('ios', 'NativeHUD'), c.dm('i18n'), c.dm('data'), c.dm('PRODUCT')]) + contract +
                        (sha256(lib) if os.path.isfile(lib) else '-')).encode())
    return h.hexdigest()[:16]


def step_overlay(c, a):
    s = overlay_stamp(c); d = overlay_dir(c, s)
    if not a.force and os.path.isfile(os.path.join(d, 'romfs_overlay', 'overlay.manifest')): return s, False
    def gen(part):
        os.makedirs(part)
        c.run('overlay', [c.python, *PY, c.dm('ios', 'tools', 'gen_romfs_overlay.py'), os.path.join(c.game, 'RomFS'), os.path.join(part, 'romfs_overlay')],
              env=c.env(DM_OVERLAY_MAC='1', DM_DESWIZZLE_LIB=os.path.join(c.runtime, 'lib', 'libnvn_deswizzle.dylib'), DM_GEN_ASSETS=os.path.join(c.build, 'gen_assets')))
        if not os.path.isfile(os.path.join(part, 'romfs_overlay', 'overlay.manifest')): raise Fail('the interface overlay was not generated')
    write_atomic(d, gen)
    return s, True


def tables_stamp(c): return tree_sha([c.dm('ios', 'tools', 'gen_hud_exp.py'), c.dm('ios', 'tools', 'romfs_art.py'), c.dm('tools', 'gen_bright_levels.py'), c.dm('ios', 'tools', 'touch_layout.py')])


def step_tables(c, a):
    s = tables_stamp(c); d = os.path.join(c.build, 'tables', s)
    if not a.force and all(os.path.isfile(os.path.join(d, n)) for n in ('hud_exp.tsv', 'bright_levels.tsv')): return s, False
    rom = os.path.join(c.game, 'RomFS')
    def gen(part):
        os.makedirs(part)
        for script, name in ((c.dm('ios', 'tools', 'gen_hud_exp.py'), 'hud_exp.tsv'), (c.dm('tools', 'gen_bright_levels.py'), 'bright_levels.tsv')):
            c.run('tables', [c.python, *PY, script, '--tsv', rom, os.path.join(part, name)])
    write_atomic(d, gen)
    return s, True


def info_plist(c, m):
    d = {'CFBundleName': 'D2R Metal', 'CFBundleDisplayName': 'D2R Metal', 'CFBundleIdentifier': c.bundle_id, 'CFBundleExecutable': 'D2RMobile',
         'CFBundleIconFile': 'AppIcon', 'CFBundleIconName': 'AppIcon', 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': m.get('version', '0'), 'CFBundleVersion': str(m.get('build', 1)),
         'LSMinimumSystemVersion': m.get('macos_min', '27.0'), 'NSHighResolutionCapable': True, 'LSApplicationCategoryType': 'public.app-category.role-playing-games',
         'GCSupportsControllerUserInteraction': True, 'DMLauncherManaged': True}   # DMLauncherManaged: an app this engine may replace (see install_app)
    if m.get('update_url'): d['SWUpdateURL'] = m['update_url']
    return d


def app_paths(c):
    st = c.state['steps']
    return {'product': product_dir(c), 'msl': msl_root(c), 'romfs': c.game,
            'romfs_overlay': os.path.join(overlay_dir(c, st['overlay']), 'romfs_overlay'),
            'romfs_overlay_variants': os.path.join(overlay_dir(c, st['overlay']), 'romfs_overlay_variants'),
            'tables': os.path.join(c.build, 'tables', st['tables'])}


def app_stamp(c, m):
    """Everything the app is assembled from: the release, the bundle id, the host program, the shader tables, the folders it points at and the icon"""
    h = hashlib.sha256()
    for f in (os.path.join(c.runtime, 'host', 'D2RMobile'), c.dm('gfx', 'nvn', 'shader_map.tsv'), c.dm('gfx', 'nvn', 'pso_manifest.tsv'),
              os.path.join(c.build, 'c1_full.tsv'), os.path.join(c.runtime, 'app', 'Assets.car'), os.path.join(c.runtime, 'app', 'AppIcon.icns')):
        h.update((sha256(f) if os.path.isfile(f) else '-').encode() + b'\0')
    h.update(json.dumps(app_paths(c), sort_keys=True).encode() + json.dumps(info_plist(c, m), sort_keys=True).encode() + c.app_dir.encode())
    return f"{m.get('version', '?')} {m.get('build', '?')} {c.bundle_id} {h.hexdigest()[:16]}"


def step_app(c, a):
    """Assembles the app in a staging folder, signs it (ad-hoc: it never leaves this Mac and is never quarantined, so Gatekeeper does not look at it)
    and swaps it in; the one it replaces becomes the previous version when it is another build. Skipped when the installed app is this very one"""
    m = c.manifest; dest = os.path.join(c.app_dir, APP_NAME); stamp = app_stamp(c, m)
    if not a.force and c.state['steps'].get('app') == stamp and managed(dest) and (release_of(dest) or {}).get('build') == m.get('build'):
        record_apps(c); return stamp, False
    stage = os.path.join(c.build, 'app.new'); app = os.path.join(stage, APP_NAME)
    shutil.rmtree(stage, ignore_errors=True)
    res = os.path.join(app, 'Contents', 'Resources'); os.makedirs(res); os.makedirs(os.path.join(app, 'Contents', 'MacOS'))
    shutil.copy2(os.path.join(c.runtime, 'host', 'D2RMobile'), os.path.join(app, 'Contents', 'MacOS', 'D2RMobile'))
    for name in ('shader_map.tsv', 'pso_manifest.tsv'): shutil.copy2(c.dm('gfx', 'nvn', name), os.path.join(res, name))
    shutil.copy2(os.path.join(c.build, 'c1_full.tsv'), os.path.join(res, 'c1_full.tsv'))
    for name in ('Assets.car', 'AppIcon.icns'):   # the icon (dm/brand/icon.sh): the layered one in Assets.car, the .icns as its fallback
        icon = os.path.join(c.runtime, 'app', name)
        if os.path.isfile(icon): shutil.copy2(icon, os.path.join(res, name))
    paths = app_paths(c)
    with open(os.path.join(res, 'paths.txt'), 'w', encoding='utf-8') as f:
        for k, v in paths.items():
            if os.path.exists(v): f.write(f'{k}={v}\n')
    with open(os.path.join(res, 'build.txt'), 'w', encoding='utf-8') as f:
        f.write(f"version {m.get('version', '?')} build {m.get('build', '?')} product {os.path.basename(paths['product'])} commit {m.get('commit', '?')} launcher install\n")
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'wb') as f: plistlib.dump(info_plist(c, m), f)
    for cmd in (['codesign', '--force', '-s', '-', app], ['codesign', '--verify', '--deep', '--strict', app]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: raise Fail(f'{" ".join(cmd[:2])} failed: {r.stderr.strip()[:200]}')
    install_app(c, app, m.get('build')); shutil.rmtree(stage, ignore_errors=True)   # empty once the app has moved into place
    return stamp, True


def managed(app):
    if os.path.islink(app): return False
    try:
        with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: return bool(plistlib.load(f).get('DMLauncherManaged'))
    except (OSError, ValueError, plistlib.InvalidFileException): return False


def release_of(app):
    """The release an installed app was built from, as its own Info.plist and build.txt say; None when there is no app"""
    try:
        with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: p = plistlib.load(f)
    except (OSError, ValueError, plistlib.InvalidFileException): return None
    try: build = int(p.get('CFBundleVersion', ''))
    except ValueError: build = None
    try: commit = (re.search(r'\bcommit (\S+)', read(os.path.join(app, 'Contents', 'Resources', 'build.txt'))) or [None, None])[1]
    except OSError: commit = None
    return {'version': p.get('CFBundleShortVersionString'), 'build': build, 'commit': commit}


def previous_path(c): return os.path.join(c.build, 'previous.noindex', APP_NAME)


def previous_app(c):
    """Where the replaced app is kept for revert: build/previous.noindex, out of app search; a copy an older engine kept in build/previous moves there"""
    prev = previous_path(c); legacy = os.path.join(c.build, 'previous', APP_NAME)
    if os.path.isdir(legacy):
        if os.path.lexists(prev): shutil.rmtree(legacy)
        else: os.makedirs(os.path.dirname(prev), exist_ok=True); shutil.move(legacy, prev)
    if os.path.isdir(os.path.dirname(legacy)) and not os.listdir(os.path.dirname(legacy)): os.rmdir(os.path.dirname(legacy))
    return prev


def record_apps(c):
    """state.json from the apps themselves: where the app is and which release it and the previous one are (read from their Info.plist, so the
    state never names a release that is not installed), and the packs of each"""
    dest, prev = os.path.join(c.app_dir, APP_NAME), previous_path(c)
    c.state['app'] = dest; c.state['bundle_id'] = c.bundle_id
    c.state['release'] = release_of(dest); c.state['previous_release'] = release_of(prev) if os.path.isdir(prev) else None
    if c.runtime and (c.state['release'] or {}).get('build') == c.manifest.get('build'):
        c.state['packs'] = {'runtime': c.runtime, 'tools': c.tools}   # what the launcher runs this engine from for revert / repair / status
    rf = c.state.get('reverted_from')
    if rf is not None and ((c.state['release'] or {}).get('build') or 0) >= rf: c.state.pop('reverted_from', None)
    c.save_state()


def install_app(c, app, build):
    dest = os.path.join(c.app_dir, APP_NAME); prev = previous_app(c)
    if os.path.lexists(dest) and not managed(dest):
        raise Fail(f'{dest} exists and was not installed by the launcher (a link or another build): move it aside, or choose another folder for the app')
    os.makedirs(c.app_dir, exist_ok=True); os.makedirs(os.path.dirname(prev), exist_ok=True)

    def retire(old):
        """The app being replaced: the previous version when it is another build (its packs with it); the same build again just goes"""
        if (release_of(old) or {}).get('build') != build:
            shutil.rmtree(prev, ignore_errors=True); shutil.move(old, prev)   # a rename on one volume, a copy across volumes
            if c.state.get('packs'): c.state['previous_packs'] = c.state['packs']
        else:
            gone = os.path.join(c.build, 'replaced.tmp'); shutil.rmtree(gone, ignore_errors=True); shutil.move(old, gone); shutil.rmtree(gone, ignore_errors=True)

    with c.critical():
        # the app moved since the last install (launcher 0.9.7 keeps it in its own folder instead of /Applications): the copy at the old place is the
        # one being replaced, and it leaves its old folder
        old = c.state.get('app')
        if old and not os.path.lexists(dest) and os.path.realpath(old) != os.path.realpath(dest) and os.path.isdir(old) and managed(old): retire(old)
        if os.path.lexists(dest): retire(dest)
        shutil.move(app, dest)
        record_apps(c)


def step_warm(c, a):
    # again for every new app (cheap when nothing changed: ~1 s from the cache), every macOS build and every chain
    v = chain(c); key = f"{v} {os_build()} {c.state['steps'].get('app')}"
    if c.state['steps'].get('warm') == key and not a.force: return key, False
    exe = os.path.join(c.app_dir, APP_NAME, 'Contents', 'MacOS', 'D2RMobile')
    c.run('warm', [exe, '--warm-shaders'], env=c.env(),
          progress=lambda l: (lambda m: (int(m[1]), int(m[2])) if m else None)(re.match(r'WARM (\d+)/(\d+)', l)))
    return key, True


STEP_FN = {'unpack': step_unpack, 'product': step_product, 'packs': step_packs, 'shaders': step_shaders, 'c1': step_c1,
           'overlay': step_overlay, 'tables': step_tables, 'app': step_app, 'warm': step_warm}


# ---------- housekeeping ----------

def sweep(c):
    """At the start of a run, under the lock: what a killed run (SIGKILL, power loss) left behind. The extraction's folders carry their owner's pid;
    the key copies live in the system's temporary folder, so only those whose owner is gone go"""
    def gone(path): shutil.rmtree(path, ignore_errors=True) if os.path.isdir(path) and not os.path.islink(path) else None
    for d in glob.glob(os.path.join(c.build, 'cache', 'dm', 'extract.*')):
        pid = d.rsplit('.', 1)[1]
        if not pid.isdigit() or not pid_alive(int(pid)): gone(d)
    tmp = os.environ.get('TMPDIR') or '/tmp'
    for d in glob.glob(os.path.join(tmp, 'dm-keys.*')):
        try: owner = int(read(os.path.join(d, '.owner')).strip())
        except (OSError, ValueError): owner = None
        try: old = time.time() - os.stat(d).st_mtime > 3600
        except OSError: continue
        if (owner and not pid_alive(owner)) or (owner is None and old): gone(d)
    for d in (os.path.join(c.build, 'tmp'), os.path.join(c.build, 'app.new'), os.path.join(c.build, 'replaced.tmp'), c.game + '.part'): gone(d)
    for d in glob.glob(c.game + '.damaged.*'): gone(d)
    # write_atomic's half-built outputs (a .part) and replaced ones (a .old) of a run killed in the middle
    for pattern in ('products/*.part', 'products/*.old', 'overlay/*.part', 'overlay/*.old', 'tables/*.part', 'tables/*.old', 'cache/dm/dmsh/*.part'):
        for d in glob.glob(os.path.join(c.build, pattern)):
            if os.path.isdir(d) and not os.path.islink(d): gone(d)
            else: os.unlink(d)
    for f in (os.path.join(c.build, 'c1_full.tsv.part'), os.path.join(c.build, 'c1_full.tsv.old')):
        if os.path.lexists(f): os.unlink(f)


def referenced(c):
    """The folders the installed app and the previous one run from (their paths.txt), the shader libraries they load (their c1_full.tsv names the
    chain) and the packs their releases came with"""
    keep, chains = set(), set()
    for app in (os.path.join(c.app_dir, APP_NAME), previous_path(c), c.state.get('app') or ''):
        res = os.path.join(app, 'Contents', 'Resources')
        try:
            for line in read(os.path.join(res, 'paths.txt')).splitlines(True):
                if '=' in line: keep.add(os.path.realpath(line.rstrip('\n').split('=', 1)[1]))
        except OSError: pass
        try:
            m = re.match(r'# chain (b-[0-9a-f]{12}) ', read(os.path.join(res, 'c1_full.tsv')).split('\n', 1)[0])
            if m: chains.add(m[1])
        except OSError: pass
    for packs in (c.state.get('packs'), c.state.get('previous_packs')):
        for k in ('runtime', 'tools'):
            if packs and packs.get(k): keep.add(os.path.realpath(packs[k]))
    return keep, chains


def prune(c):
    """After a successful install: what neither the installed app nor the previous one uses goes (older releases' packs, rewrite products,
    overlays, tables, shader libraries and their translation cache entries), and the logs beyond the last KEEP_LOGS. Only in folders this engine
    writes, only names it gives"""
    if os.path.isdir(os.path.join(c.build, 'cache', 'dm', 'venv')): return   # a developer's workspace (./setup.sh made a venv), never a launcher's root
    keep, chains = referenced(c)
    def used(path): p = os.path.realpath(path); return any(k == p or k.startswith(p + os.sep) for k in keep)
    def gone(path):
        c.write(f'prune {path}\n')
        if os.path.isdir(path) and not os.path.islink(path): shutil.rmtree(path, ignore_errors=True)
        elif os.path.lexists(path): os.unlink(path)
    for parent, pattern in ((os.path.join(c.root, 'releases'), '*'), (os.path.join(c.root, 'tools'), '*'), (os.path.join(c.build, 'products'), '*-touch-*'),
                            (os.path.join(c.build, 'cache', 'dm', 'out'), '*-touch'), (os.path.join(c.build, 'overlay'), '*'), (os.path.join(c.build, 'tables'), '*')):
        for d in glob.glob(os.path.join(parent, pattern)):
            if not used(d): gone(d)
    if chains:
        for d in glob.glob(os.path.join(msl_root(c), 'b-*')):
            if os.path.basename(d) not in chains: gone(d)
        # the translation cache: one entry per shader per input (cas/index/<key>.json names the newest); older entries are never read again
        cas = os.path.join(msl_root(c), 'cas'); live = set()
        for f in glob.glob(os.path.join(cas, 'index', '*.json')):
            live.add((load_json(f) or {}).get('digest'))
        for d in glob.glob(os.path.join(cas, 'shader', '*')):
            if os.path.basename(d) not in live: gone(d)
    logs = sorted(glob.glob(os.path.join(c.root, 'logs', 'install-*.log')))
    for f in logs[:-KEEP_LOGS]:
        if f != c.log_path: gone(f)


def recover_revert(c):
    """A revert cut short by power loss (a stop request waits for the swap) leaves one app in build/revert.tmp: every copy goes back to its place"""
    tmp = os.path.join(c.build, 'revert.tmp')
    if not os.path.isdir(tmp): return
    dest, prev = os.path.join(c.app_dir, APP_NAME), previous_path(c)
    if not os.path.lexists(dest):
        if os.path.isdir(prev): shutil.move(prev, dest); shutil.move(tmp, prev)   # stopped after the installed app moved out
        else: shutil.move(tmp, dest)
    elif not os.path.lexists(prev): shutil.move(tmp, prev)                         # stopped after the previous one moved in
    else: shutil.rmtree(tmp, ignore_errors=True)


# ---------- commands ----------

def need_gb(c, a, need_game):
    if not need_game: return NEED_GB
    if getattr(a, 'game', None): return NEED_GB_PACKAGES
    src = find_update(os.path.abspath(os.path.expanduser(a.source))) if getattr(a, 'source', None) else None
    if src and os.stat(src).st_dev != os.stat(existing(c.root)).st_dev:
        return NEED_GB + sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(src) for f in fs) / 2**30
    return NEED_GB


def preflight(c, a, need_game):
    """Problems that stop an install before anything is written: (code, text) pairs"""
    out = []
    if platform.machine() != 'arm64': out.append(('arch', 'D2R Metal needs a Mac with Apple silicon'))
    osv = platform.mac_ver()[0]
    try: major = int(osv.split('.')[0])
    except ValueError: major = 0
    if major < 27: out.append(('macos', f'D2R Metal needs macOS 27 or later (this Mac has {osv or "?"})'))
    want = need_gb(c, a, need_game)
    if free_gb(c.root) < want: out.append(('disk', f'{free_gb(c.root):.0f} GB free where the game goes; at least {want:.0f} GB is needed'))
    if c.tools:
        for p in (c.python, os.path.join(c.tools, 'dmsh_xlate', 'dmsh_xlate'), os.path.join(c.tools, 'bin', 'spirv-cross'), os.path.join(c.tools, 'bin', 'hactool')):
            if not os.access(p, os.X_OK): out.append(('tools', f'the tools pack lacks {os.path.relpath(p, c.tools)}'))
    if c.runtime and not os.path.isfile(os.path.join(c.runtime, 'host', 'D2RMobile')): out.append(('runtime', 'the runtime pack has no host/D2RMobile'))
    return out


def cmd_install(c, a, force=False):
    a.force = force
    need_game = game_ok(c) is not None
    probs = preflight(c, a, need_game)
    for code, text in probs: c.event(event='error', code=code, msg=text)
    if probs: return 1
    m = c.manifest
    c.event(event='start', release=m.get('version'), build=m.get('build'), steps=STEPS, typical_s=TYPICAL_S, log=c.log_path, protocol=PROTOCOL)
    for name in STEPS:
        t0 = time.time()
        c.event(event='step', step=name, state='start', typical_s=TYPICAL_S[name])
        try: stamp, ran = STEP_FN[name](c, a)
        except Fail as e:
            msg = str(e)
            if free_gb(c.root) < 1: msg += f' (the disk is nearly full: {free_gb(c.root):.1f} GB free)'
            c.event(event='error', step=name, code=e.code, msg=msg); c.event(event='step', step=name, state='fail', msg=msg); return 1
        except Exception as e:   # noqa: BLE001 -- anything else (a full disk, a permission) is still this step's failure, with its cause in the log
            c.write(traceback.format_exc())
            msg, code = unexpected(e)
            c.event(event='error', step=name, code=code, msg=msg); c.event(event='step', step=name, state='fail', msg=msg); return 1
        c.state['steps'][name] = stamp; c.save_state()
        c.event(event='step', step=name, state='done' if ran else 'kept', seconds=round(time.time() - t0, 1), stamp=stamp)
    record_apps(c)
    try: prune(c)
    except Exception:   # noqa: BLE001 -- the install itself is done; a failed clean-up is only logged
        c.write(traceback.format_exc())
    c.event(event='done', msg=f"D2R Metal {m.get('version', '?')} is installed in {c.app_dir}", app=c.state['app'])
    return 0


def cmd_revert(c, a):
    dest, prev = os.path.join(c.app_dir, APP_NAME), previous_app(c)
    if not os.path.isdir(prev): c.event(event='error', code='no_previous', msg='there is no previous version to go back to'); return 1
    if os.path.lexists(dest) and not managed(dest): c.event(event='error', msg=f'{dest} was not installed by the launcher'); return 1
    was = (release_of(dest) or {}).get('build')
    tmp = os.path.join(c.build, 'revert.tmp')
    with c.critical():
        shutil.rmtree(tmp, ignore_errors=True)
        if os.path.lexists(dest): shutil.move(dest, tmp)
        shutil.move(prev, dest)
        if os.path.isdir(tmp): shutil.move(tmp, prev)
        packs, before = c.state.get('packs'), c.state.get('previous_packs')
        c.state['packs'] = before or packs; c.state['previous_packs'] = packs if before else None   # unknown packs (an older engine's state): keep these
        c.state['steps'].pop('app', None); c.state['steps'].pop('warm', None)   # the next install rebuilds the app; the warm pass is rechecked
        if was is not None: c.state['reverted_from'] = was   # the launcher offers that build again as "Update to …", never by itself
        record_apps(c)
    c.event(event='done', msg='the previous version is back', app=dest)
    return 0


def cmd_status(c, a):
    why = game_ok(c)
    s = dict(c.state, root=c.root, protocol=PROTOCOL, game=why or 'ok', game_files=why is None, game_present=os.path.isdir(c.game),
             app_present=managed(os.path.join(c.app_dir, APP_NAME)), previous_present=os.path.isdir(previous_path(c)) or os.path.isdir(os.path.join(c.build, 'previous', APP_NAME)))
    print(json.dumps(s, indent=None if a.json else 1, ensure_ascii=False))
    return 0


def cmd_check(c, a):
    probs = preflight(c, a, game_ok(c) is not None)
    for code, text in probs: c.event(event='error', code=code, msg=text)
    c.event(event='done', msg='ready' if not probs else f'{len(probs)} problem(s)', game=game_ok(c) or 'ok')
    return 1 if probs else 0


def unexpected(e):
    """(message, code) for an error no step expected"""
    if isinstance(e, OSError) and e.errno == errno.ENOSPC:
        return 'the disk is full: free some space and run it again (it resumes where it stopped)', 'disk'
    if isinstance(e, OSError) and e.errno in (errno.EACCES, errno.EPERM):
        return f'macOS refused access to {e.filename or "a file"} ({e.strerror}); the log has the details', 'access'
    return f'unexpected error: {type(e).__name__}: {e} (the log has the details)', 'internal'


def main(argv):
    ap = argparse.ArgumentParser(prog='install.py', description='Install engine of the D2R Metal Launcher')
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('check', 'install', 'repair', 'revert', 'status'):
        p = sub.add_parser(name); p.add_argument('--root', required=True); p.add_argument('--json', action='store_true')
        if name in ('check', 'install', 'repair'):
            p.add_argument('--runtime', required=name != 'check'); p.add_argument('--tools', required=name != 'check')
        if name in ('install', 'repair'):
            p.add_argument('--game', nargs='+'); p.add_argument('--keys'); p.add_argument('--from', dest='source')
            p.add_argument('--bundle-id')
        if name in ('install', 'repair', 'revert', 'status'): p.add_argument('--app-dir')
    a = ap.parse_args(argv)
    c = Ctx(a)
    if a.cmd == 'status': return cmd_status(c, a)
    c.open_log()
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP): signal.signal(sig, c.on_signal)
    threading.Thread(target=c.watch, args=(os.getppid() if a.json else 0,), daemon=True).start()
    try:
        if a.cmd == 'check': return cmd_check(c, a)
        if not c.take_lock():
            c.event(event='error', code='busy', msg='another install or update is running for this game folder; wait for it to end'); return 1
        sweep(c); recover_revert(c)
        if a.cmd == 'install': return cmd_install(c, a)
        if a.cmd == 'repair': return cmd_install(c, a, force=True)
        return cmd_revert(c, a)
    except Interrupted as e:
        c.event(event='error', code='interrupted', msg=f'stopped ({e}); run the same command again to resume'); return 130
    except Exception as e:   # noqa: BLE001 -- never a silent failure: the cause goes to the log and to the launcher
        c.write(traceback.format_exc())
        msg, code = unexpected(e)
        c.event(event='error', code=code, msg=msg); return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
