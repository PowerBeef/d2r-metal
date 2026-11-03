# License texts of code adapted into the project

Some of D2R Metal's own files are adapted from other projects instead of vendored whole. Each such file names its source, license and copyright
holder in its opening comment; the full license texts are here, and every release pack that carries the adapted code (its source or the program
built from it) carries them too (`dm/tools/release_mac.sh`, docs/THIRD_PARTY_NOTICES.md).

| Text | Project | Adapted into |
|---|---|---|
| `nxnative.txt` (MIT, Copyright (c) 2026 seedds) | [nxnative](https://github.com/seedds/nxnative) | GPU timestamps, the 120 Hz display-link timing, NVN frame capture and replay (`dm/gfx/nvn`: `hle_nvn.c`, `nvn_mtl.m`, `nvn_capture.c` / `.h`, `nvn_timestamp.h`), the Metal compile cache and the shader bounds guard (`dm/gfx/tools`: `dmsh_chain.py`, `dmsh_mtlcache.py`, `c1_guard.py`), the function map (`dm/tools/nso_fnmap.py`), the fuzzing tools (`dm/tools/fuzz`, `dm/tools/test_fuzz_corpus.py`) and their tests (`dm/gfx/tests`: `nvn_replay.m`, `t_capture.c`, `t_link_ticks.m`, `t_timestamp.c`) |
| `deko3d.txt` (zlib, Copyright (C) 2018-2020 fincs) | [deko3d](https://github.com/devkitPro/deko3d) | The block-linear texture layout size algorithm in `dm/gfx/nvn/nvn_tex.c` (an altered version: ported to C, with the game's own layout parameters), whose opening comment also carries this notice |

`dm/gfx/nvn/include/nvn.h` (MIT, Copyright (c) 2020 h1k421) carries its license text in the file itself.
