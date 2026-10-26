#!/usr/bin/env python3
# 客体（游戏程序）符号表：把「main+0x5b9290」这类偏移换成「它落在哪个函数里、那个函数叫什么 / 源码注释怎么说它」。
#   函数边界取自各模块的 .eh_frame（展开表的 FDE：起点 + 长度），名字取自动态符号表（sdk 等模块导出的名字），说明取自源码注释里提到「模块+0x偏移」的那几行。
#   查名字时还看引擎分析库的导出 $DM_ROOT/re/export/<模块>.names.tsv（dm/tools/re/export.py；有就用，没有照旧）：动态符号表没有名字的函数用它的名字，可信度不是 high 的在名字后标出。
#   build [--product 产物目录]   生成 $DM_ROOT/build/cache/dm/symbols/<产物名>.tsv（由游戏程序推出来的数据：只放本机缓存，不入库）
#   lookup <模块+0x偏移>…        打印每个地址所在的函数、函数内偏移、名字与注释
#   作为模块：lookup_many([...]) → {地址: 说明}（crashlog.py 用）
# Guest (game program) symbol table: turns an offset like "main+0x5b9290" into "which function it falls in, what that function is called / what the source comments say about it".
#   Function boundaries come from each module's .eh_frame (the unwind table's FDEs: start + length), names from the dynamic symbol table (the names exported by sdk and other modules), notes from the source-comment lines that mention "module+0xoffset".
#   Lookups also read the engine-analysis database's export $DM_ROOT/re/export/<module>.names.tsv (dm/tools/re/export.py; used when present, otherwise nothing changes): a function with no dynamic-symbol name takes its name from there, marked with its confidence unless that is high.
#   build [--product build output dir]   generate $DM_ROOT/build/cache/dm/symbols/<product name>.tsv (data derived from the game program: kept in the local cache only, never committed)
#   lookup <module+0xoffset>...          print, for each address, the function it falls in, the offset into it, the name and the notes
#   As a module: lookup_many([...]) -> {address: description} (used by crashlog.py)
import argparse
import bisect
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
MODULES = ('main', 'sdk', 'subsdk0')
MENTION = re.compile(r'\b(main|sdk|subsdk0)\+0x([0-9a-f]{3,})\b')


def product_dir(p=None):
    return p or subprocess.run(['bash', os.path.join(HERE, 'product.sh'), 'path', 'touch'], capture_output=True, text=True, check=False).stdout.strip()


def table_path(prod):
    return os.path.join(DM_ROOT, 'build', 'cache', 'dm', 'symbols', os.path.basename(prod.rstrip('/')) + '.tsv')


def re_names_path(mod):
    return os.path.join(DM_ROOT, 're', 'export', f'{mod}.names.tsv')


_RE_NAMES = None


def re_names():
    """{模块: {函数起点: (名字, 可信度)}}：引擎分析库导出的函数名（只取 kind = function）。 | {module: {function start: (name, confidence)}}: function names exported from the engine-analysis database (kind = function only)."""
    global _RE_NAMES   # noqa: PLW0603 -- 进程内只读一次 | read once per process
    if _RE_NAMES is not None: return _RE_NAMES
    _RE_NAMES = {}
    for mod in MODULES:
        p = re_names_path(mod)
        if not os.path.exists(p): continue
        with open(p, encoding='utf-8') as f:
            for line in f:
                if line.startswith(('#', 'offset\t')): continue
                c = line.rstrip('\n').split('\t')
                if len(c) >= 4 and c[2] == 'function': _RE_NAMES.setdefault(mod, {})[int(c[0], 16)] = (c[1], c[3])
    return _RE_NAMES


