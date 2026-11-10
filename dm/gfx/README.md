# dm/gfx — graphics layer (NVN → Metal)

The Switch version of the game draws with NVN (the Switch's native graphics interface). The graphics layer reimplements this interface on top of Apple's Metal: every NVN call the game makes lands here, is recorded as a command, and is replayed as Metal commands at submit time. The game's shaders are not translated at run time; they are translated offline, once, on a computer, into Metal shader libraries. Where the chain is built without Apple's Metal compiler (the launcher's installs on a player's Mac), the libraries hold the Metal source instead and the Mac App compiles it with macOS's own compiler (`dmsh_chain.py --update-lock --runtime`, `nvn_mtl.m` `lib_load`); an iOS build always needs compiled libraries.

For the build steps of the whole project see `README.md` and `setup.sh` in the repository root, and for dependencies see `docs/DEPENDENCIES.md`; this page covers only the graphics layer's own matters.

## How it works

### Taking over the NVN interface

- The game's main program imports only one NVN symbol, `nvnBootstrapLoader`; all other NVN functions are obtained through it by name. The host forcibly takes over this import (`nvn/hle_nvn.c`), and not a single instruction of the original graphics driver is executed.
- The game fetches the addresses of 523 names in total (`nvn/names.txt`). Each name has a thunk (`nvn/nvn_thunks.S`, a generated file): it first increments that function's call count and then jumps to the implementation.
- Of these, 216 have call sites in the game's main program (`nvn/nvn_slots.tsv`): 203 are implemented according to NVN semantics, 9 are empty implementations (`g_nops` in `hle_nvn.c`) and 4 are left on the red-reporting stub on purpose (see "Known limitations"). Three functions without a static call site are implemented too (`CommandBufferSetCopyImageStride` and the two device timestamp getters). The remaining 304 are connected to the same red-reporting stub, which, when called, prints `【红】NVN 未实现被调用：名字` in the log ("[red] unimplemented NVN function called: name").
- Every implementation gets a compile-time signature check against `nvn/nvn_protos.h` (a generated file). The calling conventions of Apple arm64 and the Switch differ for integer parameters narrower than 32 bits, so on the host side `NVNboolean` parameters are always received as `uint32_t` and only the low 8 bits are examined.

### Two layers: record and replay

- **Interface layer `nvn/hle_nvn.c`**: host-side objects are stored in place in the memory the game allocates for NVN objects; the layouts are in `nvn/nvn_obj.h`, and each has a static assertion guaranteeing it does not exceed NVN's reserved size. Commands are recorded into the command memory provided by the game, in the record format of `nvn_obj.h`.
- **Replay layer `nvn/nvn_mtl.m`**: at submit time it replays the record stream as Metal commands.
  - Memory pools are created as zero-copy `MTLBuffer`s, with the GPU address equal to the CPU address, so only unified-memory devices are supported.
  - The visible effects of "GPU completion" (sync objects becoming ready, events writing their value, counters) happen only after the Metal command buffer has actually completed.
  - Pipeline states and depth-stencil states are cached by state hash; at startup, shaders and pipelines are pre-built in the background according to the warm-up manifest.
  - The game's packaged textures are in Tegra block-linear layout and are deswizzled before upload (see `tex/README.md`).
  - Presentation: the game's swapchain texture is drawn to a `CAMetalLayer`, and presentation timing is scheduled by the pacer in `nvn/nvn_pace.h`.

For which Metal object each NVN object and command lands on, see [nvn_metal_mapping.md](nvn_metal_mapping.md).

### Shader translation chain

The game's shaders are GPU machine code for the Tegra X1 (the Maxwell instruction set). The translation from this machine code to Metal Shading Language is not done on the phone; all of it happens offline:

```
Original Switch shader package
  → tools/dmsh_xlate        produces SPIR-V with Ryujinx's shader translation library (a C# tool)
  → spirv-cross             converts SPIR-V to Metal Shading Language (MSL)
  → xcrun metal / metallib  compiles into Metal shader libraries
```

The whole chain is driven by `tools/dmsh_build.py`, and `setup.sh` runs it for you. Key points:

- **Key**: each shader is keyed by the first 16 hexadecimal digits of the sha256 of its control section (0x880 bytes). At run time the game hands the control section to `nvnProgramSetShaders`, and the replay layer computes the same key to find the library.
- **Build outputs**: `<key>_b.metallib` (the shader library), `<key>_b.json` (reflection used by replay: each binding's kind, slot number, index within the argument buffer and so on), `<key>_b.metal` (the MSL source). Some fragment shaders additionally have `<key>_bf.metallib`, a "readable tile" version used for pass merging (see the file header of `tools/dmsh_fetch.py`). `_b_build.json` in the directory records the success or failure of each key in this run.
- **`nvn/shader_map.tsv`**: the shader map, one row per shader variant. At run time only its "key → stage (vertex / fragment / compute)" is used.
- **`nvn/shader_facts.tsv`**: the answer table for translation queries. The translator needs to know some facts that cannot be read from the machine code (the dimension type of a texture, image formats, the component types of vertex attributes and fragment outputs, a compute shader's thread group size and shared memory byte count), and this table gives the answers by key. The table ships with the repository, and building the shader libraries only reads it; the table is maintained by `tools/dmsh_facts.py`, whose file header has the details.
- **Translation chain version and the `b-<V>/` directory**: the version V is a string of 12 hexadecimal digits, obtained by hashing together the contents of the source files involved in the translation chain, the translation options and the tool fingerprints (`tools/dmsh_chain.lock`); `tools/dmsh_chain.py` computes it. Shader libraries are written into `<output directory>/b-<V>/`, and every reflection json also records V. When the host is compiled it takes V from `nvn/dmsh_chain.h`, loads only from `b-<V>/`, and checks the version in each json one by one; on a mismatch it reports red and does not load. So after changing any file on the chain, or changing a tool, the libraries must be rebuilt and the host recompiled.
  - `dmsh_chain.py`: prints V.
  - `dmsh_chain.py --check-tools`: checks that the local tools match the lock file.
  - `dmsh_chain.py --update-lock`: rewrites the lock file according to the local tools.
  - `dmsh_chain.py --header`: rewrites `nvn/dmsh_chain.h`.
  - It is normal for the local Xcode or spirv-cross version to differ from the one registered in the repository; `setup.sh` automatically re-registers according to the local tools.
- **`nvn/c1_full.tsv`**: the index list for the shaders' built-in constant section (c1), generated by `tools/c1_guard.py` scanning the translated MSL; it lists the shader keys whose "run-time index could not be proven bounded". Its first line records the `b-<V>` it corresponds to and the hash of the `c1_guard.py` that made it, so `./setup.sh` regenerates it when either changes. Conversions that can wrap (`int()` of a `uint` known only as non-negative, a `short` of a value past its range) prove nothing. For the shaders on the list the replay layer allocates a separate whole 64 KiB buffer for c1; when the list is missing or its version does not match, it reports red and conservatively treats all shaders this way.
- **`nvn/pso_manifest.tsv`**: the warm-up seed manifest, recording the shaders and pipelines used first after startup. Its first line is the key layout line `# pso_skey v2 432` (`nvn_pso_skey.h`); rows are checked and normalised when they load (`pso_skey_check`, `pso_key_norm`), the same as in the players' own `pso_user.tsv`. `t_pso_skey` lints the shipped seed (layout line, canonical rows, every shader key in `shader_map.tsv` at its stage) and `dm/tools/pso_seed.py normalise` rewrites a manifest that way. The row order is still the pre-build order; do not sort it. A `# warm: background` line splits the seed: the warm-up builds the rows before it, then the player's `pso_user.tsv`, then the rows after it, and the first loading screen waits only for the first two parts; the rest builds during play at utility priority (the launcher's `--warm-shaders` pass builds and waits for everything). A row after the line that the player draws before a background worker has built it is compiled on the submit thread and recorded in their `pso_user.tsv`, so from the next launch it builds in the foreground.
- **Shaders embedded in the program itself**: the main program additionally contains 4 compute shaders that are not in the shader package; they were hand-written as MSL from the disassembly and are in `nvn/nvn_builtin.h`.

Binding conventions followed during translation (file header of `tools/maxwell_refs.py`):

- uniform buffer r is constant section `c[3 + r]` in the machine code;
- the handle of separate texture r is at `c[2][0x168 + 8r]`, and the handle of separate sampler r is at `c[2][0x568 + 8r]`;
- the address of storage buffer r is at `c[0][base + 16r]`, with base: vertex 0x110, compute 0x310, fragment 0x510;
- the window origin is at the top left and the depth range is 0 to 1, the same as Metal, so nothing is flipped during translation.

## Directory structure

| Location | Contents |
|---|---|
| `nvn/hle_nvn.c` | Interface layer: NVN takeover, object implementations, command recording, presentation pacing |
| `nvn/nvn_mtl.m`, `nvn/nvn_mtl.h` | Replay layer: Metal device, memory pools, textures, shader loading, pipelines, drawing, presentation |
| `nvn/nvn_obj.h` | Host-side object layouts and the command record format, shared by both layers |
| `nvn/nvn_entry.S`, `nvn/nvn_thunks.S` | Entries for the unimplemented stub and the timing wrapper, per-function thunks |
| `nvn/nvn_tex.c`, `nvn/nvn_tex.h` | Storage size, alignment and per-level offsets of block-linear textures (ported from deko3d) |
| `nvn/nvn_dirty.c`, `nvn/nvn_dirty.h` | Tracks dirty pages of game memory with write protection, to decide whether texture contents have changed and need re-uploading |
| `nvn/nvn_refl.m`, `nvn/nvn_refl.h` | Converts shader reflection json into fixed-length C arrays, so that drawing only walks arrays |
| `nvn/nvn_hmap.h`, `nvn/nvn_cover.h`, `nvn/nvn_pace.h`, `nvn/nvn_pso_skey.h` | Hash table, geometric proof of full-coverage draws, presentation pacer, pipeline manifest key |
| `nvn/nvn_builtin.h` | Hand-written MSL for the 4 compute shaders embedded in the main program |
| `nvn/nvn_mv.inc`, `nvn/nvn_fg.inc`, `nvn/nvn_fgpace.h` | Included by `nvn_mtl.m`: the motion vectors MetalFX's temporal scaler needs, frame generation with MetalFX's frame interpolator, and when a frame starts under frame generation without a display link |
| `nvn/nvn_rt.inc` | Included by `nvn_mtl.m`: ray tracing for the Mac's Ray Tracing row (needs hardware ray tracing and the motion vector pass) |
| `nvn/nvn_rtex.inc` | Included by `nvn_mtl.m`: the shader pass of an extrapolated frame, which re-projects the last full frame's opaque picture and depth to this frame's camera |
| `nvn/nvn_capture.c`, `nvn/nvn_capture.h`, `nvn/nvn_capture.inc` | NVN frame capture (`DM_NVN_CAPTURE`): the record walk and file format shared with the replayer `tests/nvn_replay`, and the host's capture of one frame |
| `nvn/nvn_framerec.inc`, `nvn/nvn_testload.inc` | Test aids included by `nvn_mtl.m`: the frame recorder (every frame the screen is given, in order, for judging motion) and known GPU work as the control for measurements |
| `nvn/nvn_adaptive_pace.h`, `nvn/nvn_presproj.h`, `nvn/nvn_timestamp.h`, `nvn/nvn_copy_pitch.h`, `nvn/nvn_ratelog.h` | Small pure helpers with unit tests: adaptive pacing tiers, which presented time a drawable gets, NVN counter timestamp units, the byte strides of buffer / texture copies, rate-limited error lines |
| `nvn/nvn_protos.h`, `nvn/nvn_count.h` | Generated by `tools/gen_protos.py`: the prototypes and the function count |
| `nvn/rt_range_check.inc` | A same-frame checker compiled only into its test build (`NVN_RT_RANGE_TEST`), not part of normal builds |
| `nvn/include/nvn.h` | NVN interface declarations (third party; for the license see the file header) |
| `nvn/*.tsv`, `nvn/names.txt`, `nvn/dmsh_chain.h` | The tables described above and the translation chain version header |
| `nvn/bf_want.tsv`, `nvn/pso_allow.tsv`, `nvn/shader_map.tsv.conflicts.tsv` | Three more tables: the fragment shader keys that get an output-mapping-only "readable tile" variant (`tools/dmsh_build.py` reads it), the reviewed pipeline rows that `dm/tools/pso_seed.py check` and `gate` accept although the seed lacks them, and how key conflicts in the shader map were decided |
| `tex/` | Texture format mapping, deswizzling, BC decompression, see [tex/README.md](tex/README.md) |
| `metal/` | A standalone minimal Metal offscreen example, not compiled into the host, see [metal/README.md](metal/README.md) |
| `tools/` | Offline tools, see below |
| `tests/` | Unit tests |
| `hle_vi.c`, `hle_nv.c` | Glue layer for display service and graphics driver initialization: returns host static objects or does nothing |

The scripts in `tools/`; usage is written in each file's header:

| Script | Purpose |
|---|---|
| `dmsh_xlate/` | The shader translation tool (C#), which references the translation library in `dm/third_party/ryujinx` |
| `dmsh_build.py` | Drives the whole translation chain to produce the shader libraries; when the environment variable `IOSOUT=directory` is set, it also produces the iOS version |
| `dmsh_rules.py` | MSL rewrite rules applied to `spirv-cross` output by shader key (each rule's source is part of its shaders' cache fingerprint): seven today, each with a row in [docs/ENGINE_CHANGES.md](../../docs/ENGINE_CHANGES.md): `hdr_switch_split`, `velocity_entry`, `capture_entry`, `rt_shadow_hook`, `rt_ao_hook`, `rt_local_hook` and `rt_env_hook` |
| `dmsh_chain.py`, `dmsh_chain.lock` | Translation chain version and tool fingerprints |
| `dmsh_facts.py` | Generation and checking of the answer table `shader_facts.tsv` |
| `dmsh_fetch.py` | Generates the "readable tile" versions of fragment shaders |
| `c1_guard.py` | Generates `c1_full.tsv` (its header names the chain version and the guard's own hash) |
| `dmsh_mtlcache.py` | Compiled-shader cache keyed by the Metal source: a chain change that leaves a shader's MSL as it was costs that shader no Metal compile |
| `mv_check.py` | Offline check of the motion vectors: warps each frame's previous picture onto it with the dumped velocities and compares |
| `ub_table.py` | Reads the host log of a `DM_NVN_UB` A/B run: each variant phase against its neighbouring baseline phases, last line `RESULT {json}` |
| `dmsh_dryrun.py` | Dry-run conversion of the whole library, measuring only "can it be converted" |
| `shader_map.py`, `maxwell_refs.py` | Generates the shader map; reads resource references from the machine code |
| `nvn_slots.py`, `gen_protos.py` | Generate `nvn_slots.tsv` / `names.txt`, and `nvn_protos.h` / the thunks |
| `tbdr_budget.py`, `tbdr_load_dead.py`, `tbdr_store_dead.py`, `tbdr_pass_merge.py` | Read the `DM_NVN_PASSDUMP` log and analyze the load / store volume of each render pass's attachments per frame |
| `passgpu_top.py` | Reads the `DM_NVN_PASSGPU` output and ranks GPU time by pass shape, for a whole run or one segment between scenario marks (`--from town --to wild`); `--tsv` writes the full table. span_ms is shown as excl_ms only for a `DM_NVN_PASSGPU_SERIAL` record whose passes don't overlap; clear-only passes (no draws) are left out of that check and show `-` there, since their spans overlap the previous pass |

## Building

The graphics layer has no separate library. `dm/host/Makefile` compiles `dm/gfx/*.c` and the source files under `dm/gfx/nvn/` and `dm/gfx/tex/` into the host `dmhost` by wildcard; the iOS App is built by `dm/ios/run.sh build`, which also copies the map, `pso_manifest.tsv`, `c1_full.tsv` and the `b-<V>/` shader libraries into the App. `tests/` and `metal/` do not go into the host.

The shader libraries are generated by `setup.sh`:

- the Mac version is in `$DM_ROOT/build/cache/metal_shader_cache/b-<V>/`;
- the iOS version is in `$DM_ROOT/build/ios/dmsh_b/b-<V>/`.

After changing a file on the translation chain, the order for rebuilding manually is the same as in `setup.sh`: `dmsh_chain.py --header` → `dmsh_build.py` → `c1_guard.py` → recompile the host. `$DM_ROOT` defaults to `~/dm-work`.

## Tests

```sh
make -C dm/gfx/tests run        # graphics layer unit tests
make -C dm/gfx/tex/tests run    # texture submodule unit tests; the real-data items need texture samples from both the Switch and PC versions, see tex/README.md
make -C dm/gfx/metal run        # offscreen example self-test, see metal/README.md
```

The tests in `dm/gfx/tests` compile the production code in directly and need a Metal device on the local machine; a few of them run under `MTL_DEBUG_LAYER=1` (the Metal validation layer). Most tests do not read the workspace; the exceptions are `t_refl`, `t_refl_lib` and `t_rt_range_ro`, which read the translated shader reflection json under `$DM_ROOT/build/cache/metal_shader_cache`, and `test_tbdr_budget.py` / `test_passgpu_top.py`, which create a temporary directory under `$DM_ROOT/run/gfx`. Main contents:

| Test | What it checks |
|---|---|
| `t_nvn` | Interface layer: fetches addresses from `nvnBootstrapLoader` the way the game does, all the way through to presentation |
| `t_builtin` | The 4 embedded compute shaders |
| `t_refl`, `t_hmap`, `t_cover`, `t_pace`, `t_pso_skey`, `t_dirty_slots` | Reflection parsing, hash table, full-coverage proof, pacer, pipeline manifest key, dirty page tracking |
| `t_deswz_gpu` | GPU deswizzling |
| `t_present_crop` | Presentation cropping |
| `t_mip_extent` | Effective size when rendering to a mip level other than level 0 |
| `t_fuse_check`, `t_reorder_chain`, `t_defer_chain`, `t_zskip`, `t_xv`, `t_ub`, `t_pfv`, `t_rt_range`, `t_rt_range_shape`, `t_rt_range_ro`, `t_rt_depth_ro` | Compile the replay layer `nvn_mtl.m` in directly and check the various optimization and diagnostic logic inside the replay layer |
| `t_aniso` | Anisotropy override: `aniso_eff` and `samp_desc` cases, the `aniso_want` mapping, the level changed mid-run (deferred release, new state, `g_reg_gen`), deferred release on sampler re-registration |
| `t_bar` | Pass-barrier decisions (levels 0 / 1, the `DM_NVN_PASSGPU_SERIAL` pin, encoders that write memory) and the game-barrier counts (mid-pass only when a draw follows; between encoders, ordered by a full end barrier or not), through their counters under the Metal validation layer |
| `test_tbdr_budget.py` | The parsing in `tools/tbdr_budget.py` |
| `test_c1_guard.py` | `tools/c1_guard.py`: integer conversions that can wrap leave an index unproven; bounded values still prove; the list header names the guard's hash |
| `test_dmsh_rules.py` | `tools/dmsh_rules.py`: the HDR-switch split on synthetic MSL; rules read no module-level names; every HDR-switch key exists in `shader_map.tsv` with its role |
| `test_compile_convention.py` | One shader compile convention across the Mac offline compile (`dmsh_build.py` `MAC_METAL`), the iOS one (`ios_metal.py` `METAL`) and the run-time compile of source-form libraries (`nvn_mtl.m` `src_opts`); deployment-target flags aside |
| `test_passgpu_top.py` | `tools/passgpu_top.py`: a record's spans count as exclusive costs only with the serial header and no overlap between passes; clear-only passes never show one |
| `t_ratelog` | Rate-limited GPU error lines (`nvn/nvn_ratelog.h`): the first lines in full, one summary per period, the flush that counts a quiet tail, and four threads at once under ThreadSanitizer |
| `t_pso_warm` | Pipeline keys handed to `pso_desc` are normalised and build the same descriptor as the raw state (2,000 random keys); where the user manifest lives and which manifests the warm-up reads; the user manifest's dedupe across old and normalised rows, and its recording of the seed's background rows; one load per shader when workers need it together, and a specialised function not compiled under the cache lock; the warm-up plan (order, background marker, dedupe), its counters end to end for every source combination and worker count, and the two waits |
| `t_capture` | Frame capture (`nvn_capture.c`): every record op's pointers encode to region offsets and object indices and decode back; unknown ops are refused; the capture file round-trips its chunks, two-part chunks streamed through LZ4 included; the per-submit state text (anisotropy, MetalFX 3D) and its precedence in the replayer |
| `t_adaptive_pace`, `t_fgpace`, `t_presproj`, `t_timestamp`, `t_copy_pitch` | The pure helpers: adaptive pacing tiers, frame starts under frame generation (`nvn/nvn_fgpace.h`), presented-time selection (`nvn/nvn_presproj.h`), NVN timestamp units (`nvn/nvn_timestamp.h`), copy strides (`nvn/nvn_copy_pitch.h`) |
| `t_bloom_lut` | Bloom 1/4 on a real Metal replay with closed-form expectations: the full chain, every abort and mid-chain reset leaving B0 black, the cutoff's viewport and scissor guard |
| `t_dirty_race` | The ordering race between "set read-only" and "unprotect before write" in dirty-page tracking |
| `t_gpu_lost` | The GPU watchdog: with work waiting and none finished for `DM_GPU_LOST_S` seconds the GPU is taken as lost, every wait on it returns and nothing more is sent to it |
| `t_hangrec` | The hang recorder: a submission that keeps the GPU busy stands in for a hung one, and the record must name the block that did not finish |
| `t_link_ticks` | The acquire's waits on the display link, counted in its own ticks or in 60 Hz game frames |
| `t_mfx_life`, `t_mfx_on` | MetalFX: the lifetime of the scaler sets (when one is built, replaced and put down), and policy and pixel checks on this Mac's GPU |
| `t_prog_key` | The shader key is taken at `nvnProgramSetShaders`; the replay uses the stored key and no longer reads the control section |
| `t_pso_pend` | Slot recycling of the parallel precompile once its table is full of predictions nobody took |
| `t_quadrun` | 2D quad runs: the look-ahead's rules on hand-made command streams, the verify mode's check, and a real Metal replay that draws the same pixels with the merge off, on and checked |
| `t_refl_lib` | Every reflection json of the current chain version's library goes through the parser the replay uses; one rejection fails the check |
| `t_replay_cuts` | The translation thread's shortcuts: the pools a thread remembers, driver-segment blocks reused within a submission, repeated argument-table writes skipped |
| `t_replay_fix`, `t_replay_san` | Regression tests for replay fixes on a real Metal replay, and for replay defects only a sanitizer shows (built as `t_replay_tsan` and `t_replay_asan`) |
| `t_rt_srgb_view` | Render-target views that change the format: an RGBA8 texture used through an sRGB view stores the sRGB-encoded value, and presenting from an sRGB swapchain texture samples the stored bytes |
| `test_ub_table.py` | `tools/ub_table.py` on synthetic log lines |
| `nvn_replay` (built on demand, not in `run`) | Replays a `DM_NVN_CAPTURE` frame against this tree's replay layer and compares the presented frame with the live one (see `dm/verify/debugging.md`) |

## Common run-time switches

All are environment variables. The iOS App sets `DM_NVN_SHMAP` and `DM_MSL_DIR` itself at startup.

Resource locations:

| Switch | Effect |
|---|---|
| `DM_NVN_SHMAP=path` | The shader map. The default is `../gfx/nvn/shader_map.tsv` relative to the executable's directory; `c1_full.tsv` and `pso_manifest.tsv` are taken from the same directory |
| `DM_MSL_DIR=directory` | The parent directory of the shader libraries; the host loads from `b-<V>/` beneath it. When running the host on a Mac, set it to `$DM_ROOT/build/cache/metal_shader_cache` |
| `DM_PSO_MANIFEST=path` or `off` | `path`: warm only that manifest (its rows after `# warm: background` in the background, as the seed's); `off`: no warm-up. Either way the player's `pso_user.tsv` is not warmed but is still recorded to and deduped against; add `DM_PSO_USER=off` to leave it untouched |
| `DM_PSO_USER=path` or `off` | The user manifest (default `pso_user.tsv` beside the save folder): shaders and pipelines this machine had to build while drawing, warmed at the next launch; `off` = neither read nor written |
| `DM_PSO_WARM_WORKERS=1`–`4` | Workers building the warm-up plan in priority order (default 2) |
| `DM_PSO_PREDICT=0`, `1` | The parallel precompile (predicted pipelines compiled in the background): `0` off, `1` always on; default auto, off for the rest of the run once at least 24 predictions are resolved and fewer than 10% were taken |
| `DM_PSO_RECORD=path` | Appends the shaders and pipelines used in this run to a file, for re-recording the warm-up manifest |

Logging and capture:

| Switch | Effect |
|---|---|
| `DM_NVN_LOG=1` | Logs the creation of NVN objects |
| `DM_NVN_FRAMES=N` | Prints the call table frame by frame for the first N frames (default 3) |
| `DM_NVN_TSV=path` | Writes the per-frame call table and timings as TSV; adding `DM_NVN_TSV_CALLS=0` writes only the timing rows |
| `DM_SLOWFRAME=1` | Writes the timing breakdown into the log only for the frames that dropped; the attribution script is `dm/ios/tools/slowframe.py` |
| `DM_NVN_SHOT=frame[,frame…]:path` | Saves the swapchain texture as an image at the specified frames; `%d` in the path is replaced by the frame number |
| `DM_NVN_SHOT_OUT=WxH` | Makes the screenshot above capture the picture after upscaling to this resolution instead |
| `DM_NVN_TEXDUMP=frame:path` | Saves each color render target used in that frame as an image |
| `DM_NVN_ARRDUMP=frame:dir` | Writes every compressed texture array a copy has filled, all levels and layers as raw blocks; `dm/tools/arrdump.py` reads them (see `dm/verify/debugging.md`) |
| `DM_NVN_PASSDUMP=frame` or `start-end` | Writes each render pass's attachments, samples and presentation for these frames to standard error entry by entry, for analysis by `tools/tbdr_*.py`; additionally setting `DM_NVN_PASSDUMP_ALL` logs every draw |
| `DM_NVN_PASSGPU=file` | Per-pass GPU time (requires device support for counter sampling at stage boundaries) |
| `DM_NVN_PROF=1` | Wraps the implemented NVN functions in a timing wrapper and collects the time each function takes during recording |

Comparison and testing:

| Switch | Effect |
|---|---|
| `DM_NVN_TBDR=0` | Turns off all pass optimizations in the replay layer and takes the plainest path of "all attachments attached, all loaded, all stored", for equivalence comparison |
| `DM_NVN_UNWRITTEN_DC=0` | Turns off the single item "attachments not written in this pass are not stored" |
| `DM_NVN_FUSE_CHECK=1` | Same-frame check of pass merging; logs the comparison result every time |
| `DM_TEST_NO_BC=1` | Forces the decompression path for "the GPU does not support BC compressed textures" on any machine |
| `DM_FPS=30` or `60` | Pins the target frame rate |
| `DM_PRESENT_HZ=N` | Pins the frequency of presentation pacing; 0 means no frame limit and is for performance profiling only. Unset: 60 without a window; with a window the rate follows the refresh rate of the window's screen, at a tier that divides it evenly |
| `DM_NVN_BARRIER=0\|1` | Pass-barrier level (default 1): 0 = every encoder waits for all earlier work and holds back all later work; 1 = a render pass's vertex stage waits only for compute and copies (see `bar_r_begin`) |
| `DM_NVN_PASSGPU_SERIAL=1` | Measurement only: holds the pass-barrier level at 0, so passes run one after another and each `DM_NVN_PASSGPU` span is that pass's exclusive cost (frame totals are inflated); the record's first line says so for `passgpu_top.py` |

## Known limitations

The following paths are not implemented and report red in the log when reached (`RED_ONCE` in `nvn/nvn_mtl.m`):

- clears in the middle of a render pass, and color clears with a partial mask or partial scissor;
- indexed quad primitives, 8-bit indices;
- some forms of texture-to-texture copy and texture clear.

Functions listed in `g_nops` (`hle_nvn.c`), such as `DispatchComputeIndirect` and `SetDepthBounds`, are empty implementations: calling them neither reports red nor has any effect. `SetPatchSize`, `MultiDraw*IndirectCount` and `DownsampleTextureView` aren't among them: they report red if the game ever calls them, which it never has in about 42,000 frames.

Everything the port changes in the game's engine, graphics or otherwise, is listed with its reason, switch, test and measurements in [docs/ENGINE_CHANGES.md](../../docs/ENGINE_CHANGES.md).
