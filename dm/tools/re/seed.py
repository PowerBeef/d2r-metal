#!/usr/bin/env python3
# 把仓库里已知的东西写进引擎分析库（Ghidra 工程），可反复跑：同一结果不重复写，手工起的名字（USER_DEFINED）一律不动。
#   ① NVN：dm/gfx/nvn/nvn_slots.tsv 的每个取址槽 → 标签 pfn_<名字>，类型 = nvn.h 里对应的函数指针（解析不了就用普通指针）。
#   ② 源码注释：仓库里提到 main+0x… 的注释行 → 该地址的前置注释，以「[dm]」开头，每次整段替换。
#   ③ 改写规则：产物里的 main.cuts / obs / params.tsv → 书签（类别 dm-rewrite），提醒这里的原代码在运行时被换掉或被观察。
#   ④ 已知布局：dm/host/dm_widget.h 的控件虚表 → 标签 vt_<类>，控件基类与各类专有字段、dm_unit.h 与 dm/verify/tick_state_format.md 的单位链 → 结构体（类别 /dm）。
#   ⑥ 反射表 $DM_ROOT/re/export/reflect.json（reflect.py）：每个类一个结构体（/dm/reflect，含父类字段，字段名 = 布局 JSON 里的名字），
#      类型描述符 type_<类>、类型对象虚表 vt_Type_<类>、工厂 <类>_Create、只登记一个类的取类型函数 <类>_GetType；
#      消息频道与各频道动作做成 8 字节枚举（值 = 名字的 FNV-1a 64），$DM_ROOT/re/enums.tsv 里的枚举（如 ControllerAction）做成 4 字节枚举。
#   ⑦ 数据表 $DM_ROOT/re/export/tables.json（tables.py）：每张表一个记录结构体 txt_<表>（/dm/tables，字段 = 列名，标志位并成一个 u32 并在注释里列位），
#      表根对象结构体 DataTables（每张表的记录指针与行数），三个表根指针（[main+0x2d64a18 + 版本 × 16]，版本 1–3）标成 g_tables_v1..v3 并带类型。
#   ⑧ 批量起名 $DM_ROOT/re/names_auto.tsv（autoname.py）里 high / medium 的行，排在 names.tsv 之后（names.tsv 的地址与手工名不碰）。
#   用法（由 re.sh seed 调用；工程不能被别的进程打开）：seed.py [--dry-run]
# Writes what the repository already knows into the engine-analysis database (the Ghidra project); safe to rerun: the same result is not written twice, and hand-given names (USER_DEFINED) are never touched.
#   ① NVN: every lookup slot in dm/gfx/nvn/nvn_slots.tsv -> label pfn_<name>, typed as the matching function pointer from nvn.h (a plain pointer when it cannot be parsed).
#   ② Source comments: comment lines in the repo that mention main+0x... -> a pre-comment at that address starting with "[dm]", replaced as a whole on each run.
#   ③ Rewrite rules: main.cuts / obs / params.tsv from the build output -> bookmarks (category dm-rewrite), a reminder that the original code here is replaced or observed at run time.
#   ④ Known layouts: the widget vtables in dm/host/dm_widget.h -> labels vt_<class>; the widget base class and per-class fields, and the unit chain from dm_unit.h and dm/verify/tick_state_format.md -> structures (category /dm).
#   ⑤ The name table $DM_ROOT/re/names.tsv (offset name confidence source [evidence]): renames the function at a function start, adds a label elsewhere; the confidence goes into a comment. The optional evidence column (static | derived | probed | observed-mac | observed-iphone) only reaches the export.
#   ⑥ The reflection table $DM_ROOT/re/export/reflect.json (reflect.py): one structure per class (/dm/reflect, parent fields included, field names = the names in layout JSON),
#      type descriptors type_<Class>, type-object vtables vt_Type_<Class>, factories <Class>_Create, getters that register a single class <Class>_GetType;
#      message channels and each channel's actions become 8-byte enums (value = FNV-1a 64 of the name), the enums in $DM_ROOT/re/enums.tsv (e.g. ControllerAction) 4-byte enums.
#   ⑦ The data tables $DM_ROOT/re/export/tables.json (tables.py): one record structure txt_<table> per table (/dm/tables, fields = column names, flag bits merged into a u32 listed in its comment),
#      the table root structure DataTables (each table's record pointer and row count), and the three root pointers ([main+0x2d64a18 + version × 16], versions 1-3) named g_tables_v1..v3 and typed.
#   (8) The high / medium rows of the bulk names $DM_ROOT/re/names_auto.tsv (autoname.py), after names.tsv (addresses in names.tsv and hand names are left alone).
#   Afterwards it adds the names it wrote or found in place to log/seed_names.tsv (offset, name, kind; kept across runs) and stamps the inputs it read
#   in log/stamps.json (dbprofile.py), so the export knows where each name came from and which inputs the database saw.
#   Usage (called by re.sh seed; the project must not be open in another process): seed.py [--dry-run]
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DM = HERE.parents[2]          # 仓库根 | repository root
sys.path.insert(0, str(HERE))
import dbprofile  # noqa: E402
import ghidra_db  # noqa: E402

