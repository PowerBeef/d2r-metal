# What this port changes in the game's engine

D2R Metal runs the Switch build's own aarch64 code. This page lists everything we change on top of it: what the engine does by itself, what we do instead, why, how to turn it off, what test pins it, and what it measured. `dm/tools/enginemap.py check` (gate step `quick_enginemap`) keeps the page and the sources in step: a rewrite entry, shader rule, hard-coded shader key or raised engine field with no row turns the gate red, and so does a row that cites a switch, test, commit or address that no longer exists.

**When you change engine behaviour, add or update its row in the same commit.** A row's status is `confirmed` when we know what the engine does there, or `assumes:<id>` when it rests on an open question in the private analysis ledger. Addresses are offsets into the original 3.3.94012 `main` image. Everything here is in our own words. No game content or analysis output belongs on this page.

Measurements name their device and date because a number without them doesn't mean much: the Mac's GPU lowers its clock when it has room, so whole-frame GPU time can't rank costs there. Costs are per-pass sums or ABBA runs on an M6 (12 GPU cores) unless a row says otherwise. No other Mac chip has been measured.

## The frame

The game builds one frame as a graph of GPU and CPU jobs and hands the result to its NVN driver. We keep that graph. It runs unchanged in the game's own code, and each NVN call is recorded and replayed as Metal on one translation thread (EC-029). The table follows the GPU order.

"Kept" means the game's pass runs as the game wrote it, through the translated shader. Costs are from an M6 at 2560 × 1440, Recommended, town, rays Full (2026-11-07), except where noted. All the game's passes together are about 4.7 ms of a 19.2 ms GPU frame; the rest is what this port adds.

| Stage | What runs in Metal | Kind | Cost | Off with | Rows |
|---|---|---|---|---|---|
| Frame constants and the camera | Kept, with the game's own Halton jitter switched on under the temporal scaler | Kept + switch | - | `DM_MFXT=0` | EC-018 |
| Interface mask | Kept | Kept | - | - | - |
| Lights gathered | Kept, with more effect lights at High | Kept + field | No cost seen | `DM_ENV_DETAIL=0` | EC-019 |
| Directional shadow map, two layers | Kept, at a larger edge and with environment models casting | Kept + switch | 0.65 + 0.19 ms at 4096 | `DM_SHADOW_SIZE`, `DM_ENV_SHADOWS=0` | EC-016, EC-017 |
| Colour grading table (compute) | Kept | Kept | - | - | EC-047 |
| Terrain composite (compute, one tile at a time) | Kept, with the arrays at the assets' size | Kept | - | - | EC-020 |
| Grass and clutter node | Kept; on Switch most of it is compiled out or empty | Kept | - | - | - |
| Depth prepass | Kept; we note each draw for motion vectors and rays | Kept + notes | - | `DM_MV=0` | EC-040 |
| Light grid (compute) | Kept | Kept | - | - | - |
| Rays (Mac only): structures, traces, blurs | Added before the first forward draw | Added | 5.2 ms kernels, 0.7 ms builds (8.4 in the Cathedral) | `DM_RT=0` | EC-034 to EC-038 |
| Forward opaque | Kept; four shader hooks read the rays' planes | Kept + hooks | About 27% of the game's passes (2026-10-29) | `DM_RT=0` | EC-035 to EC-037 |
| Depth and colour pyramid | Kept | Kept | - | - | - |
| Transparent layers | Kept; tile memory instead of load and store where proven safe | Optimised | About 9% of the game's passes (2026-10-29) | `DM_NVN_TBDR=0` | EC-049 |
| Outline | Kept; its blur is skipped when its input is empty | Optimised | - | `DM_NVN_ZSKIP=0` | EC-046 |
| Bloom | Starts at a quarter size instead of the game's full chain | Replaced | Equal to the full chain in the picture | `DM_NVN_BLOOM_Q=0` | EC-044 |
| Anti-aliasing | The game's own SMAA, selected by its mode byte; off under the temporal scaler | Field | 0.31 ms edge pass | `DM_AA_MODE=0` | EC-021, EC-032 |
| 3D upscale | MetalFX replaces the game's pass | Replaced | About 2.8 ms with motion (2026-11-04) | `DM_MFX3D=0`, `DM_MFXT=0` | EC-039, EC-040 |
| Compose with the interface | Kept; the frame interpolator's result goes in underneath | Kept + added | About 4.6 ms (2026-11-04) | `DM_FG=0` | EC-041, EC-042 |
| Present | Our own present thread and queue, no display link under frame generation | Replaced | 0.13 to 0.16 ms | `DM_PRESENT_LINK=1`, `DM_PRESENT_QUEUE=0` | EC-053 |

## Register

### EC-001 Thread and system-register rewrite
- **Layer:** binary rule
- **Engine:** Reads its thread area through `x18` and `tpidr_el0`, calls the kernel with `svc`, reads the clock through `cntpct_el0`.
- **Ours:** Each such instruction becomes a thread slot access or a jump to a fixed gate into host C. Slot numbers sit in a read-only key page. Nothing is written as code at run time.
- **Why:** Apple's kernel owns those registers, and iOS forbids writing code at run time.
- **Setting:** none
- **Switch:** none
- **Test:** unit_regress
- **Commit:** 46578a2
- **Measured:** none: structural; `dm/verify/regress.sh` proves the rewrite equivalent.
- **Status:** confirmed
- **Covers:** K18, KTLS, KTLR, KGATE

### EC-002 GPU heap 0 budget
- **Layer:** binary rule
- **Engine:** Reserves 260 MiB for render targets, depth and display textures at boot. Texture creation doesn't check for a failed allocation, so a full heap crashes.
- **Ours:** 768 MiB, with the same amount added to the application pool so the main heap keeps its free space.
- **Why:** 260 MiB failed above about 3.45 megapixels, and 512 MiB failed at 3840 x 2160.
- **Setting:** none
- **Switch:** none
- **Test:** native_2940_mac, dm/verify/scenarios/heap
- **Commit:** 3a4628f
- **Measured:** Boots 3840 x 2160 to 3840 x 2880 at 3D 67 to 100%, resident memory unchanged at 1440p (M6, 2026-10-30).
- **Status:** confirmed
- **Covers:** main+0x8524c4

