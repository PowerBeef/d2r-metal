#!/usr/bin/env python3
# Pipeline warm-up seed tool (dm/gfx/nvn/pso_manifest.tsv): reads pipeline manifests (the seed, a run's pso_user.tsv) at the byte level of the
# pso_skey rows the graphics layer writes (dm/gfx/nvn/nvn_pso_skey.h), with the same check and normalisation (pso_skey_check, pso_key_norm), records
# the seed over a route matrix, checks runs for pipelines the seed lacks, and measures true cold starts.
#   diff <manifest>... --seed FILE   the distinct normalised S / R / C rows of the manifests that the normalised seed lacks:
#                                    'DIFF S n R n C n', then RESULT {json} with each row's shader keys
#   normalise <in> [--out FILE] [--note TEXT]...   writes the layout line and a comment block, then the rows in their order, each checked and normalised as
#                                    pso_skey_check does; invalid rows and later duplicates are dropped ('DROPPED <tag> <reason> <hex…>'),
#                                    the background marker line is kept; 'KEPT S n R n C n', then RESULT {json}
#   check <manifest|run dir>... [--allow FILE]   rows neither the seed nor the reviewed allow-list (dm/gfx/nvn/pso_allow.tsv) holds, decoded;
#                                    'PSO_CHECK OK|RED rows=<n>'
#   gate [--backend mac-app|cli|ios-mac] [--headless]   runs dm/verify/bench/town_loop.scn (mac-app at this Mac's Recommended in full screen, cli with
#                                    Recommended's features forced, ios-mac as is) and checks its manifest: 'PSO_GATE OK|RED rows=<n>'
#   record [--set front,act1,tour|all] [--only <backend>] [--front create|menu] [--out DIR] [--keep]   runs the 26-row route matrix (matrix())
#                                    under the game lock and harvests each run's manifest to DIR/<NN>_<set>_<name>.tsv; 'RUN <NN> <name> OK|RED|UNAVAIL rows=<n>'
#   merge --dir DIR [--seed FILE] [--write]   the new seed from a record folder: front rows, the seed's rows, Act 1 rows, the background marker, tour rows
#   cold [--backend mac-app|ios-mac] [--route FILE] [--env K=V]... [--user-manifest FILE] [--launcher-warm] [--keep]   one true cold start (mac-app: a
#                                    fresh bundle ID; ios-mac: the package's Metal cache moved to the Trash first) with DM_BOOTPROF; appends a row to
#                                    $DM_ROOT/run/pso/cold.tsv: 'PSO_COLD OK|RED'
# Every command that runs the game takes the Mac game lock itself. Run folders and test copies go to the Trash ('dm-work pso <date>'), never rm.
# Every row form (S<TAB>shader key<TAB>stage, R<TAB>pso_skey hex, C<TAB>compute shader key) and the byte offsets below follow nvn_pso_skey.h;
# norm_row and pso_key_norm must change together (t_pso_skey and test_pso_seed.py check both against one hash).
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
DM = os.path.normpath(os.path.join(HERE, '..'))
SEED = os.path.join(DM, 'gfx', 'nvn', 'pso_manifest.tsv')

SKEY_LEN = 432   # sizeof(pso_skey)
HEAD = '# pso_skey v2 432'         # PSO_MANIFEST_HEAD
BG_MARK = '# warm: background'     # PSO_WARM_BG_MARK
# pso_skey byte offsets (little-endian): vs 0:24, fs 24:48, vs_stage 48, fs_stage 49, merged 50, fz 51, fz_desc kind 52, fbmask 53, fx 54:86,
# padding 86:88, then pso_key at 88: vs pointer 88:96, fs pointer 96:104, cfmt 104:136 (8 x u32), blend_en 148, chmask 152, blend 160:224 (8 x 8 bytes),
# lmap 416:424, fsfn pointer 424:432
O_MERGED, O_CFMT, O_BLEND_EN, O_CHMASK, O_BLEND, O_LMAP = 50, 104, 148, 152, 160, 416


def norm_row(b: bytes) -> bytes:
    """The rule of pso_key_norm on a 432-byte row: the write mask and blend state of an output with no attachment (normal pass) or with no
    physical attachment mapped to it (merged pass), and the blend factors of an output whose blending is off, are cleared; a normal pass gets
    an all-zero lmap."""
    b = bytearray(b)
    merged = b[O_MERGED]
    cfmt = struct.unpack_from('<8I', b, O_CFMT)
    en_in, ch_in = struct.unpack_from('<II', b, O_BLEND_EN)
    en = ch = 0
    bl = [bytes(8)] * 8
    for i in range(8):
        if not cfmt[i]: continue
        lo = b[O_LMAP + i] if merged else i
        if lo > 7: continue
        ch |= ch_in & (0xf << (4 * lo))
        if en_in >> lo & 1:
            en |= 1 << lo
            bl[lo] = bytes(b[O_BLEND + 8 * lo:O_BLEND + 8 * lo + 8])
    struct.pack_into('<II', b, O_BLEND_EN, en, ch)
    b[O_BLEND:O_BLEND + 64] = b''.join(bl)
    if not merged: b[O_LMAP:O_LMAP + 8] = bytes(8)
    return bytes(b)


def check_row(h):
    """pso_skey_check on a row's hex: (reason, hex) for a row the warm-up refuses, reason one of parse, pad, ptr, flags, fz, kind, key, lmap
    (the first failed check, in the C order); (None, canonical hex) otherwise: shader keys zero after their NUL, no fz_desc on an unspecialised
    row, the key normalised."""
    if len(h) != SKEY_LEN * 2: return 'parse', h
    try: b = bytearray(bytes.fromhex(h))
    except ValueError: return 'parse', h
    if any(b[86:88]): return 'pad', h
    if any(b[88:104]) or any(b[424:432]): return 'ptr', h
    if b[50] > 1 or b[51] > 1: return 'flags', h
    if b[51] and not b[50]: return 'fz', h
    if b[51] and b[52] != 1: return 'kind', h
    if not b[0] or 0 not in b[0:24] or 0 not in b[24:48]: return 'key', h
    if b[50] and any(x > 7 and x != 0xff for x in b[O_LMAP:O_LMAP + 8]): return 'lmap', h
    for s0 in (0, 24):
        z = b.index(0, s0, s0 + 24); b[z:s0 + 24] = bytes(s0 + 24 - z)
    if not b[51]: b[52:86] = bytes(34)
    return None, norm_row(bytes(b)).hex()