MENTION = re.compile(r'\bmain\+0x([0-9a-f]{3,})\b')
PRODUCT_OUT = ghidra_db.DM_ROOT / 'build/cache/dm/out'
NAMES = ghidra_db.RE / 'names.tsv'
REFLECT = ghidra_db.RE / 'export' / 'reflect.json'
TABLES = ghidra_db.RE / 'export' / 'tables.json'
ENUMS = ghidra_db.RE / 'enums.tsv'
TAG = '[dm] '

# 控件：基类字段（所有控件共有）与各类专有字段，(偏移, 名字, 类型名)；类型名用 Ghidra 内建类型
# Widgets: base-class fields (shared by all widgets) and per-class fields, (offset, name, type name); type names are Ghidra built-ins
WIDGET_BASE = [(0x0, 'vtable', 'pointer'), (0x8, 'name', 'char *'), (0x50, 'visible', 'byte'), (0x51, 'visible2', 'byte'),
               (0x58, 'children', 'pointer'), (0x60, 'child_count', 'qword'), (0x70, 'rect', 'float[4]')]
WIDGETS = {   # 类名 → (dm_widget.h 的虚表常量名, 专有字段) | class name -> (vtable constant in dm_widget.h, own fields)
    'TextBoxWidget': ('DM_VT_TEXT', [(0x88, 'text', 'char *'), (0x90, 'text_len', 'qword'), (0xbc, 'color', 'dword')]),
    'InputTextWidget': ('DM_VT_INPUT', [(0x558, 'committed_text', 'char *')]),
    'NumericInputTextBoxWidget': ('DM_VT_NUMINPUT', []),
    'ToggleButtonWidget': ('DM_VT_TOGGLE', [(0x610, 'cur_frame', 'dword'), (0x6e8, 'frames', 'dword[4]'), (0xa88, 'checked', 'byte')]),
    'TabBarWidget': ('DM_VT_TABBAR', []),
    'ItemSlotWidget': ('DM_VT_SLOT', [(0x5c8, 'owner', 'pointer'), (0x5d8, 'body_loc', 'dword')]),
    'ItemSlotWidgetB': ('DM_VT_SLOT_B', []),
    'ItemSlotWidgetC': ('DM_VT_SLOT_C', []),
    'ImageWidget': ('DM_VT_IMAGE', [(0x90, 'cur_frame', 'dword')]),
    'CarouselWidget': ('DM_VT_CAROUSEL', [(0x7d8, 'choosable', 'byte[16]'), (0x7e8, 'count', 'dword'), (0x7ec, 'current', 'dword')]),
    'ButtonWidget': ('DM_VT_BUTTON', [(0x610, 'cur_frame', 'dword'), (0x6e8, 'frames', 'dword[4]')]),
}
# 单位链（dm_unit.h、tick_state_format.md；字段名借经典 D2 的叫法）；游戏对象 G、物品栏、物品数据（dm/host/diag.c 的读法注释）
# Unit chain (dm_unit.h, tick_state_format.md; field names borrowed from classic D2); the game object G, inventory and item data (the reading notes in dm/host/diag.c)
UNIT_TYPES = {
    'Unit': [(0x0, 'type', 'dword'), (0x4, 'class_id', 'dword'), (0x8, 'unit_id', 'dword'), (0xc, 'mode', 'dword'),
             (0x10, 'type_data', 'pointer'), (0x18, 'act', 'byte'), (0x28, 'seed_lo', 'dword'), (0x2c, 'seed_hi', 'dword'),
             (0x38, 'path', 'Path *'), (0x88, 'stats', 'StatList *'), (0x90, 'inventory', 'Inventory *'), (0x158, 'hash_next', 'Unit *')],
    'Game': [(0x104, 'difficulty', 'byte'), (0x106, 'data_version', 'byte'), (0x170, 'tick', 'dword'), (0x1a8, 'seed', 'qword'),
             (0x2230, 'players', 'Unit *[128]'), (0x2630, 'monsters', 'Unit *[128]'), (0x2a30, 'objects', 'Unit *[128]'),
             (0x2e30, 'items', 'Unit *[128]'), (0x3230, 'missiles', 'Unit *[128]')],
    'Inventory': [(0x0, 'signature', 'dword'), (0x10, 'first_item', 'Unit *'), (0x40, 'cursor_item', 'Unit *')],
    'ItemData': [(0x0, 'quality', 'dword'), (0x18, 'flags', 'dword'), (0x55, 'page', 'byte'), (0xb0, 'next_item', 'Unit *')],
    'Path': [(0x0, 'x_frac', 'word'), (0x2, 'x', 'word'), (0x4, 'y_frac', 'word'), (0x6, 'y', 'word'), (0x20, 'room', 'Room *')],
    'Room': [(0x18, 'room_ex', 'RoomEx *'), (0x48, 'next', 'Room *')],
    'RoomEx': [(0x90, 'level', 'Level *')],
    'Level': [(0x10, 'first_room', 'Room *'), (0x1f8, 'level_id', 'dword')],
    'StatList': [(0x30, 'stats', 'pointer'), (0x38, 'count', 'dword')],
}


