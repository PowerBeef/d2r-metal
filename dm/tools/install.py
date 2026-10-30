#!/usr/bin/env python3
# Install engine of the D2R Metal Launcher (docs/LAUNCHER.md): builds the Mac app on the player's own Mac from their own game files, with the prebuilt tools
# of a GitHub release, and on an update redoes only what the release changed. Standard library only: it runs with the tools pack's Python (python3 -I).
#
# The install root is a workspace like ~/dm-work (it is the DM_ROOT of every step): the game files under data/switch/3.3.94012/Update and the build outputs
# under build/, so the repository's own scripts (extract_game.sh, product.sh, dmx.py, dmsh_build.py, c1_guard.py, gen_romfs_overlay.py) run on it unchanged.
# Nothing here needs Xcode, Homebrew, .NET or a compiler: the shader chain is of the source form (dmsh_chain.py --update-lock --runtime) and the app
# compiles its libraries with macOS's own Metal compiler, warmed once by D2RMobile --warm-shaders.
#
#   install.py check   --root R --runtime P --tools T                     what this Mac has and lacks; changes nothing
#   install.py install --root R --runtime P --tools T [--game <pkg>… --keys <prod.keys> | --from <folder>] [--app-dir D] [--bundle-id ID] [--json]
#                      the whole pipeline, each step skipped when its stamp still matches: the first install, an update (the new release's packs) and a
#                      resume after an interruption are the same command; game files are needed only when the root has none yet
#   install.py repair  …the same arguments…   every step again
#   install.py revert  --root R [--app-dir D]                                swaps the app back to the one before the last install or update
#   install.py status  --root R [--json]
# P = an unpacked runtime pack (manifest.json, host/D2RMobile, repo/dm/…, lib/libnvn_deswizzle.dylib), T = an unpacked tools pack (python/bin/python3,
# dmsh_xlate/dmsh_xlate, bin/spirv-cross, bin/hactool). --json prints one JSON event per line on stdout (what the launcher reads); the steps' own output
# goes to <R>/logs/install-<time>.log either way. Keys and game packages are only read, in place: never copied, never kept.
import argparse, datetime, hashlib, json, os, platform, plistlib, re, shutil, subprocess, sys, time

VERSION = '3.3.94012'
EXEFS_SHA = {'main': '55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e',   # the three program files of 1.0.34.0 (= setup.sh, extract_game.sh)
             'sdk': '1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017',
             'subsdk0': '67a38fcf89cd13e040a021eb3d157072f38ebf6680a907874c35c7b231b5d4e2'}
APP_NAME = 'D2R Metal.app'
BUNDLE_ID = 'org.d2rmobile.mac'   # the data-bearing id: saves, settings and the Metal cache of earlier builds carry over
STEPS = ['unpack', 'product', 'packs', 'shaders', 'c1', 'overlay', 'tables', 'app', 'warm']
TYPICAL_S = {'unpack': 300, 'product': 15, 'packs': 20, 'shaders': 120, 'c1': 10, 'overlay': 20, 'tables': 5, 'app': 5, 'warm': 240}   # M6, for the ETA
NEED_GB, NEED_GB_GAME = 30, 15
PY = ['-E', '-s']   # the steps' Python: no PYTHON* variables, no user site; not -I, which would drop the script's own folder that these scripts import from   # free space: unpacking the packages takes ~20 GB on top of the ~12 GB workspace


class Fail(Exception):
    pass


