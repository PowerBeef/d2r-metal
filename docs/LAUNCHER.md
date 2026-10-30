# D2R Metal Launcher

> [!NOTE]
> **Coming soon.** The launcher is not released yet; it arrives with the project's first release. Until then, build the Mac App from source ("Getting started" in the [README](../README.md#getting-started)). This page describes the launcher as it will ship.

The launcher installs D2R Metal on your Mac from your own game files and keeps it up to date from this project's GitHub releases. It needs no Xcode, Homebrew, .NET or Python on your Mac: everything it runs comes with the release, and the parts made from your game files are built on your Mac and never leave it.

## What you need

- **A Mac with Apple silicon and macOS 27 or later.**
- **Your own copy of the game:** the Nintendo Switch release of Diablo II: Resurrected with the Reign of the Warlock expansion, updated to **1.0.34.0**, dumped from your own console ([Getting your game files](GAME_FILES.md)).
- **One of these:**
  - the game packages (`.nsp` or `.xci`: the base game and the 1.0.34.0 update) and your console's `prod.keys`;
  - or a folder where they are already extracted (`ExeFS` and `RomFS`, as `dm/tools/extract_game.sh` or `./setup.sh` leave them).
- **About 30 GB of free disk space while installing** (about 13 GB afterwards) and an internet connection to github.com.

## Install

