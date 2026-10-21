# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is the `PowerBeef/d2r-mobile` fork of `jhaizhou-ops/d2r-mobile`.
- The project ports the Nintendo Switch build of Diablo II: Resurrected + Reign of the Warlock (1.0.34.0 = PC 3.3.94012) to iOS/macOS.
- The game's own aarch64 code runs natively; only a few instructions are rewritten, and the graphics API is reimplemented on Metal.

Everything derived from the user's game files lives in the workspace `$DM_ROOT` (default `~/dm-work`), outside git:
- the extracted ExeFS/RomFS in `data/switch/3.3.94012/Update/`
- rewrite products, the Python venv and shader caches in `build/`
- run logs in `run/`

## Fork workflow

The fork develops on its own `main`. Changes worth sharing go upstream as cherry-picks.

- **Remotes:**
  - `origin` = the fork. `main` is its default branch.
  - `upstream` = jhaizhou-ops, fetch only (push URL `DISABLED`). Its branch is read as `upstream/main`; there is no local mirror.
- **Branches:**
  - **`main` is the fork's trunk.** All work lands here first, and the user's apps are built from it.
    - It is public: never force-push it, rebase it, or reset it to `upstream/main`.
    - Bigger work can use a short-lived local topic branch, merged into `main` when done.
  - **`up/<topic>` branches exist only for upstream PRs.** Each starts from `upstream/main` and receives cherry-picks from `main`.
    - PRs opened before this workflow keep their old branch names: `fix/extract-split-nsp`, `fix/ios-install-battery-gate`, `feat/mac-default-icon`, `feat/mac-cursor-autohide`.
- **Commits on `main`:**
  - Keep one topic per commit (or a short series), so any of them can be cherry-picked on its own.
  - Code follows the upstream rules below even when it isn't meant for upstream yet, so it can be sent later without rework.
  - End each message with an `Upstream:` trailer. Put it on the line just above the attribution lines, in the same block with no blank line, so git reads it as a trailer:
    - `Upstream: candidate` may go upstream. It must not touch fork-only files.
    - `Upstream: fork-only` stays here: this file, fork tooling, and the user's own preferences.
- **Pulling upstream changes into `main`:**
  1. `git fetch upstream`
  2. `git switch main && git merge upstream/main`. Always merge, never rebase. On a conflict, `git merge --abort` and report; don't auto-resolve.
     - Maintainers adopt PRs by re-applying them, so a conflict can come from our own change in its adopted form. Upstream's version wins, after the user confirms.
  3. Rebase each open PR branch onto `upstream/main` and push it with `--force-with-lease`. This updates the open upstream PR.
  4. `git push origin main`, then rebuild the apps.
- **Sending commits upstream:**
  1. The user picks the commits. Candidates not yet upstream:
     - `git log --format='%h %s' --grep='^Upstream: candidate' upstream/main..main`
     - `git cherry -v upstream/main main` marks with `-` the commits upstream already has in identical form.
     - `git log --all --grep='cherry picked from commit <sha>'` shows whether a commit is already on a PR branch.
  2. `git fetch upstream && git switch -c up/<topic> upstream/main`
  3. `git cherry-pick -x <sha>...`. A conflict means the change depends on other fork commits: `git cherry-pick --abort` and report.
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

dm/tools/mac_app.sh                   # Mac App → $DM_ROOT/out/macapp/D2R Mobile.app (macOS 26+; DM_MAC_M3=1 for 15–25)
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
- `dm/verify/accept.sh` items need ground-truth recordings that aren't shipped, so they can't be fully run here.

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
- **Products** go to `$DM_ROOT/build/cache/dm/out/<dm/PRODUCT>-<kind>` (currently `3.3.94012-r44-touch`):
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
- **Features are `dm/host/dm_*.c`:** world taps, rift, vault, pouch, padmode, custom settings, and so on.
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
  - The key order in `dm_custom.c` must match the Custom page rows (`touch_layout.py _CUSTOM_ROWS`).
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
- **Regenerate after editing:**
  - `patch_nso.py` or `dm/PRODUCT` → `dm/tools/product.sh build`
  - shader-chain sources or tools → `./setup.sh`, which re-registers the chain and re-translates
  - NVN names or prototypes → `gfx/tools/nvn_slots.py` + `gen_protos.py`
  - `pso_manifest.tsv` row order matters; don't sort it.
- **Mac App:**
  - It links to build outputs under `$DM_ROOT`; the bundle ID is `org.d2rmobile.mac` (`.m3` for the older-API build).
  - Its resource folder is stored as the `RomfsDir` default.
  - The pointer auto-hide delay is `CursorHideSeconds`.
- **iOS:**
  - Reinstall over the app; deleting it wipes the 5.6 GB of resources and the saves.
  - The 18 resource archives can be pushed with `xcrun devicectl device copy to … --domain-type appDataContainer --destination 'Library/Application Support/romfs'`.
  - `run.sh install` passes a battery gate meant for the maintainers' test phones. On upstream code without the gate fix, set `DM_DEVBATTERY=0` when `DM_IOS_UDID` isn't configured.