def vt_offsets():
    """dm_widget.h 里 DM_VT_* 的值。 | The DM_VT_* values from dm_widget.h."""
    text = (DM / 'dm/host/dm_widget.h').read_text(encoding='utf-8')
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(r'(DM_VT_\w+)\s*=\s*0x([0-9a-f]+)', text)}


def source_comments():
    """{偏移: [「文件:行 文字」…]}：仓库注释里提到 main+0x… 的行（整行注释文字，最长 240 字）。 | {offset: ["file:line text", ...]}: repo comment lines that mention main+0x... (the whole comment text, at most 240 chars)."""
    out = subprocess.run(['git', '-C', str(DM), 'grep', '-nE', r'main\+0x[0-9a-f]{3,}', '--', 'dm', ':!dm/third_party'],
                         capture_output=True, text=True, check=False).stdout
    notes = {}
    for line in out.splitlines():
        try: f, n, text = line.split(':', 2)
        except ValueError: continue
        if f.endswith(('.md', '.tsv', '.json')): c = 0
        else:
            c = text.find('#') if f.endswith(('.py', '.sh')) else text.find('//')
            if c < 0: continue
        com = text[c:].lstrip('/#! ').strip()
        for m in MENTION.finditer(com):
            row = f'{f}:{n} {com[:240]}'
            lst = notes.setdefault(int(m.group(1), 16), [])
            if row not in lst: lst.append(row)
    return notes


def product_dir():
    p = subprocess.run(['bash', str(DM / 'dm/tools/product.sh'), 'path', 'touch'], capture_output=True, text=True, check=False).stdout.strip()
    return Path(p) if p else None


def rewrite_rules():
    """[(偏移, 种类, 说明)]：产物里 main 的剪口 / 观察门 / 宿主填值。 | [(offset, kind, description)]: main's cuts / observation gates / host-filled values in the build output."""
    prod, rows = product_dir(), []
    if not prod: return rows
    for kind, col in (('cut', 4), ('obs', 3), ('param', 4)):
        p = prod / f'main.{kind}s.tsv'
        if not p.exists(): continue
        for line in p.read_text(encoding='utf-8').splitlines():
            if line.startswith('#') or not line.strip(): continue
            parts = line.split('\t')
            try: rows.append((int(parts[0], 16), kind, parts[col] if len(parts) > col else ''))
            except ValueError: continue
    return rows


def names_table():
    """[(偏移, 名字, 可信度, 来源)]：$DM_ROOT/re/names.tsv（# 起的行是注释）。 | [(offset, name, confidence, source)]: $DM_ROOT/re/names.tsv (lines starting with # are comments)."""
    if not NAMES.exists(): return []
    rows = []
    for line in NAMES.read_text(encoding='utf-8').splitlines():
        if line.startswith('#') or not line.strip(): continue
        p = line.split('\t') + ['', '', '']
        rows.append((int(p[0].replace('main+', ''), 16), p[1].strip(), p[2].strip() or 'medium', p[3].strip()))
    return rows


EVIDENCE = ('static', 'derived', 'probed', 'observed-mac', 'observed-iphone')


def names_evidence():
    """{offset: evidence} from names.tsv's optional fifth column: the kind of proof behind a name (one of EVIDENCE; blank = static)."""
    out = {}
    for line in NAMES.read_text(encoding='utf-8').splitlines() if NAMES.exists() else []:
        if line.startswith('#') or not line.strip(): continue
        p = line.split('\t')
        if len(p) > 4 and p[4].strip() in EVIDENCE: out[int(p[0].replace('main+', ''), 16)] = p[4].strip()
        elif len(p) > 4 and p[4].strip(): print(f'  names.tsv: unknown evidence {p[4].strip()!r} at {p[0]} (taken as static; one of {", ".join(EVIDENCE)})', file=sys.stderr)
    return out


LEDGER = ghidra_db.RE / 'log' / 'seed_names.tsv'


def seeded_names():
    """{(offset, name): kind} of every name seed has written or found already in place, across runs (log/seed_names.tsv).
    kind: nvn, widget vtable, reflect, tables, names.tsv, autoname. The export reads it to tell reflection, table and bulk names
    (derived by our tooling) from loader and repo names, and to spot a bulk name whose row was later dropped."""
    out = {}
    for line in LEDGER.read_text(encoding='utf-8').splitlines() if LEDGER.exists() else []:
        p = line.split('\t')
        if len(p) == 3 and p[0].startswith('0x'): out[(int(p[0], 16), p[1])] = p[2]
    return out


def save_ledger(named):
    """Adds this run's [(offset, name, kind)] to the ledger (a later kind for the same offset and name wins)."""
    rows = seeded_names() | {(off, name): kind for off, name, kind in named}
    LEDGER.parent.mkdir(parents=True, exist_ok=True)
    LEDGER.write_text('# names seed.py wrote into the database (offset, name, kind); local only\n'
                      + ''.join(f'{off:#x}\t{name}\t{kind}\n' for (off, name), kind in sorted(rows.items())), encoding='utf-8')


