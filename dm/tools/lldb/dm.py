# lldb 里调宿主（dmhost / Mac App / iOS 包）用的命令：游戏模块的地址换成「模块+偏移」与客体符号（dm/tools/guestsyms.py 的本机符号表）。
#   载入：(lldb) command script import <仓库>/dm/tools/lldb/dm.py
#   dm modules              列出已装的游戏模块（宿主全局 g_mod / g_nmod）
#   dm where <地址表达式>    这个地址在哪：游戏模块 + 偏移 + 客体函数，或宿主符号
#   dm bt [-a]              当前线程（-a：全部线程）的回溯，每帧带模块偏移与客体函数
#   dm break <模块+0x偏移>   在游戏模块里下断点（按当前装载基址换算）
#   dm tick                 当前服务端拍号（调 dm_cur_tick）
#   dm ui [-a]              界面根的面板（名字 / 可见 / 激活 / 优先级；-a 连不可见的）——同 dm_engine.c 读的那几处
#   dm focus                焦点管理器：悬停控件、焦点所在面板、焦点控件
#   dm saves                存盘状态：忙、提交推迟、各存档容器的挂载状态与脏标
#   dm unit <id>            服务端单位（先找物品表、再找怪物表）：地址、类型、行号、模式
#   给代理用：先 (lldb) protocol start MCP，代理的 MCP 配置里用 xcrun lldb-mcp 连这个 lldb（见 dm/verify/debugging.md）
# Commands for debugging the host (dmhost / Mac App / iOS package) in lldb: game-module addresses become "module+offset" and guest symbols (the local symbol table of dm/tools/guestsyms.py).
#   Load: (lldb) command script import <repo>/dm/tools/lldb/dm.py
#   dm modules              list the loaded game modules (host globals g_mod / g_nmod)
#   dm where <address expr> where this address is: game module + offset + guest function, or a host symbol
#   dm bt [-a]              backtrace of the current thread (-a: all threads), each frame with module offset and guest function
#   dm break <module+0xoff> set a breakpoint in a game module (converted with the current load base)
#   dm tick                 the current server tick (calls dm_cur_tick)
#   dm ui [-a]              the UI root's panels (name / visible / active / priority; -a includes the hidden ones) -- the same fields dm_engine.c reads
#   dm focus                the focus manager: hovered widget, the focus's panel, focused widget
#   dm saves                save state: busy, commit deferred, each save container's mount state and dirty flag
#   dm unit <id>            a server unit (item table first, then the monster table): address, type, row, mode
#   For agents: (lldb) protocol start MCP first, and the agent's MCP configuration uses xcrun lldb-mcp to connect to this lldb (see dm/verify/debugging.md)
import os
import re
import shlex
import sys

import lldb  # pyright: ignore[reportMissingImports]  # noqa: F401 -- lldb 自己的 Python 环境里才有 | only present inside lldb's own Python

sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')))
try:
    import guestsyms   # noqa: E402
except ImportError:
    guestsyms = None


def _modules(target):
    """[(名字, 基址, 长度)]：读宿主的 g_mod[g_nmod]（module_t 开头是 char name[16]，后面是 base 指针与 size）。 | [(name, base, size)]: reads the host's g_mod[g_nmod] (module_t starts with char name[16], followed by the base pointer and size)."""
    n = target.FindFirstGlobalVariable('g_nmod')
    arr = target.FindFirstGlobalVariable('g_mod')
    if not n.IsValid() or not arr.IsValid(): return []
    out = []
    for i in range(n.GetValueAsUnsigned()):
        m = arr.GetChildAtIndex(i)
        name = m.GetChildMemberWithName('name').GetSummary() or ''
        out.append((name.strip('"'), m.GetChildMemberWithName('base').GetValueAsUnsigned(), m.GetChildMemberWithName('size').GetValueAsUnsigned()))
    return out


