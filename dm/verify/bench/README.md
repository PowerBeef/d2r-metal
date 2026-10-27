# Benchmarks

中文：[README.zh-CN.md](README.zh-CN.md)

A benchmark route is a [scenario](../scenarios/README.md) with `mark <segment>` steps. `dm/tools/bench.py` runs it several times and reads two per-frame records:
- `DM_FRAMES`: the present interval of every frame, on the event clock;
- `DM_NVN_TSV` (timing rows only): frame, NVN, GPU and frame-thread CPU time per frame, keyed by the frame number that each `mark` event also carries.

The results are statistics per segment.

```bash
python3 dm/tools/bench.py run dm/verify/bench/town_loop.scn --reps 3                 # bench.json in $DM_ROOT/run/bench/
python3 dm/tools/bench.py ab dm/verify/bench/town_loop.scn --reps 6 \
    --a-env DM_3D_SCALE=1.0 --b-env DM_3D_SCALE=0.5 --metric gpu_ms.p50             # or --a-host / --b-host: two dmhost builds
python3 dm/tools/bench.py show bench.json
```

## What is measured

| Series | Source | Notes |
|---|---|---|
| `present.*` (`fps`, `mean_ms`, `p95_ms`, `p99_ms`, `slow_share` = share above 20.8 ms, `dropped`) | `DM_FRAMES` (`dm_perf_present`, same source as `perf.txt`) | Pinned by vsync on a fast Mac (16.67 ms), so it shows hitches rather than headroom. |
| `cpu_ms.*` | frame-thread CPU time | Not quantized by the refresh rate. |
| `nvn_ms.*` | time inside NVN calls (translation layer) | Default A/B metric (`nvn_ms.p50`). |
| `gpu_ms.*` | GPU time per frame (read 8 frames late) | |
| `frame_ms.*`, `wait_ms.*` | game frame interval, image-acquire wait | |

Each series has `mean`, `p50`, `p95` and `p99`. `bench.json` keeps every run, plus the median over runs per segment and metric (`summary`).

The environment is recorded with each result: git revision (and whether the tree is dirty), machine model, OS, CPU and Game Mode status (`gamepolicyctl`). Before each run, `pmset -g therm` is checked. While the machine reports a thermal warning or CPU speed limit, the run waits (up to `--cool` seconds). The thermal state before and after each run is stored. `DM_STATEREP=1` is set, so the system's Metal performance recording can be split by game state (`metalperftrace overview --aggregate --domain GameState`).

## A/B

`ab` runs the variants interleaved as ABBA (so slow thermal or clock drift affects both alike), `--reps` runs each. For every segment it compares the chosen metric's per-run values:
- **Mann-Whitney U**: two-sided, exact by permutation when there are at most 20000 arrangements, otherwise the normal approximation with tie correction.
- **Bootstrap**: a 95% interval of the B − A difference in medians, with a fixed seed.

A difference is reported only when the interval lies entirely on one side of 0 and p < 0.05. With 4 runs per variant the smallest possible p is 2/70 ≈ 0.029, so use at least 4 and preferably 6. Every comparison is appended to `$DM_ROOT/run/bench/ledger.tsv`.

Checked on this tree (Apple M6, macOS 27):
- **A/A run** (two identical variants): "no difference", town −0.004 ms [−0.032, +0.056], p = 1.0.
- **Half 3D resolution** (`DM_3D_SCALE` 1.0 vs 0.5): "B faster", GPU p50 6.28 → 3.79 ms [−2.51, −2.46], p = 0.029.

## Profiling a run

| Tool | Command | What you get |
|---|---|---|
| Signposts | `xcrun xctrace record --template Logging --attach <pid> --time-limit 10s` | `XTRACE_*` intervals (subsystem `org.d2rmobile`, Points of Interest): the translation thread's `dm-x-submit`, `dm-x-present`, `dm-x-frame`, `dm-x-wait-sub`, `dm-game-syncwait`, plus `dm-count` values. Off the timeline they cost one check. |
| Live Metal stats | `python3 dm/tools/gpucap.py listen --pid <pid> --seconds 10`, or `bench.py run … --mpt` | `metalperftrace listen`: on-glass frame interval, present lateness and skipped frames per second. |
| Always-on Metal record | `metalperftrace collect --last 10m <dir>` then `metalperftrace overview --aggregate --domain GameState <trace>` | The system's own record, split by `game_state` when the run had `DM_STATEREP=1`. |
| GPU capture | `python3 dm/tools/gpucap.py capture --pid <pid>` (or the scenario step `gpu_capture`) | One frame at the layer boundary, about 4 GB; the process must start with `MTL_CAPTURE_ENABLED=1`. `gpucap.py summary <trace>` lists command buffers, encoders, draws and dispatches (`gpudebug --json`). Capturing pauses presentation for a few seconds, so the host logs a red "display link silent" line (code `e6db5cb6`). |
| Sampled attribution | `python3 dm/tools/bench.py profile <route> [--ms 4] [--hitch-ms 25]` | One run with every guest thread sampled (`DM_SAMPLE=<ms>:*`). Per segment and thread role (main, job workers, IO pool, audio, other): the running share and the hottest game functions, named from the local engine symbol table (`guestsyms.py`); then every frame over `--hitch-ms` (`@frame_ms` in `nvn.tsv`) with the functions running during it. Writes `profile.json`. |
| Metal HUD | `python3 dm/tools/gpucap.py hud --pid <pid> on|off` | The system performance overlay, switched on a running process (`metalperftrace setup`). |

## Regression against a kept baseline

```bash
python3 dm/tools/bench.py baseline save                 # keep this tree's dmhost (and its gfx/nvn tables) as $DM_ROOT/run/bench/baseline/<rev>, current → it
python3 dm/tools/bench.py regress <route> [--reps 6] [--metrics cpu_ms.p50,nvn_ms.p50,gpu_ms.p50]
```

`regress` runs the baseline host (A) against the current one (B) in ABBA order and judges each metric as `ab` does. Only "B slower" (p < 0.05, interval on one side of 0) is red; every row goes to the ledger. With no baseline it is ⚪ (exit code 3).

## Boot-time history

Every benchmark run and every `dm/ios/tools/bootprof.sh` run appends the time from process start to each boot stage to `$DM_ROOT/run/bootprof/history.tsv`, with the git revision. The stages come from the event stream (`boot`, `main_menu`, `game_enter`, `loading_done`) and from the `DM_BOOTPROF` table (⓪–⑥). `python3 dm/tools/boothist.py show --label bench:cli` compares the median of the latest runs with the runs before and marks stages more than 10% and 50 ms slower.

## Routes

| Route | Segments | Notes |
|---|---|---|
| `combat.scn` | `walk` (to the Blood Moor), `fight` (`tap_unit` on monsters) | `save warlock`, fixed map seed; the path of `golden/fight.scn` |
| `ui_heavy.scn` | `panels` (the combined panel, tabs back and forth, the pause page) | `save warlock` |
| `rejoin.scn` | `leave` (Save and Exit to the front end), `join` (rejoin until the loading screen goes away) | `save warlock` |
| `town_loop.scn` | `town` (a loop inside the Rogue Encampment), `wild` (out of the south gate into the Blood Moor) | `save warlock`, fixed map seed (`DM_GAME_SEED`), `walk_to` cell coordinates; about 32 s per run. |

To add a route, write a scenario with `mark <name>` before each segment and `mark end` after the last, and keep it deterministic: a fixed save, a fixed `DM_GAME_SEED`, and moves that don't depend on timing.