def functions(img):
    """(起点, 长度) 列表：.eh_frame_hdr 的查找表给 FDE 地址，FDE 里读 pc_begin / pc_range（AArch64 缺省编码：pcrel / datarel sdata4）。 | List of (start, length): the .eh_frame_hdr lookup table gives the FDE addresses, pc_begin / pc_range are read from each FDE (default AArch64 encodings: pcrel / datarel sdata4)."""
    m = struct.unpack_from('<I', img, 4)[0]
    if img[m:m + 4] != b'MOD0': raise ValueError('no MOD0 header')
    ehs = m + struct.unpack_from('<i', img, m + 16)[0]
    ver, ptrenc, cntenc, tblenc = img[ehs:ehs + 4]
    if (ver, ptrenc, cntenc, tblenc) != (1, 0x1b, 0x03, 0x3b): raise ValueError(f'unexpected eh_frame_hdr encodings {img[ehs:ehs + 4].hex()}')
    n = struct.unpack_from('<I', img, ehs + 8)[0]; out = []
    for i in range(n):
        start, fde = (ehs + x for x in struct.unpack_from('<ii', img, ehs + 12 + 8 * i))
        ln = struct.unpack_from('<I', img, fde)[0]
        if ln == 0xffffffff: continue   # 64 位长度的 FDE：AArch64 上不会出现 | 64-bit length FDEs do not occur on AArch64
        rng = struct.unpack_from('<I', img, fde + 12)[0]
        out.append((start, rng))
    return sorted(set(out))


def dynsyms(img):
    """动态符号表里有定义的函数：{起点: 名字}。 | Defined functions in the dynamic symbol table: {start: name}."""
    m = struct.unpack_from('<I', img, 4)[0]; dyn = m + struct.unpack_from('<i', img, m + 4)[0]
    tags = {}
    for o in range(dyn, min(len(img), dyn + 16 * 512), 16):
        t, v = struct.unpack_from('<qQ', img, o)
        if t == 0: break
        tags.setdefault(t, v)
    symtab, strtab, strsz = tags.get(6), tags.get(5), tags.get(10, 0)
    if not symtab or not strtab: return {}
    count = struct.unpack_from('<I', img, tags[4] + 4)[0] if 4 in tags else (strtab - symtab) // 24
    out = {}
    for i in range(count):
        nm, info, _other, shndx, value, _size = struct.unpack_from('<IBBHQQ', img, symtab + 24 * i)
        if shndx and value and (info & 0xf) == 2 and nm < strsz:
            end = img.index(b'\0', strtab + nm)
            out.setdefault(value, img[strtab + nm:end].decode('utf-8', 'replace'))
    return out


def comment_notes():
    """源码注释里提到「模块+0x偏移」的行：{(模块, 偏移): [「文件:行 前文」…]}（只取注释行：// 或 # 起的那段）。 | Source-comment lines mentioning "module+0xoffset": {(module, offset): ["file:line text before it", ...]} (comment parts only: from // or #)."""
    notes = {}
    out = subprocess.run(['git', '-C', DM, 'grep', '-nE', r'(main|sdk|subsdk0)\+0x[0-9a-f]{3,}', '--', 'host', 'gfx', 'ios/Sources', 'tools'], capture_output=True, text=True, check=False).stdout
    for line in out.splitlines():
        try: f, n, text = line.split(':', 2)
        except ValueError: continue
        c = text.find('//') if not f.endswith('.py') else text.find('#')
        if c < 0: continue
        com = text[c:].split(' | ')[0]   # 双语注释取中文那半 | of a bilingual comment, the Chinese half
        for mm in MENTION.finditer(com):
            before = com[2:mm.start()].strip(' ：:，,（(') [-40:]
            notes.setdefault((mm.group(1), int(mm.group(2), 16)), []).append(f'{f}:{n} {before}'.strip())
    return notes


def demangle(names):
    """C++ 名字还原（xcrun c++filt，一次一批）；不可用时原样。 | Demangle C++ names (xcrun c++filt, one batch); unchanged when unavailable."""
    if not names: return {}
    try: out = subprocess.run(['xcrun', 'c++filt'], input='\n'.join(names) + '\n', capture_output=True, text=True, timeout=120, check=False).stdout.splitlines()
    except (OSError, subprocess.SubprocessError): return {}
    return dict(zip(names, out, strict=False)) if len(out) == len(names) else {}


