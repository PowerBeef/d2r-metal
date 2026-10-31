# Third-party notices

D2R Metal is under the MIT License (`LICENSE`). Its launcher and the packs the launcher downloads also carry other people's work, each under its own
license. The full texts travel with the files they cover: in the launcher (`D2R Metal Launcher.app/Contents/Resources/Licenses` and
`Contents/Resources/Brand/fonts/OFL.txt`), and in each pack's `licenses` folder. `dm/tools/release_mac.sh` refuses to build a pack when a text is
missing.

None of this is game content: the game's program, art, text and shaders come only from the player's own copy, on the player's own Mac.

## D2R Metal Launcher

| Component | License | Text |
|---|---|---|
| D2R Metal (the launcher's own code) | MIT | `Resources/Licenses/LICENSE` |
| Cinzel typeface (Natanael Gama) | SIL Open Font License 1.1 | `Resources/Brand/fonts/OFL.txt` |

## Runtime pack (`d2r-metal-runtime-<version>.tar.gz`)

The game host (`host/D2RMobile`), the texture library (`lib/libnvn_deswizzle.dylib`) and the install scripts (`repo/`).

| Component | License | Text |
|---|---|---|
| D2R Metal | MIT | `licenses/LICENSE` |
| bcdec (BC texture decoding, in the host) | MIT or public domain (Unlicense), at the user's choice | `licenses/bcdec.txt` |
| libdeflate (decompression, in the host) | MIT | `licenses/libdeflate.txt` |

## Tools pack (`d2r-metal-tools-<version>.tar.gz`)

What runs the install steps on the player's Mac, so that nothing has to be installed there.

| Component | License | Text |
|---|---|---|
| CPython 3.14 | PSF License | `licenses/python/LICENSE.cpython.txt` |
| python-build-standalone (the CPython build) | Mozilla Public License 2.0 | `licenses/python-build-standalone.txt` |
| Libraries inside that CPython: OpenSSL, SQLite, zlib, bzip2, liblzma, libffi, expat, mpdecimal, libedit, ncurses, libuuid, Berkeley DB, Tcl/Tk and X11 parts | their own licenses | `licenses/python/LICENSE.<name>.txt` |
| lz4 (Python bindings and library) | BSD 3-Clause | `licenses/python-packages/lz4/` |
| Capstone 5 (disassembly) | BSD 3-Clause | `licenses/python-packages/capstone/` |
| NumPy (with the libraries its wheel bundles) | BSD 3-Clause and the bundled libraries' licenses | `licenses/python-packages/numpy/` |
| Pillow (with the image libraries its wheel bundles) | MIT-CMU (HPND) and the bundled libraries' licenses | `licenses/python-packages/pillow/` |
| The shader translator `dmsh_xlate`: D2R Metal code and Ryujinx's shader library, compiled with .NET NativeAOT | MIT (Ryujinx), MIT (.NET runtime parts, with their third-party notices) | `licenses/Ryujinx.txt`, `licenses/dotnet-LICENSE.txt`, `licenses/dotnet-ThirdPartyNotices.txt` |
| SPIRV-Cross | Apache-2.0 | `licenses/SPIRV-Cross.txt` |
| hactool (unpacks the player's game packages) | ISC | `licenses/hactool.txt` |
| mbed TLS (inside hactool) | Apache-2.0 | `licenses/mbedtls.txt` |
| cJSON (inside hactool) | MIT | `licenses/cJSON.txt` (the notice at the top of its source) |
