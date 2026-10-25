# Debugging

中文：[debugging.zh-CN.md](debugging.zh-CN.md)

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
make -C dm/host dmhost_m3 && SCN_DMHOST=$PWD/dm/host/dmhost_m3 python3 dm/tools/scenario.py run dm/verify/bench/town_loop.scn --metal-validation shader
```

| Level | Turns on | Verdict |
|---|---|---|
| `api` | API validation (`MTL_DEBUG_LAYER=1`); errors are logged instead of asserting, warnings are off | Any message is red |
| `lint` | `api` plus the warnings (`MTL_DEBUG_LAYER_WARNING_MODE=nslog`): redundant or unused state settings | Summary only |
| `shader` | Shader validation, reported to stderr; the stall limit is raised to 300 s, because every pipeline is instrumented and recompiled (about 0.15 s each) | Any message is red |
| `all` | `api` + `shader` | Any message is red |

The messages are summarized in `metal_validation.txt`; from the shell, `python3 dm/verify/assertlib.py metal <log> [--lint]`. The flag works with the `cli` and `mac-app` backends. The iOS backends start their process differently and don't pass these variables.

Findings:

- With shader validation on, the Metal 4 host crashes at its first present commit inside Metal's own `-[MTL4GPUDebugCommandBuffer applyResidencySets:]`, with the layer's residency set attached (macOS 27). Use the older-API host (`dmhost_m3`) for shader validation. It runs the same shaders, and the runner prints a note when it sees the combination.
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

## Replay and the first divergent tick

`dm/tools/replay.py` records a scenario run's client command packets and per-tick state (`DM_C2S_LOG`, `DM_TICKS_FULL`), then replays the packets on any host build (`DM_C2S_REPLAY`) and compares tick by tick. This answers whether a change or a build changes the game logic.

```bash
python3 dm/tools/replay.py record dm/verify/bench/town_loop.scn                  # → $DM_ROOT/run/replay/town_loop_4242/
python3 dm/tools/replay.py check ~/dm-work/run/replay/town_loop_4242 --dmhost dm/host/dmhost --dmhost dm/host/dmhost_asan
python3 dm/tools/tickdiff.py a/ticks.jsonl b/ticks.jsonl                          # any two DM_TICKS records
```

`tickdiff.py` reports the first tick where the two records differ and the state slots that differ (player, monster, object, missile, item, seed). With per-unit rows on both sides, it also names the units and fields: `player id 1 cls 1: mode 3→5, yf 65437→32768`. It also replaces the comparator `host_savecmp.sh` used to call, which was missing from the repository.

What determinism needs, and what the tool handles:

- **Fixed seed and time:** `DM_GAME_SEED` and `DM_FIXED_TIME`. `record` uses the scenario's own values when it sets them; `town_loop` does, because its coordinates depend on them.
- **The same entry and spawn:** the replay enters through the same path as the `enter_game` macro. The host moves the town spawn next to the waypoint (`dm_townwp.c`), but by default not during a replay, because original-game recordings walk from the original spawn. The replay therefore sets `DM_TOWNWP_OFF=0` to keep the move. Without it, the replay starts about 40 cells away, every movement packet misses, and replays agree with each other only because nothing happens.
- **No join handshake in the file:** the tick-0 system messages are left out. Replay drops the client's own messages between the file's first and last ticks, so a first tick of 0 would also drop the host client's own join, and the server would wait forever.

Measured:

- **Builds agree:** `town_loop` (781 ticks, 144 command packets, camp and Blood Moor) replays identically to the recording on the normal, ASan and older-API hosts.
- **Changes are caught:** removing the walk commands of ticks 405–500 gives a first divergent tick of 444: the player stops where the route already issued runs out.

Recordings are derived from the game program and stay local.
