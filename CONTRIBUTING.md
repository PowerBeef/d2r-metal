# Contributing

This page covers what to run before and after a change, and the rules of this repository. How the parts work is not repeated here; see [dm/README.md](dm/README.md) and [dm/verify/README.md](dm/verify/README.md).

## Checks

Run the checks on the untouched code first, then again after your change. All commands run from the repository root.

The gate runs them in tiers and prints one machine-readable last line, `FORKGATE OK|RED {json}` ([dm/verify/FORK_GATE.md](dm/verify/FORK_GATE.md)):

```
dm/verify/fork_gate.sh quick unit   # every change: source checks + the test suites (~3 min)
dm/verify/fork_gate.sh core         # host or graphics changes: scenarios on the CLI host + the touch cases (≤ 15 min)
dm/verify/fork_gate.sh full         # before rebuilding the apps: Mac App, the iOS package on the Mac, Metal validation, faults, baselines
```

### No game files needed

| Command | What it checks |
|---|---|
| `python3 dm/i18n/scan_literals.py` | Whether any Chinese text the player can see bypasses the string table; see [dm/i18n/README.md](dm/i18n/README.md) |
| `python3 dm/ios/tools/dm_i18n.py --check` | Whether every row of `dm/i18n/ui_text.tsv` follows the rules, and whether the checked-in `dm/host/dm_i18n_tab.h` was generated from it |
| `python3 dm/tools/switches.py check` | Whether `dm/switches.tsv` lists every `DM_*` switch the sources read, and no script sets a switch nothing reads (`switches.py gen` rewrites it) |

GitHub CI (`.github/workflows/tests.yml`) runs these checks, the scenario parser, the HUD-derivatives check and the Python tool tests on every push to `main` and every pull request, with an empty workspace. The host, graphics and iOS suites need macOS 27 / Xcode 27.1 and partly the game's files, so they stay local. A nightly job (`fuzz.yml`) fuzzes the file parsers (`dm/tools/fuzz`).

### Game files and a prepared workspace needed

Prerequisite: your own game files are in place, `./setup.sh` has finished (Python environment, rewrite output, shader translation), and the environment is active: `source ~/dm-work/build/cache/dm/venv/bin/activate`. `./setup.sh check` shows what is missing.

| Command | What it checks | Extra prerequisites |
|---|---|---|
| `make -C dm/host/tests run` | Host unit tests, plus the source scans: no writes to the original game's UI widgets (`native_write_scan.py`), touch / controller mode behaviour registered (`mode_scan.py`) | Some tests read the game's program files and resource directory |
| `make -C dm/gfx/tests run` | Graphics-layer unit tests on this machine's Metal device | Some tests read the shader cache `./setup.sh` translated |
| `make -C dm/ios/tests test` | Unit tests of the iOS shell (touch handling, skill binding parsing, …); no device | The skill index is generated from the game files when missing |
| `dm/verify/regress.sh` | Rewrite equivalence: the rewriter's output is structurally right, every rewritten site behaves like the original instruction, the host gates work. Last line `REGRESS OK` / `REGRESS FAIL`. Use `--fresh` whenever `dm/tools/patch_nso.py` changed | `llvm-objdump` and `llvm-objcopy` (`LLVM_BIN`) |
| `python3 dm/verify/golden.py replay check` / `shots check` | Our own baselines: fixed routes replayed tick-identically, and the screen pages | Recorded baselines in `$DM_ROOT/data/switch/golden` |
| `cd dm/ios && ./run.sh build` | The iOS package builds (no device involved) | Xcode, and the signing configuration in `dm/ios/local.env` |
| `dm/ios/tools/smoke_mac.sh 1` | Launches the iOS package on the Mac, loads a save and enters the game, with sound and no 【红】 (red) line in the log. Last line `SMOKE-MAC OK` / `SMOKE-MAC RED` | `dm/ios/tools/make_fixture.sh` once, for the smoke save |
| `dm/ios/tools/check_hud_gen.sh` | Nothing generated from game files was added to git | Run inside the git repository |

Criteria, output locations and environment variables are in [dm/verify/README.md](dm/verify/README.md) and [dm/ios/README.md](dm/ios/README.md).

