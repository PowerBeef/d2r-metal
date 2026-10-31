<p align="center">
  <img src="docs/images/r2-hero.jpg" width="100%" alt="D2R Metal: Diablo II: Resurrected and Reign of the Warlock. The ultimate way to play on Apple silicon Macs, iPhones and iPads">
</p>

<p align="center">
  <img alt="macOS 27 and later" src="https://img.shields.io/badge/macOS-27%2B-a07638?style=for-the-badge&labelColor=1a110d">
  <img alt="iOS and iPadOS 27 and later" src="https://img.shields.io/badge/iOS%20%2F%20iPadOS-27%2B-a07638?style=for-the-badge&labelColor=1a110d">
  <img alt="Metal 4 and MetalFX" src="https://img.shields.io/badge/Metal%204-MetalFX-7a1d12?style=for-the-badge&labelColor=1a110d">
  <img alt="Plays offline" src="https://img.shields.io/badge/plays-offline-7a1d12?style=for-the-badge&labelColor=1a110d">
  <a href="https://github.com/PowerBeef/d2r-metal/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/PowerBeef/d2r-metal?style=for-the-badge&labelColor=1a110d&color=7a1d12&label=release"></a>
  <img alt="MIT license" src="https://img.shields.io/badge/license-MIT-5a4a3a?style=for-the-badge&labelColor=1a110d">
</p>

<p align="center"><b>Diablo II: Resurrected with Reign of the Warlock, running natively on Apple silicon,<br>drawn sharper than the Switch ever showed it, at a steady 60 fps.</b></p>

D2R Metal runs the game's own program, the Nintendo Switch release, built from files you own, directly on your Mac, iPhone or iPad. Every frame goes through a renderer rebuilt on Metal 4. The interface is drawn at your screen's own resolution. MetalFX enlarges the 3D world. The game's PC anti-aliasing and texture filtering are switched on, and the scenery casts its shadows. Around that picture: a Mac App that behaves like a Mac game, touch and controller play on iPhone and iPad, saves that survive quitting, the background and a full disk, and every setting inside the game's own Options.

It is not an emulator, cloud gaming or streaming, and it plays fully offline. The game itself stays as Blizzard shipped it.

<p align="center"><b><a href="https://github.com/PowerBeef/d2r-metal/releases/latest">Download D2R Metal Launcher 1.0</a></b> for the Mac: choose your game files, press Install, and it builds the game on your Mac and keeps it up to date. No Xcode, Homebrew or developer account. <a href="#getting-started">Getting started</a>.</p>

<p align="center">
  <img src="docs/images/r2-plaques.jpg" width="100%" alt="Native: Apple silicon, Metal 4, no emulation. Sharper: MetalFX, SMAA, 16x filtering, native-resolution interface. 60 fps: steady on the Mac and on iPhone. As shipped: rules, drops, skills, economy and stash.">
</p>

<h2 align="center" id="beyond-the-switch"><img src="docs/images/r2-plate-beyond.jpg" width="100%" alt="Beyond the Switch"></h2>

The Switch draws Diablo II: Resurrected within the limits of a handheld: 1600 × 900 docked, 1280 × 720 in your hands. D2R Metal draws the same game with what Apple silicon can do, on the Mac and on iPhone and iPad. The comparisons below are windows of the frame at 1:1 pixels, so on a Retina screen they show close to every pixel of both sides.

<p align="center">
  <img src="docs/images/r2-split-mac-1to1.jpg" width="100%" alt="The Rogue Encampment at 1:1 pixels: the left half is the Switch's docked 1600 × 900 output enlarged, the right half D2R Metal's Recommended on a 2560 × 1440 Mac">
</p>

<p align="center"><sub><b>Mac, 2560 × 1440 screen, M6.</b> The same spot, a window of the frame at 1:1 pixels. Left: the Switch's docked output (1600 × 900, FXAA, original filtering, character shadows only), enlarged to the same size as a TV or monitor would. That is the Switch at its sharpest: in busy scenes it also lowers its 3D resolution, which this comparison leaves out. Right: D2R Metal's Recommended.</sub></p>

The cards below show each gain at 1:1 pixels from the same frame. The first compares the Switch's settings with Recommended; each of the others changes a single setting, so you see exactly what it does.

<p align="center">
  <img src="docs/images/r2-card-interface.jpg" width="100%" alt="Interface at your screen's resolution: the life text and skill bar stretched from 1600 × 900 beside the same HUD drawn at native 2560 × 1440">