class Seeder:
    def __init__(self, program, dry):
        from ghidra.program.model.symbol import SourceType
        self.p, self.dry, self.ST = program, dry, SourceType
        self.stats, self.skip, self.named = {}, set(), []

    def bump(self, k): self.stats[k] = self.stats.get(k, 0) + 1

    def a(self, off): return ghidra_db.addr(self.p, off)

    def name(self, off, name, kind):
        """函数起点改函数名，否则加主标签；手工名字不动，同名跳过。 | Renames the function at a function start, otherwise adds a primary label; hand-given names are left alone, the same name is skipped."""
        if off in self.skip: return
        st = self.p.getSymbolTable(); at = self.a(off)
        f = self.p.getFunctionManager().getFunctionAt(at)
        cur = f.getSymbol() if f else st.getPrimarySymbol(at)
        if cur is not None and cur.getName() == name: self.named.append((off, name, kind)); return
        if cur is not None and cur.getSource() == self.ST.USER_DEFINED: self.bump(f'{kind}: kept hand name'); return
        if self.dry: self.bump(f'{kind}: would name'); return
        self.named.append((off, name, kind))
        if f: f.setName(name, self.ST.IMPORTED)
        else: st.createLabel(at, name, self.ST.IMPORTED).setPrimary()
        self.bump(f'{kind}: named')

    def comment(self, off, text):
        from ghidra.program.model.listing import CodeUnit
        lst = self.p.getListing(); at = self.a(off)
        old = lst.getComment(CodeUnit.PRE_COMMENT, at) or ''
        keep = '\n'.join(x for x in old.split('\n') if not x.startswith(TAG))
        new = '\n'.join(filter(None, [keep, '\n'.join(TAG + x for x in text)]))
        if new == old: return
        if not self.dry: lst.setComment(at, CodeUnit.PRE_COMMENT, new)
        self.bump('comments set')

    def bookmark(self, off, kind, text):
        from ghidra.program.model.listing import BookmarkType
        bm = self.p.getBookmarkManager(); at = self.a(off)
        if bm.getBookmark(at, BookmarkType.NOTE, 'dm-rewrite') is not None: return
        if not self.dry: bm.setBookmark(at, BookmarkType.NOTE, 'dm-rewrite', f'{kind}: {text[:300]}')
        self.bump('bookmarks set')

    def structs(self):
        from ghidra.program.model.data import CategoryPath, StructureDataType, PointerDataType, DataTypeConflictHandler
        from ghidra.app.util.cparser.C import CParserUtils  # noqa: F401 -- 只为确认解析器可用 | only to confirm the parser is available
        dtm = self.p.getDataTypeManager(); cat = CategoryPath('/dm')
        def resolve(tname):
            from ghidra.util.data import DataTypeParser
            from ghidra.program.model.data import DataTypeManager  # noqa: F401
            if tname.endswith('*') and tname[:-1].strip() in UNIT_TYPES:
                base = dtm.getDataType(cat, tname[:-1].strip())
                return PointerDataType(base, dtm)
            m = re.fullmatch(r'(\w+ \*)\[(\d+)\]', tname)   # 单位指针数组（单位哈希表） | arrays of unit pointers (the unit hash tables)
            if m and m.group(1)[:-1].strip() in UNIT_TYPES:
                from ghidra.program.model.data import ArrayDataType
                el = resolve(m.group(1)); return ArrayDataType(el, int(m.group(2)), el.getLength())
            return DataTypeParser(dtm, dtm, None, DataTypeParser.AllowedDataTypes.ALL).parse(tname)
        def define(name, fields, size=None):
            s = StructureDataType(cat, name, 0, dtm)
            end = max(o + resolve(t).getLength() for o, _, t in fields)
            s.growStructure(max(end, size or 0))
            for o, fname, t in fields:
                dt = resolve(t); s.replaceAtOffset(o, dt, dt.getLength(), fname, 'dm')
            return s
        changed = 0
        for name in UNIT_TYPES:   # 先占位，指针才解析得到 | placeholders first so the pointers resolve
            if dtm.getDataType(cat, name) is None and not self.dry: dtm.addDataType(StructureDataType(cat, name, 0, dtm), DataTypeConflictHandler.KEEP_HANDLER)
        defs = [(n, f, None) for n, f in UNIT_TYPES.items()]
        base_end = max(o + 8 for o, _, _ in WIDGET_BASE) + 0x10
        defs.append(('Widget', WIDGET_BASE, base_end))
        for cls, (_, own) in WIDGETS.items(): defs.append((cls, WIDGET_BASE + own, base_end))
        for name, fields, size in defs:
            if self.dry: continue
            new = define(name, fields, size)
            old = dtm.getDataType(cat, name)
            if old is not None and old.isEquivalent(new): continue
            dtm.addDataType(new, DataTypeConflictHandler.REPLACE_HANDLER); changed += 1
        self.stats['structures changed'] = changed

    def nvn(self):
        from ghidra.program.model.data import PointerDataType
        from ghidra.program.model.data import DataUtilities
        protos = self.nvn_protos()
        for line in (DM / 'dm/gfx/nvn/nvn_slots.tsv').read_text(encoding='utf-8').splitlines():
            if line.startswith(('#', 'name\t')) or not line.strip(): continue
            name, slot = line.split('\t')[:2]
            off = int(slot, 16)
            self.name(off, f'pfn_{name}', 'nvn')
            dt = protos.get(name)
            if dt is None or self.dry: continue
            cur = self.p.getListing().getDataAt(self.a(off))
            ptr = PointerDataType(dt, self.p.getDataTypeManager())
            if cur is not None and cur.getDataType().isEquivalent(ptr): continue
            DataUtilities.createData(self.p, self.a(off), ptr, -1, DataUtilities.ClearDataMode.CLEAR_ALL_CONFLICT_DATA)
            self.bump('nvn: typed')

    def nvn_protos(self):
        """nvn.h → {nvn 函数名: 函数定义类型}；解析失败返回 {}（槽仍有名字，只是不带参数类型）。 | nvn.h -> {nvn function name: function definition type}; returns {} on a parse failure (the slots still get names, just without parameter types)."""
        try:
            from ghidra.app.util.cparser.C import CParserUtils
            from ghidra.program.model.data import FunctionDefinition, Pointer
            from ghidra.util.task import TaskMonitor
            dtm = self.p.getDataTypeManager()
            # Ghidra 的 C 解析器找不到系统头：去掉 #include，前面补上 nvn.h 用到的标准类型（LP64） | Ghidra's C parser cannot find system headers: drop the #includes and prepend the standard types nvn.h uses (LP64)
            pre = ''.join(f'typedef {c} {t};\n' for t, c in (('int8_t', 'signed char'), ('uint8_t', 'unsigned char'), ('int16_t', 'short'),
                          ('uint16_t', 'unsigned short'), ('int32_t', 'int'), ('uint32_t', 'unsigned int'), ('int64_t', 'long'), ('uint64_t', 'unsigned long'),
                          ('size_t', 'unsigned long'), ('ptrdiff_t', 'long'), ('intptr_t', 'long'), ('uintptr_t', 'unsigned long')))
            body = re.sub(r'^\s*#\s*include\s*<[^>]+>.*$', '', (DM / 'dm/gfx/nvn/include/nvn.h').read_text(encoding='utf-8'), flags=re.M)
            hdr = ghidra_db.RE / 'log' / 'nvn_for_ghidra.h'
            hdr.write_text(pre + body, encoding='utf-8')
            res = CParserUtils.parseHeaderFiles([], [str(hdr)], ['-D__aarch64__', '-D_LP64'], dtm, TaskMonitor.DUMMY)
            if not res.successful(): self.stats['nvn.h parse'] = 'failed'; return {}
            out = {}
            it = dtm.getAllDataTypes()
            while it.hasNext():
                dt = it.next()
                m = re.fullmatch(r'PFN(NVN\w+)PROC', dt.getName())
                if not m: continue
                base = dt.getDataType() if hasattr(dt, 'getDataType') else dt
                tgt = base.getDataType() if isinstance(base, Pointer) else base
                if isinstance(tgt, FunctionDefinition): out[m.group(1).lower()] = tgt
            self.stats['nvn.h prototypes'] = len(out)
            # nvn_slots.tsv 用驼峰名，nvn.h 的类型名是全大写：按小写对上 | nvn_slots.tsv uses camel-case names, nvn.h type names are upper case: match in lower case
            return {n: out[n.lower()] for n in self.slot_names() if n.lower() in out}
        except Exception as e:   # noqa: BLE001 -- 解析器异常种类多，失败不挡其余步骤 | the parser throws many kinds of errors; a failure must not block the other steps
            self.stats['nvn.h parse'] = f'failed: {e}'[:200]
            return {}

    def slot_names(self):
        return [ln.split('\t')[0] for ln in (DM / 'dm/gfx/nvn/nvn_slots.tsv').read_text(encoding='utf-8').splitlines()
                if ln.strip() and not ln.startswith(('#', 'name\t'))]


