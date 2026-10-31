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
- **About 30 GB of free disk space while installing from packages** (about 11 GB afterwards; an extracted folder on the same disk needs about 3 GB, and so does an update) and an internet connection to github.com.

## Install

1. Download `D2R-Metal-Launcher-<version>.dmg` from the [releases page](https://github.com/PowerBeef/d2r-metal/releases), open it and drag **D2R Metal Launcher** to Applications. It is signed with the project's Developer ID and notarized by Apple, so it opens normally.
2. Open it, choose your game packages and `prod.keys` (or drop them on the window), or your extracted folder, and press **Install**.
3. The first install takes about 10–15 minutes on a recent Mac. The window shows each step:

| Step | What it does | Time (M6) |
|---|---|---|
| Downloading D2R Metal | the release's two packs (about 50 MB the first time; an update usually downloads only the runtime pack, about 8 MB) | depends on your connection |
| Unpacking your game files | reads the packages with your keys and keeps the update's program and data | about 5 min (instant from an extracted folder on the same disk) |
| Preparing the game program | rewrites the game's program for the Mac | 15 s |
| Extracting the shaders | takes the game's 162 shader packs out of its data | 20 s |
| Translating the shaders | turns the game's 6,927 shaders into Metal source | about 2 min |
| Checking the shaders | the table of shaders the game may compile at run time | 10 s |
| Building the interface | the overlay with the project's own pages and controls | 20 s |
| Reading the game tables | two small tables from the game's data | 5 s |
| Assembling D2R Metal | `D2R Metal.app` in the launcher's own folder | 5 s |
| Compiling the shaders for this Mac | macOS compiles every shader once and keeps them | about 4 min |

You can keep using your Mac. **Stop** (or Esc) stops the running step cleanly; quitting the launcher asks first and does the same. The next run
continues where it stopped, and if your game files were already unpacked the welcome screen offers **Continue** without choosing them again.

4. Press **Play**.

The launcher is how you open the game, as Battle.net is for Blizzard's games: D2R Metal itself is not in Applications or in app search. If you open
it some other way (a Dock icon kept from an earlier session, say), it opens the launcher instead. An install made by launcher 0.9.6 or earlier,
which put the app in Applications, is moved into the launcher's folder the first time a newer launcher opens; the old copy becomes the previous version.

## Updates

- The launcher checks GitHub each time you open it. The game's main menu also shows **New version N** when a release is out; selecting it opens the launcher.
- **The launcher updates itself** first when a release brings a newer launcher, as Battle.net does: it downloads it, checks it (the checksum, then the
  app's identity, build and the project's Developer ID signature), swaps it into its own place in one step, keeps the old one
  (`build/launcher-previous.noindex`) and restarts. A launcher run from its disk image, or from a folder it cannot write to, says so and links to the
  releases page instead; move it to Applications to let it update itself.
- An update downloads the new release and redoes only what that release changed. Usually that is only the app itself (seconds). A release that changes the shader translation redoes it and the compile (a few minutes).
- **Quit D2R Metal first:** an update, a repair or going back changes the game's files, so while the game runs the launcher asks you to quit it (leave your game first so your progress is saved).
- **Go Back to the Previous Version** (under Manage) restores the app from before the last update. The version you went back from is then offered under Manage (**Update to …**) instead of as the main button, until a newer one comes out.
- There is one channel: the release GitHub shows as the project's latest. **Check for Updates** (under Manage, or in the app menu) asks GitHub again.
- **After a macOS update**, macOS's Metal compiler is new, so the game compiles its shaders again the first time it starts. A small window shows the progress for a few minutes; this happens once.

## Where things go

| What | Where |
|---|---|
| The game app | `~/Library/Application Support/D2R Metal/Game.noindex/D2R Metal.app` (**Show Game in Finder** under Manage; `.noindex` keeps it out of Spotlight and app search) |
| Game files, build outputs, the release packs, logs | `~/Library/Application Support/D2R Metal/` |
| Your characters and settings | `~/Library/Application Support/D2R Mobile/` (the folder keeps the project's former name, so saves from earlier builds carry over) |

The **Manage** list beside the game holds the rest:

- **Repair** runs every step again for the version you have (never an update or a downgrade), from that version's own packs, which it first checks file by file (a damaged pack is downloaded again; intact ones need no network). If your game files are damaged, it asks you to choose them again.
- **Remove the Game…** moves the game files, the build outputs and the app to the Trash. Your characters and settings are never touched.

The launcher keeps the current version and the previous one, and deletes what neither uses after each successful install or update: older
releases' packs, shader libraries and build outputs, and logs beyond the last 20.

**iCloud:** an app installed by the launcher keeps its saves on this Mac only. iCloud save sync needs the app to be signed with an Apple
provisioning profile, and the launcher signs the app it builds on your Mac without one. To move a character between devices, import it by hand
([MAC.md](MAC.md), "Saves, logs and iCloud").

## Privacy

- The launcher talks only to GitHub: github.com for the release description and its files (which GitHub serves from its download host,
  `objects.githubusercontent.com` / `release-assets.githubusercontent.com`). The game's main menu reads one small version file from the same
  releases.
- Nothing is uploaded.
- Your game packages are read where they are, never copied or kept. The keys are used only while unpacking, from a temporary copy in a private
  folder that is deleted when unpacking ends (also after a crash: the next run deletes what a killed one left). The tickets in the packages,
  which hold title keys, are moved into that private folder as soon as they are unpacked. The copy of the keys leaves out lines of an impossible
  length, which some key dumps contain and hactool refuses.

## Trust

What the launcher installs comes only from the project:

- **The release description is signed.** Every release's `manifest.json` comes with `manifest.json.sig`, an Ed25519 signature made with the
  project's release key, whose private half exists only in the maintainer's keychain. The launcher checks it against the public key built into
  it before it reads anything in the description, and refuses an unsigned or altered one.
- **The files are checked.** Every downloaded file must match the checksum in the signed description, and the launcher's packs keep a list of
  their files' checksums, which Repair checks.
- **The programs are the project's.** The launcher itself is signed with the project's Developer ID and notarized by Apple. It reads its own
  team from its signature and requires that same team's Developer ID signature on every program it runs or installs: the packs' main programs
  and a new launcher (whatever the description says). Names in the description can't point outside the launcher's own folders, and files are
  only fetched over https.
- **Test builds:** a launcher built from source (`dm/tools/launcher_app.sh`, ad-hoc signed) has no team; it checks the team the description
  names, if any, and a signature whenever one is published.

## If something goes wrong

- **Show Log** opens the log of the last run (`~/Library/Application Support/D2R Metal/logs/`; `engine-stderr.log` there has anything the engine
  printed before its own log was open). Please attach it to an issue. The error shown in the window is the engine's own message, with its cause
  (a full disk, a folder macOS refused access to, a step that stopped responding).
- **"Another install is running for this game"**: only one install can work on the game's folder at a time (another copy of the launcher?).
- **"Your game files are damaged or incomplete"**: a resource archive is missing or cut short (checked by size at every run). Choose your game
  packages, or an extracted folder, again: the damaged files are replaced.
- **"…exists and was not installed by the launcher"**: there is already a D2R Metal where the launcher puts the game, one you built yourself (or a link to one). The launcher never replaces it: move it aside.
- **"…not from version 1.0.34.0"**: the game files are another version. The launcher checks the three program files by checksum.
- **The keys are refused**: see [Getting your game files](GAME_FILES.md). The launcher reads at most 1 MB of the file you choose as `prod.keys`
  and needs `header_key` and the master keys in it. A personalized ticket needs the title key in your own `~/.switch/title.keys`.

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
| Runtime pack (`d2r-metal-runtime-<version>.tar.gz`, about 8 MB) | the host (`D2RMobile`, built without the game's files), the install scripts, the deswizzle library, the license texts and `manifest.json` | |
| Tools pack (`d2r-metal-tools-<n>.tar.gz`, about 40 MB) | CPython 3.14 (without pip, IDLE or Tk) with lz4, capstone 5, numpy and Pillow, the self-contained shader translator (`dmsh_xlate`), spirv-cross, hactool, and their license texts | versioned apart: `tools_version` in `dm/tools/release_pins.txt` moves whenever an input changes (a pin, `release_requirements.txt`, `release_nuget.tsv`, the translator's sources), and `release_mac.sh` refuses a changed input under an old version. A release points at the tools pack an earlier published release carries when that copy is identical, else it carries it again |

**The engine on a player's Mac** (`dm/tools/install.py`, `docs` in its header): one engine at a time per root (a lock the kernel drops with the
process); every tool runs in its own process group, and a stop (Stop, quitting, the launcher going away) stops it before the engine exits;
a tool silent for too long fails its step as stalled; every error reaches the launcher with a code and its cause; the app is swapped in one
move that a stop never cuts; outputs an installed app uses are never rewritten in place (one folder per stamp, built under `.part`); and
`status --json` reports the game files and both apps.

**The shaders on a player's Mac:**
- The shader chain is of the source form (`dmsh_chain.py --update-lock --runtime`): the libraries hold Metal source, which the game compiles with macOS's own compiler (`nvn_mtl.m` `lib_load`).
- `D2RMobile --warm-shaders` fills macOS's per-app cache once, with the libraries in two worker processes, then the preset pipelines (`pso_manifest.tsv`).
- Measured on an M6 against the Xcode-compiled libraries: the same pictures (a replayed town frame differs in 6 of 1.44 M pixels) and the same frame pacing.

**Building a release** (on this Mac; GitHub has no macOS 27 runners):

```bash
dm/tools/release_mac.sh 1.0.0 --notes notes.md   # signed, notarized, uploaded as a draft release to publish on github.com
dm/tools/release_mac.sh --check-latest           # after publishing: GitHub's "latest release" is that release, with its signed manifest
dm/tools/release_mac.sh 1.0.0 --unsigned         # a local test release in $DM_ROOT/release/1.0.0
dm/tools/release_mac.sh 1.0.0 --no-upload        # a signed dry run (it still notarizes: the app goes to Apple's notary service)
```

- An upload needs HEAD on `origin/main` and a build above every published release's. `--min-build` sets the oldest launcher that may install the
  release (default: `LAUNCHER_MIN_BUILD` in `install.py`). A draft left by a failed run is replaced with `--replace-draft`.
- The release's packs pass `dm/tools/macho_deps.py` (every library they load is macOS's or their own, `@rpath` resolved), carry every license
  text (`docs/THIRD_PARTY_NOTICES.md`), and are built from pinned inputs (checksums for the Python build, its license archive, the .NET SDK,
  the wheels, the NuGet packages, spirv-cross and the hactool commit).
- **The release key:** `swift dm/tools/manifest_sign.swift keygen` made it once (private key in the login keychain, service
  `org.d2rmetal.manifest-signing`); `release_mac.sh` signs `manifest.json` with it. **Rotating it:** make a new key on the release Mac (delete the
  old item first), add its public key (`manifest_sign.swift public`) to `ManifestSignature.publicKeys` beside the old one, ship that launcher in a
  release signed with the old key, sign the next releases with the new key, and drop the old public key a release later.
- **Never mark another release as "latest":** the stable channel and the in-game notice read GitHub's latest release. The `assets` release (README
  images) is a pre-release for that reason.

**Testing the launcher against a local release:**

```bash
DM_LAUNCHER_FEED=file://$HOME/dm-work/release/1.0.0/manifest.json DM_LAUNCHER_ROOT=/tmp/d2r-test \
  DM_LAUNCHER_BUNDLE_ID=org.d2rmobile.mac.launchertest open -n "D2R Metal Launcher.app"
```

- `DM_LAUNCHER_ROOT` keeps the install apart from a player's root; the game app goes into its `Game.noindex` folder.
- `DM_LAUNCHER_BUNDLE_ID` gives the installed app its own preferences and Metal cache.
- Start the installed app with `DM_APP_SUPPORT` to keep its saves apart (a test copy also skips the hand-over to the launcher).
- `DM_LAUNCHER_AUTORUN=update` (or `repair`, or `selfupdate`) runs that job as its button does, prints `LAUNCHER-AUTORUN OK|RED …` and quits.
- `DM_LAUNCHER_PREVIEW=welcome|empty|offline|continue|damaged|working|stopping|home|reverted|game|failed` shows one screen with sample data (screenshots). `DM_LAUNCHER_PREVIEW_SIZE=<width>x<height>` sets the window's size, to check how the layout scales.
- A launcher built from source is build 0 unless `DM_LAUNCHER_BUILD` says otherwise: it never updates itself and never looks newer than a
  release to macOS. `DM_LAUNCHER_APP_ID` builds it with another bundle id.
- A release launcher reads `DM_LAUNCHER_FEED` too, but every manifest it reads must carry the release key's signature; it ignores and removes the
  persistent `DMLauncherFeed` default a test build uses.

**Testing a release:** before publishing, update a copy of a real install to it:

```bash
python3 dm/verify/launcher_update.py --feed file://$HOME/dm-work/release/1.0.0/manifest.json
```

It clones the install root (APFS clones: instant, no extra space), runs the installed launcher's Update on the copy with a test bundle ID,
then checks the engine's steps, the install record, the new app (version, icon, signature, host, paths), the previous version kept for
revert, the hand-over to the launcher (to a test launcher, never the installed one), a game scenario on the updated app and the way back to the
previous version; then a stop in the middle of a repair (no process left, the lock released, the next run resumes), a game running during an
update (refused until it quits), an offline feed and a run killed outright (the next run sweeps what it left). Last, it checks that the real
install, its app, the installed launcher, the saves and Applications did not change. `--keep` keeps the copy; `--no-game` skips the in-game
checks; `--launcher <app>` runs the jobs with another launcher (the release's own, unpacked) while the installed one is still checked
untouched; `--no-hard` skips the stop, kill, running-game and offline cases; `--clean-mac` runs the update and the repair in a sandbox that hides Homebrew, `/usr/local`, MacPorts, the python.org framework,
pyenv, conda, Xcode and the command line tools, as on a player's Mac.

Then the launcher's own update:

```bash
python3 dm/verify/launcher_selfupdate.py --feed file://$HOME/dm-work/release/1.0.0/manifest.json
```

It turns the release's launcher into an "old" one (build 1) in a throwaway folder, checks that it refuses a release whose launcher zip has a
wrong checksum, one whose manifest signature does not verify, and one whose manifest names no team but carries an ad-hoc launcher (refused by a
launcher with a team), then that it installs the release's launcher in its own place, keeps the old one, and restarts into the new one; last,
that the installed launcher is untouched and is still the one macOS opens.

The Swift side has its own tests (`swift test --package-path dm/launcher --filter CoreTests`, in the gate's `unit_packaging` step) and an end
to end install from a feed without the window (`DM_LAUNCHER_TEST_FEED=file://…/manifest.json DM_LAUNCHER_TEST_GAME=<extracted folder> swift test
--package-path dm/launcher --filter installFromFeed`, several minutes).

**Licenses of what the launcher and the packs ship:** [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists every component, its license
and where its text is (each pack's `licenses/` folder, the launcher's `Contents/Resources/Licenses`).
