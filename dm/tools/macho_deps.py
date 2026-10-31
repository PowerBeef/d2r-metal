#!/usr/bin/env python3
"""Checks that a release pack is self-contained (dm/tools/release_mac.sh): every Mach-O in it loads only macOS's own libraries (/usr/lib,
/System/Library) or a file inside the pack. @loader_path and @executable_path are resolved, and @rpath against the image's own LC_RPATH entries and
those of the pack's programs, so a library the build Mac has but a player's Mac lacks (Homebrew, Xcode, a runtime) is caught, as is a library
named through @rpath that the pack does not carry.

Usage: macho_deps.py <pack dir>   prints each offender ("<file> loads <library>: <why>") and exits 1 when there is any; standard library only
"""
import os
import subprocess
import sys

MAGIC = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}   # 64-bit Mach-O (both byte orders) and universal
LOADS = {'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB', 'LC_LAZY_LOAD_DYLIB', 'LC_LOAD_UPWARD_DYLIB'}
SYSTEM = ('/usr/lib/', '/System/Library/')


def is_macho(path):
    try:
        with open(path, 'rb') as f: return f.read(4) in MAGIC
    except OSError: return False


def load_commands(path):
    """(the image's dependencies, its rpaths, whether it is a program) from otool -l"""
    out = subprocess.run(['otool', '-l', path], capture_output=True, text=True).stdout.splitlines()
    head = subprocess.run(['otool', '-h', path], capture_output=True, text=True).stdout
    deps, rpaths, cmd = [], [], None
    for line in out:
        w = line.split()
        if len(w) >= 2 and w[0] == 'cmd': cmd = w[1]
        elif cmd in LOADS and len(w) >= 2 and w[0] == 'name': deps.append(w[1])
        elif cmd == 'LC_RPATH' and len(w) >= 2 and w[0] == 'path': rpaths.append(w[1])
    filetype = next((l.split()[4] for l in head.splitlines() if l.split()[:1] and l.split()[0].startswith('0x')), '0')
    return deps, rpaths, filetype in ('2', 'EXECUTE')   # MH_EXECUTE


def main(argv):
    if len(argv) != 1: sys.exit(__doc__)
    root = os.path.realpath(argv[0])
    images = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if not os.path.islink(p) and is_macho(p): images[p] = load_commands(p)
    programs = [p for p, (_, _, exe) in images.items() if exe]

    def subst(path, image, exe):
        return path.replace('@loader_path', os.path.dirname(image)).replace('@executable_path', os.path.dirname(exe))

    bad = []
    for image, (deps, rpaths, exe) in sorted(images.items()):
        hosts = [image] if exe else programs or [image]   # @executable_path of a library: the pack's programs that may load it
        for dep in deps:
            if dep.startswith(SYSTEM): continue
            cands = []
            for host in hosts:
                if dep.startswith('@rpath/'):
                    for r in rpaths + (images[host][1] if host in images else []):
                        cands.append(os.path.join(subst(r, image if r in rpaths else host, host), dep[len('@rpath/'):]))
                elif dep.startswith(('@loader_path/', '@executable_path/')):
                    cands.append(subst(dep, image, host))
            if not cands:
                bad.append(f'{os.path.relpath(image, root)} loads {dep}: outside macOS and the pack'); continue
            found = [c for c in cands if os.path.exists(c)]
            inside = [c for c in found if os.path.realpath(c).startswith(root + os.sep)]
            if not inside:
                bad.append(f'{os.path.relpath(image, root)} loads {dep}: ' + ('found only outside the pack' if found else 'not in the pack'))
    for b in bad: print('  ' + b)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
