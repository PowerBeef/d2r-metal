#!/usr/bin/env python3
# 批量起名候选：从引擎分析库里机械地推出还没有名字的函数该叫什么，写进 $DM_ROOT/re/names_auto.tsv（只放本机，不入库）供复核；
#   seed 把其中 high / medium 的行写进工程（排在 names.tsv 之后：names.tsv 与手工名永远优先）。来源：
#   ① 字符串：函数引用的「类::方法」串（只有这一个函数引用它）→ 类__方法；库代码断言里的源文件路径（只引用一个文件）→ <库>_<文件>_<偏移>
#   ② 跳板：整个函数只有一条 b 到有名字的函数 → j_<名字>；只调一个有名函数的短包装（≤ 8 条）→ <名字>_w（low，只列不写）
#   ③ 虚表：已知虚表（vt_<类>）、库代码的 C++ 类型信息找到的虚表、反射类的工厂取址的虚表（只取一张时；取几张的只列为 low），只被这一张表引用的槽 → <类>_vf<槽号>
#   ④ 数据表回调：tables.json 里回调类列的回调函数 → TxtCb_<表>_<列>
#   ⑤ 客户端命令包处理函数：$DM_ROOT/re/notes/c2s_labels.tsv（逐个读过的结论）
#   打印起名覆盖率（有名字的函数 / 全部函数）现状与写入后的预计值。用法（re.sh autoname 调用；工程不能被别的进程打开）：autoname.py
# Bulk naming candidates: mechanically derives names for functions the engine-analysis database has not named yet and writes them to $DM_ROOT/re/names_auto.tsv (local only, never committed) for review;
#   seed writes the high / medium rows into the project (after names.tsv: names.tsv and hand names always win). Sources:
#   (1) strings: a "Class::Method" string referenced by this function only -> Class__Method; a source file path in library asserts (one file only) -> <library>_<file>_<offset>
#   (2) thunks: a whole function that is one b to a named function -> j_<name>; a short wrapper (≤ 8 instructions) calling one named function -> <name>_w (low: listed, not written)
#   (3) vtables: known vtables (vt_<Class>), vtables found through the C++ type info of library code, and the vtable a reflected class's factory takes (when it takes one; several are listed as low only); a slot referenced by that one table only -> <Class>_vf<slot>
#   (4) data-table callbacks: the callback of a callback-kind column in tables.json -> TxtCb_<table>_<column>
#   (5) client command packet handlers: $DM_ROOT/re/notes/c2s_labels.tsv (conclusions from reading each one)
#   Prints naming coverage (named functions / all functions) now and as expected after writing. Usage (called by re.sh autoname; the project must not be open in another process): autoname.py
import argparse
import collections
import json
import os
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parents[1] / 'verify'))
import ghidra_db  # noqa: E402
import nsoimg  # noqa: E402
import seed  # noqa: E402

AUTO = ghidra_db.RE / 'names_auto.tsv'
C2S = ghidra_db.RE / 'notes' / 'c2s_labels.tsv'
DEFAULT_NAME = re.compile(r'^(FUN|thunk_FUN|SUB)_[0-9a-fA-F]+$')
METHOD = re.compile(r'^(?:\[INTERNAL\] |-- )?([A-Za-z_]\w*)::(~?[A-Za-z_]\w*)')
SRCFILE = re.compile(r'[\\/]([\w.+-]+)\.(c|cc|cpp|h|hpp|inl)$')
LIBS = ('openssl', 'fmod', 'granny', 'protobuf', 'curl', 'zlib', 'websocket', 'havok', 'oodle')


NSO = ghidra_db.DM_ROOT / 'data/switch/3.3.94012/Update/ExeFS/main'
TEXT_END = 0x1b786a0


