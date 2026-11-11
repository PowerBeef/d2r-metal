#!/usr/bin/env python3
"""What to run for a change (fork_gate.sh auto), and what a release needs.

  gate_pick.py tiers [--base REF]     the tiers a change runs, on one line: quick unit
  gate_pick.py explain [--base REF]   the same, then the checks this change touches (to run by hand) and every changed file with the tier
                                      that exercises it when the whole gate runs
  gate_pick.py release                the tiers a release or an app rebuild needs: quick unit core full
  gate_pick.py lint [--gate FILE]     every step line of fork_gate.sh: each "$DM/..." path names a file, no step name twice
  gate_pick.py passed [--sha REV]     the newest gate run that passed quick unit core full on that commit with nothing uncommitted
                                      (what a release and an app rebuild need); exit 1 when there is none

The rule since 2026-11-10 (the player's decision: the game tiers were run per change and got in the way): a change runs the quick and unit
tiers and the few checks it touches (CHECKS below: its scenario on the headless host, the rewrite regression for a rewriter change, the
pipeline check for a seed change, a verify mode). The whole gate runs once, on the commit a release or an app rebuild is cut from
(release_mac.sh refuses without it), so nothing reaches players ungated. What that costs: a regression only the game tiers catch shows
at release time, not at the change that made it.

The change is everything between the merge base with origin/main (or --base, or DM_GATE_BASE) and the working tree:
commits not pushed yet, staged and unstaged edits, deletions and new files. The three files setup.sh rewrites are
never part of it.

RULES still says which tier exercises each file when the whole gate runs (`*` in a pattern also matches `/`): explain prints it, so
it is plain what a change leaves for the release run. A curated scenario is exercised in core when it runs on the command-line host
and in full when its `requires` line leaves that host out. Files no rule names count as core.

auto never picks core, full, long or device: ask for those tiers by name.
"""
import argparse
import fnmatch
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ORDER = ('quick', 'unit', 'core', 'full')
SETUP_FILES = ('dm/gfx/tools/dmsh_chain.lock', 'dm/gfx/nvn/dmsh_chain.h', 'dm/gfx/nvn/c1_full.tsv')   # rewritten by setup.sh, never committed
SCENARIO = 'scenario'   # a curated scenario: core or full by its requires line

# Tools and inputs only the long tier runs: auto never picks long, so it says when a change touches one (test_gate_pick checks no other tier uses them)
LONG_ONLY = ('dm/tools/bench.py', 'dm/tools/bench_why.py', 'dm/tools/explore.py', 'dm/tools/playbot.py', 'dm/verify/why_gate.py', 'dm/verify/fnfuzz.py',
             'dm/verify/bench/*', 'dm/verify/scenarios/heap/*')

RULES = [
    # Words only: the quick tier's checks read them (enginemap, switches, the string tables)
    ('*.md', 'unit', 'docs'),
    ('docs/*', 'unit', 'docs'),
    ('LICENSE', 'unit', 'docs'),
    ('.github/*', 'unit', 'GitHub CI (runs there)'),
    ('dm/switches.tsv', 'unit', 'the switch registry (quick_switches)'),
    # Full: what only the apps, the replays or the picture sets exercise
    ('dm/gfx/tests/nvn_replay*', 'full', 'the frame replayer (full_rays_replay)'),
    ('dm/*/tests/*', 'unit', 'unit tests'),
    ('dm/tools/test_*.py', 'unit', 'unit tests'),
    ('dm/verify/touchcases/test_lib.py', 'unit', 'unit tests'),
    ('dm/gfx/*', 'full', 'graphics: the replay layer, shaders and textures'),
    ('dm/ios/*', 'full', 'the iOS app'),
    # The host is one program for all three: the Mac App ships this dmhost (mac_app.sh) and the iOS app compiles every host source
    # (ios/project.yml), while 19 scenarios run only on the Mac App and 8 only on the iOS app
    ('dm/host/*', 'full', 'the game host: the Mac App runs the same program and the iOS app compiles it'),
    ('dm/data/*', 'full', 'game data the apps ship'),
    ('dm/PRODUCT', 'full', 'the rewritten game program'),
    ('setup.sh', 'full', 'the rewritten game program'),
    ('dm/tools/patch_nso.py', 'full', 'the binary rules'),
    ('dm/tools/product.sh', 'full', 'the rewritten game program'),
    ('dm/tools/mac_app.sh', 'full', 'the Mac App package'),
    ('dm/tools/scenario.py', 'full', 'the scenario runner (its Mac App and iOS backends run in full)'),
    ('dm/tools/pso_seed.py', 'full', 'pipeline warm-up (gated on the apps in full)'),
    ('dm/third_party/*', 'full', 'third-party code (graphics decoders, FLIP)'),
    ('dm/i18n/ui_text.tsv', 'full', "text on the apps' screens"),
    ('dm/verify/golden*', 'full', 'the kept baselines'),
    ('dm/verify/rays_replay.sh', 'full', 'the rays replay test'),
    ('dm/verify/scenarios/picture/*', 'full', 'the picture scenarios'),
    ('dm/verify/scenarios/faults/*', 'full', 'the fault matrix'),
    ('dm/verify/scenarios/*/*', 'core', 'scenario saves and helpers'),
    ('dm/verify/scenarios/*.scn', SCENARIO, 'a curated scenario'),
    # Unit: tools no game run uses, the launcher (unit_packaging), the long tier's material
    ('dm/launcher/*', 'unit', 'the launcher (unit_packaging)'),
    ('dm/tools/release_mac.sh', 'unit', 'release packaging (unit_packaging)'),
    ('dm/tools/launcher_app.sh', 'unit', 'release packaging (unit_packaging)'),
    ('dm/tools/extract_game.sh', 'unit', 'game extraction (unit_packaging)'),
    ('dm/tools/re/*', 'unit', 'engine-analysis tools (quick_readdr)'),
    ('dm/tools/enginemap.py', 'unit', 'the engine-changes check (quick_enginemap)'),
    ('dm/tools/switches.py', 'unit', 'the switch registry check (quick_switches)'),
    ('dm/tools/arrdump.py', 'unit', 'the texture array dump reader'),
    ('dm/tools/gate_pick.py', 'unit', 'this picker'),
    ('dm/tools/bench*.py', 'unit', 'benchmarks (long tier, on demand)'),
    ('dm/verify/bench/*', 'unit', 'benchmarks (long tier, on demand)'),
    ('dm/i18n/*', 'unit', 'the string checks (quick)'),
    ('dm/brand/*', 'unit', 'brand art'),
    # Core: the gate, the harness and anything else that a game run on the command-line host uses
    ('dm/verify/fork_gate.sh', 'core', 'the gate itself (quick_gate parses it)'),
    ('*', 'core', 'no rule names it: a game run'),
]


