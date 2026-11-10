# Debugging

Tools for finding out what went wrong in a run: crash records, symbols for the game program, the debugger, validation and sanitizer builds, and automatic exploration.

## Crashes

When the host is started with `DM_CRASH_JSON=<path>`, its crash handler (`dm/host/crash.c`) writes a structured record there before printing the usual crash lines. The scenario runner sets it to `crash.json` in the result folder. The handler uses only async-signal-safe calls, so the record is written even when the log dump hangs.

| Field | Meaning |
|---|---|
| `sig`, `code`, `addr` | Signal, `si_code`, faulting address |
| `pc`, `lr`, `sp`, `fp`, `x[0..28]` | Registers |
| `pc_where`, `lr_where` | `main+0x…` / `sdk+0x…` when inside a game module, otherwise `null` (host code) |
| `bt` | Frame-pointer backtrace, each `{pc, where}` |
| `modules` | Load range of each game module |
| `host_base` | Load address of the host executable, for `atos -l` |
| `tick`, `stage` | Last tick and stage the watchdog recorded |

After printing, every fatal signal is re-raised with the default action. The process therefore ends by that signal and the system writes its own crash report (`.ips`); a shell still sees exit code 128 + signal. macOS suppresses repeated reports of the same process, so `crash.json` is the record to rely on.

```bash
python3 dm/tools/crashlog.py show crash.json [--bin dm/host/dmhost]     # host frames via atos (file:line), game frames via the symbol table
python3 dm/tools/crashlog.py collect --process dmhost --since <epoch>  # copy and summarize the system .ips reports
```

When a scenario's process crashes, the runner symbolicates the record into `crash.txt`, collects any `.ips`, and the verdict names the crash (`crash SIGSEGV at cmd_crash (dm_ctl_cmds.c:280)`). The control port's `crash kind=segv|abort|trap` command crashes the host on purpose, for testing this path.

## Symbols for the game program

```bash
python3 dm/tools/guestsyms.py build                       # → $DM_ROOT/build/cache/dm/symbols/<product>.tsv (local only, never committed)
python3 dm/tools/guestsyms.py lookup main+0x455aa4 sdk+0x2b0470
```

- **Function boundaries** come from each module's `.eh_frame` (about 57,000 functions in `main`, 29,000 in `sdk`).
- **Names** come from the dynamic symbol table, demangled; `sdk` exports most of them.
- **Notes** come from the source comments that mention `module+0x…`. For example, `main+0x455aa4` is 0x24 into `fn_455a80`, the function the comments describe as building the health-bar info.

`crashlog.py` and anything else that prints `module+offset` can use this table.

- **Engine names** come from the engine-analysis database when it exists ([dm/tools/re](../tools/re/README.md)): a private Ghidra project of `main` with a local query server (`re.sh serve`, `re.sh q decompile main+0x…`). Its export `$DM_ROOT/re/export/main.names.tsv` names the functions the dynamic symbols leave unnamed, in this table's lookups, `crashlog.py`, `dm bt` and `sampagg.py`. Its output stays on your machine.

## Probes

`DM_PROBE=<module>:<offset>[@<hex>|@*<hex>][,…]` (Mac only, up to 16; every number is hex, `0x` optional) replaces one game instruction with a trap whose handler logs and then runs the original instruction from a trampoline, so put it at a function entry or where x16 is dead, never on a PC-relative instruction. Each hit logs `PROBE <module>+<offset> #<n>` with x0..x3 (and the strings they point at), x19..x23, x8 and a backtrace; `@<hex>` adds the 16 bytes at x0+hex, `@*<hex>` the 16 bytes at [x0+hex]. A probe may carry one filter after its offset, `?<source><op><value>`. The source is a register `x<r>`, or `x<r>+<hex>.<b|h|w|q>` for the 1, 2, 4 or 8 bytes at that register plus an offset. The operator is `=` (equal to a hex value or `main+<hex>`), `&` (any bit of a hex mask set), `~` (the C string the register points at contains the text) or `%` (no value: count every value the source takes; not `#`, which starts a comment in scenario files). Only matching hits are printed; the summary line adds `match=<m>`, so `match=0` answers "never with that value" for every hit, not only the printed ones, and a `%` probe prints `values=<value>:<count>,…` (up to 16 values, then `other`). Run a positive control (a value that does occur) before trusting `match=0`. Put the filter after `@`. Printed hits carry the thread's Mach id (`tid=`). `DM_PROBE_MAX=<n>` prints only the first n hits of each probe in full and counts the rest, for probes on paths that run every frame. `DM_PROBE_AFTER=<s>` prints nothing from the first s seconds (still counted), to skip boot and the front end. `DM_PROBE_DUMP=<module>:<offset>:<length>[:file]` (hex as well: `main:2d64a18:100` is 256 bytes) writes a range of guest memory to a file on the first hit of any probe.

When the host ends through its own exit paths it prints one `PROBE_SUMMARY <name> hits=<n>` line per installed probe, zero hits included, then `PROBE_SUMMARY end probes=<n> why=<exit|shutdown|shutdown_timeout|watchdog|signal|ticks_full|game_break|memory_cap>`. Ctrl-C (SIGINT), SIGKILL and `DM_TEST_KILL_AT` end the process without one: stop a hand-started probe run with `kill <pid>` (SIGTERM, a graceful shutdown) or the control port's `quit`. A claim that a path is *not* taken needs all three: the install line (`探针 <name>`), a run that ended the way the scenario expects (`why`), and `hits=0`. Without them the answer is unknown, not "never".

## Live hooks

`DM_LIVE=1` (Mac development runs only, never a released app) lets the control port's `live` command put hooks on the game's code while it runs (`dm/host/dm_live.c`; the protocol in [control_port.md](control_port.md)). `DM_LIVE_HOOKS="<spec>;<spec>…"` (or `@<file>`, one spec a line) arms them at boot instead, before the game's threads start (the spec is the `add` arguments as `name=value` words: `at=main+0x16926d0 kind=ret cap=x2`); the log names the first 50 armed and refused, then `live hooks: N armed at boot, M refused`.

- **How:** the hooked instruction becomes a branch to a trampoline that saves every register, records the hit into the calling thread's ring, runs the displaced instruction (relocated when it is PC-relative: adr, adrp, b, bl, b.cond, cbz, tbz, ldr literal) and branches back. A collector thread writes the records to `DM_LIVE_OUT` (default `live.bin`) and lists the hooks in `<out>.hooks.tsv`; `python3 dm/tools/re/live.py summary <out>` gives per hook the records, threads, call rate, callers (named through `guestsyms.py`), return durations and each capture's most frequent values; `dump` lists the records.
- **Patching** goes through a second, writable mapping of the code: the running pages never change protection, so other threads keep running. The trampolines sit in an arena the loader reserves right after `main` in `DM_LIVE=1` runs, so every one is in branch range.
- **Kinds:** `entry` and `insn` record the registers at that instruction; `ret`, at a function's first instruction only, also records the return value and the call's duration (the return address goes through a shared return trampoline; a per-thread shadow stack matched by the stack pointer brings it back).
- **Cost:** a hit is a few ns (about 4 ns in a tight loop, against 1.8 µs for a `DM_PROBE` trap); a capture that reads memory adds a `vm_read` (a bad pointer reads 0, never a crash). `max`, `every` and `rate` bound a hook: over `rate` hits a second it disarms itself and the log says so.
  - The worst case, measured: the 20 hottest game sites (`$DM_ROOT/re/notes/hot.tsv`) recording every hit, about 500,000 records a second (1.8 GB a 35 s run), `bench.py ab town_loop` at this Mac's Recommended, 6 + 6 ABBA, 2026-11-09. Process CPU per frame +0.66 ms in town / +1.14 ms in the Blood Moor (+3% / +5%, p 0.002); main thread +0.35 / +0.45 ms; frame time p50 unchanged (33.36 ms); slow frames not more.