### EC-003 Upload heap budget
- **Layer:** binary rule
- **Engine:** Reserves 400 MiB for per-frame constants and draw data at boot, unchecked. A crowd of about 80 monsters ran it out.
- **Ours:** 640 MiB, with the same 240 MiB added to the application pool.
- **Why:** The crash came at any picture setting.
- **Setting:** none
- **Switch:** none
- **Test:** crowd_upload
- **Commit:** 7ea9998
- **Measured:** Holds about 155 monsters (M6, 2026-11-05). Unverified on a phone.
- **Status:** confirmed
- **Covers:** main+0x8524d0

### EC-004 Offline play without Battle.net
- **Layer:** binary rule
- **Engine:** Allows offline play for 30 days after the last online check.
- **Ours:** The check always passes, and it reads a setting key that always exists.
- **Why:** The port has no online play. The system service layer can't get around the check.
- **Setting:** none
- **Switch:** none
- **Test:** enter_game
- **Commit:** 46578a2
- **Measured:** none: a gate, not a cost.
- **Status:** confirmed
- **Covers:** main+0x1979bc, main+0x1d53e80

### EC-005 Straight to the main menu
- **Layer:** binary rule
- **Engine:** Shows the logo video and the press-A page, and sets online mode on the way.
- **Ours:** Boot goes straight to the main menu. Online mode stays off, so no network check or lost-connection page appears.
- **Why:** There's nothing to connect to, and the console's press-A page means nothing here.
- **Setting:** none
- **Switch:** none
- **Test:** enter_game
- **Commit:** 46578a2
- **Measured:** none: a flow change.
- **Status:** confirmed
- **Covers:** main+0x8019ec

### EC-006 Data block decompression in the host
- **Layer:** binary rule
- **Engine:** Inflates each data block with its own zlib code.
- **Ours:** The entry jumps to the host, which uses libdeflate.
- **Why:** Load speed. Output matched on every block compared.
- **Setting:** none
- **Switch:** none
- **Test:** unit_host
- **Commit:** 46578a2
- **Measured:** none: inherited; not remeasured here.
- **Status:** confirmed
- **Covers:** main+0x1b0ee60

### EC-007 Observation and replay gates
- **Layer:** binary rule
- **Engine:** Delivers client packets to the server loop, runs one server tick, posts interface messages.
- **Ours:** Gates at those points let the host log ticks, packets and interface messages, and replay recorded packets. With nothing attached, a gate goes in and out once, or forwards the call unchanged.
- **Why:** Tick-identical replays are how we prove a change leaves the game's behaviour alone.
- **Setting:** none
- **Switch:** DM_C2S_REPLAY
- **Test:** full_golden_replay, core_scenarios
- **Commit:** 46578a2
- **Measured:** none: test machinery.
- **Status:** confirmed
- **Covers:** main+0x5d3a88, main+0x5d39c8, main+0xb1dc60, main+0x5d3bc0, main+0x6e5b40, main+0x6310b0

### EC-008 Output resolution from the host
- **Layer:** binary rule
- **Engine:** Knows two output sizes, 1280 x 720 and 1600 x 900, with a 1920 x 1080 cap, and derives a bit mask from those two widths.
- **Ours:** The host fills both sizes, the cap is 4096 x 2160, and the width is read whole instead of through the mask.
- **Why:** Native output on any screen. With the mask, a width of 2868 came out as 768.
- **Setting:** Resolution, Picture Size (phones)
- **Switch:** DM_GAME_RES
- **Test:** native_2940_mac, true_full_wide_mac, t_picios, t_picrec
- **Commit:** 46578a2
- **Measured:** none: enables a setting.
- **Status:** confirmed
- **Covers:** main+0x211da30, main+0xe10e48, main+0xe12e88, main+0xe12d8c, main+0xe12e1c, main+0xe10d60, main+0xe10d5c, KRES0, KRES1

### EC-009 Wider screens
- **Layer:** binary rule
- **Engine:** Caps the aspect ratio at 19:9 and draws black bars outside it; the interface canvas stops at 4560 units.
- **Ours:** The host fills the cap with the screen's own ratio, and the canvas follows the screen.
- **Why:** Phones and wide Mac screens. At 16:9 the result is unchanged.
- **Setting:** none
- **Switch:** none
- **Test:** true_full_wide_mac, true_full_1610_mac, t_geom
- **Commit:** 46578a2
- **Measured:** none: enables a shape.
- **Status:** confirmed
- **Covers:** main+0xb1a660, main+0x1663ee8, main+0xb152bc, main+0x23e888, KASPECT, KASPECTUI

### EC-010 Classic picture toggle removed
- **Layer:** binary rule
- **Engine:** Can cross-fade to the classic 2D picture.
- **Ours:** The two state writes that start the fade are gone, so the target stays the new picture.
- **Why:** The classic renderer's assets aren't part of this port's picture path.
- **Setting:** none
- **Switch:** none
- **Test:** full_golden_shots
- **Commit:** 46578a2
- **Measured:** none: a removal.
- **Status:** confirmed
- **Covers:** main+0xb15e20, main+0xb15e24

### EC-011 Language tables
- **Layer:** binary rule
- **Engine:** Loads a legacy string table, then selects between it and the new one by a flag the classic toggle controls.
- **Ours:** The legacy table isn't loaded: after EC-010 nothing reads it. A gate at language selection lets the host follow changes.
- **Why:** Load time and memory.
- **Setting:** none
- **Switch:** none
- **Test:** lang_es, lang_zh
- **Commit:** 46578a2
- **Measured:** none: inherited; not remeasured here.
- **Status:** confirmed
- **Covers:** main+0x7f669c, main+0x317298

