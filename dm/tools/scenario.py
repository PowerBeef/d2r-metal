#!/usr/bin/env python3
# 场景运行器：按场景文件（dm/verify/scenarios/*.scn）一步一步驱动游戏（控制口 dm/verify/control_port.md），每步等它生效再走下一步，末尾按检查项判 OK / RED。
# 场景文件是一行一步的文本，用的是触屏脚本的那套动作名（tap_ui、await_ui、walk_to、tap_unit、shot …），但不按时刻排，按顺序做；格式见 dm/verify/scenarios/README.md。
#   后端：cli（Mac 命令行宿主，缺省）、mac-app（玩家装的 Mac App，DM_APP_SUPPORT 隔开存档与日志）、ios-mac（Mac 上跑的 iOS 包，走日志端口）、device（真机：devicectl 拉起 + iproxy 转日志端口）
#   用法：scenario.py run <场景文件>… [--backend cli|mac-app|ios-mac|device] [--out 目录] [--headless] [--metal-validation api|shader|all|lint]
#         scenario.py check <场景文件>…      只解析、不跑（语法 / 动作名 / 参数）
#   每个场景打「SCENARIO <名> OK|RED …」判定行（dmtest.py 认得），末行 RESULT {json}；结果目录里有 host.log、events.ndjson、port.log、steps.tsv 与截图
#   跑完都查一遍日志里的消毒器报告（SCN_DMHOST=dm/host/dmhost_asan 等，有就判红，汇总进 sanitizer.txt）；--metal-validation 开 Metal 校验层，消息汇总进 metal_validation.txt（lint 只汇总、不判红）
# Scenario runner: drives the game step by step from a scenario file (dm/verify/scenarios/*.scn) through the control port (dm/verify/control_port.md), waits for each step to take effect before the next, and judges OK / RED by the checks at the end.
# A scenario file is text with one step per line, using the touch script's action names (tap_ui, await_ui, walk_to, tap_unit, shot ...), but run in order instead of by time; the format is in dm/verify/scenarios/README.md.
#   Backends: cli (Mac command-line host, default), mac-app (the Mac App players install, saves and logs kept apart with DM_APP_SUPPORT), ios-mac (the iOS package running on a Mac, over the log port), device (real device: launched with devicectl, log port forwarded through iproxy)
#   Usage: scenario.py run <scenario file>... [--backend cli|mac-app|ios-mac|device] [--out dir] [--headless] [--metal-validation api|shader|all|lint]
#          scenario.py check <scenario file>...   parse only, do not run (syntax / action names / arguments)
#   Each scenario prints a "SCENARIO <name> OK|RED ..." verdict line (dmtest.py understands it), last line RESULT {json}; the result directory holds host.log, events.ndjson, port.log, steps.tsv and the screenshots
#   Every run then checks the log for sanitizer reports (SCN_DMHOST=dm/host/dmhost_asan and the like; any report is red, summarized in sanitizer.txt); --metal-validation turns on the Metal validation layers, with the messages summarized in metal_validation.txt (lint only summarizes, never red)
import argparse
import datetime
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import dmctl  # noqa: E402

DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
# 控制口命令的位置参数（其余用 名=值） | positional arguments of the control-port commands (the rest as name=value)
POS = {'press': ['keys', 'frames'], 'stick': ['x', 'y', 'frames'], 'tap': ['x', 'y', 'hold'], 'wheel': ['x', 'y', 'dy', 'precise', 'view'], 'drag': ['x0', 'y0', 'x1', 'y1', 'frames'], 'msg': ['spec'], 'world_tap': ['x', 'y'],
       'walk_to': ['x', 'y', 'timeout'], 'screenshot': ['path', 'frames'], 'ui_dump': ['want'], 'step': ['n'], 'save_now': ['timeout'],
       'tap_ui': ['name', 'timeout'], 'await_ui': ['name', 'timeout'], 'tap_unit': ['type', 'cls', 'timeout'], 'app': ['do'], 'quit': ['save'],
       'pause': [], 'resume': [], 'wait': [], 'dump': [], 'state': [], 'ping': [], 'mark': ['name'], 'crash': ['kind']}
ALIAS = {'shot': 'screenshot'}   # 触屏脚本的名字 | touch-script names
MACROS = ('enter_game', 'open_pause', 'close_pause', 'background', 'system_quit', 'swipe_away')
LOCAL = ('expect', 'expect_no', 'expect_state', 'sleep', 'log', 'remember', 'export_save', 'gpu_capture')
HEADER = ('name', 'save', 'env', 'timeout', 'window', 'requires')
CHECKS = ('no_red', 'no_red_log', 'saved', 'exit', 'event', 'no_event')
BAD = ('red', 'fatal', 'crash', 'watchdog')


class ScenarioError(Exception):
    pass


def value(v):
    return dmctl._value(v)   # noqa: SLF001 -- 同一套「true / 数 / 串」换算 | the same "true / number / string" conversion