## Rules

### 1. No game content, no engine-analysis output

The repository contains and distributes no game content. The game program, art, text, shaders and saves, and any image, table or code generated from them, stay out. Everyone produces these on their own machine from their own game files. The one exception is the README's own pictures in `docs/images`: screenshots of the game running in D2R Metal and the project's original artwork, shown on the project page only. They are never used by the build, never part of a release, and each stays under 2 MB; add one only for the README. What the repository does carry is interface information recorded to make the port work (shader interface descriptions, the graphics API function table, addresses inside the program and so on), listed item by item in [docs/DERIVED.md](docs/DERIVED.md).

The same goes for the analysis of the game's code (`dm/tools/re`): the database, decompiled code, disassembly and strings from it stay in `$DM_ROOT/re`. Code and commits refer to a game function by its `main+0x…` address and what it does, in our own words. The pre-commit guard (`dm/tools/git-hooks/pre-commit`, linked by `./setup.sh`) refuses game file types, analysis databases and lines that look like decompiler output.

These are generated at build time and are listed in the `.gitignore` files of their directories. Do not force-add them:

| Path | What it is |
|---|---|
| `dm/host/dm_hud_exp.h`, `dm/host/dm_bright_levels.inc` | Headers generated from the game's data tables |
| `dm/ios/Gen/` | Generated parts of the iOS package: UI images made from the game files, the embedded game program, the overlay |
| `dm/ios/Sources/TouchArt/dm_native_*.png`, `dm/ios/Sources/dm_touch_native_art.c` | The skill atlas and skill index |
| `dm/ios/DmIOS.xcodeproj/`, `dm/ios/Sources/Info.plist`, `dm/ios/Sources/DmIOS.entitlements`, and `Info.plist` and `RomfsDownloader.entitlements` under `dm/ios/RomfsDownloader/` | The project and configuration generated from `dm/ios/project.yml` |
| `dm/ios/local.env` | Your own signing configuration |
| `dm/host/dmhost`, `*.dSYM/`, `*.bin`, `*.pkl`, `__pycache__/`, the test programs built under each `tests/` | Build output and intermediate files |
| `.iosdd/`, `.iosdd_mac/`, `.iosdd_*/` | Xcode derived-data directories |

Three files are in the repository but get rewritten by `./setup.sh` to match the tools on your machine: `dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h` and `dm/gfx/nvn/c1_full.tsv`. That is expected; never commit those changes.

Everything under the workspace `$DM_ROOT` (default `~/dm-work`) lives outside the repository and never enters it.

### 2. Comments and docs in English

New comments and documentation are in English. Many existing comments are bilingual (Chinese, then English); when you edit one, you may replace the pair with the English text.

Every source file opens with a comment saying what it does; the feature files in `dm/host` also name the addresses in the game program they rely on (offsets into the original 3.3.94012 image).

`dm/third_party/` holds other people's code, kept as it was published; this rule does not apply there.

### 3. On-screen text goes into the string table

Any text of ours that the player can see is registered in `dm/i18n/ui_text.tsv` (key / Chinese / English / … / where used); code refers to the key, never to a literal. The game offers Chinese, so the Chinese column stays. After changing the table, regenerate and commit `dm/host/dm_i18n_tab.h`:

```
python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h
```

`scan_literals.py` stops Chinese literals that are not in the table; it cannot catch hard-coded English, which belongs in the table all the same. Text that only goes to logs does not need an entry. The table's rules are in [dm/i18n/README.md](dm/i18n/README.md).

### 4. Logs

New log lines are in English. A failure line carries the marker `【红】`: the tools match on it (`dm/verify/logred.sh`, `assertlib.py logred`, `dmtest.py`). Many existing log lines are in Chinese and scripts match their text; change one only together with every script that reads it. Prefer an event (`DM_EV`, [dm/verify/event_stream.md](dm/verify/event_stream.md)) for anything a test needs to see.

## Submitting a change

- Say what you changed and why.
- Say which checks you ran and paste their last lines.
- If anything visible on screen changed, attach screenshots taken after the change.
- Confirm that no game content and no engine-analysis output is included.

The pull request template lists these items.