def relocated(base):
    """原映像施加重定位（同 reflect.py）→ (映像, {偏移: 指针值})；指针都来自重定位（位置无关的程序）。 | The original image with relocations applied (as reflect.py) -> (image, {offset: pointer value}); every pointer comes from a relocation (a position-independent program)."""
    img = bytearray(nsoimg.load(str(NSO))); ptrs = {}
    for o, typ, _name, val, add in nsoimg.relocs(img):
        if typ == 1027: v = base + add
        elif val is not None: v = base + val + (add if typ == 257 else 0)
        else: continue
        struct.pack_into('<Q', img, o, v); ptrs[o] = v
    return img, ptrs


def previous():
    """上一次的候选行（这些地址的名字是本工具给的，可以重新推）。 | The previous candidate rows (names at these addresses came from this tool and may be re-derived)."""
    if not AUTO.exists(): return {}
    out = {}
    for ln in AUTO.read_text(encoding='utf-8').splitlines():
        p = ln.split('\t')
        if len(p) >= 2 and p[0].startswith('0x'): out[int(p[0], 16)] = p[1]
    return out


def itanium(name):
    """简单的嵌套名（N…E 或 <长度><名字>）→ 用 _ 连起来；解析不了返回 None。 | A simple nested name (N...E or <length><name>) -> joined with _; None when it cannot be parsed."""
    s = name[1:-1] if name.startswith('N') and name.endswith('E') else name
    parts, i = [], 0
    while i < len(s):
        m = re.match(r'(\d+)', s[i:])
        if not m: return None
        n = int(m.group(1)); i += len(m.group(1)); parts.append(s[i:i + n]); i += n
    return '_'.join(p for p in parts if p and not p.startswith('_GLOBAL')) or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '10g'))
    a = ap.parse_args()
    ghidra_db.start(a.maxmem)
    import pyghidra
    from ghidra.program.model.symbol import RefType, SourceType
    prev = previous()
    pinned = {off for off, *_ in seed.names_table()}
    project = ghidra_db.open_project()
    rows, stats = {}, collections.Counter()
    with pyghidra.program_context(project, '/main.elf') as program:
        B = ghidra_db.BASE
        fm, listing, rm, mem = program.getFunctionManager(), program.getListing(), program.getReferenceManager(), program.getMemory()
        funcs = list(fm.getFunctions(True))
        def off(addr): return addr.getOffset() - B
        def free(f):   # 还没有名字（或名字是上次本工具给的） | not named yet (or named by this tool last time)
            o = off(f.getEntryPoint())
            return o not in pinned and (DEFAULT_NAME.match(f.getName()) or prev.get(o) == f.getName()) and f.getSymbol().getSource() != SourceType.USER_DEFINED
        named = {off(f.getEntryPoint()): f.getName() for f in funcs if not free(f)}
        unnamed = {off(f.getEntryPoint()) for f in funcs if DEFAULT_NAME.match(f.getName())}   # 现在还是缺省名的（上次本工具给的名字不算新增） | still default-named now (names this tool gave last time are not new)
        total, before = len(funcs), sum(1 for f in funcs if not DEFAULT_NAME.match(f.getName()))
        def propose(o, name, conf, src, why):
            if o in named or o in rows: return
            rows[o] = (name, conf, src, why); stats[f'{src} {conf}'] += 1
        def callers_of(addr): return {fm.getFunctionContaining(r.getFromAddress()) for r in rm.getReferencesTo(addr)} - {None}
        # ① 字符串 | (1) strings
        it = listing.getDefinedData(True)
        while it.hasNext():
            d = it.next()
            if not d.hasStringValue(): continue
            v = str(d.getValue() or '')
            fs = callers_of(d.getAddress())
            if len(fs) != 1: continue
            f = next(iter(fs))
            if not free(f): continue
            m = METHOD.match(v)
            if m: propose(off(f.getEntryPoint()), f'{m.group(1)}__{m.group(2).replace("~", "dtor_")}', 'medium', 'string', v[:60]); continue
            m = SRCFILE.search(v)
            if m:
                lib = next((x for x in LIBS if x in v.lower()), None) or Path(v.replace('\\', '/')).parent.name.lower() or 'src'
                propose(off(f.getEntryPoint()), f'{lib}_{m.group(1)}_{off(f.getEntryPoint()):x}', 'medium', 'srcfile', v[-60:])
        # ② 跳板与包装 | (2) thunks and wrappers
        for f in funcs:
            if not free(f) or f.isThunk(): continue
            ins = list(listing.getInstructions(f.getBody(), True))
            if not ins or len(ins) > 8: continue
            calls = [(i, i.getFlows()) for i in ins if i.getFlowType().isCall() or (i.getFlowType().isJump() and i.getFlowType().isUnConditional())]
            if len(ins) == 1 and calls and calls[0][1]:
                t = fm.getFunctionAt(calls[0][1][0])
                if t is not None and off(t.getEntryPoint()) in named: propose(off(f.getEntryPoint()), f'j_{t.getName()}', 'high', 'thunk', 'single b')
            elif len(calls) == 1 and calls[0][0].getFlowType().isCall() and calls[0][1]:
                t = fm.getFunctionAt(calls[0][1][0])
                if t is not None and off(t.getEntryPoint()) in named: propose(off(f.getEntryPoint()), f'{t.getName()}_w', 'low', 'wrapper', f'{len(ins)} instructions')
        # ③ 虚表：在施加了重定位的映像里看指针（工程里的数据引用还含展开表索引，数不准）；一个槽的函数在全映像只被这一处指针指着才起名
        # (3) vtables: pointers are read from the relocated image (the project's data references also count the unwind-table index); a slot's function is named only when this is the one pointer to it in the whole image
        img, ptrs = relocated(B)
        where = collections.defaultdict(list)
        for o, v in ptrs.items(): where[v].append(o)
        def slots(at, cls, why, conf='medium'):
            for k in range(512):
                v = ptrs.get(at + 8 * k)
                f = fm.getFunctionAt(ghidra_db.addr(program, v - B)) if v and B <= v < B + len(img) else None
                if f is None: break
                if free(f) and len(where[v]) == 1: propose(off(f.getEntryPoint()), f'{cls}_vf{k}', conf, 'vtable', why)
        st = program.getSymbolTable(); sit = st.getSymbolIterator(True)
        while sit.hasNext():
            sy = sit.next()
            if sy.getName().startswith('vt_') and not sy.getName().startswith('vt_Type_'): slots(off(sy.getAddress()), sy.getName()[3:], sy.getName())
        # 库代码的类型信息：名字串 ← 类型信息对象 +8 ← 虚表 {到顶偏移（0 或负数）, 类型信息, 槽…} | library type info: name string <- type-info object +8 <- vtable {offset to top (0 or negative), type info, slots...}
        for m in re.finditer(rb'(?<=\x00)N?(?:\d+[A-Za-z_]\w*)+E?(?=\x00)', bytes(img[TEXT_END:])):
            cls = itanium(m.group().decode())
            if not cls: continue
            for q in where.get(B + TEXT_END + m.start(), []):
                for v in where.get(B + q - 8, []):
                    top = struct.unpack_from('<q', img, v - 8)[0] if v >= 8 else 1
                    if v - 8 not in ptrs and -0x10000 < top <= 0: slots(v + 8, cls, f'type info {m.group().decode()[:40]}')
        # 引擎反射登记的类（游戏代码没有 C++ 类型信息）：工厂 <类>_Create（或它直接调的构造函数）取址的虚表就是这个类的；
        # 虚表 = {0, 0, 槽…}（关了类型信息），槽 0 是对象里存的那个地址。工厂与构造函数里取了几张虚表时取最后存的那张（子类在基类之后存）
        # Classes the engine registers by reflection (game code has no C++ type info): the vtable whose address the factory <Class>_Create (or the constructor it calls directly) takes belongs to that class;
        # vtable = {0, 0, slots...} (type info off), slot 0 is the address objects store. When the factory and constructor take several vtables, the last stored one wins (a subclass stores after its base)
        def isfn(v): return bool(v) and B <= v < B + TEXT_END and fm.getFunctionAt(ghidra_db.addr(program, v - B)) is not None
        vt0 = {o for o in ptrs if o - 8 not in ptrs and o - 16 not in ptrs and not any(img[o - 16:o]) and isfn(ptrs[o]) and isfn(ptrs.get(o + 8))}
        def vt_refs(fn):
            out = []
            for ins in listing.getInstructions(fn.getBody(), True):
                for r in ins.getReferencesFrom():
                    t = off(r.getToAddress())
                    if t in vt0: out.append((off(ins.getAddress()), t))
            return out
        for f in funcs:
            n = f.getName()
            if not n.endswith('_Create') or f.getSymbol().getSource() == SourceType.DEFAULT: continue
            cls = n[:-len('_Create')]
            hits = vt_refs(f)
            if not hits:
                for c in f.getCalledFunctions(None):
                    h = vt_refs(c)
                    if h: hits = h; break
            if not hits: continue
            vt, conf = max(hits)[1], 'medium' if len({t for _, t in hits}) == 1 else 'low'   # 几张里猜的那张：类名可能差一层，只列不写 | a guess among several: the class may be one level off, listed but not written
            propose(vt, f'vt_{cls}', conf, 'vtable', f'{n} takes {len(hits)} vtable address(es)')
            slots(vt, cls, f'{n}: last vtable it stores', conf)
        # ④ 数据表回调 | (4) data-table callbacks
        tj = ghidra_db.RE / 'export' / 'tables.json'
        if tj.exists():
            use = collections.defaultdict(list)
            for t in json.loads(tj.read_text(encoding='utf-8')).get('tables', []):
                for c in t.get('columns', []):
                    if c['kind'] in (26, 27, 28) and c['callback'].startswith('main+'): use[int(c['callback'][5:], 16)].append((t['name'], c['name']))
            for o, us in use.items():
                tbls = sorted({u[0] for u in us})
                f = fm.getFunctionAt(ghidra_db.addr(program, o))
                if f is None or not free(f): continue
                propose(o, f'TxtCb_{seed.ident(us[0][0])}_{seed.ident(us[0][1])}', 'high' if len(tbls) == 1 else 'medium', 'tables', f'{len(us)} column(s) in {", ".join(tbls[:3])}')
        # ⑤ 命令包处理函数 | (5) packet handlers
        if C2S.exists():
            for ln in C2S.read_text(encoding='utf-8').splitlines()[1:]:
                p = ln.split('\t')
                if len(p) < 5 or not p[1].startswith('main+') or not re.fullmatch(r'\w+', p[2]): continue
                o = int(p[1][5:], 16); f = fm.getFunctionAt(ghidra_db.addr(program, o))
                if f is not None and free(f): propose(o, p[2], p[3] if p[3] in ('high', 'medium') else 'low', 'c2s', f'opcode {p[0]}: {p[4]}'[:80])
    project.close()
    seen = collections.Counter()   # 同名加后缀 | suffix repeated names
    out = []
    for o in sorted(rows):
        n, c, s, w = rows[o]; seen[n] += 1
        out.append((o, n if seen[n] == 1 else f'{n}_{seen[n]}', c, s, w.replace('\t', ' ').replace('\n', ' ')))
    with open(AUTO, 'w', encoding='utf-8') as fo:
        fo.write('# 批量起名候选（autoname.py）：由游戏程序推出，只放本机，不入库 | bulk naming candidates (autoname.py): derived from the game program, local only, never committed\n')
        fo.write('offset\tname\tconfidence\tsource\tevidence\n')
        for o, n, c, s, w in out: fo.write(f'{o:#x}\t{n}\t{c}\t{s}\t{w}\n')
    applied = sum(1 for r in out if r[2] in ('high', 'medium') and r[0] in unnamed)
    for k in sorted(stats): print(f'  {k}: {stats[k]}')
    print(f'  coverage: {before}/{total} functions named ({100 * before / total:.1f}%) → {before + applied} ({100 * (before + applied) / total:.1f}%) once seed writes the high / medium rows')
    print('AUTONAME OK', AUTO)


if __name__ == '__main__':
    main()