def parse(path):
    """返回 (表头, 步骤, 检查)；步骤 = {op, args, kw, line, n}。 | Returns (header, steps, checks); a step = {op, args, kw, line, n}."""
    meta: dict[str, Any] = dict(name=os.path.splitext(os.path.basename(path))[0], save='warlock', env={}, timeout=300, window=1, requires=[])
    steps: list[dict[str, Any]] = []; checks: list[dict[str, Any]] = []
    for n, raw in enumerate(open(path, encoding='utf-8'), 1):
        try: tok = shlex.split(raw, comments=True)
        except ValueError as e: raise ScenarioError(f'{path}:{n}: {e}') from e
        if not tok: continue
        op, rest = ALIAS.get(tok[0], tok[0]), tok[1:]
        args = [t for t in rest if '=' not in t or t.startswith('=')]
        kw = dict(t.split('=', 1) for t in rest if '=' in t and not t.startswith('='))
        if op in HEADER:
            if op == 'env': meta['env'].update(kw)
            elif op == 'requires': meta['requires'] = args
            elif op in ('timeout', 'window'): meta[op] = int(args[0]) if args else meta[op]
            elif args: meta[op] = args[0]
            continue
        if op == 'check':
            if not args or args[0] not in CHECKS and not args[0].startswith('exit'): raise ScenarioError(f'{path}:{n}: unknown check {args[:1]} (known: {", ".join(CHECKS)})')
            checks.append(dict(op=args[0], args=args[1:], kw=kw, line=raw.strip(), n=n)); continue
        if op not in POS and op not in MACROS and op not in LOCAL: raise ScenarioError(f'{path}:{n}: unknown step {op!r}')
        if op in POS:
            if len(args) > len(POS[op]): raise ScenarioError(f'{path}:{n}: {op} takes at most {len(POS[op])} positional arguments ({" ".join(POS[op])})')
            kw = {**dict(zip(POS[op], args, strict=False)), **kw}; args = []
        if op in ('expect', 'expect_no') and not args: raise ScenarioError(f'{path}:{n}: {op} needs an event type')
        if op == 'sleep' and (not args or not re.fullmatch(r'[\d.]+', args[0])): raise ScenarioError(f'{path}:{n}: sleep needs seconds')
        if op == 'remember' and not args: raise ScenarioError(f'{path}:{n}: remember needs a state field')
        if op == 'export_save' and (not args or not re.fullmatch(r'[a-z0-9_]+', args[0])): raise ScenarioError(f'{path}:{n}: export_save needs a name of a-z 0-9 _')
        steps.append(dict(op=op, args=args, kw=kw, line=raw.split('#')[0].strip(), n=n))
    return meta, steps, checks


# ---------- 后端 | Backends ----------
SAVES = os.path.join(DM, 'verify', 'scenarios', 'saves')   # 生成存档的场景：saves/<名>.scn 导出 save_ref_<名> | scenarios that generate saves: saves/<name>.scn exports save_ref_<name>


def save_dir(name):
    return name if os.path.isabs(name) else os.path.join(DM_ROOT, 'data', 'switch', f'save_ref_{name}', '0000000000000001', '0')


def ensure_save(name, headless):
    """场景要的存档不在、又有同名的生成场景：先在命令行宿主上跑它（生成的存档留在本机工作区，不入库）。 | When the save a scenario needs is missing and a generator scenario of the same name exists: run it on the CLI host first (the generated save stays in the local workspace, never committed)."""
    if name in ('empty',) or os.path.isdir(save_dir(name)): return
    gen = os.path.join(SAVES, f'{name}.scn')
    if not os.path.exists(gen): return
    print(f'存档 save_ref_{name} 不在，先跑生成场景 | save_ref_{name} missing, running its generator: {os.path.relpath(gen, DM)}', flush=True)
    res, code = Runner(gen, 'cli', None, headless).go()
    if code or not os.path.isdir(save_dir(name)): raise ScenarioError(f'generator {os.path.basename(gen)} did not produce save_ref_{name}: {res.get("reason")}')


def prepare_save(meta, run):
    s = meta['save']
    dst = os.path.join(run, 'save')
    if s == 'empty': os.makedirs(dst, exist_ok=True); return dst
    src = save_dir(s)
    if not os.path.isdir(src): raise ScenarioError(f'save {s!r}: {src} not found (create it with dm/ios/tools/make_fixture.sh or name an existing save_ref_*)')
    shutil.copytree(src, dst)
    return dst