### EC-012 Model segment decoding gates
- **Layer:** binary rule
- **Engine:** Decodes model data block by block on its streaming paths.
- **Ours:** The call sites go through the host, which forwards them unchanged by default.
- **Why:** The hook point for pre-decoding.
- **Setting:** none
- **Switch:** none
- **Test:** unit_host
- **Commit:** 46578a2
- **Measured:** none: inherited; forwards by default.
- **Status:** confirmed
- **Covers:** main+0x11f02dc, main+0x11f0914

### EC-013 Item appearance and drop sound gates
- **Layer:** binary rule
- **Engine:** Looks up an item's appearance path and its drop sound.
- **Ours:** The host makes the same call, then may pick a variant for items on the ground.
- **Why:** Inherited hook points for item visibility. Items in storage or worn aren't touched.
- **Setting:** none
- **Switch:** none
- **Test:** unit_host
- **Commit:** 46578a2
- **Measured:** none: inherited.
- **Status:** confirmed
- **Covers:** main+0x468f2c, main+0x8fa590

### EC-014 Where a run starts
- **Layer:** binary rule
- **Engine:** Picks a spawn point for the player and another for the corpse when joining.
- **Ours:** In town both land beside the waypoint.
- **Why:** Inherited convenience. Replays turn it back with `DM_TOWNWP_OFF=0`.
- **Setting:** none
- **Switch:** DM_TOWNWP_OFF
- **Test:** enter_game
- **Commit:** 46578a2
- **Measured:** none: a placement.
- **Status:** confirmed
- **Covers:** main+0x5d8040, main+0x5d8400

### EC-015 A failed save write no longer stops later saves
- **Layer:** binary rule
- **Engine:** After one failed open for writing, it skips its own unmount, so no later commit ever happens. The player loses progress silently on a full disk.
- **Ours:** The failure path unmounts as the success path does, and the file is rewritten at the next save.
- **Why:** A real bug found by reading the save path.
- **Setting:** none
- **Switch:** DM_TEST_FS_FAILAT
- **Test:** save_open_fails
- **Commit:** 715ed80
- **Measured:** Before, one injected failure meant no later commit; after, later saves commit (M6, 2026-10-26).
- **Status:** confirmed
- **Covers:** main+0xe0c04c

### EC-016 Environment shadows
- **Layer:** binary rule
- **Engine:** On Switch, nearly every non-character model is kept out of the shadow casters by its platform tier.
- **Ours:** That test also takes its "may cast" branch when a host slot is set, latched at launch.
- **Why:** Roofs, walls and props cast the shadows PC players see.
- **Setting:** Environment Shadows
- **Switch:** DM_ENV_SHADOWS
- **Test:** full_golden_picture
- **Commit:** 4f952ed
- **Measured:** Shadow passes +0.70 ms in town, +0.38 in the Blood Moor with the 4096 map, at 3840 x 2160 (M6, 6 + 6 ABBA, 2026-11-04). Unmeasured on a phone.
- **Status:** confirmed
- **Covers:** main+0x15cea80, KESHADOW

### EC-017 Shadow map size
- **Layer:** binary rule
- **Engine:** Edge 2100 at the Switch's quality level, up to 4096 at its top level. Shaders carry a bias tuned for 2100.
- **Ours:** The instruction loads the edge from a host slot: 2100, 3072 or 4096, latched at launch.
- **Why:** Character self-shadowing looks better. An inherited cut to 1024 caused coarse acne and is gone.
- **Setting:** Shadow Quality
- **Switch:** DM_SHADOW_SIZE
- **Test:** full_golden_picture
- **Commit:** 92bcad1
- **Measured:** Ultra against Standard at 3840 x 2160: shadow passes +0.59 ms in town, +0.55 in the Blood Moor (M6, 6 + 6 ABBA, 2026-11-04).
- **Status:** confirmed
- **Covers:** main+0x168a580, KSHSIZE

### EC-018 Camera jitter
- **Layer:** binary rule
- **Engine:** Has a Halton(2,3) jitter of 32 steps that no path on Switch selects.
- **Ours:** The two selectors take a host slot's value when it's set, refreshed as each command buffer begins recording.
- **Why:** The temporal scaler needs it, and using the game's own means every shader stays in step.
- **Setting:** none (on with MetalFX Temporal)
- **Switch:** DM_MFXT
- **Test:** rect, rec75t, core_picture
- **Commit:** 190d9d2
- **Measured:** See EC-040.
- **Status:** confirmed
- **Covers:** main+0x1637fbc, main+0x162a814, KJITTER

### EC-019 Environment Detail: terrain tier and effect lights
- **Layer:** engine field
- **Engine:** Copies a boot config into the renderer at its init. On Switch the terrain tier is 0 and the effect-light budget 2.
- **Ours:** A gate at that init raises the tier to 3 and the budget to 4, latched at launch.
- **Why:** Torches light the walls near them and glint on polished floors. The tier changes terrain colours a little.
- **Setting:** Environment Detail
- **Switch:** DM_ENV_DETAIL, DM_TEST_RCFG
- **Test:** recfg, full_metal_validation_rec, full_metal_validation_rt
- **Commit:** ff85299
- **Measured:** No GPU cost at 2560 x 1440, ABBA x4, Far Oasis -0.12 ms, Burial Grounds +0.05 (M6, 2026-11-05). Each alone, share of pixels changed against Standard: the tier 13% in the Outer Cloister (its lawn texture only), 0.1 to 0.3% elsewhere; the lights 2.7% in the Barracks and 1.7% in the Cathedral, where torches burn (M6, fixed clock, 2026-11-07).
- **Status:** confirmed
- **Covers:** main+0x15ff470, renderer+0x6622, renderer+0x66bc