</p>
<p align="center">
  <img src="docs/images/r2-card-metalfx.jpg" width="100%" alt="MetalFX on the 3D world: the waypoint enlarged by the game's own upscale beside the same waypoint enlarged by MetalFX">
</p>
<p align="center">
  <img src="docs/images/r2-card-smaa.jpg" width="100%" alt="SMAA anti-aliasing: the palisade and stone wall with FXAA beside the same with SMAA">
</p>
<p align="center">
  <img src="docs/images/r2-card-filtering.jpg" width="100%" alt="16x texture filtering: ground near the forge with the original filtering beside the same ground with 16x anisotropic filtering">
</p>
<p align="center">
  <img src="docs/images/r2-card-shadows.jpg" width="100%" alt="Environment shadows: Charsi's forge lit with character shadows only, beside the same forge where the shelter and the stone wall cast shadows">
</p>

**What the picture gains over the Switch:**

- **Your screen's resolution.** On the Mac the interface and text are drawn at the display's own width, up to 3840 pixels, and are never stretched. Only the 3D world is scaled, by a 3D Resolution chosen for your chip.
- **MetalFX.** Apple's upscaler enlarges the 3D world in place of the game's bilinear stretch, on the Mac and on iPhone and iPad.
- **SMAA.** The game's own PC anti-aliasing, in place of the Switch's FXAA.
- **16x anisotropic filtering** on the game's textures (never on shadows or the interface), so the ground stays sharp as it recedes.
- **Environment shadows** on the Mac. Trees, walls, fences and buildings cast shadows as on PC; the Switch keeps shadows to characters.
- **Finer shadows.** Shadow Quality raises the shadow map from the Switch's 2100 pixels a side to the PC's 4096 (Ultra, in Mac Recommended), so characters' self-shadowing and the shadows on the ground keep their detail at your screen's resolution.
- **Accurate color.** The picture is tagged sRGB, the color space the game was made in, so wide-gamut (P3) screens show it as intended instead of stretching it to their wider colors.

<p align="center">
  <img src="docs/images/r2-split-iphone-1to1.jpg" width="100%" alt="The Rogue Encampment at 1:1 iPhone 17 Pro pixels: the left half is the Switch's handheld 1280 × 720 output enlarged, the right half D2R Metal's iPhone Recommended">
</p>

<p align="center"><sub><b>iPhone 17 Pro, 2622 × 1206,</b> a window at 1:1 phone pixels. Left: the Switch's handheld output (1280 × 720, FXAA, original filtering), enlarged to the phone's size. Right: D2R Metal's iPhone Recommended (3D at 75% enlarged by MetalFX, SMAA, 16x filtering). Drawn by the same iOS app running on a Mac at the phone's resolution and settings; the phone draws the same picture.</sub></p>

On the M6 the Mac's Recommended (native 2560 × 1440, everything above switched on) takes about 7.3 ms of GPU time per frame in town, under half of the 16.7 ms a 60 fps frame allows. On an iPhone 17 Pro, MetalFX costs about 0.6 ms, and with SMAA and 16x filtering the phone still holds 60 fps with frame pacing and CPU time unchanged. Details are under [Built to be measured](#built-to-be-measured).

<h2 align="center" id="across-sanctuary"><img src="docs/images/r2-plate-sanctuary.jpg" width="100%" alt="Across Sanctuary"></h2>

One stop in every act, each split at 1:1 pixels from the same moment: the Switch's docked output on the left, D2R Metal's Recommended on the right, on a 2560 × 1440 Mac.

<p align="center">
  <img src="docs/images/r2-zone-catacombs.jpg" width="100%" alt="Act I, the Catacombs: torchlight in a dungeon, Switch docked on the left, D2R Metal on the right">
</p>
<p align="center">
  <img src="docs/images/r2-zone-lutgholein.jpg" width="100%" alt="Act II, Lut Gholein: desert sun and long shadows, Switch docked on the left, D2R Metal on the right">
</p>
<p align="center">
  <img src="docs/images/r2-zone-arcane.jpg" width="100%" alt="Act II, the Arcane Sanctuary: marble walkways over the void, Switch docked on the left, D2R Metal on the right">
</p>
<p align="center">
  <img src="docs/images/r2-zone-flayer.jpg" width="100%" alt="Act III, the Flayer Jungle: foliage and ruins, Switch docked on the left, D2R Metal on the right">
