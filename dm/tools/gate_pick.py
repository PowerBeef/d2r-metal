#!/usr/bin/env python3
"""Pick the gate's tiers from what a change touches (fork_gate.sh auto).

  gate_pick.py tiers [--base REF]     the tiers, in order, on one line: quick unit [core] [full]
  gate_pick.py explain [--base REF]   the same, then every changed file with the tier it asks for and why

The change is everything between the merge base with origin/main (or --base, or DM_GATE_BASE) and the working tree:
commits not pushed yet, staged and unstaged edits, deletions and new files. The three files setup.sh rewrites are
never part of it.

Each file takes the tier of the first rule that matches it (RULES below; `*` in a pattern also matches `/`), and the
gate runs up to the highest: quick and unit always, core when a file needs a game run, full when it needs the Mac App,
the iOS app on the Mac, the replays or the picture sets. A curated scenario asks for core when it runs on the
command-line host and for full when its `requires` line leaves that host out. Files no rule names ask for core.

auto never picks long or device, and it does not replace full before rebuilding the apps or making a release.
"""
import argparse
import fnmatch
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ORDER = ('quick', 'unit', 'core', 'full')
SETUP_FILES = ('dm/gfx/tools/dmsh_chain.lock', 'dm/gfx/nvn/dmsh_chain.h', 'dm/gfx/nvn/c1_full.tsv')   # rewritten by setup.sh, never committed
SCENARIO = 'scenario'   # a curated scenario: core or full by its requires line

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
    ('dm/host/app_mac*', 'full', 'the Mac App front end'),
    ('dm/host/front_mac.m', 'full', 'the Mac App front end'),
    ('dm/host/front.h', 'full', 'the front ends'),
    ('dm/host/dm_screen_mac.*', 'full', 'the Mac App window'),
    ('dm/host/dm_picios.*', 'full', 'the iOS picture settings'),
    ('dm/host/dm_video.*', 'full', 'the picture settings, which the Mac App video scenarios check'),
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
    # Core: the game host, the gate and anything else that a game run uses
    ('dm/host/*', 'core', 'the game host'),
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
        lines = open(os.path.join(root, path), encoding='utf-8').read().splitlines()
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
    """The tiers to run, in order, and each file's (tier, reason)."""
    each = {f: classify(f, root) for f in files}
    top = max([ORDER.index('unit')] + [ORDER.index(t) for t, _ in each.values()])
    return list(ORDER[:top + 1]), each


def main():
    ap = argparse.ArgumentParser(description="Pick the gate's tiers from what a change touches (fork_gate.sh auto).")
    ap.add_argument('what', choices=('tiers', 'explain'))
    ap.add_argument('--base', default=os.environ.get('DM_GATE_BASE') or 'origin/main')
    a = ap.parse_args()
    mb, files = changed(a.base)
    tiers, each = pick(files)
    print(' '.join(tiers))
    if a.what == 'explain':
        print(f"GATE_PICK against {a.base} (merge base {mb[:9]}): {len(files)} changed file{'s' if len(files) != 1 else ''} → {' '.join(tiers)}")
        rows = sorted(each.items(), key=lambda kv: (-ORDER.index(kv[1][0]), kv[0]))
        for f, (t, why) in rows[:40]:
            print(f'  {t:<5} {f}: {why}')
        if len(rows) > 40:
            print(f'  … and {len(rows) - 40} more')
    return 0


if __name__ == '__main__':
    sys.exit(main())