### EC-020 Material arrays stay the size of the assets
- **Layer:** replay layer
- **Engine:** Keeps ground materials and masks in three texture arrays whose edge comes from the boot config (128 px on Switch, 512 at the game's top level), and copies each source level into the array level with the same index, assuming source and array are the same size. The terrain composite reads these arrays only at level 0, for terrain stamps and terrain decals.
- **Ours:** Environment Detail High leaves the edge at the Switch's 128 px, the size of every channel texture the Switch ships (2,524 of them). A copy larger than its source, which only a raised edge asks for, is refused with a red line instead of reading past the source.
- **Why:** Raising the edge to 512 (2026-11-05 to 2026-11-07) shrank each stamp and terrain decal to one corner, a sixteenth of its area: the Outer Cloister lost its authored lawn and moss beds, and the Barracks its paving, uncovering floor blood the artists had paved over. The data, the level's geometry and an artist's 2019 capture of the Cloister agree on the lawn; the player chose the authored look. Related, not a change of ours: the Switch build never draws its ordinary decals (blood stains, ash): at `main+0x165a960` every one of them fails its visibility test (1 against a bound of 0; 12,240 of 12,240 tests in the Barracks, 2026-11-07). The artists' Barracks captures show them.
- **Setting:** Environment Detail
- **Switch:** DM_TEST_RCFG, DM_NVN_ARRDUMP
- **Test:** recfg, t_video, full_golden_picture
- **Commit:** 02ffbbb
- **Measured:** At 512 px the arrays took 73.4 MB and changed 51% of the Outer Cloister's pixels against Standard; at 128 px they take 4.6 MB and match the source textures byte for byte (M6, 2026-11-07).
- **Status:** confirmed
- **Covers:** none

### EC-021 Anti-aliasing mode
- **Layer:** engine field
- **Engine:** Picks FXAA, SMAA or temporal SMAA from a mode byte; the Switch uses FXAA.
- **Ours:** The host writes that byte each tick: SMAA at the conventional picture, none under the temporal scaler. Temporal modes crash without a velocity target, so they're refused.
- **Why:** The game's own better option.
- **Setting:** Anti-Aliasing
- **Switch:** DM_AA_MODE
- **Test:** aa_toggle, core_picture
- **Commit:** cc9cdb1
- **Measured:** SMAA about +0.5 ms before EC-032 (M6, 2026-10-27).
- **Status:** confirmed
- **Covers:** none

### EC-022 3D scale and Frame Drops
- **Layer:** engine field
- **Engine:** Sets its 3D target size through a resolution field. Its own dynamic resolution never shrinks the view here, as our timestamps don't feed it real per-scope times.
- **Ours:** The host writes the field from the 3D Resolution row, and Frame Drops steps it down at most three levels on judged frames.
- **Why:** The world can be upscaled while the interface stays at screen resolution.
- **Setting:** 3D Resolution, Frame Drops
- **Switch:** DM_3D_SCALE_DYN
- **Test:** mfx3d_dyn, t_oe, fg_drops_mac
- **Commit:** b811b90
- **Measured:** none: a controller; the settings it serves are measured in EC-039 and EC-040.
- **Status:** assumes:u-nvn-14
- **Covers:** none

### EC-023 Input: PC, controller and touch
- **Layer:** host take-over
- **Engine:** On Switch takes a controller and touch. PC mouse code and key bindings are in the binary, unused.
- **Ours:** `dm_cursor.c`, `dm_kbm.c`, `dm_world.c`, `dm_padmode.c` and `hle_hid.c` read and write the game's input state. The PC mode calls the game's own PC paths (bindings, hover target, click entry).
- **Why:** Mouse and keyboard behave as on PC, and touch works on phones. No rule changes.
- **Setting:** Controls
- **Switch:** DM_PC_MODE
- **Test:** pc_mode, npc_talk, pc_combat_mac, pc_items_mac, t_kbm, t_touch
- **Commit:** 46578a2, bdc2e06
- **Measured:** none: behaviour, judged by tick-identical replays and scenarios.
- **Status:** confirmed
- **Covers:** none

### EC-024 Interface overlay and our Options pages
- **Layer:** host take-over
- **Engine:** Reads layouts and strings from its data archives.
- **Ours:** An overlay holds only differences (layout patches, string rows) and is checked before the game's files. `dm_custom.c` and `dm_video.c` run the added pages. We never write to the game's own widgets.
- **Why:** Video and Extras tabs, PC layouts, touch layouts.
- **Setting:** none
- **Switch:** DM_ROMFS_OVERLAY
- **Test:** quick_hudgen, t_custom, t_video, t_widget
- **Commit:** 46578a2
- **Measured:** none: interface.
- **Status:** confirmed
- **Covers:** none

### EC-025 Test reads of engine state
- **Layer:** host take-over
- **Engine:** Keeps panels, focus, saves, items and units in its own structures.
- **Ours:** `diag.c` and `dm_engine.c` read them for the control port's `state` and `engine` and for the events tests judge. They only read, except the test-only paths: packet replay (EC-007) and the world freeze, which no player path attaches.
- **Why:** Tests assert what the game knows, not what a picture shows.
- **Setting:** none
- **Switch:** DM_CTL_PORT
- **Test:** engine_state, game_state, t_events
- **Commit:** 46578a2
- **Measured:** none: test machinery.
- **Status:** confirmed
- **Covers:** none

### EC-026 Platform services
- **Layer:** host take-over
- **Engine:** Calls Horizon's services for files, accounts, friends, network, time, audio, video and text input.
- **Ours:** Named take-overs in `hle_*.c`, each with a reason in its file, and a logging stub for anything else.
- **Why:** None of those services exist here.
- **Setting:** none
- **Switch:** DM_SVC
- **Test:** unit_host, t_fs, t_audio, t_movie, t_swkbd, t_unimpl
- **Commit:** 46578a2
- **Measured:** none: structural.
- **Status:** confirmed
- **Covers:** none

### EC-027 Saves
- **Layer:** host take-over
- **Engine:** Writes saves through a mounted save directory, committing transactions.
- **Ours:** The game reads and writes a working folder. A commit clones it and swaps it into place atomically, is refused when a write failed, and then raises an alert.
- **Why:** Saves must survive a crash, a full disk or the app being killed.
- **Setting:** none
- **Switch:** DM_TEST_SAVE_DELAY
- **Test:** full_faults, save_open_fails, swipe_away_saves, system_quit_saves, t_save_hooks
- **Commit:** 46578a2
- **Measured:** none: behaviour.
- **Status:** confirmed
- **Covers:** none

### EC-028 Shader translation
- **Layer:** shader rule
- **Engine:** Ships Tegra machine code.
- **Ours:** Offline chain: bundled Ryujinx translator, SPIR-V, `spirv-cross`, Metal libraries, keyed by a hash of each control section. Compiled with `-fpreserve-invariance`, or the prepass and colour pass round positions differently and the depth-equal test drops pixels.
- **Why:** Nothing can be translated at run time on iOS.
- **Setting:** none
- **Switch:** DM_MSL_DIR
- **Test:** test_compile_convention, test_c1_guard, t_refl
- **Commit:** 46578a2, c8cd85c
- **Measured:** Black one-frame grass flashes, about 20 a minute, gone with the flag (M6, 2026-10-29).
- **Status:** confirmed
- **Covers:** none

### EC-029 NVN on Metal
- **Layer:** replay layer
- **Engine:** Calls NVN, records command memory and submits it.
- **Ours:** `hle_nvn.c` records into the game's own command memory, and `nvn_mtl.m` replays it as Metal at submit, on one translation thread. Memory pools are zero-copy buffers, so a GPU address is a CPU address. The NVN driver never runs.
- **Why:** The core of the port. See `dm/gfx/nvn_metal_mapping.md` and the Metal 4 section below.
- **Setting:** none
- **Switch:** DM_NVN_SYNC
- **Test:** unit_gfx, t_nvn, t_replay_fix
- **Commit:** 46578a2
- **Measured:** Translation thread 2.3 to 2.7 ms per drawn frame in town, 5 to 6 ms in the Cathedral with rays (M6, 2026-11-07).
- **Status:** confirmed
- **Covers:** none

### EC-030 Filter repeated state binds
- **Layer:** replay layer
- **Engine:** Binds state freely, relying on the driver to be cheap.
- **Ours:** The recorder drops a state bind equal to the current one. Texture, sampler and buffer binds aren't filtered, since replay rewrites them.
- **Why:** Translation thread time.
- **Setting:** none
- **Switch:** DM_NVN_BINDFILT
- **Test:** t_ub
- **Commit:** 3178a1b
- **Measured:** Binds -57%, submit-thread instructions -13%, replay -10% (M6, 2026-10-27).
- **Status:** confirmed
- **Covers:** none

### EC-031 Precise pass barriers
- **Layer:** replay layer
- **Engine:** Issues its own barriers against NVN's model, with deferred bits.
- **Ours:** Level 1: a render pass's vertex stage waits only for compute and copies, and compute and copy encoders get a full end barrier. Level 0 serialises everything.
- **Why:** Metal 4's explicit barriers let passes overlap.
- **Setting:** none
- **Switch:** DM_NVN_BARRIER, DM_NVN_PASSGPU_SERIAL
- **Test:** t_bar
- **Commit:** 96e291a
- **Measured:** GPU -0.54 ms, -8.5% (M6, 2026-10-27).
- **Status:** confirmed
- **Covers:** none

### EC-032 SMAA without the HDR curve
- **Layer:** shader rule
- **Engine:** Seven fragment shaders compute the PQ curve for every sample and then choose by an HDR switch that's always off here. The Metal compiler doesn't move that work under the test.
- **Ours:** The rule splits each shader at the switch, so the off copy has the curve as dead code. Each copy computes what the original did for its switch value.
- **Why:** The SMAA edge pass was 18% of the phone's GPU work with its blend pass.
- **Setting:** none
- **Switch:** none
- **Test:** test_dmsh_rules
- **Commit:** e070b94
- **Measured:** Edge pass 0.681 to 0.308 ms, whole-frame GPU 7.25 to 6.72 ms at Recommended (M6, ABBA x4, 2026-10-29).
- **Status:** confirmed
- **Covers:** hdr_switch_split

### EC-033 Anisotropic filtering and colour
- **Layer:** replay layer
- **Engine:** Uses trilinear samplers, and draws for a Switch's display.
- **Ours:** Trilinear, non-compare samplers are raised to 16x. On the Mac the layer can be tagged sRGB ("Accurate").
- **Why:** Sharper floors at the game's angle, and a Mac display's colours.
- **Setting:** Anisotropic Filtering, Color
- **Switch:** DM_NVN_ANISO, DM_COLORSPACE, DM_NVN_SAMPHIST
- **Test:** t_aniso, core_picture
- **Commit:** c988d27, 5c407c0
- **Measured:** 16x about +0.2 ms (M6, 2026-10-27). No measurable cost on an iPhone 17 Pro (2026-10-28).
- **Status:** confirmed
- **Covers:** none

### EC-034 Ray tracing: structures
- **Layer:** replay layer
- **Engine:** Has no ray tracing on Switch.
- **Ours:** Before the first forward draw of a frame, `nvn_rt.inc` draws again what moved, with the shader's own vertex code and no rasterization, which yields world positions. It builds one structure per object and one instance structure. Opaque shadow draws are recognised by their pixel shader. At most 24 new structures a frame enter the scene. Mac only.
- **Why:** Skinning and transforms come from the game's own code. A hundred new structures in one frame could stall the GPU for good.
- **Setting:** Ray Tracing
- **Switch:** DM_RT
- **Test:** video_rt_mac, full_metal_validation_rt, dm/verify/rays_replay.sh
- **Commit:** 52d7e81, 6bb74db
- **Measured:** Builds 0.7 ms per drawn frame (M6, 2026-11-07). Level-change freezes 12 of 120 before the limit, 0 of 60 after (M6, 2026-11-05).
- **Status:** confirmed
- **Covers:** capture_entry, 949495670492f2e4

### EC-035 Ray-traced key shadows and occlusion
- **Layer:** shader rule
- **Engine:** Reads a shadow map for the key light, and has no ambient occlusion in the Switch's forward shaders.
- **Ours:** Half-size traces with a depth-aware box write planes every shader has bound, and hooks multiply them in. Occlusion is divided back out of key and local lights so it stays on ambient. The shadow map stays.
- **Why:** Sharp at contact, softer with distance.
- **Setting:** Ray Tracing
- **Switch:** DM_RT, DM_RT_AO, DM_RT_CHAR
- **Test:** video_rt_mac, full_metal_validation_rt
- **Commit:** 52d7e81
- **Measured:** Full against off: Cathedral +11 ms per drawn frame, town +6.4 at 2560 x 1440 under load (M6, 2026-11-05).
- **Status:** confirmed
- **Covers:** rt_shadow_hook, rt_ao_hook

### EC-036 Fire that casts shadows
- **Layer:** shader rule
- **Engine:** On Switch only a few local lights cast shadows, through small maps.
- **Ours:** Rays go to the first four local lights of each tile, stopping short so a torch doesn't shadow itself. They're traced in compute and read by a hook, which keeps ray tracing code out of the shaders every Mac runs.
- **Why:** Torches and braziers shape a dungeon.
- **Setting:** Ray Tracing (Full)
- **Switch:** DM_RT_LOCAL
- **Test:** video_rt_mac
- **Commit:** cb30a77
- **Measured:** Standard, without them, +7.0 ms against Full's +11.0 (M6, Cathedral, 2026-11-06).
- **Status:** confirmed
- **Covers:** rt_local_hook

### EC-037 One material response for every surface
- **Layer:** shader rule
- **Engine:** The Switch's forward shaders leave out the image lighting and the specular response to surroundings that PC has.
- **Ours:** Mirror rays off every surface read the hit from last frame's picture, and a miss takes the game's image lighting cube at its per-place intensity. The shaders' own normal, roughness and reflectance drive it. A per-level damp table is art direction, as the game has no such value.
- **Why:** The player's direction: one cohesive world, each material believable.
- **Setting:** Ray Tracing
- **Switch:** DM_RT_REFL, DM_RT_WET
- **Test:** video_rt_mac
- **Commit:** d0875c3
- **Measured:** Part of EC-035's figure. The pale wash on the pole near Charsi comes from this term (M6, 2026-11-07).
- **Status:** assumes:u-material-40
- **Covers:** rt_env_hook

### EC-038 Rays: traces first, one barrier
- **Layer:** replay layer
- **Engine:** Has no ray tracing on Switch.
- **Ours:** All traces run in one go, then one barrier, then the blurs.
- **Why:** Fewer stalls between kernels. The result planes are byte-identical to the old order.
- **Setting:** Ray Tracing
- **Switch:** DM_RT
- **Test:** video_rt_mac, dm/verify/rays_replay.sh
- **Commit:** acb1b72
- **Measured:** Cathedral -1.1 ms (M6, ABBA, p 0.03, 2026-11-07).
- **Status:** confirmed
- **Covers:** none

### EC-039 MetalFX replaces the 3D upscale
- **Layer:** replay layer
- **Engine:** Upscales the 3D picture with bilinear sampling, a 5-tap sharpen and an R8 mask.
- **Ours:** Below 100% 3D scale, that draw becomes a copy, MetalFX, and a write-back of colour times the mask. On the Mac, the spatial scaler can also enlarge at the present, once per frame.
- **Why:** Sharper world, and the interface stays at screen resolution.
- **Setting:** 3D Resolution
- **Switch:** DM_MFX, DM_MFX3D, DM_MFX_STACK
- **Test:** mfx3d_dyn, rec75, t_mfx_on, t_mfx_life
- **Commit:** 4ac0543, 7c2f892
- **Measured:** iPhone 17 Pro: 11.65 to 12.28 ms, edge variance 404 to 728 (2026-10-25). M6: no significant cost in ABBA x4 (2026-10-29).
- **Status:** confirmed
- **Covers:** 52aac1f2306b8fc5

### EC-040 MetalFX Temporal and motion vectors
- **Layer:** shader rule
- **Engine:** Has no velocity pass on Switch, and its camera is orthographic in the world.
- **Ours:** The rule adds a second vertex entry that runs the shader's body on this frame's and last frame's constants. At the upscale we fill motion from depth and the two view-projections, then redraw what changed, depth-equal. A sharpen on write-back restores detail.
- **Why:** Stable edges and the input frame generation needs.
- **Setting:** none (on where the chip has it)
- **Switch:** DM_MFXT, DM_MV
- **Test:** rect, rec75t, core_motion
- **Commit:** b27766f, 190d9d2, 16b1e92
- **Measured:** About 2.8 ms with motion on an M6 at full clock (2026-11-04). Slow frames in town 1.06% to 0.00%, p 0.002, cause not established.
- **Status:** confirmed
- **Covers:** velocity_entry

### EC-041 Frame generation
- **Layer:** replay layer
- **Engine:** Presents the frames it draws.
- **Ours:** MetalFX's frame interpolator runs between two Final Results, and this frame's interface is blended over at the game's compose draw, so it's never warped.
- **Why:** Render 30, show 60, which pays for rays.
- **Setting:** none (on where the chip has it)
- **Switch:** DM_FG
- **Test:** video_fg_mac, core_motion, t_fgpace
- **Commit:** ff8b6b8
- **Measured:** About 4.6 ms. Input to picture 96 ms against 73.5 at real 60 (M6, 2026-11-04).
- **Status:** confirmed
- **Covers:** 9a650561e5c40791

### EC-042 Moving things stay solid in generated frames
- **Layer:** replay layer
- **Engine:** Has no frame interpolation.
- **Ours:** What really moves is painted out of the interpolator's input and put back half way along its own motion.
- **Why:** The interpolator alone loses thin things against sliding scenery and keeps a pale copy.
- **Setting:** none
- **Switch:** DM_FG_FORE
- **Test:** core_motion
- **Commit:** ed27fbe
- **Measured:** +0.5 ms GPU at 2560 x 1440 (M6, 2026-11-05).
- **Status:** confirmed
- **Covers:** none

### EC-043 Skipping FXAA for comparison
- **Layer:** replay layer
- **Engine:** Runs FXAA at its AA mode.
- **Ours:** A test switch redirects FXAA's reader to its input and skips the draw. Off by default.
- **Why:** A/B of what FXAA costs.
- **Setting:** none
- **Switch:** DM_NVN_NOFXAA
- **Test:** none: a test switch, nothing the player reaches.
- **Commit:** 46578a2
- **Measured:** FXAA edge 0.20 ms against SMAA's 0.68 before EC-032 (M6, 2026-10-29).
- **Status:** confirmed
- **Covers:** ebcfcd1fa0a77dbc

### EC-044 Bloom from a quarter size
- **Layer:** replay layer
- **Engine:** Runs its bloom chain from a larger size.
- **Ours:** The cutoff and downsample are redirected to start at a quarter size, when the cutoff's viewport covers its target. A chain left unfinished clears its output to black.
- **Why:** GPU time with no visible loss.
- **Setting:** none
- **Switch:** DM_NVN_BLOOM_Q
- **Test:** t_bloom_lut, full_golden_shots
- **Commit:** 46578a2, 85eda5f
- **Measured:** Measured equal to the game's full chain in the picture (M6, 2026-10-27).
- **Status:** confirmed
- **Covers:** 2938a5b71b8e21b2, 019ce2f82edbda84, cc264583f2000c81, 2e2d6700a2569795

### EC-045 Extrapolated 3D frames (off)
- **Layer:** replay layer
- **Engine:** Draws the whole 3D scene each frame.
- **Ours:** An inherited extrapolation path keeps only skinned units on alternate frames and moves last frame's picture. Off by default: it stuttered in released builds upstream. The ten keys name skinned vertex shaders.
- **Why:** Inherited as a power saver.
- **Setting:** none
- **Switch:** DM_RT_AB, DM_RT_AB_TOGGLE
- **Test:** none: off by default, no player path.
- **Commit:** 46578a2
- **Measured:** none: not used.
- **Status:** confirmed
- **Covers:** 0fa4eb7559f1637a, 1d97d33ab66abcba, 3deaf68fd65b5298, 4dffa355f2fbcdaf, 54c93fa22abff9ee, 64dd7d022f3fae39, 67e2ebc28c830f44, add86ee8e20f89b9, b3ae9be5cc670f35, fbf6dc1873a449e9

### EC-046 Zero-source skip
- **Layer:** replay layer
- **Engine:** Blurs its outline target every frame, empty or not.
- **Ours:** When every texture a proven shader samples is known to be all zero, the draw is skipped and its target counts as zero. The proof is written in the source for each key.
- **Why:** Most frames have no outline.
- **Setting:** none
- **Switch:** DM_NVN_ZSKIP
- **Test:** t_zskip
- **Commit:** 46578a2
- **Measured:** none: inherited; not remeasured here.
- **Status:** confirmed
- **Covers:** 4a819818b8e489c5

### EC-047 Colour table probe
- **Layer:** replay layer
- **Engine:** Computes its colour grading table in a compute pass.
- **Ours:** Notes the table when that dispatch runs, so a probe can copy a few cells at present and log a red line when the picture turns magenta.
- **Why:** A player's video showed that on an A16 phone under heavy load; the cause was never found on our side.
- **Setting:** none
- **Switch:** none
- **Test:** unit_gfx
- **Commit:** 42283dc
- **Measured:** none: diagnostic.
- **Status:** confirmed
- **Covers:** 552f45c348a61b86

### EC-048 Texture re-upload and block-linear layout
- **Layer:** replay layer
- **Engine:** Writes textures into pool memory in Tegra block-linear layout.
- **Ours:** Deswizzled at upload. Write protection detects a later rewrite, and the texture is uploaded again.
- **Why:** Metal textures aren't block-linear.
- **Setting:** none
- **Switch:** DM_TEST_NO_BC
- **Test:** t_deswz_gpu, t_dirty_slots, t_dirty_race
- **Commit:** 46578a2
- **Measured:** none: structural.
- **Status:** confirmed
- **Covers:** none

### EC-049 Tile memory: load, store and pass merging
- **Layer:** replay layer
- **Engine:** Is written for an immediate GPU: attachments stay live across passes.
- **Ours:** Attachments not read later aren't stored, and ones not read first aren't loaded. Neighbouring passes merge when they can read the tile, using "readable tile" fragment shader variants.
- **Why:** On tile-based GPUs, load and store are the expensive part.
- **Setting:** none
- **Switch:** DM_NVN_TBDR, DM_NVN_UNWRITTEN_DC, DM_NVN_XV, DM_NVN_FUSE_CHECK
- **Test:** t_fuse_check, t_reorder_chain, t_defer_chain, t_xv, t_pfv, t_rt_range, test_tbdr_budget
- **Commit:** 46578a2
- **Measured:** none: inherited; not remeasured here.
- **Status:** confirmed
- **Covers:** none

### EC-050 Pipeline warm-up
- **Layer:** replay layer
- **Engine:** Has its shaders ready, as a console driver's are.
- **Ours:** Pipelines are built in the background from a seed manifest and from what this machine used last time, plus a predicted background compile. Pipeline states are cached by state hash.
- **Why:** No hitches at first use of a pipeline.
- **Setting:** none
- **Switch:** DM_PSO_MANIFEST, DM_PSO_USER, DM_PSO_PREDICT, DM_PSO_WARM_WORKERS
- **Test:** core_pso_cli, full_pso_mac, full_pso_ios, t_pso_warm, t_pso_skey
- **Commit:** 46578a2
- **Measured:** Warm compiles about 2 in 1000 frames (M6, 2026-10-27).
- **Status:** confirmed
- **Covers:** none

### EC-051 Timestamps, colour outputs and LOD bias
- **Layer:** replay layer
- **Engine:** Reads GPU timestamps and expects colour outputs and sampler LOD bias to work as on NVN.
- **Ours:** Real timestamps through a counter heap, per-target colour outputs and LOD bias, adapted from nxnative. Each frame's GPU span is one value, so the game sees no real per-scope times (EC-022).
- **Why:** Correctness and measurement.
- **Setting:** none
- **Switch:** DM_NVN_PASSGPU
- **Test:** t_timestamp, t_rt_srgb_view
- **Commit:** bb88916, 6429431, 6886351
- **Measured:** none: correctness.
- **Status:** confirmed
- **Covers:** none

### EC-052 Frame capture and pass records
- **Layer:** replay layer
- **Engine:** Has no such tools.
- **Ours:** Test switches record a frame or per-pass data, and dump the texture arrays.
- **Why:** Measurement and proof.
- **Setting:** none
- **Switch:** DM_NVN_CAPTURE, DM_NVN_PASSDUMP, DM_NVN_PASSGPU, DM_NVN_ARRDUMP, DM_LINK_TRACE
- **Test:** t_capture, test_passgpu_top
- **Commit:** 3458763
- **Measured:** none: tools. Usage in `dm/verify/debugging.md`.
- **Status:** confirmed
- **Covers:** none

### EC-053 Present
- **Layer:** present
- **Engine:** Acquires a texture, waits for vertical sync there, and queues a present.
- **Ours:** The present thread draws the swap texture to a `CAMetalLayer`. With frame generation there's no display link: as in Apple's guide, each image stays on screen at least 90% of a refresh, a frame starts from the last generated image's real time plus two refreshes minus a lead, and presents go on a queue of their own.
- **Why:** A late link callback cost a whole refresh on the Mac; on the phone a late generated frame discarded the real one.
- **Setting:** none
- **Switch:** DM_PRESENT_LINK, DM_PRESENT_QUEUE, DM_FG_LEAD_MS, DM_PRESENT_HZ
- **Test:** native_2940_mac, t_fgpace, t_pace, t_adaptive_pace, t_link_ticks
- **Commit:** 4eda105, 4d31858
- **Measured:** Mac town: 15 late and 12 discarded to 0 (M6, 2026-11-06). iPhone 17 Pro town: 1.7 to 1.9% late to 0.06 to 0.11%, none discarded (2026-11-06).
- **Status:** confirmed
- **Covers:** none

## Metal 4

The port runs only on Metal 4 (macOS and iOS 27 and later, decided 2026-10-27). The encoder macros at the top of `nvn_mtl.m` name the Metal 4 types.

- **Queues and allocators:** one `MTL4CommandQueue` for replay and another for presents under frame generation, with `MTL4CommandAllocator`s per frame in flight. Fences come from an `MTLSharedEvent`, so a game `Sync` only clears after the GPU has really finished.
- **Argument tables:** vertex, fragment and compute each have an `MTL4ArgumentTable`, filled from each shader's reflection. Encodes come to about one per draw (M6, 2026-11-07), and that's still a known cost.
- **Residency:** one residency set per device, holding every pool buffer and texture, attached to the queues. A new pool is committed on the spot, before any command buffer that can reach it.
- **Barriers:** explicit, at two levels (EC-031). The game's own deferred barriers come through as at most one per draw, dispatch or pass.
- **Tile work:** load, store and pass merging (EC-049). There are no heaps, memoryless targets, indirect command buffers, mesh or tile shaders.
- **Pipelines:** still built through the device path, warmed in the background (EC-050). `MTL4Compiler` is used only for MetalFX.
- **MetalFX:** the spatial and temporal scalers and the frame interpolator, using the 27 systems' matrices, which suit the orthographic world camera (EC-039 to EC-042).
- **Ray tracing:** Mac only. Acceleration structures built with Metal 4 commands, compute kernels for the traces, and no ray tracing code in fragment shaders (EC-034 to EC-038).
- **Timing:** `MTL4CounterHeap` for per-pass timestamps. Whole-frame GPU time can't rank costs on a fast Mac, so compare pass sums, energy and slow frames.

## Tried and rejected

Listed so they aren't tried again without new evidence. All on an M6 unless noted.

| Idea | Result | Date |
|---|---|---|
| Refit acceleration structures instead of rebuilding | Nothing measurable, ABBA 4 + 4; removed | 2026-11-07 |
| 16 x 16 threadgroups for the ray kernels | Nothing measurable; removed | 2026-11-07 |
| Fast-intersection structure usage | Nothing measurable; removed | 2026-11-07 |
| Rays replace the raster shadow map | Dropped on the look at the player's decision; about 0.8 ms in town | 2026-11-07 |
| Compose into the drawable | The present costs only 0.13 to 0.16 ms | 2026-11-07 |
| Small pass skips | The transparency resolve reads pyramid levels 1 and 2 every frame; nothing above 0.1 ms | 2026-11-07 |
| The engine's dynamic resolution on real GPU times | Reverted experiment; real per-scope times are still missing (EC-022) | 2026-11-07 |
| Zeroed top levels in the material arrays | Stamps and terrain decals vanish entirely | 2026-11-07 |
| 512 px material arrays at Environment Detail High | Stamps and terrain decals shrink to a corner with the Switch's 128 px assets: the Outer Cloister's lawn became a stone court (EC-020) | 2026-11-07 |
| Game quality level 2 and above | Level 2 mirrors characters on the ground; levels 3 and 4 crash | 2026-10-27 |
| Temporal SMAA | Needs a velocity target the Switch build doesn't create; crashes | 2026-10-27 |
| Reflections field at `renderer+0x6634` | Everything turns into a mirror: nothing writes the target it reads | 2026-11-05 |
| Bloom mode 2 | Misfires our quarter-size bloom | 2026-11-05 |
| Host at -O2 | No significant gain | 2026-10-27 |
| Game sharpening over MetalFX | Over-sharpens | 2026-10-27 |
| Negative mip bias under the temporal scaler | Nothing measurable | 2026-11-04 |
| Thread quality of service by role | No energy change on the Mac | 2026-10-27 |
| Forward pass switch specialisation | At most -0.06 ms: the work sits in real branches | 2026-10-29 |
| Pass overlap beyond the barrier levels | Upper bound -0.1 ms | 2026-10-27 |
