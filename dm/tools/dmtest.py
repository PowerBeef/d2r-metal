#!/usr/bin/env python3
# 统一的测试结果外壳：照常跑任一条测试命令（touchchain / smoke / accept / make … test），输出原样转发，同时从现有的判定行里读出结论，
# 写成机器可读的结果（脚本 / 代理 / CI 不用再各自 grep 中文行）。不改被包的命令；被包的命令可以往 $DMTEST_OUT 里放附加文件。
#   读的行：「名字 [用例] OK|RED|FAIL|PASS …」判定行、「RESULT {json}」结果行、⚪ / ⏭ 不可用与跳过行、【红】 / 🔴 / FATAL / CRASH sig= 红标行
#   结论：ok = 退出码 0 且没有 RED / FAIL 判定；red = 退出码非 0 或有 RED / FAIL 判定（--strict-log 时红标行也算）；error = 超时 / 命令起不来
#   产物：<结果目录>/log.txt（完整输出）、result.json；--junit 另写 JUnit XML；末行 RESULT {json}
# 用法：dmtest.py [--name 名字] [--out 目录] [--junit 文件] [--timeout 秒] [--strict-log] -- <命令> [参数…]
#   结果目录缺省 $DM_ROOT/run/dmtest/<名字>_<时间>_<pid>；退出码 0 = ok，1 = red，2 = error / 用法
# A uniform test-result wrapper: runs any test command as usual (touchchain / smoke / accept / make ... test), passes its output through unchanged, and reads the verdict from the existing verdict lines,
# writing a machine-readable result (scripts / agents / CI no longer each grep the Chinese lines). The wrapped command is not changed; it may put extra files in $DMTEST_OUT.
#   Lines read: "NAME [case] OK|RED|FAIL|PASS ..." verdict lines, "RESULT {json}" result lines, ⚪ / ⏭ unavailable and skipped lines, 【红】 / 🔴 / FATAL / CRASH sig= red-marker lines
#   Verdict: ok = exit code 0 and no RED / FAIL verdict; red = non-zero exit code or a RED / FAIL verdict (with --strict-log red-marker lines count too); error = timeout / the command could not start
#   Outputs: <result dir>/log.txt (full output), result.json; --junit also writes JUnit XML; last line RESULT {json}
# Usage: dmtest.py [--name NAME] [--out DIR] [--junit FILE] [--timeout SEC] [--strict-log] -- <command> [args...]
#   The result directory defaults to $DM_ROOT/run/dmtest/<name>_<time>_<pid>; exit code 0 = ok, 1 = red, 2 = error / usage
import argparse
import datetime
import json
import os
import platform
import re
import signal
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

VERDICT = re.compile(r'^\s*([A-Z][A-Z0-9-]{2,}(?: \S+)?) (OK|RED|FAIL|PASS)\b\s*(.*)$')   # 名字后可带一个用例名（TOUCHCHAIN move OK） | the name may carry one case name (TOUCHCHAIN move OK)
RESULT = re.compile(r'^\s*RESULT (\{.*\})\s*$')
UNAVAIL = re.compile(r'^\s*(⚪|[A-Z][A-Z0-9-]{2,} \S+ 不可用)')
SKIPPED = re.compile(r'^\s*⏭')
RED = re.compile(r'【红】|🔴|\bFATAL\b|CRASH sig=')
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
KEEP = 50   # 每类最多留几行进 result.json（计数照记全部） | at most this many lines of each kind go into result.json (counts include all)


class Scan:
    def __init__(self):
        self.verdicts, self.results, self.unavailable, self.skipped, self.red, self.red_n = [], [], [], [], [], 0

    def line(self, raw, n):
        s = ANSI.sub('', raw.rstrip('\r\n'))
        m = RESULT.match(s)
        if m:
            try: self.results.append(json.loads(m.group(1)))
            except ValueError: pass
            return
        m = VERDICT.match(s)
        if m: self.verdicts.append(dict(name=m.group(1), verdict=m.group(2), detail=m.group(3)[:300], line=n))
        if UNAVAIL.match(s) and len(self.unavailable) < KEEP: self.unavailable.append(s.strip()[:300])
        if SKIPPED.match(s) and len(self.skipped) < KEEP: self.skipped.append(s.strip()[:300])
        if RED.search(s):
            self.red_n += 1
            if len(self.red) < KEEP: self.red.append(dict(line=n, text=s.strip()[:300]))


