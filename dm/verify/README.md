# dm/verify — Verification Tools

d2r-metal rewrites the Switch version of the game program and runs it directly on macOS and iOS. This directory answers two questions: does the rewritten program behave the same as the original, and has a change broken something that already worked.

Terms used throughout this directory:

- **Build output**: the output directory of the rewriter `dm/tools/patch_nso.py` for the three modules `main`, `sdk` and `subsdk0`, at `$DM_ROOT/build/cache/dm/out/<codename>-<kind>`; the kinds are `keypage`, `rules` and `touch`. `dm/tools/product.sh build` produces all of them, and `product.sh path <kind>` prints the path.
- **Host**: `dmhost`, built from `dm/host`, which loads the build output on a Mac and runs the game. Most verification runs against it.
- **Baseline**: a recording made from our own build (`golden.py`): per-tick state, saves, screenshots. Later builds are compared against it.
- **Tick**: one step of the game's server-side logic. Many criteria are "identical tick by tick".
- **Run directory**: the directory holding all the output of one run (logs, screenshots, record files).
- `$DM_ROOT`: the workspace root, default `~/dm-work`. Game files, build outputs, baselines and run artifacts all live under it and never go into the repository.

## What you need for what you run

| What you need | What you can run |
|---|---|
| Repository + Python environment | The self-tests `touchcases/test_lib.py`, `test_assertlib.py`, `test_golden.py`, `test_d2save.py`: synthetic inputs only, no host, no game files |
| Game files + build output (prepared by `./setup.sh`) | `regress.sh` (rewrite equivalence; also needs llvm, see "External tools"), `fnfuzz.py`, the host and graphics unit tests, the scenarios (`dm/tools/scenario.py`) |
| Plus a standard save | `touchchain.sh` (touch feature cases), `golden.py` (baselines), the benchmarks |

External tools:

- The Python packages `lz4`, `capstone`, `unicorn`, `numpy` and `Pillow`, installed in `$DM_ROOT/build/cache/dm/venv` (created by `./setup.sh`). Most scripts use this environment's interpreter by default (`DM_PY` overrides it); a few scripts and the `make` targets call `python3` directly, so run `source ~/dm-work/build/cache/dm/venv/bin/activate` first.
- `structck.py` uses `llvm-objdump` and `llvm-objcopy` as an independent disassembler; their directory is given by `LLVM_BIN`, default `/opt/homebrew/opt/llvm@23/bin`. If that directory does not exist, set `LLVM_BIN=$(brew --prefix llvm)/bin`.
- Several scripts use GNU `timeout` (coreutils).

## The gate: `fork_gate.sh`

`fork_gate.sh quick|unit|core|full|long|device` runs the checks below in tiers, each step with a machine-readable result. See [FORK_GATE.md](FORK_GATE.md).

## Where the reference behaviour comes from

There is no recording of the original game running on a console or an emulator. Three references take its place:

1. **The original image itself, executed instruction by instruction** (`regress.sh`, `fnfuzz.py`): every rewritten site must do exactly what the original instruction did, checked under Unicorn.
2. **Our own baselines** (`golden.py`): fixed routes replayed tick-identically, the saves they write, and the screenshot pages, recorded from a known-good build. A change that moves them must say why (`golden.py … record --reason`).
3. **The game's own code** (`dm/tools/re/README.md`): a private analysis database of `main`, kept in `$DM_ROOT/re` and never in the repository. It answers "what does the game do here" statically; dynamic questions go to the headless host under lldb (`dm/tools/lldb/dm.py`) or a `DM_PROBE` scenario. Code and commits cite a game function by its `main+0x…` address and its role, never by pasting analysis output.

## Rewrite-equivalence regression: `regress.sh`

Verifies that the rewrite has not changed the program's behaviour. Needs only the game files and the build output, and finishes in seconds.

```
dm/verify/regress.sh [--fresh] [--struct-only] [build output dir] [module…]
```

By default it verifies `main sdk subsdk0` of the `keypage` build output. It runs three things in order:

| Tool | What it verifies |
|---|---|
| `structck.py` | **Structural check**. Executes no code and checks the rewriter's output word by word: apart from registered cuts, non-code segments are byte-for-byte identical to the original image; the words changed in the code segment are exactly the registered rewrite sites; an independent disassembler recounts the instructions that should be changed, with none missed and none extra; the patch area no longer contains the original instructions that cannot be executed; the bytes of every host gate, argument gate and observation gate equal the template and match the gate table one to one. The full list of checks is in the file header |
| `equiv.py` | **Site-by-site equivalence**: using Unicorn (a CPU emulation library), for each rewrite site, executes the original instruction and the patch block separately from several sets of random state, and compares all general-purpose and vector registers, flags, stack pointer, memory writes, end address and memory-access faults. Includes a mutation self-check: a batch of patch blocks is deliberately broken, and every one must be judged non-equivalent |
| `gatefn/` | **Gate function**: builds a test bench from the host's current `dm/host/entry.S`, executes each gate, and checks the register write-back convention |

- `--struct-only`: runs only the structural check and the gate function check, not `equiv.py`.
- `--fresh`: first regenerates a copy with this worktree's rewriter into `$DM_ROOT/run/verify/dm/fresh/` and verifies that, and also compares it file by file with the given directory; used to detect a stale shared build output. Run it whenever `patch_nso.py` changed.
- Environment variables: `DM_PY`, `DM_NSO_DIR` (directory holding the original modules, default `$DM_ROOT/data/switch/3.3.94012/Update/ExeFS`), `SEED`, `JOBS`, `LLVM_BIN`.
- Output: a summary table with one row each for structure and equivalence per module, with the denominator and the coverage; the last line is `REGRESS OK|FAIL <build output name>`, and any failure gives exit code 1. The log is at `$DM_ROOT/run/verify/dm/regress_<timestamp>.log`.

Related standalone tools:

- `fnfuzz.py <build output dir> <module> <NSO>`: function-level differential fuzzing. For each function containing a rewrite site, the original image and the rewritten image each execute the whole function on the same random input, and the final states are compared. Slower than `equiv.py`; the `long` gate tier runs it.
- `keypage_check.py`: checks key-page indirect accesses; see the file header.
- `nsoimg.py`: unpacks an NSO into a memory image, shared by the tools above.

## Baselines: `golden.py`

```
python3 dm/verify/golden.py replay|shots check        # compare with the kept baselines
python3 dm/verify/golden.py replay|shots record --reason "…"
python3 dm/verify/golden.py prefix [--reason "…"]     # the wilderness prefix for the touch cases
```

- `replay`: the routes in `golden/*.scn` are recorded once (`replay.py`: input packets, per-tick state, the save) and replayed on later builds; the first divergent tick is reported (`tickdiff.py`), and the save is compared with `savecmp.py`. A negative control (dropped input) must diverge.
- `shots`: the page specs in `shotgate_pages/` (trigger, key script, environment, deterministic clock) are shot with `hostshot.sh` and compared with the kept image by `shotgate.py`. The criteria and the page spec format are in [screenshot_gate.md](screenshot_gate.md).
- Baselines live in `$DM_ROOT/data/switch/golden/` with a `LOG.tsv` of every recording and its reason.

The record formats have their own pages: [tick_state_format.md](tick_state_format.md), [command_packet_format.md](command_packet_format.md), [ui_message_stream.md](ui_message_stream.md). The event stream that test scripts read for milestones, saves, red markers and crashes (`DM_EVENTS`) is in [event_stream.md](event_stream.md), and the control port that drives a running game step by step (`DM_CTL_PORT`) in [control_port.md](control_port.md); scenarios built on it are in [scenarios/](scenarios/README.md), and fixed-route benchmarks in [bench/](bench/README.md). Crash records, game-program symbols and the other debugging tools are in [debugging.md](debugging.md).

## Touch feature test cases: `touchcases/` and `touchchain.sh`

Each case is a pair of files under `touchcases/<name>/`:

- `script.txt`: the touch-point script (at which moment, which finger, pressed or lifted where), handed to the host's touch front end via `DM_TOUCH_SCRIPT`. A `<seconds> players <N>` line instead requests player setting N through the game's own `/players` path.
- `assert.py`: the assertions. It reads the four records the host writes in the run directory — per-tick state `ticks.jsonl`, command packets `c2s.jsonl`, UI messages `uimsg.jsonl`, state probes `state.jsonl` — and judges whether the touch produced the result it should (an item moved, a command packet was sent, a panel opened). The common functions are in `assertlib.py` (cases still `import lib`, a shim in `touchcases/`), tested by `touchcases/test_lib.py` and `test_assertlib.py`; it also has the log red-marker guard (`python3 dm/verify/assertlib.py logred <log> [--allow regex]`), the sanitizer-report scan (`sanitizers <log>`) and the Metal validation summary (`metal <log> [--lint]`; [debugging.md](debugging.md)).

Optional files: `prefix` (one line, the path of a prefix directory: a recorded segment of input replayed first, taking the character somewhere such as the wilderness; `golden.py prefix` makes it), `ticks` (one line, the total tick count for this case), `pending` (a case with this file is listed but not run; the first line gives the reason).

```
dm/verify/touchchain.sh <case name>
```

- Cases run on the same UI overlay as the iOS package (`dm/ios/tools/gen_romfs_overlay.py` generates it from the game resources), so what is tested is the layout the player sees.
- The criteria are all logical results, so the run does not follow the product's pacing: rendering is not rate-limited, the game clock is fast-forwarded in fixed steps, and the touch-point script advances by server ticks, so the result is independent of the machine's load.
- The last line is `TOUCHCHAIN <case> OK|RED` with the time taken. The run directory of a passing case is deleted; `DM_TOUCH_KEEP=1` keeps it; red ones are kept in `$DM_ROOT/run/verify/dm/touchchain/`.
- Environment variables: `DM_HOST_OUT` (build output, default `touch`), `DM_TOUCH_SAVE` (the `0/` directory of the starting save), `DM_TOUCH_PREFIX`, `DM_TOUCH_RUNROOT`, `DM_ROMFS_SRC`, `DM_TICKS_N`.

The assertions were written against the content of the standard save (which inventory cells hold items, which skills the character has), so with a different save the assertions have to change.

## Other tools

| File | Purpose |
|---|---|
| `savecmp.py` | Byte-by-byte comparison of two save directories. Checksum fields are each checked for self-consistency, the two wall-clock time fields are checked for their source, and everything else must be identical |
| `savesha.sh` | Computes the content fingerprint of a save directory |
| `d2save.py` | Parses and validates a save (every `cli` / `mac-app` scenario run checks its save with it) |
| `hostshot.sh`, `shotgate.py`, `dynmask.py` | One host screenshot; one page compared with its kept image; animation masks |
| `logred.sh` | The keyword table of host-log red lines |
| `gfxq/` | Image quality metric and visual-equivalence ruler, see [gfxq/image_quality.md](gfxq/image_quality.md) |

## Data you prepare yourself

| Data | Where scripts look for it | Used by | How to get it |
|---|---|---|---|
| Standard save | `$DM_ROOT/data/switch/save_ref_warlock/0000000000000001/` (with subdirectories such as `0/`) | Almost every check that runs the host | Your own save, or `dm/ios/tools/make_fixture.sh`. `MANIFEST.sha256` beside it lists "sha256 + two spaces + `0000000000000001/relative path`", sorted by path |
| Baselines | `$DM_ROOT/data/switch/golden/` | `golden.py`, `fork_gate.sh full` | `golden.py replay record` / `shots record` with a reason |
| Touch-case prefix | The path in the case's `prefix` file (`$DM_ROOT/data/switch/gate/prefix_wild_warlock`) | `touchchain.sh skill_main` | `golden.py prefix` |

A change to the standard save means re-recording the baselines and the prefix with it.
