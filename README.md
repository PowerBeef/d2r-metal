# D2R Metal

**Diablo II: Resurrected with the Reign of the Warlock expansion, running natively on Apple silicon Macs, iPhones and iPads.**

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/3b-mac-improved-1920-metalfx.jpg" width="100%" alt="The game in full screen on a Mac with a 2560 × 1440 screen: rendered at 1920 × 1080 and enlarged with MetalFX">
</p>

This is the game's own program — the Nintendo Switch release, from files you own — running directly on Apple hardware, with its graphics redrawn through Metal. It is not an emulator, cloud gaming or streaming, and it plays fully offline.

D2R Metal is a fork of [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile) by [@jhaizhou-ops](https://github.com/jhaizhou-ops), who built the port itself. This fork focuses on Apple platforms only and adds a sharper picture through MetalFX, picture settings inside the game's own Options, a Mac App that behaves like a Mac game, an on-screen keyboard for controller players, and safer saves. Changes that suit upstream are sent back to it.

Mac (Apple silicon) · iPhone and iPad · Game: Switch release 1.0.34.0 with Reign of the Warlock (PC 3.3.94012) · MIT license · **Bring your own game files**

## Highlights

- **A sharper picture.** MetalFX, Apple's edge-aware upscaler, enlarges the picture instead of a plain stretch.
- **Picture settings in the game's Options.** Resolution, full screen, upscaling, frame rate — on a Video tab drawn with the game's own selectors.
- **A real Mac game.** Full screen, mouse wheel and trackpad scrolling, keyboard and controller, saves on quit.
- **Type with a controller.** When the game asks for a name or an amount, an in-game keyboard appears; type on the keyboard or click and it steps aside for a regular text box.
- **Saves you can trust.** Quitting, minimizing or a failed write no longer loses progress.
- **Everything from D2R Mobile:** touch controls, controller support, the Greater Rift, Boss Rush, Elysium mode and more.

## Mac and iPhone / iPad at a glance

| | Mac | iPhone / iPad |
|---|---|---|
| Picture drawn at | 1600 × 900 to 2560 × 1440 | The screen's full resolution |
| 3D world | Same as the picture | 75, 85 or 100% |
| MetalFX enlarges | The whole picture | The 3D world |
| First launch | Full screen, sized for your screen | 3D at 75% |
| When frames drop | Keeps the 3D resolution | Lowers it for a while |
| Controls | Keyboard, mouse, trackpad, controller | Touch, controller |
| Saves | On quit and when minimized | When sent to the background |
| System | macOS 26 (15 – 25: older-API build) | iOS 26 (18 – 25: older-API build) |

The picture rows are settings you can change in Options; the details are below.

## A sharper picture with MetalFX

The two platforms enlarge different things, and D2R Metal puts MetalFX on both:

- **Mac**: the whole game is drawn below your screen's resolution and enlarged to the window or screen at the last step. Upstream uses a plain stretch; here MetalFX does it ([4ac0543](https://github.com/PowerBeef/d2r-metal/commit/4ac0543)).
- **iPhone / iPad**: the interface is drawn at the screen's full resolution, but the 3D world is drawn at 75% and scaled back up by the game's own pass (a bilinear stretch with light sharpening). MetalFX replaces that pass ([7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892)).

Full frames from the same build and the same spot; only the setting changed. Click one for full size.

**Mac, 2560 × 1440 screen.** Left: the original 1600 × 900, stretched. Right: what D2R Metal picks for that Mac, 1920 × 1080 enlarged with MetalFX.

<table><tr><th>Original</th><th>D2R Metal</th></tr><tr>
<td><img width="440" alt="Mac, 1600 × 900, plain stretch" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/3a-mac-original-1600-standard.jpg" /></td>
<td><img width="440" alt="Mac, 1920 × 1080 with MetalFX" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/3b-mac-improved-1920-metalfx.jpg" /></td>
</tr></table>

**iPhone 17 Pro, 2622 × 1206.** Both with the 3D world at 75%. Left: the game's own upscale. Right: MetalFX.

<table><tr><th>Original</th><th>D2R Metal</th></tr><tr>
<td><img width="440" alt="iPhone 17 Pro, 3D at 75% with the game's own upscale" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/7a-iphone-original-75-standard.jpg" /></td>
<td><img width="440" alt="iPhone 17 Pro, 3D at 75% with MetalFX" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/7b-iphone-improved-75-metalfx.jpg" /></td>
</tr></table>

What it costs is measured in [For developers](#measured-on-apple-hardware): about 1 ms of GPU time per frame on a Mac, about 0.6 ms on an iPhone 17 Pro, which still holds 60 fps.

## Picture settings in the game's Options

Options has five tabs on both platforms: **Video**, **Audio**, **Gameplay**, **Settings** and **Extras**. The Video tab holds the picture settings, drawn with the game's own selectors and text:

- **Mac**: Display Mode (Windowed / Fullscreen), Resolution (1600 × 900 / 1920 × 1080 / 2560 × 1440), Graphic Presets, Upscaling (Standard / MetalFX), Frame Rate, Frame Drops.
- **iPhone / iPad**: 3D Resolution (75% / 85% / 100%, applied immediately), Graphic Presets, Upscaling, Frame Rate, Frame Drops.
- **Frame Rate**: 60 fps, or 30 fps to save power and run cooler.
- **Frame Drops**: *Lower 3D Res* lets the game draw the 3D world at a lower resolution while frames are dropping and bring it back afterwards; *Keep 3D Res* never lowers it. Phones default to Lower; the Mac defaults to Keep, since its GPU has headroom and its rare drops are short stutters a lower resolution doesn't fix ([b0891c8](https://github.com/PowerBeef/d2r-metal/commit/b0891c8)).
- **Recommended** is worked out at first launch. On a Mac: full screen, the highest resolution up to three quarters of the screen's width (2560 × 1440 only on Pro, Max and Ultra chips), and MetalFX whenever the picture is enlarged. On iPhone and iPad: 3D at 75% with MetalFX.
- Apply, Discard and Default work like the game's own option pages, and Default resets only the tab you are on.

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/2-mac-upscaling-changed.jpg" width="49%" alt="The Mac Video tab: Display, Graphics, Frame Rate and Frame Drops">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/4-mac-extras-tab.jpg" width="49%" alt="The Extras tab with the game's own checkbox highlight">
</p>
<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/6-ios-video-and-extras.jpg" width="98%" alt="The iOS Video and Extras tabs">
</p>

## On the Mac

- **Full screen** at launch when Display Mode says so; View → Full Screen (⌃⌘F) updates the setting.
- **Mouse wheel and trackpad** scroll the game's lists; drag scroll bars and sliders; click Apply, Default and Discard.
- **Keyboard and controller** move a highlight that looks like the game's own, on the new Video and Extras tabs too.
- **Typing with a controller**: naming a character, entering an amount of gold or searching the Chronicle brings up an in-game keyboard, drawn in the game's style with the game's own fonts. The D-pad or stick moves, A types, B deletes, X switches case, + confirms; the button hints match your Switch, Xbox or PlayStation controller. Names follow the game's rule, an empty name picks a random one, and the gold pad has Half and Max ([03a0bda](https://github.com/PowerBeef/d2r-metal/commit/03a0bda)).
- **Typing with the keyboard**: the text box is a sheet on the game window, in your language, with the same name and amount rules. Press a key or click while the on-screen keyboard is up and it hands over to the sheet, keeping what you typed; press a controller button and it comes back.
- **Saves** on every quit, including from the Dock, at logout and with a text box still open; minimizing is treated like an iPhone going to the background.
- Clicks on the black bars around the picture no longer reach the game, and a key released while ⌘ is held no longer leaves your character walking.

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/8-mac-controller-keyboard.jpg" width="100%" alt="Naming a character with a controller on the Mac: the in-game keyboard under the game's name box">
</p>

The Mac guide — keys, mouse, saves, iCloud and moving saves over from an iPhone — is [docs/MAC.md](docs/MAC.md).

## On iPhone and iPad

- **Touch controls** rebuilt for fingers, and **controllers** with Xbox and PlayStation button prompts (from D2R Mobile).
- **3D Resolution** (75 / 85 / 100%) and MetalFX on the Video tab, applied as soon as you press Apply.
- **Saves** when the app goes to the background, so swiping it away doesn't lose progress.
- The on-screen controller keyboard is in the iPhone and iPad app too (touching the screen switches to the system text box); it has been built but not yet tried on a device.

## Saves you can trust

- The Mac App saves on every system quit and treats minimizing like iOS going to the background ([112400c](https://github.com/PowerBeef/d2r-metal/commit/112400c)); quitting with a name or gold box open still saves ([a8714ad](https://github.com/PowerBeef/d2r-metal/commit/a8714ad)).
- Save requests from the menus are carried out on the game thread ([1f84541](https://github.com/PowerBeef/d2r-metal/commit/1f84541)).
- A refused or failed save no longer loses the stash, vault or pouch side files, and you are told ([7f0a328](https://github.com/PowerBeef/d2r-metal/commit/7f0a328)).
- A death or a busy cell no longer drops pouch, vault or page records ([0a14be4](https://github.com/PowerBeef/d2r-metal/commit/0a14be4)).
- A damaged shared stash can no longer hang iCloud sync ([5bfed70](https://github.com/PowerBeef/d2r-metal/commit/5bfed70)).

These came out of a full audit of the port (111 findings) and a later bug hunt with sanitizers, Metal validation and static analysis.

## Everything from D2R Mobile

All of upstream's work is here and kept in sync: the original program running unchanged (every class, Runeword and drop rule as shipped), touch controls rebuilt for fingers, controller support, the Greater Rift, the Endgame Boss Rush, Elysium mode and the Kill Board, the Chronicle stash, Trove and Pouch, auto-drink, the loot filter, the Runeword helper and minimap guidance, and five languages (English, Chinese, Spanish, Russian and Korean). Upstream's [README](https://github.com/jhaizhou-ops/d2r-mobile#readme) shows them with screenshots.

**This repository contains and distributes no game content**: no game program, art, text, data table or shader. Everything is generated on your own machine from your own files ([docs/GAME_FILES.md](docs/GAME_FILES.md); the interface information the port relies on is listed in [docs/DERIVED.md](docs/DERIVED.md)).

## Which devices are supported

The graphics layer needs Metal 3, so the minimum is an **A14 or M1 chip**. A17 Pro means iPhone 15 Pro and later and iPhone 16 and later; A14 – A16 covers the iPhone 12 to 14 families and the iPhone 15 / 15 Plus.

| Device | Plays? | What you need |
|---|---|---|
| Mac, M1 or later | Yes | macOS 26, or 15 – 25 with the [older-API build](docs/OLDER_SYSTEMS.md) |
| iPhone / iPad, A17 Pro or M chip | Yes | iOS 26, or 18 – 25 with the older-API build |
| iPhone / iPad, A14 – A16 | Yes | Also a [Legacy Device Pack](docs/LEGACY_DEVICES.md) |
| A13 and older, Intel Macs | No | — |

Devices with 4 GB of memory run, but tightly. Apple TV, Android and HarmonyOS versions exist in [upstream](https://github.com/jhaizhou-ops/d2r-mobile); this fork doesn't build or test them.

## Getting started

You need **the files of the game you bought**: the Switch release of Diablo II: Resurrected with the Reign of the Warlock expansion, updated to 1.0.34.0. How to prepare them is in [docs/GAME_FILES.md](docs/GAME_FILES.md).

> This project provides only tools and contains no game content. We do not distribute or endorse, and are not responsible for, any third party distributing game assets, game executables or install packages that contain game content. Please use only game files you legally own.

There are no prebuilt downloads: you build the apps on your Mac.

1. Put your game files in the workspace (default `~/dm-work`, or set `DM_ROOT`):

   ```
   ~/dm-work/data/switch/3.3.94012/Update/ExeFS/    main, sdk, subsdk0
   ~/dm-work/data/switch/3.3.94012/Update/RomFS/    data/…
   ```

2. Prepare the workspace. This checks your tools, rewrites the game program and translates the game's shaders to Metal; the first run takes a while. `./setup.sh check` reports what is missing and what to run next.

   ```bash
   ./setup.sh
   source ~/dm-work/build/cache/dm/venv/bin/activate
   ```

3. Build the app.

   - **Mac**: `dm/tools/mac_app.sh` produces `~/dm-work/out/macapp/D2R Metal.app` (macOS 15 – 25: `DM_MAC_M3=1 dm/tools/mac_app.sh`). No developer account needed. See [docs/MAC.md](docs/MAC.md).
   - **iPhone / iPad**: copy `dm/ios/local.env.example` to `dm/ios/local.env` and set your signing details (`DM_TEAM_ID`, `DM_BUNDLE_ID`; with a free Apple ID also `DM_FREE_ACCOUNT=1` and `DM_IOS_UDID`), then `cd dm/ios && ./run.sh build && ./run.sh install`. On first launch the app asks for the game resource folder ([docs/GAME_FILES.md](docs/GAME_FILES.md)).

Run `./setup.sh` again after updating to a new commit. It also rewrites three files that depend on your local tools (`dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`, `dm/gfx/nvn/c1_full.tsv`); that is expected, don't commit them. The full dependency list is in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

## For developers

### How it works

1. An offline step rewrites a few instructions in the game's aarch64 program so it can run as an ordinary process (no code is generated at run time, which iOS forbids).
2. A native host loads it, provides the Switch system services it calls, and runs it on the device's CPU.
3. The game's NVN graphics calls are replayed on Metal 4 (or the older Metal API), with its shaders translated offline to Metal.

A tour of the code is in [dm/README.md](dm/README.md).

### Measured on Apple hardware

"Edge sharpness" below is the edge variance of the whole captured frame: a single number for how much contrast its edges have. Higher is crisper; it only compares frames of the same scene.

- **Mac, MetalFX at the last step** — M6 (12 GPU cores), 2560 × 1440 screen, character select. Enlarging the same 1600 × 900 picture costs 0.28 ms of GPU time with a plain stretch and 1.38 ms with MetalFX; edge sharpness goes from 235 to 569. At 1920 × 1080 with MetalFX (Recommended on that Mac) enlarging costs 1.78 ms, the whole frame about 13.7 ms — inside the 16.7 ms of 60 fps — and edge sharpness reaches 754 ([4ac0543](https://github.com/PowerBeef/d2r-metal/commit/4ac0543)).
- **iPhone, MetalFX on the 3D world** — iPhone 17 Pro, 3D at 75% (1967 × 905 inside a 2622 × 1206 frame). Steady 60 fps; median GPU time per frame 11.65 → 12.28 ms (+0.6 ms); edge sharpness 404 → 728 with the same colours ([7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892)).
- Not measured yet: battery and heat with MetalFX on phones over long sessions.

### Built to be measured

The tools used to check every change are in the repository:

- a **control port** to drive a running game, and a **scenario runner** that plays scripted sessions on the command-line host, the Mac App, the iOS app on a Mac, or an iPhone ([dm/verify/control_port.md](dm/verify/control_port.md), [dm/verify/scenarios/README.md](dm/verify/scenarios/README.md));
- an **event stream**, signposts, fixed-route **A/B benchmarks** with statistics, and GPU frame capture;
- **sanitizer builds** (ASan, UBSan, TSan), seeded **random exploration** with shrinking, **replay**, and LLDB commands that show game addresses as module offsets, which a coding agent can drive through Xcode's LLDB MCP server ([dm/verify/debugging.md](dm/verify/debugging.md)).

### How this fork works

- `main` is the trunk. Upstream's `main` is merged in regularly, so its fixes and features arrive here too.
- Changes that also fit upstream are sent back as pull requests, one topic per branch. Upstream adopted the first four in v2.0 (#106 – #109: split game packages, the iOS install battery check, the Mac App icon, pointer auto-hide); [#144](https://github.com/jhaizhou-ops/d2r-mobile/pull/144) (MetalFX, the picture settings and the rebuilt Options page for the Mac and iOS) is open.
- Android and HarmonyOS code stays in the tree only so upstream changes merge cleanly; it isn't built or tested here.
- Issues and pull requests about Apple platforms are welcome. Code follows upstream's rules in [CONTRIBUTING.md](CONTRIBUTING.md), so it can be sent upstream later.

## Legal

**You must own the game, including the Reign of the Warlock expansion.** This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Blizzard Entertainment, Nintendo or Apple. Diablo and Blizzard Entertainment are trademarks of Blizzard Entertainment, Inc.; Nintendo Switch is a trademark of Nintendo; Apple, Mac, iPhone, iPad, Metal and MetalFX are trademarks of Apple Inc. Use it only with a copy of the game that you own.

## Acknowledgements

D2R Metal exists because of **[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)** by [@jhaizhou-ops](https://github.com/jhaizhou-ops) and its contributors; their acknowledgements, including the community authors of the Android and HarmonyOS ports, are in upstream's README.

The project stands on the open-source work below. Thank you to its authors and maintainers.

- **Ryujinx** (Ryubing 1.3.2, MIT): `dm/third_party/ryujinx` bundles its shader translator unchanged (the Ryujinx.Graphics.Shader, Spv.Generator and Ryujinx.Common projects), which translates the Switch game's shaders to SPIR-V. Upstream also used the original game in a patched Ryujinx as the reference for comparing pictures and game state (see [dm/verify/README.md](dm/verify/README.md)).
- **deko3d** (devkitPro, zlib): the texture layout size algorithm in `dm/gfx/nvn/nvn_tex.c` is ported from it.
- **nvn.h** (h1k421, MIT): the graphics API declarations in `dm/gfx/nvn/include/nvn.h`.
- **libdeflate** (Eric Biggers, MIT): decompressing game data.
- **bcdec** (iOrange, MIT or Unlicense): decoding BC textures on devices that do not support the BC formats.
- **FLIP** (NVIDIA, BSD 3-Clause): the image difference metric used in image-quality checks.
- **Offline fix patch** (LFS Mod Offline Fix): the 7 rewrites in `dm/tools/patch_nso.py` that let the game run offline come from this community patch, whose original author is not reliably recorded.
- Tools used to build and verify: SPIRV-Cross, XcodeGen, .NET, Capstone and Unicorn, Pillow, numpy, lz4, astcenc, hactool, libimobiledevice and LLVM. Versions and links are in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

## License

MIT, see [LICENSE](LICENSE) (Copyright (c) 2026 jhaizhou). Third-party code keeps its own license in its own directory; see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).
