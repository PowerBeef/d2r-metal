# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is **d2r-metal** (`PowerBeef/d2r-metal`), developed independently since 2026-10-28; it started as a fork of `jhaizhou-ops/d2r-mobile`.
- The project ports the Nintendo Switch build of Diablo II: Resurrected + Reign of the Warlock (1.0.34.0 = PC 3.3.94012) to macOS and iOS / iPadOS.
- The game's own aarch64 code runs natively; only a few instructions are rewritten, and the graphics API is reimplemented on Metal.

Everything derived from the user's game files lives in the workspace `$DM_ROOT` (default `~/dm-work`), outside git:
- the extracted ExeFS/RomFS in `data/switch/3.3.94012/Update/`
- rewrite products, the Python venv and shader caches in `build/`
- run logs in `run/`

## Project workflow

d2r-metal started from `jhaizhou-ops/d2r-mobile` and has been developed independently since 2026-10-28 (a hard fork: no upstream remote, merges or pull requests).

- **Platform scope: macOS and iOS / iPadOS only**, on Apple silicon, macOS / iOS 27 and later, Metal 4 and MetalFX (see "Metal 4 only" under Architecture). There is no Android, HarmonyOS, Vulkan, tvOS or older-API code; don't add any.
- **Out of scope for good:** features that change the game's rules or economy, or play part of it for the player. On 2026-10-27 the fork deleted those it had inherited (Greater Rift, Boss Rush, Elysium, Kill Board, free town portal / identify, Refresh Stock, auto-drink, mercenary auto-feed, auto-recast buffs, the loot filter, the Runeword helper, the extra storage, the difficulty selectors, the minimap marks and two logic-changing byte patches). The stash stays 10 columns (`inventory.bin` is never patched); Options keeps the game's `OfflineDifficultyScaling` row.
  - Their gate numbers stay retired: OBS 15, 16, 17 and 22 (`OBS_MAX` stays 23) and svc 0x89, 0x8c, 0x8d and 0x8e. Never reuse them. The product revision only moves forward (`dm/PRODUCT`).
- **Branding:** the apps show "D2R Metal" (`mac_app.sh` `NAME`, the Mac window title, the iOS `CFBundleDisplayName`, player-facing strings). Everything player data hangs on keeps the old spelling: the bundle IDs, the Mac data folders (`APP_NAME` "D2R Mobile" in `app_mac.m`), the iOS "D2R Mobile Resources" folder, the `org.d2rmobile.*` IDs and the executable `D2RMobile`. Don't rename those.
- **`origin`** = `PowerBeef/d2r-metal`. **`main` is the trunk**: all work lands there, and the user's apps are built from it.
  - It is public: never force-push it, rebase it, or reset it.
  - Bigger work can use a short-lived local topic branch, merged into `main` with `--no-ff` when done.
- **Commits:** one topic per commit (or a short series), English message, ending with the attribution line.
- **The three files `./setup.sh` rewrites for local tools stay dirty and are never committed:** `dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`, `dm/gfx/nvn/c1_full.tsv`.
  - Don't `git reset --hard` / `git checkout .` over them. Restoring them means re-running `./setup.sh`, which re-translates every shader (~10 min).
  - Switch branches with `git switch`.
  - `./setup.sh` links the tracked pre-commit guard (`dm/tools/git-hooks/pre-commit`): it refuses these files, `dm/ios/local.env`, `dm/ios/Gen/`, game, key and save file types, analysis databases, lines that look like decompiler output, and anything over 2 MB.
- **Worktrees and parallel sessions** (git worktrees, Claude Code `--worktree` or desktop worktree sessions):
  - **A new worktree is missing local files:**
    - The three `setup.sh` rewrites are tracked, so a worktree gets the committed versions. When the branch hasn't changed them, copy the main checkout's rewritten copies in. Otherwise run `./setup.sh` in the worktree.
    - `dm/ios/local.env` and the two host headers generated from the RomFS (`dm/host/dm_hud_exp.h`, `dm_bright_levels.inc`) are gitignored. Copy them, or regenerate the headers with `make -C dm/host <header> ROMFS=$DM_ROOT/data/switch/3.3.94012/Update/RomFS`.
    - Claude Code copies the gitignored files listed in a `.worktreeinclude` at the repo root (`.gitignore` syntax) into the worktrees it creates.
  - **Every checkout shares `$DM_ROOT`:**
    - `$DM_ROOT/out/macapp` is the player's installed Mac App. From a worktree, build into another folder: `dm/tools/mac_app.sh <dir>`.
    - Install on the phone (`run.sh install`) only from the main checkout on `main`.
    - Windowed game runs steal each other's frames, so take the Mac game lock around them: `source dm/ios/tools/mlock.sh; mlock_acquire "<who> <what> ~N min"; trap mlock_release EXIT`. `smoke_mac.sh`, the touch cases, `scenario.py` (windowed `cli`, `mac-app`; the `ios-mac` / `device` backends take it or the device lock), `bench.py`, `explore.py`, `replay.py` and `fork_gate.sh` take it themselves unless `XT_LOCK_HELD=1` says a caller holds it; headless runs don't need it.
  - **Land worktree work on `main` from the main checkout:** merge the branch, then delete it.
    - Claude Code's worktrees branch from `origin/main` by default, so push `main` first if a new worktree needs local commits.
    - Two sessions never edit the same files.

## Repository rules (CONTRIBUTING.md)