def auto_names():
    """$DM_ROOT/re/names_auto.tsv（autoname.py）→ [(偏移, 名字, 可信度, 来源, 依据)]。 | $DM_ROOT/re/names_auto.tsv (autoname.py) -> [(offset, name, confidence, source, evidence)]."""
    f = ghidra_db.RE / 'names_auto.tsv'
    if not f.exists(): return []
    out = []
    for line in f.read_text(encoding='utf-8').splitlines():
        p = line.split('\t')
        if len(p) >= 5 and p[0].startswith('0x'): out.append((int(p[0], 16), p[1], p[2], p[3], p[4]))
    return out


def fnv1a64(s):
    h = 0xcbf29ce484222325
    for b in s.encode(): h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def ident(name):
    """类名 → 标识符（A::B → A__B，其余非字母数字换成 _）。 | Class name -> identifier (A::B -> A__B, other non-alphanumerics become _)."""
    return ''.join(c if c.isalnum() or c == '_' else '_' for c in (name or '').replace('::', '__'))


def reflect_classes():
    if not REFLECT.exists(): return [], []
    import json
    d = json.loads(REFLECT.read_text(encoding='utf-8'))
    return d.get('classes', []), d.get('enums', [])


def enum_rows():
    """{枚举名: [(名字, 值)]}，取自 $DM_ROOT/re/enums.tsv。 | {enum name: [(name, value)]} from $DM_ROOT/re/enums.tsv."""
    out = {}
    if not ENUMS.exists(): return out
    for line in ENUMS.read_text(encoding='utf-8').splitlines():
        if line.startswith('#') or not line.strip(): continue
        e, n, v = line.split('\t')[:3]
        out.setdefault(e, []).append((n, int(v, 0)))
    return out