class CliBackend:
    """Mac 命令行宿主：控制端先占好监听口，宿主带 DM_CTL_PORT 连过来。 | Mac command-line host: the controller takes the listening socket first, and the host connects in with DM_CTL_PORT."""
    name = 'cli'

    def __init__(self, meta, run, headless):
        self.meta, self.run, self.headless, self.proc, self.log = meta, run, headless, None, None

    def start(self):
        dmhost = os.environ.get('SCN_DMHOST')   # 另指一个宿主可执行文件（基准 A/B 比两个构建时用），不重编 | point at another host executable (benchmark A/B between two builds), without rebuilding
        if dmhost and not os.access(dmhost, os.X_OK): raise ScenarioError(f'SCN_DMHOST {dmhost} is not executable')
        if not dmhost:
            dmhost = os.path.join(DM, 'host', 'dmhost')
            if subprocess.run(['make', '-s', '-C', os.path.join(DM, 'host'), 'dmhost'], capture_output=True, check=False).returncode or not os.access(dmhost, os.X_OK):
                raise ScenarioError('host build failed (make -C dm/host dmhost)')
        out = subprocess.run(['bash', os.path.join(DM, 'tools', 'product.sh'), 'path', 'touch'], capture_output=True, text=True, check=False).stdout.strip()
        if not out or not os.path.isdir(out): raise ScenarioError('no rewrite output: run ./setup.sh first')
        romfs = os.environ.get('DM_ROMFS') or os.path.join(DM_ROOT, 'data', 'switch', '3.3.94012', 'Update', 'RomFS')
        if not os.path.isdir(romfs): raise ScenarioError(f'no game resources at {romfs} (set DM_ROMFS)')
        os.makedirs(os.path.join(self.run, 'tmp'), exist_ok=True)
        srv, port = dmctl.bind()
        env = dict(os.environ, DM_ROMFS=romfs, DM_SAVE=prepare_save(self.meta, self.run), DM_TEMP=os.path.join(self.run, 'tmp'),
                   DM_MSL_DIR=os.path.join(DM_ROOT, 'build', 'cache', 'metal_shader_cache'), DM_CTL_PORT=str(port),
                   DM_EVENTS=os.path.join(self.run, 'events.ndjson'), DM_WATCH=str(self.meta['timeout'] + 60), DM_CRASH_JSON=os.path.join(self.run, 'crash.json'))
        if self.meta['window'] and not self.headless: env['DM_WINDOW'] = '1'
        env.update(self.meta['env'])
        self.log = open(os.path.join(self.run, 'host.log'), 'wb')
        self.proc = subprocess.Popen([dmhost, out, 'main', 'sdk'], cwd=os.path.join(DM, 'host'), env=env, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try: return dmctl.Ctl(server=srv, connect_timeout=30, log=os.path.join(self.run, 'port.log'))
        except OSError as e: self.stop(); raise ScenarioError(f'host did not connect to the control port: {e}') from e

    def alive(self): return self.proc is not None and self.proc.poll() is None
    def pid(self): return self.proc.pid if self.alive() and self.proc else None
    def wait_exit(self, timeout):
        try: return self.proc.wait(timeout) if self.proc else None
        except subprocess.TimeoutExpired: return None
    def kill(self):
        if self.proc and self.proc.poll() is None: self.proc.kill(); self.proc.wait()
    def exit_code(self): return self.proc.returncode if self.proc else None
    def stop(self):
        self.kill()
        if self.log: self.log.close()
    def shot_path(self, p): return p if os.path.isabs(p) else os.path.join(self.run, p)
    def event_file(self): return os.path.join(self.run, 'events.ndjson')
    def log_file(self): return os.path.join(self.run, 'host.log')


class MacAppBackend(CliBackend):
    """Mac App（玩家装的那个包）：直接起包里的可执行文件当子进程，DM_APP_SUPPORT 指到结果目录里的 support/（存档副本、设置、日志都在那里，iCloud 不同步，
    玩家的存档与日志不动），DM_APP_ROMFS 给资源。测试用的包缺省是 $DM_ROOT/out/macapp_test/D2R Mobile.app（没有就用 dm/tools/mac_app.sh 出一个；DM_MAC_APP 可另指）。
    The Mac App (the package players install): its executable runs directly as a child process, with DM_APP_SUPPORT pointing at support/ in the result directory (the save copy, settings and logs live there, iCloud does not sync,
    and the player's saves and logs are untouched) and DM_APP_ROMFS giving the resources. The test package defaults to $DM_ROOT/out/macapp_test/D2R Mobile.app (made with dm/tools/mac_app.sh when missing; DM_MAC_APP points elsewhere)."""
    name = 'mac-app'

    def start(self):
        app = os.environ.get('DM_MAC_APP') or os.path.join(DM_ROOT, 'out', 'macapp_test', 'D2R Mobile.app')
        if not os.path.isdir(app):
            print(f'出测试用的 Mac App | building the test Mac App: {app}', flush=True)
            p = subprocess.run(['bash', os.path.join(DM, 'tools', 'mac_app.sh'), os.path.dirname(app)], capture_output=True, text=True, check=False)
            if p.returncode or not os.path.isdir(app): raise ScenarioError('mac_app.sh failed: ' + (p.stdout + p.stderr).strip()[-300:])
        import plistlib   # noqa: PLC0415
        with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f: exe = os.path.join(app, 'Contents', 'MacOS', plistlib.load(f).get('CFBundleExecutable', ''))
        if not os.access(exe, os.X_OK): raise ScenarioError(f'no executable in {app}')
        romfs = os.environ.get('DM_APP_ROMFS') or os.path.join(DM_ROOT, 'data', 'switch', '3.3.94012', 'Update')
        sup = os.path.join(self.run, 'support'); os.makedirs(sup, exist_ok=True)
        save = prepare_save(self.meta, self.run); shutil.move(save, os.path.join(sup, 'save'))
        srv, port = dmctl.bind()
        env = {k: v for k, v in os.environ.items() if k not in ('DM_SAVE', 'DM_TEMP', 'DM_ROMFS', 'DM_ROOT')}
        env.update(DM_APP_SUPPORT=sup, DM_APP_ROMFS=romfs, DM_CTL_PORT=str(port), DM_EVENTS=os.path.join(self.run, 'events.ndjson'), DM_AUDIO='0', DM_WATCH=str(self.meta['timeout'] + 60),
                   DM_CRASH_JSON=os.path.join(self.run, 'crash.json'))
        env.update(self.meta['env'])
        self.log = open(os.path.join(self.run, 'launch.log'), 'wb')
        self.proc = subprocess.Popen([exe], env=env, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try: return dmctl.Ctl(server=srv, connect_timeout=60, log=os.path.join(self.run, 'port.log'))
        except OSError as e: self.stop(); raise ScenarioError(f'the Mac App did not connect to the control port: {e}') from e

    def log_file(self): return os.path.join(self.run, 'support', 'Logs', 'host.log')


class LaunchBackend:
    """iOS 包（Mac 上的 ios-mac / 真机 device）：dm/ios/tools/scenario_launch.sh 一个会话一个进程（设备锁跟着它走），带着冒烟的那套守卫拉起，
    经日志端口控制；事件随日志（DM_EVENTS=log）。向它的标准输入发 alive / kill / stop，每条回一行。
    The iOS package (ios-mac on a Mac / device): dm/ios/tools/scenario_launch.sh is one process per session (the device lock belongs to it) and launches with the smoke tests' guards;
    control goes over the log port and events travel with the log (DM_EVENTS=log). alive / kill / stop are sent to its stdin, one reply line each."""

    def __init__(self, name, meta, run):
        self.name, self.meta, self.run, self.ended, self.p = name, meta, run, False, None

    def _ask(self, cmd, timeout=60):
        if not self.p or self.p.poll() is not None or not self.p.stdin or not self.p.stdout: return ''
        try: self.p.stdin.write(cmd + '\n'); self.p.stdin.flush()
        except OSError: return ''
        end = time.time() + timeout
        while time.time() < end:
            line = self.p.stdout.readline()
            if not line: return ''
            if line.strip() in ('yes', 'no', 'done') or line.startswith('unknown'): return line.strip()
            self.logf.write(line)
        return ''

    def start(self):
        env = dict(os.environ, SCN_RUN=self.run, SCN_SAVE=self.meta['save'], SCN_ENV=' '.join(f'{k}={v}' for k, v in self.meta['env'].items()))
        self.logf = open(os.path.join(self.run, 'launch.log'), 'a', encoding='utf-8')
        self.p = subprocess.Popen(['bash', os.path.join(DM, 'ios', 'tools', 'scenario_launch.sh'), self.name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.logf, text=True, env=env)
        assert self.p.stdout is not None
        port, lines = None, []
        for line in self.p.stdout:   # 拿锁可能要排队：读到 PORT 或进程结束为止 | taking the lock may queue: read until PORT or the process ends
            self.logf.write(line); lines.append(line.strip())
            m = re.match(r'^PORT (\d+)$', line.strip())
            if m: port = int(m.group(1)); break
        if not port: self.stop(); raise ScenarioError(f'{self.name} launch failed: {lines[-1] if lines else "no output"} (see launch.log)')
        return dmctl.Ctl(port, connect_timeout=90, log=os.path.join(self.run, 'port.log'))

    def alive(self): return not self.ended and self._ask('alive', 30) == 'yes'
    def pid(self): return None   # iOS 包的 GPU 截帧走 Xcode | GPU capture of the iOS package goes through Xcode
    def wait_exit(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if not self.alive(): self.ended = True; return 0
            time.sleep(0.5)
        return None
    def kill(self): self._ask('kill'); self.ended = True
    def exit_code(self): return 0 if self.ended else None
    def stop(self):
        if self.p and self.p.poll() is None:
            self._ask('stop', 60)
            try: self.p.wait(10)
            except subprocess.TimeoutExpired: self.p.kill()
        self.ended = True
    def shot_path(self, p): return p   # iOS：相对路径落在 App 的 Documents 下 | iOS: relative paths land under the App's Documents
    def event_file(self): return None
    def log_file(self): return os.path.join(self.run, 'port.log')   # 日志端口收下来的整份日志 | the whole log received over the log port


# ---------- 运行 | Running ----------
# --metal-validation 各档的环境（man MetalValidation）：api = 接口校验，错误记日志不断言、警告不报；lint = 再把警告（多余 / 没用上的状态设置等，性能上的提示）也记下来；
# shader = 着色器校验（管线现场插桩重编，每条约 0.15 秒，停顿阈值放宽到 300 秒），报到 stderr；all = api + shader
# Environment of each --metal-validation level (man MetalValidation): api = API validation, errors logged instead of asserting, warnings not reported; lint = also log the warnings (redundant / unused state settings and the like, performance hints);
# shader = shader validation (pipelines are instrumented and recompiled on the spot, about 0.15 s each, so the stall threshold is raised to 300 s), reported to stderr; all = api + shader
METAL_VALIDATION = {
    'api': dict(MTL_DEBUG_LAYER='1', MTL_DEBUG_LAYER_ERROR_MODE='nslog', MTL_DEBUG_LAYER_WARNING_MODE='ignore'),
    'lint': dict(MTL_DEBUG_LAYER='1', MTL_DEBUG_LAYER_ERROR_MODE='nslog', MTL_DEBUG_LAYER_WARNING_MODE='nslog'),
    'shader': dict(MTL_SHADER_VALIDATION='1', MTL_SHADER_VALIDATION_REPORT_TO_STDERR='1', DM_WATCH_STALL='300'),
}
METAL_VALIDATION['all'] = dict(METAL_VALIDATION['api'], **METAL_VALIDATION['shader'])


class Runner:
    def __init__(self, path, backend_name, out, headless, metal=None):
        self.path = path
        self.meta, self.steps, self.checks = parse(path)
        stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
        self.run = out or os.path.join(DM_ROOT, 'run', 'scenario', f"{self.meta['name']}_{backend_name}_{stamp}_{os.getpid()}")
        os.makedirs(self.run, exist_ok=True)
        self.backend = CliBackend(self.meta, self.run, headless) if backend_name == 'cli' else MacAppBackend(self.meta, self.run, headless) if backend_name == 'mac-app' else LaunchBackend(backend_name, self.meta, self.run)
        if any(st['op'] == 'gpu_capture' for st in self.steps): self.meta['env'].setdefault('MTL_CAPTURE_ENABLED', '1')   # 进程得带它起才截得了帧 | the process must start with it to be capturable
        self.metal = metal
        for k, v in METAL_VALIDATION.get(metal or '', {}).items(): self.meta['env'].setdefault(k, v)
        self.on_started: Callable[[Any], None] | None = None   # 回调（后端）：连上之后、第一步之前调，基准测试用它挂系统读数 | callback(backend): called after connecting and before the first step; benchmarks attach system readings with it
        self.c: dmctl.Ctl | None = None; self.prev_mark = -1; self.records: list[dict[str, Any]] = []
        self.vars: dict[str, Any] = {}   # remember 记下的值，后面的值里写 $名 或 $名+N / $名-N 引用 | values kept by remember, referenced later as $name or $name+N / $name-N

    @property
    def cc(self) -> dmctl.Ctl:   # 步骤里用：这时一定已连上 | used inside steps: always connected by then
        assert self.c is not None
        return self.c

    def say(self, line):
        print(line, flush=True)

    # 步骤 | steps
    def ctl(self, cmdname, **kw):   # 参数可能就叫 name（mark、tap_ui）：命令名用别的名字 | an argument may be called name (mark, tap_ui): the command name uses another name
        if cmdname == 'screenshot' and 'path' in kw: kw['path'] = self.backend.shot_path(kw['path'])
        args: dict[str, Any] = {k: value(v) if not isinstance(v, (int, float, bool)) else v for k, v in kw.items()}
        return self.cc.cmd(cmdname, reply_timeout=float(kw.get('timeout', 0) or 0) + 120, **args)

    def expect(self, ev, kw, after):
        kw = dict(kw); t = float(kw.pop('timeout', 30))
        return self.cc.wait_for(ev, timeout=t, after=after, **{k: value(v) for k, v in kw.items()})

    def expect_state(self, kw):
        kw = dict(kw); t = float(kw.pop('timeout', 5)); conds = []
        for k, v in kw.items():   # 名=值；名 后面可带 > < >= <= !（如 hp>=1 写成 hp>=1 也行） | name=value; the name may end in > < >= <= ! (hp>=1 works as written)
            m = re.fullmatch(r'(\w+)([<>!]?)', k)
            if not m: raise ScenarioError(f'bad condition {k}={v}')
            conds.append((m.group(1), m.group(2) + '=' if m.group(2) in ('<', '>', '!') else '=', value(v) if isinstance(v, str) else v))
        def ok(st, f, op, v):
            x = st.get(f)
            if op == '=': return x == v or str(x) == str(v)
            if op == '!=': return x != v and str(x) != str(v)
            try: return x >= v if op == '>=' else x <= v
            except TypeError: return False
        end = time.time() + t
        while True:
            last = self.cc.cmd('state')
            bad = [f'{f}{op}{v} (is {last.get(f)!r})' for f, op, v in conds if not ok(last, f, op, v)]
            if not bad: return last
            if time.time() > end: raise ScenarioError('state: ' + '; '.join(bad))
            time.sleep(0.2)

    def macro(self, op, kw):
        if op == 'enter_game':
            st = self.cc.cmd('state')
            if st.get('in_game'): return 'already in a game'
            self.cc.wait_for('main_menu', timeout=float(kw.get('timeout', 120)))
            m = self.cc.mark(); self.ctl('press', keys='X')
            self.cc.wait_for('panel', timeout=30, after=m, act='OpenPanel', name='CharacterSelectPanel'); time.sleep(1)
            m = self.cc.mark(); self.ctl('msg', spec=f"CharacterSelect:LoadCharacter:{kw.get('slot', 0)}")
            self.cc.wait_for('game_enter', timeout=60, after=m); self.cc.wait_for('loading_done', timeout=120, after=m)
            return 'in game'
        if op == 'open_pause':   # 局内 ＋ 开总面板，暂停页在默认页签左边第三个 | in a game + opens the combined panel; the pause page is the third tab left of the default
            m = self.cc.mark(); self.ctl('press', keys='PLUS')
            self.cc.wait_for('panel', timeout=10, after=m, act='OpenPanel', name='UISwitcher')
            for _ in range(3): self.ctl('press', keys='L')
            self.cc.wait_for('pause_open', timeout=10, after=m)
            return 'pause menu open'
        if op == 'close_pause':
            m = self.cc.mark(); self.ctl('press', keys='B')
            self.cc.wait_for('panel', timeout=10, after=m, act='ClosePanel', name='UISwitcher')
            return 'closed'
        if op == 'background':   # 回来以前台事件为准：Mac 是 restored（还原），iOS 是 active（激活） | back means the foreground event: restored on the Mac, active on iOS
            self.ctl('app', do='background'); time.sleep(float(kw.get('for', 3)))
            m = self.cc.mark(); self.ctl('app', do='foreground')
            end = time.time() + 15
            while not any(e['ev'] == 'app' and e.get('state') in ('restored', 'active') and e.get('seq', 0) > m for e in list(self.cc.events)):
                if time.time() > end: raise ScenarioError('no foreground event (app state=restored / active) within 15 s')
                time.sleep(0.1)
            return 'background and back'
        if op == 'system_quit':
            self.ctl('app', do='terminate')
            code = self.backend.wait_exit(float(kw.get('timeout', 15)))
            if code is None: raise ScenarioError('process still running after terminate')
            return f'exited {code}'
        if op == 'swipe_away':   # 失活后直接杀进程（同在多任务界面划掉） | resign active, then kill the process outright (as swiping it away in the app switcher)
            self.ctl('app', do='resign') if self.backend.name != 'cli' else None
            time.sleep(float(kw.get('after', 0.5))); self.backend.kill()
            return 'killed'
        raise ScenarioError(f'unknown macro {op}')

    def subst(self, v):
        if not isinstance(v, str) or '$' not in v: return v
        m = re.fullmatch(r'\$(\w+)([+-]\d+(?:\.\d+)?)?', v)
        if not m or m.group(1) not in self.vars: raise ScenarioError(f'unknown variable in {v!r} (set it with remember)')
        x = self.vars[m.group(1)]
        return x + value(m.group(2)) if m.group(2) else x

    def do(self, st):
        op, kw = st['op'], {k: self.subst(v) for k, v in st['kw'].items()}
        if op in POS: r = self.ctl(op, **kw); return {k: v for k, v in r.items() if k not in ('id', 'cmd', 'ok')}
        if op in MACROS: return self.macro(op, kw)
        if op == 'expect': return self.expect(st['args'][0], kw, self.prev_mark)
        if op == 'expect_no':
            kw = dict(kw); t = float(kw.pop('for', kw.pop('timeout', 3)))
            try: e = self.cc.wait_for(st['args'][0], timeout=t, after=self.prev_mark, **{k: value(v) for k, v in kw.items()})
            except TimeoutError: return 'not seen'
            raise ScenarioError(f'unexpected event: {json.dumps(e, ensure_ascii=False)}')
        if op == 'expect_state': return self.expect_state(kw)
        if op == 'sleep': time.sleep(float(st['args'][0])); return None
        if op == 'gpu_capture':   # gpu_capture [frames=1] [path=frame.gputrace] [keep=1]：截 GPU 帧（gpucapture，只 cli / mac-app），结果带 gpudebug 的摘要 | capture GPU frames (gpucapture, cli / mac-app only); the result carries the gpudebug summary
            pid = self.backend.pid()
            if not pid: raise ScenarioError('gpu_capture needs the cli or mac-app backend')
            import gpucap   # noqa: PLC0415
            out = self.backend.shot_path(kw.get('path', 'frame.gputrace'))
            cap = gpucap.capture(pid, int(kw.get('frames', 1)), out, False)
            if not cap['ok']: raise ScenarioError(f"gpucapture failed: {cap['error']}")
            sm = gpucap.summary(out)
            kept = str(kw.get('keep', '0')) == '1'   # 一帧约 4 GB：缺省只留摘要，keep=1 才留截帧文件 | about 4 GB per frame: by default only the summary is kept, keep=1 keeps the capture file
            if not kept: shutil.rmtree(out, ignore_errors=True)
            return dict(path=out if kept else None, mb=round(cap['bytes'] / 1e6, 1), **{k: sm.get(k) for k in ('command_buffers', 'encoders', 'draws', 'dispatches')})
        if op == 'export_save':   # export_save 名 [overwrite=1]：存盘并等提交，把这局已提交的存档拷成 save_ref_<名>（只命令行宿主） | export_save name [overwrite=1]: save and wait for the commit, then copy this run's committed save to save_ref_<name> (CLI host only)
            if self.backend.name not in ('cli', 'mac-app'): raise ScenarioError('export_save works on the cli / mac-app backends only (the iOS backends use saves bundled at build time)')
            src_save = os.path.join(self.run, 'support', 'save') if self.backend.name == 'mac-app' else os.path.join(self.run, 'save')
            name = st['args'][0]; dst = save_dir(name)
            if name == 'warlock' or (os.path.exists(dst) and str(kw.get('overwrite', '0')) != '1'): raise ScenarioError(f'save_ref_{name} exists (overwrite=1 to replace; warlock is never replaced)')
            self.ctl('save_now', timeout=8)
            tmp = dst + f'.tmp{os.getpid()}'; shutil.rmtree(tmp, ignore_errors=True)
            shutil.copytree(src_save, tmp)
            shutil.rmtree(dst, ignore_errors=True); os.makedirs(os.path.dirname(dst), exist_ok=True); os.rename(tmp, dst)
            return f'save_ref_{name} written'
        if op == 'remember':   # remember 字段 [as=名]：把 state 的这个字段记下来 | remember field [as=name]: keep this state field
            st_now = self.cc.cmd('state'); f = st['args'][0] if st['args'] else ''
            if f not in st_now: raise ScenarioError(f'state has no field {f!r}')
            self.vars[kw.get('as', f)] = st_now[f]; return f"{kw.get('as', f)} = {st_now[f]!r}"
        if op == 'log': self.say('  # ' + ' '.join(st['args'])); return None
        raise ScenarioError(f'unknown step {op}')

    def events(self):
        f = self.backend.event_file()
        if f and os.path.exists(f):
            import dmevents   # noqa: PLC0415
            return dmevents.load(f)
        return list(self.c.events) if self.c else []

    def final_checks(self):
        evs, out = self.events(), []
        for ck in self.checks:
            op, ok, why = ck['op'], True, ''
            if op == 'no_red':
                allow = set(ck['kw'].get('allow', '').split(',')) - {''}
                bad = [e for e in evs if e['ev'] in BAD and e.get('code') not in allow]
                ok = not bad; why = json.dumps(bad[0], ensure_ascii=False)[:200] if bad else f'{len(evs)} events, none red'
            elif op == 'no_red_log':   # 日志里的红标行（含不经 LOG 的 plog 等，事件流看不到）；allow = 放行的正则 | red-marker lines in the log (including plog and others that bypass LOG, which the event stream does not see); allow = a regex to let through
                sys.path.insert(0, os.path.join(DM, 'verify')); import assertlib   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
                log = self.backend.log_file()
                bad = assertlib.logred(log, [ck['kw']['allow']] if ck['kw'].get('allow') else []) if os.path.exists(log) else [(0, f'no log {os.path.basename(log)}')]
                ok = not bad; why = f'{bad[0][0]}: {bad[0][1][:160]}' if bad else f'no red lines in {os.path.basename(log)}'
            elif op == 'saved':
                n = sum(1 for e in evs if e['ev'] == 'save' and e.get('result') == 'commit'); ok = n > 0; why = f'{n} save commits'
            elif op.startswith('exit'):
                want = int(ck['kw'].get('exit', ck['args'][0] if ck['args'] else 0) if op == 'exit' else op.split('=', 1)[1])
                code = self.backend.exit_code(); ok = code == want; why = f'exit code {code} (want {want})'
            elif op in ('event', 'no_event'):
                ev = ck['args'][0] if ck['args'] else ''
                hit = [e for e in evs if e['ev'] == ev and all(e.get(k) == value(v) or str(e.get(k)) == v for k, v in ck['kw'].items())]
                ok = bool(hit) if op == 'event' else not hit; why = f'{len(hit)} matching {ev} events'
            out.append(dict(check=ck['line'], ok=ok, detail=why))
            self.say(f"CHECK {'✓' if ok else '✗'} {ck['line']} — {why}")
        return out

    def log_scans(self):
        """跑完查日志：消毒器报告（总查，有就判红）与 Metal 校验层消息（开了 --metal-validation 才查；lint 只汇总）。汇总写进结果目录的 sanitizer.txt / metal_validation.txt。 | Log scans after the run: sanitizer reports (always; any is red) and Metal validation messages (only with --metal-validation; lint only summarizes). Summaries go to sanitizer.txt / metal_validation.txt in the result folder."""
        sys.path.insert(0, os.path.join(DM, 'verify')); import assertlib   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
        lf = getattr(self.backend, 'log_file', None)
        logs = [x for x in ((lf() if lf else None), os.path.join(self.run, 'launch.log')) if x and os.path.exists(x)]   # mac-app 的 stderr 在 launch.log | the mac-app stderr is in launch.log
        out = []
        san = [r for x in logs for r in assertlib.sanitizers(x)]
        if san:
            with open(os.path.join(self.run, 'sanitizer.txt'), 'w', encoding='utf-8') as f: f.writelines(f'{n}\t{k}\t{w}\n' for k, w, n in san)
            why = f'{sum(n for _, _, n in san)} reports, first: {san[0][0]}: {san[0][1][:140]}'
            out.append(dict(check='no sanitizer reports', ok=False, detail=why)); self.say(f'CHECK ✗ no sanitizer reports — {why}')
        if self.metal:
            mv = [r for x in logs for r in assertlib.metal_validation(x)]
            with open(os.path.join(self.run, 'metal_validation.txt'), 'w', encoding='utf-8') as f: f.writelines(f'{n}\t{t}\t{m}\n' for t, m, n in mv)
            ok = self.metal == 'lint' or not mv
            why = f'{sum(n for _, _, n in mv)} messages of {len(mv)} kinds' + (f', top: {mv[0][0]}: {mv[0][1][:120]}' if mv else '') + (' (lint: summary only)' if self.metal == 'lint' else '')
            out.append(dict(check=f'metal validation ({self.metal})', ok=ok, detail=why)); self.say(f"CHECK {'✓' if ok else '✗'} metal validation ({self.metal}) — {why}")
        return out

    def go(self):
        t0 = time.time(); fail: dict[str, Any] | None = None; name = self.meta['name']
        self.say(f'场景 | scenario {name}（{self.backend.name}，{len(self.steps)} 步 | steps）→ {self.run}')
        if self.meta['requires'] and self.backend.name not in self.meta['requires']:
            self.say(f"SCENARIO {name} SKIP 只在 {','.join(self.meta['requires'])} 上跑 | only runs on {','.join(self.meta['requires'])}")
            return dict(name=name, verdict='SKIP', backend=self.backend.name, dir=self.run), 0
        tsv = open(os.path.join(self.run, 'steps.tsv'), 'w', encoding='utf-8'); tsv.write('步 | step\t行 | line\t结果 | result\t秒 | s\t说明 | detail\n')
        try:
            self.c = self.backend.start()
            if self.on_started: self.on_started(self.backend)
            deadline = t0 + self.meta['timeout']
            for i, st in enumerate(self.steps, 1):
                if time.time() > deadline: fail = dict(step=i, line=st['line'], reason=f"scenario timeout {self.meta['timeout']} s"); break
                mark = self.cc.mark(); s = time.time()
                try: r = self.do(st); ok, detail = True, r
                except (ScenarioError, dmctl.CtlError, dmctl.BadEvent, TimeoutError, OSError) as e: ok, detail = False, str(e)
                dt = time.time() - s
                text = detail if isinstance(detail, str) else json.dumps(detail, ensure_ascii=False) if detail else ''
                self.say(f"STEP {i:>2} {'ok ' if ok else 'RED'} {dt:6.2f}s  {st['line']}" + (f'  → {text[:160]}' if text else ''))
                tsv.write(f"{i}\t{st['line']}\t{'ok' if ok else 'RED'}\t{dt:.3f}\t{text[:300]}\n")
                self.records.append(dict(step=i, line=st['line'], ok=ok, s=round(dt, 3), detail=text[:300]))
                self.prev_mark = mark
                if not ok: fail = dict(step=i, line=st['line'], reason=text); break
        except ScenarioError as e:
            fail = dict(step=0, line='start', reason=str(e))
        finally:
            tsv.close()
            if self.backend.alive() and self.c:
                try: self.c.cmd('quit', reply_timeout=5)
                except (dmctl.CtlError, TimeoutError, OSError): pass
                self.backend.wait_exit(10)
            if self.c: self.c.close()
            self.backend.stop()
        cj = os.path.join(self.run, 'crash.json')
        if os.path.exists(cj):   # 进程崩了：现场符号化进 crash.txt，系统的 .ips 也收进结果目录，判红原因直接说崩在哪 | the process crashed: the state is symbolicated into crash.txt, the system .ips is collected into the result folder, and the red reason says where it crashed
            import contextlib, io, crashlog   # noqa: E401, PLC0415
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                try: cr = crashlog.show(cj, None)
                except (OSError, ValueError, KeyError): cr = {}
                crashlog.collect('D2RMobile' if self.backend.name == 'mac-app' else 'dmhost', t0, self.run)
            with open(os.path.join(self.run, 'crash.txt'), 'w', encoding='utf-8') as f: f.write(buf.getvalue())
            top = next((fr for fr in cr.get('frames', []) if fr.get('name') or fr.get('where')), {})
            what = f"crash {cr.get('signal', '?')} at " + (' '.join(x for x in (top.get('where'), top.get('name')) if x) or '?')
            fail = dict(step=(fail or {}).get('step', len(self.steps)), line=(fail or {}).get('line', '(after the steps)'), reason=what + (f"; {fail['reason']}" if fail else ''))
        checks = (self.final_checks() if not fail or fail['step'] else []) + self.log_scans()
        bad_ck = [c for c in checks if not c['ok']]
        verdict = 'OK' if not fail and not bad_ck else 'RED'
        why = f"step {fail['step']} ({fail['line']}): {fail['reason'][:160]}" if fail else f'check: {bad_ck[0]["check"]} — {bad_ck[0]["detail"][:120]}' if bad_ck else f'{len(self.steps)} steps, {len(checks)} checks'
        self.say(f'SCENARIO {name} {verdict} {why} 用时 {time.time() - t0:.0f} 秒')
        res = dict(name=name, backend=self.backend.name, verdict=verdict, reason=why, failed=fail, steps=self.records, checks=checks, dir=self.run, elapsed_s=round(time.time() - t0, 1))
        with open(os.path.join(self.run, 'result.json'), 'w', encoding='utf-8') as f: json.dump(res, f, ensure_ascii=False, indent=1)
        return res, 0 if verdict == 'OK' else 1


def main(argv):
    ap = argparse.ArgumentParser(prog='scenario.py', description='Run scenario files through the control port (dm/verify/scenarios/README.md).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run'); r.add_argument('files', nargs='+'); r.add_argument('--backend', default='cli', choices=('cli', 'mac-app', 'ios-mac', 'device'))
    r.add_argument('--out'); r.add_argument('--headless', action='store_true')
    r.add_argument('--metal-validation', choices=sorted(METAL_VALIDATION), help='turn on the Metal validation layers; messages go to metal_validation.txt (lint: summary only)')
    c = sub.add_parser('check'); c.add_argument('files', nargs='+')
    a = ap.parse_args(argv)
    if a.cmd == 'check':
        bad = 0
        for f in a.files:
            try: meta, steps, checks = parse(f); print(f"SCENARIO-CHECK {meta['name']} OK {len(steps)} steps, {len(checks)} checks")
            except ScenarioError as e: bad += 1; print(f'SCENARIO-CHECK {os.path.basename(f)} RED {e}')
        return 1 if bad else 0
    results, rc = [], 0
    if a.metal_validation in ('shader', 'all') and not os.environ.get('SCN_DMHOST', '').endswith('_m3'):
        # macOS 27 实测：Metal 4 宿主开着色器校验时，第一次出图提交就崩在 Metal 自己的 -[MTL4GPUDebugCommandBuffer applyResidencySets:]（挂着图层的驻留集）；旧接口宿主正常
        # Measured on macOS 27: with shader validation the Metal 4 host crashes at the first present commit inside Metal's own -[MTL4GPUDebugCommandBuffer applyResidencySets:] (with the layer's residency set attached); the older-API host works
        print('NOTE 着色器校验在 Metal 4 宿主上会崩在 Metal 的校验层里，用旧接口宿主：make -C dm/host dmhost_m3 && SCN_DMHOST=dm/host/dmhost_m3 | shader validation crashes inside Metal\'s validation layer on the Metal 4 host; use the older-API host: make -C dm/host dmhost_m3 && SCN_DMHOST=dm/host/dmhost_m3')
    for f in a.files:
        try:
            ensure_save(parse(f)[0]['save'], a.headless)
            res, code = Runner(f, a.backend, a.out if len(a.files) == 1 else None, a.headless, metal=a.metal_validation).go()
        except ScenarioError as e: res, code = dict(name=os.path.basename(f), verdict='RED', reason=str(e)), 1; print(f'SCENARIO {os.path.basename(f)} RED {e}')
        results.append(res); rc = max(rc, code)
    print('RESULT ' + json.dumps(dict(name='scenario', verdict='OK' if rc == 0 else 'RED', scenarios=[dict(name=r['name'], verdict=r['verdict'], reason=r.get('reason'), dir=r.get('dir')) for r in results]), ensure_ascii=False))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
