#!/usr/bin/env python3
# 引擎分析库（Ghidra 工程 $DM_ROOT/re/ghidra/d2r）的无界面操作；由 re.sh 在 $DM_ROOT/re/venv（Python 3.13 + pyghidra）里调用，不在项目 venv 里跑。
#   import    nso2elf 产物 → 工程（ELF 加载器，基址 0x7100000000：低 32 位 = main+0x 偏移），不做分析；
#             按 .eh_frame 的函数起点先反汇编并建函数，标出 Ghidra 自带清单里没有的不返回导入。
#   analyze   跑一遍自动分析（含 Decompiler Parameter ID），存盘。
#   baseline  多线程把每个函数反编译一遍，记 成功 / 超时 / 失败 计数与失败地址 → log/decomp_baseline.json。
#   check     导入核对：重定位条数与状态、重定位后字节与独立计算一致、每个 eh_frame 起点都是函数入口且函数体不越界 → log/check.json。
#   地址约定：对外一律写 main+0x…；工程里的地址 = 0x7100000000 + 偏移。
# Headless operations on the engine-analysis database (the Ghidra project $DM_ROOT/re/ghidra/d2r); run by re.sh inside $DM_ROOT/re/venv (Python 3.13 + pyghidra), not the project venv.
#   import    nso2elf output -> project (ELF loader, image base 0x7100000000: the low 32 bits = the main+0x offset), no analysis;
#             disassembles and creates a function at every .eh_frame start first, and marks the non-returning imports missing from Ghidra's own list.
#   analyze   one auto-analysis pass (including Decompiler Parameter ID), then save.
#   baseline  decompiles every function once on several threads, recording success / timeout / failure counts and the failing addresses -> log/decomp_baseline.json.
#   check     import checks: relocation count and status, relocated bytes equal an independent computation, every eh_frame start is a function entry and no body crosses its range -> log/check.json.
#   Address convention: always main+0x... to the outside; an address in the project = 0x7100000000 + offset.
import argparse
import json
import os
import struct
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent)); sys.path.insert(0, str(HERE.parent.parent / 'verify'))
DM_ROOT = Path(os.environ.get('DM_ROOT') or Path.home() / 'dm-work')
RE = DM_ROOT / 're'
PROJECT_DIR, PROJECT = RE / 'ghidra', 'd2r'
BASE = 0x7100000000
ELF = RE / 'in' / 'main.elf'
NSO = DM_ROOT / 'data/switch/3.3.94012/Update/ExeFS/main'
NORETURN = ('_ZN2nn4diag6detail9AbortImpl', '_ZN2nn6detail21UnexpectedDefaultImpl', 'nndetailDiagAbortFormatImpl', '__cxa_rethrow')
BIG_FN = 0x4000                     # 超过 16 KiB 的函数反编译给更长时限 | Functions over 16 KiB get a longer decompile timeout


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


def start(maxmem):
    from pyghidra.launcher import HeadlessPyGhidraLauncher
    launcher = HeadlessPyGhidraLauncher()
    launcher.add_vmargs(f'-Xmx{maxmem}')
    launcher.start()


def open_project(create=False):
    import pyghidra
    PROJECT_DIR.mkdir(parents=True, exist_ok=True)
    return pyghidra.open_project(str(PROJECT_DIR), PROJECT, create=create)


def fde_functions():
    """(偏移, 长度) 列表，取自原映像的 .eh_frame（与 guestsyms.py 同一解法）。 | List of (offset, length) from the original image's .eh_frame (same method as guestsyms.py)."""
    import guestsyms
    import nsoimg
    img = nsoimg.load(str(NSO))
    return [f for f in guestsyms.functions(img) if f[1] > 0]


def addr(program, off):
    return program.getAddressFactory().getDefaultAddressSpace().getAddress(BASE + off)


