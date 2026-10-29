#!/usr/bin/env python3
# 环境开关（DM_*）登记表：从源码扫出谁读了哪些开关，生成 dm/switches.tsv；检查脚本里设的开关有没有人读（拼错 / 早已删掉的开关一设就静默无效）。
#   读：C / ObjC 源码里的 "DM_…" 字面量（getenv 直接读与经名字表读的都算）、Python 的 os.environ.get / os.environ[…] / os.getenv、shell 与 Makefile 里的 $DM_… / ${DM_…}
#   设：shell 的 DM_…= / export / env -u / --env DM_…= / DEVICECTL_CHILD_DM_…=（去掉前缀）、Python 的环境字典键 / dict(…, DM_…=) / 'DM_…=值' 串
#   gen    重新生成 dm/switches.tsv（列：开关、读它的部分、读它的文件、文档里提到没有）
#   check  登记表与源码一致，且每个「设」的开关都有人读；不符打 SWITCHES RED 与明细、退出码 1
# Registry of environment switches (DM_*): scans the sources for who reads which switches and generates dm/switches.tsv; checks that every switch a script sets is read by something (a misspelt / long-removed switch silently does nothing when set).
#   Reads: "DM_..." literals in C / ObjC sources (both direct getenv and reads through name tables count), Python's os.environ.get / os.environ[...] / os.getenv, $DM_... / ${DM_...} in shell and Makefiles
#   Sets: shell DM_...= / export / env -u / --env DM_...= / DEVICECTL_CHILD_DM_...= (prefix removed), Python environment dict keys / dict(..., DM_...=) / 'DM_...=value' strings
#   gen    regenerate dm/switches.tsv (columns: switch, parts that read it, files that read it, mentioned in the docs or not)
#   check  the registry matches the sources and every switch that is set is read by something; otherwise prints SWITCHES RED with details, exit code 1
import ast
import collections
import os
import re
import subprocess
import sys

DM = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
NAME = r'DM_[A-Z0-9]+(?:_[A-Z0-9]+)*'
C_LIT = re.compile(r'"(' + NAME + r')"')
SH_READ = re.compile(r'\$\{?(' + NAME + r')\b')
MK_READ = re.compile(r'\$[({](' + NAME + r')[)}]')   # Makefile 与 XcodeGen（project.yml）的 $(DM_…) / ${DM_…} | $(DM_...) / ${DM_...} in Makefiles and XcodeGen (project.yml)
LIT_EXT = ('.c', '.m', '.h', '.inc', '.mm', '.S', '.cpp', '.cs', '.swift')   # "DM_..." literals in these languages count as reads
# 有意保留、没人读的开关：名字 → 理由（新加这里要写明为什么不删调用处） | switches deliberately kept although nothing reads them: name -> reason (a new entry must say why the callers are not cleaned up)
KEPT = {
    'DM_NOINSTALL': 'ios/run.sh 说明为兼容保留：build 本来就只编译、不装机 | kept for compatibility per ios/run.sh: build only compiles and never installs anyway',
}
SH_SET = re.compile(r'(?:^|[\s;&|(`"\'])(?:export\s+|--env[ =]["\']?|-u\s+)?(?:DEVICECTL_CHILD_)?(' + NAME + r')(?:=|(?=\s*(?:$|;)))')
SH_UNSET = re.compile(r'\benv\b[^|;]*?-u\s+(?:DEVICECTL_CHILD_)?(' + NAME + r')')
SET_STR = re.compile(r'^(?:--env[ =])?(?:DEVICECTL_CHILD_)?(' + NAME + r')=')


def files(root):
    out = subprocess.run(['git', '-C', root, 'ls-files', '-z', '--', '.'], capture_output=True, text=True, check=True).stdout
    return [f for f in out.split('\0') if f and not f.startswith('third_party/')]


def part(f):
    for p, n in (('host/', 'host'), ('gfx/', 'gfx'), ('ios/Sources/', 'ios'), ('ios/', 'ios-tools'),
                 ('verify/', 'verify'), ('tools/', 'tools'), ('i18n/', 'i18n')):
        if f.startswith(p): return n
    return 'other'


def sh_code(line):   # 去掉 shell 注释（整行注释；行尾「 # …」） | strip shell comments (whole-line comments; trailing " # ...")
    s = line.lstrip()
    if s.startswith('#'): return ''
    i = line.find(' # ')
    return line[:i] if i >= 0 else line