1. Download `D2R-Metal-Launcher-<version>.dmg` from the [releases page](https://github.com/PowerBeef/d2r-metal/releases), open it and drag **D2R Metal Launcher** to Applications. It is signed with the project's Developer ID and notarized by Apple, so it opens normally.
2. Open it, choose your game packages and `prod.keys` (or drop them on the window), or your extracted folder, and press **Install**.
3. The first install takes about 10–15 minutes on a recent Mac. The window shows each step:

| Step | What it does | Time (M6) |
|---|---|---|
| Downloading D2R Metal | the release's two packs (about 80 MB the first time) | depends on your connection |
| Unpacking your game files | reads the packages with your keys and keeps the update's program and data | a few minutes (instant from an extracted folder on the same disk) |
| Preparing the game program | rewrites the game's program for the Mac | 10 s |
| Extracting / translating the shaders | turns the game's 6,927 shaders into Metal source | about 2 min |
| Building the interface | the overlay with the project's own pages and controls | 25 s |
| Reading the game tables | two small tables from the game's data | about 1 min |
| Assembling D2R Metal | `D2R Metal.app` in the launcher's own folder | instant |
| Compiling the shaders for this Mac | macOS compiles every shader once and keeps them | about 4 min |

You can keep using your Mac. If you quit midway, the next run continues where it stopped.

4. Press **Play**.

The launcher is how you open the game, as Battle.net is for Blizzard's games: D2R Metal itself is not in Applications or in app search. If you open
it some other way (a Dock icon kept from an earlier session, say), it opens the launcher instead. An install made by launcher 0.9.6 or earlier,
which put the app in Applications, is moved into the launcher's folder the first time a newer launcher opens; the old copy becomes the previous version.

## Updates

- The launcher checks GitHub each time you open it. The game's main menu also shows **New version N** when a release is out; selecting it opens the launcher.
- An update downloads the new release and redoes only what that release changed. Usually that is only the app itself (seconds). A release that changes the shader translation redoes it and the compile (a few minutes).
- **Go Back to the Previous Version** (gear menu) restores the app from before the last update.
- **Updates: With pre-releases** (under Your Game) also offers pre-releases. **Check for Updates** in the gear menu asks GitHub again.
- **After a macOS update**, macOS's Metal compiler is new, so the game compiles its shaders again the first time it starts. A small window shows the progress for a few minutes; this happens once.

## Where things go

| What | Where |
|---|---|
| The game app | `~/Library/Application Support/D2R Metal/Game.noindex/D2R Metal.app` (**Show Game in Finder** in the gear menu; `.noindex` keeps it out of Spotlight and app search) |
| Game files, build outputs, the release packs, logs | `~/Library/Application Support/D2R Metal/` |
| Your characters and settings | `~/Library/Application Support/D2R Mobile/` (the folder keeps the project's former name, so saves from earlier builds carry over) |

The gear menu next to Play holds the rest:

- **Repair** runs every step again and rebuilds whatever is missing or damaged.
- **Remove the Game…** moves the game files, the build outputs and the app to the Trash. Your characters and settings are never touched.

## Privacy

- The launcher talks only to github.com: the release description and its files.
- Nothing is uploaded.
- Your game packages are read where they are, never copied or kept. The keys are used only while unpacking, from a temporary copy in a private folder that is deleted when unpacking ends. That copy leaves out lines of an impossible length, which some key dumps contain and hactool refuses.
- Every downloaded file is checked against the checksum in the release's description. The two packs' main programs must also carry the project's Developer ID signature.

## If something goes wrong

- **Show Log** opens the log of the last run (`~/Library/Application Support/D2R Metal/logs/`). Please attach it to an issue.
- **"…exists and was not installed by the launcher"**: there is already a D2R Metal where the launcher puts the game, one you built yourself (or a link to one). The launcher never replaces it: move it aside.
- **"…not from version 1.0.34.0"**: the game files are another version. The launcher checks the three program files by checksum.
- **The keys are refused**: see [Getting your game files](GAME_FILES.md). A personalized ticket needs the title key in your own `~/.switch/title.keys`.

## For maintainers

**How it works:**
- The launcher (`dm/launcher`, SwiftUI) is a thin front end. The work is `dm/tools/install.py`, run with the tools pack's Python. It can be run by hand:

  ```bash
  python3 dm/tools/install.py install --root <root> --runtime <runtime pack> --tools <tools pack> --game <pkg>… --keys <prod.keys> --app-dir <dir>
  ```

- The install root is a workspace like `~/dm-work` (its `DM_ROOT`), so `extract_game.sh`, `product.sh`, `dmx.py`, `dmsh_build.py`, `c1_guard.py` and `gen_romfs_overlay.py` run on it unchanged.

**The release's packs:**

| Pack | What it holds | Notes |
|---|---|---|
| Runtime pack (`d2r-metal-runtime-<version>.tar.gz`, about 10 MB) | the host (`D2RMobile`, built without the game's files), the install scripts, the deswizzle library and `manifest.json` | |
| Tools pack (`d2r-metal-tools-<n>.tar.gz`, about 80 MB) | CPython 3.14 with lz4, capstone 5, numpy and Pillow, the self-contained shader translator (`dmsh_xlate`), spirv-cross, hactool, and their licenses | versioned apart (`dm/tools/release_pins.txt`): a release reuses the last one unless a pin changed |

**The shaders on a player's Mac:**
- The shader chain is of the source form (`dmsh_chain.py --update-lock --runtime`): the libraries hold Metal source, which the game compiles with macOS's own compiler (`nvn_mtl.m` `lib_load`).
- `D2RMobile --warm-shaders` fills macOS's per-app cache once, with the libraries in two worker processes, then the preset pipelines (`pso_manifest.tsv`).
- Measured on an M6 against the Xcode-compiled libraries: the same pictures (a replayed town frame differs in 6 of 1.44 M pixels) and the same frame pacing.

**Building a release** (on this Mac; GitHub has no macOS 27 runners):

```bash
dm/tools/release_mac.sh 1.0.0 --notes notes.md   # signed, notarized, uploaded as a draft release to publish on github.com
dm/tools/release_mac.sh 1.0.0 --unsigned         # a local test release in $DM_ROOT/release/1.0.0
```

**Testing the launcher against a local release:**

```bash
DM_LAUNCHER_FEED=file://$HOME/dm-work/release/1.0.0/manifest.json DM_LAUNCHER_ROOT=/tmp/d2r-test \
  DM_LAUNCHER_BUNDLE_ID=org.d2rmobile.mac.launchertest open -n "D2R Metal Launcher.app"
```

- `DM_LAUNCHER_ROOT` keeps the install apart from a player's root.
- `DM_LAUNCHER_BUNDLE_ID` gives the installed app its own preferences and Metal cache.
- Set the app folder to a test folder in the launcher's settings, and start the installed app with `DM_APP_SUPPORT` to keep its saves apart.

**Licenses of what the packs ship:**
- CPython (PSF);
- python-build-standalone (MIT);
- numpy (BSD-3);
- Pillow (MIT-CMU);
- capstone (BSD-3);
- lz4 (BSD-3);
- .NET runtime (MIT);
- Ryujinx's shader translator (MIT);
- SPIRV-Cross (Apache-2.0);
- hactool (ISC).

Their license texts are in the tools pack's `licenses/` folder and in the packages' own metadata.