def cmd_import(a):
    import pyghidra
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.program.model.address import AddressSet
    from ghidra.util.task import TaskMonitor
    project = open_project(create=True)
    if project.getProjectData().getFile('/main.elf') is not None: sys.exit('🔴 /main.elf is already in the project; delete the project folder to re-import')
    loader = pyghidra.program_loader().project(project).source(str(ELF)).name('main.elf').loaders('ElfLoader')
    loader = loader.addLoaderArg('-loader-imagebase', f'{BASE:x}')
    with loader.load() as results:
        results.save(TaskMonitor.DUMMY)
    with pyghidra.program_context(project, '/main.elf') as program:
        got = program.getImageBase().getOffset()
        if got != BASE: sys.exit(f'🔴 image base is {got:#x}, expected {BASE:#x}')
        fns = fde_functions()
        log(f'{len(fns)} eh_frame functions: disassembling')
        with pyghidra.transaction(program, 'eh_frame functions'):
            starts = AddressSet()
            for off, _ in fns: starts.add(addr(program, off))
            DisassembleCommand(starts, None, True).applyTo(program, TaskMonitor.DUMMY)
            made = 0
            for off, _ in fns:
                if CreateFunctionCmd(addr(program, off)).applyTo(program, TaskMonitor.DUMMY): made += 1
            log(f'{made} functions created')
            n = 0
            for f in program.getFunctionManager().getExternalFunctions():
                if f.getName().startswith(NORETURN): f.setNoReturn(True); n += 1
            log(f'{n} imports marked non-returning')
        program.save('import: eh_frame functions', TaskMonitor.DUMMY)
    project.close()
    print('IMPORT OK')


def cmd_analyze(a):
    import pyghidra
    from ghidra.util.task import TaskMonitor
    project = open_project()
    with pyghidra.program_context(project, '/main.elf') as program:
        props = pyghidra.analysis_properties(program)
        with pyghidra.transaction(program, 'analysis options'):
            props.setBoolean('Decompiler Parameter ID', True)
        t0 = time.time()
        log('analysis started')
        text = pyghidra.analyze(program)
        (RE / 'log' / 'analysis.log').write_text(text)
        program.save('auto-analysis', TaskMonitor.DUMMY)
        log(f'analysis done in {(time.time() - t0) / 60:.1f} min; {program.getFunctionManager().getFunctionCount()} functions')
    project.close()
    print('ANALYZE OK')


def cmd_baseline(a):
    import pyghidra
    from ghidra.app.decompiler import DecompInterface, DecompileOptions
    from ghidra.util.task import ConsoleTaskMonitor
    project = open_project()
    with pyghidra.program_context(project, '/main.elf') as program:
        fns = [f for f in program.getFunctionManager().getFunctions(True) if not f.isExternal() and not f.isThunk()]
        log(f'decompiling {len(fns)} functions on {a.jobs} threads')
        def worker(chunk):
            d = DecompInterface(); d.setOptions(DecompileOptions()); d.openProgram(program)
            out = []
            for f in chunk:
                big = f.getBody().getNumAddresses() > BIG_FN
                r = d.decompileFunction(f, a.big_timeout if big else a.timeout, ConsoleTaskMonitor())
                if r.decompileCompleted(): out.append(('ok', None))
                elif r.isTimedOut(): out.append(('timeout', f.getEntryPoint().getOffset() - BASE))
                else: out.append(('fail', f.getEntryPoint().getOffset() - BASE))
            d.dispose()
            return out
        chunks = [fns[i::a.jobs] for i in range(a.jobs)]
        t0 = time.time()
        with ThreadPoolExecutor(a.jobs) as ex:
            res = [x for part in ex.map(worker, chunks) for x in part]
        counts = {k: sum(1 for r in res if r[0] == k) for k in ('ok', 'timeout', 'fail')}
        out = {'functions': len(res), **counts, 'minutes': round((time.time() - t0) / 60, 1),
               'timeout_s': a.timeout, 'big_timeout_s': a.big_timeout,
               'timeouts': [f'main+{o:#x}' for k, o in res if k == 'timeout'], 'failures': [f'main+{o:#x}' for k, o in res if k == 'fail']}
        (RE / 'log' / 'decomp_baseline.json').write_text(json.dumps(out, indent=1))
        log(f"baseline: {counts} in {out['minutes']} min")
    project.close()
    print(f"BASELINE OK {counts['ok']}/{len(res)} decompiled, {counts['timeout']} timeouts, {counts['fail']} failures")