def seed_reflection(s, program):
    """⑥：反射类 → 结构体与名字；枚举。 | ⑥: reflected classes -> structures and names; enums."""
    from ghidra.program.model.data import (ArrayDataType, BooleanDataType, ByteDataType, CategoryPath, DataTypeConflictHandler,
                                           EnumDataType, FloatDataType, IntegerDataType, StructureDataType)
    classes, enums = reflect_classes()
    if not classes: return
    dtm = program.getDataTypeManager(); cat = CategoryPath('/dm/reflect')
    by = {c['name']: c for c in classes}
    def chain(c):   # 父类在前，子类覆盖同偏移字段 | parents first, a subclass overrides a field at the same offset
        out, seen, cur = {}, set(), c
        stack = []
        while cur is not None and cur['name'] not in seen:
            seen.add(cur['name']); stack.append(cur); cur = by.get(cur['parent'])
        for k in reversed(stack):
            for f in k['fields']: out[f['offset']] = f
        return [out[o] for o in sorted(out)]
    # 先给每个类一个空结构体，字段类型才引用得到 | an empty structure per class first, so field types can refer to them
    if not s.dry:
        for c in classes:
            if dtm.getDataType(cat, ident(c['name'])) is None: dtm.addDataType(StructureDataType(cat, ident(c['name']), 0, dtm), DataTypeConflictHandler.KEEP_HANDLER)
    scalar = {0: IntegerDataType.dataType, 1: BooleanDataType.dataType, 2: FloatDataType.dataType, 5: IntegerDataType.dataType}
    def element(f):
        stride = f['flags'] & 0xffffffff
        if f['kind'] in scalar and scalar[f['kind']].getLength() == stride: return scalar[f['kind']]
        if f['kind'] == 4 and f['type'] in by:
            t = dtm.getDataType(cat, ident(f['type']))
            if t is not None and t.getLength() == stride: return t
        return ArrayDataType(ByteDataType.dataType, max(stride, 1), 1)
    changed = 0
    # 两遍：第一遍定下每个结构体的大小，第二遍字段才能用上别的类的结构体（大小对得上时），一次跑完就是定点
    # two passes: the first fixes every structure's size, so in the second a field can use another class's structure (when the sizes match); one run then reaches the fixed point
    for c in classes * 2:
        if s.dry: continue
        st = StructureDataType(cat, ident(c['name']), 0, dtm)
        fields = chain(c)
        if not fields: continue
        def size(f):
            n = f['flags'] >> 32
            return (f['flags'] & 0xffffffff) * (1 if n == 0xffffffff else max(n, 1))
        st.growStructure(max(f['offset'] + size(f) for f in fields))
        for f in fields:
            n = f['flags'] >> 32; el = element(f)
            dt = el if n == 0xffffffff else ArrayDataType(el, max(n, 1), el.getLength())
            try: st.replaceAtOffset(f['offset'], dt, dt.getLength(), ident(f['name']), f"kind {f['kind']}" + (f", type {f['type']}" if f['type'] else ''))
            except Exception: s.bump('reflect: field overlaps skipped')   # noqa: BLE001 -- 少数字段重叠（联合体式的布局） | a few fields overlap (union-like layouts)
        old = dtm.getDataType(cat, ident(c['name']))
        if old is not None and old.isEquivalent(st): continue
        dtm.addDataType(st, DataTypeConflictHandler.REPLACE_HANDLER); changed += 1
    s.stats['reflect: structure writes'] = changed
    # 名字：描述符、类型对象虚表、工厂、取类型函数 | names: descriptor, type-object vtable, factory, getter
    getters, factories = {}, {}
    mem = program.getMemory()
    for c in classes:
        getters.setdefault(c['getter'], []).append(c)
        if c['desc_vtable'] is not None:
            f = mem.getLong(ghidra_db.addr(program, c['desc_vtable'] + 0x10)) - ghidra_db.BASE
            if 0 <= f < 0x1b786a0 and program.getFunctionManager().getFunctionAt(ghidra_db.addr(program, f)) is not None: factories.setdefault(f, []).append(c)
    for c in classes:
        n = ident(c['name'])
        if c['desc'] is not None: s.name(c['desc'], f'type_{n}', 'reflect')
        if c['desc_vtable'] is not None: s.name(c['desc_vtable'], f'vt_Type_{n}', 'reflect')
        if len(getters[c['getter']]) == 1: s.name(c['getter'], f'{n}_GetType', 'reflect')
    for f, cs in factories.items():   # 只给专属一个类的工厂起名（几个类共用的工厂不起，免得来回改名） | name only factories that belong to one class (shared ones stay unnamed, to avoid renaming back and forth)
        if len(cs) == 1: s.name(f, f"{ident(cs[0]['name'])}_Create", 'reflect')
    # 枚举 | enums
    def make_enum(name, size, items):
        if s.dry or not items: return
        e = EnumDataType(CategoryPath('/dm/enums'), ident(name), size, dtm)
        for k, v in items:
            try: e.add(ident(k), v)
            except Exception: pass   # noqa: BLE001 -- 重名或重值 | duplicate name or value
        old = dtm.getDataType(CategoryPath('/dm/enums'), ident(name))
        if old is None or not old.isEquivalent(e): dtm.addDataType(e, DataTypeConflictHandler.REPLACE_HANDLER); s.bump('enums changed')
    for e in enums:
        make_enum('MsgChannel' if e['name'] == 'MessageChannels' else f"Msg_{e['name']}", 8, [(v, fnv1a64(v)) for v in e['values'] if v])
    for name, items in enum_rows().items(): make_enum(name, 4, items)


