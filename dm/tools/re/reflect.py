#!/usr/bin/env python3
# 引擎反射表提取：游戏用一套自己的反射登记类（类名、父类、字段表），布局 JSON 的 "type" / "fields" 靠它找类与字段偏移。
#   登记点 = 所有直接调用 REGISTER（main+0xb115f0，参数：类描述符、类名、父类型、字段表）的函数（多为带静态守卫的「取类型」函数）；
#   TYPE_BY_NAME（main+0xb11bf0，参数：类名）按名取类型。字段表每项 0x30 字节 {名字, 种类, 偏移, 类型, 标志, 附加}，名字为 0 结束。
#   做法：Unicorn 里装原映像（基址 0x7100000000、施加重定位），逐个从登记函数入口跑：守卫一律放行，TYPE_BY_NAME 返回记名的假句柄，
#   REGISTER 记下四个参数，其余被调函数一律当场返回假句柄（记被调地址），导入按名字做最小模拟。跑完从模拟内存读字段表与描述符首字（类型对象的虚表）。
#   输出 $DM_ROOT/re/export/reflect.json（由游戏程序推出：只放本机，不入库），并打印统计。seed.py 读它建结构体与名字。
#   用法（项目 venv：unicorn / capstone / lz4）：reflect.py [--only main+0x…]
# Extracts the engine's reflection tables: the game registers classes with its own reflection system (class name, parent class, field table); layout JSON "type" / "fields" find classes and field offsets through it.
#   Registration sites = every function that directly calls REGISTER (main+0xb115f0; arguments: class descriptor, class name, parent type, field table), mostly static-guarded "get type" functions;
#   TYPE_BY_NAME (main+0xb11bf0; argument: class name) returns a type by name. Each field-table entry is 0x30 bytes {name, kind, offset, type, flags, extra}, ended by a null name.
#   Method: load the original image in Unicorn (base 0x7100000000, relocations applied) and run each registration function from its entry: guards always pass, TYPE_BY_NAME returns a fake handle that records the name,
#   REGISTER records its four arguments, every other callee returns at once with a fake handle (recording the callee address), and imports get a minimal by-name emulation. Afterwards the field tables and the descriptor's first word (the type object's vtable) are read from emulated memory.
#   Output: $DM_ROOT/re/export/reflect.json (derived from the game program: local only, never committed), plus statistics. seed.py reads it to build structures and names.
#   Usage (project venv: unicorn / capstone / lz4): reflect.py [--only main+0x...]
import argparse
import bisect
import collections
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent)); sys.path.insert(0, str(HERE.parent.parent / 'verify'))
import guestsyms  # noqa: E402
import nsoimg  # noqa: E402
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL  # noqa: E402
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_X30  # noqa: E402

DM_ROOT = Path(os.environ.get('DM_ROOT') or Path.home() / 'dm-work')
NSO = DM_ROOT / 'data/switch/3.3.94012/Update/ExeFS/main'
OUT = DM_ROOT / 're' / 'export' / 'reflect.json'
IB = 0x7100000000
REGISTER, TYPE_BY_NAME, REGISTER_ENUM = 0xb115f0, 0xb11bf0, 0xb12b1c   # 枚举登记（参数：描述符、枚举名、名字数组、个数） | enum registration (arguments: descriptor, enum name, name array, count)
TRAP, HANDLES, STACK, RET, SCRATCH = 0x300000000, 0x600000000000, 0x7f0000000000, 0x400000000, 0x500000000000
SCRATCH_SZ = 0x40000                  # 每个参数寄存器一块清零的假对象（登记点常在成员函数里，入口就解引用 this） | one zeroed fake object per argument register (registration sites often sit in member functions that dereference this at entry)
HSTEP, HCOUNT = 0x100, 0x10000
ENTRY = 0x30
XREG = [UC_ARM64_REG_X0 + i for i in range(29)]   # x0..x28 在 unicorn 里编号连续 | x0..x28 are numbered consecutively in unicorn
TEXT_END = 0x1b786a0


def page(n): return (n + 0xfff) & ~0xfff


def scan_callers(img, starts, target):
    """直接 bl 到 target 的调用点所在函数的起点集合。 | The start addresses of the functions holding a direct bl to target."""
    out = set()
    for o in range(0, TEXT_END, 4):
        w = struct.unpack_from('<I', img, o)[0]
        if w >> 26 != 0x25: continue
        imm = w & 0x3ffffff
        if imm & (1 << 25): imm -= 1 << 26
        if o + 4 * imm == target:
            i = bisect.bisect_right(starts, o) - 1
            if i >= 0: out.add(starts[i])
    return sorted(out)