def cmd_check(a):
    import pyghidra
    import nsoimg
    project = open_project()
    report, red = {}, []
    img = nsoimg.load(str(NSO))
    rel = nsoimg.relocs(img)
    with pyghidra.program_context(project, '/main.elf') as program:
        mem = program.getMemory()
        # ① 重定位：条数、状态 | ① relocations: count and status
        from ghidra.program.model.reloc import Relocation
        Status = Relocation.Status
        rt = program.getRelocationTable()
        status = {}
        for r in rt.getRelocations():
            k = str(r.getStatus()); status[k] = status.get(k, 0) + 1
        report['relocations'] = {'expected': len(rel), 'in_program': sum(status.values()), 'status': status}
        bad = sum(v for k, v in status.items() if k in (str(Status.FAILURE), str(Status.UNSUPPORTED)))
        if sum(status.values()) != len(rel) or bad: red.append('relocations')
        # ② 重定位后字节 = 独立计算（RELATIVE / ABS64 定义符号；导入槽不比） | ② relocated bytes = an independent computation (RELATIVE / ABS64 of defined symbols; import slots not compared)
        from jpype import JArray, JByte
        diff = checked = 0
        for off, typ, name, val, add in rel:
            if typ == 1027: want = BASE + add
            elif typ in (257, 1025) and val is not None: want = BASE + val + add
            else: continue
            buf = JArray(JByte)(8); mem.getBytes(addr(program, off), buf)
            got = struct.unpack('<Q', bytes((b & 0xff) for b in buf))[0]
            checked += 1; diff += got != want
        report['relocated_bytes'] = {'checked': checked, 'different': diff}
        if diff: red.append('relocated bytes')
        # ③ 原字节：未重定位处与原映像相同（抽 text 全段） | ③ original bytes: where nothing is relocated, same as the original image (the whole of text)
        t_sz = struct.unpack_from('<I', open(NSO, 'rb').read(0x30), 0x18)[0]
        buf = JArray(JByte)(t_sz); mem.getBytes(addr(program, 0), buf)
        same = bytes((b & 0xff) for b in buf) == img[:t_sz]
        report['text_bytes_equal'] = same
        if not same: red.append('text bytes')
        # ④ eh_frame 函数 | ④ eh_frame functions
        fm = program.getFunctionManager()
        missing, crossing = [], []
        for off, ln in fde_functions():
            f = fm.getFunctionAt(addr(program, off))
            if f is None: missing.append(f'main+{off:#x}'); continue
            hi = f.getBody().getMaxAddress().getOffset() - BASE
            if hi >= off + ln: crossing.append(f'main+{off:#x}')
        report['eh_functions'] = {'total': len(fde_functions()), 'missing': len(missing), 'body_crosses_fde': len(crossing),
                                  'missing_list': missing[:200], 'crossing_list': crossing[:200], 'program_functions': fm.getFunctionCount()}
        if missing: red.append('eh_frame functions missing')
    project.close()
    (RE / 'log' / 'check.json').write_text(json.dumps(report, indent=1))
    print(json.dumps({k: v for k, v in report.items() if k != 'eh_functions'} | {'eh_functions': {k: v for k, v in report['eh_functions'].items() if not k.endswith('_list')}}))
    print('CHECK ' + ('OK' if not red else 'RED ' + ', '.join(red)))
    return 1 if red else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '10g'))
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('import'); sub.add_parser('analyze'); sub.add_parser('check')
    b = sub.add_parser('baseline')
    b.add_argument('--jobs', type=int, default=8); b.add_argument('--timeout', type=int, default=60); b.add_argument('--big-timeout', type=int, default=300)
    a = ap.parse_args()
    start(a.maxmem)
    return {'import': cmd_import, 'analyze': cmd_analyze, 'baseline': cmd_baseline, 'check': cmd_check}[a.cmd](a) or 0


if __name__ == '__main__':
    sys.exit(main())