def _where(target, addr):
    for name, base, size in _modules(target):
        if base <= addr < base + size:
            w = f'{name}+{addr - base:#x}'
            g = guestsyms.lookup(w) if guestsyms else ''
            return w + (f'  {g}' if g else '')
    sa = target.ResolveLoadAddress(addr)
    sym = sa.GetSymbol()
    return f'{sym.GetName()}+{addr - sym.GetStartAddress().GetLoadAddress(target):#x} ({sa.GetModule().GetFileSpec().GetFilename()})' if sym.IsValid() else hex(addr)


def _eval_addr(frame, target, expr):
    v = frame.EvaluateExpression(expr) if frame and frame.IsValid() else target.EvaluateExpression(expr)
    if v.GetError().Fail(): raise ValueError(v.GetError().GetCString())
    return v.GetValueAsUnsigned()


def _rd(proc, addr, n):
    err = lldb.SBError(); b = proc.ReadMemory(addr, n, err) if addr else None
    return b if b is not None and err.Success() else None


def _u(proc, addr, n=8, signed=False):
    b = _rd(proc, addr, n)
    return int.from_bytes(b, 'little', signed=signed) if b else 0


def _cstr(proc, addr, cap=64):
    if not addr: return ''
    err = lldb.SBError(); s = proc.ReadCStringFromMemory(addr, cap, err)
    return s if err.Success() else '?'


def _main_base(target):
    return next((b for n, b, _ in _modules(target) if n == 'main'), 0)


def _game(target, proc, result, what):   # 游戏状态读法与 dm/host/dm_engine.c 同一组地址 | reads game state at the same addresses as dm/host/dm_engine.c
    b = _main_base(target)
    if not b: result.SetError('no main module (dm modules)'); return
    root = _u(proc, b + 0x2f05458)
    if what in ('ui', 'ui-a'):
        ch, n = _u(proc, root + 0x58), _u(proc, root + 0x60)
        if not root or not ch or n > 256: result.AppendMessage('界面根还没建 | no UI root yet'); return
        for j in range(n):
            p = _u(proc, ch + 8 * j); vis = _u(proc, p + 0x50, 1)
            if vis or what == 'ui-a':
                result.AppendMessage(f'{p:#014x} {"V" if vis else "-"}{"A" if _u(proc, p + 0x51, 1) else "-"} prio {_u(proc, p + 0x84, 4, True):>11}  {_cstr(proc, _u(proc, p + 8))}')
    elif what == 'focus':
        fm = _u(proc, root + 0xd0)
        for lab, off in (('hover', 0x178), ('focus panel', 0x188), ('focus', 0x190)):
            w = _u(proc, fm + off) if fm else 0
            result.AppendMessage(f'{lab:<12} {w:#014x}  {_cstr(proc, _u(proc, w + 8)) if w else ""}')
        result.AppendMessage(f'cursor mode {_u(proc, b + 0x2d128f0, 1)}  cursor state {_u(proc, b + 0x2d12928, 4, True)}')
    elif what == 'saves':
        result.AppendMessage(f'busy {_u(proc, b + 0x31162c9, 1)}  commit pending {_u(proc, b + 0x315e9ac, 1)}')
        arr, cn = _u(proc, b + 0x3115d60 + 0xa0), _u(proc, b + 0x3115d60 + 0xa8)
        for i in range(min(cn, 16)):
            c = _u(proc, arr + 8 * i)
            result.AppendMessage(f'  container {i}: {c:#014x} state {_u(proc, c + 0x378, 4)} ({"mounted" if _u(proc, c + 0x378, 4) > 1 else "idle"}) dirty {_u(proc, c + 0x388, 1)}')


