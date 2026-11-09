#!/usr/bin/env python3
# The live harness's snapshot of the analysis map: $DM_ROOT/re/live/index.sqlite, read by live runs and reports whether or not the Ghidra
# project is open. Run by `re.sh live-index` (both steps) and `re.sh live-decomp main+0x…`:
#   image    (project venv: capstone) from the product's rewritten main: every basic block's first instruction, every call site (direct with
#            its target, or computed), and the addresses the rewriter changed (gates, cuts, observation gates, parameters, switches, register
#            sites), which live hooks must leave alone; plus the export's names
#   ghidra   ($DM_ROOT/re/venv: pyghidra, the project closed elsewhere) every function with its prototype (each parameter's name, type and
#            register), and the /dm structures and enums with their fields
#   decomp main+0x…   one function's decompiled text with each line's instruction addresses → $DM_ROOT/re/live/decomp/<offset>.json
#   stats    row counts
# Everything it writes is derived from the game program: it stays under $DM_ROOT/re, never in the repository
import argparse
import json
import os
import sqlite3
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DM_ROOT = Path(os.environ.get('DM_ROOT') or Path.home() / 'dm-work')
LIVE = DM_ROOT / 're' / 'live'
BASE = 0x7100000000

SCHEMA = """
create table if not exists meta(key text primary key, value text);
create table if not exists functions(off integer primary key, size integer, name text, ret text, sig_source text);
create table if not exists params(func integer, ord integer, name text, type text, storage text, primary key(func, ord));
create table if not exists blocks(off integer primary key, func integer);
create table if not exists calls(off integer primary key, func integer, target integer);   -- target null = computed (blr / br)
create table if not exists never_hook(off integer primary key, why text);
create table if not exists names(off integer primary key, name text, kind text, confidence text, source text, evidence text);
create table if not exists structs(name text primary key, path text, size integer);
create table if not exists fields(struct text, off integer, name text, type text, size integer);
create table if not exists enums(name text, member text, value integer);
create index if not exists blocks_func on blocks(func);
create index if not exists calls_func on calls(func);
create index if not exists fields_struct on fields(struct);
"""
BRANCH = ('b', 'cbz', 'cbnz', 'tbz', 'tbnz')
ENDS = ('ret', 'br', 'braa', 'brab', 'retaa', 'retab')


def connect(path=None):
    path = Path(path) if path else LIVE / 'index.sqlite'
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(str(path)); db.executescript(SCHEMA); return db


def scan_function(insns, start, end):
    """Block leaders and call sites of one function from its decoded instructions (address, mnemonic, operand text).
    A leader is the entry, a branch's target inside the function and whatever follows a branch or return (a call's return point is not one).
    Calls are (address, target) with target None when computed."""
    lead, calls = {start}, []
    for addr, mn, ops in insns:
        m = mn.split('.')[0]
        if m in BRANCH or m in ENDS:
            lead.add(addr + 4)
            if m in BRANCH:
                try: t = int(ops.split('#')[-1], 16)
                except ValueError: t = None
                if t is not None:
                    if start <= t < end: lead.add(t)
                    elif m == 'b': calls.append((addr, t))   # a tail call
        elif m == 'bl':
            try: calls.append((addr, int(ops.split('#')[-1], 16)))
            except ValueError: pass
        elif m in ('blr', 'blraa', 'blrab', 'blraaz', 'blrabz'): calls.append((addr, None))
        if m in ('br', 'braa', 'brab'): calls.append((addr, None))   # a computed jump: a jump table or a tail call
    return sorted(a for a in lead if a < end), calls


