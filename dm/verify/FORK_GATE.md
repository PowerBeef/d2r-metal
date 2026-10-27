# The fork gate: `fork_gate.sh`

中文：[FORK_GATE.zh-CN.md](FORK_GATE.zh-CN.md)

d2r-metal's own verification run. Upstream's acceptance run (`accept.sh`) depends on ground-truth recordings of the original game in a patched emulator; this fork has none, so most of its items are ⚪ unavailable here. The fork gate runs only what this Mac can check by itself: source checks, the test suites, scenarios on our own builds, the touch cases, faults, sanitizers and benchmarks. `accept.sh` stays for upstream merges.

```bash
dm/verify/fork_gate.sh quick            # < 1 min: source checks
dm/verify/fork_gate.sh quick unit       # ~6 min: + the test suites
dm/verify/fork_gate.sh core             # ≤ 15 min: scenarios on the CLI host + touch cases
dm/verify/fork_gate.sh full             # ~25 min: + Mac App, iOS package on the Mac, Metal validation, faults
dm/verify/fork_gate.sh long             # on demand, 1.5–2.5 h: benchmarks, sanitizer runs, fuzzing
dm/verify/fork_gate.sh device           # when the iPhone is reachable
```

Several tiers can be given at once; they run in the order given.

## Results

- Each step runs through `dm/tools/dmtest.py`, which writes `result.json` and a JUnit file into the step's folder under `$DM_ROOT/run/gate/<time>_<tiers>/`; the step's console output sits beside it as `<step>.console`.
- `steps.tsv` lists every step: name, verdict (OK / RED / ERROR / UNAVAIL), seconds, unavailable items.
- `summary.json` and the last line, `FORKGATE OK|RED {json}`, give the tiers, the commit, the time, the red steps and the number of unavailable items.
- `$DM_ROOT/run/gate/latest` points at the most recent run.
- Unavailable items (⚪) are not red: a missing recording, a fixture save without what a case needs, or no phone.

## Tiers

| Tier | Steps |
|---|---|
| quick | `scan_literals.py`, `dm_i18n.py --check`, `switches.py check`, `scenario.py check` on every scenario, `check_hud_gen.sh` |
| unit | host, gfx, gfx older-API and iOS suites; `regress.sh` |
| core | builds the host; the scenarios without front-end steps run headless, three at a time; the ones that minimize, background or quit run windowed, one at a time; then the touch cases |
| full | rebuilds the Mac App test copy for the current commit, runs the scenarios on it; `smoke_mac.sh 1` (which also rebuilds the iOS package for the Mac), the scenarios on that package; one scenario under Metal API validation; the fault scenarios |
| long | `bench.py` on the town route (first, while the machine is cool); `explore.py` on the ASan and UBSan hosts; TSan alone; `fnfuzz.py` |
| device | `smoke.sh`, `lifecycle.sh` on the iPhone; ⚪ when it isn't reachable |

## Rules it keeps

- **The Mac game lock.** Windowed runs steal each other's frames. The gate takes the lock (`dm/ios/tools/mlock.sh`) once for the tiers that open windows and sets `XT_LOCK_HELD=1`, so nested tools don't take it again. `scenario.py` (the `cli` backend when windowed, and `mac-app`), `bench.py`, `explore.py` and `replay.py` now take it themselves when it isn't held; headless runs don't need it.
- **Test apps from the current commit.** The `mac-app` test copy (`$DM_ROOT/out/macapp_test`, bundle ID `org.d2rmobile.mac.test`) is rebuilt when its stamp (`.gate_head`) doesn't match the commit and the working tree state. Never the player's app in `out/macapp`.
- **Red markers.** `assertlib.RED_MARKERS` also catches Metal command-buffer errors and GPU faults (`logred.sh`'s patterns), checked first against the existing scenario logs for false hits.

## Touch cases and fixture saves

The touch cases start from `save_ref_warlock` (or `DM_TOUCH_SAVE`). Some were written against upstream's fixture save: an item in inventory slot (5,3), the Horadric Cube at (8,0). The checks on the starting state are **preconditions** (`lib.precondition`): when the save doesn't have what the case needs, the case is ⚪ unavailable (`assert.py` exit code 3, `touchchain.sh` exit code 3) instead of red, and the other checks don't count. A case whose prefix recording is missing is ⚪ the same way.

## What it doesn't do

- No ground-truth comparison: `accept.sh` items ①–⑧ need recordings of the original game.
- No upload: results stay in `$DM_ROOT/run/gate/`.
