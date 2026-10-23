# lldb 里调宿主（dmhost / Mac App / iOS 包）用的命令：游戏模块的地址换成「模块+偏移」与客体符号（dm/tools/guestsyms.py 的本机符号表）。
#   载入：(lldb) command script import <仓库>/dm/tools/lldb/dm.py
#   dm modules              列出已装的游戏模块（宿主全局 g_mod / g_nmod）
#   dm where <地址表达式>    这个地址在哪：游戏模块 + 偏移 + 客体函数，或宿主符号
#   dm bt [-a]              当前线程（-a：全部线程）的回溯，每帧带模块偏移与客体函数
#   dm break <模块+0x偏移>   在游戏模块里下断点（按当前装载基址换算）
#   dm tick                 当前服务端拍号（调 dm_cur_tick）
#   给代理用：先 (lldb) protocol start MCP，代理的 MCP 配置里用 xcrun lldb-mcp 连这个 lldb（见 dm/verify/debugging.md）
# Commands for debugging the host (dmhost / Mac App / iOS package) in lldb: game-module addresses become "module+offset" and guest symbols (the local symbol table of dm/tools/guestsyms.py).
#   Load: (lldb) command script import <repo>/dm/tools/lldb/dm.py
#   dm modules              list the loaded game modules (host globals g_mod / g_nmod)
#   dm where <address expr> where this address is: game module + offset + guest function, or a host symbol
#   dm bt [-a]              backtrace of the current thread (-a: all threads), each frame with module offset and guest function
#   dm break <module+0xoff> set a breakpoint in a game module (converted with the current load base)
#   dm tick                 the current server tick (calls dm_cur_tick)
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


def dm(debugger, command, result, internal_dict):   # noqa: ARG001
    args = shlex.split(command)
    target = debugger.GetSelectedTarget(); proc = target.GetProcess()
    thread = proc.GetSelectedThread() if proc.IsValid() else None
    frame = thread.GetSelectedFrame() if thread else None
    if not args or args[0] in ('help', '-h'):
        result.AppendMessage('dm modules | dm where <addr> | dm bt [-a] | dm break <module+0xoff> | dm tick'); return
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
        else:
            result.SetError('unknown dm command (dm help)')
    except ValueError as e:
        result.SetError(str(e))


def __lldb_init_module(debugger, internal_dict):   # noqa: ARG001
    debugger.HandleCommand('command script add -o -f dm.dm dm')
    print('dm: dm modules | where | bt | break | tick（dm/verify/debugging.md）')
