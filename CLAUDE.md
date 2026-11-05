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
- **Releases and the launcher:** players install the Mac App with D2R Metal Launcher (`dm/launcher`, engine `dm/tools/install.py`, `docs/LAUNCHER.md`) from the GitHub releases; 1.0.0 (2026-10-31) was the first.
  - `dm/tools/release_mac.sh <version> --notes <file>` builds, signs, notarizes and uploads a draft from a HEAD on `origin/main` (needs the Developer ID identity, the `d2r-notary` notarytool profile and the release key in the keychain); publish it, then `release_mac.sh --check-latest`. Run `--no-upload` first and `dm/verify/launcher_update.py` / `launcher_selfupdate.py` against it.
  - The launcher and the in-game notice read GitHub's latest release, so the releases page holds only D2R Metal releases.
  - A release changes the players' installs: build it only when the user asks.
- **Brand art:** the logo (the Runestone emblem and wordmark), the app icon and the Cinzel font live in `dm/brand` (README there): `icon.sh` puts the icon into the game app, the launcher and the release pack; `out/icon-flat.png` is the iPhone icon.
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
    - `dm/ios/local.env` and the host header generated from the RomFS (`dm/host/dm_hud_exp.h`) are gitignored. Copy them, or regenerate the header with `make -C dm/host <header> ROMFS=$DM_ROOT/data/switch/3.3.94012/Update/RomFS`.
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
   - The one exception: the README's pictures in `docs/images` (screenshots of the game running in D2R Metal, and the project's original art), JPEG / PNG under 2 MB, used by the README only and never by the build or a release. The releases page holds only D2R Metal releases; no images go there.
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

