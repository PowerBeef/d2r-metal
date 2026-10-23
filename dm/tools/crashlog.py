#!/usr/bin/env python3
# 崩溃现场的读取与符号化：宿主写的 crash.json（DM_CRASH_JSON，crash.c）与系统的崩溃报告（.ips）。
#   show <crash.json> [--bin 宿主可执行文件]   信号、出错地址、各帧：客体帧按「模块+偏移」再查客体符号表（guestsyms.py），宿主帧用 atos（按 crash.json 的映像基址）
#   collect [--process dmhost] [--since 秒] [--out 目录]   把 ~/Library/Logs/DiagnosticReports 里该进程、该时刻之后的 .ips 拷过来，出摘要（异常、信号、出错线程前几帧）
#   末行 RESULT {json}
#   注：macOS 会压掉同一进程反复出现的崩溃报告，没有 .ips 时以 crash.json 为准
# Reading and symbolicating crash state: the crash.json written by the host (DM_CRASH_JSON, crash.c) and the system's crash reports (.ips).
#   show <crash.json> [--bin host executable]   signal, fault address, frames: guest frames as "module+offset" looked up in the guest symbol table (guestsyms.py), host frames through atos (with the image base from crash.json)
#   collect [--process dmhost] [--since seconds] [--out dir]   copy the .ips files of that process after that time from ~/Library/Logs/DiagnosticReports, with a summary (exception, signal, the first frames of the crashed thread)
#   Last line RESULT {json}
#   Note: macOS suppresses repeated crash reports of the same process; when there is no .ips, crash.json is what counts
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
SIGNAMES = {4: 'SIGILL', 5: 'SIGTRAP', 6: 'SIGABRT', 10: 'SIGBUS', 11: 'SIGSEGV', 14: 'SIGALRM'}


def atos(binary, base, addrs):
    if not addrs or not binary or not os.path.exists(binary): return {}
    try: out = subprocess.run(['xcrun', 'atos', '-o', binary, '-l', base, *addrs], capture_output=True, text=True, timeout=60, check=False).stdout.splitlines()
    except (OSError, subprocess.SubprocessError): return {}
    return {a: s.strip() for a, s in zip(addrs, out, strict=False) if s.strip() and s.strip() != a}


def guest_names(wheres):
    """「模块+偏移」→ 客体函数名（guestsyms.py 的本机符号表；没有就空）。 | "module+offset" -> guest function name (guestsyms.py's local symbol table; empty when missing)."""
    try:
        sys.path.insert(0, HERE); import guestsyms   # noqa: PLC0415
        return guestsyms.lookup_many(wheres)
    except (ImportError, OSError, ValueError): return {}


def show(path, binary):
    with open(path, encoding='utf-8') as f: c = json.load(f)
    frames = [dict(pc=c['pc'], where=c.get('pc_where'), kind='pc')] + [dict(pc=b['pc'], where=b.get('where'), kind='bt') for b in c.get('bt', [])]
    host = atos(binary or os.path.join(DM, 'host', 'dmhost'), c.get('host_base', '0x0'), [f['pc'] for f in frames if not f['where']])
    gn = guest_names([f['where'] for f in frames if f['where']])
    sig = c.get('sig'); print(f"{SIGNAMES.get(sig, f'signal {sig}')}（code {c.get('code')}）addr {c.get('addr')}  拍 | tick {c.get('tick')}  阶段 | stage {c.get('stage')}")
    out = []
    for i, f in enumerate(frames):
        name = gn.get(f['where'], '') if f['where'] else host.get(f['pc'], '')
        print(f"  {i:>2} {f['pc']:>18}  {f['where'] or '(host)':<24} {name}")
        out.append(dict(pc=f['pc'], where=f['where'], name=name))
    return dict(ok=True, signal=SIGNAMES.get(sig, sig), addr=c.get('addr'), tick=c.get('tick'), frames=out)


def collect(process, since, out_dir):
    d = os.path.expanduser('~/Library/Logs/DiagnosticReports')
    hits = sorted(p for p in (os.path.join(d, n) for n in os.listdir(d) if n.startswith(process + '-') and n.endswith('.ips')) if os.path.getmtime(p) >= since) if os.path.isdir(d) else []
    reports = []
    for p in hits:
        if out_dir: os.makedirs(out_dir, exist_ok=True); shutil.copy2(p, out_dir)
        with open(p, encoding='utf-8', errors='replace') as f: text = f.read()
        head, _, body = text.partition('\n')
        try: b = json.loads(body); h = json.loads(head)
        except ValueError: reports.append(dict(file=os.path.basename(p), error='unparsable')); continue
        ex = b.get('exception', {}); th = next((t for t in b.get('threads', []) if t.get('triggered')), {})
        imgs = b.get('usedImages', [])
        top = []
        for fr in th.get('frames', [])[:8]:
            img = imgs[fr['imageIndex']]['name'] if 0 <= fr.get('imageIndex', -1) < len(imgs) and 'name' in imgs[fr['imageIndex']] else '?'
            top.append(f"{img} {fr.get('symbol', '')}+{fr.get('symbolLocation', fr.get('imageOffset', 0))}")
        reports.append(dict(file=os.path.basename(p), time=h.get('timestamp'), exception=ex.get('type'), signal=ex.get('signal'), crashed_thread=top))
        print(f"  {os.path.basename(p)}  {ex.get('type')} {ex.get('signal')}"); [print(f'      {t}') for t in top]
    if not reports: print(f'  没有 {process} 的系统崩溃报告（macOS 会压掉重复的）| no system crash report for {process} (macOS suppresses repeats)')
    return dict(ok=True, process=process, reports=reports)


def main(argv):
    ap = argparse.ArgumentParser(prog='crashlog.py', description='Read and symbolicate crash.json and .ips crash reports.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('show'); s.add_argument('file'); s.add_argument('--bin')
    c = sub.add_parser('collect'); c.add_argument('--process', default='dmhost'); c.add_argument('--since', type=float, default=0); c.add_argument('--out')
    a = ap.parse_args(argv)
    res = show(a.file, a.bin) if a.cmd == 'show' else collect(a.process, a.since, a.out)
    print('RESULT ' + json.dumps(dict(name=f'crashlog-{a.cmd}', verdict='OK' if res.get('ok') else 'RED', **{k: v for k, v in res.items() if k != 'frames'}), ensure_ascii=False))
    return 0 if res.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