def git_info(cwd):
    def g(*a):
        try: return subprocess.run(['git', '-C', cwd, *a], capture_output=True, text=True, timeout=10, check=False).stdout.strip()
        except (OSError, subprocess.SubprocessError): return ''
    rev = g('rev-parse', '--short=12', 'HEAD')
    return dict(rev=rev, branch=g('rev-parse', '--abbrev-ref', 'HEAD'), dirty=bool(g('status', '--porcelain', '--untracked-files=no'))) if rev else {}


def run(cmd, log_path, timeout, env):
    """跑命令，输出逐行转发并写日志；返回 (退出码, 是否超时, 扫描结果)。 | Runs the command, forwarding output line by line and logging it; returns (exit code, timed out, scan)."""
    sc, timed_out = Scan(), False
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    except OSError as e:
        with open(log_path, 'w', encoding='utf-8') as f: f.write(f'dmtest: 命令起不来 | cannot start command: {e}\n')
        return 127, False, sc
    def stop():   # 超时：整组先 TERM，10 秒后 KILL | timeout: TERM the whole group, KILL after 10 s
        nonlocal timed_out
        timed_out = True
        for sig, wait in ((signal.SIGTERM, 10), (signal.SIGKILL, 0)):
            try: os.killpg(p.pid, sig)
            except OSError: return
            try: p.wait(wait); return
            except subprocess.TimeoutExpired: pass
    timer = threading.Timer(timeout, stop) if timeout else None
    if timer: timer.daemon = True; timer.start()
    assert p.stdout is not None
    with open(log_path, 'w', encoding='utf-8') as f:
        for n, b in enumerate(iter(p.stdout.readline, b''), 1):
            s = b.decode('utf-8', 'replace')
            sys.stdout.write(s); sys.stdout.flush(); f.write(s)
            sc.line(s, n)
    rc = p.wait()
    if timer: timer.cancel()
    return rc, timed_out, sc


def verdict_of(rc, timed_out, sc, strict, rc_only=False):
    if timed_out or rc in (126, 127): return 'error'
    if rc_only: return 'red' if rc != 0 else 'ok'   # 测试套：退出码就是结论（套里的负向用例自己会打出 RED 判定行） | test suites: the exit code is the verdict (the suite's own negative cases print RED verdict lines)
    bad = [v for v in sc.verdicts if v['verdict'] in ('RED', 'FAIL')] + [r for r in sc.results if str(r.get('verdict', '')).upper() in ('RED', 'FAIL')]
    return 'red' if rc != 0 or bad or (strict and sc.red_n) else 'ok'