def _unit(target, proc, result, uid):   # 服务端单位表（同 diag.c dm_diag_srv_item）：G+0x2e30 物品、G+0x2630 怪物，128 桶，下一个 +0x158 | server unit tables (as diag.c dm_diag_srv_item): G+0x2e30 items, G+0x2630 monsters, 128 buckets, next +0x158
    gv = target.FindFirstGlobalVariable('g_lastG'); G = gv.GetValueAsUnsigned() if gv.IsValid() else 0
    if not G: result.SetError('no g_lastG (not in a game, or the host was built without debug info)'); return
    for lab, off in (('item', 0x2e30), ('monster', 0x2630)):
        u = _u(proc, G + off + 8 * (uid & 0x7f))
        for _ in range(4096):
            if not u: break
            if _u(proc, u + 8, 4) == uid:
                result.AppendMessage(f'{lab} {uid}: {u:#014x} type {_u(proc, u, 4)} row {_u(proc, u + 4, 4)} mode {_u(proc, u + 0xc, 4)}'); return
            u = _u(proc, u + 0x158)
    result.AppendMessage(f'unit {uid}: not in the item or monster tables')


def dm(debugger, command, result, internal_dict):   # noqa: ARG001
    args = shlex.split(command)
    target = debugger.GetSelectedTarget(); proc = target.GetProcess()
    thread = proc.GetSelectedThread() if proc.IsValid() else None
    frame = thread.GetSelectedFrame() if thread else None
    if not args or args[0] in ('help', '-h'):
        result.AppendMessage('dm modules | dm where <addr> | dm bt [-a] | dm break <module+0xoff> | dm tick | dm ui [-a] | dm focus | dm saves | dm unit <id>'); return
    try:
        if args[0] == 'modules':
            for name, base, size in _modules(target): result.AppendMessage(f'{name:<10} {base:#x} - {base + size:#x}  ({size / 1048576:.1f} MiB)')
            if not _modules(target): result.AppendMessage('没有 g_mod（不是宿主进程，或还没装模块）| no g_mod (not a host process, or modules not loaded yet)')
        elif args[0] == 'where' and len(args) > 1:
            result.AppendMessage(_where(target, _eval_addr(frame, target, ' '.join(args[1:]))))
        elif args[0] == 'bt':
            threads = [proc.GetThreadAtIndex(i) for i in range(proc.GetNumThreads())] if '-a' in args else [thread]
            for th in threads:
                if not th: continue
                result.AppendMessage(f'thread #{th.GetIndexID()} {th.GetName() or ""}')
                for i in range(th.GetNumFrames()):
                    pc = th.GetFrameAtIndex(i).GetPC()
                    result.AppendMessage(f'  #{i:<2} {pc:#018x}  {_where(target, pc)}')
        elif args[0] == 'break' and len(args) > 1:
            m = re.fullmatch(r'(\w+)\+(0x[0-9a-fA-F]+|\d+)', args[1])
            base = next((b for n, b, _ in _modules(target) if m and n == m.group(1)), None)
            if not m or base is None: result.SetError(f'unknown module offset {args[1]} (dm modules lists them)'); return
            bp = target.BreakpointCreateByAddress(base + int(m.group(2), 0))
            result.AppendMessage(f'breakpoint {bp.GetID()} at {base + int(m.group(2), 0):#x} = {_where(target, base + int(m.group(2), 0))}')
        elif args[0] == 'tick':
            v = (frame or target).EvaluateExpression('(unsigned)dm_cur_tick()')
            result.AppendMessage(f'tick {v.GetValueAsUnsigned()}' if not v.GetError().Fail() else f'cannot call dm_cur_tick: {v.GetError().GetCString()}')
        elif args[0] in ('ui', 'focus', 'saves'):
            _game(target, proc, result, 'ui-a' if args[0] == 'ui' and '-a' in args else args[0])
        elif args[0] == 'unit' and len(args) > 1:
            _unit(target, proc, result, int(args[1], 0))
        else:
            result.SetError('unknown dm command (dm help)')
    except ValueError as e:
        result.SetError(str(e))


def __lldb_init_module(debugger, internal_dict):   # noqa: ARG001
    debugger.HandleCommand('command script add -o -f dm.dm dm')
    print('dm: dm modules | where | bt | break | tick | ui | focus | saves | unit（dm/verify/debugging.md）')
