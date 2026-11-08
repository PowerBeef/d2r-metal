#!/usr/bin/env python3
"""Keep docs/ENGINE_CHANGES.md true: every change this port makes to the game's engine has a row, and every row points at things that exist.

  enginemap.py check    every rewrite entry, shader rule, hard-coded shader key and engine field the host sets is covered by a row;
                        every row's address, switch, test and commit exists; prints ENGINEMAP OK or ENGINEMAP RED with the details, exit 1
  enginemap.py list     what the sources say needs a row, and which row covers it

The sources are read, not imported (patch_nso.py runs at import):
  dm/tools/patch_nso.py        CUTS, OBS, PARAMS, SWITCHES (offsets), KEYS (names)
  dm/gfx/tools/dmsh_rules.py   RULES (function names)
  dm/gfx/nvn/nvn_mtl.m, *.inc  shader keys named in code ("<16 hex digits>")
  dm/host/dm_video.c           renderer config fields the host raises (rcfg_put)
  dm/gfx/nvn/*.inc             each replay-layer file a row names, or a tool listed in TOOL_INC
Other host take-overs and present-path changes have rows too, but nothing finds them for the check: those are reviewed by hand.

A row in the doc's Register section:

  ### EC-012 A short title
  - **Layer:** binary rule | engine field | shader rule | replay layer | present | host take-over
  - **Engine:** what the game does on its own
  - **Ours:** what the port does instead or on top
  - **Why:** the reason
  - **Setting:** the player's row, or none
  - **Switch:** DM_... that turns it off or forces it, or none
  - **Test:** what pins it: a gate step, a scenario, a t_ test or a file; or none: <why>
  - **Commit:** the commit or commits that made it
  - **Measured:** the effect, with device and date; or none: <why>
  - **Status:** confirmed | assumes:<unknown id>[,<unknown id>]
  - **Covers:** the sources' items it accounts for: main+0x..., KEYNAME, rule_function, shader key, renderer+0x...

Unknown ids name rows of the private unknowns ledger. Where the ledger is at hand (ENGINEMAP_LEDGER, default $DM_ROOT/re/notes/unknowns.tsv), a row may only
assume a question still open: one that was settled must become confirmed, or the row must change.
"""
import ast
import glob
import os
import re
import subprocess
import sys

DM = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
ROOT = os.path.dirname(DM)
FIELDS = ('Layer', 'Engine', 'Ours', 'Why', 'Setting', 'Switch', 'Test', 'Commit', 'Measured', 'Status', 'Covers')
LAYERS = ('binary rule', 'engine field', 'shader rule', 'replay layer', 'present', 'host take-over')
ROW = re.compile(r'^### (EC-\d{3}) (.+)$')
FIELD = re.compile(r'^- \*\*([A-Za-z]+):\*\* ?(.*)$')
SWITCH = re.compile(r'\bDM_[A-Z0-9]+(?:_[A-Z0-9]+)*\b')
SHADER_KEY = re.compile(r'"([0-9a-f]{16})"')
ADDR = re.compile(r'\bmain\+0x[0-9a-f]+\b')
FIELD_ADDR = re.compile(r'\brenderer\+0x[0-9a-f]+\b')
UNKNOWN_ID = re.compile(r'^u-[a-z0-9]+-\d+$')
# The replay layer's included files (dm/gfx/nvn/*.inc): one that changes what the engine draws must be named by a row; these are tools, each with why
TOOL_INC = {
    'nvn_capture.inc': 'frame capture for debugging (DM_NVN_CAPTURE)',
    'nvn_framerec.inc': 'the frame recorder the motion checks read',
    'nvn_testload.inc': 'known GPU work, the control for measurements (DM_TEST_LOAD)',
    'rt_range_check.inc': 'a private checker, not in normal builds',
}


def read(root, path):
    with open(os.path.join(root, path), encoding='utf-8') as f:
        return f.read()