def seed_tables(s, program):
    """⑦：数据表 → 记录结构体、表根结构体、表根指针。 | ⑦: data tables -> record structures, the root structure, the root pointers."""
    if not TABLES.exists(): return
    import json
    from ghidra.program.model.data import (ArrayDataType, ByteDataType, CategoryPath, CharDataType, DataTypeConflictHandler, DataUtilities,
                                           IntegerDataType, PointerDataType, ShortDataType, StructureDataType, UnsignedIntegerDataType)
    d = json.loads(TABLES.read_text(encoding='utf-8'))
    tables = [t for t in d.get('tables', []) if t.get('columns') and t.get('rec_size')]
    by = {t['name']: t for t in tables}   # 两套数据各读一遍：取后读的那份（列更全） | each data set reads every table once: keep the later load (it has every column)
    slots = {}                            # 表根内偏移 → 表名（三个表根同一布局） | offset in a root -> table name (all three roots share one layout)
    for t in tables:
        for sl in t.get('slots', []):
            r, off = sl['at'].split('+')
            if r.startswith('root'): slots.setdefault(int(off, 16), (t['name'], sl['next_u32'] == t['rows']))
    link = {o + 0x18: n for o, (n, _) in slots.items()}   # 查找列的 +0x18 指向被查表槽位后 0x18 处的索引对象 | a lookup column's +0x18 points at the index object 0x18 past the looked-up table's slot
    if s.dry: s.stats['tables: would define'] = len(by); return
    dtm = program.getDataTypeManager(); cat = CategoryPath('/dm/tables')
    width = {1: ByteDataType.dataType, 2: ShortDataType.dataType, 4: IntegerDataType.dataType}
    changed = 0
    for name, t in sorted(by.items()):
        st = StructureDataType(cat, f'txt_{ident(name)}', t['rec_size'], dtm)
        flags = {}
        for c in t['columns']:
            kind = c['kind']
            if kind in (29, 30):   # 标志位：按所在 u32 归组 | flag bits: grouped by the u32 they live in
                flags.setdefault(c['offset'] + 4 * (c['len'] >> 5), []).append(f"bit {c['len'] & 31}{' (inverted)' if kind == 30 else ''}: {c['name']}")
                continue
            if kind == 1: dt = ArrayDataType(CharDataType.dataType, max(c['len'], 1), 1)
            else:
                w = {2: 4, 3: 2, 4: 1, 5: 1, 6: 1, 7: 1, 8: 4, 9: 4, 10: 4, 11: 4, 12: 1, 13: 1, 14: 2, 15: 2, 16: 1, 17: 2, 18: 4, 19: 3, 20: 5,
                     21: 4, 22: 2, 23: 1, 24: 2, 25: 4}.get(kind, 0)
                if not w: continue   # 回调类的列不占记录字段 | callback columns take no record field
                dt = width.get(w) or ArrayDataType(ByteDataType.dataType, w, 1)
            note = c['type'] + (f" -> {link[int(c['callback'].split('+')[1], 16)]}" if c['callback'].startswith('root') and int(c['callback'].split('+')[1], 16) in link else '')
            try: st.replaceAtOffset(c['offset'], dt, dt.getLength(), ident(c['name']), note)
            except Exception: s.bump('tables: field overlaps skipped')   # noqa: BLE001
        for off, bits in sorted(flags.items()):
            try: st.replaceAtOffset(off, UnsignedIntegerDataType.dataType, 4, f'flags_{off:x}', '; '.join(sorted(bits, key=lambda b: int(b.split()[1].rstrip(':')))))
            except Exception: s.bump('tables: flag words overlapping a field')   # noqa: BLE001
        old = dtm.getDataType(cat, st.getName())
        if old is None or not old.isEquivalent(st): dtm.addDataType(st, DataTypeConflictHandler.REPLACE_HANDLER); changed += 1
    s.stats['tables: record structures written'] = changed
    root = StructureDataType(CategoryPath('/dm'), 'DataTables', 0x2700, dtm)
    for off, (name, counted) in sorted(slots.items()):
        rec = dtm.getDataType(cat, f'txt_{ident(name)}')
        try:
            root.replaceAtOffset(off, PointerDataType(rec, dtm), 8, ident(name), 'record array')
            if counted: root.replaceAtOffset(off + 8, IntegerDataType.dataType, 4, f'{ident(name)}_count', 'rows')
        except Exception: s.bump('tables: root slot overlaps skipped')   # noqa: BLE001
    items = dtm.getDataType(cat, 'txt_weapons')   # 三张物品表（同一记录布局）拼成的一张 | the one table built from the three item tables (one record layout)
    if items is not None and 0x15c0 not in slots:
        root.replaceAtOffset(0x15c0, PointerDataType(items, dtm), 8, 'items', 'weapons + armor + misc, merged'); root.replaceAtOffset(0x15c8, IntegerDataType.dataType, 4, 'items_count', 'rows')
    old = dtm.getDataType(CategoryPath('/dm'), 'DataTables')
    if old is None or not old.isEquivalent(root): dtm.addDataType(root, DataTypeConflictHandler.REPLACE_HANDLER); s.bump('tables: DataTables written')
    ptr = PointerDataType(dtm.getDataType(CategoryPath('/dm'), 'DataTables'), dtm)
    st = program.getSymbolTable()
    for key, label in (('root0', 'g_tables_v1'), ('root1', 'g_tables_v2'), ('root2', 'g_tables_v3')):   # 数据版本 1–3（G+0x106、单位 +0x1bd） | data versions 1-3 (G+0x106, unit +0x1bd)
        if key not in d.get('roots', {}): continue
        off = int(d['roots'][key].replace('main+', ''), 16)
        for old in list(st.getSymbols(s.a(off))):   # 本步以前起的旧名去掉 | drop names this step gave earlier
            if old.getName().startswith('g_tables_') and old.getName() != label and old.getSource() != s.ST.USER_DEFINED: old.delete()
        s.name(off, label, 'tables')
        cur = program.getListing().getDataAt(s.a(off))
        if cur is None or not cur.getDataType().isEquivalent(ptr):
            DataUtilities.createData(program, s.a(off), ptr, -1, DataUtilities.ClearDataMode.CLEAR_ALL_CONFLICT_DATA); s.bump('tables: root pointers typed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--maxmem', default=os.environ.get('GHIDRA_MAXMEM', '10g'))
    a = ap.parse_args()
    ghidra_db.start(a.maxmem)
    import pyghidra
    from ghidra.util.task import TaskMonitor
    project = ghidra_db.open_project()
    with pyghidra.program_context(project, '/main.elf') as program:
        s = Seeder(program, a.dry_run)
        with pyghidra.transaction(program, 'dm seed'):
            s.nvn()
            for cls, (const, _) in WIDGETS.items():
                off = vt_offsets().get(const)
                if off is not None: s.name(off, f'vt_{cls}', 'widget vtable')
            s.structs()
            for off, kind, text in rewrite_rules(): s.bookmark(off, kind, text)
            notes = {off: rows[:8] for off, rows in source_comments().items()}
            table = names_table()
            s.skip = {off for off, *_ in table}   # names.tsv 的地址归它管，反射名不碰 | names.tsv owns its addresses; reflection names leave them alone
            seed_reflection(s, program)
            seed_tables(s, program)
            s.skip = set()
            for off, name, conf, src in table:
                s.name(off, name, 'names.tsv')
                notes.setdefault(off, []).insert(0, f'name {name}: confidence {conf}; {src}')
            pinned = {off for off, *_ in table}
            for off, name, conf, src, why in auto_names():   # ⑧ 批量起名（names.tsv 的地址不碰） | (8) bulk names (addresses in names.tsv are left alone)
                if off in pinned or conf not in ('high', 'medium'): continue
                s.name(off, name, 'autoname')
                notes.setdefault(off, []).insert(0, f'auto name {name}: confidence {conf}; {src}: {why}')
            for off, rows in sorted(notes.items()): s.comment(off, rows)   # 每个地址只写一次，整段 [dm] 一起换 | each address written once, the whole [dm] block replaced together
        if not a.dry_run:
            program.save('dm seed', TaskMonitor.DUMMY)
            save_ledger(s.named)
            dbprofile.stamp('seed', (*dbprofile.SEED_INPUTS, 'seed.py'))
        for k in sorted(s.stats): print(f'  {k}: {s.stats[k]}')
    project.close()
    print('SEED OK' + (' (dry run)' if a.dry_run else ''))


if __name__ == '__main__':
    main()