def parse_line(line):
    """(tag, value, stage, reason) of a manifest row: value is the canonical hex for R; reason is None for a valid row."""
    c = line.split('\t')
    if c[0] == 'S':
        return ('S', c[1] if len(c) > 1 else '', c[2] if len(c) > 2 else None, None if len(c) >= 3 and c[1] else 'fields' if len(c) < 3 else 'empty')
    if c[0] == 'C':
        return ('C', c[1] if len(c) > 1 else '', None, None if len(c) > 1 and c[1] else 'empty')
    if c[0] == 'R':
        why, h = check_row(c[1] if len(c) > 1 else '')
        return ('R', h, None, why)
    return (c[0], '', None, 'tag')


def row_id(tag, val):
    return f'{tag}\t{val}'


def normalise(lines):
    """(output lines, dropped [(tag, reason, value)], kept counts) for a manifest's lines."""
    out, dropped, seen, marked = [], [], set(), False
    kept = {'S': 0, 'R': 0, 'C': 0}
    for line in lines:
        line = line.rstrip('\r\n')
        if line == BG_MARK and not marked: out.append(line); marked = True; continue
        if not line or line.startswith('#'): continue
        tag, val, stage, why = parse_line(line)
        if why: dropped.append((tag, why, val)); continue
        k = row_id(tag, val)
        if k in seen: dropped.append((tag, 'duplicate', val)); continue
        seen.add(k); kept[tag] += 1
        out.append(f'S\t{val}\t{stage}' if tag == 'S' else f'{tag}\t{val}')
    return out, dropped, kept


NORM_NOTE = ['# Pipeline warm-up manifest. Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex, dm/gfx/nvn/nvn_pso_skey.h) / C<TAB>compute shader key.',
             '# Rows are checked and normalised (dm/tools/pso_seed.py normalise; t_pso_skey lints the shipped seed). The row order is the build order: do not sort.',
             f"# In the seed, rows after a '{BG_MARK}' line warm in the background: the first loading screen waits only for the rows before it."]


def cstr(b: bytes) -> str:
    return b.split(b'\0', 1)[0].decode('ascii', 'replace')


def row_keys(tag, val):
    """(vs, fs) shader keys of a row ('' when absent)."""
    if tag == 'R':
        b = bytes.fromhex(val)
        return cstr(b[0:24]), cstr(b[24:48])
    return val, ''


def read_rows(path):
    """[(tag, value, stage)] of a manifest's valid rows, R values as canonical hex; comments, blank lines and invalid rows are left out."""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'): continue
            tag, val, stage, why = parse_line(line)
            if not why: out.append((tag, val, stage))
    return out


def diff(manifests, seed):
    have = {row_id(t, v) for t, v, _ in read_rows(seed)}
    seen, rows = set(), []
    for m in manifests:
        for t, v, _ in read_rows(m):
            k = row_id(t, v)
            if k in have or k in seen: continue
            seen.add(k)
            vs, fs = row_keys(t, v)
            rows.append(dict(tag=t, vs=vs, fs=fs))
    return rows


def count(rows):
    n = {'S': 0, 'R': 0, 'C': 0}
    for r in rows: n[r['tag']] += 1
    return n


# ---------- Runs: the route matrix, checking, gating, cold starts ----------
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
ALLOW = os.path.join(DM, 'gfx', 'nvn', 'pso_allow.tsv')
TOWN = os.path.join(DM, 'verify', 'bench', 'town_loop.scn')
COMBAT = os.path.join(DM, 'verify', 'bench', 'combat.scn')
HARVEST = 'pso_user.harvest.tsv'   # a run's manifest copied out of the iOS package's container
REC_ENV = {'DM_TEST_VIDEO_FIRSTBOOT': '1'}   # Mac App rows: this Mac's Recommended picture, full screen
CLI_REC = {'DM_AA_MODE': '2', 'DM_ENV_SHADOWS': '1', 'DM_3D_SCALE': '0.75', 'DM_MFX3D': '1'}   # Recommended's features forced on the CLI host, which has no Recommended
# CLI forces: the player-reachable settings the Mac's Recommended does not use (anti-aliasing is FXAA or SMAA for players; AA off is not reachable)
FORCES = [('orig', {}), ('smaa_shadows', {'DM_AA_MODE': '2', 'DM_ENV_SHADOWS': '1'}), ('rec_mfx3d', dict(CLI_REC)), ('s75_bilinear', {'DM_3D_SCALE': '0.75', 'DM_MFX3D': '0'})]
# Waypoint stops of the local all-waypoint save save_ref_tour: (name, ZR presses to the act's tab, the stop's row on the waypoint list)
TOUR = [('a1_darkwood', 0, 0.446), ('a1_catacombs', 0, 0.822), ('a2_town', 1, 0.222), ('a2_faroasis', 1, 0.521), ('a2_arcane', 1, 0.747), ('a3_docks', 2, 0.222),
        ('a3_flayer', 2, 0.446), ('a4_town', 3, 0.222), ('a4_riverflame', 3, 0.371), ('a5_town', 4, 0.222), ('a5_frigid', 4, 0.296), ('a5_arreat', 4, 0.371)]
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
COLD_ID = 'org.d2rmobile.mac.cold'   # cold runs' Mac App bundle IDs: COLD_ID + timestamp, removed afterwards


def matrix():
    """The 26 recording rows: front end (Mac App, iOS package), Act 1 (Mac App and iOS package town / combat, CLI town / combat under each force),
    then the tour stops (Mac App). Mac App rows run at this Mac's Recommended in full screen."""
    rows = [dict(nn='01', set='front', name='mac_front', backend='mac-app', route='front'), dict(nn='02', set='front', name='ios_front', backend='ios-mac', route='front'),
            dict(nn='03', set='act1', name='mac_town', backend='mac-app', route=TOWN), dict(nn='04', set='act1', name='mac_combat', backend='mac-app', route=COMBAT),
            dict(nn='05', set='act1', name='ios_town', backend='ios-mac', route=TOWN), dict(nn='06', set='act1', name='ios_combat', backend='ios-mac', route=COMBAT)]
    n = 7
    for tag, env in FORCES:
        for rname, route in (('town', TOWN), ('combat', COMBAT)):
            rows.append(dict(nn=f'{n:02d}', set='act1', name=f'cli_{rname}_{tag}', backend='cli', route=route, env=dict(env))); n += 1
    for stop, z, y in TOUR:
        rows.append(dict(nn=f'{n:02d}', set='tour', name=f'tour_{stop}', backend='mac-app', route='tour', stop=(stop, z, y))); n += 1
    for r in rows:
        r['env'] = {**(REC_ENV if r['backend'] == 'mac-app' else {}), **r.get('env', {})}
    return rows