def literal(tree, name):
    for n in tree.body:
        if isinstance(n, ast.Assign) and len(n.targets) == 1 and getattr(n.targets[0], 'id', None) == name:
            return ast.literal_eval(n.value)
    return None


def source_items(root):
    """What the sources say the map has to account for: {item: where}."""
    items = {}
    tree = ast.parse(read(root, 'dm/tools/patch_nso.py'))
    for table in ('CUTS', 'OBS', 'PARAMS', 'SWITCHES'):
        for rows in (literal(tree, table) or {}).values():
            for row in rows:
                items[f'main+{row[0]:#x}'] = f'patch_nso.py {table}'
    for key in literal(tree, 'KEYS') or {}:
        items[key] = 'patch_nso.py KEYS'
    rules = read(root, 'dm/gfx/tools/dmsh_rules.py')
    table = rules[rules.index('\nRULES = '):]
    for name in re.findall(r'\((\w+), lambda ', table):
        items[name] = 'dmsh_rules.py RULES'
    gfx = os.path.join(root, 'dm/gfx/nvn')
    for name in sorted(os.listdir(gfx)):
        if name == 'nvn_mtl.m' or name.endswith('.inc'):
            for key in SHADER_KEY.findall(read(gfx, name)):
                items[key] = f'{name} shader key'
    for off in re.findall(r'rcfg_put\(cfg, (0x[0-9a-f]+),', read(root, 'dm/host/dm_video.c')):
        items[f'renderer+{off}'] = 'dm_video.c rcfg_put'
    return items


def parse(doc):
    """The rows of the Register section, the ids the rest of the doc cites, and format errors."""
    rows, errors, cur, section = {}, [], None, None
    cited = []
    for i, line in enumerate(doc.split('\n'), 1):
        if line.startswith('## '):
            section, cur = line[3:].strip(), None
            continue
        if section == 'Register':
            m = ROW.match(line)
            if m:
                if m.group(1) in rows: errors.append(f'line {i}: {m.group(1)} appears twice')
                cur = rows.setdefault(m.group(1), {'title': m.group(2), 'line': i})
                continue
            m = FIELD.match(line)
            if m and cur is not None:
                if m.group(1) not in FIELDS: errors.append(f'line {i}: {m.group(1)} is not a field')
                cur[m.group(1)] = m.group(2).strip()
                continue
        else:
            cited += re.findall(r'\bEC-\d{3}\b', line)
    ids = list(rows)
    if ids != sorted(ids): errors.append('the rows are not in id order')
    return rows, cited, errors


def gate_steps(root):
    return set(re.findall(r'\bstep ([a-z0-9_]+)', read(root, 'dm/verify/fork_gate.sh')))


def file_names(root):
    """The base names of the repo's tests and scenarios, so one walk serves every row."""
    names = set()
    for base, dirs, files in os.walk(os.path.join(root, 'dm')):
        dirs[:] = [d for d in dirs if d not in ('third_party', '.git', 'build')]
        names.update(files)
    return names


def test_exists(token, root, steps, names):
    """A test token: a gate step, a t_ test, a scenario name or a path from the repo root."""
    if token in steps: return True
    if '/' in token or token.endswith(('.py', '.scn', '.sh')):
        return os.path.exists(os.path.join(root, token))
    return any(token + ext in names for ext in ('.scn', '.m', '.c', '.py', '.sh'))


def switches(root):
    known = set()
    with open(os.path.join(root, 'dm/switches.tsv'), encoding='utf-8') as f:
        for line in f:
            if line.startswith('DM_'): known.add(line.split('\t', 1)[0])
    return known


def commit_exists(sha, root):
    return subprocess.run(['git', '-C', root, 'cat-file', '-e', sha + '^{commit}'], capture_output=True).returncode == 0


def shallow(root):
    r = subprocess.run(['git', '-C', root, 'rev-parse', '--is-shallow-repository'], capture_output=True, text=True)
    return r.returncode != 0 or r.stdout.strip() != 'false'


