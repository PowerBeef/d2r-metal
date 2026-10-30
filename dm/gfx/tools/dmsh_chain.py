#!/usr/bin/env python3
# Translation chain version of the B shader library (the build result must be determined by source alone).
# Chain version V = first 12 digits of the sha256 of the contents of the following in-repo files + the translation options: this script, dmsh_build.py, its MSL rules dmsh_rules.py, dmsh_fetch.py (readable-tile version), the dmsh_xlate sources (Program.cs, csproj),
# the mapping table (including the conflict table), the answer table for translation queries (dmsh_facts.py and nvn/shader_facts.tsv), the iOS compile convention ios_metal.py, the tool lock dmsh_chain.lock (measured fingerprints of the Ryujinx shader dll / spirv-cross / metal compiler).
# The build (dmsh_build.py) writes into <cache dir>/b-<V>/ and the reflection json records chain = V; at compile time the host takes V from nvn/dmsh_chain.h, loads only from b-<V>/ and verifies each json.
# Usage: dmsh_chain.py                 print V (default options)
#       dmsh_chain.py --header        rewrite nvn/dmsh_chain.h (run after changing any file listed above; tests/t_chain.sh verifies it)
#       dmsh_chain.py --check-tools   verify that the actual tools match the lock file; on mismatch report a failure and exit 1
#       dmsh_chain.py --update-lock   rewrite the lock file from the actual tools (run after changing Ryujinx / spirv-cross / Xcode; committed to the repo)
#       dmsh_chain.py --update-lock --runtime   the same for the source form: no Metal compiler where the chain is built (the launcher's installs), the lock
#                                     records "metal_macosx runtime" and the host compiles each library's .metal with macOS's own compiler (nvn_mtl.m lib_load)
# Environment: XLATE_DEBUG / XLATE_REDUCED (same meaning as in dmsh_build.py, default 1 / 1; non-default options yield a different V, which the host will not load)
import os, sys, hashlib, subprocess
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作区根目录（游戏文件、产物、运行现场都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
HERE = os.path.dirname(os.path.abspath(__file__)); GFX = os.path.dirname(HERE); HOME = os.path.expanduser('~')
FILES = ['tools/dmsh_chain.py', 'tools/dmsh_build.py', 'tools/dmsh_rules.py', 'tools/dmsh_fetch.py', 'tools/dmsh_facts.py', 'nvn/shader_facts.tsv', 'tools/dmsh_xlate/Program.cs', 'tools/dmsh_xlate/dmsh_xlate.csproj',
         'nvn/shader_map.tsv', 'nvn/shader_map.tsv.conflicts.tsv', '../ios/tools/ios_metal.py', 'tools/dmsh_chain.lock']
LOCK = os.path.join(HERE, 'dmsh_chain.lock'); HEADER = os.path.join(GFX, 'nvn/dmsh_chain.h')
XLATE_DIR = os.path.dirname(os.environ.get('XLATE', f'{DM_ROOT}/build/gfx/dmsh_xlate/dmsh_xlate'))

def options():
    return 'debug=%s reduced=%s' % (os.environ.get('XLATE_DEBUG', '1') != '0', os.environ.get('XLATE_REDUCED', '1') == '1')

def version():
    h = hashlib.sha256()
    for f in FILES:
        h.update(f.encode() + b'\0' + open(os.path.join(GFX, f), 'rb').read() + b'\0')
    h.update(options().encode())
    return h.hexdigest()[:12]

def code_form(source, blank=()):
    """The part of a Python source that can change what it computes, as bytes: the AST dump (comments, blank lines, formatting and line numbers are not in it)
    without docstrings, and with the bodies of the functions named in `blank` replaced by `pass` (dmsh_build.py: the rule functions, which have their own
    per-shader fingerprints). Anything else (a constant, a string literal that is not a docstring, a default argument) changes it. ast.dump's text differs
    between Python versions, so the version is part of the form. Adapted from nxnative (github.com/seedds/nxnative, MIT, (c) 2026 seedds)."""
    import ast
    tree = ast.parse(source)
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            if getattr(node, 'name', None) in blank: node.body = [ast.Pass()]
            elif node.body and isinstance(node.body[0], ast.Expr) and isinstance(node.body[0].value, ast.Constant) and isinstance(node.body[0].value.value, str):
                node.body = node.body[1:] or [ast.Pass()]
    return ('python %d.%d\0' % sys.version_info[:2]).encode() + ast.dump(tree).encode()

