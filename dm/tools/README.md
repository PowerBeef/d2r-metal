# dm/tools: tool index

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
| `tact_bootlist.py` | Generates the startup prefetch list: resource blocks needed before character select, decoded ahead of time at launch |
| `mac_app.sh`, `mac_bundle.py`, `test_mac_bundle.py` | Builds the Mac App (`D2R Metal.app`, default `$DM_ROOT/out/macapp`; [docs/MAC.md](../../docs/MAC.md)); `mac_bundle.py` relocates the dylibs the host links into the bundle |
| `install.py`, `test_install.py` | Install engine of D2R Metal Launcher: builds the Mac app on the player's own Mac from their own game files with a release's prebuilt tools, and on an update redoes only what the release changed ([docs/LAUNCHER.md](../../docs/LAUNCHER.md)) |
| `launcher_app.sh` | Builds `D2R Metal Launcher.app` from `dm/launcher` (ad-hoc signed by default) |
| `release_mac.sh`, `release_pins.txt`, `release_requirements.txt`, `release_nuget.tsv` | Builds a launcher release on this Mac (the tools pack, the runtime pack, the signed and notarized launcher, a draft GitHub release with the signed manifest) from pinned inputs |
| `manifest_sign.swift` | Signs a release's `manifest.json` for the launcher (Ed25519; `keygen`, `public`, `sign`) |
| `macho_deps.py` | Checks that a release pack is self-contained: every Mach-O in it loads only macOS's own libraries or a file inside the pack |

## Measurement and regression scaffolding

