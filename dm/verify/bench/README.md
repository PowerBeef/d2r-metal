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

## Routes

| Route | Segments | Notes |
|---|---|---|
| `town_loop.scn` | `town` (a loop inside the Rogue Encampment), `wild` (out of the south gate into the Blood Moor) | `save warlock`, fixed map seed (`DM_GAME_SEED`), `walk_to` cell coordinates; about 32 s per run. |

To add a route, write a scenario with `mark <name>` before each segment and `mark end` after the last, and keep it deterministic: a fixed save, a fixed `DM_GAME_SEED`, and moves that don't depend on timing.