def sha16(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]

RUNTIME = 'runtime'   # the lock's metal_macosx in the source form (--update-lock --runtime)

def lock():
    return dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK) if '\t' in l and not l.startswith('#'))

def runtime_form():
    """True when the lock is of the source form: the build writes .metal libraries and compiles nothing (no xcrun anywhere)"""
    return lock().get('metal_macosx') == RUNTIME

def tools_now(runtime=None):
    """实际工具指纹：翻译器 dll（含 Ryujinx 着色器库）、spirv-cross、metal 编译器（macOS / iOS） | Actual tool fingerprints: translator dll (including the Ryujinx shader library), spirv-cross, metal compiler (macOS / iOS)
    runtime (default: the lock's form): the source form records no compiler, the host's macOS compiles the libraries"""
    if runtime is None: runtime = runtime_form()
    t = {}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll'): t[dll] = sha16(os.path.join(XLATE_DIR, dll))
    t['xlate_source'] = subprocess.run([os.path.join(XLATE_DIR, 'dmsh_xlate'), '--source-sha'], capture_output=True, text=True).stdout.strip()
    sc = subprocess.run(['which', 'spirv-cross'], capture_output=True, text=True).stdout.strip()
    t['spirv-cross'] = sha16(sc) if sc else 'missing'
    if runtime:
        t['metal_macosx'] = RUNTIME
        return t
    for sdk in ('macosx', 'iphoneos'):
        t['metal_' + sdk] = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True, text=True).stdout.splitlines()[0:1] or ['missing']
        t['metal_' + sdk] = t['metal_' + sdk][0]
    return t

def xlate_source_sha():   # 与 dmsh_xlate --source-sha 同口径：嵌入的 Program.cs 与 csproj 依次拼接 | same convention as dmsh_xlate --source-sha: the embedded Program.cs and csproj concatenated in that order
    return hashlib.sha256(b''.join(open(os.path.join(HERE, 'dmsh_xlate', f), 'rb').read() for f in ('Program.cs', 'dmsh_xlate.csproj'))).hexdigest()[:16]

def check_tools():
    want = lock()
    now = tools_now(); bad = [f'{k}：锁 {want.get(k)} / 实际 {v}' for k, v in now.items() if want.get(k) != v]
    if now['xlate_source'] != xlate_source_sha(): bad.append(f'翻译器不是用仓内源编的：嵌入源 {now["xlate_source"]} / 仓内 {xlate_source_sha()}（重编 dmsh_xlate）')
    return bad

if __name__ == '__main__':
    a = sys.argv[1:]
    if a in (['--update-lock'], ['--update-lock', '--runtime']):
        t = tools_now(runtime=a[1:] == ['--runtime'])
        if t['xlate_source'] != xlate_source_sha(): sys.exit(f'【红】翻译器不是用仓内源编的（嵌入 {t["xlate_source"]} / 仓内 {xlate_source_sha()}）：先重编 dmsh_xlate')
        open(LOCK, 'w').write('# 翻译链工具指纹（dmsh_chain.py --update-lock 生成；换工具后重生成并随提交入仓）\n' + ''.join(f'{k}\t{v}\n' for k, v in t.items()))
        print('已写', LOCK)
    elif a == ['--check-tools']:
        bad = check_tools()
        for b in bad: print('【红】翻译链工具与锁文件不符：' + b)
        sys.exit(1 if bad else 0)
    elif a == ['--header']:
        open(HEADER, 'w').write('// 生成文件（dm/gfx/tools/dmsh_chain.py --header）：本源码树期望的 B 着色器库翻译链版本（缺省选项）；宿主只加载 <库目录>/b-<V>/\n'
                                f'#define DMSH_CHAIN "{version()}"\n')
        print(version())
    elif not a: print(version())
    else: sys.exit(__doc__ if __doc__ else '用法见文件头')
