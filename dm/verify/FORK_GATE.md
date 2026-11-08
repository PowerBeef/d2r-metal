# The fork gate: `fork_gate.sh`

d2r-metal's verification run. It runs only what this Mac can check by itself: source checks, the test suites, scenarios on our own builds, the touch cases, faults, sanitizers and benchmarks. The reference behaviour comes from the original image (rewrite equivalence), our own baselines (`golden.py`) and the private analysis of the game's code; see [README.md](README.md#where-the-reference-behaviour-comes-from).

```bash
dm/verify/fork_gate.sh auto             # the tiers this change needs (below)
dm/verify/fork_gate.sh quick            # < 1 min: source checks
dm/verify/fork_gate.sh quick unit       # ~3 min: + the test suites
dm/verify/fork_gate.sh core             # ~9 min: scenarios on the CLI host + touch cases
dm/verify/fork_gate.sh full             # ~30 min: + Mac App, iOS package on the Mac, Metal validation, faults, replays, picture sets
dm/verify/fork_gate.sh long             # on demand, 1.5–2.5 h: benchmarks, sanitizer runs, fuzzing
dm/verify/fork_gate.sh device           # when the iPhone is reachable
```

Several tiers can be given at once; they run in the order given.

## Picking the tiers: `auto`

`fork_gate.sh auto` runs the tiers the change needs and first prints why, file by file (`dm/tools/gate_pick.py explain`; test `test_gate_pick.py`). The change is everything since the merge base with `origin/main` (`DM_GATE_BASE` names another base): commits not pushed yet, edits, deletions and new files, never the three files `setup.sh` rewrites. Each file asks for a tier and the gate runs up to the highest:

| Asks for | Files |
|---|---|
| quick unit | docs (`*.md`, `docs/`), unit tests, the switch registry, the launcher, release packaging, the engine-analysis and check tools, benchmarks, brand art, GitHub CI |
| core | the game host (`dm/host/`), scenario saves and helpers, the gate script, a curated scenario that runs on the CLI host, and any file no rule names |
| full | graphics (`dm/gfx/`), the iOS app, the Mac App front end and window, the picture settings (`dm_video`, `dm_picios`), game data, the binary rules and product, the scenario runner, pipeline warm-up, third-party code, the apps' text, the baselines and the rays replay, picture and fault scenarios, and a curated scenario whose `requires` line leaves out the CLI host (19 run only on the Mac App) |

The rules are `RULES` in `gate_pick.py`, first match wins. `auto` never picks `long` or `device`, and it doesn't replace `full` before rebuilding the apps or making a release: a host change gets `core`, which runs the scenarios on the CLI host but not on the two apps.

## Results

- Each step runs through `dm/tools/dmtest.py`, which writes `result.json` and a JUnit file into the step's folder under `$DM_ROOT/run/gate/<time>_<tiers>/`; the step's console output sits beside it as `<step>.console`.
- `steps.tsv` lists every step: name, verdict (OK / RED / ERROR / UNAVAIL, or INFO: the RED of an informational step, a measurement with no threshold, which doesn't make the gate red; `INFO_STEPS` in `fork_gate.sh`), seconds, unavailable items.
- `summary.json` and the last line, `FORKGATE OK|RED {json}`, give the tiers, the commit, the time, the red steps and the number of unavailable items.
- `$DM_ROOT/run/gate/latest` points at the most recent run.
- Unavailable items (⚪) are not red: a missing recording, a fixture save without what a case needs, or no phone, or a front run still disturbed after its redos (exit 3: another app took the front, a Space switch, display sleep; `scenarios/README.md`).
- **What takes the front:** the long tier's benchmarks (by design) and, in `full`, the iOS package on the Mac (`smoke_mac.sh`, the `ios-mac` scenarios: it takes the front even when launched with `open -g`; its scenarios are front runs, redone when disturbed). Every windowed scenario of `core` and the Mac App scenarios of `full` are background runs (`DM_TEST_BGRUN=1`): no focus taken, no Space switched, the window off screen.

## Tiers

| Tier | Steps |
|---|---|
| quick | `scan_literals.py`, `dm_i18n.py --check`, `switches.py check`, `enginemap.py check` (`docs/ENGINE_CHANGES.md` against the sources), `bash -n` on this script (`auto` may skip the tier where a typo sits), `scenario.py check` on every scenario, `check_hud_gen.sh`, `addrcheck.py` (cited game addresses; ⚪ without the analysis export) |
| unit | host, gfx and iOS suites; `regress.sh`; the Mac packaging tools (`test_mac_bundle.py`, `test_install.py`, the launcher's core tests) |
| core | builds the host; the scenarios without front-end steps run headless, three at a time; the ones that minimize, background or quit (or carry the `front` header) run windowed, one at a time, in the background; then the touch cases, three at a time on one shared UI overlay (each in its own folder on the deterministic clock, so load doesn't change a verdict: 1–2 min against 6 one at a time); the picture chain at this Mac's Recommended on the CLI host (`scenarios/picture/` rec, rec75, recfg, rect, rec75t, aa_toggle: this Mac's Recommended, 3D 75% with the MetalFX 3D pass, frame generation, Temporal at 100% and 75%, AA switched live); the motion check (`motion_gate.py`: frame generation and the hero's animation on a walk over open ground, against a control run); the pipeline warm-up check (`pso_seed.py gate --backend cli --headless`: the town route with Recommended's features forced builds no pipeline that the seed `dm/gfx/nvn/pso_manifest.tsv` or the reviewed allow-list `pso_allow.tsv` lacks, `PSO_GATE OK rows=0`) |
| full | rebuilds the Mac App test copy for the current commit, runs the scenarios on it at this Mac's Recommended picture (`DM_TEST_VIDEO_FIRSTBOOT=1`; full screen emulated in the background), then the pipeline warm-up check on it (`pso_seed.py gate --backend mac-app`); `smoke_mac.sh 1` (which also rebuilds the iOS package for the Mac), the same check on that package (`--backend ios-mac`), the scenarios on it; Metal API validation on one scenario and on the Recommended picture (`picture/rec.scn`); the Mac App first launches (`picture/mac_firstboot_m6`, `mac_firstboot_m1`: the whole Recommended set from the `picture` event); the fault scenarios; the golden replays, screenshot pages and FLIP picture sets (`golden.py shots check --set orig,rec,rec75,phone`, ⚪ until recorded); the Mac ray planes byte for byte against a kept capture (`rays_replay.sh check`, ⚪ until recorded) |
| long | `bench.py` on the four routes, one profile and the regression (first, while the machine is cool; at this Mac's Recommended in real full screen, in front; a disturbed rep is redone and otherwise ⚪ not judged); one true cold start of the Mac App (`pso_seed.py cold --backend mac-app`: a fresh bundle ID, so an empty Metal cache, in front; a row of its warm-up times, loading wait and pipelines built while drawing is appended to `$DM_ROOT/run/pso/cold.tsv`; informational); GPU heap 0 at the Native cap (`scenarios/heap/`, measurements: front runs in real full screen, one at a time); `explore.py` on the ASan and UBSan hosts; TSan alone; `fnfuzz.py` |
| device | `smoke.sh`, `lifecycle.sh` on the iPhone; ⚪ when it isn't reachable |

## Rules it keeps

- **The Mac game lock.** Windowed runs steal each other's frames. The gate takes the lock (`dm/ios/tools/mlock.sh`) once for the tiers that open windows and sets `XT_LOCK_HELD=1`, so nested tools don't take it again. `scenario.py` (the `cli` backend when windowed, and `mac-app`), `bench.py`, `explore.py` and `replay.py` now take it themselves when it isn't held; headless runs don't need it.
- **Test apps from the current commit.** The `mac-app` test copy (`$DM_ROOT/out/macapp_test`, bundle ID `org.d2rmobile.mac.test`) is rebuilt when its stamp (`.gate_head`) doesn't match the commit and the working tree state. Never the player's app in `out/macapp`.
- **Red markers.** `assertlib.RED_MARKERS` also catches Metal command-buffer errors and GPU faults (`logred.sh`'s patterns), checked first against the existing scenario logs for false hits.

## Engine-aware checks

`dm/host/dm_engine.c` reads the game's own UI and save state once per frame on the game's main thread (read-only; widgets tracked by name and UI generation, never by a kept pointer) and publishes it:

- **Control port:** `state` gains `panels` (visible panels, top first), `focus`, `focus_panel`, `cursor_mode`, `save_busy`, `save_pending`, `save_mounted`, `save_containers`, `exp_permille`, `quest_unread`; `engine` returns the whole snapshot plus the latest controller actions and UI messages.
- **Events:** `action` (controller actions under the game's own names, read from its action table at run time), `focus`, `save_request`, `save_stuck` (red: a requested save with no commit or rejection, or a container mounted too long), `save_alert`, `fault`.
- **Scenario checks:** `expect_state panels~=Name focus=Widget`, `check actions seq=…`, `check save_pairs`, `check saves_valid` (`dm/verify/d2save.py`, the game's load rules: header, version, size field, checksum, stash sections, gold cap). Every `cli` / `mac-app` run also checks its committed save.
- **On red:** the runner stores the host's `engine` snapshot as `engine.json` in the run folder.
- **lldb:** `dm ui`, `dm focus`, `dm saves`, `dm unit <id>` (`dm/verify/debugging.md`).

## Fault matrix

`dm/verify/scenarios/faults/` (run by `full`): a write-mode open failing in a game, a failed re-clone after a refused commit, a failed clone at boot (read-only session), a quit landing in a save, and the `save_stuck` positive control. Faults are armed with the control port's `fault` command (`arm` with `skip` / `count`, `delay`) or `DM_TEST_FS_FAILAT=<point>[@<skip>][:<count>]` / `DM_TEST_SAVE_DELAY`; each fault emits a `fault` event that its case requires, so a fault that never fires can't pass silently. `guestname` / `renameback` aren't reached in a normal run and stay with `dm/host/tests/t_fs.c`.

Found while building it: the game's save jobs run on its async job pool, and while one job is stuck the tasks queued every frame use up the pool's task memory; after about 8 s the pool's enqueue function (main+0x14f0590) writes through the null allocation and the game crashes. A save that blocks for seconds (slow storage, a host hook holding the save lock) is a crash, not just a late save.

## Our own baselines: `golden.py`

Instead of recordings of the original game, the fork records baselines from its own build, kept in `$DM_ROOT/data/switch/golden/` (never in git) and re-recorded only on purpose: every `record` needs `--reason`, which goes into `golden/LOG.tsv` with the commit.

```bash
python3 dm/verify/golden.py replay record --reason "…"   # dm/verify/golden/{walk,fight,belt}.scn: packets + per-tick state + the committed save
python3 dm/verify/golden.py replay check                 # replay on dmhost, tick by tick; savecmp on the replay's save; negative control
python3 dm/verify/golden.py shots record --reason "…"    # the 11 shotgate pages that need no recorded input, per renderer (m4 / m3)
python3 dm/verify/golden.py shots check                  # shotgate.py against the baseline, page thresholds as in the spec
python3 dm/verify/golden.py prefix                       # the touch cases' wilderness prefix (golden/prefix_wild.scn) + the save's MANIFEST.sha256
python3 dm/verify/golden.py list
```

- **Logic:** each route is replayed with the recorded packets (`replay.py`) and compared tick by tick (`tickdiff.py`). The replay also asks for a save at the recorded tick (`DM_TEST_SAVE_AT`), and `savecmp.py` compares that save with the recorded one (checksums self-consistent, times checked against their run's window, everything else byte for byte).
- **Negative control:** every check also replays the route with the client packets from the middle one onward dropped; it must diverge, or the comparison is blind and the check is red. (Dropping a single move isn't enough: the next packets re-target the same spot.)
- **Screens:** `cube` is ⚪ (the Warlock save has no Horadric Cube); `newchar` catches a black transition frame, so it only proves that frame is stable.
- **Rebaseline** after a deliberate change to what the routes see: the town spawn move (`dm_townwp.c`), the player count, custom-setting defaults, picture settings.
- `full` runs both checks; with no baselines they are ⚪.

## Playtesting and wider coverage

- **Front-end input from scenarios:** the control port's `fe` runs one line of `DM_TEST_NSMOUSE` input (virtual controller, keys, the text sheet, real mouse events) through the same front-end path as a player; `inputmode` switches the controller / touch layout and key caps (`padmode` event when done). Scenarios with `fe` steps run windowed.
- **`dm/tools/playbot.py`:** seeded player intents on player paths only (no `msg` steps, so none of the known crashers), with invariants after every intent: ticks advance while not paused, the HUD is on the panel stack, no focus once panels are closed; at the end no red, no `save_stuck`, a valid save. A red episode is rerun, then shrunk by intent (`explore.py`'s ddmin). `playbot.py soak --minutes 30` is the Mac soak, judged by `soak_report.py` (ticks, memory slope, handles / threads, frame time). `long` runs both.
- **Coverage scenarios:** `new_character_vkbd` (a character named with the controller's on-screen keyboard, through `fe`; `new_character_vkbd_mac` for the Mac App, whose front end waits for + to accept the name), `input_mode_switch` (controller ↔ touch, Xbox / PlayStation / Switch caps), `opmode_switch` (docked → handheld; the self-contained accept ⑩), `lang_zh`, `lang_es` (the game language really switched: `language` event), `mfx3d_dyn` (the iOS package's MetalFX 3D pass keeps its scaler set while Frame Drops steps the 3D scale down: `mfx` and `scale3d` events), and the existing `ios-mac` lifecycle scenarios in `full`.
- Found while building them: `state.in_game` stayed true after Save and Exit (HUD readings only refresh on ticks); `DM_OE_SCRIPT`'s `@ingame` trigger and `DM_TEST_LANG` silently did nothing on the CLI host. All fixed; each scenario now requires an event proving its switch happened.
- The first `full` run (22 min) caught one more: at mount the host rewrote the committed `Settings.json` (seeding, the language row) before comparing it with the last session's leftover work folder, so a clean leftover looked like uncommitted changes and was moved aside with a red line. The leftover is now judged first.
- Not covered: a language switch during play (the language applies when the save is mounted).

## Benchmarks that explain the time

- **Routes** (`dm/verify/bench/`): `town_loop`, `combat` (`tap_unit` fights in the Blood Moor), `ui_heavy` (panels and tabs), `rejoin` (Save and Exit, then rejoin).
- **New per-frame counters** in `nvn.tsv`: `@binds` (bind commands recorded: textures, samplers, buffers, programs, state) and `@cmdmem_cb` (the game's out-of-command-memory callbacks); `bench.py` reports them per segment with `@draws`. Counting *redundant* binds needs per-command-buffer shadow state and belongs to the s38 work.
- **`bench.py profile`**: one sampled run (`DM_SAMPLE=4:*`, with a `SAMPLE-THR` line naming each guest thread): per segment and thread role, the running share and the hottest functions under their engine names; every frame over 25 ms with what was running during it.
- **`bench.py baseline save` / `regress`**: ABBA against a kept copy of an earlier host, run on its own rewrite output and shader library (the snapshot `baseline save` takes); red only on a slowdown that stays significant after Holm's correction over every segment × metric (default metrics `cpu_ms.p50`, `nvn_ms.p50`, `proc_cpu_ms.p50`; `gpu_ms` is clock-scaled on this Mac); a `--reps` too small to reach significance is refused before any run, and a segment × metric left without a judgement (too few runs with a value) is ⚪, never OK; the ledger keeps every comparison. A baseline kept before snapshots existed is ⚪ until it is saved again.
- `long` runs the four routes, one profile and the regression (⚪ without a saved baseline, or with a rep still disturbed after its redos), all in front at this Mac's Recommended picture in real full screen (`bench.py --picture rec`, the default; `dm/tools/picture.py`).

## Touch cases and fixture saves

The touch cases start from `save_ref_warlock` (or `DM_TOUCH_SAVE`). Some were written against the original project's fixture save: an item in inventory slot (5,3), the Horadric Cube at (8,0). The checks on the starting state are **preconditions** (`lib.precondition`): when the save doesn't have what the case needs, the case is ⚪ unavailable (`assert.py` exit code 3, `touchchain.sh` exit code 3) instead of red, and the other checks don't count. A case whose prefix recording is missing is ⚪ the same way.

The fork generates what it can:

- `dm/verify/touch_fixture.sh inv53` runs `touchcases/_fixtures/inv53` (drags the scroll from (9,3) to (5,3)), has the game save (`DM_TEST_SAVE_AT`) and exports `save_ref_touch_inv53`. A case's `save` file points at it: `inv_tap`, `inv_drag`, `inv_drag_back`, `inv_drag_out`, `inv_long_press`.
- `golden.py prefix` records `prefix_wild_warlock` for `skill_main`.
- `inv_double_tap` stays ⚪: it needs the Horadric Cube.

## What it doesn't do

- No recording of the original game on a console or emulator: the baselines are our own.
- No upload: results stay in `$DM_ROOT/run/gate/`.