</p>
<p align="center">
  <img src="docs/images/r2-zone-riverflame.jpg" width="100%" alt="Act IV, the River of Flame: lava light in Hell, Switch docked on the left, D2R Metal on the right">
</p>
<p align="center">
  <img src="docs/images/r2-zone-harrogath.jpg" width="100%" alt="Act V, Harrogath: snow, stone and timber, Switch docked on the left, D2R Metal on the right">
</p>

**More of Sanctuary at Recommended:**

<p align="center">
  <img src="docs/images/r2-gallery-darkwood.jpg" width="49%" alt="Act I, Dark Wood at D2R Metal's Recommended">
  <img src="docs/images/r2-gallery-faroasis.jpg" width="49%" alt="Act II, Far Oasis at D2R Metal's Recommended">
  <img src="docs/images/r2-gallery-docks.jpg" width="49%" alt="Act III, Kurast Docks at D2R Metal's Recommended">
  <img src="docs/images/r2-gallery-pandemonium.jpg" width="49%" alt="Act IV, the Pandemonium Fortress at D2R Metal's Recommended">
  <img src="docs/images/r2-gallery-frigid.jpg" width="49%" alt="Act V, Frigid Highlands at D2R Metal's Recommended">
  <img src="docs/images/r2-gallery-arreat.jpg" width="49%" alt="Act V, Arreat Plateau at D2R Metal's Recommended">
</p>

<h2 align="center" id="a-true-mac-game"><img src="docs/images/r2-plate-mac.jpg" width="100%" alt="A True Mac Game"></h2>

