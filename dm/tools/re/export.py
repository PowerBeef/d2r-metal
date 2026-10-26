#!/usr/bin/env python3
# 把引擎分析库里有意义的名字与 /dm 类别下的类型导出成文本（$DM_ROOT/re/export），并提交进 $DM_ROOT/re 的本机仓库（没有远端、禁止推送）。
#   export/main.names.tsv  偏移 名字 种类 可信度 来源：手工名（manual）、种子与加载器给的名字（seed）、分析得出的非缺省函数名（analysis）；
#                          可信度取 names.tsv 的值（没有就按来源：manual / seed = high，analysis = analysis）。guestsyms.py 读它。
#   export/types.h         /dm 类别下全部类型的 C 头（Ghidra DataTypeWriter）。
#   用法（re.sh export 调用；工程不能被别的进程打开）：export.py [--no-commit]
# Exports the meaningful names and the types under the /dm category of the engine-analysis database as text ($DM_ROOT/re/export), and commits them to the local repository in $DM_ROOT/re (no remote, pushes refused).
#   export/main.names.tsv  offset name kind confidence source: hand names (manual), names from seeding and the loader (seed), non-default function names from analysis (analysis);
#                          the confidence comes from names.tsv (otherwise by source: manual / seed = high, analysis = analysis). guestsyms.py reads it.
#   export/types.h         C header of every type under the /dm category (Ghidra DataTypeWriter).
#   Usage (called by re.sh export; the project must not be open in another process): export.py [--no-commit]
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ghidra_db  # noqa: E402
import seed  # noqa: E402

OUT = ghidra_db.RE / 'export'
DEFAULT_NAME = re.compile(r'^(FUN|DAT|LAB|PTR|SUB|thunk_FUN|switchD|caseD|s|u)_[0-9a-fA-F]')


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
    conf = {off: c for off, _, c, _ in seed.names_table()}
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
            c = conf.get(off - lo) or ('analysis' if src == 'analysis' else 'high')
            rows.append((off - lo, name, kind, c, src))
        dtm = program.getDataTypeManager()
        cat = dtm.getCategory(CategoryPath('/dm'))
        types = list(cat.getDataTypes()) if cat else []
        w = FileWriter(str(OUT / 'types.h'))
        try: DataTypeWriter(dtm, w).write(types, TaskMonitor.DUMMY)
        finally: w.close()
    project.close()
    rows.sort()
    with open(OUT / 'main.names.tsv', 'w', encoding='utf-8') as f:
        f.write('# 引擎分析库导出的名字：由游戏程序推出，只放本机，不入库 | names exported from the engine-analysis database: derived from the game program, local only, never committed\n')
        f.write('offset\tname\tkind\tconfidence\tsource\n')
        for off, name, kind, c, src in rows: f.write(f'{off:#x}\t{name}\t{kind}\t{c}\t{src}\n')
    by = {}
    for r in rows: by[r[4]] = by.get(r[4], 0) + 1
    print(f'  names: {len(rows)} {by}; types: {len(types)}')
    if not a.no_commit:
        git = ['git', '-C', str(ghidra_db.RE)]
        subprocess.run(git + ['add', '-A', 'export', 'notes'] + (['names.tsv'] if seed.NAMES.exists() else []), check=True)
        if subprocess.run(git + ['diff', '--cached', '--quiet'], check=False).returncode:
            subprocess.run(git + ['commit', '-qm', f'export: {len(rows)} names, {len(types)} types'], check=True)
            print('  committed to the local-only repository in', ghidra_db.RE)
    print('EXPORT OK', OUT / 'main.names.tsv')


if __name__ == '__main__':
    main()