- **It doesn't change the game:** `golden.py replay check` with 23 hooks armed at boot (those 20 sites, 4 of them reading memory, and 3 return hooks) gave every route tick-identical with byte-identical saves, and each negative control diverged (2026-11-09). The same check with every function of `main` hooked from boot (the call-trace list, 57,027 count-only hooks) was identical too.
- **Refused:** an instruction the rewriter turned into a branch into the patch area (gates, observation gates), an exclusive load or store or an instruction between the two (the store would fail every time through a hook), a `brk`, a floating-point literal load, and a second hook on an instruction that has one armed. A site armed again after `del` reuses its hook number and trampoline, with fresh counters.
- **Limits to know:** the trampoline keeps every general and vector register and the flags, not the floating-point status flags. A `ret` hook puts a trampoline's address where the return address was until the function returns: a backtrace taken inside shows it, and a C++ exception thrown through such a frame can't unwind. Replacing an instruction that another core is executing at that instant relies on the processor running the old or the new one whole, which Apple's cores do (2,000 patches under three calling threads, and the runs below); it is why this stays a development tool.
- **Call-trace mode** (which functions run, how often): `live.py trace-spec --out F` writes one count-only entry hook (`count=1`) per function of `main` (57,045; `--only main+0x…-main+0x…` for a range, `--records N` to also record the first N calls of each, callers included). `live do=load file=F` arms them on the running game (57,027 armed in 0.09 s; 18 refused, rewritten entries), `live do=counts file=counts.tsv` writes every hook's counters beside the output, and `live.py counts` names the functions that ran. In the Den of Evil, standing: 2,400 functions ran, 19.8 million calls in 10 s.
- **Block coverage** (which code runs during an action): `live.py blocks --out F` lists every basic block's first instruction (1,383,429 in `main`; needs the venv's capstone). `live do=cover file=F` puts a one-shot trap on each (0.03 s): the first time a block runs, the handler puts the instruction back and notes the order, so a block costs one trap ever. `cover_dump file=…` writes the blocks that ran, `cover_rearm` traps those again for a second pass, `cover_off` puts everything back. `live.py cover` lists functions by blocks run, and `live.py cover-diff a b` the blocks that ran only in the second pass: standing then walking in the Den gave 33,917 blocks in 2,279 functions, then 4,440 new ones (footprints, the terrain tile window).
- **Watchpoints** (who writes or reads a field): `live do=watch at=main+0x… [chain=<off>,…] [len=1|2|4|8] [mode=write|read|rw] [max=N]` (the address as the `mem` command's) sets one of the processor's 4 data watchpoints on every thread, new game threads included, with no debugger. Each access is recorded once it has happened: the instruction, the address, the value now there and whether it was a write (`live.py summary` lists them by address). After `max` hits (20,000) a watch turns itself off, since a field read at every draw would stall the game; `live do=stats` shows each watch's hits. `live do=unwatch [watch=N]` removes it. An access from the kernel (a `read` into the watched bytes) is not seen. Run a positive control in the same slot range before trusting zero hits (a field known to be written every frame).
- **Text:** `cap=str:<source>` records up to 32 bytes of the text a register points at (a package name, a path) in place of the four values, and `filt=<source>~<text>` keeps only the hits whose text contains it, as `DM_PROBE`'s `~` does.
- **A check list in one run:** name each hook after its question (`name=D47_SetPipeline`), dump `live do=counts file=counts_<phase>.tsv` at each phase and re-arm the list (a re-armed hook's counters start over); `live.py table` then prints one line per hook with its hits per phase, callers and values.
- **Runs, reports and the write-back:** `live.py run <scenario>` keeps a run in `$DM_ROOT/re/live/runs`; `report`, `edges`, `view` and `re.sh ingest` join it with the map, and the MCP server `dm-live` offers the same to a Claude Code session with a game kept alive between calls (`dm/tools/re/README.md`, "Live runs"). `type=x0:Unit` in a hook's spec names what a register points at for the reports.
- **Tests:** `dm/host/tests/t_live.c` (every relocation case, flags and floating-point registers kept, captures, filters, budgets, return values, four threads, refusals, disarm, re-arming, count-only hooks, block coverage in two passes, a watchpoint's five writes) and `dm/verify/scenarios/live_hooks.scn` on the CLI host.

## The debugger

### Stopping at a tick

`DM_BREAK_AT_TICK=<tick>[:<wait seconds>]` (default wait 300 s) stops the game's tick thread at the entry of that server tick:

1. The host logs the pid and emits a `break` event (`tick`, `pid`, `wait_s`; [event_stream.md](event_stream.md)).
2. The tick thread waits for a debugger to attach (game ticks are frozen meanwhile).
3. Once one is attached, it executes `__builtin_debugtrap()`, so the debugger stops on the tick thread at the tick entry. After `continue` or `process detach` the game carries on.
4. If no debugger arrives within the wait, the game carries on by itself.

The watchdogs (`DM_WATCH`, `DM_WATCH_STALL`) count the wait and the time stopped in the debugger as no progress. For a debugging session, start the host by hand (the CLI host command in `CLAUDE.md`) with neither set. The scenario runner always sets `DM_WATCH` and arms the 15 s stall limit, so a stop under it must be short.

```bash
DM_BREAK_AT_TICK=40 DM_ROMFS=… DM_SAVE=… ./dmhost "$(../tools/product.sh path touch)" main sdk
lldb -p <pid from the log>
(lldb) command script import dm/tools/lldb/dm.py
(lldb) continue          # stops at the trap on the tick thread
```

In `lldb --batch`, a stop at the trap counts as a crash-like stop, so commands after it must be given with `-k` (run on stop), not `-o`.

### LLDB commands for game addresses

`dm/tools/lldb/dm.py` adds a `dm` command that reads the host's module table and the guest symbol table above:

| Command | Does |
|---|---|
| `dm modules` | Load range of each game module (`g_mod`) |
| `dm where <address expression>` | `main+0x…` / `sdk+0x…` plus the guest function, or the host symbol |
| `dm bt [-a]` | Backtrace of the current thread (all threads with `-a`), every frame as module offset and guest function, with source-comment notes |
| `dm break <module>+0x<offset>` | Breakpoint at a game address, converted with the current load base |
| `dm tick` | Current server tick (calls `dm_cur_tick`) |
| `dm ui [-a]` | The UI root's panels: name, visible / active, priority (`-a` also the hidden ones) |
| `dm focus` | The focus manager: hovered widget, the focus's panel, focused widget; cursor mode and state |
| `dm saves` | Save busy / commit pending, and each save container's mount state and dirty flag |
| `dm unit <id>` | A server unit by id (item table, then monster table): address, type, row, mode |

The same game state is readable without a debugger: the control port's `state` / `engine` commands (`dm/host/dm_engine.c`), and `engine.json` in a scenario's run folder after a red event.

### Agents: lldb-mcp

Xcode's LLDB can serve the Model Context Protocol, so an agent can drive the same session:

1. In the lldb session: `(lldb) protocol-server start MCP`. `protocol-server get MCP` shows the listener.
2. In the agent's MCP configuration, the server command is `xcrun lldb-mcp` (stdio). For Claude Code: `claude mcp add lldb -- xcrun lldb-mcp`.
3. Its tools are `command` (runs one lldb command, for example `dm bt`) and `debugger_list`.

Only one lldb may serve MCP at a time: with more, `lldb-mcp` stops with "too many MCP servers running". An lldb that was killed leaves its `~/.lldb/lldb-mcp-<pid>.json` behind; delete stale ones.

`lldb-mcp` looks for servers only when it starts. If none is running then, it starts its own lldb (`lldb -O 'protocol start MCP'`), which keeps running after the agent exits, is reused by the next one and counts as the one server. With Xcode 27.1 that lldb starts with its standard input and output closed: it can load a target, but every `process attach` fails with "Bad file descriptor". Either start your own lldb with the server (step 1) before the agent starts, or point `LLDB_EXE_PATH` at a wrapper that gives the lldb `lldb-mcp` starts a standard input that never closes:

```bash
#!/bin/bash
fifo=$(mktemp -u); mkfifo "$fifo"; exec 3<>"$fifo"; rm -f "$fifo"
exec "$(xcrun --find lldb)" "$@" <&3 3<&- >>"${TMPDIR:-/tmp}/lldb-mcp-backend.log" 2>&1
```

Register it with `claude mcp add lldb -e LLDB_EXE_PATH=/path/to/wrapper -- xcrun lldb-mcp`. The agent then attaches with `process attach -p <pid>` and loads the commands above with `command script import <full path>/dm/tools/lldb/dm.py`. Start the program yourself and attach: `process launch` through `lldb-mcp` never returns, with or without the wrapper, and blocks every later command until that lldb is killed. To serve from your own lldb again, first stop that one: `pkill -f 'lldb -O protocol start MCP'`.

## Sanitizer builds

```bash
make -C dm/host dmhost_asan     # AddressSanitizer + UndefinedBehaviorSanitizer
make -C dm/host dmhost_ubsan    # UndefinedBehaviorSanitizer only
make -C dm/host dmhost_tsan     # ThreadSanitizer
SCN_DMHOST=$PWD/dm/host/dmhost_asan python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn
```

- Only host code is instrumented; the game program is not. The targets don't track headers, so remove the binary to force a rebuild after a header change.
- Every scenario run scans its logs for sanitizer reports. Any report makes the run red, and they are summarized, deduplicated by summary line, in `sanitizer.txt` in the result folder. From the shell: `python3 dm/verify/assertlib.py sanitizers <log>`.
- The options each sanitizer needs are built into the binaries (`main.c`), so no environment is required.

What the builds adapt to, and why:

| Build | Adaptation | Reason |
|---|---|---|
| ASan | Stack frames are not instrumented (`-mllvm -asan-stack=0`); heap and globals are | Host code runs on stacks the game provides. Redzones make frames larger, and stale redzone marks are falsely reported when the game reuses that memory. |
| ASan | Registration tables (`DM_SECT`) get no redzones | The host reads each table section as a packed array. |
| ASan | `use_sigaltstack=0:detect_leaks=0` | The host sets its own per-thread alternate signal stack (`crash.c`); ASan would `munmap` that malloc'd stack at thread exit. |
| ASan, TSan | Game threads run on an 8 MiB host stack instead of the game's buffer (`hle_thread.c`) | ASan's free path and TSan's report symbolization run on the current stack and overflowed the game's small thread stacks (some are a dozen KiB). |
| TSan | Guest memory (the address-space reservation and the module images) is registered as a benign-race range (`DM_TSAN_GUEST`) | It is ordered by the game's own synchronization, which TSan cannot see. |
| TSan | Built-in suppressions (`__tsan_default_suppressions` in `main.c`) | Reviewed host-side cases: bookkeeping counters, the state dumps that read without locks on purpose, the fd tables (ordered by `close`/`open`), and single-writer values (`g_fence`, `g_done`, `g_link`, `g_lastG`, `g_layout_logged`). New races still report. |

ASan prints "ignoring requested `__asan_handle_no_return`" once per run: the guest threads' stacks are not the pthread stacks ASan knows. It is harmless.

What they found (fixed):

- UBSan: `nvn_mtl.m` tested `lmap[j] >= 0` on a `uint8_t` map, where 0xff means "no mapping". The test was always true, so clear colors were read from slot 255 of an 8-entry table. It now tests `< 8`.
- UBSan: misaligned 8-byte relocation stores in `loader.c`, a misaligned 2-byte read in `dm_vault.c`, and misaligned reads of the packed sprite extents in `dm_world.c` (found by random play on the UBSan host, below). All now go through `memcpy`.

`inventory`, `pause_menu`, `save_while_paused`, `enter_game` and `bench/town_loop` run clean under all three builds. `enter_game` takes about 4 s on the normal and ASan hosts, and about 23 s on the TSan host.

## Metal validation

```bash
python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn --metal-validation api
python3 dm/tools/scenario.py run dm/verify/bench/town_loop.scn --headless --metal-validation shader
```

| Level | Turns on | Verdict |
|---|---|---|
| `api` | API validation (`MTL_DEBUG_LAYER=1`); errors are logged instead of asserting, warnings are off | Any message is red |
| `lint` | `api` plus the warnings (`MTL_DEBUG_LAYER_WARNING_MODE=nslog`): redundant or unused state settings | Summary only |
| `shader` | Shader validation, reported to stderr; the stall limit is raised to 300 s, because every pipeline is instrumented and recompiled (about 0.15 s each) | Any message is red |
| `all` | `api` + `shader` | Any message is red |

The messages are summarized in `metal_validation.txt`; from the shell, `python3 dm/verify/assertlib.py metal <log> [--lint]`. The flag works with the `cli` and `mac-app` backends. The iOS backends start their process differently and don't pass these variables.

Findings:

- With shader validation on, a windowed run crashes at its first present commit inside Metal's own `-[MTL4GPUDebugCommandBuffer applyResidencySets:]` (macOS 27, still on 27.0.1; moving the layer's residency set off the queue does not help). Run shader validation headless (`--headless`): there is no present step, and all of the game's rendering is still validated. The fork is Metal 4 only, so the older-API host is no longer an option. A windowed run also reports MetalFX's own scaler shader (`MFX_SCALE_SHADER_V1_FRAG`) reading a non-resident 4-byte internal buffer; MetalFX exposes no residency hook for it.
- `api` and `shader` runs of the scenarios above report nothing.
- `lint`: about 46,600 redundant `setComputePipelineState` calls in a 12 s `enter_game`. They come from the texture deswizzle encoder (`dm/gfx/tex/nvn_deswizzle_gpu.m`), which sets the pipeline once per level and layer on the same encoder. This is cheap (a few ms over a load) and has not been changed.

## Automatic exploration

`dm/tools/explore.py` plays seeded random sequences of player actions through the scenario runner and judges each run by the scenario criteria: crash, red events, red log lines, sanitizer reports and the watchdog.

```bash
python3 dm/tools/explore.py run --seed 21 --episodes 3 --steps 40 --dmhost dm/host/dmhost_asan
python3 dm/tools/explore.py gen --seed 21 --steps 40      # print one episode's steps
```

- **Actions:** `press` (any button), `stick`, `tap`, `world_tap`, `sleep`. All are plain steps without assertions, so a red run means the game or host went wrong, not the script. Steps that end or interrupt the run on purpose (`quit`, `crash`, `app`, `background`) are never generated.
- **On a red run:** it is rerun once to see whether it reproduces. Then delta debugging shrinks the sequence to the shortest one that still goes red with the same fingerprint: the crash site, the red event code, or the red line or sanitizer summary with numbers blanked out.
- **Output:** the result is `minimal_<k>.scn`, a scenario that replays directly with `scenario.py run`. Everything is in `$DM_ROOT/run/explore/<time>_<seed>/`, with `EXPLORE OK|RED` and a final `RESULT {json}`.
- **Options:** `--budget` caps the shrinking runs (default 24), `--allow-red` tolerates known red codes, and `--no-shrink` skips shrinking.

First result: on the UBSan host, three episodes of seed 21 all went red with misaligned reads in `dm_world.c`. Each shrank to one step (`world_tap` somewhere) in 7–11 tries. After the fix, the same seed runs clean.

## One GPU frame, captured and replayed

`DM_NVN_CAPTURE=<frame>:1:<file>` writes one frame at the replay boundary: the pools, the texture and sampler registries, the draw state the frame starts from, the textures whose content lives only in Metal, each submission's records (pointers relocated), the host settings each submit was drawn with (the latched drawing switches), and the presented frame's digest and pixels. `dm/gfx/tests/nvn_replay` replays it against the current replay layer, in a process of its own, and compares. Use it to check a change to `nvn_mtl.m` on a fixed frame without running the game, or to bisect a rendering difference to one frame.

```bash
DM_WATCH_STALL=300 DM_NVN_SYNC=1 DM_NVN_CAPTURE=800:1:$DM_ROOT/run/capture/town800.nvncap \
  python3 dm/tools/scenario.py run dm/verify/bench/town_loop.scn --backend cli --headless   # also writes town800.nvncap.live.png
make -C dm/gfx/tests nvn_replay
dm/gfx/tests/nvn_replay $DM_ROOT/run/capture/town800.nvncap --repeat 3 --max-pixels 64     # MATCH / 【红】MISMATCH; --png, --list
DM_NVN_TEXDUMP=<frame>:<dir>/rep_%d.png dm/gfx/tests/nvn_replay …                         # every colour target of the replayed frame, to compare with a live dump
```

- `<frame>` counts presents from 1. `DM_NVN_SYNC=1` keeps the game from writing the next frame into the pools while this one is read; `DM_WATCH_STALL=300` because the first submit writes every pool (about 1.8 GB raw, 320 MB with LZ4, about 1 s in town) while the game waits.
- A town frame (CLI host, original look) replays within about 30 of 1.44 million pixels (largest difference 9 of 255). Fresh replays differ from each other by the same amount: the frame itself is not bit-exact on the GPU (order-dependent float accumulation), so compare with `--max-pixels`.
- The replayer refuses a capture whose record layout (`nvn_obj.h`) or draw state (`nvn_mtl.m` `S`) changed, and stops when a program's translated shaders take other bindings than at capture. Captures hold game memory: they stay in `$DM_ROOT`, and the pre-commit guard refuses `*.nvncap`.
- Debug a mismatch with `DM_NVN_PASSDUMP=<mtl_frame>` and `DM_NVN_TEXDUMP` on both sides (the capture's header names `mtl_frame`): the first render target that differs points at the missing input.

### The rays, byte for byte

A plain replay never traces rays: in a test build the host's ray tracing switch answers off. `--rays` turns them on at Full, `--frames n` replays the frame's submits as n consecutive frames (the rays bring in at most 24 new structures a frame, so give them about 600), `--planes <file>` writes the five ray planes after the last frame, and `--planes-ref <file>` compares them with another run's, plane by plane, byte for byte: then the planes decide the exit status, not the picture.

```bash
dm/gfx/tests/nvn_replay town.nvncap --rays --frames 600 --planes a.bin
dm/gfx/tests/nvn_replay town.nvncap --rays --frames 600 --planes b.bin --planes-ref a.bin       # PLANES MATCH / 【红】PLANES DIFFER
dm/verify/rays_replay.sh record --reason "<why>"     # keeps a town capture and its planes in $DM_ROOT/data/switch/golden/rays
dm/verify/rays_replay.sh check                       # this tree's replay layer against them, with a control (gate step full_rays_replay)
```

The check's control replays with a larger sun (`DM_RT_SUN=0.5`): the key plane must change and no other, or a match proves nothing. (Occlusion off isn't a control: it also changes that plane's layout.) A change meant to alter what the rays draw records again, with its reason (`LOG.tsv` beside the capture). The planes are what the kernels wrote; how the forward shaders use them is judged by the picture sets. `DM_RT=2` with `--rays` traces in the rays-alone mode. After about 600 frames the replay's scene holds the live run's objects (town: 165, about 86,000 triangles).

Open (2026-11-07): in a replay the rays don't reach the picture. The forward pass's output and the presented frame are byte-identical with rays off, on, rays-alone or occlusion off, although the planes are computed and the forward shaders carry the hooks. In the live game they do reach it. Until that is understood, judge the rays' look on the live game, not on a replayed picture.

## What the terrain material arrays hold

The game keeps ground materials, terrain decals and terrain stamp masks in three texture arrays (BC1 colour, BC3 normals, BC4 single channels), filled by copying each source level to the array level with the same index. The terrain composite reads them only at level 0.

`DM_NVN_ARRDUMP=<frame>:<dir>` writes every compressed array a copy has filled by that frame, all levels and layers as raw blocks, one file per array. `<frame>` counts presents from 1; a headless run shows about 27 a second.

```bash
python3 dm/tools/scenario.py run dm/verify/scenarios/enter_game.scn --backend cli --headless --env "DM_NVN_ARRDUMP=800:$DM_ROOT/run/arr"
python3 dm/tools/arrdump.py list $DM_ROOT/run/arr
python3 dm/tools/arrdump.py nonzero <file> <layer> [--level N]        # how much of a layer holds data, and where
python3 dm/tools/arrdump.py sheet <file> out.png [--layers a,b-c] [--level N] [--cell 128]
```

To find which layer a place uses, read the per-tile records through the control port (`mem`, at most 4096 bytes a read): terrain decals at `main+0x3208ea0`, 196 records of 0x70 bytes; terrain stamps at `main+0x31fd6e0`, 196 tiles of 0xf0 bytes, a count and then up to two records of 0x70. A place with no terrain decals, such as the Outer Cloister, reads all zero there: run a place that has them, such as the Barracks, as a control. The dumps hold your own game's assets, so keep them and pictures made from them out of the repository, and delete them when done.

## Replay and the first divergent tick

`dm/tools/replay.py` records a scenario run's client command packets and per-tick state (`DM_C2S_LOG`, `DM_TICKS_FULL`), then replays the packets on any host build (`DM_C2S_REPLAY`) and compares tick by tick. This answers whether a change or a build changes the game logic.

```bash
python3 dm/tools/replay.py record dm/verify/bench/town_loop.scn                  # → $DM_ROOT/run/replay/town_loop_4242/
python3 dm/tools/replay.py check ~/dm-work/run/replay/town_loop_4242 --dmhost dm/host/dmhost --dmhost dm/host/dmhost_asan
python3 dm/tools/tickdiff.py a/ticks.jsonl b/ticks.jsonl                          # any two DM_TICKS records
python3 dm/tools/c2s.py summary ~/dm-work/run/replay/town_loop_4242               # packets per kind, labelled, lengths checked
python3 dm/tools/c2s.py near ~/dm-work/run/replay/town_loop_4242 300 --span 5     # the packets in ticks 295-300
```

`tickdiff.py` reports the first tick where the two records differ and the state slots that differ (player, monster, object, missile, item, seed). With per-unit rows on both sides, it also names the units and fields: `player id 1 cls 1: mode 3→5, yf 65437→32768`. It also replaces the comparator `host_savecmp.sh` used to call, which was missing from the repository. When `replay.py check` finds a divergence it also lists the recorded packets of the five ticks before it, labelled from `dm/verify/c2s_opcodes.tsv` ("run to a map location" rather than `0x03`). `c2s.py summary` counts a recording's packets by kind and turns red when a fixed-length packet has another length: each handler in the game checks its own exact length, and the table holds those lengths.

What determinism needs, and what the tool handles:

- **Fixed seed and time:** `DM_GAME_SEED` and `DM_FIXED_TIME`. `record` uses the scenario's own values when it sets them; `town_loop` does, because its coordinates depend on them.
- **The same entry and spawn:** the replay enters through the same path as the `enter_game` macro. The host moves the town spawn next to the waypoint (`dm_townwp.c`), but by default not during a replay, because original-game recordings walk from the original spawn. The replay therefore sets `DM_TOWNWP_OFF=0` to keep the move. Without it, the replay starts about 40 cells away, every movement packet misses, and replays agree with each other only because nothing happens.
- **No join handshake in the file:** the tick-0 system messages are left out. Replay drops the client's own messages between the file's first and last ticks, so a first tick of 0 would also drop the host client's own join, and the server would wait forever.

Measured:

- **Builds agree:** `town_loop` (781 ticks, 144 command packets, camp and Blood Moor) replays identically to the recording on the normal and ASan hosts.
- **Changes are caught:** removing the walk commands of ticks 405–500 gives a first divergent tick of 444: the player stops where the route already issued runs out.

Recordings are derived from the game program and stay local.

## Motion and animation

A still picture cannot show a frozen walk or a frame out of order, and a video only shows them to someone watching. These give numbers:

- `DM_FRAME_REC=<first s>:<seconds>:<prefix>[:x,y,w,h]` writes every frame the screen is given, in order, as `<prefix>.raw` and `<prefix>.tsv` (game frame, generated or real, planned time). It copies in the present's own command buffer, never stalls the game and needs no screen-recording permission; when the writer falls behind it counts a drop instead of waiting. A whole 2560 x 1440 picture is about 885 MB/s.
- `motion_scan.py` also gives `detail`: how sharp a generated frame is against its real neighbours while the picture moves. `nvn_fg.inc`'s sharpen, which follows each pixel's motion, keeps it near 1. Without it a run reads about 0.76, and with the camera still about 0.98, which is why a fixed sharpen is wrong. Apple's interpolator mostly finds motion itself: generated frames kept 0.63 of a real frame's edges with the motion vectors zeroed and 0.69 with them, so better motion data can't fix doubled limbs.
- `DM_MV_TRACE=<file>` writes one line per drawn frame about the hero's limbs, from the velocity target: the camera's motion, how many pixels move apart from the ground, and how fast the limbs go. The camera follows the hero, so whatever moves in her area is her animation. It needs the temporal scaler (`DM_MFXT=1`), which draws the velocity target.
- `dm/tools/motion_scan.py <prefix | video> [--trace file]` judges them: shown-frame cadence, repeated frames, picture steps that go backwards, whether generated frames sit half way, and the longest run of frames with a frozen pose. `--clip` and `--sheet` make a video and a contact sheet. Its detectors are pinned by `dm/verify/test_motion_scan.py`.
- `motion_scan.py --events <run's events.ndjson>` gives input to picture: from each mark named `inN` to the first shown frame that moves, on the time the system planned to show it. A trial is a mark, a stick push and a walk from standing; judge the mean over about 25 trials. On an M6 in town: 96 ms with frame generation, 73.5 ms at real 60, Temporal or not (two runs each).
- `motion_scan.solid_scan(<dump prefix>)` (a `DM_MV_DUMP` with `DM_MV_DUMP_ONLY=real,comp,mv`) says whether what moves stays whole in generated frames, by comparing pixels with where the real frames say they belong. `lost` is the share of the thin parts of moving things (a staff, a limb's edge) whose real colour isn't half way along their motion: 6-16% with the moving things put back, 20-59% with `DM_FG_FORE=0`. `pale` is the share of the ground in a ring around them that matches neither real frame half a camera step away. Edge energy and the whole figure couldn't tell the runs apart: thick parts hold either way.
- `dm/verify/motion_gate.py` (gate step `core_motion`) runs them on a walk with frame generation, then with `DM_FG_FORE=0` and on the fixed clock as controls: the first must read lower, the second as frozen.

Traps:

- **The fixed clock used to freeze poses.** `DM_DETCLOCK` aligned the guest clock to 2^52 ns at ToGame, 52 days after the game's start, where its animation clock (32-bit seconds, measured from the game's start) moves only in half-second steps: each pose held for half a second, then jumped. It now aligns to 2^40 ns (18 minutes) and counts from the process's start instead of the Mac's boot (on the open-ground walk under frame generation with a 33 ms step: 1 still frame of 480 and the same limb motion as the real clock, against 354 still frames, 33 in a row; replays tick-identical). `DM_DETCLOCK_ALIGN_NS=<ns>` brings the old value back for the gate's control. A long uptime isn't this bug: `guest_ns` counts from the Mac's boot in normal runs, and a simulated 52 days showed no frozen frame, because the game measures from its own start.
- Recording the whole picture adds an occasional late frame (2 to 4 in 18 s, up to 83 ms; none in the same runs without the recorder). To judge pacing use a rectangle, as the gate does, or no recorder and `DM_FRAMES`.
- Without `DM_DETCLOCK_PACE=1` the fixed clock outruns the display and shown frames are skipped, so a recording of it isn't complete.
- Frames in a scenario's `stick` step are drawn frames: under frame generation (30 drawn per second) the same step lasts twice as long.
- A scenario `--out` folder holds that run's save: reusing it gives a false red about uncommitted save changes, or a stalled start.
- **Judge motion while the hero really moves through the place.** Against a wall her legs run but the camera stands still, and ghosting on thin things doesn't show (`scenarios/picture/motion_walk.scn` goes back and forth on open ground). `DM_TEST_GOTO_LEVEL` plays the game's bright doorway animation first: that is not a bug.
- **Read steps and sharpness on the ground, away from the hero.** She stays put on the screen, so a picture with her in it reads as either motion (`motion_gate.py` records a piece of the ground).

## Ray tracing

The Mac's Ray Tracing row (`DM_RT=0/1/2` forces it) draws everything below in compute before the forward pass, into planes behind the mask in the support buffer that the forward shaders read (`dm/gfx/nvn/nvn_rt.inc`, hooks in `dmsh_rules.py`). All of them run at half the picture each way, then a depth-aware box over the 4 x 4 pattern of the rays. Each can be turned off alone, which is how to find what a look or a cost comes from:

- Key light: the darker of the rays and the game's map (`DM_RT=2` the rays alone, `DM_RT=3` none). `DM_RT_KEY_HALF=0` traces the whole picture, `DM_RT_SUN=<tangent>` the size of the sun, `DM_RT_BIAS=<world units>` moves the ray's start.
- Ambient occlusion: `DM_RT_AO=<rays>` (1, 0 = none), `DM_RT_AO_R=<reach>`, `DM_RT_AO_P=<power>`, `DM_RT_AO_HALF=0`. The key and local lights divide it back out, so it stays on the ambient terms.
- Local lights, the first four of each tile: `DM_RT_LOCAL=0`, `DM_RT_LOCAL_SRC=<flame size>`, `DM_RT_LOCAL_STOP=<how far short of the light a ray stops>`, `DM_RT_LOCAL_FILL=<what a blocked ray leaves>`.
- Characters, along the axis of the game's layer 1, hitting characters only: `DM_RT_CHAR=0`, `DM_RT_CHAR_SUN=<tangent>`. With them on, the game's layer 1 draws only what can't be in the structures (hair, cloaks).
- Surfaces' specular response to their surroundings, which the Switch's forward shaders leave out and the PC's have (`rt_env_hook`): `DM_RT_REFL=0`, `DM_RT_REFL_REACH=<world units>`, `DM_RT_REFL_K=<times the physical strength>`, `DM_RT_REFL_SEEN=<how lit a surface must be to show it all>`. Mirror rays off every surface read what they hit from last frame's picture, still in the forward pass's colour target; where they hit nothing the game's image lighting cube for that environment stands in (engine slot 34, at the `iblIntensity` its artists set). Each surface takes it by its own normal, roughness and reflectance. Damp places (the table `k_wet` in `dm_video.c`, by level; `DM_RT_WET=<0..1>` forces it) also make stone smoother for the lights' own highlights: metal doesn't change, and what is very rough soaks it up. A surface in the dark stays dark, as the light radius is gameplay. `DM_RT_SHOW=1` shows roughness as bands (red under 0.2, orange under 0.4, green under 0.6, blue under 0.8, grey above), `2` normals, `3` this term alone, `4` reflectance, `5` the cube's part, `6` the rays' part. What a draw adds is scaled by its own alpha: the models the artists blend into the ground are drawn in layers, premultiplied, and at full strength they pile up to white (the foot of the ice walls in the Glacial Caves). A dielectric's response also stops at 0.3.

`DM_RT_DUMP=<frame>:<file.pgm>` writes the mask, the occlusion as `<file.pgm>.ao.pgm` and the first two local lights as `.l0.pgm`, `.l1.pgm`. `DM_RT_LIGHTLOG=1` logs how many lights the tile lists hold, by kind.

On a phone the row isn't there yet. Pin it with the switches: `DM_RT=3` traces no key light, `DM_RT_AO_HALF=0` traces the occlusion at the whole picture rather than half each way (`DM_RT_KEY_HALF=0` the same for the key light), and `DM_RT_AO_CHARS=2/1/0` lets its rays hit every character, the hero alone or none. Each moving character is rebuilt every frame, so 1 holds the cost steady in a crowd. A launch from the icon can't carry switches: for a device playtest put `K=V` lines in `Documents/dm_env.txt` (`dm/ios/Sources/main.m`); what a launch already sets wins.

- **The forward lighting works in view space.** The scene block's shadow matrices (rows 14 to 21) and the per-draw light block take view-space positions and directions; the structures are in world space. The ray's direction is the axis of the game's key-light shadow view, turned back by the camera's rotation. A direction used as is still gives shadows of the right shapes, falling the wrong way.
- **Characters have a shadow of their own.** The game draws them into layer 1 of the directional map, with an axis a few degrees off the key light's. In the key light's rays as well they get two overlapping shadows, so they carry an instance mask of their own: the occlusion rays and their own rays hit them, the key light's don't.
- **A vertex with no finite position goes in as NaN.** `w` is 0 for some vertices in a level's first frames; NaN leaves the triangle out of the structure, and an infinite position isn't allowed in a build.
- **`rtx_u` is shared with MSL.** A `uint2` is 8-aligned there, so a C struct drifts by 4 bytes: use `packed_` types. The symptom is a plane that ignores its switches.
- **A light sits inside what holds it.** Rays to a brazier's flame stop 2 units short, or it blocks its own light, and a blocked ray leaves 35%, or the warm pools go.
- **A level brings hundreds of new structures at once, and they come in at 24 a frame (`RT_NEW_PER_FRAME`).** Over a hundred in one frame stalled the GPU for good in about one level change in ten, with the game's main thread in its present wait (`main+0x1743a0c`) and the driver's submit thread in the kernel, no GPU error reported (Tower Cellar, `DM_TEST_GOTO_LEVEL`: 12 of 120 runs at the old rate, 0 of 60 with the limit, 0 of 30 with rays off). The mechanism inside the driver isn't known. A freeze here shows as a run with no dump and `优雅退出 1.5 秒未完成` at the end.
- **A structure key made of an object's state makes a new structure every frame for anything animated.** They pile up (GPU 90+ ms and growing). Only what hasn't changed for a while gets such a key (`rtx_slot_moving`); 256 MiB is a safety net.
- **The key light is often a small part of the picture.** On the Rogue Encampment's ground a full shadow is about 10% darker; most of the light is the unshadowed fill and back lights. The occlusion changes the picture far more than the shadows do.
- **The cost is the number of rays.** On an M6 about 400 million a second, under `DM_TEST_LOAD=gpu:4000`; the blur passes are free by comparison, and better structures, shorter rays and dropping the triangle data barely move it. So what helps is tracing fewer: pixels that were black last frame trace nothing (`DM_RT_DARK=<linear luminance>`, most of a dungeon's picture), where the scene block's row 51 gives the key light no strength it gets no rays, the occlusion takes one ray, and the mirror rays leave out every other pixel.
- **A plane of four channels sits on a four-byte boundary.** The reflections came out bright blue for as long as theirs didn't: the shader and the kernel disagreed by a few bytes, so the strength read as colour.
- **Last frame's picture is still in the forward pass's own colour target** until that pass opens. The game's copy with mips is made later in the frame.
- On an M6 at 2560 x 1440 with frame generation and Temporal, one run each, `gpu_ms.p50` under `DM_TEST_LOAD=gpu:4000`: +6.4 ms per drawn frame in town, +5.3 in the wild, +11.3 walking in the Cathedral, everything on. Free before it: about 21.5.

## A frame that misses a refresh

`DM_LINK_TRACE=<file>` writes one line per display link tick (the time, how long before its target presentation, whether a frame was waiting and how long ago it was queued, how long the last present took), `<file>.gpu` the present's GPU times, and `<file>.shown` when each present really reached the screen (0 = discarded), with the time the link asked for. On an M6 at real 60 (conventional, town and the Blood Moor, 2026-11-05), the 1-2% of frames shown for two refreshes are the system's:

- A frame was waiting at every tick around a miss, and the present took 0.1-0.4 ms of the link thread and about 1 ms of GPU, 31 ms before the next tick. The link's callback itself comes one refresh late, 31.4 ms after the last one.
- `DM_PRESENT_LATENCY=2` changes nothing (ABBA x6: 1.0% and 2.4% against 1.4% and 1.8%, not significant). A single run can read 0%: the misses come and go over minutes, so never judge this from one run.
- More GPU work removes them: `DM_TEST_LOAD=gpu:1500` (about 6 ms at full clock, ABBA x6: 0.9% and 2.6% to 0%, p 0.002), `gpu:700` nearly (ABBA x4: 1.1% and 2.6% to 0% and 0.4%), `gpu:300` not. So it comes with the GPU running slowly because it has room, which also fits fewer misses at 3840 x 2160. Frame Generation shows none.
- No fix is in, and players no longer get real frames (Frame Generation has no row since 2026-11-06): the only one found was to burn GPU time on purpose. Its energy cost isn't known: `@proc_mj` moved only 2% with 3 ms of added GPU work, so it doesn't seem to count the GPU.

With Frame Generation, what the trace shows differs by device (2026-11-06, the game drawing 30 and showing 60):

- **The Mac (M6, 2560 x 1440, 9 runs of 30 s): the link skips a refresh when the GPU has gone idle.** At every miss a frame was waiting, every present met its deadline by 14 ms or more and none was discarded: the link's callback comes about 1.6 ms late and the next refresh brings no callback at all. At the refresh the GPU was busy for 8,076 ticks (1 late, 0 skipped), idle under 5 ms for 6,569 (23 late, 4 skipped) and idle 5-10 ms for 1,187 (67 late, 24 skipped). The frame's GPU work runs from about 7 to 26 ms after the real frame's tick, so it covers the generated frame's refresh and ends 5-10 ms before the next. `DM_TEST_LOAD=gpu:3000` took late callbacks from 33 to 7 and skips from 10 to 3 (3 runs each). `DM_PRESENT_LATENCY=2`, a time-constraint link thread and the Mac App in Game Mode change nothing. What happens inside the system during that idle isn't visible from here: "the GPU's power state" is the reading the numbers fit, not something measured.
- **The phone (iPhone 17 Pro, native output, 3D 50%): a generated frame misses its deadline.** It can't be presented until the frame's GPU work ends, and the GPU slows until that work fills about 30 ms of 33, so it ends a median 5.9 ms before the deadline. Under 2 ms or past the deadline it usually misses (77 of 98 in town), lands on the real frame's refresh, and the system discards the real frame (town: 107 generated frames late, 102 real ones discarded of 2,687 presents). The link itself is on time. A generated frame's margin is about 49.7 ms minus the time to commit the frame minus its GPU time, so what moves it is how early the frame starts.
- **The Mac's fix (2026-11-06): no link.** Apple's frame interpolation guide (`github.com/apple/game-porting-toolkit`, `using-metalfx-frame-interpolation`) doesn't tie each present to a link callback: its own present thread, each image on screen for a minimum time, a timer between the pair. The same here (`nvn_fgpace.h`, `mtl_fg_pace_wait`, `present_main`; `DM_PRESENT_LINK=1` brings the link back). Mac App in front, full screen, Recommended: 15 late images and 12 discarded over four 24 s runs of the link in town with rays off, none with this; with rays on, a 52 s walk in the Cathedral went from 58 late and 48 discarded to 3 late and none discarded, all three in the first 15 s. Energy unchanged.
- **What a frame's start does.** A generated image needs about 16.5 ms between its GPU work ending and its refresh on this Mac, in a window or full screen. The GPU follows the time it has: at a 44 ms lead the frame's GPU time reads about 15 ms against 19.5 at 50, with no more energy and no late images in four runs; at 40 the image no longer fits and everything falls a whole frame back. So the lead stays at 50 (`DM_FG_LEAD_MS`) with rays in mind, which also lets a heavy scene ask for more: `<trace>.start` has each frame's start and lead, and `.shown` its frame number.
- **The iPhone's fix (2026-11-06, iPhone 17 Pro, native output, 3D 50%, alternating runs, 30 s counted): the same, with presents on their own queue.** Without it the link-free path is worse than the link: frames ran 83 ms ahead and a town run fell to 38 fps, as `waitForDrawable` on the game's queue held up the next frame's work (a generated present ended about 75 ms after the frame's start, with 30 ms of work). `g_present_q4` runs presents only and waits on `g_ev` for the frame they show (`DM_PRESENT_QUEUE=0` puts them back). Link against this: town 1.7-1.9% late with 29-32 real frames discarded, 0.06-0.11% and none; Cathedral 0.3-1.3% and 6-22, 0.06% and none; walk 0.22% and 4, 0.06% and none; a crowd of 100 at 24 fps shown and 52% late, 53-55 fps and 10-16% late. A 5.5 minute walk at thermal state 2: 5 late of 19,200. A press takes about 6 ms longer to show (start to generated image 56 ms against 50), and about 100 ms in that crowd. A late image holds for 8-12 ms more on this 120 Hz screen rather than costing a frame. The governor still slows the GPU to about 30 of 33 ms. On the Mac the queue changes neither late images nor energy (1-3 in a 52 s walk with rays on either way).
- **A background run without frame generation could stall on its window (found 2026-11-07; explained 2026-11-08).** `native_2940_mac` went red in 6 of 11 full gates from 2026-11-06 to 11-07 and in 2 of 20 background runs; the state dump showed the game in its own present wait and the GPU three frames behind. It was read then as the system holding images back for a window outside every screen, and three scenarios moved to the front. The real cause was a GPU hang (next section): the same scenario in the background hung 5 times in 40 with presents on their own queue and 0 in 40 on the game's queue, and the three run in the background again.
- **Slower iPhones, simulated on an iPhone 17 Pro (2026-11-06, 3D 50%, 30 s counted).** Each device's Picture Size through `DM_GAME_RES`, plus `DM_TEST_LOAD=gpu:<n>` so a drawn frame takes as long as `dm_picios_ms` says it would there: n = 1000 × (the estimate - the time at that size here) / 14, as 1000 is about 14 ms here. iPhone 17 at Full gpu:336: 60 fps, 0 late. 16 Pro at High gpu:531 in the Cathedral: 60 fps, 1 late of 1800. 16e at High gpu:1050: 60 fps, 1 late. 15 Pro at High gpu:812: 60 fps, 2 late. 15 at Medium gpu:842: 60 fps, 2 late. 14 at Medium gpu:1128: 60 fps, 2 late. 12 at Low gpu:1540: 60 fps, 0 late in town and on a walk, and the image lags its frame's start by 83 ms against 56. A crowd of 100 on the 16 Pro's settings: 55 fps, 8.6% late; on the 12's: 44 fps, 51% late. The CPU isn't slowed, so a real older phone can only do worse in a crowd, and these runs don't show heat over a long session.
- A generated image needs far less after its GPU work ends on the phone (about 2 ms at least) than on this Mac (about 15 ms), so the Mac's limit on starting later isn't the queue.

## The touch overlay's CPU on the phone

`DM_THRCPU=<file>,<s>` writes each thread's CPU every few seconds (100 = one core; the process's first thread is the iOS main thread, which draws the touch overlay), and `DM_NVN_TSV` carries `@proc_mj` and `@t_s` per frame. `touch_e2e.sh` takes them through `E2E_ENV`; pull the files with `devicectl device copy from` afterwards.

- **Joystick held and circled for 60 s in camp (iPhone 17 Pro, 2026-11-08, ABBA, one build installed in turn: before and after af874db).** iOS main thread 23.6% and 19.1% of a core before, 1.3% and 1.2% after; the whole process 86% and 79% before, 66% and 66% after; energy 259 and 387 mW before, 206 and 215 mW after (single runs vary, as `@proc_mj` does); 30 frames drawn a second in all four, thermal state 0. The game's main thread didn't move (13%): walking in camp has no fight, so the skill-button hook's half isn't in this route.
- Before, every finger move redrew the whole overlay in Core Graphics; after, the joystick is two layers that move, and the overlay redraws only when a button's look changes or, for HUD numbers, only the bottom bar.

## The GPU hangs for 4 s

A frame whose GPU time reads about 4000 ms (`[长帧] … GPU 4000 ms`, then a frame of the same length on the CPU while the game waits for it) is a GPU hang that the kernel's driver detected and recovered: the system log has `AGX: Channel error 2` at the moment it ends, and Metal reports no error to the app. Read it with `/usr/bin/log show --start "<time>" --end "<time>" --predicate 'process == "kernel" AND eventMessage CONTAINS "AGX: Channel error"'` (in zsh, a bare `log` is a builtin). A hang that ends during a quit makes the graceful exit run out (`优雅退出 1.5 秒未完成`).

- **Cause (2026-11-08):** the second command queue that took presents from 2026-11-06 (4d31858). From then to 11-08, 23 of about 1,800 Mac App and iOS-package runs had such a frame (none in the runs kept from before), each with the kernel line where the log still covered it; the 2.0.2 update test's "frames are not advancing" was one. `native_2940_mac` (`DM_TEST_NO_FG=1`, `DM_PRESENT_LINK=0`) in the background, alternating: 5 of 40 runs hung with presents on their own queue, 0 of 40 with `DM_PRESENT_QUEUE=0`. In a caught run every present command buffer took 0.2 ms, and the game's command buffer of frame 381 ran from 7152.7 to 11153.3 ms on the GPU (`DM_LINK_TRACE` `.gpu` and `DM_NVN_TSV` `@gpu_beg_ms` / `@gpu_end_ms`).
- **Fix, part one:** on Macs presents go on the game's queue again, as before 2026-11-06 (the separate queue made no measurable difference there); iPhones and iPads keep their own queue, which fixed late and discarded frames there, and no phone log shows such a hang. `DM_PRESENT_QUEUE=0/1` still decides when set. Why two queues hang the Mac's GPU is not known.
- **Not the whole cause (2026-11-08, 20:30, a player's session on 2.0.4, presents on the game's queue):** full screen, the Den of Evil, this Mac's Recommended (rays Full, frame generation, MetalFX Temporal). The display stopped updating at 20:30:40.9; the kernel logged `AGX: Channel error 2` at 44.845 and again 33 ms later, then `Deny submissions/ignore app[D2RMobile] with 2 GPURestarts in 35 submissions`. Metal's own submission thread then sat in the driver for good (the system's spin report has it in `IOGPUFamily` → `AGXG18G`): no command buffer completed or failed, so no error line was logged, the game's main thread waited in `nvnSyncWait` → `mtl_wait` for ever, and the full-screen display never came back; 40 s later WindowServer's watchdog restarted the session (`/Library/Logs/DiagnosticReports/WindowServer-*.ips`, `bug_type` 409, `displayState` OFF; `*.userspace_watchdog_timeout.spin` has every process's threads, symbolicated with `atos -o <the runtime's D2RMobile> -l 0x100000000 <0x100000000 + offset>`). The A/B route above ran neither rays nor frame generation, so neither is needed for a hang.
- **Fix, part two (the game survives one):** the GPU watchdog (`nvn_mtl.m` `gpu_watch`): with work waiting and no main submission finished for `DM_GPU_LOST_S` seconds (10; a recovered hang takes about 4 s; 0 turns it off), the GPU is taken as lost (`gpu_lost` event, `【红】` lines naming the first waiting submission and every wait point). From then on nothing is sent to the GPU (each command buffer's feedback runs at once with no feedback object), every wait on it returns, frames are dropped, and the game's loop runs on undrawn, so the front end saves and quits as its own quit does (`front_gpu_lost`), leaving `gpu_lost.txt` beside the settings; the next launch takes it and tells the player (`gpu_lost_note`). Tested by `scenarios/faults/gpu_lost.scn` (`fault arm gpustall`: every later submission vanishes, as in that driver) and `dm/gfx/tests/t_gpu_lost.m`. Whether the display comes back once the process has gone is not known: a real hang with a full-screen window has not been reproduced on purpose.
- **The hang recorder (what the GPU was on):** `DM_HANGREC` (on; `=0` turns it off). Each pass of a main submission writes a timestamp at its start and at its end into its submission's counter heap (`DM_NVN_PASSGPU`'s, without its file), each present's draw writes one, and a queue log keeps the last operations sent to the GPU's queues (submissions, uploads, waits for drawables, presents). Nothing is read back in play.
  - The GPU watchdog prints the record as `[hang]` lines after `DM_HANGREC_EARLY_S` (2 s) with work waiting and none finished, before the driver's reset (about 4 s in) can cut the submission short, and again when it takes the GPU as lost. A record has the queue log, the first unfinished submission's passes (how many finished, the first that didn't, the two before it, any MetalFX or frame generation run in between), whether each waiting present drew (a present draws only once its drawable is free), the translation summaries of the last finished submission and the first unfinished one, and the scene (`dm_hang_scene`: area, position, life, monsters, panel, loading screen).
  - A submission or present that took `DM_HANGREC_SLOW_MS` (1000) or more on the GPU, as the recovered hangs did, gets its five longest passes printed.
  - The GPU writes a pass's timestamps only once its work has run: while a pass runs, even its start mark reads 0, and a timestamp in an encoder with no work is never written. So the record names the first pass that didn't finish, not which one had started (`dm/gfx/tests/t_hangrec.m`, with a kernel that keeps the GPU busy for about 300 ms). A finished pass's marks can take a few milliseconds to be readable: a record from a GPU that is fine may show its last pass or two as unfinished, but within 5 ms they read while later work is stuck, in the same command buffer or the next, so a hang's record (2 s in) is not affected.
  - `DM_TEST_HANGREC_DUMP_S=<s>` prints one record that many seconds after the first submission, from a GPU that is fine.
  - Cost on an M6 (2026-11-09, `town_loop` on the Mac App at this Mac's Recommended in full screen, 6 + 6 ABBA, `DM_HANGREC=0` against on): no measurable difference. GPU p50 −0.03 ms in town and +0.07 ms in the Blood Moor, translation thread +0.13 / +0.06 ms, the process's CPU +0.36 / +0.04 ms and energy +0.44 / +0.10 mJ per frame; every p ≥ 0.23.
- **Catching one:** loop a scenario and keep the first run with a frame over a second (both traces on); `native_2940_mac` in the background hung in about one run in eight before the fix.

## The game's own quality settings

`DM_TEST_RCFG=<hex renderer offset>.<b|w>=<value>[,...]` changes the renderer's boot settings at the observation gate at `Renderer_Init` (`main+0x15ff470`), just before it copies them to `renderer+0x65f4` (0xd5 bytes). The Switch build runs the game's lowest level, and most modules read their knob once at creation, so this is how to try the others one at a time. What it found:

- High in the Video tab's Environment Detail: terrain tier `6622.b=3`, effect-light budget `66bc.w=4`.
- Don't raise the material arrays `664c` past 7: the Switch's assets are 128 px, and at 8 or 9 every terrain stamp and terrain decal shrinks to a corner (the replay layer refuses those copies with a red line).
- No visible change: VFX key `66b8`, combat level `66c0`, upsample `66a8`, model LOD `6621` / `6650`, atmosphere `662c` / `6630` (fog density is 0 in every shipped area).
- Don't use: bloom mode `6628.w=2` misfires the host's bloom replacement, and reflections `6634.w=5` turn every surface into a mirror, since nothing writes the normal and roughness target they read on Switch.

## PC mode probes

Test switches for finding out how far the Switch build's own PC (keyboard and mouse) mode still works. They change nothing unless set, and players never set them.

- **PC layouts:** `DM_PROBE_PC_LAYOUTS=3` points the `Controller/Console/Switch/` and `Controller/Console/` layout prefixes at folders that don't exist. Bit 4 (the bare `Controller/`) breaks boot, since other loaders share that string. For the PC screens, also pass an overlay (`casc_overlay.py`) that replaces each `Controller/<Panel>HD.json` that has a PC twin with that twin. Leave out `GlobalData*`, `_profile*` and `FocusManager*`: replacing those crashes boot.
- **PC input,** as `DM_UI_TAPS` actions (frame and x,y as for the other actions):

  | Action | What it sends |
  |---|---|
  | `K<vk>` | `KeyDown` / `KeyUp` to the interface root |
  | `Y<vk>` | a key down/up platform event, into the game's input translator |
  | `L` / `R` | a left / right click platform event, into the game's input translator |
  | `H<frames>` | the left button held, as platform events |
  | `M` | a mouse move platform event |
  | `E<action>` | the world click entry at a tile beside the player (x,y − 500 per mille / 25 = tile offset) |
  | `X<id>` | runs key binding `<id>` through the game's hotkey executor |
  | `Z<mode>` | sets the game's hotkey mode |

**The PC interface set** (Phase 1 of the PC control mode): the Mac overlay (`DM_OVERLAY_MAC`) carries it in `layouts/3pc_console_switch_layout/` and `4pc_console_layout/` with the marker `pc_layouts`; `DM_PC_MODE=1` starts in it (also on the command-line host with that overlay), and `inputmode mode=pc|controller` switches live. The bare `Controller/` prefix is never repointed: the game also builds console art paths with it.

What the first probes found (2026-10-31, command-line host):
- **Works:**
  - **The PC screens:** the PC HUD and character select render.
  - **Key bindings:** in game the key-binding list holds D2's PC defaults. `X1` and `X0` open the PC inventory and character sheet, still inside the console tab frame.
  - **Click to walk:** `E0` (left press) walks to the clicked ground, once the host's click-ground move drop is exempted.
- **Doesn't, so far:**
  - **Platform events:** keys and mouse sent through the input translator change nothing.
  - **The PC world-mouse handlers** are empty stubs on the Switch.
- Details are in the private engine notes.