class Emu:
    def __init__(self, img, rel, starts):
        self.starts = set(starts)
        self.img = bytearray(img)
        self.imports = {}
        for off, typ, name, val, add in rel:
            if typ == 1027: v = IB + add
            elif val is not None: v = IB + val + (add if typ == 257 else 0)
            else: v = TRAP + 16 * self.imports.setdefault(name, len(self.imports)) + (add if typ == 257 else 0)
            struct.pack_into('<Q', self.img, off, v)
        self.iname = {i: n for n, i in self.imports.items()}
        u = self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(IB, page(len(self.img)), UC_PROT_ALL); u.mem_write(IB, bytes(self.img))
        u.mem_map(TRAP, page(16 * len(self.imports) + 16), UC_PROT_ALL)
        u.mem_map(RET, 0x1000, UC_PROT_ALL)
        u.mem_map(HANDLES, HSTEP * HCOUNT, UC_PROT_ALL)
        u.mem_map(STACK, 0x200000, UC_PROT_ALL)
        u.mem_map(SCRATCH, SCRATCH_SZ * 8, UC_PROT_ALL)
        self.zero = bytes(SCRATCH_SZ * 8)
        self.plt = self.plt_names(img)
        self.heap = STACK + 0x100000          # 栈区后半当小堆（malloc 类导入） | the upper half of the stack region serves as a small heap (malloc-like imports)
        self.handles = []
        u.hook_add(UC_HOOK_BLOCK, self.on_block)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, lambda uc, acc, addr, size, val, d: self.fault(addr))

    def fault(self, addr):
        self.err = f'unmapped access {addr:#x}'
        return False

    def handle(self, what):
        h = HANDLES + HSTEP * len(self.handles)
        if len(self.handles) >= HCOUNT: h = HANDLES   # 用尽就复用第一个（不影响字段表） | when used up, reuse the first one (does not affect field tables)
        else: self.handles.append(what)
        return h

    def cstr(self, a, limit=200):
        if not (IB <= a < IB + len(self.img)): return None
        o = a - IB; e = self.img.find(b'\0', o, o + limit)
        return self.img[o:e].decode('latin1') if e > o else ''

    def ret(self, x0):
        u = self.u
        u.reg_write(UC_ARM64_REG_X0, x0)
        lr = u.reg_read(UC_ARM64_REG_X30)
        # 不返回的调用常是函数最后一条指令，返回地址恰是下一个函数的起点：再进去会被当成又一次调用、原地打转，直接停 | a non-returning call is often a function's last instruction, so the return address is the next function's start: going there would count as another call and spin in place, so stop
        if lr - IB in self.starts and lr - IB != self.fn: self.err = self.err or 'returned into another function'; u.emu_stop(); return
        u.reg_write(UC_ARM64_REG_PC, lr)

    def on_block(self, uc, addr, size, d):
        if addr == RET: uc.emu_stop(); return
        if self.regs:   # 登记之后再走几块（描述符首字随后写入）就停 | stop a few blocks after a registration (the descriptor's first word is written right after)
            self.after += 1
            if self.after > 6: uc.emu_stop(); return
        if TRAP <= addr < TRAP + 16 * len(self.imports):
            name = self.iname.get((addr - TRAP) // 16, '?')
            x = [uc.reg_read(r) for r in XREG[:4]]
            if 'guard_acquire' in name: self.ret(1)
            elif name in ('malloc', 'aligned_alloc', '_Znwm', '_Znam') or name.startswith('_Znwm'):
                n = x[1] if name == 'aligned_alloc' else x[0]
                p = (self.heap + 15) & ~15; self.heap = p + max(16, n)
                if self.heap > STACK + 0x200000: self.heap = STACK + 0x100000; p = self.heap
                self.ret(p)
            elif name == 'memset': uc.mem_write(x[0], bytes([x[1] & 0xff]) * min(x[2], 1 << 16)); self.ret(x[0])
            elif name in ('memcpy', 'memmove'): uc.mem_write(x[0], bytes(uc.mem_read(x[1], min(x[2], 1 << 16)))); self.ret(x[0])
            elif name == 'strlen': s = self.cstr(x[0], 4096); self.ret(len(s) if s is not None else 0)
            else: self.ret(0)
            return
        if addr == IB + TYPE_BY_NAME:
            self.ret(self.handle(('type', self.cstr(uc.reg_read(UC_ARM64_REG_X0)))))
            return
        if addr == IB + REGISTER_ENUM:
            x = [uc.reg_read(r) for r in XREG[:4]]
            names = [self.cstr(self.u64(x[2] + 8 * i) or 0) for i in range(min(x[3], 4096))] if x[2] else []
            self.enums.append({'desc': x[0] - IB if x[0] >= IB else None, 'name': self.cstr(x[1]), 'values': names})
            self.ret(x[0])
            return
        if addr == IB + REGISTER:
            x = [uc.reg_read(r) for r in XREG[:4]]
            self.regs.append({'desc': x[0], 'name': self.cstr(x[1]), 'parent': x[2], 'fields': x[3],
                              'site': uc.reg_read(UC_ARM64_REG_X30) - 4})
            self.after = 0
            self.ret(x[0])
            return
        off = addr - IB
        if off in self.starts and off != self.fn:
            self.ret(self.handle(('call', off)))

    def plt_names(self, img):
        """{PLT 桩偏移: 导入名}：桩 = adrp x16 / ldr x17, [x16, #off] / … ，按它读的 GOT 槽对上 JUMP_SLOT 重定位的名字。 | {PLT stub offset: import name}: a stub is adrp x16 / ldr x17, [x16, #off] / ..., matched to the JUMP_SLOT relocation name through the GOT slot it reads."""
        slot = {off: name for off, typ, name, val, add in nsoimg.relocs(img) if typ == 1026}
        out = {}
        for o in range(0, TEXT_END - 8, 4):
            w0, w1 = struct.unpack_from('<II', img, o)
            if (w0 & 0x9f00001f) != 0x90000010 or (w1 & 0xffc003ff) != 0xf9400211: continue
            immlo, immhi = (w0 >> 29) & 3, (w0 >> 5) & 0x7ffff
            pg = (o & ~0xfff) + (((immhi << 2 | immlo) ^ (1 << 20)) - (1 << 20) << 12)
            got = pg + ((w1 >> 10) & 0xfff) * 8
            if got in slot: out[o] = slot[got]
        return out

    def run(self, fn, budget, start=None, x0=0):
        """从 start（缺省 = 函数入口）跑 fn；参数寄存器指向清零的假对象（x0 可指定）。 | Runs fn from start (default = the function entry); argument registers point at zeroed fake objects (x0 can be given)."""
        u = self.u
        self.fn, self.regs, self.err, self.after = fn, [], None, 0
        self.enums = getattr(self, 'enums', [])
        u.mem_write(SCRATCH, self.zero)
        u.reg_write(UC_ARM64_REG_SP, STACK + 0xf0000); u.reg_write(UC_ARM64_REG_X29, STACK + 0xf8000)
        u.reg_write(UC_ARM64_REG_X30, RET)
        for i, r in enumerate(XREG): u.reg_write(r, SCRATCH + SCRATCH_SZ * i + 0x1000 if i < 8 else 0)
        if start is not None: u.reg_write(UC_ARM64_REG_X0, x0)
        try: u.emu_start(IB + (fn if start is None else start), RET, timeout=2_000_000, count=budget)   # 墙钟 2 秒兜底 | 2 s wall-clock safety net
        except UcError as e: self.err = self.err or str(e)
        return self.regs

    def guard_starts(self, fn, end):
        """fn 里每个「bl __cxa_guard_acquire」之后的那条指令：从这里跑 = 守卫已拿到、直接进初始化块。 | The instruction after each "bl __cxa_guard_acquire" in fn: running from there = guard acquired, straight into the init block."""
        out = []
        for o in range(fn, end, 4):
            w = struct.unpack_from('<I', self.img, o)[0]
            if w >> 26 != 0x25: continue
            imm = w & 0x3ffffff
            if imm & (1 << 25): imm -= 1 << 26
            if 'guard_acquire' in self.plt.get(o + 4 * imm, ''): out.append(o + 4)
        return out

    def u64(self, a):
        try: return struct.unpack('<Q', bytes(self.u.mem_read(a, 8)))[0]
        except UcError: return None

    def who(self, h):
        """假句柄 → ('type', 名字) / ('call', 地址)；真地址 → ('addr', 偏移)；0 → None。 | Fake handle -> ('type', name) / ('call', address); a real address -> ('addr', offset); 0 -> None."""
        if not h: return None
        if HANDLES <= h < HANDLES + HSTEP * len(self.handles) and (h - HANDLES) % HSTEP == 0: return self.handles[(h - HANDLES) // HSTEP]
        if IB <= h < IB + len(self.img): return ('addr', h - IB)
        return ('raw', h)

    def fields(self, at, limit=512):
        out = []
        for i in range(limit):
            e = at + ENTRY * i
            w = [self.u64(e + 8 * k) for k in range(6)]
            if w[0] is None or not w[0]: break
            out.append({'name': self.cstr(w[0]), 'kind': w[1], 'offset': w[2], 'type': self.who(w[3]), 'flags': w[4], 'extra': w[5]})
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', help='main+0x… of one registration function')
    ap.add_argument('--budget', type=int, default=60000)
    a = ap.parse_args()
    img = nsoimg.load(str(NSO))
    starts = [s for s, _ in guestsyms.functions(img)]
    fns = [int(a.only.split('+')[1], 16)] if a.only else sorted(set(scan_callers(img, starts, REGISTER)) | set(scan_callers(img, starts, REGISTER_ENUM)))
    emu = Emu(img, nsoimg.relocs(img), starts)
    sizes = dict(guestsyms.functions(img))
    classes, errs, retried = [], collections.Counter(), 0
    for fn in fns:
        regs = emu.run(fn, a.budget)
        if not regs:   # 入口跑不到登记点：改从每个守卫块直接跑 | the entry run did not reach the registration: run each guarded block directly instead
            retried += 1; seen = set()
            for st in emu.guard_starts(fn, fn + sizes.get(fn, 0)):
                for r in emu.run(fn, a.budget // 4, start=st, x0=1):
                    if r['desc'] not in seen: seen.add(r['desc']); regs.append(r)
        for r in regs:
            par = emu.who(r['parent'])
            fl = emu.fields(r['fields']) if r['fields'] else []
            classes.append({'getter': fn, 'site': r['site'] - IB, 'desc': r['desc'] - IB if r['desc'] >= IB else None, 'name': r['name'],
                            'parent': par, 'desc_vtable': (lambda v: v - IB if v and v >= IB else None)(emu.u64(r['desc'])),
                            'fields': fl})
        if emu.err: errs[emu.err.split(' 0x')[0]] += 1
    # 用「取类型函数 → 类名」把 ('call', 地址) 形式的父类 / 字段类型换成名字 | Turn ('call', address) parents / field types into names through "getter -> class name"
    by_getter = collections.defaultdict(set)
    for c in classes: by_getter[c['getter']].add(c['name'])
    def named(ref):
        if not ref: return None
        if ref[0] == 'type': return ref[1]
        if ref[0] == 'call' and len(by_getter.get(ref[1], ())) == 1: return next(iter(by_getter[ref[1]]))
        return f'{ref[0]}:{ref[1]:#x}' if isinstance(ref[1], int) else f'{ref[0]}:{ref[1]}'
    for c in classes:
        c['parent'] = named(c['parent'])
        for f in c['fields']: f['type'] = named(f['type'])
    # 同一描述符常被多处内联的取类型函数登记：每个描述符留一份，取有父类、字段最多的那份 | the same descriptor is often registered by several inlined getters: keep one copy per descriptor, the one with a parent and the most fields
    best = {}
    for c in classes:
        k = c['desc'] if c['desc'] is not None else ('name', c['name'])
        score = (c['parent'] is not None, len(c['fields']))
        if k not in best or score > best[k][0]: best[k] = (score, c)
    missing = sum(1 for fn in fns if not any(c['getter'] == fn for c in classes))
    classes = sorted((c for _, c in best.values() if c['name']), key=lambda c: c['name'])
    enums = {}
    for e in emu.enums:
        if e['name'] and (e['name'] not in enums or len(e['values']) > len(enums[e['name']]['values'])): enums[e['name']] = e
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({'classes': classes, 'enums': sorted(enums.values(), key=lambda e: e['name'])}, indent=1))
    kinds = collections.Counter(f['kind'] for c in classes for f in c['fields'])
    names = collections.Counter(c['name'] for c in classes)
    print(f'registration functions {len(fns)}, classes {len(classes)} ({len(names)} distinct), fields {sum(len(c["fields"]) for c in classes)}')
    print('field kinds', dict(kinds.most_common()))
    print('without parent', sum(1 for c in classes if not c['parent']), '| retried from guards', retried, '| last-run errors', dict(errs))
    print('registration functions with nothing captured', missing, '| enums', len(enums), 'with', sum(len(e['values']) for e in enums.values()), 'values')
    print(f'REFLECT OK → {OUT}')


if __name__ == '__main__':
    main()
