#!/usr/bin/env python3
# Analysis profile of the engine-analysis database: which inputs and tool revisions produced the export, and what changed since.
#   Each step that shapes the database stamps what it used, when it runs ($DM_ROOT/re/log/stamps.json):
#     import   the game's ExeFS main, re/in/main.elf, the NSO build ID, nso2elf.py, ghidra_db.py
#     analyze  ghidra_db.py, the Ghidra version, the analysis options
#     seed     names.tsv, names_auto.tsv, enums.tsv, export/reflect.json, export/tables.json, seed.py
#     reflect / tables / autoname   their own script
#   export.py then writes $DM_ROOT/re/export/profile.json from those stamps (a key no step has stamped yet is recorded as it is at
#   export time and listed under `unstamped`). `re.sh profile` and `re.sh status` compare the profile with the files and tools now.
#   inputs  sha256 of the files above (a missing file is null, never an empty hash) and the NSO build ID
#   tools   git blob ids of the scripts in dm/tools/re that shape the database; ghidra / analysis: the engine and its options
#   checks  the import check (log/check.json) and decompile baseline (log/decomp_baseline.json) summaries, recorded, not compared
#   A stale profile does not mean the database is wrong: it means the export no longer proves which inputs it came from.
# Usage: dbprofile.py [check|show]   (default check; show prints the files and tools as they are now)
#   check: last line PROFILE OK | PROFILE STALE <n> change(s) | ⚪ PROFILE unavailable, then RESULT {json}; exit 1 only when stale
import datetime
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
DM_ROOT = Path(os.environ.get('DM_ROOT') or Path.home() / 'dm-work')
RE = DM_ROOT / 're'
PROFILE = RE / 'export' / 'profile.json'
STAMPS = RE / 'log' / 'stamps.json'
NSO = DM_ROOT / 'data/switch/3.3.94012/Update/ExeFS/main'
GHIDRA = Path(os.environ.get('GHIDRA_INSTALL_DIR', '/opt/homebrew/Cellar/ghidra/12.2/libexec'))
INPUTS = {'exefs_main': NSO, 'main_elf': RE / 'in' / 'main.elf', 'names_tsv': RE / 'names.tsv', 'names_auto_tsv': RE / 'names_auto.tsv',
          'enums_tsv': RE / 'enums.tsv', 'reflect_json': RE / 'export' / 'reflect.json', 'tables_json': RE / 'export' / 'tables.json'}
TOOLS = ('nso2elf.py', 'ghidra_db.py', 'seed.py', 'autoname.py', 'reflect.py', 'tables.py', 'export.py')
ANALYSIS = {'decompiler_parameter_id': True}   # the options ghidra_db.py analyze sets beyond the defaults
SEED_INPUTS = ('names_tsv', 'names_auto_tsv', 'enums_tsv', 'reflect_json', 'tables_json')
REBUILD = 'rebuild the database: move re/ghidra aside (import refuses an existing /main.elf), then re.sh convert, import, analyze, check, baseline, seed, export'
# What to run when a field differs from now: the database or the export no longer matches it
REMEDY = {'exefs_main': 'another game binary: ' + REBUILD, 'nso_build_id': 'another game build: ' + REBUILD, 'main_elf': 'main.elf changed: ' + REBUILD,
          'nso2elf.py': 'the converter changed: ' + REBUILD, 'ghidra_db.py': 'import / analysis code changed: rerun its changed step (import or analyze: ' + REBUILD + ')',
          'ghidra': 'another Ghidra: re.sh check and baseline, then seed and export', 'analysis': 'other analysis options: ' + REBUILD,
          'seed.py': 're.sh seed && re.sh export', 'export.py': 're.sh export',
          'autoname.py': 're.sh autoname (review names_auto.tsv), then re.sh seed && re.sh export',
          'reflect.py': 're.sh reflect, then re.sh seed && re.sh export', 'tables.py': 're.sh tables (server stopped), then re.sh seed && re.sh export'}
REMEDY.update({k: 're.sh seed && re.sh export' for k in SEED_INPUTS})


def sha256(path):
    p = Path(path)
    if not p.is_file(): return None
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()


def build_id(path):
    """The NSO header's 32-byte module ID at 0x40, hex (the build ID the rewrite rules are keyed on); None when unreadable."""
    try:
        with open(path, 'rb') as f: head = f.read(0x60)
    except OSError: return None
    return head[0x40:0x60].hex() if len(head) == 0x60 and head[:4] == b'NSO0' else None


def blob(name):
    out = subprocess.run(['git', '-C', str(REPO), 'hash-object', str(HERE / name)], capture_output=True, text=True, check=False)
    return out.stdout.strip() or None


def ghidra_version():
    props = GHIDRA / 'Ghidra' / 'application.properties'
    try:
        for line in props.read_text(encoding='utf-8').splitlines():
            if line.startswith('application.version='): return line.split('=', 1)[1].strip()
    except OSError: pass
    return None