def env_line(env):
    return 'env ' + ' '.join(f'{k}={v}' for k, v in env.items()) if env else ''


def front_text(variant, backend, env):
    """The front-end route: 'create' goes from the main menu to the new-character page (the campfire scene), 'menu' stays on the main menu."""
    lines = ['# generated by dm/tools/pso_seed.py: the front end' + (' up to the new-character page' if variant == 'create' else ''), 'name pso_front']
    if backend == 'mac-app': lines.append('requires mac-app')
    lines += ['save warlock', 'timeout 240', env_line(env), '', 'expect main_menu timeout=120']
    if variant == 'create': lines += ['sleep 4', 'press PLUS', 'sleep 1', 'press DOWN', 'press A', 'expect_state panels~=CharacterCreatePanel timeout=20', 'sleep 6']
    else: lines += ['sleep 6']
    return '\n'.join(lines + ['quit', 'check no_red']) + '\n'


def tour_text(stop, z, y, env):
    """One waypoint stop of the tour save: take the waypoint, step off it both ways."""
    lines = [f'# generated by dm/tools/pso_seed.py: waypoint stop {stop}', f'name pso_tour_{stop}', 'requires mac-app', 'save tour',
             env_line({'DM_GAME_SEED': '0x12345678', 'DM_FIXED_TIME': '1790000000', **env}), 'timeout 200', '', 'enter_game', 'sleep 4', 'tap_unit type=2 cls=any', 'sleep 1.5']
    for _ in range(z): lines += ['press keys=ZR', 'sleep 0.5']
    lines += [f'tap 0.5 {y}', 'sleep 6', 'stick x=0.7 y=-0.7 frames=40', 'sleep 2', 'stick x=-0.7 y=0.7 frames=80', 'sleep 2', 'quit', 'check no_red']
    return '\n'.join(lines) + '\n'


def route_text(route, env):
    """A copy of a route file with extra env lines (an env header line counts wherever it is; every backend passes it to the game)."""
    with open(route, encoding='utf-8') as f: text = f.read()
    return text.rstrip('\n') + ('\n# added by dm/tools/pso_seed.py\n' + env_line(env) + '\n' if env else '\n')


def row_text(row, front):
    if row['route'] == 'front': return front_text('create' if row['backend'] == 'mac-app' else front, row['backend'], row['env'])
    if row['route'] == 'tour': return tour_text(*row['stop'], row['env'])
    return route_text(row['route'], row['env'])


def local_env(key):
    """A value from the environment or dm/ios/local.env (the iOS package's bundle ID)."""
    if os.environ.get(key): return os.environ[key]
    try:
        with open(os.path.join(DM, 'ios', 'local.env'), encoding='utf-8') as f:
            for line in f:
                k, _, v = line.strip().partition('=')
                if k.replace('export ', '').strip() == key: return v.strip().strip('"\'')
    except OSError: pass
    return ''


def ios_container(bundle=None):
    """The iOS package's container Data folder on this Mac (containermanagerd's metadata names the bundle), or None."""
    import glob, plistlib   # noqa: E401, PLC0415
    b = bundle or local_env('DM_BUNDLE_ID')
    for m in glob.glob(os.path.expanduser('~/Library/Containers/*/.com.apple.containermanagerd.metadata.plist')):
        try:
            with open(m, 'rb') as f: ident = plistlib.load(f).get('MCMMetadataIdentifier')
        except (OSError, ValueError): continue
        if b and ident == b: return os.path.join(os.path.dirname(m), 'Data')
    return None


def run_manifest(backend, run_dir):
    """Where a run's own pipeline manifest is: beside the save copy (cli), in the test copy's support folder (mac-app), in the container's
    Documents/settings.test (ios-mac, PSO-7)."""
    if backend == 'cli': return os.path.join(run_dir, 'pso_user.tsv')
    if backend == 'mac-app': return os.path.join(run_dir, 'support', 'pso_user.tsv')
    c = ios_container()
    return os.path.join(c, 'Documents', 'settings.test', 'pso_user.tsv') if c else None


PSO7_LINE = 'boot: pipeline manifest in Documents/settings.test'   # main.m logs it at boot in a test run (PSO-7)


def ios_pso7(run_dir):
    """Whether an ios-mac run's package keeps the test run's pipeline manifest in Documents/settings.test: its port.log has PSO7_LINE. A package
    built before that writes a test run's rows to the player's Documents/pso_user.tsv, so a missing settings.test manifest proves nothing there."""
    try:
        with open(os.path.join(run_dir, 'port.log'), encoding='utf-8', errors='replace') as f: return PSO7_LINE in f.read()
    except OSError: return False


def harvest(backend, run_dir, dest):
    """Copies a run's manifest to dest; a run that built nothing synchronously wrote none, which is an empty manifest. Returns dest, or None (and
    writes nothing) for an ios-mac run without a manifest whose package does not log PSO7_LINE: its rows, if any, went elsewhere."""
    import shutil   # noqa: PLC0415
    src = run_manifest(backend, run_dir)
    if src and os.path.isfile(src): shutil.copyfile(src, dest)
    elif backend == 'ios-mac' and not ios_pso7(run_dir): return None
    else:
        with open(dest, 'w', encoding='utf-8') as f: f.write(HEAD + '\n')
    return dest


NO_PSO7 = ("the iOS package's run left no Documents/settings.test/pso_user.tsv and its port.log lacks '" + PSO7_LINE +
           "': the package predates the test runs' own manifest, rebuild it (dm/ios/tools/smoke_mac.sh 1)")


def find_manifest(p):
    """A manifest file, or a run folder's manifest (pso_user.tsv, support/pso_user.tsv, or the harvested copy). (path or None, whether p exists)."""
    if os.path.isfile(p): return p, True
    for c in ('pso_user.tsv', os.path.join('support', 'pso_user.tsv'), HARVEST):
        q = os.path.join(p, c)
        if os.path.isfile(q): return q, True
    return None, os.path.isdir(p)