1. **No game content, no engine-analysis output.** Nothing from the game, and nothing generated from it, goes into git, issues or PRs: no program, art, text, shaders, saves, keys or built packages; no decompiled code, disassembly or database files either (see "Game reference").
2. **English for new comments and docs.**
   - Many existing comments are bilingual (Chinese lines, then English; trailing `中文 | English`). When you edit one, replace the pair with the English text; don't add new Chinese comments.
   - Every source file opens with a comment saying what it does. `dm/host` feature files also list the game addresses they rely on.
   - Docs are English only (the zh-CN / es copies were dropped on 2026-10-28). `dm/third_party/` is exempt.
3. **Player-visible text goes in the string table, never as a literal:**
   - Add the key / Chinese / English row to `dm/i18n/ui_text.tsv` (the game offers Chinese, so that column stays). Keys are `dm` + alphanumerics, no underscore; English length caps are in `EN_LIMITS`.
   - Regenerate with `python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h` and commit it.
   - A new Python script that writes player-visible text must be added to `PY_FILES` in `dm/i18n/scan_literals.py`.
4. **Logs:** new log lines in English. A failure line carries `【红】`, which the tools match (`logred.sh`, `assertlib.py`, `dmtest.py`). Many existing log lines are Chinese and scripts match their text: change one only together with every script that reads it, and prefer a `DM_EV` event for anything a test needs.

## Commands

The Python version and the venv:
- Use the Python version the docs verified (3.14). If the default `python3` is newer, put a 3.14 first on `PATH` for `./setup.sh`, which creates the venv from it.
- Most scripts need the venv: `source $DM_ROOT/build/cache/dm/venv/bin/activate`.

```bash
./setup.sh check                      # read-only health check (tools, game files, signing)
./setup.sh                            # venv, rewrite (product.sh build), shader translation; reruns skip done steps
./setup.sh <package>... <prod.keys>   # extract game files first (base and update may be separate packages)

dm/tools/mac_app.sh                   # Mac App → $DM_ROOT/out/macapp/D2R Metal.app (macOS 27+)
cd dm/ios && DM_DEVICE_FAMILY=1 ./run.sh build && ./run.sh install   # iPhone; signing in dm/ios/local.env
```

**Source-only checks** (no game files needed; run for every change):

```bash
python3 dm/i18n/scan_literals.py      # → I18N OK
python3 dm/ios/tools/dm_i18n.py --check
```

**Test suites** (need the prepared workspace + venv). Full suite / one test:

```bash
make -C dm/host/tests run             # host unit tests + source scans (native_write_scan, mode_scan, i18n)
cd dm/host/tests && make build/t_widget && ./build/t_widget        # one host test (binaries in build/)
make -C dm/gfx/tests run              # graphics tests on the local Metal device
cd dm/gfx/tests && make t_nvn && ./t_nvn                            # one gfx test (binaries in tests dir)
make -C dm/ios/tests test             # iOS shell unit tests (plain `make` only builds)
cd dm/ios/tests && make t_touch_controls && ./t_touch_controls      # one iOS test
bash dm/ios/tools/devbattery_test.sh  # needs GNU `timeout` + `ideviceinfo` on PATH
```

Some host tests need environment or arguments; copy them from the `run:` recipe in the Makefile:
- `t_fs_case`, `t_cloudsync` and `t_i18n` need `TMPDIR="$(pwd)/build"`.
- `t_i18n` takes a mode (`en zh none …`).
- `t_romfs_validate` needs `DM_ROMFS=…/RomFS`.

**New test files:**
- Host: add to both the dependency list and the `run:` command list.
- iOS tests: built with `-Werror -pedantic`.

**Verification and smoke:**

```bash
dm/verify/regress.sh [--fresh]        # rewrite-equivalence regression → REGRESS OK|FAIL; needs LLVM_BIN (llvm-objdump/objcopy) and GNU timeout
dm/ios/tools/make_fixture.sh          # once: creates the smoke save
dm/ios/tools/smoke_mac.sh 1           # launches the iOS package on the Mac into a game → SMOKE-MAC OK|RED
dm/ios/tools/check_hud_gen.sh         # no game derivatives tracked in git → HUDGEN OK
dm/tools/product.sh build|path <touch|rules|keypage>|rev
```

- Use `regress.sh --fresh` whenever `dm/tools/patch_nso.py` changed.
- **The fork's gate is `dm/verify/fork_gate.sh quick|unit|core|full|long|device`** (`dm/verify/FORK_GATE.md`): each step through `dmtest.py`, results in `$DM_ROOT/run/gate/latest/`, last line `FORKGATE OK|RED {json}`. Run `quick unit` for every change, `core` for host changes (≤ 15 min), `full` before rebuilding the apps.

**Machine-readable results** (prefer these to grepping Chinese log lines):

```bash
python3 dm/tools/dmtest.py [--junit j.xml] [--timeout s] -- <any runner>   # result.json + last RESULT {json} from the runner's verdict lines
DM_EVENTS=<file>|log ./dmhost …                                           # NDJSON events: boot, main_menu, game_enter, loading_done, save, red, fatal, crash, app, exit
python3 dm/tools/dmevents.py expect <file> main_menu game_enter loading_done --no-red   # also: summary | why | follow
python3 dm/tools/switches.py check                                         # dm/switches.tsv is current; no script sets a switch nothing reads
```

