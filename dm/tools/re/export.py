#!/usr/bin/env python3
# Exports the meaningful names and the types under the /dm category of the engine-analysis database as text ($DM_ROOT/re/export), and commits them to the local repository in $DM_ROOT/re (no remote, pushes refused).
#   export/main.names.tsv  offset name kind confidence source evidence (guestsyms.py reads the first five columns):
#                          source = manual (hand names), seed (seeding and the loader), auto:<rule> (bulk names from names_auto.tsv that seed
#                          wrote; auto:dropped = a bulk name whose row is gone), analysis;
#                          confidence = names.tsv's value, else the bulk row's own value (low for a dropped row), else high (manual / seed)
#                          or analysis;
#                          evidence = the kind of proof behind the name: names.tsv's evidence column when given; derived for names our tooling
#                          computed (reflection, data tables, bulk-naming rules; known from seed's log/seed_names.tsv); else static
#                          (a code reading, the loader's symbols, the NVN slot table, widget vtables).
#   export/types.h         C header of every type under the /dm category (Ghidra DataTypeWriter).
#   export/profile.json    which inputs and tool revisions produced this export (dbprofile.py; `re.sh profile` reports what changed since).
#   Usage (called by re.sh export; the project must not be open in another process): export.py [--no-commit]
import argparse
import datetime
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dbprofile  # noqa: E402
import ghidra_db  # noqa: E402
import seed  # noqa: E402

OUT = ghidra_db.RE / 'export'
DEFAULT_NAME = re.compile(r'^(FUN|DAT|LAB|PTR|SUB|thunk_FUN|switchD|caseD|s|u)_[0-9a-fA-F]')


def classify(off, plain_name, src, conf, evidence, auto, seeded):
    """(confidence, source, evidence) of one exported name.
    conf / evidence: names.tsv by offset; auto: {offset: (name, confidence, rule)} of every names_auto.tsv row (any confidence: a row
    demoted to low still describes the name seed wrote earlier); seeded: {(offset, name): kind} from seed's ledger.
    A bulk row applies only while the database still carries that name and nobody set it by hand."""
    if off in conf: return conf[off], src, evidence.get(off, 'static')
    if src == 'seed' and off in auto and auto[off][0] == plain_name: return auto[off][1], f'auto:{auto[off][2]}', 'derived'
    kind = seeded.get((off, plain_name)) if src == 'seed' else None
    if kind == 'autoname': return 'low', 'auto:dropped', 'derived'
    return ('analysis' if src == 'analysis' else 'high'), src, ('derived' if kind in ('reflect', 'tables') else 'static')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-commit', action='store_true')
    ap.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '10g'))
    a = ap.parse_args()
    ghidra_db.start(a.maxmem)
    import pyghidra
    from java.io import FileWriter
    from ghidra.program.model.data import CategoryPath, DataTypeWriter
    from ghidra.program.model.symbol import SourceType
    from ghidra.util.task import TaskMonitor
    table = seed.names_table()
    conf = {off: c for off, _, c, _ in table}
    evidence = seed.names_evidence()
    # Bulk names keep their own confidence and rule (seed writes high / medium rows at addresses names.tsv leaves free; a row demoted
    # later still matches the name seed wrote)
    auto = {off: (name, c, rule) for off, name, c, rule, _ in seed.auto_names() if off not in conf}
    seeded = seed.seeded_names()
    if not seeded: print(f'  ⚠️ no {seed.LEDGER}: run re.sh seed first; reflection, table and dropped bulk names export as static / high')
    src_name = {SourceType.USER_DEFINED: 'manual', SourceType.IMPORTED: 'seed', SourceType.ANALYSIS: 'analysis'}
    OUT.mkdir(parents=True, exist_ok=True)
    project = ghidra_db.open_project()
    rows = []
    with pyghidra.program_context(project, '/main.elf') as program:
        lo, hi = ghidra_db.BASE, ghidra_db.BASE + 0x100000000
        it = program.getSymbolTable().getAllSymbols(True)
        while it.hasNext():
            sym = it.next()
            if sym.isExternal() or sym.getSource() == SourceType.DEFAULT: continue
            off = sym.getAddress().getOffset()
            if not lo <= off < hi: continue
            name = sym.getName(True)
            if DEFAULT_NAME.match(sym.getName()): continue
            src = src_name.get(sym.getSource())
            if src is None or '::override::' in name: continue   # 反编译器的调用覆盖标记 | the decompiler's call-override markers
            kind = 'function' if program.getFunctionManager().getFunctionAt(sym.getAddress()) is not None and sym.isPrimary() else 'label'
            if kind == 'label' and src == 'analysis': continue   # 分析起的标签（fde_ / switchD_ / lsda_ …）是簿记，不导 | labels made by analysis (fde_ / switchD_ / lsda_ ...) are bookkeeping, not exported
            c, src, ev = classify(off - lo, sym.getName(), src, conf, evidence, auto, seeded)
            rows.append((off - lo, name, kind, c, src, ev))
        dtm = program.getDataTypeManager()
        def walk(cat):   # /dm 及其子类别（/dm/reflect、/dm/enums） | /dm and its subcategories (/dm/reflect, /dm/enums)
            return [] if cat is None else list(cat.getDataTypes()) + [t for sub in cat.getCategories() for t in walk(sub)]
        types = walk(dtm.getCategory(CategoryPath('/dm')))
        w = FileWriter(str(OUT / 'types.h'))
        try: DataTypeWriter(dtm, w).write(types, TaskMonitor.DUMMY)
        finally: w.close()
    project.close()
    rows.sort()
    with open(OUT / 'main.names.tsv', 'w', encoding='utf-8') as f:
        f.write('# 引擎分析库导出的名字：由游戏程序推出，只放本机，不入库 | names exported from the engine-analysis database: derived from the game program, local only, never committed\n')
        f.write('offset\tname\tkind\tconfidence\tsource\tevidence\n')
        for off, name, kind, c, src, ev in rows: f.write(f'{off:#x}\t{name}\t{kind}\t{c}\t{src}\t{ev}\n')
    by = {}
    for r in rows: k = r[4].split(':')[0]; by[k] = by.get(k, 0) + 1
    print(f'  names: {len(rows)} {by}; types: {len(types)}')
    dbprofile.write({'exported': datetime.datetime.now().isoformat(timespec='seconds'), 'export_counts': {'names': len(rows), 'types': len(types)} | by})
    if not a.no_commit:
        git = ['git', '-C', str(ghidra_db.RE)]
        subprocess.run(git + ['add', '-A', 'export', 'notes'] + (['names.tsv'] if seed.NAMES.exists() else []), check=True)
        if subprocess.run(git + ['diff', '--cached', '--quiet'], check=False).returncode:
            subprocess.run(git + ['commit', '-qm', f'export: {len(rows)} names, {len(types)} types'], check=True)
            print('  committed to the local-only repository in', ghidra_db.RE)
    print('EXPORT OK', OUT / 'main.names.tsv')


if __name__ == '__main__':
    main()