- **Full screen at your display's own resolution** from the first launch. View → Full Screen (⌃⌘F) switches, and the App remembers it. The window can be resized freely, and the picture fills it at its own aspect ratio.
- **Mouse, trackpad, keyboard and controller, all at once.** The wheel and trackpad scroll the game's lists, scroll bars and sliders drag, and the keyboard or a controller moves a highlight drawn like the game's own. Button hints follow what you hold: Xbox, PlayStation or Switch.
- **The pointer steps aside.** It hides after resting over the game for a few seconds and returns the moment the mouse moves.
- **It saves like a Mac app should:** on every quit, from the Dock, at logout, even with a text box open. Minimizing counts as going to the background.
- **iCloud save sync** with your iPhone or iPad, in an App you build and sign with a paid developer account. An app the launcher installs keeps its saves on the Mac; moving a character over by hand takes a minute ([docs/MAC.md](docs/MAC.md#saves-logs-and-icloud)).

**Typing with a controller.** Naming a character or entering an amount of gold brings up an in-game keyboard in the game's own style and fonts, with button hints for your controller. Press a key, click or touch the screen and it steps aside for a regular text box, keeping what you typed.

<p align="center">
  <img src="docs/images/r2-frame-controller-keyboard.jpg" width="80%" alt="Naming a character with a controller on the Mac: the in-game keyboard under the game's name box">
</p>

The Mac guide, with keys, saves, iCloud and moving saves over from an iPhone, is [docs/MAC.md](docs/MAC.md).

<h2 align="center" id="made-for-iphone-and-ipad"><img src="docs/images/r2-plate-iphone.jpg" width="100%" alt="Made for iPhone and iPad"></h2>

- **Touch controls and controller support,** with the game's own controller interface.
- **The sharper picture on the go:** 3D Resolution from 50 to 100% enlarged by MetalFX, SMAA, 16x filtering and Accurate color. Recommended is 3D at 75% with all of them on.
- **Smooth when it gets busy:** by default the 3D resolution drops for a moment while frames drop, then comes back. **Frame Rate** has a 30 fps power saver.
- **Saves when the app goes to the background,** so swiping it away loses nothing, and **iCloud sync** carries your characters between devices.
- **Older iPhones and iPads** (A14 to A16) play with a [Legacy Device Pack](docs/LEGACY_DEVICES.md).

| At a glance | Mac | iPhone / iPad |
|---|---|---|
| Controls | Keyboard, mouse, trackpad, controller | Touch, controller |
| Picture drawn at | The screen's own resolution (Native), 3D at 50–100% | The screen's full resolution, 3D at 50–100% |
| MetalFX enlarges | The 3D world | The 3D world |
| Environment shadows | On in Recommended | Available, off until measured on a phone |
| When frames drop | Keeps the 3D resolution | Lowers it for a while |
| Saves | On every quit and when minimized | When sent to the background |
| System | macOS 27 or later | iOS / iPadOS 27 or later |

<h2 align="center" id="picture-settings"><img src="docs/images/r2-plate-settings.jpg" width="100%" alt="Picture Settings"></h2>

Every setting lives in the game's own Options, drawn with its own selectors, checkboxes and text, on five tabs: **Video**, **Audio**, **Gameplay**, **Settings** and **Extras**.

- **Video**:
  - On the Mac: Display Mode, Resolution (1600 × 900 to 2560 × 1440, or Native), Color (Accurate / Vivid), Graphic Presets, 3D Resolution (100 down to 50%) and Upscaling (Standard / MetalFX).
  - On iPhone and iPad: 3D Resolution (50 to 100%), Color, Graphic Presets and Upscaling.
  - On both: **Anti-Aliasing** (the game's FXAA or SMAA), **Anisotropic Filtering** (Original up to 16x), **Environment Shadows** and **Shadow Quality** (Standard, the Switch's own shadow map, up to Ultra; both take effect at the next start), **Frame Rate** (60 fps, or 30 to save power) and **Frame Drops** (lower the 3D resolution while frames drop, or keep it).
- **Recommended** is worked out for your device:
  - on a Mac, full screen at Native with the 3D resolution chosen for its chip, MetalFX, SMAA, 16x filtering, Accurate color, Environment Shadows and Ultra shadows;
  - on iPhone and iPad, 3D at 75% with MetalFX, SMAA, 16x filtering and Accurate color.
- **Settings** holds the game's own **Offline Difficulty Scaling**, which sets the player count and takes effect at once.
- **Apply, Discard and Default** work like the game's own option pages; Default resets only the tab you are on.

<p align="center">
  <img src="docs/images/r2-frame-video-display.jpg" width="49%" alt="The Mac Video tab at Recommended: Fullscreen, Native, Accurate color, Recommended preset, 3D Resolution 75%">
  <img src="docs/images/r2-frame-video-graphics.jpg" width="49%" alt="The Mac Video tab's Graphics rows: MetalFX, SMAA, 16x filtering, Environment Shadows On, Frame Rate and Frame Drops">
</p>
<p align="center">
  <img src="docs/images/r2-frame-settings.jpg" width="49%" alt="The Settings tab with the game's own Offline Difficulty Scaling row">
</p>

<h2 align="center" id="saves-you-can-trust"><img src="docs/images/r2-plate-saves.jpg" width="100%" alt="Saves You Can Trust"></h2>

- The Mac App saves on every quit, including from the Dock, at logout and with a text box still open; minimizing is treated like an iPhone going to the background.
- The iPhone app saves when it goes to the background, so swiping it away doesn't lose progress.
- A failed save (a full disk, for example) is refused with a message, and the next one commits; one failure can't silently stop every later save.
- A damaged shared stash can't hang iCloud sync.

These came out of a full audit of the port, a bug hunt with sanitizers and Metal validation, and a study of how the game itself saves.

<h2 align="center" id="the-game-as-it-shipped"><img src="docs/images/r2-plate-shipped.jpg" width="100%" alt="The Game as It Shipped"></h2>

> **D2R Metal changes the platform, not the game.**

The rules, drop rates, economy, skills and their caps, the stash with its tabs, the automap and the difficulty are the ones Blizzard shipped, with no gameplay systems added on top. The game's logic is left alone apart from the part of a community fix that lets it run offline; the port's own work is in how the game runs and how it is drawn.

A few conveniences sit on the **Extras** tab of Options, each one a switch you can turn off: light beams on valuable drops, item notes, potion pickup and belt refill, and a buzz on valuable drops (iPhone). A new game starts beside the town's waypoint, and the pause menu has **Quick Restart** (save, exit and rejoin with the same character and difficulty).

<h2 align="center" id="built-to-be-measured"><img src="docs/images/r2-plate-measured.jpg" width="100%" alt="Built to Be Measured"></h2>

<p align="center">
  <img src="docs/images/r2-stats.jpg" width="100%" alt="60 fps with no dropped frames on the Mac at Recommended. 7.3 ms of GPU time per frame at native 1440p on an M6. Byte-exact: replays tick for tick and saves byte for byte. Sanitized: ASan, UBSan, TSan and Metal validation.">
</p>

Every change is checked before it lands, and the numbers on this page come from runs that can be repeated. One command runs the checks in tiers, from a few seconds of source checks to long soak runs: `dm/verify/fork_gate.sh` ([dm/verify/FORK_GATE.md](dm/verify/FORK_GATE.md)). Picture-neutral changes are checked against tick-identical replays and pixel comparisons; costs are measured with alternating A/B runs.

<details>
<summary><b>Measured on Apple hardware</b></summary>

"Edge sharpness" below is the edge variance of the whole captured frame: one number for how much contrast its edges have. Higher is crisper; it only compares frames of the same scene.

- **Mac, Recommended on the M6** (2560 × 1440 screen, town route, 6 + 6 alternating runs): native output with the 3D at 75% + MetalFX takes about the same GPU time per frame as 1920 × 1080 + MetalFX (game frame 6.1 → 7.5 ms, present 1.7–2.1 → 0.3–0.4 ms). The game's SMAA added about 0.5 ms before the shader change below, and 16x filtering adds about 0.2 ms. Letting rendering passes overlap saves 0.54 ms (−8.5%). Skipping repeated state binds cuts bind records by 57% and the replay thread's instructions by 13%.
- **Mac, SMAA** (Mac App at Recommended, 4 + 4 alternating runs): building the anti-aliasing shaders without the HDR curve the game computes and then discards here cut the SMAA edge pass from 0.68 to 0.31 ms and the GPU time per frame from 7.25 to 6.72 ms, with a captured frame replaying pixel-identical ([e070b94](https://github.com/PowerBeef/d2r-metal/commit/e070b94)).
- **Mac, Environment Shadows** (M6, Recommended, town route, 3 + 3 alternating runs): scenery shadows add about 0.5 ms of GPU time per frame in town (6.78 → 7.32 ms) and 0.25 ms in the Blood Moor, with 60 fps and no dropped frames either way ([4f952ed](https://github.com/PowerBeef/d2r-metal/commit/4f952ed)).
- **Mac, MetalFX at the last step**: M6 (12 GPU cores), 2560 × 1440 screen, character select. Enlarging the same 1600 × 900 picture costs 0.28 ms of GPU time with a plain stretch and 1.38 ms with MetalFX; edge sharpness goes from 235 to 569 ([4ac0543](https://github.com/PowerBeef/d2r-metal/commit/4ac0543)).
- **iPhone, MetalFX on the 3D world**: iPhone 17 Pro, 3D at 75% (1967 × 905 inside a 2622 × 1206 frame). Steady 60 fps; median GPU time per frame 11.65 → 12.28 ms (+0.6 ms); edge sharpness 404 → 728 with the same colors ([7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892)).
- **iPhone, SMAA and 16x filtering**: iPhone 17 Pro standing in town, 3D at 75% (one pass with MetalFX, one without; runs alternated, 110 s each). Both hold 60 fps with frame time p95 17.6 ms, CPU per frame about 16 ms and the thermal state the same as the original. The phone doesn't show their GPU cost to an app: its GPU slows its clock until each frame's work fills the frame (about 15 ms whatever the setting), and the process energy reading follows the CPU.
- Not measured yet: battery and heat with MetalFX and the new picture options on phones over long sessions, and Environment Shadows on a phone.

</details>

<details>
<summary><b>The tools in the repository</b></summary>

- a **control port** to drive a running game, and a **scenario runner** that plays scripted sessions on the command-line host, the Mac App, the iOS app on a Mac, or an iPhone ([control_port.md](dm/verify/control_port.md), [scenarios](dm/verify/scenarios/README.md));
- **checks that read the game's own state**: the panels on screen, the focused widget, the controller actions as the game names them, and the save system, where a save that never commits is a failure; every scripted session also checks that its saved game would load;
- **baselines recorded from this project's own build**: three routes replay tick for tick, the replayed saves match the recorded ones byte for byte, and key screens are compared;
- **save fault tests** (a full disk at different moments, a quit in the middle of a save), a **playtest bot** that checks invariants after every action, and long **soak** runs;
- **A/B benchmarks** with statistics, regression checks against a kept build, sampled profiles per game thread and function, per-pass GPU timing, and **frame capture and replay**;
- **sanitizer builds** (ASan, UBSan, TSan), seeded **random exploration** with shrinking, **replay**, and LLDB commands that read the game's panels, focus and saves ([debugging.md](dm/verify/debugging.md)).

</details>

<h2 align="center" id="supported-devices"><img src="docs/images/r2-plate-devices.jpg" width="100%" alt="Supported Devices"></h2>

D2R Metal is built for **macOS 27 and iOS / iPadOS 27 or later** and draws only through Metal 4, so it can use what those systems add without older fallbacks. Metal 4 needs an **A14 or M1 chip** or later.

| Device | Plays? | What you need |
|---|---|---|
| Mac, M1 or later | Yes | macOS 27 or later |
| iPhone / iPad, A17 Pro or M chip | Yes | iOS / iPadOS 27 or later |
| iPhone / iPad, A14–A16 | Yes | iOS / iPadOS 27 or later, and a [Legacy Device Pack](docs/LEGACY_DEVICES.md) |
| A13 and older, Intel Macs | No | — |

A17 Pro means iPhone 15 Pro and later and iPhone 16 and later; A14–A16 covers the iPhone 12 to 14 families and the iPhone 15 / 15 Plus. Devices with 4 GB of memory run, but tightly. Older systems (macOS 15–26, iOS 18–26), Apple TV, Android and HarmonyOS are not supported.

<h2 align="center" id="getting-started"><img src="docs/images/r2-plate-start.jpg" width="100%" alt="Getting Started"></h2>

You need **the files of the game you bought**: the Switch release of Diablo II: Resurrected with the Reign of the Warlock expansion, updated to 1.0.34.0, dumped from your own console. How to get them is in [docs/GAME_FILES.md](docs/GAME_FILES.md).

> This project provides only tools and contains no game content. We do not distribute or endorse, and are not responsible for, any third party distributing game assets, game executables or install packages that contain game content. Please use only game files you legally own.

### On a Mac: D2R Metal Launcher

1. **Download** `D2R-Metal-Launcher-<version>.dmg` from the [latest release](https://github.com/PowerBeef/d2r-metal/releases/latest), open it and drag **D2R Metal Launcher** to Applications. It is signed with the project's Developer ID and notarized by Apple.
2. **Choose your game files:** the game packages (`.nsp` or `.xci`: the base game and the 1.0.34.0 update) with your console's `prod.keys`, or a folder you already extracted.
3. **Press Install.** The launcher builds the game on your Mac from your own files, in about 10–15 minutes the first time, then press **Play**.

The launcher is how you open the game from then on. It updates the game and itself from this project's releases (the game's main menu also says when a new version is out), can go back to the previous version, and repairs an install. It needs about 30 GB free while installing and runs nothing you have to install yourself. Everything about it is in [docs/LAUNCHER.md](docs/LAUNCHER.md); playing on the Mac (keys, mouse, saves) is in [docs/MAC.md](docs/MAC.md).

### On iPhone and iPad, or to develop: build from source

The iPhone / iPad app, a Mac App with iCloud sync, and development all build from this repository on a Mac with Xcode 27:

1. **Put your game files in the workspace** (default `~/dm-work`, or set `DM_ROOT`):

   ```
   ~/dm-work/data/switch/3.3.94012/Update/ExeFS/    main, sdk, subsdk0
   ~/dm-work/data/switch/3.3.94012/Update/RomFS/    data/…
   ```

   `./setup.sh <packages…> <prod.keys>` extracts them from your game packages for you.

2. **Prepare the workspace.** This checks your tools, rewrites the game program and translates the game's shaders to Metal; the first run takes a while. `./setup.sh check` reports what is missing and what to run next.

   ```bash
   ./setup.sh
   source ~/dm-work/build/cache/dm/venv/bin/activate
   ```

3. **Build the app.**

   - **iPhone / iPad**: copy `dm/ios/local.env.example` to `dm/ios/local.env` and set your signing details (`DM_TEAM_ID`, `DM_BUNDLE_ID`; with a free Apple ID also `DM_FREE_ACCOUNT=1` and `DM_IOS_UDID`), then `cd dm/ios && ./run.sh build && ./run.sh install`. On first launch the app asks for the game resource folder ([docs/GAME_FILES.md](docs/GAME_FILES.md)).
   - **Mac**: `dm/tools/mac_app.sh` produces `~/dm-work/out/macapp/D2R Metal.app`. No developer account needed; with a paid one it can carry iCloud sync. See [docs/MAC.md](docs/MAC.md#build-from-source).

Run `./setup.sh` again after updating to a new commit. It also rewrites three files that depend on your local tools (`dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`, `dm/gfx/nvn/c1_full.tsv`); that is expected, don't commit them. The full dependency list is in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

**Coming from D2R Mobile?** Its saves load here. Items kept in its extra stash space (the wider columns, extra pages, Pouch or vault) don't carry over.

**This repository contains and distributes no game content**: no game program, art, text, data table or shader. Everything is generated on your own machine from your own files; the interface information the port relies on is listed in [docs/DERIVED.md](docs/DERIVED.md).

<h2 align="center" id="for-developers"><img src="docs/images/r2-plate-developers.jpg" width="100%" alt="For Developers"></h2>

<details>
<summary><b>How it works</b></summary>

1. An offline step rewrites a few instructions in the game's aarch64 program so it can run as an ordinary process (no code is generated at run time, which iOS forbids).
2. A native host loads it, provides the Switch system services it calls, and runs it on the device's CPU.
3. The game's NVN graphics calls are replayed on Metal 4, with its shaders translated offline to Metal.

A tour of the code is in [dm/README.md](dm/README.md).

</details>

<details>
<summary><b>History</b></summary>

- D2R Metal began in October 2026 as a fork of D2R Mobile. Its first changes went back upstream (four were adopted in D2R Mobile v2.0); since 2026-10-28 it is developed on its own, for Apple platforms only.
- **1.0** (2026-10-31) is the first release, with D2R Metal Launcher for the Mac. Releases are listed on the [releases page](https://github.com/PowerBeef/d2r-metal/releases).
- `main` is the trunk. Changes are checked by the project's own gate ([dm/verify/FORK_GATE.md](dm/verify/FORK_GATE.md)); see [CONTRIBUTING.md](CONTRIBUTING.md).

</details>

<p align="center"><img src="docs/images/divider.png" width="400" alt=""></p>

## Legal

**You must own the game, including the Reign of the Warlock expansion.** This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Blizzard Entertainment, Nintendo or Apple. Diablo and Blizzard Entertainment are trademarks of Blizzard Entertainment, Inc.; Nintendo Switch is a trademark of Nintendo; Apple, Mac, iPhone, iPad, Metal and MetalFX are trademarks of Apple Inc. Use it only with a copy of the game that you own. The banner, plaques, section plates, frames and ornaments on this page are original artwork made for this project; they use no art from the game. The screenshots show the game running in D2R Metal; they live with this page in `docs/images` and are not part of any release.

## Acknowledgements

D2R Metal exists because of **[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)** by [@jhaizhou-ops](https://github.com/jhaizhou-ops) and its contributors, who built the port: the original program running unchanged, touch controls, controller support and five languages. Their acknowledgements, including the community authors of the Android and HarmonyOS ports, are in upstream's README.

The project stands on the open-source work below. Thank you to its authors and maintainers.

- **Ryujinx** (Ryubing 1.3.2, MIT): `dm/third_party/ryujinx` bundles its shader translator unchanged (the Ryujinx.Graphics.Shader, Spv.Generator and Ryujinx.Common projects), which translates the Switch game's shaders to SPIR-V.
- **deko3d** (devkitPro, zlib): the texture layout size algorithm in `dm/gfx/nvn/nvn_tex.c` is ported from it.
- **nvn.h** (h1k421, MIT): the graphics API declarations in `dm/gfx/nvn/include/nvn.h`.
- **nxnative** (seedds, MIT): GPU timestamps, the 120 Hz display-link timing, the Metal compile cache, the shader bounds guard, NVN frame capture and replay, the function-map and fuzzing tools are adapted from it; each adapted file says so in its header.
- **libdeflate** (Eric Biggers, MIT): decompressing game data.
- **bcdec** (iOrange, MIT or Unlicense): decoding BC textures on devices that do not support the BC formats.
- **FLIP** (NVIDIA, BSD 3-Clause): the image difference metric used in image-quality checks.
- **Offline fix patch** (LFS Mod Offline Fix): the two rewrites in `dm/tools/patch_nso.py` that let the game run offline come from this community patch, whose original author is not reliably recorded. Its two other byte patches change game logic and are left out.
- Tools used to build and verify: SPIRV-Cross, XcodeGen, .NET, Capstone and Unicorn, Pillow, numpy, lz4, astcenc, hactool, libimobiledevice and LLVM. Versions and links are in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

## License

MIT, see [LICENSE](LICENSE) (Copyright (c) 2026 jhaizhou; Copyright (c) 2026 the d2r-metal contributors). Third-party code keeps its own license in its own directory; see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).
