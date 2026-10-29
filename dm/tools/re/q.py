#!/usr/bin/env python3
# Command-line client for the query server (pyghidra-mcp started by re.sh serve), for worktree sessions, subagents and people; run by re.sh q ($DM_ROOT/re/venv).
#   Addresses are always written main+0x... (function / label names work too); an address in the project = 0x7100000000 + offset, so e.g. a FUN_71… name ending in 00abcdef in the output is main+0xabcdef.
#   decompile <address|name>... [--callees] [--xrefs] [--strings] [--timeout s]   decompile
#   xrefs <address|name>...          what references it
#   callgraph <function> [--called]  call graph (default = what it calls; --called = what calls it)
#   strings <substring> [--limit N]  search strings        find <substring> [--functions] [--limit N]   search symbol names
#   bytes <address> [count]          read bytes
#   name <address|name> <new name>   rename a function (saved)
#   comment <address> <text> [--type plate|pre|eol|post|decompiler|repeatable]   set a comment (saved)
#   save                             have the server save its changes (re.sh stop calls it first)
#   tools                            list every tool the server offers
#   Searches (xrefs, strings, find) end with a `Searched:` line: what was searched, the limit, the export date, the count, and
#   whether the list was cut at --limit. Copy it into a note's negative claim; 0 results means none within that scope only.
#   The output is analysis derived from the game program: read it on this machine only; never paste it into the repo, issues, PRs or web pages.
import argparse
import asyncio
import json
import os
import re
import sys

BASE = 0x7100000000
URL = f"http://127.0.0.1:{os.environ.get('DM_RE_PORT', '8000')}/mcp"


def target(where):
    """main+0x… → 工程地址（0x71…）；其余（名字、已是 0x 的地址）原样。 | main+0x... -> project address (0x71...); anything else (names, addresses already in 0x form) unchanged."""
    m = re.fullmatch(r'main\+0x([0-9a-fA-F]+)', where)
    return f'0x{BASE + int(m.group(1), 16):x}' if m else where


def export_date():
    """When the database was last exported (export/profile.json), for the Searched: line."""
    p = os.path.join(os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work'), 're', 'export', 'profile.json')
    try:
        with open(p, encoding='utf-8') as f: return json.load(f).get('exported') or 'unknown'
    except (OSError, ValueError): return 'unknown'


def main_off(addr):
    """Project address (0x71...) -> main+0x...; anything else unchanged."""
    try: v = int(addr, 16)
    except (TypeError, ValueError): return addr
    return f'main+{v - BASE:#x}' if v >= BASE else addr


def searched(a, data):
    """The Searched: lines for a search command: scope, count and whether --limit cut the list."""
    if not isinstance(data, dict): return []
    db = f'db export {export_date()}'
    if a.cmd in ('strings', 'find'):
        rows = data.get('strings' if a.cmd == 'strings' else 'symbols') or []
        what = f'strings the analysis defined matching "{a.query}" (bytes Ghidra never typed as a string are not searched)' if a.cmd == 'strings' else f'{"function" if a.functions else "symbol"} names matching "{a.query}"'
        cut = f'; truncated at --limit {a.limit}, more may exist' if len(rows) >= a.limit else ''
        return [f'Searched: {what} in main (limit {a.limit}, {db}) -> {len(rows)} result{"" if len(rows) == 1 else "s"}{cut}']
    if a.cmd == 'xrefs':
        out = []
        for t in data.get('result') or []:
            n = len(t.get('cross_references') or [])
            err = f' (error: {t["error"]})' if t.get('error') else ''
            out.append(f'Searched: direct references to {main_off(t.get("target"))} recorded by the analysis ({db}; calls through vtables, '
                       f'message dispatch or computed branches are not listed) -> {n}{err}')
        return out
    return []


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
            rc = show(r)
            if not r.isError:
                for line in searched(a, r.structuredContent): print(line)
            return rc


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
