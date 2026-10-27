<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/banner-hero.jpg" width="100%" alt="D2R Metal: Diablo II: Resurrected and Reign of the Warlock, the game as it shipped, running natively on Apple silicon Macs, iPhones and iPads">
</p>

<p align="center">
  <img alt="macOS 27 and later" src="https://img.shields.io/badge/macOS-27%2B-a07638?style=for-the-badge&labelColor=1a110d">
  <img alt="iOS and iPadOS 27 and later" src="https://img.shields.io/badge/iOS%20%2F%20iPadOS-27%2B-a07638?style=for-the-badge&labelColor=1a110d">
  <img alt="Metal 4 and MetalFX" src="https://img.shields.io/badge/Metal%204-MetalFX-7a1d12?style=for-the-badge&labelColor=1a110d">
  <img alt="Plays offline" src="https://img.shields.io/badge/plays-offline-7a1d12?style=for-the-badge&labelColor=1a110d">
  <img alt="MIT license" src="https://img.shields.io/badge/license-MIT-5a4a3a?style=for-the-badge&labelColor=1a110d">
</p>

<p align="center"><b>Diablo II: Resurrected with Reign of the Warlock, the way it shipped,<br>running natively on Apple silicon Macs, iPhones and iPads.</b></p>

This is the game's own program, the Nintendo Switch release, built from files you own and run directly on Apple hardware, with its graphics redrawn through Metal. It is not an emulator, cloud gaming or streaming, and it plays fully offline.

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/banner-plaques.jpg" width="100%" alt="22 additions left out to stay close to the original. As shipped: rules, drops, economy, skills and stash. Native: Apple silicon, Metal 4, MetalFX. Offline: your own game files, no game servers.">
</p>

## The game as it shipped

> **D2R Metal changes the platform, not the game.**

