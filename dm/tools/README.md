# dm/tools: tool index

中文: [README.zh-CN.md](README.zh-CN.md)

Three kinds of things live here. The first you will use when following the front page or the docs; the second is the measurement and regression scaffolding we use during development, published as is; the third is a set of one-off probes written when the game program was first analysed, kept for the record.

## What you will use

| File | What it does |
|---|---|
| `patch_nso.py` | The program rewriter: rewrites the few instructions in the game program that cannot run unchanged under Apple's kernel and leaves everything else byte-for-byte intact. Called by `setup.sh` |
| `extract_game.sh` | Extracts the two parts this project needs (program files and resources) from your own game package into the workspace |
| `product.sh` | The identifier of the current rewrite output lives in one place (`dm/PRODUCT`); scripts get paths through this |
| `dmx.py` | A minimal read-only reader for the game's resource storage: list files, fetch a file |
| `casc_overlay.py` | Builds the overlay at build time: points chosen resource paths at our new content without touching the original archives |
| `astc_pack.py` | Supplement pack for older devices: converts BC-format game textures offline into ASTC, which those devices support |
| `gen_bright_levels.py` | Generates the level table used by the 3D brightness boost (built from your game files) |
| `tact_bootlist.py` | Generates the startup prefetch list: resource blocks needed before character select, decoded ahead of time at launch |

## Measurement and regression scaffolding

| File | What it does |
|---|---|
| `crashlog.py` | Reads and symbolicates the host's `crash.json` (`DM_CRASH_JSON`) and collects the system `.ips` reports |
| `guestsyms.py`, `test_guestsyms.py` | Symbol table for the game program (`.eh_frame` functions, dynamic symbols, source-comment notes), kept in the local build cache |
| `explore.py`, `test_explore.py` | Automatic exploration: seeded random play through the scenario runner, shrinking a red run to a minimal scenario ([debugging.md](../verify/debugging.md)) |
| `replay.py` | Records a scenario run's command packets and per-tick state, replays them on host builds and finds the first divergent tick |
| `playbot.py`, `test_playbot.py` | Playtest bot: seeded player intents (panels, pause page, belt, walking, leaving town to fight, save and rejoin) on player paths only, invariants between them, shrinking by intent; `soak` runs one long session judged by `soak_report.py` ([dm/verify/FORK_GATE.md](../verify/FORK_GATE.md)) |
| `tickdiff.py`, `test_tickdiff.py` | First divergent tick between two `DM_TICKS` records, with the slots, units and fields that differ |
| `lldb/dm.py` | LLDB command `dm` (`modules`, `where`, `bt`, `break`, `tick`): game addresses as module offsets and guest functions ([debugging.md](../verify/debugging.md)) |
| `gpucap.py` | GPU frame capture and summary, live Metal stats and the Metal HUD (wraps `gpucapture`, `gpudebug --json`, `metalperftrace`) |
| `boothist.py`, `test_boothist.py` | Boot-time history ledger: boot stages per run from the event stream and the `DM_BOOTPROF` table, compared across revisions |
| `bench.py`, `test_bench.py` | Fixed-route benchmarks ([dm/verify/bench](../verify/bench/README.md)): per-segment frame, CPU, NVN and GPU time per frame into bench.json; interleaved A/B with Mann-Whitney U and a bootstrap interval, recorded in a ledger |
| `imgdiff.py`, `test_imgdiff.py` | Perceptual difference of two screenshots (LDR-FLIP: what the eye can tell apart at a viewing distance), for A/B and before / after comparisons; `--max-mean` judges OK / RED |
| `scenario.py`, `test_scenario.py` | Scenario runner: plays the step-by-step scenarios in [dm/verify/scenarios](../verify/scenarios/README.md) through the control port on the CLI host, the iOS package on a Mac or a device, and judges them by events, state and checks |
| `dmctl.py`, `test_dmctl.py` | Client for the control port ([control_port.md](../verify/control_port.md)): send commands, wait for events, from Python or the shell |
| `dmevents.py`, `test_dmevents.py` | Reads and checks the `DM_EVENTS` event stream ([event_stream.md](../verify/event_stream.md)): timeline summary, expected event order, why a run went red, wait for an event |
| `switches.py`, `test_switches.py` | Registry of the `DM_*` environment switches: `gen` writes [dm/switches.tsv](../switches.tsv) (who reads each switch, documented or not) from a scan of the sources; `check` fails when the registry is stale or a script sets a switch nothing reads |
| `dmtest.py`, `test_dmtest.py` | Uniform result wrapper for any test command: passes its output through and turns its verdict lines into `result.json`, optional JUnit XML and a last `RESULT {json}` line |
| `conc_watch.sh` | Concurrency check for timed runs: a timing taken while other heavy work was running is discarded |
| `sampagg.py`, `lockalign.py` | Aggregation of sampling logs; call-chain statistics for lock-holding threads |
| `hle_inventory.py` | Inventory of the host's system service implementations (full / simplified / stub) |
| `sdis.py`, `sideeff.py` | Disassemble one function; walk the call graph for a function's side effects |
| `blackcols.py` | Checks a screenshot for fully black columns (whether the picture fills the screen) |
| `guirun.sh` | Starts a program inside the graphical session and collects its output |

## One-off probes from the early analysis

`nso.py` (splits the game program into segments), `fn.py` (function boundaries from the unwind table), `sweep.py` (linear instruction sweep), `x18.py` and `cat18.py` (find and classify instructions using x18, a register Apple reserves), `xref_sdk.py` (cross-references of system library functions), `tp.c` (checks how the thread pointer register behaves under Apple's kernel). They were written before the rewrite approach was settled and depend on intermediate files from that time; they are not guaranteed to run as they are. The basis of the rewrite approach is in [dm/README.md](../README.md) and [dm/verify/README.md](../verify/README.md).
