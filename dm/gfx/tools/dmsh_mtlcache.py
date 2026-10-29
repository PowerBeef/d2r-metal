#!/usr/bin/env python3
# Compiled-shader cache keyed by the Metal source (dmsh_build.py): a library is compiled once per (MSL text, compile convention, compiler), so a chain change
# that leaves a shader's MSL as it was (a translator fix, a new rule for other shaders) costs that shader no Metal compile. The Metal compiler is deterministic:
# the same MSL with the same flags gives a byte-identical metallib (checked for macOS and iOS), which is what makes reuse safe.
# Layout: <cache>/<target>/<2 hex>/<digest>.metallib, digest = sha256 over FORMAT, the target's compile convention (the record written into json chain_metal),
# the compiler's identity (metal / metallib --version without the install path, the SDK version and build) and the MSL bytes.
# Filling from earlier builds: every imported library must record the current compiler identity and flags in its reflection. Legacy libraries without that identity are not imported.
# A fresh compile of two shaders per stage also checks their bytes. Each folder is read once per compiler identity (filled.json remembers its number of libraries).
# Not part of the chain version (dmsh_chain.py): the cache never changes an output, it only skips recompiling one. Change FORMAT when what it stores changes.
# Pruning: each build records the entries it used (used/<chain>.txt); prune() keeps the entries the newest few chains used and deletes the rest.
# Adapted from nxnative (github.com/seedds/nxnative, MIT, (c) 2026 seedds).
import os, re, json, shutil, ctypes, hashlib, threading, subprocess, collections

FORMAT = b'dmsh_mtlcache 2'   # earlier prefill could misclassify an unsampled library from another compiler
_clonefile = getattr(ctypes.CDLL(None, use_errno=True), 'clonefile', None)   # APFS copy-on-write copy (no data written); elsewhere a plain copy


def clone(src, dst):
    tmp = f'{dst}.tmp-{os.getpid()}-{threading.get_ident()}'
    if _clonefile is None or _clonefile(os.fsencode(src), os.fsencode(tmp), 0) != 0: shutil.copyfile(src, tmp)
    os.replace(tmp, dst)


def compiler_id(sdk):
    def run(*a): return subprocess.run(['xcrun', '-sdk', sdk, *a], capture_output=True, text=True).stdout
    tools = ''.join(run(t, '--version') for t in ('metal', 'metallib'))
    return re.sub(r'(?m)^InstalledDir:.*\n?', '', tools) + run('--show-sdk-version') + run('--show-sdk-build-version')   # the install path changes with each toolchain mount


