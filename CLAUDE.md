# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is **d2r-metal**, the `PowerBeef/d2r-metal` fork of `jhaizhou-ops/d2r-mobile` (renamed from `PowerBeef/d2r-mobile` on 2026-10-25).
- The project ports the Nintendo Switch build of Diablo II: Resurrected + Reign of the Warlock (1.0.34.0 = PC 3.3.94012) to iOS/macOS.
- The game's own aarch64 code runs natively; only a few instructions are rewritten, and the graphics API is reimplemented on Metal.

Everything derived from the user's game files lives in the workspace `$DM_ROOT` (default `~/dm-work`), outside git:
- the extracted ExeFS/RomFS in `data/switch/3.3.94012/Update/`
- rewrite products, the Python venv and shader caches in `build/`
- run logs in `run/`

## Fork workflow

The fork develops on its own `main`. Changes worth sharing go upstream as cherry-picks.

- **Platform scope: macOS and iOS only.**
  - The fork targets Apple silicon, Metal 4, MetalFX and current Apple hardware.
  - The Android, HarmonyOS and Vulkan code (`dm/android`, `dm/harmony`, `dm/gfx/vulkan`, their docs and switches) stays in the tree so upstream merges stay clean, but it isn't built, tested or maintained here. Shared-code changes may break it; don't delete it.
  - When a change also fits upstream's platforms, keep it portable and offer it as an `up/<topic>` PR. Apple-only work stays `Upstream: fork-only` unless upstream would want it.
- **Branding:** the apps show "D2R Metal" (`mac_app.sh` `NAME`, the Mac window title, the iOS `CFBundleDisplayName`, player-facing strings). Everything player data hangs on keeps the old spelling: the bundle IDs, the Mac data folders (`APP_NAME` "D2R Mobile" in `app_mac.m`), the iOS "D2R Mobile Resources" folder, the `org.d2rmobile.*` IDs and the executable `D2RMobile`. Don't rename those.

- **Remotes:**
  - `origin` = the fork. `main` is its default branch.
  - `upstream` = jhaizhou-ops, fetch only (push URL `DISABLED`). Its branch is read as `upstream/main`; there is no local mirror.