def summary(path, keep):
    """The named fields of a JSON report, as recorded (nested objects one level deep, lists as their length); None when missing."""
    try: d = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError): return None
    def flat(v):
        if isinstance(v, list): return len(v)
        if isinstance(v, dict): return {k: (len(x) if isinstance(x, list) else x) for k, x in v.items() if not isinstance(x, dict)}
        return v
    return {k: flat(d[k]) for k in keep if k in d}


def current(key):
    """The value of one profile key as it is now: an input name, a tool file name, 'ghidra' or 'analysis'."""
    if key == 'nso_build_id': return build_id(NSO)
    if key in INPUTS: return sha256(INPUTS[key])
    if key in TOOLS: return blob(key)
    if key == 'ghidra': return ghidra_version()
    if key == 'analysis': return ANALYSIS
    raise KeyError(key)


def collect():
    """The files and tools as they are now, in the profile's shape."""
    repo = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, check=False).stdout.strip()
    return {
        'inputs': {k: current(k) for k in (*INPUTS, 'nso_build_id')},
        'tools': {t: current(t) for t in TOOLS},
        'ghidra': current('ghidra'),
        'analysis': current('analysis'),
        'checks': {'import_check': summary(RE / 'log' / 'check.json', ('text_bytes_equal', 'relocations', 'relocated_bytes', 'eh_functions')),
                   'decompile_baseline': summary(RE / 'log' / 'decomp_baseline.json', ('functions', 'ok', 'timeout', 'fail', 'timeouts', 'failures'))},
        'repo_commit': repo or None,
    }


def load(path):
    """A JSON object from disk, or None when missing or unreadable."""
    try: d = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError): return None
    return d if isinstance(d, dict) else None


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def stamp(step, keys):
    """Records what one step used, as it runs: {key: {value, step, at}} in log/stamps.json (the latest step that used a key wins)."""
    stamps = load(STAMPS) or {}
    at = datetime.datetime.now().isoformat(timespec='seconds')
    for k in keys: stamps[k] = {'value': current(k), 'step': step, 'at': at}
    save(STAMPS, stamps)


def write(extra=None):
    """export.py calls this after writing names and types: the profile is what each step stamped, plus the export's own revision."""
    now = collect()
    stamps = load(STAMPS) or {}
    p = now | {'stamps': {}, 'unstamped': []}
    for group in ('inputs', 'tools'):
        for k in now[group]:
            if k in stamps and k != 'export.py': p[group][k] = stamps[k]['value']; p['stamps'][k] = f"{stamps[k]['step']} {stamps[k]['at']}"
            elif k != 'export.py': p['unstamped'].append(k)
    for k in ('ghidra', 'analysis'):
        if k in stamps: p[k] = stamps[k]['value']; p['stamps'][k] = f"{stamps[k]['step']} {stamps[k]['at']}"
        else: p['unstamped'].append(k)
    p |= extra or {}
    save(PROFILE, p)
    return p


def diff(old, new):
    """[(field, old, new, remedy)] for every input, tool or engine setting that differs; the recorded checks are not compared."""
    out = []
    for group in ('inputs', 'tools'):
        for k in sorted(set(old.get(group, {})) | set(new.get(group, {}))):
            a, b = old.get(group, {}).get(k), new.get(group, {}).get(k)
            if a != b: out.append((f'{group}.{k}', a, b, REMEDY.get(k, 're.sh export')))
    for k in ('ghidra', 'analysis'):
        if old.get(k) != new.get(k): out.append((k, old.get(k), new.get(k), REMEDY.get(k, 're.sh export')))
    return out


def check():
    if not PROFILE.exists():
        print(f'⚪ PROFILE unavailable: no {PROFILE} yet (re.sh export writes it)')
        print('RESULT ' + json.dumps({'ok': None, 'unavailable': str(PROFILE)}))
        return 0
    old = load(PROFILE)
    if old is None:
        print(f'⚪ PROFILE unavailable: {PROFILE} is unreadable (rerun re.sh export)')
        print('RESULT ' + json.dumps({'ok': None, 'unavailable': str(PROFILE), 'unreadable': True}))
        return 0
    changes = diff(old, collect())
    for field, a, b, remedy in changes:
        print(f'  changed {field}: {str(a)[:16]} -> {str(b)[:16]}   ({remedy})')
    if old.get('unstamped'): print(f"  recorded at export, not when used (no step stamped them yet): {', '.join(old['unstamped'])}")
    when = old.get('exported', '?')
    print(f"PROFILE {'STALE ' + str(len(changes)) + ' change(s)' if changes else 'OK'} (export of {when}, repo {old.get('repo_commit')})")
    print('RESULT ' + json.dumps({'ok': not changes, 'changes': [c[0] for c in changes], 'exported': when, 'unstamped': old.get('unstamped', [])}))
    return 1 if changes else 0


def main(argv=None):
    cmd = (argv if argv is not None else sys.argv[1:]) or ['check']
    if cmd[0] == 'check': return check()
    if cmd[0] == 'show': print(json.dumps(collect(), indent=1, sort_keys=True)); return 0
    print('usage: dbprofile.py [check|show]', file=sys.stderr); return 2


if __name__ == '__main__':
    sys.exit(main())