def read_never_hook(prod):
    """The rewritten addresses of main: {offset: why} from the product's tables."""
    out = {}
    def rows(name):
        p = Path(prod) / f'main.{name}.tsv'
        if not p.exists(): return
        with open(p, encoding='utf-8') as f:
            for line in f:
                if line.startswith('#') or not line.strip(): continue
                yield line.rstrip('\n').split('\t')
    for r in rows('gates'):   # column 4: where the gate returns to; the rewritten instruction is that one or the one before
        for o in (int(r[3], 16) - 4, int(r[3], 16)): out[o] = f'gate {r[1]} {r[2]}'
    for r in rows('cuts'): out[int(r[0], 16)] = 'cut'
    for r in rows('obs'): out[int(r[0], 16)] = f'observation gate {r[1]}'
    for r in rows('params'):
        for c in r[:2]:
            if c.startswith('0x'): out[int(c, 16)] = f'parameter {r[3] if len(r) > 3 else ""}'.strip()
    for r in rows('switches'):
        if r[0].startswith('0x'): out[int(r[0], 16)] = 'switch'
    for r in rows('sites'): out.setdefault(int(r[0], 16), f'site {r[1] if len(r) > 1 else ""}'.strip())
    return out


def read_names(path):
    rows = []
    if not Path(path).exists(): return rows
    with open(path, encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 6 and p[0].startswith('0x'): rows.append((int(p[0], 16), p[1], p[2], p[3], p[4], p[5]))
    return rows


def set_meta(db, **kv):
    for k, v in kv.items(): db.execute('insert or replace into meta values(?, ?)', (k, str(v)))


def cmd_image(a):
    sys.path.insert(0, str(HERE.parent))
    import capstone
    import guestsyms
    prod = guestsyms.product_dir(); t0 = time.time()
    with open(os.path.join(prod, 'main.patched.bin'), 'rb') as f: img = f.read()
    fns = [(s, n) for s, n, _, _ in guestsyms.load().get('main', [])]
    if not fns: raise SystemExit('no function table: python3 dm/tools/guestsyms.py build')
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    db = connect(a.db)
    for t in ('blocks', 'calls', 'never_hook', 'names'): db.execute(f'delete from {t}')
    nb = nc = 0
    for s, n in fns:
        lead, calls = scan_function(((ad, mn, ops) for ad, _, mn, ops in md.disasm_lite(img[s:s + n], s)), s, s + n)
        db.executemany('insert or ignore into blocks values(?, ?)', ((x, s) for x in lead)); nb += len(lead)
        db.executemany('insert or ignore into calls values(?, ?, ?)', ((x, s, t) for x, t in calls)); nc += len(calls)
    nh = read_never_hook(prod); db.executemany('insert or replace into never_hook values(?, ?)', nh.items())
    names = read_names(DM_ROOT / 're' / 'export' / 'main.names.tsv'); db.executemany('insert or replace into names values(?, ?, ?, ?, ?, ?)', names)
    set_meta(db, product=os.path.basename(prod), image_built=time.strftime('%Y-%m-%dT%H:%M:%S'), image_functions=len(fns))
    db.commit()
    print(f'LIVE-INDEX image OK: {len(fns)} functions, {nb} blocks, {nc} call sites, {len(nh)} rewritten addresses, {len(names)} names, {time.time() - t0:.1f} s → {a.db or LIVE / "index.sqlite"}')


def cmd_ghidra(a):
    sys.path.insert(0, str(HERE))
    import ghidra_db
    ghidra_db.start(a.maxmem); t0 = time.time()
    import pyghidra
    from ghidra.program.model.data import CategoryPath, Enum, Structure
    project = ghidra_db.open_project(); db = connect(a.db)
    for t in ('functions', 'params', 'structs', 'fields', 'enums'): db.execute(f'delete from {t}')
    nf = np_ = ns = ne = 0
    with pyghidra.program_context(project, '/main.elf') as program:
        it = program.getFunctionManager().getFunctions(True)
        while it.hasNext():
            f = it.next(); off = f.getEntryPoint().getOffset() - BASE
            if not 0 <= off < 0x100000000: continue
            db.execute('insert or replace into functions values(?, ?, ?, ?, ?)', (off, int(f.getBody().getNumAddresses()), str(f.getName()), str(f.getReturnType().getName()), str(f.getSignatureSource())))
            nf += 1
            for i, p in enumerate(f.getParameters()):
                db.execute('insert or replace into params values(?, ?, ?, ?, ?)', (off, i, str(p.getName()), str(p.getDataType().getName()), str(p.getVariableStorage()))); np_ += 1
        dtm = program.getDataTypeManager()
        def walk(cat):
            return [] if cat is None else list(cat.getDataTypes()) + [t for sub in cat.getCategories() for t in walk(sub)]
        for t in walk(dtm.getCategory(CategoryPath('/dm'))):
            if isinstance(t, Structure):
                db.execute('insert or replace into structs values(?, ?, ?)', (str(t.getName()), str(t.getCategoryPath()), int(t.getLength()))); ns += 1
                for c in t.getDefinedComponents():
                    db.execute('insert into fields values(?, ?, ?, ?, ?)', (str(t.getName()), int(c.getOffset()), str(c.getFieldName() or ''), str(c.getDataType().getName()), int(c.getLength())))
            elif isinstance(t, Enum):
                for n in t.getNames(): db.execute('insert into enums values(?, ?, ?)', (str(t.getName()), str(n), int(t.getValue(n)))); ne += 1
    project.close()
    set_meta(db, ghidra_built=time.strftime('%Y-%m-%dT%H:%M:%S'), ghidra_functions=nf)
    db.commit()
    print(f'LIVE-INDEX ghidra OK: {nf} functions, {np_} parameters, {ns} structures, {ne} enum members, {time.time() - t0:.1f} s')


def cmd_decomp(a):
    sys.path.insert(0, str(HERE))
    import ghidra_db
    ghidra_db.start(a.maxmem)
    import pyghidra
    from ghidra.app.decompiler import DecompInterface
    from ghidra.app.decompiler.component import DecompilerUtils
    from ghidra.util.task import TaskMonitor
    out_dir = LIVE / 'decomp'; out_dir.mkdir(parents=True, exist_ok=True)
    project = ghidra_db.open_project()
    with pyghidra.program_context(project, '/main.elf') as program:
        ifc = DecompInterface(); ifc.openProgram(program)
        for w in a.where:
            off = int(w.split('+')[-1], 16); f = program.getFunctionManager().getFunctionContaining(ghidra_db.addr(program, off))
            if f is None: print(f'  {w}: no function there'); continue
            res = ifc.decompileFunction(f, a.timeout, TaskMonitor.DUMMY)
            if not res.decompileCompleted(): print(f'  {w}: {res.getErrorMessage()}'); continue
            lines = []
            for ln in DecompilerUtils.toLines(res.getCCodeMarkup()):
                addrs = set(); text = str(ln.getIndentString())
                for tok in ln.getAllTokens():
                    text += str(tok.getText()); m = tok.getMinAddress()
                    if m is not None: addrs.add(int(m.getOffset()) - BASE)
                lines.append({'n': int(ln.getLineNumber()), 'text': text, 'addrs': sorted(addrs)})
            entry = f.getEntryPoint().getOffset() - BASE
            with open(out_dir / f'{entry:x}.json', 'w', encoding='utf-8') as fo: json.dump({'entry': entry, 'name': str(f.getName()), 'lines': lines}, fo)
            print(f'  main+{entry:#x} {f.getName()}: {len(lines)} lines → {out_dir / f"{entry:x}.json"}')
    project.close()


def cmd_stats(a):
    db = connect(a.db)
    for t in ('functions', 'params', 'blocks', 'calls', 'never_hook', 'names', 'structs', 'fields', 'enums'):
        print(f'{t:12s} {db.execute(f"select count(*) from {t}").fetchone()[0]}')
    print('computed calls', db.execute('select count(*) from calls where target is null').fetchone()[0])
    for k, v in db.execute('select * from meta order by key'): print(f'meta {k} = {v}')


def main(argv):
    ap = argparse.ArgumentParser(prog='live_index.py'); ap.add_argument('--db')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('image')
    g = sub.add_parser('ghidra'); g.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '6g'))
    d = sub.add_parser('decomp'); d.add_argument('where', nargs='+'); d.add_argument('--timeout', type=int, default=120); d.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '6g'))
    sub.add_parser('stats')
    a = ap.parse_args(argv)
    {'image': cmd_image, 'ghidra': cmd_ghidra, 'decomp': cmd_decomp, 'stats': cmd_stats}[a.cmd](a)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
