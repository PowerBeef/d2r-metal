# Dependencies

Everything the build needs is on this page: what each item is, what it is for, where to get it and how to install it. The versions listed are the ones we actually build with; newer versions usually work, and if something breaks, go back to the listed version first.

## What you bring yourself

| What | Used for | Notes |
|---|---|---|
| An Apple silicon Mac | Building, and running the host tools | Needs macOS and the Xcode version below |
| Your own game files | The game program and all game data | This repository contains no game files. You need the program files (ExeFS) and the update data (RomFS) of the Switch release 1.0.34.0 (corresponding to PC version 3.3.94012), dumped from a copy you bought and own. See [GAME_FILES.md](GAME_FILES.md) |
| An Apple developer account | Signing the app and installing it on your own device | A paid account gives you everything. A free account (personal team) cannot sign iCloud, sustained execution or the increased memory limit: building with `DM_FREE_ACCOUNT=1` removes them; what you lose is iCloud save sync, and the game runs as usual. Apps signed with a free account must be reinstalled every 7 days. With a free account: before building, connect the phone by cable and unlock it, and set `DM_IOS_UDID` in the local configuration (the build targets this phone, which is what makes Apple generate a provisioning profile for a free team); after installing, before the first launch, trust your developer account on the phone under Settings → General → VPN & Device Management. This mode has been signed with an actual free account, installed and played (on a 12 GB device); 6 GB / 8 GB devices have not been verified |

## Build tools

| Tool | Version | Used for | Official site | Install |
|---|---|---|---|---|
| Xcode (with the Metal compiler) | 27.1 (macOS / iOS 27 and Metal 4 only) | Building the app and compiling shaders into Metal libraries; with 27.1, `./setup.sh` recompiles the shaders with its compiler | <https://developer.apple.com/xcode/> | App Store or the developer site |
| Homebrew | — | Installing the command-line tools below | <https://brew.sh> | The one-line command on its home page |
| XcodeGen | 2.45.4 | Generating the Xcode project from `project.yml` | <https://github.com/yonaskolb/XcodeGen> | `brew install xcodegen` |
| .NET SDK | 10.0 | Building the shader translation tool | <https://dotnet.microsoft.com/download> | `brew install dotnet` |
| SPIRV-Cross | 1.4.357 | Turning translated SPIR-V into Metal Shading Language | <https://github.com/KhronosGroup/SPIRV-Cross> | `brew install spirv-cross` |
| Python 3 | 3.14 | Running the generation and verification scripts | <https://www.python.org/downloads/> | Ships with macOS, or `brew install python` |

## Python packages

| Package | Version | Used for | Link |
|---|---|---|---|
| Pillow | 12.1.1 | Generating interface art from the game files | <https://pypi.org/project/pillow/> |
| numpy | 2.4.4 | Same | <https://pypi.org/project/numpy/> |
| lz4 | 4.4.5 | Unpacking the program files | <https://pypi.org/project/lz4/> |
| capstone | 5.0.9 | Disassembly, for rewriting the program and for verification | <https://pypi.org/project/capstone/> |
| unicorn | 2.1.4 | Checking, site by site, that rewritten code behaves the same | <https://pypi.org/project/unicorn/> |

All at once: `python3 -m pip install pillow numpy lz4==4.4.5 capstone==5.0.9 unicorn==2.1.4` (`./setup.sh` creates an isolated environment and installs them for you, with lz4, capstone and unicorn pinned to these versions; capstone 6 renames the constants the rewriter uses, so the rewrite stops on it)

## Optional

| Tool | Used for | Official site |
|---|---|---|
| astcenc | Converting textures for older devices without BC texture support (A16 and earlier); built from source — how to build it and the whole procedure are in [LEGACY_DEVICES.md](LEGACY_DEVICES.md). We use v5.2.0 | <https://github.com/ARM-software/astc-encoder> |
| hactool | Needed by `dm/tools/extract_game.sh` to extract files from your game package; not needed if your game files are already prepared some other way. We tested with upstream commit `1d64a83` | <https://github.com/SciresM/hactool> |
| FFmpeg | Optional VP8 cutscene playback, off by default; default builds and our release packages do not contain it. Mac (`MOVIE_FFMPEG=1`): `brew install ffmpeg pkg-config`. iOS (`DM_MOVIE_FFMPEG=1`): `dm/ios/tools/build_ffmpeg_ios.sh` downloads the official FFmpeg 7.1.1 source tarball from ffmpeg.org, checks its SHA-256 and builds static libraries with only the Matroska demuxer, VP8 decoder and swscale; see [dm/host/MOVIE.md](../dm/host/MOVIE.md). License: that configuration does not enable GPL or non-free parts, so the libraries are LGPL 2.1 or later; the Homebrew build may enable GPL parts (check `ffmpeg -L`). If you distribute an app that statically links FFmpeg, you must meet the LGPL terms yourself (provide the FFmpeg source and let recipients relink the app with a modified FFmpeg) | <https://ffmpeg.org> |

