#!/usr/bin/env python3
# 查询服务（re.sh serve 起的 pyghidra-mcp）的命令行客户端，给 worktree 会话、子代理和人用；由 re.sh q 调用（$DM_ROOT/re/venv）。
#   地址一律写 main+0x…（也可写函数名 / 标签名）；工程里地址 = 0x7100000000 + 偏移，所以输出里 FUN_71… 后接 00abcdef 的名字就是 main+0xabcdef。
#   decompile <地址|名字>… [--callees] [--xrefs] [--strings] [--timeout 秒]   反编译
#   xrefs <地址|名字>…            谁引用了它
#   callgraph <函数> [--called]   调用图（缺省 = 它调了谁；--called = 谁调了它）
#   strings <子串> [--limit N]    搜字符串        find <子串> [--functions] [--limit N]   搜符号名
#   bytes <地址> [个数]           读字节
#   name <地址|名字> <新名字>      函数改名（存盘）
#   comment <地址> <文字> [--type plate|pre|eol|post|decompiler|repeatable]   写注释（存盘）
#   save                          让服务把改动存盘（re.sh stop 先调它）
#   tools                         列出服务提供的全部工具
#   输出是由游戏程序推出来的分析结果：只在本机看，不贴进仓库、议题、PR 或网页。
# Command-line client for the query server (pyghidra-mcp started by re.sh serve), for worktree sessions, subagents and people; run by re.sh q ($DM_ROOT/re/venv).
#   Addresses are always written main+0x... (function / label names work too); an address in the project = 0x7100000000 + offset, so a FUN_71… name ending in 00abcdef in the output is main+0xabcdef.
#   decompile <address|name>... [--callees] [--xrefs] [--strings] [--timeout s]   decompile
#   xrefs <address|name>...          what references it
#   callgraph <function> [--called]  call graph (default = what it calls; --called = what calls it)
#   strings <substring> [--limit N]  search strings        find <substring> [--functions] [--limit N]   search symbol names
#   bytes <address> [count]          read bytes
#   name <address|name> <new name>   rename a function (saved)
#   comment <address> <text> [--type plate|pre|eol|post|decompiler|repeatable]   set a comment (saved)
#   save                             have the server save its changes (re.sh stop calls it first)
#   tools                            list every tool the server offers
#   The output is analysis derived from the game program: read it on this machine only; never paste it into the repo, issues, PRs or web pages.
import argparse
import asyncio
import os
import re
import sys

BASE = 0x7100000000
URL = f"http://127.0.0.1:{os.environ.get('DM_RE_PORT', '8000')}/mcp"


def target(where):
    """main+0x… → 工程地址（0x71…）；其余（名字、已是 0x 的地址）原样。 | main+0x... -> project address (0x71...); anything else (names, addresses already in 0x form) unchanged."""
    m = re.fullmatch(r'main\+0x([0-9a-fA-F]+)', where)
    return f'0x{BASE + int(m.group(1), 16):x}' if m else where


def show(result):
    """工具结果 → 文字：结构化结果优先，按字段排版；代码原样打印。 | Tool result -> text: structured content first, laid out by field; code printed as is."""
    if result.isError:
        print('🔴 ' + ' '.join(c.text for c in result.content if hasattr(c, 'text')), file=sys.stderr); return 1
    data = result.structuredContent
    if data is None:
        for c in result.content:
            if hasattr(c, 'text'): print(c.text)
        return 0
    def emit(v, ind=''):
        if isinstance(v, dict):
            for k, x in v.items():
                if isinstance(x, (dict, list)): print(f'{ind}{k}:'); emit(x, ind + '  ')
                elif isinstance(x, str) and '\n' in x: print(f'{ind}{k}:'); print(x)
                else: print(f'{ind}{k}: {x}')
        elif isinstance(v, list):
            for x in v: emit(x, ind); print(ind + '-')
        else: print(f'{ind}{v}')
    emit(data)
    return 0