D2R Metal is a fork of [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile) by [@jhaizhou-ops](https://github.com/jhaizhou-ops), who did the hard part: getting the game itself to run on Apple hardware, with touch controls, controller support and five languages. D2R Mobile also offers its own additions on top of the game, such as new endgame modes, town conveniences, automation, extra storage and map aids, for players who want them.

D2R Metal takes a different path and stays as close as it can to the game Blizzard shipped: the same rules, drop rates, economy and stash. Its own work goes into how that game runs and looks on Apple hardware.

| What D2R Metal works on | What it keeps as the game shipped |
|---|---|
| Graphics redrawn through Metal 4, sharpened with MetalFX | Skills, their caps and every class |
| A Mac App that behaves like a Mac game | Drop rates, loot and what vendors stock |
| Touch controls and controller support | Identifying, portals, repairs and gambling at their usual price |
| An in-game keyboard for controller players | The stash: the game's own 10 columns and tabs |
| Saves that survive quitting, the background and a full disk | The automap, with nothing added to it |
| Picture settings inside the game's own Options | Difficulty: the game's own Players setting |

### Where D2R Metal differs from D2R Mobile

To stay close to the original, this fork leaves out 22 of D2R Mobile's additions. They are left out of the code rather than switched off, so the game underneath is the original:

| Area | In D2R Mobile | In D2R Metal, as in the original game |
|---|---|---|
| Endgame | Greater Rift (with six extra mercenary slots, a skill cap of 40 and a Hell gambling bonus) · Boss Rush · Elysium mode · Kill Board | The game's own Acts, Terror Zones and skill cap |
| Town services | Free Town Portal · free identify · vendors' Refresh Stock | A scroll or a tome charge; a scroll, a tome or Cain; vendors restock as the game decides |
| Automation | Auto-drink · mercenary auto-feed · auto-recast buffs · rune and gem auto-stash · Runeword helper | Potions, buffs, runes and recipes are handled by hand |
| Loot | A loot filter set by difficulty | Drops shown as the game shows them |
| Storage | 14-column stash with four extra personal pages · Trove · Pouch · Deposit button · Chronicle vault | The game's own stash: 10 columns, one personal page, five shared pages and the gem, material and rune tabs |
| Map and interface | Minimap marks (exits, waypoints, quest goals) · a difficulty selector in the iPhone bottom bar · a Game Difficulty row in the controller pause menu | The automap fills in as you explore; the player count is set on **Options → Settings → Offline Difficulty Scaling** |
| Game code | Two side effects of a community offline fix: Druid skills right after a shapeshift, and the Warlock's summons walking without their running animation | The game's own code; only the part of the fix that lets it run offline is kept |

Saves from D2R Mobile load here; items kept in its extra stash space (the wider columns, extra pages, Pouch or vault) don't carry over.

Some of D2R Mobile's conveniences are kept. On the **Extras** tab of Options, each one a switch: light beams on valuable drops, item notes, potion pickup and belt refill, and a buzz on valuable drops (iPhone). Always on: a new game starts beside the town's waypoint, and the pause menu has **Quick Restart** (save, exit and rejoin with the same character and difficulty).

<p align="center"><img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/divider.png" width="400" alt=""></p>

## A sharper picture with MetalFX

MetalFX is Apple's edge-aware upscaler. D2R Metal draws the interface and text at your screen's own resolution and lowers only the 3D world, which the game's own upscale pass (a bilinear stretch with light sharpening) brings back to full size; MetalFX replaces that pass:

- **Mac**: Native output, with the 3D world at a scale chosen for your chip (75% of a 2560-wide screen on a base M-series chip, full size on Max and Ultra). On this setup the GPU time per frame is about the same as drawing the whole picture at 1920 × 1080 and enlarging it, and text is visibly crisper.
- **iPhone / iPad**: the 3D world at 75% of the screen's full resolution.

The Mac also uses the game's own PC options the Switch build leaves off: SMAA anti-aliasing and up to 16x texture filtering, with colors shown as the game was made (sRGB) on wide-gamut screens.

Full frames from the same build and the same spot; only the setting changed. Click one for full size.

**Mac, 2560 × 1440 screen, full screen.** Left: the original 1600 × 900, stretched, with the game's FXAA. Right: D2R Metal's Recommended on an M6: the interface at the screen's native 2560 × 1440, the 3D world at 75% enlarged by MetalFX, SMAA and 16x texture filtering.

<table><tr><th>Original</th><th>D2R Metal Recommended</th></tr><tr>
<td><img width="440" alt="Mac, 1600 × 900 stretched, FXAA" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/11a-mac-original-1600.jpg" /></td>
<td><img width="440" alt="Mac, native 2560 × 1440 interface, 3D at 75% with MetalFX, SMAA, 16x filtering" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/11b-mac-recommended-native.jpg" /></td>
</tr></table>

The same two frames at 1:1 pixels: interface text, the waypoint and ground, name labels and the character.

<p align="center"><img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/12-mac-detail-1to1.jpg" width="100%" alt="1:1 crops of the two frames: interface text, waypoint and ground, labels and fence, character"></p>

**iPhone 17 Pro, 2622 × 1206.** Both with the 3D world at 75%. Left: the game's own upscale. Right: MetalFX.

<table><tr><th>Original</th><th>D2R Metal</th></tr><tr>
<td><img width="440" alt="iPhone 17 Pro, 3D at 75% with the game's own upscale" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/7a-iphone-original-75-standard.jpg" /></td>
<td><img width="440" alt="iPhone 17 Pro, 3D at 75% with MetalFX" src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/7b-iphone-improved-75-metalfx.jpg" /></td>
</tr></table>

On the M6 the Mac's Recommended takes about the same GPU time per frame as the earlier 1920 × 1080 + MetalFX setup, about 8 ms of the 16.7 ms a 60 fps frame allows, because the plain 1:1 present replaces the full-frame MetalFX pass and rendering passes now overlap on the GPU. On an iPhone 17 Pro MetalFX costs about 0.6 ms and it still holds 60 fps (details under *For developers*, below).

## Settings in the game's own Options

Options has five tabs on both platforms: **Video**, **Audio**, **Gameplay**, **Settings** and **Extras**, all drawn with the game's own selectors, checkboxes and text.

- **Video**: on the Mac, Display Mode, Resolution (1600 × 900 to 2560 × 1440, or Native), Color (Accurate / Vivid), Graphic Presets, 3D Resolution (100 down to 50%) and Upscaling (Standard / MetalFX); on iPhone and iPad, 3D Resolution (75 / 85 / 100%), Color, Graphic Presets and Upscaling. Both have **Anti-Aliasing** (the game's FXAA or SMAA), **Anisotropic Filtering** (Original up to 16x), **Frame Rate** (60 fps, or 30 to save power) and **Frame Drops** (lower the 3D resolution while frames drop, or keep it).
- **Recommended** is worked out for the device: on a Mac, full screen at Native with the 3D resolution chosen for its chip, MetalFX, SMAA, 16x filtering and Accurate color; on iPhone and iPad, 3D at 75% with MetalFX (the new picture options are off there until they have been measured on a phone).
- **Settings** holds the game's own **Offline Difficulty Scaling**, which sets the player count and takes effect at once.
- Apply, Discard and Default work like the game's own option pages; Default resets only the tab you are on.

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/13-mac-video-tab-native.jpg" width="49%" alt="The Mac Video tab at Recommended: Fullscreen, Native, Accurate color, Recommended preset, 3D Resolution 75%">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/14-mac-video-tab-graphics.jpg" width="49%" alt="The Mac Video tab's Graphics rows: MetalFX, SMAA, 16x filtering, Frame Rate and Frame Drops">
</p>
<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/10-mac-settings-player-count.jpg" width="49%" alt="The Settings tab with the game's own Offline Difficulty Scaling row">
</p>

## Mac and iPhone / iPad

| | Mac | iPhone / iPad |
|---|---|---|
| Controls | Keyboard, mouse, trackpad, controller | Touch, controller |
| Picture drawn at | The screen's own resolution (Native), 3D at 50–100% | The screen's full resolution, 3D at 75–100% |
| MetalFX enlarges | The 3D world | The 3D world |
| When frames drop | Keeps the 3D resolution | Lowers it for a while |
| Saves | On every quit and when minimized | When sent to the background |
| System | macOS 27 or later | iOS / iPadOS 27 or later |

**On the Mac**, it behaves like a Mac game: full screen at launch (View → Full Screen updates the setting), the mouse wheel and trackpad scroll the game's lists, scroll bars and sliders drag, and keyboard and controller move a highlight drawn like the game's own. The Mac guide, with keys, saves, iCloud and moving saves over from an iPhone, is [docs/MAC.md](docs/MAC.md).

**Typing with a controller**: naming a character or entering an amount of gold brings up an in-game keyboard in the game's own style and fonts, with button hints for Switch, Xbox or PlayStation controllers. Press a key, click or touch the screen and it steps aside for a regular text box, keeping what you typed (on iPhone and iPad it is built but not yet tried on a device).

<p align="center">
  <img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/8-mac-controller-keyboard.jpg" width="100%" alt="Naming a character with a controller on the Mac: the in-game keyboard under the game's name box">
</p>

## Saves you can trust

- The Mac App saves on every quit, including from the Dock, at logout and with a text box still open; minimizing is treated like an iPhone going to the background.
- The iPhone app saves when it goes to the background, so swiping it away doesn't lose progress.
- A failed save (a full disk, for example) is refused with a message, and the next one commits; before, one failure silently stopped every later save.
- A damaged shared stash can no longer hang iCloud sync.

These came out of a full audit of the port, a bug hunt with sanitizers and Metal validation, and a study of how the game itself saves.

<p align="center"><img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/divider.png" width="400" alt=""></p>

## Which devices are supported

D2R Metal is built for **macOS 27 and iOS / iPadOS 27 or later** and draws only through Metal 4, so it can use what those systems add without older fallbacks. Metal 4 needs an **A14 or M1 chip** or later.

| Device | Plays? | What you need |
|---|---|---|
| Mac, M1 or later | Yes | macOS 27 or later |
| iPhone / iPad, A17 Pro or M chip | Yes | iOS / iPadOS 27 or later |
| iPhone / iPad, A14–A16 | Yes | iOS / iPadOS 27 or later, and a [Legacy Device Pack](docs/LEGACY_DEVICES.md) |
| A13 and older, Intel Macs | No | — |

A17 Pro means iPhone 15 Pro and later and iPhone 16 and later; A14–A16 covers the iPhone 12 to 14 families and the iPhone 15 / 15 Plus. Devices with 4 GB of memory run, but tightly. Older systems (macOS 15–26, iOS 18–26, through the older-API build) and the Apple TV, Android and HarmonyOS versions are supported by [upstream](https://github.com/jhaizhou-ops/d2r-mobile); this fork doesn't build or test them.

## Getting started

You need **the files of the game you bought**: the Switch release of Diablo II: Resurrected with the Reign of the Warlock expansion, updated to 1.0.34.0. How to prepare them is in [docs/GAME_FILES.md](docs/GAME_FILES.md).

> This project provides only tools and contains no game content. We do not distribute or endorse, and are not responsible for, any third party distributing game assets, game executables or install packages that contain game content. Please use only game files you legally own.

There are no prebuilt downloads: you build the apps on your Mac.

1. **Put your game files in the workspace** (default `~/dm-work`, or set `DM_ROOT`):

   ```
   ~/dm-work/data/switch/3.3.94012/Update/ExeFS/    main, sdk, subsdk0
   ~/dm-work/data/switch/3.3.94012/Update/RomFS/    data/…
   ```

2. **Prepare the workspace.** This checks your tools, rewrites the game program and translates the game's shaders to Metal; the first run takes a while. `./setup.sh check` reports what is missing and what to run next.

   ```bash
   ./setup.sh
   source ~/dm-work/build/cache/dm/venv/bin/activate
   ```

3. **Build the app.**

   - **Mac**: `dm/tools/mac_app.sh` produces `~/dm-work/out/macapp/D2R Metal.app` (macOS 27 or later). No developer account needed. See [docs/MAC.md](docs/MAC.md).
   - **iPhone / iPad**: copy `dm/ios/local.env.example` to `dm/ios/local.env` and set your signing details (`DM_TEAM_ID`, `DM_BUNDLE_ID`; with a free Apple ID also `DM_FREE_ACCOUNT=1` and `DM_IOS_UDID`), then `cd dm/ios && ./run.sh build && ./run.sh install`. On first launch the app asks for the game resource folder ([docs/GAME_FILES.md](docs/GAME_FILES.md)).

Run `./setup.sh` again after updating to a new commit. It also rewrites three files that depend on your local tools (`dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`, `dm/gfx/nvn/c1_full.tsv`); that is expected, don't commit them. The full dependency list is in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

**This repository contains and distributes no game content**: no game program, art, text, data table or shader. Everything is generated on your own machine from your own files; the interface information the port relies on is listed in [docs/DERIVED.md](docs/DERIVED.md).

<p align="center"><img src="https://github.com/PowerBeef/d2r-metal/releases/download/assets/divider.png" width="400" alt=""></p>

## For developers

<details>
<summary><b>How it works</b></summary>

1. An offline step rewrites a few instructions in the game's aarch64 program so it can run as an ordinary process (no code is generated at run time, which iOS forbids).
2. A native host loads it, provides the Switch system services it calls, and runs it on the device's CPU.
3. The game's NVN graphics calls are replayed on Metal 4, with its shaders translated offline to Metal.

A tour of the code is in [dm/README.md](dm/README.md).

</details>

<details>
<summary><b>Measured on Apple hardware</b></summary>

"Edge sharpness" below is the edge variance of the whole captured frame: one number for how much contrast its edges have. Higher is crisper; it only compares frames of the same scene.

- **Mac, MetalFX at the last step**: M6 (12 GPU cores), 2560 × 1440 screen, character select. Enlarging the same 1600 × 900 picture costs 0.28 ms of GPU time with a plain stretch and 1.38 ms with MetalFX; edge sharpness goes from 235 to 569. At 1920 × 1080 with MetalFX (Recommended on that Mac) enlarging costs 1.78 ms, the whole frame about 13.7 ms, inside the 16.7 ms of 60 fps, and edge sharpness reaches 754 ([4ac0543](https://github.com/PowerBeef/d2r-metal/commit/4ac0543)).
- **iPhone, MetalFX on the 3D world**: iPhone 17 Pro, 3D at 75% (1967 × 905 inside a 2622 × 1206 frame). Steady 60 fps; median GPU time per frame 11.65 → 12.28 ms (+0.6 ms); edge sharpness 404 → 728 with the same colours ([7c2f892](https://github.com/PowerBeef/d2r-metal/commit/7c2f892)).
- **Mac, Recommended on the M6** (2560 × 1440 screen, town route, 6 + 6 alternating runs): native output with the 3D at 75% + MetalFX takes about the same GPU time per frame as 1920 × 1080 + MetalFX (game frame 6.1 → 7.5 ms, present 1.7–2.1 → 0.3–0.4 ms); the game's SMAA adds about 0.5 ms and 16x filtering about 0.2 ms; letting rendering passes overlap saves 0.54 ms (−8.5%); skipping repeated state binds cuts bind records by 57% and the replay thread's instructions by 13%. Picture-neutral changes are checked against tick-identical replays and pixel comparisons.
- Not measured yet: battery and heat with MetalFX on phones over long sessions, and the new picture options on an iPhone.

</details>

<details>
<summary><b>Built to be measured</b></summary>

The tools used to check every change are in the repository. One command runs them in tiers, from a few seconds of source checks to a long run on demand: `dm/verify/fork_gate.sh` ([dm/verify/FORK_GATE.md](dm/verify/FORK_GATE.md)).

- a **control port** to drive a running game, and a **scenario runner** that plays scripted sessions on the command-line host, the Mac App, the iOS app on a Mac, or an iPhone ([control_port.md](dm/verify/control_port.md), [scenarios](dm/verify/scenarios/README.md));
- **checks that read the game's own state**: the panels on screen, the focused widget, the controller actions as the game names them, and the save system, where a save that never commits is a failure; every scripted session also checks that its saved game would load;
- **baselines recorded from this fork's own build**: three routes replay tick for tick, the replayed saves match the recorded ones byte for byte, and key screens are compared;
- **save fault tests** (a full disk at different moments, a quit in the middle of a save), a **playtest bot** that checks invariants after every action, and long **soak** runs;
- **A/B benchmarks** with statistics, regression checks against a kept build, sampled profiles per game thread and function, and GPU frame capture;
- **sanitizer builds** (ASan, UBSan, TSan), seeded **random exploration** with shrinking, **replay**, and LLDB commands that read the game's panels, focus and saves ([debugging.md](dm/verify/debugging.md)).

</details>

<details>
<summary><b>How this fork works</b></summary>

- `main` is the trunk. Upstream's `main` is merged in regularly, so its fixes arrive here too; the additions listed above stay out of this fork.
- Changes that also fit upstream are sent back as pull requests, one topic per branch. Upstream adopted the first four in v2.0 (#106–#109); [#144](https://github.com/jhaizhou-ops/d2r-mobile/pull/144) (MetalFX, the picture settings and the rebuilt Options page) is open.
- Android and HarmonyOS code stays in the tree only so upstream changes merge cleanly; it isn't built or tested here.
- Issues and pull requests about Apple platforms are welcome. Code follows upstream's rules in [CONTRIBUTING.md](CONTRIBUTING.md), so it can be sent upstream later.

</details>

## Legal

**You must own the game, including the Reign of the Warlock expansion.** This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Blizzard Entertainment, Nintendo or Apple. Diablo and Blizzard Entertainment are trademarks of Blizzard Entertainment, Inc.; Nintendo Switch is a trademark of Nintendo; Apple, Mac, iPhone, iPad, Metal and MetalFX are trademarks of Apple Inc. Use it only with a copy of the game that you own. The banner, plaques and ornaments on this page are original artwork made for this project; they use no art from the game.

## Acknowledgements

D2R Metal exists because of **[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)** by [@jhaizhou-ops](https://github.com/jhaizhou-ops) and its contributors, who built the port: the original program running unchanged, touch controls, controller support and five languages. Their acknowledgements, including the community authors of the Android and HarmonyOS ports, are in upstream's README.

The project stands on the open-source work below. Thank you to its authors and maintainers.

- **Ryujinx** (Ryubing 1.3.2, MIT): `dm/third_party/ryujinx` bundles its shader translator unchanged (the Ryujinx.Graphics.Shader, Spv.Generator and Ryujinx.Common projects), which translates the Switch game's shaders to SPIR-V. Upstream also used the original game in a patched Ryujinx as the reference for comparing pictures and game state (see [dm/verify/README.md](dm/verify/README.md)).
- **deko3d** (devkitPro, zlib): the texture layout size algorithm in `dm/gfx/nvn/nvn_tex.c` is ported from it.
- **nvn.h** (h1k421, MIT): the graphics API declarations in `dm/gfx/nvn/include/nvn.h`.
- **libdeflate** (Eric Biggers, MIT): decompressing game data.
- **bcdec** (iOrange, MIT or Unlicense): decoding BC textures on devices that do not support the BC formats.
- **FLIP** (NVIDIA, BSD 3-Clause): the image difference metric used in image-quality checks.
- **Offline fix patch** (LFS Mod Offline Fix): the two rewrites in `dm/tools/patch_nso.py` that let the game run offline come from this community patch, whose original author is not reliably recorded. Its two other byte patches change game logic and are left out.
- Tools used to build and verify: SPIRV-Cross, XcodeGen, .NET, Capstone and Unicorn, Pillow, numpy, lz4, astcenc, hactool, libimobiledevice and LLVM. Versions and links are in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

## License

MIT, see [LICENSE](LICENSE) (Copyright (c) 2026 jhaizhou). Third-party code keeps its own license in its own directory; see [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).