class Ctx:
    def __init__(self, a):
        self.root = os.path.abspath(os.path.expanduser(a.root))
        self.runtime = os.path.abspath(os.path.expanduser(a.runtime)) if getattr(a, 'runtime', None) else ''   # '' = not given (check, revert, status)
        self.tools = os.path.abspath(os.path.expanduser(a.tools)) if getattr(a, 'tools', None) else ''
        self.app_dir = os.path.abspath(os.path.expanduser(getattr(a, 'app_dir', None) or '/Applications'))
        self.bundle_id = getattr(a, 'bundle_id', None) or BUNDLE_ID
        self.json = getattr(a, 'json', False)
        self.game = os.path.join(self.root, 'data', 'switch', VERSION, 'Update')
        self.build = os.path.join(self.root, 'build')
        self.state_path = os.path.join(self.build, 'state.json')
        os.makedirs(os.path.join(self.root, 'logs'), exist_ok=True)
        self.log_path = os.path.join(self.root, 'logs', 'install-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.log')
        self.log = open(self.log_path, 'a', encoding='utf-8')
        self.state = load_json(self.state_path) or {'steps': {}}

    # -- output: JSON events for the launcher, plain lines for a person
    def event(self, **ev):
        ev.setdefault('t', round(time.time(), 3))
        self.log.write(json.dumps(ev, ensure_ascii=False) + '\n'); self.log.flush()
        if self.json: print(json.dumps(ev, ensure_ascii=False), flush=True)
        elif ev.get('event') in ('step', 'error', 'done', 'note'):
            print(f"[{datetime.datetime.now():%H:%M:%S}] {ev.get('step', '')} {ev.get('state', ev['event'])} {ev.get('msg', '')}".rstrip(), flush=True)

    def save_state(self):
        os.makedirs(self.build, exist_ok=True)
        tmp = self.state_path + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f: json.dump(self.state, f, indent=1, ensure_ascii=False)
        os.replace(tmp, self.state_path)

    # -- the packs
    @property
    def repo(self): return os.path.join(self.runtime, 'repo')
    def dm(self, *p): return os.path.join(self.repo, 'dm', *p)
    @property
    def python(self): return os.path.join(self.tools, 'python', 'bin', 'python3')
    @property
    def manifest(self): return load_json(os.path.join(self.runtime, 'manifest.json')) or {}

    def env(self, **extra):
        """The environment of every step: this root as DM_ROOT and only the packs' tools on PATH (no Homebrew, no Xcode)"""
        e = {k: v for k, v in os.environ.items() if k in ('HOME', 'USER', 'LOGNAME', 'TMPDIR', 'LANG', 'LC_ALL', 'DOTNET_ROOT')}
        e.update(DM_ROOT=self.root, PATH=os.pathsep.join([os.path.join(self.tools, 'bin'), os.path.join(self.tools, 'python', 'bin'), '/usr/bin', '/bin', '/usr/sbin', '/sbin']),
                 XLATE=os.path.join(self.tools, 'dmsh_xlate', 'dmsh_xlate'), HACTOOL=os.path.join(self.tools, 'bin', 'hactool'), DM_PY=self.python,
                 PYTHONDONTWRITEBYTECODE='1', DEVELOPER_DIR='/nonexistent')   # an accidental xcrun fails loudly instead of using an Xcode that players don't have
        e.update(extra)
        return e

    def run(self, step, cmd, env=None, cwd=None, progress=None):
        """Runs one tool of a step; its output goes to the log. progress(line) → (done, total) or None: lines that report progress become events"""
        self.log.write(f'$ {" ".join(cmd)}\n'); self.log.flush()
        p = subprocess.Popen(cmd, env=env or self.env(), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        tail = []
        for line in p.stdout:
            self.log.write(line); tail = (tail + [line.rstrip()])[-12:]
            got = progress(line) if progress else None
            if got: self.event(event='progress', step=step, done=got[0], total=got[1])
        p.wait(); self.log.flush()
        tool = next((os.path.basename(x) for x in cmd if x.endswith(('.py', '.sh'))), os.path.basename(cmd[0]))
        if p.returncode: raise Fail(f'{tool} exited with {p.returncode}: ' + ' | '.join(l for l in tail[-3:] if l))
        return tail


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
            h.update(os.path.relpath(f, os.path.dirname(top)).encode() + b'\0'); h.update(open(f, 'rb').read()); h.update(b'\0')
    return h.hexdigest()[:16]


def os_build():
    return subprocess.run(['sysctl', '-n', 'kern.osversion'], capture_output=True, text=True).stdout.strip()


def free_gb(path):
    while not os.path.exists(path): path = os.path.dirname(path)
    st = os.statvfs(path); return st.f_bavail * st.f_frsize / 2**30


# ---------- the steps: each returns its stamp (what it was built from) or raises Fail ----------

def game_ok(c):
    """Why the root's game files are not usable, or None when they are (the three program files of 1.0.34.0 and the resource manifest)"""
    for name, want in EXEFS_SHA.items():
        p = os.path.join(c.game, 'ExeFS', name)
        if not os.path.isfile(p): return f'ExeFS/{name} is missing'
        if sha256(p) != want: return f'ExeFS/{name} is not from version 1.0.34.0 ({VERSION})'
    if not os.path.isfile(os.path.join(c.game, 'RomFS', 'data', 'build_manifest.psv')): return 'RomFS/data/build_manifest.psv is missing'
    return None


def find_update(folder):
    """An extracted game folder → the folder that holds ExeFS and RomFS (the folder itself, its Update, or the dm-work layout under it)"""
    for cand in (folder, os.path.join(folder, 'Update'), os.path.join(folder, 'data', 'switch', VERSION, 'Update')):
        if os.path.isdir(os.path.join(cand, 'ExeFS')) and os.path.isdir(os.path.join(cand, 'RomFS')): return cand
    return None


def step_unpack(c, a):
    why = game_ok(c)
    if why is None: return 'game ' + EXEFS_SHA['main'][:16]
    if os.path.exists(c.game):
        raise Fail(f'the game files in {c.game} are not usable ({why}); move that folder aside and run the install again')
    if a.source:
        src = find_update(os.path.abspath(os.path.expanduser(a.source)))
        if not src: raise Fail(f'{a.source} holds no extracted game files (a folder with ExeFS and RomFS)')
        os.makedirs(os.path.dirname(c.game), exist_ok=True)
        part = c.game + '.part'; shutil.rmtree(part, ignore_errors=True)
        # cp -c clones on the same APFS volume (no data copied, instant); across volumes it is a normal copy
        if subprocess.run(['cp', '-c', '-R', src, part], capture_output=True).returncode:
            shutil.rmtree(part, ignore_errors=True); shutil.copytree(src, part)
        os.replace(part, c.game)
    elif a.game:
        if not a.keys: raise Fail('the game packages need your prod.keys too (--keys)')
        c.run('unpack', ['bash', c.dm('tools', 'extract_game.sh'), *[os.path.abspath(os.path.expanduser(p)) for p in a.game], os.path.abspath(os.path.expanduser(a.keys))])
    else:
        raise Fail('no game files yet: give your game packages and prod.keys (--game … --keys …) or an extracted folder (--from …)')
    why = game_ok(c)
    if why: raise Fail(why)
    return 'game ' + EXEFS_SHA['main'][:16]


def product_dir(c):
    p = open(c.dm('PRODUCT')).readline().strip()
    return os.path.join(c.build, 'cache', 'dm', 'out', f'{p}-touch')


def step_product(c, a):
    want = sha256(c.dm('tools', 'patch_nso.py')); d = product_dir(c)
    try: have = open(os.path.join(d, '.complete')).read().strip()
    except OSError: have = None
    if a.force or have != want: c.run('product', ['bash', c.dm('tools', 'product.sh'), 'build', 'touch'])
    if open(os.path.join(d, '.complete')).read().strip() != want: raise Fail('the rewrite did not complete')
    return os.path.basename(d) + ' ' + want[:16]


def step_packs(c, a):
    d = os.path.join(c.build, 'cache', 'dm', 'dmsh')
    if a.force or len(os.listdir(d) if os.path.isdir(d) else []) != 162:
        os.makedirs(d, exist_ok=True)
        env = c.env(DMX_ROOT=os.path.join(c.game, 'RomFS', 'data'))
        names = subprocess.run([c.python, *PY, c.dm('tools', 'dmx.py'), 'list', 'data:data/hd/shaders/*.bin'], env=env, capture_output=True, text=True, check=False).stdout.split('\n')
        names = [n.split('\t')[0] for n in names if n.strip()]
        for i, n in enumerate(names):
            c.run('packs', [c.python, *PY, c.dm('tools', 'dmx.py'), 'get', n, os.path.join(d, os.path.basename(n))], env=env)
            c.event(event='progress', step='packs', done=i + 1, total=len(names))
    if len(os.listdir(d)) != 162: raise Fail(f'{len(os.listdir(d))} shader packs instead of 162')
    return 'packs 162'


def chain(c):
    v = subprocess.run([c.python, *PY, c.dm('gfx', 'tools', 'dmsh_chain.py')], env=c.env(), capture_output=True, text=True).stdout.strip()
    if not re.fullmatch(r'[0-9a-f]{12}', v): raise Fail('cannot read the shader chain version')
    return v


def msl_root(c): return os.path.join(c.build, 'cache', 'metal_shader_cache')


def shaders_ok(c, v):
    d = os.path.join(msl_root(c), 'b-' + v); res = load_json(os.path.join(d, '_b_build.json'))
    if not res or any(r['st'] != 'ok' for r in res): return False
    have = set(os.listdir(d))
    return all(r['key'] + '_b.metal' in have and r['key'] + '_b.json' in have for r in res)


def step_shaders(c, a):
    v = chain(c)
    if a.force or not shaders_ok(c, v):
        bad = subprocess.run([c.python, *PY, c.dm('gfx', 'tools', 'dmsh_chain.py'), '--check-tools'], env=c.env(), capture_output=True, text=True)
        if bad.returncode: raise Fail('the shader tools do not match this release: ' + bad.stdout.strip().splitlines()[0][:200])
        n = os.cpu_count() or 8
        c.run('shaders', [c.python, *PY, c.dm('gfx', 'tools', 'dmsh_build.py'), os.path.join(c.build, 'cache', 'dm', 'dmsh'), msl_root(c)],
              env=c.env(J=str(max(2, min(8, n // 2))), DMSH_PROGRESS='1'),
              progress=lambda l: (lambda m: (int(m[1]), int(m[2])) if m else None)(re.match(r'DMSH (\d+)/(\d+)', l)))
        if not shaders_ok(c, v): raise Fail('some shaders were not translated (see the log)')
    return 'chain ' + v


def step_c1(c, a):
    v = chain(c); out = os.path.join(c.build, 'c1_full.tsv')
    try: head = open(out).readline()
    except OSError: head = ''
    if a.force or not head.startswith(f'# chain b-{v} '):
        c.run('c1', [c.python, *PY, c.dm('gfx', 'tools', 'c1_guard.py'), os.path.join(msl_root(c), 'b-' + v), out])
    return 'chain ' + v


def overlay_dir(c, stamp): return os.path.join(c.build, 'overlay', stamp)


def overlay_stamp(c): return tree_sha([c.dm('ios', 'tools'), c.dm('i18n'), c.dm('data'), c.dm('PRODUCT')])


def step_overlay(c, a):
    s = overlay_stamp(c); d = overlay_dir(c, s); out = os.path.join(d, 'romfs_overlay')
    if a.force or not os.path.isfile(os.path.join(out, 'overlay.manifest')):
        shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
        c.run('overlay', [c.python, *PY, c.dm('ios', 'tools', 'gen_romfs_overlay.py'), os.path.join(c.game, 'RomFS'), out],
              env=c.env(DM_OVERLAY_MAC='1', DM_DESWIZZLE_LIB=os.path.join(c.runtime, 'lib', 'libnvn_deswizzle.dylib'), DM_GEN_ASSETS=os.path.join(c.build, 'gen_assets')))
        if not os.path.isfile(os.path.join(out, 'overlay.manifest')): raise Fail('the interface overlay was not generated')
    return s


def tables_stamp(c): return tree_sha([c.dm('ios', 'tools', 'gen_hud_exp.py'), c.dm('ios', 'tools', 'romfs_art.py'), c.dm('tools', 'gen_bright_levels.py'), c.dm('ios', 'tools', 'touch_layout.py')])


def step_tables(c, a):
    s = tables_stamp(c); d = os.path.join(c.build, 'tables', s)
    if a.force or not all(os.path.isfile(os.path.join(d, n)) for n in ('hud_exp.tsv', 'bright_levels.tsv')):
        os.makedirs(d, exist_ok=True); rom = os.path.join(c.game, 'RomFS')
        for script, name in ((c.dm('ios', 'tools', 'gen_hud_exp.py'), 'hud_exp.tsv'), (c.dm('tools', 'gen_bright_levels.py'), 'bright_levels.tsv')):
            c.run('tables', [c.python, *PY, script, '--tsv', rom, os.path.join(d, name)])
    return s


def info_plist(c, m):
    d = {'CFBundleName': 'D2R Metal', 'CFBundleDisplayName': 'D2R Metal', 'CFBundleIdentifier': c.bundle_id, 'CFBundleExecutable': 'D2RMobile',
         'CFBundleIconFile': 'AppIcon', 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': m.get('version', '0'), 'CFBundleVersion': str(m.get('build', 1)),
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


def step_app(c, a):
    """Assembles the app in a staging folder, signs it (ad-hoc: it never leaves this Mac and is never quarantined, so Gatekeeper does not look at it)
    and swaps it in, keeping the one it replaces for revert"""
    m = c.manifest; stage = os.path.join(c.build, 'app.new'); app = os.path.join(stage, APP_NAME)
    shutil.rmtree(stage, ignore_errors=True)
    res = os.path.join(app, 'Contents', 'Resources'); os.makedirs(res); os.makedirs(os.path.join(app, 'Contents', 'MacOS'))
    shutil.copy2(os.path.join(c.runtime, 'host', 'D2RMobile'), os.path.join(app, 'Contents', 'MacOS', 'D2RMobile'))
    for name in ('shader_map.tsv', 'pso_manifest.tsv'): shutil.copy2(c.dm('gfx', 'nvn', name), os.path.join(res, name))
    shutil.copy2(os.path.join(c.build, 'c1_full.tsv'), os.path.join(res, 'c1_full.tsv'))
    icon = os.path.join(c.runtime, 'app', 'AppIcon.icns')
    if os.path.isfile(icon): shutil.copy2(icon, os.path.join(res, 'AppIcon.icns'))
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
    install_app(c, app)
    return f"{m.get('version', '?')} {m.get('build', '?')} {c.bundle_id} host {sha256(os.path.join(c.runtime, 'host', 'D2RMobile'))[:12]}"


def managed(app):
    if os.path.islink(app): return False
    try:
        with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: return bool(plistlib.load(f).get('DMLauncherManaged'))
    except (OSError, ValueError, plistlib.InvalidFileException): return False


def install_app(c, app):
    dest = os.path.join(c.app_dir, APP_NAME); prev = os.path.join(c.build, 'previous', APP_NAME)
    if os.path.lexists(dest) and not managed(dest):
        raise Fail(f'{dest} exists and was not installed by the launcher (a link or another build): move it aside, or choose another folder for the app')
    os.makedirs(c.app_dir, exist_ok=True); os.makedirs(os.path.dirname(prev), exist_ok=True)
    if os.path.lexists(dest):
        shutil.rmtree(prev, ignore_errors=True); shutil.move(dest, prev)   # a rename on one volume, a copy across volumes
    shutil.move(app, dest)


def step_warm(c, a):
    # again for every new app (cheap when nothing changed: ~1 s from the cache), every macOS build and every chain
    v = chain(c); key = f"{v} {os_build()} {c.state['steps'].get('app')}"
    if c.state['steps'].get('warm') == key and not a.force: return key
    exe = os.path.join(c.app_dir, APP_NAME, 'Contents', 'MacOS', 'D2RMobile')
    c.run('warm', [exe, '--warm-shaders'], env=c.env(),
          progress=lambda l: (lambda m: (int(m[1]), int(m[2])) if m else None)(re.match(r'WARM (\d+)/(\d+)', l)))
    return key


STEP_FN = {'unpack': step_unpack, 'product': step_product, 'packs': step_packs, 'shaders': step_shaders, 'c1': step_c1,
           'overlay': step_overlay, 'tables': step_tables, 'app': step_app, 'warm': step_warm}


# ---------- commands ----------

def preflight(c, need_game):
    """Problems that stop an install before anything is written: (code, text) pairs"""
    out = []
    if platform.machine() != 'arm64': out.append(('arch', 'D2R Metal needs a Mac with Apple silicon'))
    osv = platform.mac_ver()[0]
    try: major = int(osv.split('.')[0])
    except ValueError: major = 0
    if major < 27: out.append(('macos', f'D2R Metal needs macOS 27 or later (this Mac has {osv or "?"})'))
    want = NEED_GB if need_game else NEED_GB_GAME
    if free_gb(c.root) < want: out.append(('disk', f'{free_gb(c.root):.0f} GB free where the game goes; at least {want} GB is needed'))
    if c.tools:
        for p in (c.python, os.path.join(c.tools, 'dmsh_xlate', 'dmsh_xlate'), os.path.join(c.tools, 'bin', 'spirv-cross'), os.path.join(c.tools, 'bin', 'hactool')):
            if not os.access(p, os.X_OK): out.append(('tools', f'the tools pack lacks {os.path.relpath(p, c.tools)}'))
    if c.runtime and not os.path.isfile(os.path.join(c.runtime, 'host', 'D2RMobile')): out.append(('runtime', 'the runtime pack has no host/D2RMobile'))
    return out


def cmd_install(c, a, force=False):
    a.force = force
    need_game = game_ok(c) is not None
    probs = preflight(c, need_game)
    for code, text in probs: c.event(event='error', code=code, msg=text)
    if probs: return 1
    m = c.manifest
    c.event(event='start', release=m.get('version'), build=m.get('build'), steps=STEPS, typical_s=TYPICAL_S, log=c.log_path)
    before = dict(c.state['steps'])
    for name in STEPS:
        t0 = time.time()
        c.event(event='step', step=name, state='start', typical_s=TYPICAL_S[name])
        try: stamp = STEP_FN[name](c, a)
        except Fail as e:
            c.event(event='error', step=name, msg=str(e)); c.event(event='step', step=name, state='fail', msg=str(e)); return 1
        changed = before.get(name) != stamp
        c.state['steps'][name] = stamp; c.save_state()
        c.event(event='step', step=name, state='done' if changed or name == 'app' else 'kept', seconds=round(time.time() - t0, 1), stamp=stamp)
    c.state['previous_release'] = c.state.get('release'); c.state['release'] = {'version': m.get('version'), 'build': m.get('build'), 'commit': m.get('commit')}
    c.state['app'] = os.path.join(c.app_dir, APP_NAME); c.state['bundle_id'] = c.bundle_id
    c.state['packs'] = {'runtime': c.runtime, 'tools': c.tools}   # what the launcher runs this engine from for revert / status
    c.save_state()
    c.event(event='done', msg=f"D2R Metal {m.get('version', '?')} is installed in {c.app_dir}", app=c.state['app'])
    return 0


def cmd_revert(c, a):
    dest = os.path.join(c.app_dir, APP_NAME); prev = os.path.join(c.build, 'previous', APP_NAME)
    if not os.path.isdir(prev): c.event(event='error', msg='there is no previous version to go back to'); return 1
    if os.path.lexists(dest) and not managed(dest): c.event(event='error', msg=f'{dest} was not installed by the launcher'); return 1
    tmp = os.path.join(c.build, 'revert.tmp'); shutil.rmtree(tmp, ignore_errors=True)
    if os.path.lexists(dest): shutil.move(dest, tmp)
    shutil.move(prev, dest)
    if os.path.isdir(tmp): shutil.move(tmp, prev)
    c.state['release'], c.state['previous_release'] = c.state.get('previous_release'), c.state.get('release')
    c.state['steps'].pop('app', None); c.state['steps'].pop('warm', None); c.save_state()   # the next install rebuilds the app; the warm pass is rechecked
    c.event(event='done', msg='the previous version is back', app=dest)
    return 0


def cmd_status(c, a):
    s = dict(c.state, game=game_ok(c) or 'ok', root=c.root)
    print(json.dumps(s, indent=1, ensure_ascii=False))
    return 0


def cmd_check(c, a):
    probs = preflight(c, game_ok(c) is not None)
    for code, text in probs: c.event(event='error', code=code, msg=text)
    c.event(event='done', msg='ready' if not probs else f'{len(probs)} problem(s)', game=game_ok(c) or 'ok')
    return 1 if probs else 0


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
        if name in ('install', 'repair', 'revert'): p.add_argument('--app-dir')
    a = ap.parse_args(argv)
    c = Ctx(a)
    try:
        if a.cmd == 'install': return cmd_install(c, a)
        if a.cmd == 'repair': return cmd_install(c, a, force=True)
        if a.cmd == 'revert': return cmd_revert(c, a)
        if a.cmd == 'status': return cmd_status(c, a)
        return cmd_check(c, a)
    except KeyboardInterrupt:
        c.event(event='error', msg='interrupted; run the same command again to resume'); return 130


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