| File | What it does |
|---|---|
| `re/` | The private engine-analysis database of the game's `main` (Ghidra; build, query server, names export, `addrcheck.py`); see [re/README.md](re/README.md) |
| `gate_pick.py`, `test_gate_pick.py` | What to run for a change (`fork_gate.sh auto`) and what a release needs: `tiers`, `explain`, `release`, `lint`, `passed` ([dm/verify/FORK_GATE.md](../verify/FORK_GATE.md)) |
| `enginemap.py`, `test_enginemap.py` | Keeps [docs/ENGINE_CHANGES.md](../../docs/ENGINE_CHANGES.md) true: `check` requires a row for every change the port makes to the game's engine, and every row to point at things that exist (`ENGINEMAP OK` or `ENGINEMAP RED`) |
| `quiet.py`, `test_quiet.py` | Waits for a quiet machine before a CPU-heavy tool starts: while the Mac game lock is held, a heavy job beside it changes what those runs measure |
| `workspace_prune.py`, `test_workspace_prune.py` | Keeps the workspace from filling the disk: lists what is safe to remove from `$DM_ROOT` and, with `--apply`, moves it to the Trash |
| `crashlog.py` | Reads and symbolicates the host's `crash.json` (`DM_CRASH_JSON`) and collects the system `.ips` reports |
| `nso_fnmap.py`, `test_nso_fnmap.py` | Carries game addresses to another build of a module (after a game update): `nso_fnmap.py <old> <new> 0xoffset… | --cited` maps code offsets by function hashes with address bits masked and globals by their adrp references; `--cited` maps every `main+0x…` the sources cite. Last line `FNMAP OK|RED`. Adapted from nxnative (MIT) |
| `fuzz/` (`fuzzlib.py`, `fuzz_nso.py`, `fuzz_d2save.py`, `fuzz_dmx.py`, `known.tsv`, `corpus/`), `test_fuzz_corpus.py` | Fuzzing of the file parsers (the NSO decoder, the save checkers, the archive block decoder) from synthetic seeds: `python3 dm/tools/fuzz/fuzz_nso.py --time 60` (atheris when installed, else blind mutations). Findings go to `known.tsv` and `corpus/`, which `test_fuzz_corpus.py` replays in the host suite and CI. Adapted from nxnative (MIT) |
| `guestsyms.py`, `test_guestsyms.py` | Symbol table for the game program (`.eh_frame` functions, dynamic symbols, source-comment notes), kept in the local build cache |
| `explore.py`, `test_explore.py` | Automatic exploration: seeded random play through the scenario runner, shrinking a red run to a minimal scenario ([debugging.md](../verify/debugging.md)) |
| `replay.py` | Records a scenario run's command packets and per-tick state, replays them on host builds and finds the first divergent tick |
| `playbot.py`, `test_playbot.py` | Playtest bot: seeded player intents (panels, pause page, belt, walking, leaving town to fight, save and rejoin) on player paths only, invariants between them, shrinking by intent; `soak` runs one long session judged by `soak_report.py` ([dm/verify/FORK_GATE.md](../verify/FORK_GATE.md)) |
| `pso_seed.py`, `test_pso_seed.py` | The pipeline warm-up seed (`dm/gfx/nvn/pso_manifest.tsv`): `normalise` checks and normalises a manifest the way the graphics layer does; `record` runs the 71-row route matrix (the front end and Act 1 on the Mac App at this Mac's Recommended and on the iOS package on the Mac, Act 1 on the CLI host under each force, a waypoint tour of Acts 1-5, then the warp stops) and `merge` builds the new seed from it; `check` / `gate` list the pipelines a run built that the seed lacks (`PSO_GATE OK rows=0` is the coverage pass); `cold` measures a true cold start (fresh Mac App bundle ID, or the iOS package's Metal cache moved to the Trash) into `$DM_ROOT/run/pso/cold.tsv`. The fork gate runs `gate` in core (CLI host) and full (Mac App, iOS package) and one `cold` in long ([FORK_GATE.md](../verify/FORK_GATE.md)) |
| `c2s.py`, `test_c2s.py` | Readable view of recorded client command packets (`summary`, `near`): each packet number labelled from `dm/verify/c2s_opcodes.tsv`, lengths checked ([command_packet_format.md](../verify/command_packet_format.md)) |
| `tickdiff.py`, `test_tickdiff.py` | First divergent tick between two `DM_TICKS` records, with the slots, units and fields that differ |
| `lldb/dm.py` | LLDB command `dm` (`modules`, `where`, `bt`, `break`, `tick`, `ui`, `focus`, `saves`, `unit`): game addresses as module offsets and guest functions, the interface's panels and focus, the save state, a server unit ([debugging.md](../verify/debugging.md)) |
| `gpucap.py` | GPU frame capture and summary, live Metal stats and the Metal HUD (wraps `gpucapture`, `gpudebug --json`, `metalperftrace`) |
| `boothist.py`, `test_boothist.py` | Boot-time history ledger: boot stages per run from the event stream and the `DM_BOOTPROF` table, compared across revisions |
| `bench.py`, `test_bench.py` | Fixed-route benchmarks ([dm/verify/bench](../verify/bench/README.md)): per-segment frame, CPU, NVN and GPU time per frame into bench.json; interleaved A/B with Mann-Whitney U and a bootstrap interval, recorded in a ledger. Runs default to this Mac's Recommended picture in full screen (`--picture rec`; `orig` is the host's own defaults in a window). `baseline save` keeps a host with its rewrite output and shader library; `regress` runs it against the current host with Holm's correction over every segment × metric (default metrics `cpu_ms.p50`, `nvn_ms.p50`, `proc_cpu_ms.p50`: `gpu_ms` is clock-scaled on a fast Mac) and refuses a `--reps` too small to reach significance |
| `bench_why.py`, `test_bench_why.py` | Why a run's frames cost what they cost, from its per-frame records, its marks and the whole-process sampler; `bench.py profile` runs it |
| `picture.py`, `test_picture.py` | The picture settings test runs use (`REC`, `RECT`, `CONV`, `REC75`, `REC75T`, `PHONE`), each Video-tab key pinned through its forcing switch, for `bench.py --picture`, the golden picture sets and the picture scenarios; the test ties them to the Recommended model's cases and the front ends' recommendations |
| `imgdiff.py`, `test_imgdiff.py` | Perceptual difference of two screenshots (LDR-FLIP: what the eye can tell apart at a viewing distance), for A/B and before / after comparisons; `--max-mean` judges OK / RED |
| `lookbook.py` | Picture-quality look set: screenshots of a rendering variant on the screenshot-gate pages, compared with a 2x supersampled reference of the same host |
| `motion_scan.py` | Measures motion and animation in a run's shown frames (a frame recording or a video file): cadence, late frames, the generated / real order |
| `arrdump.py`, `test_arrdump.py` | Reads the files `DM_NVN_ARRDUMP` writes: the raw blocks of the compressed texture arrays the game copies its terrain materials into |
| `scenario.py`, `test_scenario.py`, `test_scenario_bg.py`, `test_scenario_schemes.py` | Scenario runner: plays the step-by-step scenarios in [dm/verify/scenarios](../verify/scenarios/README.md) through the control port on the CLI host, the Mac App, the iOS package on a Mac or a device, and judges them by events, state and checks |
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