class Cache:
    def __init__(self, root, targets, work):
        """targets: {name: (sdk, record, compile)}, compile(msl, air, lib) as ios_metal.compile; work: a folder for temporary .air files"""
        self.root, self.targets, self.work = root, targets, work
        self.ident = {t: hashlib.sha256(FORMAT + b'\0' + rec.encode() + b'\0' + compiler_id(sdk).encode()).digest() for t, (sdk, rec, _) in targets.items()}
        self.stats, self.lock, self.used = collections.Counter(), threading.Lock(), set()

    def path(self, target, src):
        d = hashlib.sha256(self.ident[target] + b'\0' + src).hexdigest()
        return os.path.join(self.root, target, d[:2], d + '.metallib')

    def with_compiler(self, target, refl):
        return dict(refl, chain_metal=self.targets[target][1], chain_compiler=self.ident[target].hex())

    def count(self, what, n=1):
        with self.lock: self.stats[what] += n

    def _compile(self, target, msl, lib):
        air = os.path.join(self.work, f'mtlcache-{target}-{os.getpid()}-{threading.get_ident()}.air')
        try: self.targets[target][2](msl, air, lib)
        finally:
            if os.path.exists(air): os.unlink(air)

    def compile(self, target, msl, lib):
        """msl → lib for target: a clone of the library that the same source compiled to before, or a compile (then kept). Raises CalledProcessError as the compile does"""
        p = self.path(target, open(msl, 'rb').read())
        with self.lock: self.used.add(os.path.relpath(p, self.root))
        if os.path.exists(p):
            clone(p, lib); self.count((target, 'reused')); return
        tmp = f'{lib}.tmp-{os.getpid()}-{threading.get_ident()}'
        self._compile(target, msl, tmp)
        os.replace(tmp, lib)
        os.makedirs(os.path.dirname(p), exist_ok=True); clone(lib, p); self.count((target, 'compiled'))

    def touch(self, target, msl):
        """Marks the entry for this source as used (a shader the per-shader cache restored without compiling), so prune() keeps it; adds nothing"""
        with self.lock: self.used.add(os.path.relpath(self.path(target, open(msl, 'rb').read()), self.root))

    def _check(self, target, d, stems, mac_dir):
        """None if folder d was built with the current convention and compiler, else why not"""
        rec, picks = self.targets[target][1], collections.defaultdict(list)
        for stem in stems:
            try: j = json.load(open(os.path.join(d, stem + '.json')))
            except (OSError, ValueError): return f'no readable {stem}.json'
            if j.get('chain_metal') != rec: return f'built with "{j.get("chain_metal")}"'
            if j.get('chain_compiler') != self.ident[target].hex(): return f'{stem} has a different or unrecorded Metal compiler'
            if len(picks[j.get('stage')]) < 2: picks[j.get('stage')].append(stem)
        for stem in (s for v in picks.values() for s in v):
            tmp = os.path.join(self.work, f'mtlcache-check-{target}.metallib')
            try: self._compile(target, os.path.join(mac_dir, stem + '.metal'), tmp)
            except subprocess.CalledProcessError: return f'{stem}.metal does not compile now'
            same = open(tmp, 'rb').read() == open(os.path.join(d, stem + '.metallib'), 'rb').read(); os.unlink(tmp)
            if not same: return f'a fresh compile of {stem}.metal differs from its library'
        return None

    def fill(self, mac_dir, ios_dir=None, targets=None):
        """Takes the libraries of an earlier build: mac_dir = its Mac b-<chain>/ folder, ios_dir = the iOS folder of the same chain (or None). Returns the lines to print"""
        stamp_file = os.path.join(self.root, 'filled.json')
        try: stamps = json.load(open(stamp_file))
        except (OSError, ValueError): stamps = {}
        names = set(os.listdir(mac_dir)); stems = sorted(n[:-6] for n in names if n.endswith('_b.metal'))
        todo, notes = {}, []
        for target, d in (('mac', mac_dir), ('ios', ios_dir)):
            if targets is not None and target not in targets: continue   # checking an earlier build compiles samples too
            if target not in self.targets or not d or not os.path.isdir(d): continue
            libs = names if d == mac_dir else set(os.listdir(d))
            have = [s for s in stems if s + '.metallib' in libs]; key = self.ident[target].hex() + ':' + os.path.realpath(d)
            if not have or stamps.get(key) == len(have): continue
            why = self._check(target, d, have, mac_dir)
            if why: notes.append(f'Metal cache: not filled from {d}: {why}')
            else: todo[target] = (d, set(have))
            stamps[key] = len(have)
        added = collections.Counter()
        for stem in stems:
            ts = [t for t, (d, have) in todo.items() if stem in have]
            if not ts: continue
            src = open(os.path.join(mac_dir, stem + '.metal'), 'rb').read()
            for t in ts:
                p = self.path(t, src)
                if os.path.exists(p): continue
                os.makedirs(os.path.dirname(p), exist_ok=True); clone(os.path.join(todo[t][0], stem + '.metallib'), p); added[t] += 1
        for t, (d, have) in todo.items():
            self.count((t, 'filled'), added[t]); notes.append(f'Metal cache: {added[t]} {t} libraries added from {d} ({len(have)} there)')
        os.makedirs(self.root, exist_ok=True)
        tmp = f'{stamp_file}.tmp-{os.getpid()}'; json.dump(stamps, open(tmp, 'w'), indent=1); os.replace(tmp, stamp_file)
        return notes

    def save_used(self, chain):
        """Records the entries this build used, so prune() keeps them while the chain is among the newest"""
        d = os.path.join(self.root, 'used'); os.makedirs(d, exist_ok=True)
        tmp = os.path.join(d, f'{chain}.txt.tmp-{os.getpid()}')
        with open(tmp, 'w') as f: f.write(''.join(p + '\n' for p in sorted(self.used)))
        os.replace(tmp, os.path.join(d, f'{chain}.txt'))

    def prune(self, keep=3):
        """Deletes the cached libraries that none of the newest `keep` builds (by their used/<chain>.txt) used; returns the line to print"""
        d = os.path.join(self.root, 'used')
        lists = sorted((os.path.join(d, n) for n in os.listdir(d) if n.endswith('.txt')), key=os.path.getmtime, reverse=True) if os.path.isdir(d) else []
        if not lists: return 'Metal cache: nothing recorded to prune by'
        live = {line.strip() for p in lists[:keep] for line in open(p) if line.strip()}
        for p in lists[keep:]: os.unlink(p)
        gone = freed = 0
        for t in self.targets:
            for sub, _, files in os.walk(os.path.join(self.root, t)):
                for f in files:
                    full = os.path.join(sub, f)
                    if f.endswith('.metallib') and os.path.relpath(full, self.root) not in live:
                        freed += os.path.getsize(full); os.unlink(full); gone += 1
        return f'Metal cache: pruned {gone} libraries ({freed / 2**20:.0f} MiB) no build among the newest {keep} used'

    def summary(self):
        s = self.stats
        return 'Metal cache: ' + '; '.join(f'{t} {s[t, "reused"]} reused, {s[t, "compiled"]} compiled' + (f', {s[t, "filled"]} filled from earlier builds' if s[t, 'filled'] else '')
                                          for t in self.targets)