def junit(res, path, log_tail):
    """每条判定行 / 结果行一个用例；都没有时整条命令一个用例。 | One test case per verdict / result line; one for the whole command when there are none."""
    suite = ET.Element('testsuite', name=res['name'], timestamp=res['started'], time=f"{res['elapsed_s']:.3f}")
    cases = []
    for v in res['verdicts']: cases.append((v['name'], v['verdict'] in ('RED', 'FAIL'), v['detail']))
    for r in res['results']: cases.append((str(r.get('name', 'result')), str(r.get('verdict', '')).upper() in ('RED', 'FAIL'), json.dumps(r, ensure_ascii=False)[:500]))
    if not cases or (res['verdict'] != 'ok' and not any(c[1] for c in cases)):
        first = res['red_lines']['first'][0]['text'] if res['red_lines']['first'] else ''
        cases.append((res['name'], res['verdict'] != 'ok', f"exit {res['rc']}" + (' (timeout)' if res['timed_out'] else '') + (f': {first}' if first else '')))
    fails = errs = 0
    for name, failed, detail in cases:
        tc = ET.SubElement(suite, 'testcase', classname=f"dmtest.{res['name']}", name=name)
        if failed and res['verdict'] == 'error': ET.SubElement(tc, 'error', message=detail[:200]).text = detail; errs += 1
        elif failed: ET.SubElement(tc, 'failure', message=detail[:200]).text = detail; fails += 1
    for u in res['unavailable'] + res['skipped']:
        ET.SubElement(ET.SubElement(suite, 'testcase', classname=f"dmtest.{res['name']}", name=u[:80]), 'skipped', message=u[:200])
    suite.set('tests', str(len(suite))); suite.set('failures', str(fails)); suite.set('errors', str(errs))
    suite.set('skipped', str(len(res['unavailable']) + len(res['skipped'])))
    ET.SubElement(suite, 'system-out').text = log_tail
    root = ET.Element('testsuites'); root.append(suite)
    ET.indent(root); ET.ElementTree(root).write(path, encoding='utf-8', xml_declaration=True)


def main(argv):
    ap = argparse.ArgumentParser(prog='dmtest.py', description='Run a test command and write result.json / JUnit XML from its verdict lines.')
    ap.add_argument('--name'); ap.add_argument('--out'); ap.add_argument('--junit'); ap.add_argument('--timeout', type=float, default=0)
    ap.add_argument('--strict-log', action='store_true', help='red-marker lines (【红】 / 🔴 / FATAL / CRASH) make the run red')
    ap.add_argument('--rc-only', action='store_true', help='the verdict comes from the exit code only (for test suites whose negative cases print RED lines); lines are still recorded')
    ap.add_argument('cmd', nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ['--'] else a.cmd
    if not cmd: ap.print_usage(sys.stderr); return 2
    name = a.name or re.sub(r'\.(sh|py)$', '', os.path.basename(cmd[0])) or 'test'
    started = datetime.datetime.now().astimezone()
    out = a.out or os.path.join(os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work'), 'run', 'dmtest', f"{name}_{started:%Y%m%d_%H%M%S}_{os.getpid()}")
    os.makedirs(out, exist_ok=True)
    t0 = time.monotonic()
    rc, timed_out, sc = run(cmd, os.path.join(out, 'log.txt'), a.timeout, dict(os.environ, DMTEST_OUT=out))
    res = dict(schema='dmtest/1', name=name, cmd=cmd, cwd=os.getcwd(), started=started.isoformat(timespec='seconds'), elapsed_s=round(time.monotonic() - t0, 3),
               rc=rc, timed_out=timed_out, verdict=verdict_of(rc, timed_out, sc, a.strict_log, a.rc_only),
               verdicts=sc.verdicts[-KEEP:], results=sc.results[-KEEP:], unavailable=sc.unavailable, skipped=sc.skipped,
               red_lines=dict(count=sc.red_n, first=sc.red), git=git_info(os.path.dirname(os.path.abspath(__file__))),
               host=dict(os=platform.system(), release=platform.release(), machine=platform.machine()), log='log.txt')
    with open(os.path.join(out, 'result.json'), 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1); f.write('\n')
    if a.junit:
        with open(os.path.join(out, 'log.txt'), encoding='utf-8', errors='replace') as f: tail = ''.join(f.readlines()[-200:])
        junit(res, a.junit, tail)
    print('RESULT ' + json.dumps(dict(name='dmtest', test=name, verdict=res['verdict'], rc=rc, timed_out=timed_out, elapsed_s=res['elapsed_s'],
                                      verdicts=len(sc.verdicts), red_lines=sc.red_n, first_red=sc.red[0]['text'] if sc.red else '', unavailable=len(sc.unavailable), dir=out), ensure_ascii=False))
    return dict(ok=0, red=1).get(res['verdict'], 2)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