def parse_manifest(path):
    """(valid rows [(tag, value, stage)], invalid rows [(tag, reason, value)]) of a manifest."""
    rows, bad = [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'): continue
            tag, val, stage, why = parse_line(line)
            if why: bad.append((tag, why, val))
            else: rows.append((tag, val, stage))
    return rows, bad


def decode(tag, val):
    if tag != 'R': return f'{tag} {val}'
    b = bytes.fromhex(val); cf = [c for c in struct.unpack_from('<8I', b, O_CFMT) if c]; dfmt, sfmt = struct.unpack_from('<II', b, 136)
    return f"R vs {cstr(b[0:24])} fs {cstr(b[24:48]) or '-'} cfmt {cf} dfmt {dfmt} sfmt {sfmt} merged {b[50]} fz {b[51]}"


def check(paths, seed=SEED, allow=ALLOW):
    """Rows of the manifests (or run folders) that neither the seed nor the allow-list holds, compared after normalisation. A run folder without a
    manifest built nothing synchronously (0 rows); a path that does not exist is an error. Returns (rows [(tag, value)], invalid rows, errors)."""
    have = {row_id(t, v) for t, v, _ in read_rows(seed)}
    if allow and os.path.exists(allow): have |= {row_id(t, v) for t, v, _ in read_rows(allow)}
    rows, seen, bad, errors = [], set(), [], []
    for p in paths:
        m, exists = find_manifest(p)
        if not m:
            if not exists: errors.append(f'{p}: no such manifest or run folder')
            continue
        good, wrong = parse_manifest(m)
        bad += wrong
        for t, v, _ in good:
            k = row_id(t, v)
            if k in have or k in seen: continue
            seen.add(k); rows.append((t, v))
    return rows, bad, errors


def merge(seed_lines, sections, notes):
    """The new seed: the layout line and notes, the front rows, the seed's own rows (its foreground part), the Act 1 rows, the background marker,
    the seed's background rows, the tour rows. The seed's rows must be canonical (else ValueError); new rows already in the seed or earlier are
    dropped. sections = {'front': [lines], 'act1': [...], 'tour': [...]}, each in file order. Returns (lines, counts)."""
    fg, bg, in_bg = [], [], False
    for line in seed_lines:
        line = line.rstrip('\r\n')
        if line == BG_MARK: in_bg = True; continue
        if not line or line.startswith('#'): continue
        tag, val, stage, why = parse_line(line)
        canon = f'S\t{val}\t{stage}' if tag == 'S' else f'{tag}\t{val}'
        if why or canon != line: raise ValueError(f'seed row is not canonical ({why or "not normalised"}): {line[:60]} (run pso_seed.py normalise first)')
        (bg if in_bg else fg).append(line)
    seen = {row_id(*parse_line(x)[:2]) for x in fg + bg}

    def fresh(lines):
        out = []
        for line in lines:
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'): continue
            tag, val, stage, why = parse_line(line)
            if why or row_id(tag, val) in seen: continue
            seen.add(row_id(tag, val)); out.append(f'S\t{val}\t{stage}' if tag == 'S' else f'{tag}\t{val}')
        return out
    front, act1, tour = fresh(sections.get('front', [])), fresh(sections.get('act1', [])), fresh(sections.get('tour', []))
    lines = [HEAD] + NORM_NOTE + ['# ' + x for x in notes] + front + fg + act1 + [BG_MARK] + bg + tour
    return lines, dict(front=len(front), seed=len(fg), act1=len(act1), seed_background=len(bg), tour=len(tour))


def trash(path):
    """Moves path to ~/.Trash/'dm-work pso <date>' (never deletes)."""
    import datetime, shutil   # noqa: E401, PLC0415
    if not os.path.lexists(path): return None
    d = os.path.expanduser(f'~/.Trash/dm-work pso {datetime.date.today():%Y-%m-%d}'); os.makedirs(d, exist_ok=True)
    base = os.path.join(d, os.path.basename(path.rstrip('/'))); dst, i = base, 1
    while os.path.lexists(dst): dst = f'{base} {i}'; i += 1
    shutil.move(path, dst)
    return dst


def du(path):
    """Bytes under path (0 when absent)."""
    if not os.path.exists(path): return 0
    if os.path.isfile(path): return os.path.getsize(path)
    n = 0
    for root, _, files in os.walk(path):
        for f in files:
            try: n += os.lstat(os.path.join(root, f)).st_size
            except OSError: pass
    return n


def cache_dir():
    import subprocess   # noqa: PLC0415
    return subprocess.run(['getconf', 'DARWIN_USER_CACHE_DIR'], capture_output=True, text=True, check=True).stdout.strip()


def mac_cleanup_plan(bid, copy_dir, run_dirs, caches, home):
    """What a Mac App cold run leaves behind and how it goes: commands (unregister the copy, delete its preference domain) and paths moved to
    the Trash (the copy, its Metal cache, the run's support folders, then the Library folders macOS made for the bundle ID, when present). Refuses
    any bundle ID but a cold run's, so the installed app and the standard test copy are never touched."""
    if not bid.startswith(COLD_ID) or len(bid) <= len(COLD_ID): raise ValueError(f'not a cold-run bundle ID: {bid}')
    app = os.path.join(copy_dir, 'D2R Metal.app')
    lib = os.path.join(home, 'Library')
    return dict(cmds=[[LSREGISTER, '-u', app], ['defaults', 'delete', bid]],
                moves=[copy_dir, os.path.join(caches, bid)] + list(run_dirs),
                optional=[os.path.join(lib, 'Saved Application State', f'{bid}.savedState'), os.path.join(lib, 'Caches', bid), os.path.join(lib, 'HTTPStorages', bid)])


def ios_cache_plan(bid, caches):
    """The iOS package's Metal cache folders on this Mac (outside its container): moved to the Trash before a cold run."""
    root = os.path.join(caches, bid, bid)
    return [os.path.join(root, x) for x in ('com.apple.metal', 'com.apple.metalfe', 'com.apple.gpuarchiver')]


# Log lines the cold summary reads (the loading-screen line is Chinese today; the English form is accepted for when it is translated)
RE_FG = re.compile(r'pipeline warm-up: foreground rows done in ([\d.]+) ms')
RE_ALL = re.compile(r'pipeline warm-up done: .*?; ([\d.]+) ms; workers')
RE_LOAD = re.compile(r'(?:进局读取画面等管线预热|loading screen waited for the pipeline warm-up) (\d+) ms')
RE_BP_PSO = re.compile(r'管线创建 (\d+) 个'); RE_BP_LIB = re.compile(r'metallib 载入 (\d+) 个')
RE_BP_PP = re.compile(r'丢出 (\d+) / 领用 (\d+)')
RE_BP_HOST = re.compile(r'host builds lib (\d+) / pso (\d+) / ([\d.]+) ms')
RE_HOST_BUILT = re.compile(r'host (.+?) (pipeline|library) built in ([\d.]+) ms')


def cold_summary(log_text, nvn=None, marks=None):
    """Numbers of one cold run. log_text: host.log (or the iOS package's port.log); nvn: {frame: {series: value}} of DM_NVN_TSV; marks: [(name,
    frame)]. Game pipelines only in sync_*: the front end is the frames before the 'town' mark (on ios-mac, without nvn.tsv, the last [bootprof]
    line before game_enter), the game the frames from 'town' to 'end'."""
    import json as _json   # noqa: PLC0415
    out = dict(warm_fg_ms=None, warm_all_ms=None, load_wait_ms=None, sync_pso_front=None, sync_lib_front=None, sync_pso_game=None, game_p99_ms=None,
               slow_with_creations=[], kicked=None, taken=None, host_lib=None, host_pso=None, host_ms=None, host_built=[])
    last_bp, front_bp = None, None
    for line in log_text.splitlines():
        m = RE_FG.search(line)
        if m: out['warm_fg_ms'] = float(m.group(1))
        m = RE_ALL.search(line)
        if m: out['warm_all_ms'] = float(m.group(1))
        m = RE_LOAD.search(line)
        if m: out['load_wait_ms'] = int(m.group(1))
        m = RE_HOST_BUILT.search(line)
        if m: out['host_built'].append(dict(what=m.group(1), kind=m.group(2), ms=float(m.group(3))))
        if '[bootprof]' in line:
            p, lb = RE_BP_PSO.search(line), RE_BP_LIB.search(line)
            last_bp = (int(p.group(1)) if p else None, int(lb.group(1)) if lb else None)
            m = RE_BP_PP.search(line)
            if m: out['kicked'], out['taken'] = int(m.group(1)), int(m.group(2))
            m = RE_BP_HOST.search(line)
            if m: out['host_lib'], out['host_pso'], out['host_ms'] = int(m.group(1)), int(m.group(2)), float(m.group(3))
        elif 'EV {' in line and front_bp is None:
            try: ev = _json.loads(line[line.index('EV {') + 3:])
            except ValueError: continue
            if ev.get('ev') == 'game_enter': front_bp = last_bp or (0, 0)
    marks = dict(marks or [])
    if nvn and 'town' in marks:
        t0, t1 = marks['town'], marks.get('end', max(nvn) + 1)
        front = [nvn[f] for f in sorted(nvn) if f < t0]; game = [nvn[f] for f in sorted(nvn) if t0 <= f < t1]
        out['sync_pso_front'] = int(sum(r.get('pso_new', 0) for r in front)); out['sync_lib_front'] = int(sum(r.get('lib_new', 0) for r in front))
        out['sync_pso_game'] = int(sum(r.get('pso_new', 0) for r in game))
        ft = sorted(r['frame_ms'] for r in game if 'frame_ms' in r)
        if ft: out['game_p99_ms'] = round(ft[min(len(ft) - 1, int(round(0.99 * (len(ft) - 1))))], 3)
        out['slow_with_creations'] = [round(r['frame_ms'], 1) for f, r in sorted(nvn.items()) if r.get('frame_ms', 0) > 33 and r.get('pso_new', 0) + r.get('lib_new', 0) > 0]
    elif front_bp:
        out['sync_pso_front'], out['sync_lib_front'] = front_bp
        if last_bp and last_bp[0] is not None and front_bp[0] is not None: out['sync_pso_game'] = last_bp[0] - front_bp[0]
    return out


def nvn_series(path):
    """{frame: {series: value}} of a DM_NVN_TSV file (its @ rows)."""
    out = {}
    if not os.path.exists(path): return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) == 3 and p[1].startswith('@'):
                try: out.setdefault(int(p[0]), {})[p[1][1:]] = float(p[2])
                except ValueError: pass
    return out