def git(root, *args):
    return subprocess.run(['git', '-C', root, *args], capture_output=True, text=True, check=True).stdout


def changed(base, root=ROOT):
    """The files a change touches, against the merge base with base (the working tree alone when there is none)."""
    try:
        mb = git(root, 'merge-base', 'HEAD', base).strip()
    except subprocess.CalledProcessError:
        mb = 'HEAD'
    files = set(git(root, 'diff', '--name-only', '--no-renames', mb).split()) | set(git(root, 'ls-files', '--others', '--exclude-standard').split())
    return mb, sorted(f for f in files if f not in SETUP_FILES)


def scenario_tier(path, root=ROOT):
    """core when the scenario runs on the command-line host (no requires line, or one naming cli), full otherwise; a deleted one asks for core."""
    try:
        with open(os.path.join(root, path), encoding='utf-8') as f: lines = f.read().splitlines()
    except OSError:
        return 'core'
    for line in lines:
        words = line.split('#')[0].split()
        if words[:1] == ['requires']:
            return 'core' if 'cli' in words[1:] else 'full'
    return 'core'


def classify(path, root=ROOT):
    """(tier, reason) for one changed file."""
    for pattern, tier, why in RULES:
        if fnmatch.fnmatchcase(path, pattern):
            if tier == SCENARIO:
                tier = scenario_tier(path, root)
                why = f"{why}{'' if tier == 'core' else ' that never runs on the command-line host'}"
            return tier, why
    return 'core', 'no rule names it: a game run'


def pick(files, root=ROOT):
    """The tiers that exercise these files when the whole gate runs, in order, and each file's (tier, reason). A change itself runs PER_CHANGE."""
    each = {f: classify(f, root) for f in files}
    top = max([ORDER.index('unit')] + [ORDER.index(t) for t, _ in each.values()])
    return list(ORDER[:top + 1]), each

PER_CHANGE = ('quick', 'unit')