## Extra tools for on-device and verification scripts

Not needed to build the app or run the Mac smoke test; only for on-device smoke tests, UI regression and the scripts under `dm/verify/`.

| Tool | Used for | Install |
|---|---|---|
| timeout | Wall-clock limits for runs and builds (used by several scripts) | `brew install coreutils` |
| iproxy, ideviceinfo | Forwarding the log port over USB and reading battery level in on-device smoke tests | `brew install libimobiledevice` |
| llvm-objdump, llvm-objcopy | Disassembly and section extraction in the rewrite-equivalence checks | `brew install llvm` |

## What the launcher's release packs ship

Players who install with D2R Metal Launcher ([LAUNCHER.md](LAUNCHER.md); coming with the first release) need none of the build tools above. Every program and library in the packs loads only macOS's own libraries or the pack's own; the release script refuses to build a pack that does not. Each release carries the pieces below, built and pinned by `dm/tools/release_mac.sh` from `dm/tools/release_pins.txt`, with their license texts in the tools pack's `licenses/` folder.

| Piece | Version | License | Upstream |
|---|---|---|---|
| CPython (from python-build-standalone) | 3.14.8 (build 20261013) | PSF (python-build-standalone: MIT) | <https://github.com/astral-sh/python-build-standalone> |
| lz4, capstone, numpy, Pillow (Python packages) | 4.4.5, 5.0.9, 2.5.4, 13.0.0 | BSD-3, BSD-3, BSD-3, MIT-CMU | PyPI |
| .NET runtime (compiled into the native shader translator: NativeAOT with Microsoft's SDK 10.0.403 on the release Mac; players get one program, no .NET) | 10.0.14 | MIT | <https://dotnet.microsoft.com> |
| Ryujinx shader translator (inside the same) | 1.3.2, as in `dm/third_party/ryujinx` | MIT | <https://github.com/Ryubing> |
| SPIRV-Cross | 1.4.363.0 | Apache-2.0 | <https://github.com/KhronosGroup/SPIRV-Cross> |
| hactool | commit 1d64a83 | ISC | <https://github.com/SciresM/hactool> |

## Third-party code in this repository

These are already in the repository; nothing to download. Each keeps its license in its own directory.

| Path | What | License | Upstream |
|---|---|---|---|
| `dm/brand/fonts` | Cinzel, the lettering of the logo and the launcher's headings (also inside the launcher) | SIL Open Font License 1.1 | <https://github.com/NDISCOVER/Cinzel> |
| `dm/third_party/bcdec` | BC texture decompression | MIT or Unlicense (your choice) | <https://github.com/iOrange/bcdec> |
| `dm/third_party/flip` | Image difference metric | BSD 3-Clause | <https://github.com/NVlabs/flip> |
| `dm/third_party/libdeflate` | Decompression | MIT | <https://github.com/ebiggers/libdeflate> |
| `dm/third_party/ryujinx` | The shader translator from Ryujinx 1.3.2 (three projects, unmodified), which translates the original Switch shaders to SPIR-V | MIT | <https://github.com/Ryubing> |
| `dm/gfx/nvn/include/nvn.h` | Graphics interface declarations | MIT | Author named in the file header |
| Layout size algorithm in `dm/gfx/nvn/nvn_tex.c` | Ported from deko3d | zlib | <https://github.com/devkitPro/deko3d> |
| The two rewrites in `dm/tools/patch_nso.py` that let the game run offline | Taken from a community offline fix (LFS Mod Offline Fix) | Unknown | We have no reliable record of where it was first published or by whom; if you know, please open an issue, see [DERIVED.md](DERIVED.md) |