def ledger_rows(path):
    if not path or not os.path.exists(path): return None
    rows = {}
    with open(path, encoding='utf-8') as lines:
        for line in lines:
            f = line.rstrip('\n').split('\t')
            if len(f) >= 3 and f[0].startswith('u-'): rows[f[0]] = f[2]
    return rows


def check(root, doc_path, ledger_path):
    doc = open(doc_path, encoding='utf-8').read()
    rows, cited, red = parse(doc)
    if not rows: red.append('no rows found in the Register section')
    items = source_items(root); steps = gate_steps(root); known = switches(root); ledger = ledger_rows(ledger_path); names = file_names(root)
    covered = {}
    for rid, r in rows.items():
        for field in FIELDS:
            if field not in r: red.append(f'{rid}: no {field}')
        if r.get('Layer') not in LAYERS: red.append(f'{rid}: layer {r.get("Layer")!r} is not one of {", ".join(LAYERS)}')
        for token in (t.strip() for t in r.get('Covers', '').split(',')):
            if not token or token == 'none': continue
            covered.setdefault(token, rid)
            if token not in items: red.append(f'{rid}: covers {token}, which the sources no longer have')
        for token in (t.strip() for t in r.get('Test', '').split(',')):
            if not token or token.startswith('none'): break
            if not test_exists(token, root, steps, names): red.append(f'{rid}: test {token} does not exist')
        for sw in SWITCH.findall(r.get('Switch', '')):
            if sw not in known: red.append(f'{rid}: switch {sw} is not in dm/switches.tsv')
        if not shallow(root):
            for sha in re.findall(r'\b[0-9a-f]{7,12}\b', r.get('Commit', '')):
                if not commit_exists(sha, root): red.append(f'{rid}: commit {sha} does not exist')
        status = r.get('Status', '')
        if status != 'confirmed':
            if not status.startswith('assumes:'): red.append(f'{rid}: status {status!r}: confirmed or assumes:<id>')
            else:
                for uid in status[8:].split(','):
                    uid = uid.strip()
                    if not UNKNOWN_ID.match(uid): red.append(f'{rid}: {uid!r} is not an unknown id')
                    elif ledger is not None and ledger.get(uid) != 'open': red.append(f'{rid}: assumes {uid}, which the ledger has as {ledger.get(uid, "missing")}')
    for item, where in sorted(items.items()):
        if item not in covered: red.append(f'{item} ({where}) has no row')
    named = '\n'.join(' '.join(str(v) for v in r.values()) for r in rows.values())
    for f in sorted(glob.glob(os.path.join(root, 'dm', 'gfx', 'nvn', '*.inc'))):
        b = os.path.basename(f)
        if b not in TOOL_INC and b not in named: red.append(f'dm/gfx/nvn/{b} has no row that names it (a tool goes in TOOL_INC, with why)')
    for rid in cited:
        if rid not in rows: red.append(f'{rid} is cited but has no row')
    for sw in sorted(set(SWITCH.findall(doc))):
        if sw not in known: red.append(f'switch {sw} is cited but is not in dm/switches.tsv')
    return rows, items, covered, red


def main(argv):
    root = os.environ.get('ENGINEMAP_ROOT', ROOT)
    doc = os.path.join(root, 'docs/ENGINE_CHANGES.md')
    ledger = os.environ.get('ENGINEMAP_LEDGER', os.path.join(os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work')), 're/notes/unknowns.tsv'))
    op = argv[1] if len(argv) > 1 else 'check'
    if op not in ('check', 'list'):
        print(__doc__); return 2
    if not os.path.exists(doc):
        print(f'ENGINEMAP RED: {doc} is missing'); return 1
    rows, items, covered, red = check(root, doc, ledger)
    if op == 'list':
        for item, where in sorted(items.items(), key=lambda kv: (kv[1], kv[0])):
            print(f'{item}\t{where}\t{covered.get(item, "-")}')
        return 0
    if red:
        print('ENGINEMAP RED')
        for line in red: print('  ' + line)
        return 1
    print(f'ENGINEMAP OK {len(rows)} rows cover {len(items)} source items')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