def py_scan(f, text, reads, sets):
    try: tree = ast.parse(text)
    except SyntaxError: return
    def const(n): return n.value if isinstance(n, ast.Constant) and isinstance(n.value, str) else None
    def env_obj(n): return isinstance(n, ast.Attribute) and n.attr == 'environ' or isinstance(n, ast.Name) and n.id in ('env', 'environ', 'e', 'ENV', 'envs')
    for n in ast.walk(tree):
        if isinstance(n, ast.Call):
            fn = n.func; name = fn.attr if isinstance(fn, ast.Attribute) else fn.id if isinstance(fn, ast.Name) else ''
            a0 = const(n.args[0]) if n.args else None
            if a0 and re.fullmatch(NAME, a0):
                if name in ('get', 'getenv', 'pop') and (name == 'getenv' or env_obj(getattr(fn, 'value', None))): reads[a0].add(f)
                elif name in ('setdefault', 'putenv'): sets[a0].append((f, n.lineno))
            if name == 'dict':
                for k in n.keywords:
                    if k.arg and re.fullmatch(NAME, k.arg): sets[k.arg].append((f, n.lineno))
        elif isinstance(n, ast.Subscript) and env_obj(n.value) and (k := const(n.slice)) and re.fullmatch(NAME, k):
            (reads[k].add(f) if isinstance(n.ctx, ast.Load) else sets[k].append((f, n.lineno)))
        elif isinstance(n, ast.Dict):
            for k in n.keys:
                if k is not None and (s := const(k)) and re.fullmatch(NAME, s): sets[s].append((f, n.lineno))
        elif isinstance(n, ast.Constant) and isinstance(n.value, str) and (m := SET_STR.match(n.value)):
            sets[m.group(1)].append((f, getattr(n, 'lineno', 0)))


def scan(root=DM):
    reads, sets, docs = collections.defaultdict(set), collections.defaultdict(list), set()
    for f in files(root):
        p = os.path.join(root, f)
        try: text = open(p, encoding='utf-8', errors='replace').read()
        except OSError: continue
        ext = os.path.splitext(f)[1]
        if ext in LIT_EXT:
            for m in C_LIT.finditer(text): reads[m.group(1)].add(f)
        elif ext == '.py':
            py_scan(f, text, reads, sets)
        elif ext == '.sh' or os.path.basename(f) in ('setup.sh',):
            for i, line in enumerate(text.splitlines(), 1):
                code = sh_code(line)
                for m in SH_READ.finditer(code): reads[m.group(1)].add(f)
                for rx in (SH_SET, SH_UNSET):
                    for m in rx.finditer(code): sets[m.group(1)].append((f, i))
        elif os.path.basename(f) == 'Makefile' or ext in ('.mk', '.yml', '.yaml', '.xcconfig'):
            for m in MK_READ.finditer(text): reads[m.group(1)].add(f)
        elif ext == '.md':
            docs.update(re.findall(NAME, text))
    return reads, sets, docs


def table(reads, docs):
    rows = ['开关 | switch\t读它的部分 | read by\t文件 | files\t文档 | documented']
    for k in sorted(reads):
        fs = sorted(reads[k])
        rows.append(f"{k}\t{','.join(sorted({part(f) for f in fs}))}\t{' '.join(fs[:4])}{' …' if len(fs) > 4 else ''}\t{'y' if k in docs else '-'}")
    return '\n'.join(rows) + '\n'


def main(argv):
    root = os.environ.get('SWITCHES_ROOT') or DM   # 单测用：换一棵树扫 | for unit tests: scan another tree
    if len(argv) != 1 or argv[0] not in ('gen', 'check'): sys.exit('用法 | usage: switches.py gen|check')
    reads, sets, docs = scan(root)
    reg = os.path.join(root, 'switches.tsv')
    text = table(reads, docs)
    if argv[0] == 'gen':
        open(reg, 'w', encoding='utf-8').write(text)
        print(f'SWITCHES 已生成 | generated: {len(reads)} 个开关 | switches → dm/switches.tsv'); return 0
    bad = []
    try: cur = open(reg, encoding='utf-8').read()
    except OSError: cur = ''
    if cur != text:
        bad.append('dm/switches.tsv 与源码不一致：跑 python3 dm/tools/switches.py gen | dm/switches.tsv does not match the sources: run python3 dm/tools/switches.py gen')
    for k in sorted(set(sets) - set(reads) - set(KEPT)):
        where = ', '.join(f'dm/{f}:{n}' for f, n in sets[k][:3])
        bad.append(f'{k} 有地方设、没有地方读 | is set but never read: {where}')
    for b in bad: print('  ' + b)
    print(f"SWITCHES {'RED' if bad else 'OK'} {len(reads)} 个开关 | switches，{sum(map(len, sets.values()))} 处设置 | set sites" + (f'，{len(bad)} 处问题 | problems' if bad else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