# The checks a change touches: (pattern, command, what it is). Every rule that matches adds its command once; {file} is the changed file.
# They are short (seconds to about a minute each), run on the headless command-line host and take nobody's screen.
ENTER = 'python3 dm/tools/scenario.py run dm/verify/scenarios/enter_game.scn --headless'
CHECKS = [
    ('dm/tools/patch_nso.py', 'dm/tools/product.sh build && dm/verify/regress.sh --fresh', 'the rewrite regression on a fresh product'),
    ('dm/PRODUCT', 'dm/tools/product.sh build && dm/verify/regress.sh --fresh', 'the rewrite regression on a fresh product'),
    ('dm/tools/product.sh', 'dm/tools/product.sh build && dm/verify/regress.sh --fresh', 'the rewrite regression on a fresh product'),
    ('dm/gfx/nvn/pso_manifest.tsv', 'python3 dm/tools/pso_seed.py gate --backend cli --headless', 'the pipeline warm-up check on the command-line host'),
    ('dm/gfx/nvn/pso_allow.tsv', 'python3 dm/tools/pso_seed.py gate --backend cli --headless', 'the pipeline warm-up check on the command-line host'),
    ('dm/tools/pso_seed.py', 'python3 dm/tools/pso_seed.py gate --backend cli --headless', 'the pipeline warm-up check on the command-line host'),
    ('dm/gfx/nvn/hle_nvn.c', 'python3 dm/tools/scenario.py run dm/verify/scenarios/pipeset_verify.scn --headless', "the recorder: the pipeline-set fast path's verify mode"),
    ('dm/gfx/nvn/nvn_obj.h', 'python3 dm/tools/scenario.py run dm/verify/scenarios/pipeset_verify.scn --headless', "the recorder: the pipeline-set fast path's verify mode"),
    ('dm/gfx/nvn/hle_nvn.c', 'python3 dm/tools/scenario.py run dm/verify/scenarios/quadrun_verify.scn --headless', "the recorder's buffer and sampler bind filter and the replay's shortcuts, in their verify modes"),
    ('dm/gfx/nvn/nvn_mtl.m', 'python3 dm/tools/scenario.py run dm/verify/scenarios/quadrun_verify.scn --headless', "the replay: the 2D quad runs' verify mode"),
    ('dm/gfx/nvn/nvn_mtl.m', 'python3 dm/tools/scenario.py run dm/verify/scenarios/pso_wait.scn --headless', "the replay: a run with no warm-up list, effect draws waiting for their pipelines"),
    ('dm/host/dm_bootlist.c', 'python3 dm/verify/bootlist_check.py', 'the boot list a machine learns, over two launches'),
    ('dm/host/hle_tact.c', 'python3 dm/verify/bootlist_check.py', 'the boot list a machine learns, over two launches'),
    ('dm/host/hle_granny.c', 'python3 dm/verify/bootlist_check.py', 'the boot list a machine learns, over two launches'),
    ('dm/verify/bootlist_check.py', 'python3 dm/verify/bootlist_check.py', 'the boot list a machine learns, over two launches'),
    ('dm/host/dm_live.c', 'python3 dm/tools/scenario.py run dm/verify/scenarios/live_hooks.scn dm/verify/scenarios/engine_assumptions.scn --headless', 'the live hooks on a running game, and the standing checks they carry'),
    ('dm/host/dm_names.c', 'python3 dm/tools/scenario.py run dm/verify/scenarios/names.scn --headless', "the name tables read from the running game"),
    ('dm/verify/bench/warp_tour.scn', 'python3 dm/tools/pso_seed.py gate --backend cli --headless --survey --route dm/verify/bench/warp_tour.scn', 'the tour of every warp stop, with the pipelines the warm-up seed lacks listed (5½ min)'),
    ('dm/verify/bench/skill_tour.scn', 'python3 dm/tools/pso_seed.py gate --backend cli --headless --survey --route dm/verify/bench/skill_tour.scn', "the Warlock's skills cast in turn, with the pipelines the warm-up seed lacks listed (4 min; needs the generated save, scenarios/saves/warlock_hero.scn)"),
    ('dm/verify/scenarios/*.scn', 'python3 dm/tools/scenario.py run {file} --headless', 'the changed scenario itself'),
    ('dm/host/*', ENTER, 'a game on the headless host'),
    ('dm/gfx/*', ENTER, 'a game on the headless host'),
    ('dm/tools/scenario.py', ENTER, 'a game on the headless host'),
]


def checks(files, root=ROOT):
    """The commands this change's files ask for, in CHECKS order, each once: [(command, what, [files])]. Unit tests are the unit tier's;
    a scenario that cannot run on the command-line host has no short check (it runs at the release)."""
    out = {}
    for pattern, cmd, what in CHECKS:
        for f in files:
            if not fnmatch.fnmatchcase(f, pattern) or classify(f, root)[0] == 'unit': continue
            if pattern.endswith('.scn') and (scenario_tier(f, root) != 'core' or not os.path.exists(os.path.join(root, f))): continue
            out.setdefault(cmd.replace('{file}', f), (what, []))[1].append(f)
    return [(cmd, what, fs) for cmd, (what, fs) in out.items()]


STEP = re.compile(r'(?:^|[;&|]\s*|\bthen\s+|\bdo\s+)step\s+("?[\w$]+"?)\s+\d+\s+--\s+(.*)$')
DMPATH = re.compile(r'"\$DM/([^"$]*)"([^\s"\'$;)]*)')


def ignored(path):
    """A path git ignores is a build output (dmhost_tsan): it need not exist in a clean checkout, so the lint accepts it."""
    d = os.path.dirname(path)
    while d and not os.path.isdir(d): d = os.path.dirname(d)
    return subprocess.run(['git', '-C', d or '.', 'check-ignore', '-q', path], capture_output=True).returncode == 0