async def run(a):
    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client
    async with streamablehttp_client(URL, timeout=30, sse_read_timeout=900) as (rd, wr, _):
        async with ClientSession(rd, wr) as s:
            await s.initialize()
            if a.cmd == 'save': return show(await s.call_tool('save', {}))
            if a.cmd == 'tools':
                for t in (await s.list_tools()).tools: print(f'{t.name}: {(t.description or "").splitlines()[0]}')
                return 0
            progs = (await s.call_tool('list_project_binaries', {})).structuredContent or {}
            names = [p.get('name') for p in progs.get('programs', [])]
            binary = next((n for n in names if n and 'main' in n), names[0] if names else None)
            if binary is None: print('🔴 the server has no program loaded', file=sys.stderr); return 1
            call = lambda tool, args: s.call_tool(tool, {'binary_name': binary, **args})
            if a.cmd == 'decompile':
                r = await call('decompile_function', {'name_or_address': [target(w) for w in a.where], 'include_callees': a.callees,
                                                       'include_xrefs': a.xrefs, 'include_strings': a.strings, 'timeout_sec': a.timeout})
            elif a.cmd == 'xrefs': r = await call('list_xrefs', {'name_or_address': [target(w) for w in a.where]})
            elif a.cmd == 'callgraph': r = await call('gen_callgraph', {'function_name': target(a.where), 'direction': 'called' if a.called else 'calling'})
            elif a.cmd == 'strings': r = await call('search_strings', {'query': a.query, 'limit': a.limit})
            elif a.cmd == 'find': r = await call('search_symbols_by_name', {'query': a.query, 'functions_only': a.functions, 'limit': a.limit})
            elif a.cmd == 'bytes': r = await call('read_bytes', {'address': target(a.where), 'size': a.count})
            elif a.cmd == 'name':
                r = await call('rename_function', {'name_or_address': target(a.where), 'new_name': a.new_name})
                if not r.isError: await s.call_tool('save', {})
            elif a.cmd == 'comment':
                r = await call('set_comment', {'target': target(a.where), 'comment': a.text, 'comment_type': a.type})
                if not r.isError: await s.call_tool('save', {})
            else: return 2
            return show(r)


def main():
    ap = argparse.ArgumentParser(prog='re.sh q', description='Query the engine-analysis server. Addresses: main+0x...')
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('decompile'); d.add_argument('where', nargs='+'); d.add_argument('--timeout', type=int, default=60)
    for f in ('callees', 'xrefs', 'strings'): d.add_argument(f'--{f}', action='store_true')
    x = sub.add_parser('xrefs'); x.add_argument('where', nargs='+')
    c = sub.add_parser('callgraph'); c.add_argument('where'); c.add_argument('--called', action='store_true')
    s = sub.add_parser('strings'); s.add_argument('query'); s.add_argument('--limit', type=int, default=50)
    f = sub.add_parser('find'); f.add_argument('query'); f.add_argument('--functions', action='store_true'); f.add_argument('--limit', type=int, default=25)
    b = sub.add_parser('bytes'); b.add_argument('where'); b.add_argument('count', type=int, nargs='?', default=32)
    n = sub.add_parser('name'); n.add_argument('where'); n.add_argument('new_name')
    m = sub.add_parser('comment'); m.add_argument('where'); m.add_argument('text')
    m.add_argument('--type', default='plate', choices=['plate', 'pre', 'eol', 'post', 'decompiler', 'repeatable'])
    sub.add_parser('save'); sub.add_parser('tools')
    a = ap.parse_args()
    try:
        return asyncio.run(run(a))
    except Exception as e:   # noqa: BLE001 -- 连不上时 httpx / anyio 抛的是异常组，逐层取出最里面那条 | when the connection fails httpx / anyio raise exception groups; unwrap to the innermost one
        while isinstance(e, BaseExceptionGroup) and e.exceptions: e = e.exceptions[0]
        print(f'🔴 {type(e).__name__}: {e} (is the query server running? dm/tools/re/re.sh serve)', file=sys.stderr); return 1


if __name__ == '__main__':
    sys.exit(main())