- **Branches:**
  - **`main` is the fork's trunk.** All work lands here first, and the user's apps are built from it.
    - It is public: never force-push it, rebase it, or reset it to `upstream/main`.
    - Bigger work can use a short-lived local topic branch, merged into `main` when done.
  - **`up/<topic>` branches exist only for upstream PRs.** Each starts from `upstream/main` and receives cherry-picks from `main`.
    - The first PRs (#106–#109) were adopted into upstream v2.0.0 and their branches deleted.
- **Commits on `main`:**
  - Keep one topic per commit (or a short series), so any of them can be cherry-picked on its own.
  - Code follows the upstream rules below even when it isn't meant for upstream yet, so it can be sent later without rework.
  - End each message with an `Upstream:` trailer. Put it on the line just above the attribution lines, in the same block with no blank line, so git reads it as a trailer:
    - `Upstream: candidate` may go upstream. It must not touch fork-only files.
    - `Upstream: fork-only` stays here: this file, fork tooling, the README, the D2R Metal branding, and the user's own preferences.
- **Pulling upstream changes into `main`:**
  1. `git fetch upstream`
  2. `git switch main && git merge upstream/main`. Always merge, never rebase. On a conflict, `git merge --abort` and report; don't auto-resolve.
     - The READMEs are the fork's own: `.gitattributes` marks `README*.md` `merge=ours`, so upstream's README edits are dropped silently. That needs `git config merge.ours.driver true` once per clone; without it those files conflict.
     - Maintainers adopt PRs by re-applying them, so a conflict can come from our own change in its adopted form. Upstream's version wins, after the user confirms.
     - **Removed upstream features stay removed.** On 2026-10-27 the fork deleted upstream features that change the game's rules or economy, or play part of it for the player: the Greater Rift (with its extra mercenary slots and skill cap 40), Boss Rush, Elysium mode, the Kill Board, the free town portal, free identify, the vendor's Refresh Stock, auto-drink, mercenary auto-feed and auto-recast buffs; then the loot filter set by difficulty, the Runeword helper and the extra storage (14-column stash with personal pages 2–5, Trove, Pouch with auto-stash and Deposit, Chronicle vault); then the difficulty selectors (bottom bar, controller pause menu), the minimap marks (exits, waypoints, quest goals) and two logic-changing byte patches from the community offline fix.
       - Upstream changes to their deleted files come back as modify/delete conflicts: `dm/host/dm_rift*`, `dm_doorspot.h`, `dm_bossrush*`, `dm_elysium*`, `dm_kills.c`, `dm_freetp.c`, `dm_shoprefresh.c`, `dm_autobuff.c`, `dm_lootfilter.c`, `dm_runeword.c`, `dm_rwlv_core.*`, `dm_pages.c`, `dm_pouch.c`, `dm_vault.c`, `dm_vault_ui.c`, `dm/ios/Sources/dm_rift_hud.m`, `dm/ios/tools/merc_slots.py`, `gen_merc_art.py`, `wide_stash.py`, `gen_widestash_art.py`, `gen_trove_art.py`, `pouch_layout.py`, `gen_pouch_art.py`, `gen_vault_table.py`, `gen_vault_cover.py`, `gen_map_goal_art.py` and their tests. Resolve those with `git rm` without asking, and finish the merge.
       - In shared files, drop upstream hunks that only re-add hooks, strings, switches, layout entries or gates for them (auto-drink and auto-feed lived in `dm_actions.c`; the removed Extras keys are `auto_mana`, `merc_pot`, `auto_buff`, `gem_pick`, `store_btn` and `loot_filter`; the stash must stay 10 columns, so `inventory.bin` is never patched again; the iPhone bottom bar stays five cells with no difficulty cell or 1–8 strip, the controller pause menu has no Game Difficulty row (`TouchPadDiff`), and Options keeps the game's `OfflineDifficultyScaling` row; the minimap marks lived in `dm_world.c`, `front_ios.m` (`mapGoalSync`) and `dm_cursor.c` (`dm_ui_map_geom`), Extras key `map_mark`; `patch_nso.py` never regains the cuts at `0x5b14dc` and `0x5f3184`). Any other conflict is still aborted and reported.
       - Their gate numbers stay retired: OBS 15, 16, 17 and 22 (`OBS_MAX` stays 23) and svc 0x89, 0x8c, 0x8d and 0x8e. Never reuse them. The product revision only moves forward (`dm/PRODUCT`, r46 after the two byte patches went).
  3. Rebase each open PR branch onto `upstream/main` and push it with `--force-with-lease`. This updates the open upstream PR.
  4. `git push origin main`, then rebuild the apps.
- **Sending commits upstream:**
  1. The user picks the commits. Candidates not yet upstream:
     - `git log --format='%h %s' --grep='^Upstream: candidate' upstream/main..main`
     - `git cherry -v upstream/main main` marks with `-` the commits upstream already has in identical form.
     - `git log --all --grep='cherry picked from commit <sha>'` shows whether a commit is already on a PR branch.
  2. `git fetch upstream && git switch -c up/<topic> upstream/main`
  3. `git cherry-pick -x <sha>...`. A conflict means the change depends on other fork commits: `git cherry-pick --abort` and report.
     - Drop the fork-only `Upstream:` line from each picked commit, keeping the `(cherry picked from commit …)` line: `git log -1 --format=%B | sed '/^Upstream: /d' | git commit --amend -F -`.
  4. `git diff --name-only upstream/main` must list no fork-only file. Run the source checks and the test suites the change touches on this branch.
  5. Only after the user approves each PR:
     - `git push -u origin up/<topic>`
     - `gh pr create -R jhaizhou-ops/d2r-mobile --head PowerBeef:up/<topic>`
  6. Fill in `.github/PULL_REQUEST_TEMPLATE.md` honestly: tick only the checks actually run, and paste their last line.
  7. For review feedback, fix on `main` first, then cherry-pick the fix onto the PR branch.
  8. Maintainers "adopt" PRs: they close them and re-apply the change on their own `main` with author credit. After that, ask before deleting the PR branch locally and on `origin`.
- **The three files `./setup.sh` rewrites for local tools stay dirty and are never committed:** `dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`, `dm/gfx/nvn/c1_full.tsv`.
  - Don't `git reset --hard` / `git checkout .` over them. Restoring them means re-running `./setup.sh`, which re-translates every shader (~10 min).
  - Switch branches with `git switch`; use `git stash` if needed.
  - The local clone may have a `.git/hooks/pre-commit` guard that refuses these files, `dm/ios/local.env`, `dm/ios/Gen/`, game, key and save file types, and anything over 2 MB.
- **`gh` in a fork clone resolves the `upstream` remote first**, so a bare `gh pr …` (and any tool that calls it, such as an app's "create PR" button) targets jhaizhou-ops.
  - Run `gh repo set-default PowerBeef/d2r-metal` once per clone.
  - Then pass `-R jhaizhou-ops/d2r-mobile` explicitly for anything aimed upstream.
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

## Rules the upstream repo enforces (CONTRIBUTING.md)

1. **No game content.** Nothing from the game, and nothing generated from it, goes into git, issues or PRs: no program, art, text, shaders, saves, keys or built packages.
2. **Every code comment is bilingual, same content:**
   - Block and header comments: the Chinese line(s), then the English line(s).
   - Trailing comments: `中文 | English` on one line.
   - Every source file opens with a comment saying what it does. `dm/host` feature files also list the game addresses they rely on.
   - Docs are paired: `X.md` (English) with `X.zh-CN.md`. READMEs also have `.es.md`.
   - `dm/third_party/` is exempt.
3. **Player-visible text goes in the string table, never as a literal:**
   - Add the key / Chinese / English row to `dm/i18n/ui_text.tsv`. Keys are `dm` + alphanumerics, no underscore; English length caps are in `EN_LIMITS`.
   - Regenerate with `python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h` and commit it.
   - A new Python script that writes player-visible text must be added to `PY_FILES` in `dm/i18n/scan_literals.py`.

## Commands

The Python version and the venv:
- Use the Python version the docs verified (3.14). If the default `python3` is newer, put a 3.14 first on `PATH` for `./setup.sh`, which creates the venv from it.
- Most scripts need the venv: `source $DM_ROOT/build/cache/dm/venv/bin/activate`.

```bash
./setup.sh check                      # read-only health check (tools, game files, signing)
./setup.sh                            # venv, rewrite (product.sh build), shader translation; reruns skip done steps
./setup.sh <package>... <prod.keys>   # extract game files first (base and update may be separate packages)

dm/tools/mac_app.sh                   # Mac App → $DM_ROOT/out/macapp/D2R Metal.app (macOS 26+; DM_MAC_M3=1 for 15–25)
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
make -C dm/gfx/tests run              # graphics tests on the local Metal device; run-m3 = older-API pass
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
- `dm/verify/accept.sh` items need ground-truth recordings that aren't shipped, so they can't be fully run here. Those items report `⚪ 不可用` (unavailable) and don't count as red.
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
  - Real mouse events for any Mac App build, including upstream ones without a control port: `DM_TEST_NSMOUSE=<file>` (`down` / `dragto` / `hover` / `key` lines), `DM_TEST_PRESENT_SHOT=<s>:<png>`, `DM_TEST_QUIT_AT=<s>`. Leave `DM_WATCH` unset there: its stall watchdog kills the front end after 15 s.
  - Lines that work with the App in the background (no key window needed): `nav <keycode>` (a key straight to the game's key handler), `pad <buttons>` / `pads b1,b2,…` (virtual controller presses through `dm_gamepad`, self-timed; one scheduled line per press gets merged), `navpad <buttons>` (skipped while a text box is open, so extra presses to reach a page are harmless), `skey <keycode> <chars>` (through `[NSApp sendEvent:]` and the event monitors), `sheet ok|cancel [text]` (fill and press the Mac text sheet). `DM_TEST_PAD_GLYPH=2|3|0` makes the virtual controller count as Xbox / PlayStation / Switch for the key caps.
- `ios-mac` keeps running the package `smoke_mac.sh 1` last built, not the `run.sh build` output: rebuild it after layout or host changes.

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
- `XTRACE_*` are os_signposts on Apple. Record them with `xctrace record --template Logging --attach <pid>`.
- A GPU capture is about 4 GB per frame, and the disk is tight. `gpucap` refuses to capture when free space is short, and the `gpu_capture` step deletes the trace unless `keep=1`.

**Debugging** (all in `dm/verify/debugging.md`):

```bash
python3 dm/tools/crashlog.py show <run>/crash.json                                  # symbolicated crash (DM_CRASH_JSON; the runner writes crash.txt)
python3 dm/tools/guestsyms.py build                                                  # local guest symbol table for module+offset lookups
make -C dm/host dmhost_asan|dmhost_ubsan|dmhost_tsan && SCN_DMHOST=$PWD/dm/host/dmhost_asan python3 dm/tools/scenario.py run …   # sanitizer host; any report is red (sanitizer.txt)
python3 dm/tools/scenario.py run … --metal-validation api|lint|shader|all           # Metal validation layers (metal_validation.txt)
python3 dm/tools/explore.py run --seed N --episodes K [--dmhost …]                  # random play; a red run is shrunk to minimal_<k>.scn
python3 dm/tools/replay.py record <scn> && python3 dm/tools/replay.py check <dir> --dmhost A --dmhost B   # same input on two builds → first divergent tick
python3 dm/tools/tickdiff.py a/ticks.jsonl b/ticks.jsonl                             # first divergent tick between two DM_TICKS records
python3 dm/tools/c2s.py summary|near <recording> [tick]                              # recorded client packets labelled (dm/verify/c2s_opcodes.tsv), lengths checked
DM_BREAK_AT_TICK=<tick> ./dmhost … ; lldb -p <pid>; (lldb) command script import dm/tools/lldb/dm.py   # dm modules|where|bt|break|tick|ui|focus|saves|unit <id>
```

- The sanitizer targets don't track headers: delete the binary to force a rebuild. They run game threads on 8 MiB host stacks and carry their own options and TSan suppressions (`main.c`).
- Shader validation crashes the Metal 4 host inside Metal (macOS 27); use `SCN_DMHOST=dm/host/dmhost_m3`.
- `DM_BREAK_AT_TICK` waits on the tick thread: start the host by hand without `DM_WATCH` / `DM_WATCH_STALL`. In `lldb --batch`, commands after the trap go in `-k`, not `-o`.
- Agents can drive the same lldb through `(lldb) protocol-server start MCP` + `xcrun lldb-mcp` (one MCP server at a time; delete stale `~/.lldb/lldb-mcp-<pid>.json`).
  - With no server running when it starts, `lldb-mcp` starts its own lldb, which stays up and is reused. On Xcode 27.1 that lldb can't attach ("Bad file descriptor") unless `LLDB_EXE_PATH` points at a wrapper that gives it a stdin; `debugging.md` has the wrapper. Never `process launch` through `lldb-mcp`: it never returns and blocks that lldb until it's killed. Start the host yourself and attach.
- **Engine-analysis database** (`dm/tools/re/README.md`): a private Ghidra project of `main` in `$DM_ROOT/re` (never in git, issues, PRs or artifacts). `re.sh serve` runs the query server (the only process that may hold the project); `re.sh q decompile|xrefs|callgraph|strings|find|name|comment main+0x…` queries it. Project addresses are `0x7100000000 + offset` (Ghidra's `FUN_71…` + `005d3bc0` = `main+0x5d3bc0`). `re.sh seed` / `export` need the server stopped; the export feeds `guestsyms.py`. `re.sh tables` (also with the server stopped) runs the headless game under lldb to record every data table's columns into `$DM_ROOT/re/export/tables.json`, which `seed` turns into `/dm/tables/txt_<table>` structures. `re.sh autoname` (server stopped) proposes names for unnamed functions in `$DM_ROOT/re/names_auto.tsv` (strings, vtables, table callbacks, packet handlers); `seed` writes its high / medium rows after `names.tsv`.
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
- **Products** go to `$DM_ROOT/build/cache/dm/out/<dm/PRODUCT>-<kind>` (currently `3.3.94012-r45-touch`):
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
- **Metal 4 is the default.** `DM_NVN_M3` / `DM_MAC_M3` / `DM_IOS_M3` selects the older-API backend for macOS 15 / iOS 18. Every divergence is inside `#if DM_NVN_M3` in `nvn_mtl.m`.

**D. Frontends and features.**
- **`dm/host/front.h` is implemented by:**
  - `front_mac.m` + `app_mac.m` (Mac App; `touch_mac.c` for scripted touch)
  - `dm/ios/Sources/front_ios.m` (iOS)
  - `dm/tvos/` (community preview)
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
  - The key order in `dm_custom.c` `k_keys` must match `touch_layout.py _CUSTOM_ROWS`, the full list (`test_touch_layout_options.py` checks it). A page may leave rows out: Extras (Mac and iOS) drops the two the Video tab shows; Android's Custom page keeps them.
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
  - It links to build outputs under `$DM_ROOT`; the bundle ID is `org.d2rmobile.mac` (`.m3` for the older-API build).
  - Its resource folder is stored as the `RomfsDir` default.
  - The pointer auto-hide delay is `CursorHideSeconds`.
  - Picture settings (Display Mode, Resolution, Graphic Presets, Upscaling) are on the Mac Options Video tab: layout `touch_layout.py` `_VIDEO` (`DM_OVERLAY_MAC`), logic `dm_video.c`, stored in `sw_video.txt` beside `sw_custom.txt` with a first-boot "recommended" copy (`front_mac.m` `front_video_firstboot`; the older `StartFullScreen` / `RenderScale` / `MetalFXUpscale` defaults only seed it once). Selectors are native `CarouselWidget`s: read `+0x7ec`, set through `main+0xab0c84`; their update message must not be `PanelManager:ClosePanel:<name>` (the selector replaces the third part, and a nameless ClosePanel crashes).
  - The game's own 3D upscale pass (`ps_upsample_p`, fragment key `52aac1f2306b8fc5`: bilinear + 5-tap sharpen + an R8 mask) is replaced by MetalFX when Upscaling is on (`nvn_mtl.m` `mfx3d_draw`; `DM_MFX3D=0/1` forces it). It only acts below 100% 3D scale, so on the Mac (3D 1.0) it is idle and on phones (3D 0.75) it is the main sharpness gain.
  - iOS gets the same Video tab with a Graphics section (overlay flag `DM_OVERLAY_IOS`, set only by `dm/ios/run.sh`; Android shares the generator without it): 3D Resolution 75 / 85 / 100% (live through `hle_oe.c` `s3_cfg`, which re-reads the setting each tick), Graphic Presets, Upscaling. First boot (`front_ios.m` `front_video_firstboot`) = 75% + MetalFX.
  - `DM_MFX=0/1` forces MetalFX for tests and logs present-pass GPU time; `DM_GAME_RES` still overrides the render size. A test copy (`DM_APP_SUPPORT`) starts from the original settings unless `DM_TEST_VIDEO_FIRSTBOOT=1`.
- **Text input (swkbd → `front_text_input`):** with a controller in use (`dm_gamepad_mode()` and the latest glyphs aren't the keyboard's), an on-screen keyboard drawn over the picture (`dm/ios/Sources/dm_vkbd.c` model + `t_vkbd`, `dm_vkbd_ui.m` drawing in a CALayer with the game's Exocet / Formal436 / BlizzardGlobal fonts read from the RomFS); otherwise the Mac sheet / iOS alert. Keyboard, mouse or touch while it is up hands over to the box, and a controller press while the box is up hands back, keeping the text. `DM_VKBD=1/0` forces it; `DM_TEST_SWKBD_KIND=num:<max>|text` turns the naming box into the other layouts for screenshots. Its layer isn't in the presented frame: `DM_TEST_PRESENT_SHOT` composites it.
- **Options page (Mac, iOS):**
  - Tabs: Video | Audio | Gameplay | Settings | Extras (`touch_layout.py` `_tabs5`). Video = the borrowed `VideoOptionsPanel` with only the picture rows; Extras = our own page `2extras_lay/ExtrasOptionsPanel` (`_EXTRAS_PANEL`, class `VideoOptionsPanel`), in a new folder under the layout root because `casc_overlay.py` can only add a new subdirectory under an existing directory, and both input modes fall back to the root. Android keeps one Custom page.
  - Don't open a page by a case-variant of an existing name (e.g. `videooptionspanel`): the game creates a second instance that `ClosePanel` never closes, and two pages draw on top of each other. Other candidate pages crash (`LootFilterOptionsPanel`) or are something else (`OnlineGameSettingsPanel` = the online Game Creation window).
  - The host (`dm_custom.c`) runs its page code on whichever of the two pages is on screen; `dm_custom_shown()` tells "Default" which rows that page holds, so it resets only that tab. The controller selection follows the selected row only right after it changes (the Mac is always in controller mode, and following every frame undid each wheel scroll).
  - Frame Rate and Frame Drops (Mac and iOS) are selectors at the end of the Video tab's Graphics section (`dm_video.c` rows `fps` / `drops`), but their values are the custom keys `eco_fps` / `q3d` in `sw_custom.txt`: pending / Apply / Discard go through `dm_custom.c` (`dm_custom_shown_value`, `dm_custom_set_pending`), and the Video tab's Default resets them. The Extras layout leaves those two rows out (`_VIDEO_TAB_KEYS`); Android's single page keeps them. `dm_custom.c` walks and resets only the custom rows the page on screen has (`ck[]`, `g_page_keys`).
  - Controller highlight on Video / Extras: the game's own hover image on the selected control, as on the other tabs (`TouchCustomHiCar` = `Carousel_Hover`, `TouchCustomHiBox` = `CheckBox_Hover`, native size, centered). Radio rows light the chosen option; left / right step the language row. The host hides it while its item isn't wholly inside the list box. Keep list content right of the text edge: the scroll area clips at the panel's left edge, and a nine-tile's corners escape that clip (the old whole-row gold frame lost its left edge and left corners hanging over the tabs).
  - The Mac App always runs the controller layouts, with the mouse on top as touch gestures. Its Options layouts take the touch row pitch (180), clickable Apply / Default / Discard and an off-screen key legend (`PAD_SETTINGS_MAC`, `pad_settings_legend`).
  - Mac mouse paths (`touch_mac.c`): in game, presses go through the touch state machine; in the front end (`!in`) they go to `dm_ui_touch` as clicks, except that a vertical drag past 10 points scrolls and a sideways drag starting on a slider (`dm_ui_slider_at`) sets it. A press on a scroll bar (`dm_ui_scrollbar_at`) is never a click: it drags the thumb (`ui_drag_pump` kind 4, steering the list with wheel steps until `Indicator.y` sits under the pointer). The wheel sends the game's `MouseScroll` at the pointer (`dm_wheel_ui`); in game with no panel it still pushes the right stick.
- **iOS:**
  - Reinstall over the app; deleting it wipes the 5.6 GB of resources and the saves.
  - The 18 resource archives can be pushed with `xcrun devicectl device copy to … --domain-type appDataContainer --destination 'Library/Application Support/romfs'`.
  - `run.sh install` passes a battery gate meant for the maintainers' test phones. On upstream code without the gate fix, set `DM_DEVBATTERY=0` when `DM_IOS_UDID` isn't configured.