def lint(gate, dm):
    """Problems in the gate script's step lines: a "$DM/..." path that names nothing (a glob must match a file), a step name used twice.
    Checked without running anything, so a typo in a tier auto did not pick still fails the quick tier."""
    problems, names, n = [], {}, 0
    with open(gate, encoding='utf-8') as f: text = f.read().splitlines()
    for i, line in enumerate(text, 1):
        code = line.split('   # ')[0]
        m = STEP.search(code.strip())
        if not m:
            continue
        n += 1
        name = m.group(1).strip('"')
        if '$' not in name:
            if name in names: problems.append(f'line {i}: step {name} already at line {names[name]}')
            names.setdefault(name, i)
        for pm in DMPATH.finditer(m.group(2)):
            rel = pm.group(1) + pm.group(2)
            if not rel or rel.endswith('/'):
                continue
            if not glob.glob(os.path.join(dm, rel)) and not ignored(os.path.join(dm, rel)):
                problems.append(f'line {i}: step {name}: $DM/{rel} names nothing')
    return n, problems


RELEASE_TIERS = ('quick', 'unit', 'core', 'full')


def passed(sha, runs):
    """The newest gate run that passed every tier a release needs on commit sha (full id) with nothing uncommitted, or None.
    A run of an uncommitted tree doesn't count for the commit made from it: the release is built from the commit."""
    for f in sorted(glob.glob(os.path.join(runs, '*', 'summary.json')), reverse=True):
        try:
            with open(f, encoding='utf-8') as fh: s = json.load(fh)
        except (OSError, ValueError):
            continue
        short = str(s.get('sha') or '')
        if (len(short) >= 7 and sha.startswith(short) and s.get('verdict') == 'OK' and s.get('uncommitted') == 0
                and all(t in s.get('tiers', []) for t in RELEASE_TIERS)):
            return os.path.dirname(f)
    return None


def main():
    ap = argparse.ArgumentParser(description="Pick the gate's tiers from what a change touches (fork_gate.sh auto).")
    ap.add_argument('what', choices=('tiers', 'explain', 'release', 'lint', 'passed'))
    ap.add_argument('--sha', help='passed: the commit (default HEAD)')
    ap.add_argument('--runs', default=os.path.join(os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work'), 'run', 'gate'), help='passed: the gate runs folder')
    ap.add_argument('--gate', default=os.path.join(ROOT, 'dm', 'verify', 'fork_gate.sh'), help='lint: the gate script')
    ap.add_argument('--base', default=os.environ.get('DM_GATE_BASE') or 'origin/main')
    a = ap.parse_args()
    if a.what == 'passed':
        sha = git(ROOT, 'rev-parse', a.sha or 'HEAD').strip()
        run = passed(sha, a.runs)
        print(run or f"GATE_PASSED none: no run of {' '.join(RELEASE_TIERS)} passed on {sha[:9]} with nothing uncommitted in {a.runs}")
        return 0 if run else 1
    if a.what == 'lint':
        n, problems = lint(a.gate, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.gate)))))
        for problem in problems: print('  ' + problem)
        print(f"GATE_LINT {'RED' if problems or not n else 'OK'} {n} steps{', ' + str(len(problems)) + ' problems' if problems else ''}")
        return 1 if problems or not n else 0
    if a.what == 'release':
        print(' '.join(RELEASE_TIERS)); return 0
    mb, files = changed(a.base)
    tiers, each = pick(files)
    print(' '.join(PER_CHANGE))
    if a.what == 'explain':
        print(f"GATE_PICK against {a.base} (merge base {mb[:9]}): {len(files)} changed file{'s' if len(files) != 1 else ''} → {' '.join(PER_CHANGE)} now; the whole gate ({' '.join(RELEASE_TIERS)}) at the next release or app rebuild")
        todo = checks(files)
        for cmd, what, fs in todo:
            print(f"  also run: {cmd}   # {what} ({', '.join(fs[:3])}{' …' if len(fs) > 3 else ''})")
        if not todo: print('  no extra check for these files')
        rows = sorted(each.items(), key=lambda kv: (-ORDER.index(kv[1][0]), kv[0]))
        for f, (t, why) in rows[:40]:
            print(f'  at the release, {t:<5} exercises {f}: {why}')
        if len(rows) > 40:
            print(f'  … and {len(rows) - 40} more')
        long_only = [f for f in files if any(fnmatch.fnmatchcase(f, p) for p in LONG_ONLY)]
        if long_only:
            print(f"  note: only the long tier runs {', '.join(long_only[:6])}{' …' if len(long_only) > 6 else ''}: run dm/verify/fork_gate.sh long to exercise them (auto never picks it)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