- **Keep the workspace small** (the user asked on 2026-11-01): `python3 -I dm/tools/workspace_prune.py` lists what is safe to remove from `$DM_ROOT` (older shader-library versions and rewrite products, test runs older than 7 days, bench baselines older than 7 days other than the current one, older local releases, Mac App test copies older than a day); `--apply` moves it to the Trash. Under `run/` it goes by rules, not a list: only folders under `run/<area>/` are run outputs (plain files there, such as `run/bench/ledger.tsv`, are never listed), nothing named `*.lock` or `*.queue` is listed (the game and device locks and their queues), a `.keep` file protects its area or entry (`bench.py --keep` writes one), and a `latest` link and its target stay. Run it after a shader-chain or product change and after long investigations, and delete scratch outputs (captures, probe overlays) when done with them.
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
python3 dm/verify/golden.py shots check --set orig,conv,rect,rec75,rec75t,phone       # the FLIP picture sets (record one with shots record --set S --reason …)
```

- The protocol is in `dm/verify/control_port.md` and the scenario format in `dm/verify/scenarios/README.md`.
- On the Mac the host connects out to the controller (`DM_CTL_PORT`), because `ios/run.sh` allows only one `listen(` (the iOS log port). On iOS, commands go over `DM_LOG_PORT`.
- `mac-app` runs a test copy (`$DM_ROOT/out/macapp_test`) with `DM_APP_SUPPORT` set. Never point scenarios at the player's real save folder.
- `ios-mac` needs `.iosdd_mac/DmMacSmoke.app`; `dm/ios/tools/smoke_mac.sh 1` builds it. Saves for the iOS backends are the ones bundled at build time.
- Generated saves (`export_save`, `scenarios/saves/*.scn`) stay in `$DM_ROOT/data/switch/save_ref_<name>/` and are never committed.
- **Pictures for tests (`dm/tools/picture.py`):** `REC` (this Mac's Recommended: Native 2560 × 1440, 3D 100%, MetalFX Temporal, Frame Generation, 16x, Accurate, environment shadows, Ultra), `RECT` (the same at real frames), `CONV` (a chip without them: MetalFX's spatial scaler, SMAA), `REC75` (`CONV` with the 3D at 75% and the MetalFX 3D pass), `REC75T` (`REC75` with the temporal scaler, `DM_MFXT=1`, and the game's AA off) and `PHONE` (the iPhone 17 Pro's picture, drawn by the Mac host) pin every Video-tab key through its forcing switch, with Frame Drops off (a 3D scale that moves can have no picture baseline). `bench.py --picture`, the golden sets and the picture scenarios build on them. `test_picture.py` (host suite) ties them to `t_picrec_cases.h` and `front_video_firstboot` / `front_video_rec`: a change to Recommended updates `picture.py` in the same commit.
- **Golden sets:** `golden.py shots … --set` takes `pages` (the default: the shotgate pages, SSIM) or one of the six FLIP sets `orig`, `conv`, `rect`, `rec75`, `rec75t` and `phone`. Frame generation changes nothing the game draws, so `rect` judges Recommended's frames and `dm/verify/motion_gate.py` the ones in between. A FLIP set shoots each page twice to record it (it must be frame-stable, with a clean log), judges it per region (a look-set page as its 3D view and HUD band, a shotgate page as the whole frame), and runs controls that turn one setting back off and must read RED (`--allow-blind control=why` takes a measured reason). Until recorded they are ⚪.
- **Log checks and full-screen geometry:** `check log_has <regex> [file=…] [min=N] [last=<regex>]`; with `last=` only the last line matching it is judged, so a final state is checked rather than an earlier one. Judge geometry by the `layout` event (`fe "0 layout"`, then `expect layout …`: the window's layout and the last live present's viewport, drawable and crop); `DM_TEST_PRESENT_SHOT=<s>:{run}/present.png` grabs the next live present as a picture to look at (such a run goes in front).
- **Engine-aware state (`dm/host/dm_engine.c`):** `state` carries `panels` (visible, top first), `focus` / `focus_panel`, `cursor_mode`, `save_busy` / `save_pending` / `save_mounted`, and from the tick thread (`dm_hud`) `gold` / `gold_stash`, `items` / `item_dups` (repeated item ids, must be 0) and `monsters` (live, NPCs included); `engine` gives the whole snapshot plus the last actions and UI messages (the runner saves it as `engine.json` on the first red). Events `action` (controller actions under the game's own names), `focus`, `save_request`, `save_stuck` (red), `save_alert`, `fault`, `padmode`, `opmode`, `language`. Checks: `expect_state panels~=X`, `check actions seq=…`, `check save_pairs`, `check saves_valid` (`dm/verify/d2save.py`; every cli / mac-app run checks its save).
- **Faults and front-end input:** `fault arm <point> skip=N count=N` / `fault delay ms=N` (`DM_TEST_FS_FAILAT=<point>[@skip][:count]`, `DM_TEST_SAVE_DELAY`); `fe "<s> pad A"` runs one `DM_TEST_NSMOUSE` line through the Mac front end (virtual controller, on-screen keyboard, sheet), and its window test hooks too: `scale <f>|0` (a backing-scale change through `windowDidChangeBackingProperties:`, as a move to another display), `layout` (a `layout` event now), `fs 0|1` (full screen through the Options page's path), `menukey <char> [cmd,ctrl,opt,shift]` (a key equivalent through the main menu), `screen <w>x<h>|off` (the window's screen counts as that size; a `screen` event follows), `uiscr` (the `uiscreen` events now) and `pshot <path>` (one grab of the next live present); `inputmode mode=touch|controller glyph=…` (needs `env DM_ROMFS_OVERLAY=auto` on cli). Every fault / switch scenario must require the event that proves it happened.
- **Pointer steps and their paths:**
  - `tap` goes straight to `dm_ui_touch`. It skips the Mac mouse path, and the game ignores plain clicks on console tabs, so switch tabs with `press ZR` / `ZL`.
  - On the Mac App, `drag x0 y0 x1 y1` goes through the real mouse's gesture path (`dm_touch_host_live`); a zero-length `drag` is a mouse click. `wheel x y dy [precise] [view]` calls the same `dm_wheel_ui` as the App's scroll events.
  - Options opened with `msg PanelManager:OpenPanel:SettingsPanel` in game don't count as a menu for the mouse code; open them as a player does, with `open_pause` then `tap_ui Settings`.
  - Real mouse events for any Mac App build, including ones without a control port: `DM_TEST_NSMOUSE=<file>` (`down` / `dragto` / `hover` / `key` lines), `DM_TEST_PRESENT_SHOT=<s>:<png>`, `DM_TEST_QUIT_AT=<s>`. Leave `DM_WATCH` unset there: its stall watchdog kills the front end after 15 s.
  - Every `fe` line works in background runs (no key window needed; `key` / `type` go to the window that would be key, the attached sheet or else the game window): `nav <keycode>` (a key straight to the game's key handler), `pad <buttons>` / `pads b1,b2,…` (virtual controller presses through `dm_gamepad`, self-timed; one scheduled line per press gets merged), `navpad <buttons>` (skipped while a text box is open, so extra presses to reach a page are harmless), `skey <keycode> <chars>` (through `[NSApp sendEvent:]` and the event monitors), `sheet ok|cancel [text]` (fill and press the Mac text sheet). `DM_TEST_PAD_GLYPH=2|3|0` makes the virtual controller count as Xbox / PlayStation / Switch for the key caps.
- **Windowed scenario runs are background runs** (`scenario.py` sets `DM_TEST_BGRUN=1` for windowed `cli` and `mac-app`): no focus taken, no Space switched, so they can run while the user works. A scenario that must be seen runs in front with a `front <why>` header (or `--front`); front runs are judged for disturbances (`desk` / `app` events), redone (`--redo`, default 2) and otherwise reported not judged (⚪, exit 3). Benchmarks (`bench.py`) and `ios-mac` runs (the iOS package takes the front even when launched with `open -g`) always run in front.
- **Background test runs (`DM_TEST_BGRUN=1`, `front_mac.m` / `app_mac.m`):** the window is borderless, can never become key, never asks for activation and sits outside every screen at the size it would have on screen (windowed: what AppKit gives the titled window; full screen: the screen's full-screen content area, emulated, no Space switch). Launch focus, sheets, minimize / restore and `DM_TEST_COVER` are emulated through the same handler bodies (`app … emu=true`); the display link keeps ticking off screen. Unset = the product, `0` = a front test run. Events `window` and `desk` (event_stream.md) show the geometry and whether the run took the front.
- `ios-mac` keeps running the package `smoke_mac.sh 1` last built, not the `run.sh build` output: rebuild it after layout or host changes.
- **A phone on Wi-Fi only:** devicectl paths work (`lifecycle.sh`, `touch_e2e.sh` / `uireg … run_dev.sh`, `heat_ab.sh`, file pulls). `smoke.sh` and the scenario `device` backend need a USB cable, because the app's log port (`DM_LOG_PORT`) listens on the phone's loopback, which `iproxy -n` can't reach ("Connection refused"). `ideviceinfo` can't read the battery over Wi-Fi either: `DM_DEVBATTERY_INRUN=1` lets the battery gate use the app's own reading.

**Performance:**

```bash
python3 dm/tools/bench.py run dm/verify/bench/town_loop.scn --reps 3 [--mpt] [--picture rec|orig|rec75|phone]   # per-segment CPU / NVN / GPU / present time → bench.json
python3 dm/tools/bench.py ab dm/verify/bench/town_loop.scn --reps 6 --a-env … --b-env … [--metric nvn_ms.p50]   # ABBA, Mann-Whitney + bootstrap, ledger
python3 dm/tools/gpucap.py capture|summary|listen|hud …                            # gpucapture / gpudebug / metalperftrace
python3 dm/tools/boothist.py show --label bench:cli                                # boot-stage history across revisions
python3 dm/tools/bench.py profile dm/verify/bench/combat.scn                         # sampled per-thread-role attribution with engine names + hitches over 25 ms
python3 dm/tools/bench.py baseline save && python3 dm/tools/bench.py regress <route>  # ABBA against a kept host; red only on a slowdown still significant after Holm
```

- `run`, `ab`, `profile` and `regress` default to `--picture rec`: this Mac's Recommended in full screen (`dm/tools/picture.py`; on `cli` every picture key pinned plus `DM_TEST_FULLSCREEN_AT=1`, on `mac-app` `DM_TEST_VIDEO_FIRSTBOOT=1`). `--picture orig` is the host's own defaults in a window, as runs were before; results from before this default don't compare with it. The picture is recorded in the results and the ledger's route column (`town_loop@rec`).
- Present intervals are pinned at 16.67 ms by vsync on this Mac; compare `cpu_ms` and `nvn_ms` instead. `gpu_ms` is clock-scaled on a fast Mac (the GPU lowers its clock when it has room), so it is not in `regress`'s defaults (`cpu_ms.p50,nvn_ms.p50,proc_cpu_ms.p50`); A/B it (`--metric gpu_ms.p50`) only for a GPU-bound change, and read it as a relative size.
- Use at least 4 runs per variant (the minimum exact p is 0.029); 6 is the default.
- `regress` applies Holm's correction over every segment × metric of the route, and its runs guard refuses a `--reps` too small to ever reach significance before any run (red, exit 2; the defaults on `town_loop` need 5). A segment × metric left without a p (too few runs with a value), or a rep still disturbed after its redos, makes it ⚪ (exit 3), never OK.
- `baseline save` keeps the host with what it runs on: its tables, the rewrite output and the shader library (APFS clones) in `$DM_ROOT/run/bench/baseline/<rev>`, and `current` points at it; `regress` runs the kept host on its own copies (`SCN_PRODUCT`, `DM_MSL_DIR`). A baseline kept before snapshots existed is ⚪ until it is saved again.
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
  - `PARAMS`: host-filled values (a movz / movk pair, or since r48 a single movz such as the shadow map edge, `KSHSIZE`).
  - `SWITCHES`: conditional branches also taken when a host-filled slot is non-zero (environment shadows), and csel instructions whose destination takes the slot's value when it is non-zero (camera jitter, since r50); checked by `structck.py`.
- **Products** go to `$DM_ROOT/build/cache/dm/out/<dm/PRODUCT>-<kind>` (currently `3.3.94012-r50-touch`):
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
  - A new picture feature (a pass, a setting, a forced switch) builds pipelines the warm-up seed lacks: re-record it with `python3 dm/tools/pso_seed.py record` and `merge --dir <record folder> --write`, or the gate's pso steps (`core_pso_cli`, `full_pso_mac`, `full_pso_ios`: `pso_seed.py gate`, `PSO_GATE OK rows=0`) go red. A row that belongs in no seed goes in `dm/gfx/nvn/pso_allow.tsv` only after review, with a `# why` line above it.
- **Mac App:**
  - It links to build outputs under `$DM_ROOT`; the bundle ID is `org.d2rmobile.mac`.
  - Its resource folder is stored as the `RomfsDir` default.
  - The pointer auto-hide delay is `CursorHideSeconds`.
  - Picture settings (Display Mode, Resolution incl. Native, Color, Graphic Presets, 3D Resolution, Upscaling, Anti-Aliasing, Anisotropic Filtering, Environment Shadows, Shadow Quality, Frame Generation) are on the Mac Options Video tab: layout `touch_layout.py` `_VIDEO` (`DM_OVERLAY_MAC`), logic `dm_video.c`, stored in `sw_video.txt` beside `sw_custom.txt` with a first-boot "recommended" copy (`front_mac.m` `front_video_firstboot`; the older `StartFullScreen` / `RenderScale` / `MetalFXUpscale` defaults only seed it once). Keys: `mode res up aa aniso color s3d envsh shq` (value 0 = the original look; new keys go at the end; `k_rows` must follow the page order). Keys after the first three get their recommendation from `front_video_rec` (Mac App and iOS); the CLI host and Mac test copies without `DM_TEST_VIDEO_FIRSTBOOT=1` get 0 for every key, the first three included (`dm_picrec_real`; CLI runs that want MetalFX set `DM_MFX` / `DM_MFX3D`), so baselines don't move. The Mac App's Recommended comes from `dm_picrec.c` (`rec_v` 8; cases and invariants in `t_picrec`): the chip's generation and tier from the GPU's name, its GPU core count (IORegistry `gpu-core-count`), the in-game picture at Native (`native_picture`), its full-screen viewport (past the `DM_SCR_MAX_PX` cap the present's enlargement is counted) and the frame rate on the main screen (`dm_pace_hz`: the load scales by rate / 60, so 72 fps on 144 Hz is heavier); memory, the model, the panel, the thermal state and Low Power Mode are read and logged, not used. Speed = cores × per-generation Geekbench Metal points per core against the M6, capacity anchored on the M6 at Native 3840 x 2160 / 3D 100% (60 fps; the only measured machine) and on base chips' former 75%. It picks Native with the full picture down to 75%, else the light one (no environment shadows, Standard map) down to 67%, else a fixed width at 100% enlarged by MetalFX; an iPhone chip keeps the phone's shadows; Frame Drops' default (`front_q3d_default`) is Lower with the light picture or above 85% of the capacity (a fixed width's enlargement into the screen counted). The Mac GPU lowers its clock when it has room, so `gpu_ms` cannot rank picture costs on a fast Mac. `DM_TEST_GPU_NAME` / `DM_TEST_GPU_CORES` stand in for the GPU (with `DM_TEST_VIDEO_FIRSTBOOT=1` on a test copy). `rec_v` versions the recommendation and `rec_id` (`dm_picrec_id`) names the machine it was worked out for: at launch an older `rec_v` or another `rec_id` (GPU, Native size, the present's enlargement, a frame rate other than 60) works it out again, and a player whose graphics keys still equal the old Recommended moves with it (event `picture`, `how` `update` / `rescreen`). Display Mode and the Mac's Color are display preferences (`pref_key`): first launch and Default set them, but they never decide whether the picture is on Recommended and the Graphic Presets row leaves them alone (on iOS Color stays part of Recommended). `DM_TEST_SCREEN_PX`, `DM_TEST_DRAWABLE_PX` and `DM_TEST_SCREEN_HZ` stand in for the screen; `video_presets_mac.scn` drives the page. Player docs copy the model: docs/MAC.md (Frame Drops, Graphic Presets and 'What Recommended picks', rows from `t_picrec_cases.h`, which `t_picrec` checks) and README 'Picture settings'; change them in the same commit as the model or its cases. AA writes the renderer's mode byte (`hle_oe.c` `aa_tick`; `DM_AA_MODE=0`, `1` or `2` forces none / FXAA / SMAA, `off` leaves the byte alone for a test's control-port `mem` write, and 3-5 crash the game (SMAA T2x needs a velocity target the console build never creates), so they are refused with a red line); filtering is the sampler override (`nvn_mtl.m` `aniso_latch`, `DM_NVN_ANISO`); Color tags the layer sRGB (`front_mac.m` `layer_color`, `DM_COLORSPACE`). Selectors are native `CarouselWidget`s: read `+0x7ec`, set through `main+0xab0c84`; their update message must not be `PanelManager:ClosePanel:<name>` (the selector replaces the third part, and a nameless ClosePanel crashes).
  - The game's own 3D upscale pass (`ps_upsample_p`, fragment key `52aac1f2306b8fc5`: bilinear + 5-tap sharpen + an R8 mask) is replaced by MetalFX when Upscaling is on (`nvn_mtl.m` `mfx3d_draw`; `DM_MFX3D=0/1` forces it). It only acts below 100% 3D scale: on phones (3D 0.75) it is the main sharpness gain, and on the Mac it works with Native output and a 3D Resolution below 100% (`s3d`, through `dm_video_scale3d` → `hle_oe.c` `s3_cfg`), so the interface renders at the screen's resolution and only the world is upscaled. MetalFX runs once per frame: after a 3D MetalFX frame the present enlarges with bilinear filtering (`mfx_want`; `DM_MFX_STACK=1` stacks both for tests).
  - MetalFX Temporal (`up` = 2, the row's third option, live; `dm_video_temporal_active`, `DM_MFXT=0/1` forces it; `dm/gfx/nvn/nvn_mv.inc`) replaces the spatial scaler in that pass, at every 3D Resolution: at 100% `hle_oe.c` `scale3d_tick` draws the 3D at 99% so the pass exists. Inputs: the game's 3D picture and depth directly, motion vectors, and the game's own Halton(2,3) camera jitter (32 steps), which no game path selects: the rewriter's `KJITTER` slot does (`SWITCHES`, csel rows at main+0x1637fbc and main+0x162a814), set at each `nvnCommandBufferBeginRecording` (`thread.c` `dm_jitter_refresh`). The game's AA byte is 0 with it (`aa_tick`). The offset comes out of the jittered projection: the world's camera is orthographic, so it sits in the fourth row (a perspective camera, as on the character screen, has it in the third). The motion vectors, which the Switch build never makes: a depth prepass draw (`mv_note_draw`: depth-only on D32S8 with depth writes, a vertex shader with the `dm_velocity` entry from `dmsh_rules.py` `velocity_entry`) keeps a copy of its constant buffers; at the upscale `mv_resolve` fills an RG16F target with the camera's motion from depth and the two view-projections, then draws again, depth-equal, the draws whose constants changed, so skinning comes from the game's own vertex code. Draws of one mesh are matched by nearest position; a move of a quarter screen in one frame means no match (a draw) or a history reset (the camera). The depth target's store is kept while this is on (`g_mv_depth_ser`). Libraries, pipelines and the scaler build on a background queue, and until they exist the frame uses the spatial scaler. The write-back adds a contrast-adaptive sharpen, since the scaler's picture is soft (`mt_sharp_div`, by the enlargement); a negative mip bias made no measurable difference. `DM_MV=1` makes the motion vectors without the scaler; `DM_MV_DUMP=<first>:<count>:<prefix>` writes velocity, 3D picture and MetalFX output as raw files, and `dm/gfx/tools/mv_check.py` warps one frame onto the next with them. Measured on an M6 at 2560 x 1440, town_loop, 2026-11-04 (`DM_NVN_PASSGPU_SERIAL=1`, GPU times at a lowered clock): the MetalFX block 1.4 → 5 ms, the motion passes 0.3 ms, the frame 8 → 13 ms; replay thread +0.7 ms; energy per frame +15% in town, +7% in the wild; slow frames at 100% against SMAA, 6 + 6 ABBA: town 1.06% → 0.00% (p 0.002), wild 1.58% → 0.00% (p 0.10), the longest frame of a town run (median) 33.3 → 16.7 ms. Why the slow frames go is not established (the GPU staying at a higher clock is the lead, s110). Scenarios `picture/rect.scn`, `picture/rec75t.scn`, `video_temporal_mac.scn`; no phone measurement yet.
  - Frame Generation (`fg`, live; `dm_video_fg_active`, `DM_FG=0/1` forces it; `dm/gfx/nvn/nvn_fg.inc`): MetalFX's frame interpolator between two Final Results, with the temporal scaler's motion, depth and camera offset, then this frame's interface blended over the result at the game's compose draw, so the interface is never warped. Each frame queues a generated and a real present, a FIFO on the display link, and the game waits two link ticks per frame. Screens with no world and the compose shader's other paths (colour-blind, HDR) stay at the real rate. Recommended takes Temporal with it where the system offers both (`rec_fg` in `front_mac.m` / `front_ios.m`; `DM_TEST_NO_FG=1` acts as a chip without) and, on the Mac, the full picture fits with what it costs (`dm_picrec.c`, `rec_v` 11: 2.1 ms + 2.63 ms per output megapixel a drawn frame on an M6 at full clock, half that counted against the 60 fps capacity); other Macs keep the conventional picture. The phone has no such model yet: its answer is the system's alone. Off at the power saver's rate. Frame Drops judges a real frame one tick late as a drop (`hle_oe.c` `s3_fg_view`). Judge motion with `dm/verify/debugging.md` "Motion and animation", never on the fixed clock.
  - Moving things stay solid in generated frames (`DM_FG_FORE=0` turns it off): the interpolator alone loses thin things against sliding scenery and keeps a pale copy, so it never sees them. `mv_resolve` draws a mask of what really moves in the world (`mv_lives`: bones or position changed in the last 30 drawn frames; the ground's constants change too, but only follow the camera). `fg_plate` paints those pixels out of the picture it interpolates, its motion input is the camera's alone and its depth is flat; `fg_splat` and the blend put them back half way along their own motion. +0.5 ms GPU at 2560 x 1440 on an M6. On the open-ground walk: ground half way (off 0.4%), detail 0.999 of a real frame with sharpen 0.2.
  - iOS gets the same Video tab with a Graphics section (overlay flag `DM_OVERLAY_IOS`, set only by `dm/ios/run.sh`): 3D Resolution 50 / 67 / 75 / 85 / 100% (live through `hle_oe.c` `s3_cfg`, which re-reads the setting each tick; stored values keep their old meaning, 0 / 1 / 2 = 75 / 85 / 100 and 3 / 4 = 67 / 50, and `dm_video.c` `car_of` / `val_of` map them to the row's order), Graphic Presets, Upscaling, plus Color / Anti-Aliasing / Anisotropic Filtering / Environment Shadows / Shadow Quality (same keys as the Mac; `front_ios.m` `layer_color_ios` tags the present layer). First boot (`front_ios.m` `front_video_firstboot`) = 75% + MetalFX; `front_video_rec` adds SMAA, 16x and Accurate colour (`rec_v` 3, measured on an iPhone 17 Pro: 60 fps, frame pacing and CPU unchanged). The iOS Recommended is the same on every iPhone and iPad: nothing reads the chip or the memory (the first-boot log only names the device, its GPU, BC support and the screen), only MetalFX support (`front_video_mfx_ok`) decides Upscaling, and no other device has been measured. Frame Drops defaults to Lower on every iOS device (`dm_custom.c` `k_def`, `DM_PHONE`). The phone presets (`dm_video.c` `tier_of`): Performance = 75%, Standard upscaling, FXAA; Quality = 100%, MetalFX, SMAA, 16x; both leave Environment Shadows and Shadow Quality alone.
  - Phone test runs (`DM_SAVE_FRESH` / `DM_SAVE_TEST`) keep their settings in `Documents/settings.test` (`main.m`), fresh each `DM_SAVE_FRESH` run, so automated taps on Options never change the player's `sw_video.txt` / `sw_custom.txt`.
  - `DM_MFX=0/1` forces MetalFX for tests and logs present-pass GPU time; `DM_GAME_RES` still overrides the render size. A test copy (`DM_APP_SUPPORT`) starts from the original settings unless `DM_TEST_VIDEO_FIRSTBOOT=1`.
  - Environment Shadows (`envsh`; part of the Mac's full Recommended picture since `rec_v` 4, off in the light one; on iOS a row only, off and left alone by the phone presets until its phone cost is measured): on the Switch nearly every non-character model is kept out of the shadow casters by its platform shadow tier (`Model_ApplyPlatformTiers` main+0x15ce950). The rewriter's `SWITCHES` entry also takes that test's "may cast shadows" branch when the `KESHADOW` thread slot is non-zero; every guest thread gets the slot from `dm_video_env_shadows` at its start (`thread.c`), so the value is latched at launch and a change shows the restart hint. Cost on an M6 (12-core GPU), town_loop, 6 + 6 ABBA, 2026-11-04, ledger `$DM_ROOT/run/bench/ledger.tsv`: at 3840 x 2160 (`DM_GAME_RES=3840x2160 DM_3D_SCALE_DYN=0`) with `DM_NVN_PASSGPU`, shadow passes +0.70 ms in town / +0.38 in the Blood Moor with the 4096 map, +0.55 / +0.32 with 2100, whole-frame GPU p50 +0.24 ms (no difference); at Recommended (Native 2560, no PASSGPU) GPU p50 +0.33 ms (B slower) and slow frames (over 20.8 ms, one missed refresh) in all 12 runs, 0.9–1.9% of town frames with and without (the same share at 4eebc96: open, not a shadow cost). Pass sums are the cost; whole-frame `gpu_ms` at Recommended can't rank costs on this Mac (clock). The first figure (+0.5 / +0.25 ms, 4f952ed) used the old 1024 map; measure again when shadow code changes. `DM_ENV_SHADOWS=0/1` forces it.
  - Shadow Quality (`shq`: 0 Standard = the Switch's 2100-texel directional shadow map, 1 High 3072, 2 Ultra 4096; Ultra with the Mac's full Recommended picture since `rec_v` 5, Standard with the light one; iOS keeps Standard and its presets leave the row alone): the map carries the key light (layer 0) and the camera-oriented "character shadow" view that gives characters their self-shadowing (layer 1). The inherited 2100 → 1024 rewriter cut (phone GPU time) is gone; the rewriter's single-instruction `PARAMS` gate at main+0x168a580 loads the edge from the `KSHSIZE` thread slot, which every guest thread gets from `dm_video_shadow_size` at its start (`thread.c`), so it is latched at launch with the restart hint. The game sets no hardware depth bias (its shaders carry one tuned for 2100 texels), which is why 1024 showed coarse acne. Cost on an M6: shadow passes 1.07 ms at 1024 → 2.14 ms at 4096 (92bcad1); Ultra against Standard (2100), environment shadows on, at 3840 x 2160: shadow passes +0.59 ms in town / +0.55 in the Blood Moor, whole-frame GPU p50 −0.28 ms (B faster: the clock, not a cost) (2026-11-04, 6 + 6 ABBA). `DM_SHADOW_SIZE=<px>` forces the edge.
  - Environment Detail (`envd`, 0 Standard / 1 High, `DM_ENV_DETAIL=0/1` forces it; with the Mac's full Recommended picture since `rec_v` 10; on iOS a row only, off until measured): the observation gate at `Renderer_Init` (`OBS_RENDERER_CFG`, `dm_video_renderer_cfg_obs`) raises the game's own decal size, terrain tier and effect-light budget in the renderer's boot config, latched at launch. `DM_TEST_RCFG` tries the game's other settings the same way (`dm/verify/debugging.md`). Standard leaves the config as the game built it.
  - Bloom 1/4 (`DM_NVN_BLOOM_Q=0` turns it off) was measured equal to the game's full chain and is kept; it engages only when the cutoff's viewport covers its target, an abort or a cut-short chain clears B0 to black, and `bloom 1/4, frames …` lines (first after 600 frames, then every 3600) plus `applied on new targets` lines show its health. Both helper kernels are compiled on a background serial queue from `mtl_init`. The host logs `Tables: …` at start (red in the Mac App when a table is missing) and Share Logs' info.txt carries it. `DM_TEST_DAYTIME=<angle>@<tick>` pins the game's sun angle in every act that exists at that tick (an act's environment exists only once the hero has been there: pin after arrival); the README's town pairs use it on both sides.
- **Text input (swkbd → `front_text_input`):** with a controller in use (`dm_gamepad_mode()` and the latest glyphs aren't the keyboard's), an on-screen keyboard drawn over the picture (`dm/ios/Sources/dm_vkbd.c` model + `t_vkbd`, `dm_vkbd_ui.m` drawing in a CALayer with the game's Exocet / Formal436 / BlizzardGlobal fonts read from the RomFS); otherwise the Mac sheet / iOS alert. Keyboard, mouse or touch while it is up hands over to the box, and a controller press while the box is up hands back, keeping the text. `DM_VKBD=1/0` forces it; `DM_TEST_SWKBD_KIND=num:<max>|text` turns the naming box into the other layouts for screenshots. Its layer isn't in the presented frame: `DM_TEST_PRESENT_SHOT` composites it.
- **Options page (Mac, iOS):**
  - Tabs: Video | Audio | Gameplay | Settings | Extras (`touch_layout.py` `_tabs5`). Video = the borrowed `VideoOptionsPanel` with only the picture rows; Extras = our own page `2extras_lay/ExtrasOptionsPanel` (`_EXTRAS_PANEL`, class `VideoOptionsPanel`), in a new folder under the layout root because `casc_overlay.py` can only add a new subdirectory under an existing directory, and both input modes fall back to the root. The default overlay (no `DM_OVERLAY_MAC` / `_IOS`) keeps one Custom page.
  - Don't open a page by a case-variant of an existing name (e.g. `videooptionspanel`): the game creates a second instance that `ClosePanel` never closes, and two pages draw on top of each other. Other candidate pages crash (`LootFilterOptionsPanel`) or are something else (`OnlineGameSettingsPanel` = the online Game Creation window).
  - The host (`dm_custom.c`) runs its page code on whichever of the two pages is on screen; `dm_custom_shown()` tells "Default" which rows that page holds, so it resets only that tab. The controller selection follows the selected row only right after it changes (the Mac's PC and controller sets both run the controller Options pages, and following every frame undid each wheel scroll).
  - Frame Rate and Frame Drops (Mac and iOS) are selectors at the end of the Video tab's Graphics section (`dm_video.c` rows `fps` / `drops`), but their values are the custom keys `eco_fps` / `q3d` in `sw_custom.txt`: pending / Apply / Discard go through `dm_custom.c` (`dm_custom_shown_value`, `dm_custom_set_pending`), and the Video tab's Default resets them. `q3d` is written only while it differs from its default (`custom_v` 2), so it follows the Mac's Recommended until the player picks the other value. The Frame Rate row names the screen's real rates (`fps_labels`: `dm_pace_hz` / `dm_eco_hz`, 72 / 36 fps on 144 Hz). Frame Drops = Lower steps the 3D scale down at most three levels below the chosen 3D Resolution (`dm/host/dm_s3lv.h`: × 0.93 a step, rounded, never below 50%; 100% → 0.86 / 0.80 / 0.75, 75% → 0.70 / 0.65 / 0.60, none at 50%: `dm_3d_scale_levels`). Its controller (`hle_oe.c` `s3_gates`) judges only frames on screen in a game: not while the presents are hidden or off screen (judging resumes 3 s after the picture is visible again), not outside a game or under a loading screen, and not in the first 20 s of a game or after a loading screen. The world freeze (`diag.c` `freeze_tick`) is dormant for players (only `DM_TEST_FREEZE` / `DM_PANEL_FREEZE` attach it): frozen frames present at full rate and would count as clean windows, so before enabling it, gate them like hidden frames and hold the controller for 3 s after unfreezing. The Extras layout leaves those two rows out (`_VIDEO_TAB_KEYS`); the default overlay's single page keeps them. `dm_custom.c` walks and resets only the custom rows the page on screen has (`ck[]`, `g_page_keys`).
  - Controller highlight on Video / Extras: the game's own hover image on the selected control, as on the other tabs (`TouchCustomHiCar` = `Carousel_Hover`, `TouchCustomHiBox` = `CheckBox_Hover`, native size, centered). Radio rows light the chosen option; left / right step the language row. The host hides it while its item isn't wholly inside the list box. Keep list content right of the text edge: the scroll area clips at the panel's left edge, and a nine-tile's corners escape that clip (the old whole-row gold frame lost its left edge and left corners hanging over the tabs).
  - The Mac App has two interface sets and switches by the device in use: mouse / keyboard → the **PC set**, a controller press → the controller layouts (`dm_padmode.c`, a three-way prefix switch: touch / controller / PC; `inputmode mode=pc`; `DM_PC_MODE=1` starts the CLI host or a test copy in it, `=0` keeps the App on the console set). The Mac overlay (`DM_OVERLAY_MAC`) carries the PC set: the console layouts in `layouts/3pc_console_switch_layout/` and `4pc_console_layout/`, with the PC twins of the HUD, character select and modals (`gen_romfs_overlay.py` `PC_TWINS`; never repoint the bare `Controller/` prefix, the game builds art paths with it), art a twin draws from a path the overlay borrows (`PC_ART`: the HUD's statue frame `front_panel` is our close button, so the PC HUD gets the original in `3pc_ui/`), the gauntlet pointer `pc_cursor.png` (`front_mac.m` `pc_hand`) and the PC wording `pc_strings.tsv` (`pc_strings.py`: the game's own PC strings for the console prompts, swapped in by `dm_padmode.c` `str_apply`). The PC set has no controller button icons or cursor ring. Item panels stay console (the Switch build's PC item grids take no clicks), drawn 0.82× about the top centre so they end above the PC HUD: `pc_panels.py` writes PC copies of every layout whose chain uses an in-game panel rectangle of the console profile (bases in `layouts/5pc_base_lay/`; our Extras page opens there as `5pc_base_lay/ExtrasOptionsPanel`, a name `dm_custom.c` knows).
  - PC input (`dm_kbm.c`, in game with the PC set): keys run the game's own PC bindings (live list main+0x2086f64, executor main+0x3700e0); the world runs the game's own PC mouse code (`dm_world.c`): every poll the unit under the pointer becomes the game's hover target (slot main+0x2082c90, setter main+0x2f3e40, as D2R's PC hover does; the Switch build fills it from the controller's auto-target), and the click entry takes the PC actions on it (0 / 3 press: lock it and use the skill; 1 / 4 held, every poll; 2 / 5 release), so attacking, holding on a monster, talking to NPCs and picking up behave as on PC; a click on an item area (`dm_ui_item_at_now` 1, 7) follows the game's own PC item tooltips (`item_click`: left = the console's A at the pointer, pick up / put down; Shift = equip / unequip / belt; Ctrl = unequip, the console's held Y to move or sell, Y to drop; Ctrl + Shift on a stash stack = the console's X, into the cube; right = buy / open / use, a whole stash stack into the inventory, never equip; event `pc_item`); other clicks on panels go through the Mac pointer path below, and so does a press the hit test can't place (0) while anything non-resident is open; the PC HUD band (interface height) takes the game's own mouse messages (`dm_ui_ptr`). Esc: death → the screen's Continue message, game menu / Options → B, an NPC's speech or menu → B (Space too; Return does nothing there: the chat froze the speech), panels open → the Clear Screen binding, nothing open → + then L × 3 to the pause tab. On a 16:10 Mac the interface band stays at the bottom (`touch_mac.c`; only the iPad centres it while a panel is open). A page a PC binding opened is closed with Clear Screen (the console's B leaves it frameless), and an empty `UISwitcher` frame is closed by message. Shift held compares (`dm_kbm_shift`). Key rebinding isn't wired: `KeyBindingMenu` opens but takes no key through `KeyDown`. Scenarios `pc_mode.scn` (CLI), `pc_mode_mac.scn`, `npc_talk.scn` (every control scheme: `--scheme pc|controller|touch`, `--aspect 16x10` for a MacBook-shaped screen), `pc_combat_mac.scn` and `pc_items_mac.scn` (mac-app); the control port's `kbm` takes `shift=1` / `ctrl=1`; on the CLI host, panel clicks need a window and `DM_TOUCH_LIVE=1`.
  - The console Options layouts take the touch row pitch (180), clickable Apply / Default / Discard and an off-screen key legend (`PAD_SETTINGS_MAC`, `pad_settings_legend`).
  - Mac mouse paths (`touch_mac.c`): in game, presses go through the touch state machine; in the front end (`!in`) they go to `dm_ui_touch` as clicks, except that a vertical drag past 10 points scrolls and a sideways drag starting on a slider (`dm_ui_slider_at`) sets it. A press on a scroll bar (`dm_ui_scrollbar_at`) is never a click: it drags the thumb (`ui_drag_pump` kind 4, steering the list with wheel steps until `Indicator.y` sits under the pointer). The wheel sends the game's `MouseScroll` at the pointer (`dm_wheel_ui`); in game with no panel it still pushes the right stick.
- **iOS:**
  - Reinstall over the app; deleting it wipes the 5.6 GB of resources and the saves.
  - The 18 resource archives can be pushed with `xcrun devicectl device copy to … --domain-type appDataContainer --destination 'Library/Application Support/romfs'`.
  - `run.sh install` passes a battery gate meant for test phones; `DM_DEVBATTERY=0` turns it off (set in `dm/ios/local.env`).
  - **Windows and resizes (iPad, iPhone Duo):** the system resizes the scene in discrete steps (TN3192). In true full screen the render size follows the window (`front_ios.m` `window_px` → `dm_geom_window_px`; an iPad picture is never narrower than 4:3), and the picture's fit is `dm/host/dm_geom.h`, shared with the present (`nvn_mtl.m` `present_encode`, unit-tested by `t_geom`). The scene's minimum size is 667 × 375 pt (`main.m`). The scale, the refresh rate and the touch controls' display link come from the scene's screen, not the main screen (`front_ios_view` takes the window scene). Tests: `DM_TEST_IDIOM=pad|phone` makes either package take the iPad or the phone paths, the control port's `app do=resize_<w>x<h>` resizes the view as a discrete scene resize does, and the `geom` event reports the settled geometry; scenarios `ios_window_resize.scn` and `ios_fold_resize.scn`. `SMOKE_MAC_FAMILY=1,2 SMOKE_MAC_PKG_ONLY=1 dm/ios/tools/smoke_mac.sh 1` builds the iPad-idiom package `.iosdd_mac_pad/DmMacSmokePad.app` (package only); move it to the Trash when done with it.
  - **Guest memory:** the guest reservation starts at 5120 MiB and steps down by 128 MiB to 4864 MiB where the device refuses it (no extended virtual addressing entitlement, as in free-account builds); on a stepped-down reservation the pool reported to the game is trimmed to the heap region plus the image (`hle_svc.c` `pool_fit`, logged once), so the game's heap stops at the region.