def build(prod):
    rows = []
    notes = comment_notes()
    for mod in MODULES:
        p = os.path.join(prod, f'{mod}.patched.bin')
        if not os.path.exists(p): continue
        with open(p, 'rb') as f: img = f.read()
        fns = functions(img); names = dynsyms(img); starts = [s for s, _ in fns]
        fnotes = {}
        for (m2, off), ns in notes.items():
            if m2 != mod: continue
            i = bisect.bisect_right(starts, off) - 1
            if i >= 0 and off < fns[i][0] + fns[i][1]: fnotes.setdefault(fns[i][0], []).extend(f'+{off - fns[i][0]:#x} {x}' for x in ns)
        dem = demangle(sorted(set(names.values())))
        for s, ln in fns: rows.append((mod, s, ln, dem.get(names.get(s, ''), names.get(s, '')), ' | '.join(fnotes.get(s, [])[:4])))
    out = table_path(prod); os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('# 由游戏程序推出的符号表：只放本机缓存，不入库 | symbol table derived from the game program: local cache only, never committed\nmodule\tstart\tsize\tname\tnotes\n')
        for r in rows: f.write(f'{r[0]}\t{r[1]:#x}\t{r[2]:#x}\t{r[3]}\t{r[4]}\n')
    return out, rows


_TABLE = None


def load(prod=None):
    global _TABLE   # noqa: PLW0603 -- 进程内只读一次 | read once per process
    if _TABLE is not None: return _TABLE
    path = table_path(product_dir(prod)); _TABLE = {}
    if not os.path.exists(path): return _TABLE
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith(('#', 'module\t')): continue
            p = line.rstrip('\n').split('\t')
            if len(p) == 5: _TABLE.setdefault(p[0], []).append((int(p[1], 16), int(p[2], 16), p[3], p[4]))
    for v in _TABLE.values(): v.sort()
    return _TABLE


def lookup(where):
    m = re.fullmatch(r'(\w+)\+0x([0-9a-f]+)', where or '')
    if not m: return ''
    fns = load().get(m.group(1), [])
    off = int(m.group(2), 16); i = bisect.bisect_right(fns, (off, 1 << 62)) - 1
    if i < 0 or off >= fns[i][0] + fns[i][1]: return ''
    s, _, name, notes = fns[i]
    if not name and (rn := re_names().get(m.group(1), {}).get(s)):
        name = rn[0] + ('' if rn[1] == 'high' else f' ({rn[1]})')
    label = name or f'fn_{s:x}'
    return f'{label}+{off - s:#x}' + (f'  [{notes}]' if notes else '')


def lookup_many(wheres):
    return {w: x for w in wheres if (x := lookup(w))}


def main(argv):
    ap = argparse.ArgumentParser(prog='guestsyms.py', description='Guest symbol table from .eh_frame, the dynamic symbols and source comments.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build'); b.add_argument('--product')
    lk = sub.add_parser('lookup'); lk.add_argument('where', nargs='+')
    a = ap.parse_args(argv)
    if a.cmd == 'build':
        prod = product_dir(a.product)
        if not prod or not os.path.isdir(prod): print('没有改写产物：先跑 ./setup.sh | no rewrite output: run ./setup.sh first'); return 1
        out, rows = build(prod)
        by = {m: sum(1 for r in rows if r[0] == m) for m in MODULES}
        named = sum(1 for r in rows if r[3]); noted = sum(1 for r in rows if r[4])
        print(f'GUESTSYMS OK 函数 | functions {by}，有名字 | named {named}，有注释 | with notes {noted} → {out}')
        return 0
    for w in a.where: print(f'{w}  {lookup(w) or "（不在任何函数里 / 没有符号表：先 build）| (not in any function / no table: run build)"}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