def run_marks(path):
    import json as _json   # noqa: PLC0415
    out = []
    if not os.path.exists(path): return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try: e = _json.loads(line)
            except ValueError: continue
            if e.get('ev') == 'mark' and e.get('frame') is not None: out.append((e.get('name'), e['frame']))
    return out


def stamp_now():
    import datetime   # noqa: PLC0415
    return datetime.datetime.now().strftime('%Y%m%d%H%M%S')


def git_short():
    import subprocess   # noqa: PLC0415
    return subprocess.run(['git', '-C', DM, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, check=False).stdout.strip() or '?'


def run_scn(text, backend, run_dir, headless=False):
    """Writes a route into run_dir and runs it with the scenario runner. Returns (result, exit code)."""
    import scenario   # noqa: PLC0415
    os.makedirs(run_dir, exist_ok=True)
    path = os.path.join(run_dir, 'route.scn')
    with open(path, 'w', encoding='utf-8') as f: f.write(text)
    try:
        scenario.ensure_save(scenario.parse(path)[0]['save'], headless)
        return scenario.Runner(path, backend, run_dir, headless).go()
    except scenario.ScenarioError as e:
        return dict(verdict='RED', reason=str(e)), 1


def mac_app_build(dest, bundle_id):
    """A Mac App test copy of this tree under dest (dm/tools/mac_app.sh); returns the .app path."""
    import subprocess   # noqa: PLC0415
    p = subprocess.run(['bash', os.path.join(DM, 'tools', 'mac_app.sh'), dest], capture_output=True, text=True, check=False, env=dict(os.environ, DM_MAC_BUNDLE_ID=bundle_id))
    app = os.path.join(dest, 'D2R Metal.app')
    if p.returncode or not os.path.isdir(app): raise RuntimeError('mac_app.sh failed: ' + (p.stdout + p.stderr).strip()[-300:])
    return app


def app_exe(app):
    import plistlib   # noqa: PLC0415
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: return os.path.join(app, 'Contents', 'MacOS', plistlib.load(f).get('CFBundleExecutable', 'D2RMobile'))


def same_file(a, b):
    try:
        with open(a, 'rb') as f, open(b, 'rb') as g: return f.read() == g.read()
    except OSError: return False


def iosdd_app():
    import subprocess   # noqa: PLC0415
    tree = subprocess.run(['git', '-C', DM, 'rev-parse', '--show-toplevel'], capture_output=True, text=True, check=False).stdout.strip()
    return os.environ.get('SCN_MAC_APP') or os.path.join(tree, '.iosdd_mac', 'DmMacSmoke.app')


def cmd_record(a, argv):
    import json as _json, shutil, scenario   # noqa: E401, PLC0415
    held = scenario.game_lock(__file__, argv, 'pso_seed record ~50 min', True)
    if held is not None: return held
    sets = {'front', 'act1', 'tour'} if a.set == 'all' else set(a.set.split(','))
    rows = [r for r in matrix() if r['set'] in sets and (not a.only or r['backend'] == a.only)]
    out = a.out or os.path.join(DM_ROOT, 'run', 'pso', stamp_now()); os.makedirs(os.path.join(out, 'runs'), exist_ok=True)
    shutil.copyfile(SEED, os.path.join(out, 'seed_before.tsv'))
    built, results = None, []
    try:
        if any(r['backend'] == 'mac-app' for r in rows):
            if not os.environ.get('DM_MAC_APP'):
                built = os.path.join(DM_ROOT, 'out', 'macapp-pso')
                os.environ['DM_MAC_APP'] = mac_app_build(built, 'org.d2rmobile.mac.test')
            if not same_file(os.path.join(os.environ['DM_MAC_APP'], 'Contents', 'Resources', 'pso_manifest.tsv'), SEED):
                print(f"🔴 the Mac App test copy's seed is not this tree's: rebuild it (DM_MAC_BUNDLE_ID=org.d2rmobile.mac.test dm/tools/mac_app.sh …)"); return 2
        if any(r['backend'] == 'ios-mac' for r in rows) and not same_file(os.path.join(iosdd_app(), 'Wrapper', 'DmIOS.app', 'dm', 'pso_manifest.tsv'), SEED):
            print("🔴 the iOS package for the Mac carries another seed: rebuild it (dm/ios/tools/smoke_mac.sh 1)"); return 2
        with open(os.path.join(out, 'record.tsv'), 'a', encoding='utf-8') as led:
            for r in rows:
                name = f"{r['nn']}_{r['set']}_{r['name']}"; run = os.path.join(out, 'runs', f"{r['nn']}_{r['name']}")
                if r['route'] == 'tour' and not os.path.isdir(scenario.save_dir('tour')):
                    verdict, n = 'UNAVAIL', 0
                else:
                    res, code = run_scn(row_text(r, a.front), r['backend'], run)
                    dest = harvest(r['backend'], run, os.path.join(out, name + '.tsv'))
                    if dest is None:
                        if code == 0: print(f'🔴 {NO_PSO7}')
                        verdict, n = 'RED', 0
                    else:
                        new, _, _ = check([dest])
                        verdict, n = ('OK' if code == 0 else 'RED'), len(new)
                    if verdict == 'OK' and not a.keep: trash(run)
                print(f"RUN {r['nn']} {r['name']} {verdict} rows={n}", flush=True)
                led.write(f"{r['nn']}\t{r['name']}\t{r['backend']}\t{verdict}\t{n}\t{env_line(r['env'])}\n"); led.flush()
                results.append(dict(nn=r['nn'], set=r['set'], name=r['name'], backend=r['backend'], verdict=verdict, rows=n))
    finally:
        if built: trash(built)
    meta = dict(name='pso_seed record', dir=out, rev=git_short(), front=a.front, runs=results)
    with open(os.path.join(out, 'record.json'), 'w', encoding='utf-8') as f: _json.dump(meta, f, indent=1)
    ok = all(x['verdict'] == 'OK' or (x['verdict'] == 'UNAVAIL' and x['set'] == 'tour') for x in results)
    print('RESULT ' + _json.dumps(dict(meta, verdict='OK' if ok else 'RED')))
    return 0 if ok else 1


def cmd_merge(a):
    import glob, json as _json   # noqa: E401, PLC0415
    sections = {'front': [], 'act1': [], 'tour': []}
    for p in sorted(glob.glob(os.path.join(a.dir, '[0-9][0-9]_*.tsv'))):
        part = os.path.basename(p).split('_', 2)
        if len(part) == 3 and part[1] in sections:
            with open(p, encoding='utf-8', errors='replace') as f: sections[part[1]] += f.readlines()
    meta = {}
    try:
        with open(os.path.join(a.dir, 'record.json'), encoding='utf-8') as f: meta = _json.load(f)
    except (OSError, ValueError): pass
    import datetime   # noqa: PLC0415
    notes = [f"Re-recorded {datetime.date.today():%Y-%m-%d} at {meta.get('rev', git_short())} by pso_seed.py record ({len(meta.get('runs', []))} routes:"
             f" {', '.join(x['name'] for x in meta.get('runs', [])) or 'unknown'}; iOS front variant {meta.get('front', '?')}); Mac App rows at this Mac's Recommended, full screen."]
    with open(a.seed, encoding='utf-8', errors='replace') as f: seed_lines = f.readlines()
    try: lines, counts = merge(seed_lines, sections, notes)
    except ValueError as e: print(f'🔴 {e}'); return 2
    for k, v in counts.items(): print(f'MERGE {k} {v}')
    if a.write:
        tmp = a.seed + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f: f.write('\n'.join(lines) + '\n')
        os.replace(tmp, a.seed)
    print('RESULT ' + _json.dumps(dict(name='pso_seed merge', counts=counts, written=bool(a.write))))
    return 0


def print_check(rows, bad, errors):
    for t, v in rows: print('ROW ' + decode(t, v))
    for t, why, v in bad: print(f'BAD {t} {why} {v[:40]}')
    for e in errors: print(f'🔴 {e}')


def cmd_check(a):
    import json as _json   # noqa: PLC0415
    rows, bad, errors = check(a.paths, a.seed, a.allow)
    print_check(rows, bad, errors)
    ok = not rows and not bad and not errors
    print(f"PSO_CHECK {'OK' if ok else 'RED'} rows={len(rows)}")
    print('RESULT ' + _json.dumps(dict(name='pso_seed check', verdict='OK' if ok else 'RED', rows=[decode(t, v) for t, v in rows], bad=len(bad), errors=errors)))
    return 0 if ok else 1


def cmd_gate(a, argv):
    import json as _json, scenario   # noqa: E401, PLC0415
    windowed = a.backend == 'mac-app' or (a.backend == 'cli' and not a.headless)
    held = scenario.game_lock(__file__, argv, f'pso_seed gate {a.backend} ~5 min', windowed)
    if held is not None: return held
    out = a.out or os.path.join(DM_ROOT, 'run', 'pso', f'gate_{a.backend}_{stamp_now()}')
    env = {'mac-app': REC_ENV, 'cli': CLI_REC, 'ios-mac': {}}[a.backend]
    res, code = run_scn(route_text(a.route, env), a.backend, out, a.headless)
    rows, bad, errors = [], [], []
    if a.backend == 'ios-mac' and harvest('ios-mac', out, os.path.join(out, HARVEST)) is None: errors = [NO_PSO7]   # nothing was measured: never OK
    else: rows, bad, errors = check([out], a.seed, a.allow)
    print_check(rows, bad, errors)
    ok = code == 0 and not rows and not bad and not errors
    print(f"PSO_GATE {'OK' if ok else 'RED'} rows={len(rows)}" + ('' if code == 0 else f" (scenario RED: {res.get('reason')})"))
    print('RESULT ' + _json.dumps(dict(name='pso_seed gate', backend=a.backend, verdict='OK' if ok else 'RED', rows=[decode(t, v) for t, v in rows], dir=out)))
    return 0 if ok else 1


def parse_env(items):
    out = {}
    for kv in items or []:
        if '=' in kv: k, v = kv.split('=', 1); out[k] = v
    return out


def cold_mac(a, out, env, stamp, plan):
    import shutil, subprocess, time, bench   # noqa: E401, PLC0415
    bid = COLD_ID + stamp; caches = cache_dir(); copy = os.path.join(DM_ROOT, 'out', 'macapp-cold')
    run = os.path.join(out, 'run'); warm_sup = os.path.join(out, 'warm_sup')
    plan.update(mac_cleanup_plan(bid, copy, [os.path.join(run, 'support'), warm_sup], caches, os.path.expanduser('~')))
    cache = os.path.join(caches, bid)
    if os.path.exists(cache): raise RuntimeError(f'{cache} exists before the run: not a cold start')
    app = mac_app_build(copy, bid)
    env = {**REC_ENV, 'DM_BOOTPROF': '1', **env}
    res = dict(bundle_id=bid, cache_before_bytes=0, launcher_warm_s=None, fixture=os.path.basename(a.user_manifest) if a.user_manifest else None)
    if a.user_manifest:   # a per-run copy: runs append to their manifest
        fx = os.path.join(out, 'pso_user_fixture.tsv'); shutil.copyfile(a.user_manifest, fx); env['DM_PSO_USER'] = fx
    if a.launcher_warm:
        os.makedirs(warm_sup, exist_ok=True)
        if a.user_manifest: shutil.copyfile(a.user_manifest, os.path.join(warm_sup, 'pso_user.tsv'))
        t0 = time.time()
        with open(os.path.join(out, 'warm.log'), 'w', encoding='utf-8') as lf:
            subprocess.run([app_exe(app), '--warm-shaders'], env=dict(os.environ, DM_WARM_PROCS='1', DM_APP_SUPPORT=warm_sup), stdout=lf, stderr=subprocess.STDOUT, check=False)
        res['launcher_warm_s'] = round(time.time() - t0, 1)
    old = os.environ.get('DM_MAC_APP'); os.environ['DM_MAC_APP'] = app
    try: r = bench.run_once(a.route, 'mac-app', run, env, None, 120)
    finally:
        if old is None: os.environ.pop('DM_MAC_APP', None)
        else: os.environ['DM_MAC_APP'] = old
    res['cache_after_bytes'] = du(cache)
    log = os.path.join(run, 'support', 'Logs', 'host.log')
    with open(log, encoding='utf-8', errors='replace') if os.path.exists(log) else open(os.devnull, encoding='utf-8') as f: text = f.read()
    res.update(cold_summary(text, nvn_series(os.path.join(run, 'nvn.tsv')), run_marks(os.path.join(run, 'events.ndjson'))))
    res['scenario_ok'] = r['ok']; res['reason'] = r.get('reason')
    res['cold_ok'] = res['cache_after_bytes'] > 0
    return res


def cold_ios(a, out, env, stamp, plan):
    # No lock is taken here: the game lock cmd_cold holds is the iOS-on-Mac device lock itself (mlock.sh takes $DM_ROOT/run/ios/macsmoke.lock,
    # the lock smoke_mac.sh and scenario_launch.sh take), so the check that the package is not running, the cache move, the run and the size read
    # after it are serialised with every other iOS-on-Mac run, and scenario_launch.sh re-enters the lock (DM_DEVLOCK_HELD, a descendant)
    import shutil, subprocess   # noqa: E401, PLC0415
    bid = local_env('DM_BUNDLE_ID')
    if not bid: raise RuntimeError('DM_BUNDLE_ID is not set (dm/ios/local.env)')
    if subprocess.run(['pgrep', '-f', '/Wrapper/DmIOS.app/DmIOS'], capture_output=True, check=False).returncode == 0: raise RuntimeError('the iOS package is running: end it first')
    caches = cache_dir(); root = os.path.join(caches, bid, bid)
    res = dict(bundle_id=bid, cache_before_bytes=du(root), fixture=os.path.basename(a.user_manifest) if a.user_manifest else None, launcher_warm_s=None)
    for p in ios_cache_plan(bid, caches): trash(p)
    env = {'DM_GAME_SEED': '305419896', 'DM_METAL_COLD': '1', 'DM_BOOTPROF': '1', **env}
    if a.user_manifest:
        c = ios_container(bid)
        if not c: raise RuntimeError('no container for the iOS package: run dm/ios/tools/smoke_mac.sh 1 first')
        d = os.path.join(c, 'Documents', 'pso_cold'); os.makedirs(d, exist_ok=True)
        fx = os.path.join(d, f'{stamp}.tsv'); shutil.copyfile(a.user_manifest, fx); env['DM_PSO_USER'] = fx; plan['moves'] = [fx]
    run = os.path.join(out, 'run')
    r, code = run_scn(route_text(a.route, env), 'ios-mac', run)
    res['cache_after_bytes'] = du(root)
    log = os.path.join(run, 'port.log')
    with open(log, encoding='utf-8', errors='replace') if os.path.exists(log) else open(os.devnull, encoding='utf-8') as f: text = f.read()
    res.update(cold_summary(text))
    res['scenario_ok'] = code == 0; res['reason'] = r.get('reason')
    res['cold_ok'] = os.path.isdir(os.path.join(root, 'com.apple.metal')) and res['cache_after_bytes'] > 0
    return res


def run_plan(plan):
    import subprocess   # noqa: PLC0415
    for c in plan.get('cmds', []): subprocess.run(c, capture_output=True, check=False)
    for p in plan.get('moves', []) + [q for q in plan.get('optional', []) if os.path.exists(q)]: trash(p)


def cmd_cold(a, argv):
    import json as _json, scenario   # noqa: E401, PLC0415
    held = scenario.game_lock(__file__, argv, f'pso_seed cold {a.backend} ~8 min', True)
    if held is not None: return held
    stamp = stamp_now(); out = a.out or os.path.join(DM_ROOT, 'run', 'pso', f'cold_{a.backend}_{stamp}'); os.makedirs(out, exist_ok=True)
    env = parse_env(a.env); plan = {}; res = {}
    try:
        res = (cold_mac if a.backend == 'mac-app' else cold_ios)(a, out, env, stamp, plan)
    except RuntimeError as e:
        res = dict(scenario_ok=False, cold_ok=False, reason=str(e))
    finally:
        if not a.keep: run_plan(plan)
    if not res.get('cold_ok') and res.get('scenario_ok'): res['reason'] = 'cold path not used: the Metal cache was not recreated'
    ok = bool(res.get('scenario_ok') and res.get('cold_ok'))
    row = dict(date=stamp, rev=git_short(), backend=a.backend, env=' '.join(f'{k}={v}' for k, v in env.items()), **{k: v for k, v in res.items() if k not in ('host_built',)})
    tsv = os.path.join(DM_ROOT, 'run', 'pso', 'cold.tsv'); os.makedirs(os.path.dirname(tsv), exist_ok=True)
    new = not os.path.exists(tsv)
    with open(tsv, 'a', encoding='utf-8') as f:
        if new: f.write('\t'.join(COLD_COLS) + '\n')
        f.write('\t'.join('' if row.get(k) is None else str(row.get(k)) for k in COLD_COLS) + '\n')
    for h in res.get('host_built', []): print(f"HOST {h['what']} {h['kind']} {h['ms']} ms")
    print(f"PSO_COLD {'OK' if ok else 'RED'} {a.backend}" + ('' if ok else f" ({res.get('reason')})"))
    print('RESULT ' + _json.dumps(dict(name='pso_seed cold', verdict='OK' if ok else 'RED', dir=out, **res)))
    return 0 if ok else 1


COLD_COLS = ('date', 'rev', 'backend', 'env', 'fixture', 'cache_before_bytes', 'cache_after_bytes', 'warm_fg_ms', 'warm_all_ms', 'load_wait_ms', 'sync_pso_front', 'sync_lib_front',
             'sync_pso_game', 'game_p99_ms', 'slow_with_creations', 'kicked', 'taken', 'host_lib', 'host_pso', 'host_ms', 'launcher_warm_s', 'scenario_ok', 'cold_ok', 'reason')


def main(argv):
    ap = argparse.ArgumentParser(prog='pso_seed.py', description='Pipeline warm-up seed tool (dm/gfx/nvn/pso_manifest.tsv).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('diff', help='rows of the manifests that the seed lacks')
    d.add_argument('manifests', nargs='+'); d.add_argument('--seed', default=SEED)
    n = sub.add_parser('normalise', help='check, normalise and dedupe a manifest')
    n.add_argument('input'); n.add_argument('--out'); n.add_argument('--note', action='append', default=[], help='one more comment line (repeatable)')
    c = sub.add_parser('check', help='rows of runs or manifests the seed and the allow-list lack')
    c.add_argument('paths', nargs='+'); c.add_argument('--seed', default=SEED); c.add_argument('--allow', default=ALLOW)
    g = sub.add_parser('gate', help='one route, then check')
    g.add_argument('--backend', default='mac-app', choices=('mac-app', 'cli', 'ios-mac')); g.add_argument('--headless', action='store_true')
    g.add_argument('--route', default=TOWN); g.add_argument('--out'); g.add_argument('--seed', default=SEED); g.add_argument('--allow', default=ALLOW)
    r = sub.add_parser('record', help='record the route matrix')
    r.add_argument('--set', default='all'); r.add_argument('--only', choices=('cli', 'mac-app', 'ios-mac')); r.add_argument('--front', default='create', choices=('create', 'menu'))
    r.add_argument('--out'); r.add_argument('--keep', action='store_true')
    m = sub.add_parser('merge', help='build the new seed from a record folder')
    m.add_argument('--dir', required=True); m.add_argument('--seed', default=SEED); m.add_argument('--write', action='store_true')
    o = sub.add_parser('cold', help='one true cold start')
    o.add_argument('--backend', default='mac-app', choices=('mac-app', 'ios-mac')); o.add_argument('--route', default=TOWN); o.add_argument('--env', action='append', default=[])
    o.add_argument('--user-manifest'); o.add_argument('--launcher-warm', action='store_true'); o.add_argument('--keep', action='store_true'); o.add_argument('--out')
    a = ap.parse_args(argv)
    if a.cmd == 'check': return cmd_check(a)
    if a.cmd == 'gate': return cmd_gate(a, argv)
    if a.cmd == 'record': return cmd_record(a, argv)
    if a.cmd == 'merge': return cmd_merge(a)
    if a.cmd == 'cold': return cmd_cold(a, argv)
    if a.cmd == 'normalise':
        with open(a.input, encoding='utf-8', errors='replace') as f: lines = f.readlines()
        out, dropped, kept = normalise(lines)
        for tag, why, val in dropped: print(f'DROPPED {tag} {why} {val[:40]}')
        text = '\n'.join([HEAD] + NORM_NOTE + ['# ' + x for x in a.note] + out) + '\n'
        if a.out:
            tmp = a.out + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f: f.write(text)
            os.replace(tmp, a.out)
        else: sys.stdout.write(text)
        print(f"KEPT S {kept['S']} R {kept['R']} C {kept['C']}", file=sys.stdout if a.out else sys.stderr)
        why = {}
        for tag, w, _ in dropped: why[f'{tag} {w}'] = why.get(f'{tag} {w}', 0) + 1
        print('RESULT ' + json.dumps(dict(name='pso_seed normalise', kept=kept, dropped=why)), file=sys.stdout if a.out else sys.stderr)
        return 0
    if a.cmd == 'diff':
        rows = diff(a.manifests, a.seed); n = count(rows)
        for r in rows: print(f"{r['tag']}\t{r['vs']}\t{r['fs']}")
        print(f"DIFF S {n['S']} R {n['R']} C {n['C']}")
        print('RESULT ' + json.dumps(dict(name='pso_seed diff', counts=n, rows=rows)))
        return 0
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