- The event types are listed in `dm/verify/event_stream.md`. Add new events with `DM_EV(...)` and keep the Chinese log line next to it.
- `switches.py gen` must run after adding a `getenv("DM_…")` or a doc mention, or `make -C dm/host/tests run` goes red.
- Analysers with a `RESULT {json}` last line: `dm/ios/tools/soak_report.py` (soak), `dm/gfx/tools/ub_table.py` (`DM_NVN_UB` A/B phases).

**Driving a running game (control port + scenarios):**

```bash
python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn [--backend cli|mac-app|ios-mac|device] [--headless]
python3 dm/tools/scenario.py check dm/verify/scenarios/*.scn       # parse only (also in the host suite)
python3 dm/tools/dmctl.py --listen <port> state                     # one command to a host started with DM_CTL_PORT=<port>
python3 dm/verify/assertlib.py logred <log> [--allow regex]         # red-marker guard (catches plog lines events miss)
python3 dm/tools/imgdiff.py a.png b.png --max-mean 0.05            # perceptual screenshot diff (LDR-FLIP)
python3 dm/tools/playbot.py run --episodes 3 --intents 12 | soak --minutes 30    # playtest bot: player intents + invariants, shrunk by intent; soak judged by soak_report.py
python3 dm/verify/golden.py replay|shots check   (record --reason …; prefix)     # our own baselines in $DM_ROOT/data/switch/golden: tick-identical replay + savecmp + negative control, shotgate
```

