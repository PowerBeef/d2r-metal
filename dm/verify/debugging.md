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
- **A level brings hundreds of new structures at once, and they come in at 24 a frame (`RT_NEW_PER_FRAME`).** Over a hundred in one frame stalled the GPU for good in about one level change in ten, with the game's main thread in `Present_SyncWait` and the driver's submit thread in the kernel, no GPU error reported (Tower Cellar, `DM_TEST_GOTO_LEVEL`: 12 of 120 runs at the old rate, 0 of 60 with the limit, 0 of 30 with rays off). The mechanism inside the driver isn't known. A freeze here shows as a run with no dump and `优雅退出 1.5 秒未完成` at the end.
- **A structure key made of an object's state makes a new structure every frame for anything animated.** They pile up (GPU 90+ ms and growing). Only what hasn't changed for a while gets such a key (`rtx_slot_moving`); 256 MiB is a safety net.
- **The key light is often a small part of the picture.** On the Rogue Encampment's ground a full shadow is about 10% darker; most of the light is the unshadowed fill and back lights. The occlusion changes the picture far more than the shadows do.
- **The cost is the number of rays.** On an M6 about 400 million a second, under `DM_TEST_LOAD=gpu:4000`; the blur passes are free by comparison, and better structures, shorter rays and dropping the triangle data barely move it. So what helps is tracing fewer: pixels that were black last frame trace nothing (`DM_RT_DARK=<linear luminance>`, most of a dungeon's picture), where the scene block's row 51 gives the key light no strength it gets no rays, the occlusion takes one ray, and the mirror rays leave out every other pixel.
- **A plane of four channels sits on a four-byte boundary.** The reflections came out bright blue for as long as theirs didn't: the shader and the kernel disagreed by a few bytes, so the strength read as colour.
- **Last frame's picture is still in the forward pass's own colour target** until that pass opens. The game's copy with mips is made later in the frame.
- On an M6 at 2560 x 1440 with frame generation and Temporal, one run each, `gpu_ms.p50` under `DM_TEST_LOAD=gpu:4000`: +6.4 ms per drawn frame in town, +5.3 in the wild, +11.3 walking in the Cathedral, everything on. Free before it: about 21.5.

## A frame that misses a refresh

`DM_LINK_TRACE=<file>` writes one line per display link tick (the time, how long before its target presentation, whether a frame was waiting and how long ago it was queued, how long the last present took), and `<file>.gpu` the present's GPU times. On an M6 at real 60 (conventional, town and the Blood Moor, 2026-11-05), the 1-2% of frames shown for two refreshes are the system's:

- A frame was waiting at every tick around a miss, and the present took 0.1-0.4 ms of the link thread and about 1 ms of GPU, 31 ms before the next tick. The link's callback itself comes one refresh late, 31.4 ms after the last one.
- `DM_PRESENT_LATENCY=2` changes nothing (ABBA x6: 1.0% and 2.4% against 1.4% and 1.8%, not significant). A single run can read 0%: the misses come and go over minutes, so never judge this from one run.
- More GPU work removes them: `DM_TEST_LOAD=gpu:1500` (about 6 ms at full clock, ABBA x6: 0.9% and 2.6% to 0%, p 0.002), `gpu:700` nearly (ABBA x4: 1.1% and 2.6% to 0% and 0.4%), `gpu:300` not. So it comes with the GPU running slowly because it has room, which also fits fewer misses at 3840 x 2160. Frame Generation shows none.
- No fix is in, and players no longer get real frames (Frame Generation has no row since 2026-11-06): the only one found was to burn GPU time on purpose. Its energy cost isn't known: `@proc_mj` moved only 2% with 3 ms of added GPU work, so it doesn't seem to count the GPU.

## The game's own quality settings

`DM_TEST_RCFG=<hex renderer offset>.<b|w>=<value>[,...]` changes the renderer's boot settings at the observation gate at `Renderer_Init` (`main+0x15ff470`), just before it copies them to `renderer+0x65f4` (0xd5 bytes). The Switch build runs the game's lowest level, and most modules read their knob once at creation, so this is how to try the others one at a time. What it found:

- High in the Video tab's Environment Detail: decal size `664c.w=9`, terrain tier `6622.b=3`, effect-light budget `66bc.w=4`.
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
