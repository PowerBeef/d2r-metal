# D2R Metal

**Diablo II: Resurrected with the Reign of the Warlock expansion, running natively on Apple silicon — tuned for Metal 4, MetalFX and current Macs, iPhones and iPads.**

D2R Metal is a fork of [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile) by [@jhaizhou-ops](https://github.com/jhaizhou-ops). The port itself is theirs: the Switch release's own aarch64 program executing directly on the device, the graphics API reimplemented on Metal, the complete touch controls and the new endgame. This fork builds on it for Apple platforms only — a sharper picture through MetalFX, picture settings inside the game's own Options, a Mac App that feels at home on macOS, safer saves, and the tooling to measure every change. Improvements that suit upstream go back to it as pull requests.

Platforms: Mac (Apple silicon; macOS 26 and later, macOS 15 – 25 with the [older-API build](docs/OLDER_SYSTEMS.md)) · iPhone / iPad (iOS 26 and later, iOS 18 – 25 with the older-API build) · Game: the Switch release with Reign of the Warlock, 1.0.34.0 (PC version 3.3.94012) · License: MIT · **Bring your own game files**

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/3b-mac-improved-1920-metalfx.jpg" width="100%" alt="The game in full screen on a Mac at 2560 × 1440: rendered at 1920 × 1080 and enlarged with MetalFX">
</p>

## Sharper on Apple silicon: MetalFX

Both apps render the game below the screen's resolution and enlarge it. Upstream enlarges with a plain bilinear stretch on the Mac and the game's own bilinear pass on iPhone; D2R Metal uses **MetalFX**, Apple's edge-adaptive upscaler:

- **Mac**: MetalFX does the enlargement at the present step, from the render resolution to the window or screen ([4ac0543](https://github.com/PowerBeef/d2r-metal/commit/4ac0543)).
- **iPhone / iPad**: the game renders its 3D scene at 75% and scales it back up with its own pass; MetalFX replaces that pass ([7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892)).

Same build, same spot, only the setting changed. Full frames; click one for full size.

**Mac, 2560 × 1440 screen.** Original: the stock 1600 × 900 with a plain stretch. D2R Metal: Recommended on that Mac, 1920 × 1080 with MetalFX.

<table><tr><th>Original</th><th>D2R Metal</th></tr><tr>
<td><img width="440" alt="Mac, 1600 × 900, plain stretch" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/3a-mac-original-1600-standard.jpg" /></td>
<td><img width="440" alt="Mac, 1920 × 1080 with MetalFX" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/3b-mac-improved-1920-metalfx.jpg" /></td>
</tr></table>

**iPhone 17 Pro, native 2622 × 1206.** Both with the 3D scene at 75%. Original: the game's own upscale. D2R Metal: MetalFX.

<table><tr><th>Original</th><th>D2R Metal</th></tr><tr>
<td><img width="440" alt="iPhone 17 Pro, 3D at 75% with the game's own upscale" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/7a-iphone-original-75-standard.jpg" /></td>
<td><img width="440" alt="iPhone 17 Pro, 3D at 75% with MetalFX" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/7b-iphone-improved-75-metalfx.jpg" /></td>
</tr></table>

### Measured on Apple hardware

| What | Device | Result | Commit |
|---|---|---|---|
| Mac present step, character select | M6 (12 GPU cores), 2560 × 1440 screen | Present pass: 1600 × 900 stretched 0.28 ms; 1600 × 900 + MetalFX 1.38 ms; 1920 × 1080 + MetalFX 1.78 ms. Edge variance 235 → 569 → 754 | [4ac0543](https://github.com/PowerBeef/d2r-metal/commit/4ac0543) |
| 3D upscale pass | iPhone 17 Pro (3D at 1967 × 905) | Steady 60 fps; GPU p50 11.65 → 12.28 ms; edge variance 404 → 728, same colours | [7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892) |
| 3D upscale pass, 3D at 75% | Mac | Edge variance 157 → 211 (231 at 100%); in town 633 → 1267 for +0.5 ms of GPU time | [7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892) |
| Frame Rate set to 30 fps | Mac | 59.96 → 29.98 fps, applied live | [ec164ba](https://github.com/PowerBeef/d2r-metal/commit/ec164ba) |

Edge variance is a whole-frame sharpness measure: higher means crisper edges. Battery and heat with MetalFX on phones over long sessions have not been measured yet.

## Picture settings in the game's own Options

Options has five tabs on both platforms: **Video**, **Audio**, **Gameplay**, **Settings** and **Extras**. The Video tab holds the picture settings, drawn with the game's own selectors and strings:

- **Mac**: Display Mode (Windowed / Fullscreen), Resolution (1600 × 900 / 1920 × 1080 / 2560 × 1440), Graphic Presets, Upscaling (Standard / MetalFX), Frame Rate and Frame Drops.
- **iPhone / iPad**: 3D Resolution (75% / 85% / 100%, applied live), Graphic Presets, Upscaling, Frame Rate and Frame Drops.
- **Frame Rate** (60 fps / 30 fps): 30 fps is the power saver. **Frame Drops**: *Lower 3D Res* lets the game lower the 3D resolution when frames drop and bring it back afterwards; *Keep 3D Res* always keeps the one you set.
- **Recommended** is picked for your device at first launch (on the Mac: full screen, the highest resolution up to three quarters of the screen's width, MetalFX when the picture is enlarged). Apply, Discard and Default behave like the game's own option pages, and Default resets only the tab you are on.

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/2-mac-upscaling-changed.jpg" width="49%" alt="The Mac Video tab: Display, Graphics, Frame Rate and Frame Drops">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/4-mac-extras-tab.jpg" width="49%" alt="The Extras tab with the game's own checkbox highlight">
</p>
<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/6-ios-video-and-extras.jpg" width="98%" alt="The iOS Video and Extras tabs">
</p>

## A Mac App that feels at home

- **Full screen at launch** when Display Mode says so, and View → Full Screen (⌃⌘F) updates the setting.
- **Mouse wheel and trackpad** scroll the game's lists natively; drag scroll-bar thumbs and sliders; click Apply, Default and Discard.
- **Keyboard and controller** move a highlight that matches the game's own: the selector bar on picture settings, the checkbox square on Extras.
- The Options pages use the roomy iOS layout instead of the cramped console one.
- Command-key releases and clicks on the black bars around the picture no longer leak into the game.

The Mac guide, with keys, mouse, saves and importing saves from an iPhone, is [docs/MAC.md](docs/MAC.md).

## Saves you can trust

- The Mac App saves on every system quit and treats minimizing like iOS going to the background ([112400c](https://github.com/PowerBeef/d2r-metal/commit/112400c)).
- Save requests from the front end are carried out on the game thread ([1f84541](https://github.com/PowerBeef/d2r-metal/commit/1f84541)).
- A refused or failed save no longer loses the stash, vault or pouch side files, and the player is told ([7f0a328](https://github.com/PowerBeef/d2r-metal/commit/7f0a328)).
- A death or a busy cell no longer drops pouch, vault or page records ([0a14be4](https://github.com/PowerBeef/d2r-metal/commit/0a14be4)).

These came out of a full audit of the port, which listed 111 findings; 41 commits so far fix findings across the graphics replay, the host services, the loader, the saves and the tools.

## Built to be measured

Every change here is checked on real hardware, and the tools for that are in the repository:

- a **control port** to drive a running game step by step, and a **scenario runner** that plays scripted sessions on the command-line host, the Mac App, the iOS package on a Mac, or an iPhone ([dm/verify/control_port.md](dm/verify/control_port.md), [dm/verify/scenarios/README.md](dm/verify/scenarios/README.md));
- an **event stream**, signposts and fixed-route **A/B benchmarks** with statistics, plus GPU frame capture;
- **sanitizer builds** (ASan, UBSan, TSan), seeded **random exploration** with shrinking, and **replay** ([dm/verify/debugging.md](dm/verify/debugging.md)).

## Everything from D2R Mobile

All of upstream's work is here and kept in sync: the original program running unchanged (every class, Runeword and drop rule as shipped), touch controls rebuilt for fingers, controller support with Xbox and PlayStation prompts, the Greater Rift, the Endgame Boss Rush, Elysium mode and the Kill Board, the Chronicle stash, Trove and Pouch, auto-drink, the loot filter, the Runeword helper and minimap guidance, and five languages (English, Chinese, Spanish, Russian and Korean). Upstream's [README](https://github.com/jhaizhou-ops/d2r-mobile#readme) describes them with screenshots.

It is not an emulator, cloud gaming or streaming. **It contains and distributes no game content**: no game program, art, text, data table or shader is in this repository; everything is generated on your own machine from your own files ([docs/GAME_FILES.md](docs/GAME_FILES.md); the interface information the port relies on is listed in [docs/DERIVED.md](docs/DERIVED.md)). The game runs fully offline.

## Where it's heading

D2R Metal is Apple-first: the aim is to get the most out of Apple silicon and Metal 4 — less GPU time and power for the same or a better picture, smooth play on current Macs, iPhones and iPads, and native behaviour on each. These are goals, not promises; what has actually landed is listed above, with the commits and measurements behind it.

## Getting started

You need **the files of the game you bought**: the Switch release of Diablo II: Resurrected with the Reign of the Warlock expansion, updated to 1.0.34.0. How to prepare them is in [docs/GAME_FILES.md](docs/GAME_FILES.md).

> This project provides only tools and contains no game content. We do not distribute or endorse, and are not responsible for, any third party distributing game assets, game executables or install packages that contain game content. Please use only game files you legally own.

**Devices**: an Apple silicon Mac; an iPhone or iPad with an A17 Pro or later (iPhone 15 Pro and later, M-series iPads). A16 and earlier devices (A14 at minimum, the first with Metal 3) also need a [Legacy Device Pack](docs/LEGACY_DEVICES.md) made from your own game files. Intel Macs are not supported. D2R Metal has no prebuilt downloads: you build it on your Mac.

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
   - **iPhone / iPad**: copy `dm/ios/local.env.example` to `dm/ios/local.env` and set your signing details (`DM_TEAM_ID`, `DM_BUNDLE_ID`; with a free Apple ID also `DM_FREE_ACCOUNT=1` and `DM_IOS_UDID`), then `cd dm/ios && ./run.sh build`. On first launch the app asks for the game resource folder ([docs/GAME_FILES.md](docs/GAME_FILES.md)).

Run `./setup.sh` again after updating to a new commit. It also rewrites three files that depend on your local tools (`dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`, `dm/gfx/nvn/c1_full.tsv`); that is expected, don't commit them. The full dependency list is in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

**Android and HarmonyOS** are not supported here. Their code stays in the tree only so that upstream changes merge cleanly; use [upstream](https://github.com/jhaizhou-ops/d2r-mobile) for those platforms, and for the no-Mac install route.

## How this fork works

- `main` is the trunk. Upstream's `main` is merged in regularly, so its fixes and features arrive here too.
- Changes that also fit upstream are sent back as pull requests, one topic per branch. Upstream adopted the first four in v2.0 (#106 – #109: split game packages, the iOS install battery check, the Mac App icon, pointer auto-hide); [#144](https://github.com/jhaizhou-ops/d2r-mobile/pull/144) (picture settings and MetalFX for the Mac and iOS) is open.
- Issues and pull requests about Apple platforms are welcome here. Code follows upstream's rules in [CONTRIBUTING.md](CONTRIBUTING.md), so it can be sent upstream later. A tour of the code is in [dm/README.md](dm/README.md).

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