- The protocol is in `dm/verify/control_port.md` and the scenario format in `dm/verify/scenarios/README.md`.
- On the Mac the host connects out to the controller (`DM_CTL_PORT`), because `ios/run.sh` allows only one `listen(` (the iOS log port). On iOS, commands go over `DM_LOG_PORT`.
- `mac-app` runs a test copy (`$DM_ROOT/out/macapp_test`) with `DM_APP_SUPPORT` set. Never point scenarios at the player's real save folder.
- `ios-mac` needs `.iosdd_mac/DmMacSmoke.app`; `dm/ios/tools/smoke_mac.sh 1` builds it. Saves for the iOS backends are the ones bundled at build time.
- Generated saves (`export_save`, `scenarios/saves/*.scn`) stay in `$DM_ROOT/data/switch/save_ref_<name>/` and are never committed.
- **Engine-aware state (`dm/host/dm_engine.c`):** `state` carries `panels` (visible, top first), `focus` / `focus_panel`, `cursor_mode`, `save_busy` / `save_pending` / `save_mounted`, and from the tick thread (`dm_hud`) `gold` / `gold_stash`, `items` / `item_dups` (repeated item ids, must be 0) and `monsters` (live, NPCs included); `engine` gives the whole snapshot plus the last actions and UI messages (the runner saves it as `engine.json` on the first red). Events `action` (controller actions under the game's own names), `focus`, `save_request`, `save_stuck` (red), `save_alert`, `fault`, `padmode`, `opmode`, `language`. Checks: `expect_state panels~=X`, `check actions seq=…`, `check save_pairs`, `check saves_valid` (`dm/verify/d2save.py`; every cli / mac-app run checks its save).
- **Faults and front-end input:** `fault arm <point> skip=N count=N` / `fault delay ms=N` (`DM_TEST_FS_FAILAT=<point>[@skip][:count]`, `DM_TEST_SAVE_DELAY`); `fe "<s> pad A"` runs one `DM_TEST_NSMOUSE` line through the Mac front end (virtual controller, on-screen keyboard, sheet); `inputmode mode=touch|controller glyph=…` (needs `env DM_ROMFS_OVERLAY=auto` on cli). Every fault / switch scenario must require the event that proves it happened.
- **Pointer steps and their paths:**
  - `tap` goes straight to `dm_ui_touch`. It skips the Mac mouse path, and the game ignores plain clicks on console tabs, so switch tabs with `press ZR` / `ZL`.
  - On the Mac App, `drag x0 y0 x1 y1` goes through the real mouse's gesture path (`dm_touch_host_live`); a zero-length `drag` is a mouse click. `wheel x y dy [precise] [view]` calls the same `dm_wheel_ui` as the App's scroll events.
  - Options opened with `msg PanelManager:OpenPanel:SettingsPanel` in game don't count as a menu for the mouse code; open them as a player does, with `open_pause` then `tap_ui Settings`.
  - Real mouse events for any Mac App build, including ones without a control port: `DM_TEST_NSMOUSE=<file>` (`down` / `dragto` / `hover` / `key` lines), `DM_TEST_PRESENT_SHOT=<s>:<png>`, `DM_TEST_QUIT_AT=<s>`. Leave `DM_WATCH` unset there: its stall watchdog kills the front end after 15 s.
  - Lines that work with the App in the background (no key window needed): `nav <keycode>` (a key straight to the game's key handler), `pad <buttons>` / `pads b1,b2,…` (virtual controller presses through `dm_gamepad`, self-timed; one scheduled line per press gets merged), `navpad <buttons>` (skipped while a text box is open, so extra presses to reach a page are harmless), `skey <keycode> <chars>` (through `[NSApp sendEvent:]` and the event monitors), `sheet ok|cancel [text]` (fill and press the Mac text sheet). `DM_TEST_PAD_GLYPH=2|3|0` makes the virtual controller count as Xbox / PlayStation / Switch for the key caps.
- `ios-mac` keeps running the package `smoke_mac.sh 1` last built, not the `run.sh build` output: rebuild it after layout or host changes.
- **A phone on Wi-Fi only:** devicectl paths work (`lifecycle.sh`, `touch_e2e.sh` / `uireg … run_dev.sh`, `heat_ab.sh`, file pulls). `smoke.sh` and the scenario `device` backend need a USB cable, because the app's log port (`DM_LOG_PORT`) listens on the phone's loopback, which `iproxy -n` can't reach ("Connection refused"). `ideviceinfo` can't read the battery over Wi-Fi either: `DM_DEVBATTERY_INRUN=1` lets the battery gate use the app's own reading.

**Performance:**

```bash
python3 dm/tools/bench.py run dm/verify/bench/town_loop.scn --reps 3 [--mpt]       # per-segment CPU / NVN / GPU / present time → bench.json
python3 dm/tools/bench.py ab dm/verify/bench/town_loop.scn --reps 6 --a-env … --b-env … --metric gpu_ms.p50   # ABBA, Mann-Whitney + bootstrap, ledger
python3 dm/tools/gpucap.py capture|summary|listen|hud …                            # gpucapture / gpudebug / metalperftrace
python3 dm/tools/boothist.py show --label bench:cli                                # boot-stage history across revisions
python3 dm/tools/bench.py profile dm/verify/bench/combat.scn                         # sampled per-thread-role attribution with engine names + hitches over 25 ms
python3 dm/tools/bench.py baseline save && python3 dm/tools/bench.py regress <route>  # ABBA against a kept host; red only on a significant slowdown
```

- Present intervals are pinned at 16.67 ms by vsync on this Mac; compare `cpu_ms`, `nvn_ms` and `gpu_ms` instead.
- Use at least 4 runs per variant (the minimum exact p is 0.029); 6 is the default.
- To compare two rewrite outputs (one changed instruction, say), make a scratch copy of the output and point runs at it: `SCN_PRODUCT=<dir>` for scenarios and benchmarks (`--a-env "SCN_PRODUCT=…"`), `DM_SHOT_PRODUCT=<dir>` for `lookbook.py` / `golden.py` shots. Never patch the real output under `build/cache/dm/out`.
- `proc_mj` is the process's energy per frame from the kernel's account (macOS: `proc_pid_rusage`; iOS: `task_info` `TASK_POWER_INFO_V2`); A/B it with `--metric proc_mj.mean`, no admin rights needed.
- On the iPhone the GPU clocks down until each frame's GPU time fills the frame interval, so `@gpu_ms` reads the same for every picture setting (about 15 ms in game on an iPhone 17 Pro). `@proc_mj` follows the CPU and doesn't show added GPU work either (two runs of one setting differed by 15%). What the phone lets an app see is frame pacing, CPU and the thermal state.
- `XTRACE_*` are os_signposts on Apple. Record them with `xctrace record --template Logging --attach <pid>`.
- A GPU capture is about 4 GB per frame, and the disk is tight. `gpucap` refuses to capture when free space is short, and the `gpu_capture` step deletes the trace unless `keep=1`.

**Debugging** (all in `dm/verify/debugging.md`):

```bash
python3 dm/tools/crashlog.py show <run>/crash.json                                  # symbolicated crash (DM_CRASH_JSON; the runner writes crash.txt)
python3 dm/tools/guestsyms.py build                                                  # local guest symbol table for module+offset lookups
make -C dm/host dmhost_asan|dmhost_ubsan|dmhost_tsan && SCN_DMHOST=$PWD/dm/host/dmhost_asan python3 dm/tools/scenario.py run …   # sanitizer host; any report is red (sanitizer.txt)
python3 dm/tools/scenario.py run … --metal-validation api|lint|shader|all           # Metal validation layers (metal_validation.txt)
python3 dm/tools/explore.py run --seed N --episodes K [--dmhost …]                  # random play; a red run is shrunk to minimal_<k>.scn (a failed step also writes the host's thread dump into host.log)
python3 dm/tools/replay.py record <scn> && python3 dm/tools/replay.py check <dir> --dmhost A --dmhost B   # same input on two builds → first divergent tick
python3 dm/tools/tickdiff.py a/ticks.jsonl b/ticks.jsonl                             # first divergent tick between two DM_TICKS records
python3 dm/tools/c2s.py summary|near <recording> [tick]                              # recorded client packets labelled (dm/verify/c2s_opcodes.tsv), lengths checked
DM_BREAK_AT_TICK=<tick> ./dmhost … ; lldb -p <pid>; (lldb) command script import dm/tools/lldb/dm.py   # dm modules|where|bt|break|tick|ui|focus|saves|unit <id>
```

- The sanitizer targets don't track headers: delete the binary to force a rebuild. They run game threads on 8 MiB host stacks and carry their own options and TSan suppressions (`main.c`).
- Shader validation crashes windowed runs at the first present inside Metal's validation layer (macOS 27, still on 27.0.1); run it `--headless`, which still validates all game rendering (the fork has no older-API host to fall back on).
- `DM_BREAK_AT_TICK` waits on the tick thread: start the host by hand without `DM_WATCH` / `DM_WATCH_STALL`. In `lldb --batch`, commands after the trap go in `-k`, not `-o`.
- Agents can drive the same lldb through `(lldb) protocol-server start MCP` + `xcrun lldb-mcp` (one MCP server at a time; delete stale `~/.lldb/lldb-mcp-<pid>.json`).
  - With no server running when it starts, `lldb-mcp` starts its own lldb, which stays up and is reused. On Xcode 27.1 that lldb can't attach ("Bad file descriptor") unless `LLDB_EXE_PATH` points at a wrapper that gives it a stdin; `debugging.md` has the wrapper. Never `process launch` through `lldb-mcp`: it never returns and blocks that lldb until it's killed. Start the host yourself and attach.
- `explore.py` uses only steps without assertions; `open_pause` / `close_pause` are assertions and go red on their own.
- Replay needs the town spawn moved as when recording (`replay.py` sets `DM_TOWNWP_OFF=0`) and no tick-0 join lines in the packet file. Check with a positive control (drop input, expect a divergence) before trusting an "identical" result.

**CLI host (Mac):**

```bash
make -C dm/host && cd dm/host
DM_ROMFS=<Update/RomFS> DM_SAVE=<dir> DM_TEMP=<dir> DM_MSL_DIR=$DM_ROOT/build/cache/metal_shader_cache DM_WINDOW=1 \
  ./dmhost "$(../tools/product.sh path touch)" main sdk
```

- Omit `DM_WINDOW` for a headless run.
- `DM_WATCH=<s>` sets a watchdog.
- `DM_SVC=1`, `DM_IMPORTS=1`, `DM_DBG=1` turn on extra logging.

## Game reference (the decompiled game code)

There is no recording of the original game on a console or emulator; to learn what the game does, read its code. The reference is a private Ghidra project of `main` built by `dm/tools/re` (`dm/tools/re/README.md`) and kept in `$DM_ROOT/re`.

- **Static questions** (what a function does, who calls it, where a field is written): `dm/tools/re/re.sh serve` (the only process that may hold the project; one Ghidra process at a time, never next to a game run or an Xcode build), then `re.sh q decompile|xrefs|callgraph|strings|find|bytes|name|comment main+0x…`, or the `ghidra` MCP server in a main-checkout session. Project addresses are `0x7100000000 + offset`, so a `FUN_71…` name ending in `005d3bc0` is `main+0x5d3bc0`. Area notes live in `$DM_ROOT/re/notes`.
- **Dynamic questions** (what a value is at run time): the headless host under lldb (`dm/tools/lldb/dm.py`, `DM_BREAK_AT_TICK`) or a `DM_PROBE` scenario (a path counts as not taken only with the probe's install line, `PROBE_SUMMARY <name> hits=0` and a `PROBE_SUMMARY end … why=` that matches how the run should end; anything less is unknown; `dm/verify/debugging.md`), with the result checked against the reading of the code.
- **Keeping it current:** `re.sh seed` writes the repo's own names and comments into the project, `re.sh export` (server stopped) writes `$DM_ROOT/re/export/main.names.tsv`, which `guestsyms.py`, `crashlog.py`, `dm.py`, `bench.py profile` and `addrcheck.py` read. `re.sh tables` / `reflect` / `autoname` fill in data tables, reflection and bulk names.
- **Evidence kinds:** private notes tag each load-bearing claim `[static]` (read in code or data), `[derived: tool]`, `[probed: scenario, build]`, `[observed-mac]` or `[observed-iphone]`, separately from confidence. A static reading never closes a runtime claim and a Mac run never closes an iPhone one. A negative claim ("no callers", "never taken") states what was searched; outside that scope it is unknown. Open questions live in `$DM_ROOT/re/notes/unknowns.tsv` (the roadmap's unknowns list carries the game-free ones that block work).
- **`re.sh profile`** (also in `re.sh status`): which game binary, inputs and tool revisions produced the export, and what to rerun when they changed. Each step (import, analyze, reflect, tables, autoname, seed) stamps what it used as it runs (`log/stamps.json`); `re.sh export` turns the stamps into `export/profile.json`. In the export, bulk names from `autoname.py` keep their own confidence and an `auto:<rule>` source, and the sixth column is the evidence kind.
- **`re.sh addrcheck`** (also the gate's `quick_readdr` step): every `main+0x…` cited in `dm/host`, `dm/gfx`, `dm/ios/Sources`, `dm/tools` and `dm/verify` must land in a known function (or be a known name), 4-byte aligned; data addresses are only listed. Test files and lines giving a usage example (`e.g.`, `example`) are skipped. Without the symbol table or the export it reports unavailable.
- **Never copy its output anywhere public or shared:** decompiled code, disassembly, strings and database files stay out of git, issues, PRs, artifacts and memory. Code and commits cite a game function by its `main+0x…` address and its role, in our own words. The tracked pre-commit guard (`dm/tools/git-hooks/pre-commit`, linked by `./setup.sh`) refuses database files and lines that look like decompiler output.

## Architecture (the pipeline)

**A. Offline rewrite: `dm/tools/patch_nso.py` (driven by `product.sh`).**
- **Input:** the three NSO modules `main`, `sdk`, `subsdk0`.
- **What it rewrites:**
  - `x18`, `tpidr_el0` and `tpidrro_el0` access → thread slots.
  - `svc #n` and `cntpct_el0` reads → jumps to **host gates**: fixed patch blocks that branch into host C, with the target address read from a thread slot.
  - Slot numbers live in a read-only "key page" appended to the image, with the patch area after it.
  - Nothing is written as code at run time (iOS forbids that).
- **Rule tables keyed by the main NSO build ID (`db0507f3…`):**
  - `CUTS`: byte patches; the original bytes are checked before patching.
  - `OBS`: observation gates; numbers must match the `OBS_*` enum in `dm/host/dm.h`.
  - `PARAMS`: host-filled values.
  - `SWITCHES`: conditional branches also taken when a host-filled slot is non-zero (environment shadows; checked by `structck.py`).
- **Products** go to `$DM_ROOT/build/cache/dm/out/<dm/PRODUCT>-<kind>` (currently `3.3.94012-r47-touch`):
  - `touch` is what the apps run.
  - `keypage` and `rules` are used by the verifiers.

**B. Host: `dm/host`.**
- `loader.c` maps the patched images RX and registers gates from `<module>.gates.tsv`.
- `main.c` runs SDK init, then `init_array`, then `nnMain`. On iOS it is compiled as `dm_main`.
- Each guest thread is a pthread set up by `guest_thread_init` (`thread.c`).
- Gates go `dm_gate` (`entry.S`) → `gate_dispatch` (`hle.c`) → `svc_dispatch` (`hle_svc.c`, `hle_thread.c`, …).
- Imports resolve one of three ways:
  - libc pass-through (`hle_table.c`)
  - named HLE take-overs in `hle_*.c` (`HLE`/`HLE_FORCE` in `hle.h`; each needs a reason)
  - a logging stub, `UNIMPL <name>`
- **Game addresses** are offsets into the original 3.3.94012 `main` image, reached as `main_module()->base + 0x…`. Comments write them as `main+0x…`.

**C. Graphics: `dm/gfx`.**
- **The NVN API on Metal:** the game's NVN calls are reimplemented on Metal; the NVN driver never runs.
  - `nvn/hle_nvn.c` records calls into the game's own NVN object memory.
  - `nvn/nvn_mtl.m` replays them as Metal at submit.
  - Memory pools are zero-copy `MTLBuffer`s (GPU address = CPU address).
  - Textures are deswizzled in `gfx/tex`.
  - The thunks and prototypes are generated (`nvn_slots.py`, `gen_protos.py`).
- **Shaders are translated offline** (`gfx/tools/dmsh_build.py`, run by `setup.sh`):
  - Chain: `dmsh_xlate` (C#, bundled Ryujinx translator) → SPIR-V → `spirv-cross` → MSL → metallib.
  - Each shader is keyed by the first 16 hex digits of the SHA-256 of its control section.
  - Libraries live under `b-<V>/`, where V is the chain version hashed from chain sources + `dmsh_chain.lock`. The host refuses a version mismatch.
- **Metal 4 only, macOS / iOS 27 and later (decided 2026-10-27).** The fork targets macOS 27 / iOS 27 (`MACOS_MIN`, `project.yml`) and uses Metal 4 and what those systems add, with no older-system fallbacks; the older-API backend was removed on 2026-10-28. The encoder macros at the top of `nvn_mtl.m` (`RENC`, `CENC`, `FB_ON`, …) name the Metal 4 types.

**D. Frontends and features.**
- **`dm/host/front.h` is implemented by:**
  - `front_mac.m` + `app_mac.m` (Mac App; `touch_mac.c` for scripted touch)
  - `dm/ios/Sources/front_ios.m` (iOS)
- **Touch flow:**
  1. `Sources/dm_touch_controls.c` (pure C) classifies the touch.
  2. Touch, cursor and input state are published.
  3. The host applies them when the game reads input (`hle_hid.c`, `dm_cursor.c`).
- **Features are `dm/host/dm_*.c`:** world taps, padmode, custom settings, the iCloud merge, and so on.
- **The romfs overlay** (`ios/tools/gen_romfs_overlay.py`) stores only diffs: layout patches and string-table entries by key. The host checks it before the game's RomFS.
- **iOS packaging:**
  - `gen_embed.py` embeds the patched images.
  - `gen_build.py` takes the host source list from `dm/host/Makefile`.
  - XcodeGen regenerates `DmIOS.xcodeproj` and `Gen/` on every build. Never edit them.

## Gotchas

- **New host files:** `dm/host/Makefile` lists `dm_*.c` and the other non-`hle_` sources **explicitly** in `SRC`, alphabetically. Add new files there. Only `hle_*.c` and the gfx dirs are picked up by wildcard.
  - The host also compiles `dm/ios/Sources/dm_touch_controls.c`, `cloud_ios.m` and `dm_gamepad.m`, so edits there affect the Mac build too.
- **Feature switches:**
  - Read them only through `dm_custom_get("<key>")`.
  - The key order in `dm_custom.c` `k_keys` must match `touch_layout.py _CUSTOM_ROWS`, the full list (`test_touch_layout_options.py` checks it). A page may leave rows out: Extras (Mac and iOS) drops the two the Video tab shows; the default overlay's single Custom page (CLI host, `touchchain.sh`) keeps them.
- **Mode and UI rules:**
  - Touch-only or controller-only behaviour needs a row in `dm/host/dm_mode_items.h` (checked by `mode_scan.py`).
  - Writing to the original game's UI widgets is forbidden (`native_write_scan.py`).
- **Failure markers:**
  - `【红】` in a log means failure; the full regex is in `dm/verify/logred.sh`.
  - Script errors start with `🔴`.
  - Tool verdict lines end with `OK` / `RED` / `FAIL` / `红`.
- **Log locations:**
  - Mac App: `~/Library/Logs/D2R Mobile/host.log`
  - iOS: `Documents/boot.txt`
  - iOS build: `$DM_ROOT/run/ios/build.log`
- **Failed save writes:** the game's save job used to skip its own unmount after a failed open / create / truncate, so no later Commit ever happened (silent progress loss on a full disk). The host-only cut at `main+0xe0c04c` (`patch_nso.py`) fixes it; `DM_TEST_FS_FAILAT=guestopen` fails the game's open for writing once, and `dm/verify/scenarios/save_open_fails.scn` checks that later saves still commit.
- **Overlay messages:** a layout's message strings are hashed at load through the game's own channel / action name lists; an unknown category or action hashes to 0, and a button sends its click message only when both hashes are non-zero, so a click message with an unknown name silently never fires (`dm/verify/ui_message_stream.md`).
- **Overlay files:** `casc_overlay.py` can replace any file but can add new ones only in a new folder directly under an existing directory (first character unlike every item there). Layouts the game should find in both input modes go in a new folder under the layout root, which both modes fall back to (see the Extras page).
- **Weak symbols on Apple:** a `__attribute__((weak))` declaration of a function no target defines still fails to link on iOS. Give a weak default definition instead (as `dm_ctl_cmds.c` does for the Mac-only `dm_wheel_ui`).
- **A stuck save job crashes the game:** save jobs run on the game's async job pool; with one job blocked about 8 s, the tasks queued every frame exhaust the pool's task allocator and the pool's enqueue function (main+0x14f0590) writes through NULL. Never hold the save path (host hooks, locks, slow I/O) for seconds; keep `fault delay` under ~5 s.
- **Don't edit a shell script while it runs** (`fork_gate.sh`, `touchchain.sh`, `re.sh`): bash reads scripts incrementally, so the running copy executes shifted text and fails with a syntax error.
- **Test suites under `dmtest.py`:** their negative cases print `RED` verdict lines; wrap suites with `--rc-only` (the fork gate does for `unit_*`).
- **Flaky test:** `t_svc_mem` det5 is a timing test. It has failed inside the full host suite under load and then passed alone; rerun it before suspecting a change.
- **Regenerate after editing:**
  - `patch_nso.py` or `dm/PRODUCT` → `dm/tools/product.sh build`
  - shader-chain sources or tools → `./setup.sh`, which re-registers the chain and re-translates
  - NVN names or prototypes → `gfx/tools/nvn_slots.py` + `gen_protos.py`
  - `pso_manifest.tsv` row order matters; don't sort it.
- **Mac App:**
  - It links to build outputs under `$DM_ROOT`; the bundle ID is `org.d2rmobile.mac`.
  - Its resource folder is stored as the `RomfsDir` default.
  - The pointer auto-hide delay is `CursorHideSeconds`.
  - Picture settings (Display Mode, Resolution incl. Native, Color, Graphic Presets, 3D Resolution, Upscaling, Anti-Aliasing, Anisotropic Filtering, Environment Shadows) are on the Mac Options Video tab: layout `touch_layout.py` `_VIDEO` (`DM_OVERLAY_MAC`), logic `dm_video.c`, stored in `sw_video.txt` beside `sw_custom.txt` with a first-boot "recommended" copy (`front_mac.m` `front_video_firstboot`; the older `StartFullScreen` / `RenderScale` / `MetalFXUpscale` defaults only seed it once). Keys: `mode res up aa aniso color s3d envsh` (value 0 = the original look; new keys go at the end; `k_rows` must follow the page order). Keys after the first three get their recommendation from `front_video_rec` (Mac App and iOS; the CLI host and Mac test copies get 0, so baselines don't move). `rec_v` versions the recommendation: an older file gets the new one, and a player still on the old Recommended moves with it. AA writes the renderer's mode byte (`hle_oe.c` `aa_tick`, `DM_AA_MODE`; `DM_AA_MODE=off` leaves the byte alone for a test's control-port `mem` write); filtering is the sampler override (`nvn_mtl.m` `aniso_latch`, `DM_NVN_ANISO`); Color tags the layer sRGB (`front_mac.m` `layer_color`, `DM_COLORSPACE`). Selectors are native `CarouselWidget`s: read `+0x7ec`, set through `main+0xab0c84`; their update message must not be `PanelManager:ClosePanel:<name>` (the selector replaces the third part, and a nameless ClosePanel crashes).
  - The game's own 3D upscale pass (`ps_upsample_p`, fragment key `52aac1f2306b8fc5`: bilinear + 5-tap sharpen + an R8 mask) is replaced by MetalFX when Upscaling is on (`nvn_mtl.m` `mfx3d_draw`; `DM_MFX3D=0/1` forces it). It only acts below 100% 3D scale: on phones (3D 0.75) it is the main sharpness gain, and on the Mac it works with Native output and a 3D Resolution below 100% (`s3d`, through `dm_video_scale3d` → `hle_oe.c` `s3_cfg`), so the interface renders at the screen's resolution and only the world is upscaled.
  - iOS gets the same Video tab with a Graphics section (overlay flag `DM_OVERLAY_IOS`, set only by `dm/ios/run.sh`): 3D Resolution 75 / 85 / 100% (live through `hle_oe.c` `s3_cfg`, which re-reads the setting each tick), Graphic Presets, Upscaling, plus Color / Anti-Aliasing / Anisotropic Filtering (same keys as the Mac; `front_ios.m` `layer_color_ios` tags the present layer). First boot (`front_ios.m` `front_video_firstboot`) = 75% + MetalFX; `front_video_rec` adds SMAA, 16x and Accurate colour (`rec_v` 3, measured on an iPhone 17 Pro: 60 fps, frame pacing and CPU unchanged).
  - Phone test runs (`DM_SAVE_FRESH` / `DM_SAVE_TEST`) keep their settings in `Documents/settings.test` (`main.m`), fresh each `DM_SAVE_FRESH` run, so automated taps on Options never change the player's `sw_video.txt` / `sw_custom.txt`.
  - `DM_MFX=0/1` forces MetalFX for tests and logs present-pass GPU time; `DM_GAME_RES` still overrides the render size. A test copy (`DM_APP_SUPPORT`) starts from the original settings unless `DM_TEST_VIDEO_FIRSTBOOT=1`.
  - Environment Shadows (`envsh`, Mac only, on in Recommended from `rec_v` 4): on the Switch nearly every non-character model is kept out of the shadow casters by its platform shadow tier (`Model_ApplyPlatformTiers` main+0x15ce950). The rewriter's `SWITCHES` entry also takes that test's "may cast shadows" branch when the `KESHADOW` thread slot is non-zero; every guest thread gets the slot from `dm_video_env_shadows` at its start (`thread.c`), so the value is latched at launch and a change shows the restart hint. Measured on an M6 at Recommended: +0.5 ms GPU in town, still 60 fps. `DM_ENV_SHADOWS=0/1` forces it.
- **Text input (swkbd → `front_text_input`):** with a controller in use (`dm_gamepad_mode()` and the latest glyphs aren't the keyboard's), an on-screen keyboard drawn over the picture (`dm/ios/Sources/dm_vkbd.c` model + `t_vkbd`, `dm_vkbd_ui.m` drawing in a CALayer with the game's Exocet / Formal436 / BlizzardGlobal fonts read from the RomFS); otherwise the Mac sheet / iOS alert. Keyboard, mouse or touch while it is up hands over to the box, and a controller press while the box is up hands back, keeping the text. `DM_VKBD=1/0` forces it; `DM_TEST_SWKBD_KIND=num:<max>|text` turns the naming box into the other layouts for screenshots. Its layer isn't in the presented frame: `DM_TEST_PRESENT_SHOT` composites it.
- **Options page (Mac, iOS):**
  - Tabs: Video | Audio | Gameplay | Settings | Extras (`touch_layout.py` `_tabs5`). Video = the borrowed `VideoOptionsPanel` with only the picture rows; Extras = our own page `2extras_lay/ExtrasOptionsPanel` (`_EXTRAS_PANEL`, class `VideoOptionsPanel`), in a new folder under the layout root because `casc_overlay.py` can only add a new subdirectory under an existing directory, and both input modes fall back to the root. The default overlay (no `DM_OVERLAY_MAC` / `_IOS`) keeps one Custom page.
  - Don't open a page by a case-variant of an existing name (e.g. `videooptionspanel`): the game creates a second instance that `ClosePanel` never closes, and two pages draw on top of each other. Other candidate pages crash (`LootFilterOptionsPanel`) or are something else (`OnlineGameSettingsPanel` = the online Game Creation window).
  - The host (`dm_custom.c`) runs its page code on whichever of the two pages is on screen; `dm_custom_shown()` tells "Default" which rows that page holds, so it resets only that tab. The controller selection follows the selected row only right after it changes (the Mac is always in controller mode, and following every frame undid each wheel scroll).
  - Frame Rate and Frame Drops (Mac and iOS) are selectors at the end of the Video tab's Graphics section (`dm_video.c` rows `fps` / `drops`), but their values are the custom keys `eco_fps` / `q3d` in `sw_custom.txt`: pending / Apply / Discard go through `dm_custom.c` (`dm_custom_shown_value`, `dm_custom_set_pending`), and the Video tab's Default resets them. The Extras layout leaves those two rows out (`_VIDEO_TAB_KEYS`); the default overlay's single page keeps them. `dm_custom.c` walks and resets only the custom rows the page on screen has (`ck[]`, `g_page_keys`).
  - Controller highlight on Video / Extras: the game's own hover image on the selected control, as on the other tabs (`TouchCustomHiCar` = `Carousel_Hover`, `TouchCustomHiBox` = `CheckBox_Hover`, native size, centered). Radio rows light the chosen option; left / right step the language row. The host hides it while its item isn't wholly inside the list box. Keep list content right of the text edge: the scroll area clips at the panel's left edge, and a nine-tile's corners escape that clip (the old whole-row gold frame lost its left edge and left corners hanging over the tabs).
  - The Mac App always runs the controller layouts, with the mouse on top as touch gestures. Its Options layouts take the touch row pitch (180), clickable Apply / Default / Discard and an off-screen key legend (`PAD_SETTINGS_MAC`, `pad_settings_legend`).
  - Mac mouse paths (`touch_mac.c`): in game, presses go through the touch state machine; in the front end (`!in`) they go to `dm_ui_touch` as clicks, except that a vertical drag past 10 points scrolls and a sideways drag starting on a slider (`dm_ui_slider_at`) sets it. A press on a scroll bar (`dm_ui_scrollbar_at`) is never a click: it drags the thumb (`ui_drag_pump` kind 4, steering the list with wheel steps until `Indicator.y` sits under the pointer). The wheel sends the game's `MouseScroll` at the pointer (`dm_wheel_ui`); in game with no panel it still pushes the right stick.
- **iOS:**
  - Reinstall over the app; deleting it wipes the 5.6 GB of resources and the saves.
  - The 18 resource archives can be pushed with `xcrun devicectl device copy to … --domain-type appDataContainer --destination 'Library/Application Support/romfs'`.
  - `run.sh install` passes a battery gate meant for test phones; `DM_DEVBATTERY=0` turns it off (set in `dm/ios/local.env`).
