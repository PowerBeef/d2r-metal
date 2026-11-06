#!/usr/bin/env python3
"""The picture settings test runs use, as environment switches: bench.py's --picture, and the golden sets and picture scenarios that build on them.

The command-line host never works out a recommendation (front_mac.m real_recommendation() needs DM_APP), and the first-boot settings of a fresh
save give it a mix of the Mac's and the original values. A run that wants the picture players get therefore pins every picture key through the
switch that forces it, so it reads the same whatever the host's own defaults are (MetalFX included: the CLI host's first boot turns Upscaling on
today, and the remediation's REC-5 fix turns it off).

Each Video-tab key and the switch that pins it:
  Resolution (res; Native = the screen's width)   DM_GAME_RES=<w>x<h>, the render size
  3D Resolution (s3d)                             DM_3D_SCALE (0.5-1.0)
  Upscaling (up, no row: Temporal wherever offered)  DM_MFX (the present) and DM_MFX3D (the game's own 3D upscale pass); MetalFX acts only while it enlarges; DM_MFXT=1 for Temporal
  Frame Generation (fg, no row: on wherever offered) DM_FG: 0 / 1
  Anti-Aliasing (aa, no row: Temporal does it)    DM_AA_MODE: the renderer's mode byte, 1 = FXAA (the original), 2 = SMAA
  Anisotropic Filtering (aniso)                   DM_NVN_ANISO: 0 / 4 / 8 / 16
  Color (color)                                   DM_COLORSPACE: srgb = Accurate (a tag on the layer; no capture sees it)
  Environment Shadows (envsh)                     DM_ENV_SHADOWS: 0 / 1
  Shadow Quality (shq)                            DM_SHADOW_SIZE: 2100 Standard / 3072 High / 4096 Ultra
  Environment Detail (envd)                       DM_ENV_DETAIL: 0 / 1
  Frame Drops (q3d)                               DM_3D_SCALE_DYN=0: the 3D scale stays where it is set
  Display Mode (mode)                             FULLSCREEN below, kept apart because headless shots have no window

The sets:
  REC    this Mac's Recommended (a Mac mini M6 with a 12-core GPU on a 2560 x 1440 screen): the t_picrec row "Mac mini M6, 2560 display (measured)"
         (Native 2560 x 1440, 3D 100%, environment shadows on, Ultra shadows, Frame Drops keep) plus what front_mac.m front_video_firstboot /
         front_video_rec give every Mac (full screen, 16x, Accurate) and every chip with both (MetalFX Temporal, Frame Generation; the Anti-Aliasing
         row reads SMAA, which Temporal replaces, so the mode byte is 0). The Mac App picks exactly this on this Mac: its log line reads
         "picture: Apple M6 ..., 12 GPU cores ..., screen 2560x1440 ... -> Recommended Native 2560, 3D 100%, environment shadows on, shadows Ultra,
         Frame Drops keep".
  RECT   REC at the real frame rate (DM_FG off; players have no such choice). Frame generation changes nothing the game draws, only what the screen shows between: golden pictures use RECT
         (each frame is drawn the same) and dm/verify/motion_gate.py judges the frames in between.
  CONV   the conventional picture of a chip the system offers neither to, on this Mac: MetalFX's spatial scaler for the 3D pass, SMAA, the real frame rate.
  REC75  CONV with the 3D at 75%, so MetalFX does the 3D upscale: the base M chips' conventional picture at a 2560-class screen (the t_picrec rows
         "MacBook Air M1, 1280 x 800" and the M2 13-inch 8-core row at 1280 x 832; the M3's picks 85% since the model reads the picture below
         the notch). Two deliberate differences: those Airs are 16:10 (2560 x 1600 below the notch), and REC75 keeps 2560 x 1440 so it compares
         with CONV; and their Frame Drops default is lower (q3d 0),
         while REC75 keeps the 3D scale fixed (DM_3D_SCALE_DYN=0), because a scale that moves can have no picture baseline.
  PHONE  the iPhone 17 Pro's Recommended, drawn by the Mac host: its native 2622 x 1206, 3D 50% with MetalFX Temporal and Frame Generation (front_ios.m front_video_firstboot, front_video_rec),
         16x and Accurate, and no environment-shadow or shadow-quality recommendation, so environment shadows off and the Switch's 2100 map. Frame Drops is off, as phones have none. It is the phone's
         picture chain, not a phone capture: the Mac host lays out the interface for that size itself. golden.py shoots it at the real frame rate, as RECT.
  MAC_APP_REC   the Mac App test copy takes this Mac's real Recommended at first boot and starts full screen by itself
  FULLSCREEN    the command-line host goes full screen one second after its window opens: real full screen in a front run (bench.py), emulated in
                a background run (scenario.py's default for windowed runs: the off-screen window takes the screen's full-screen size, no Space switch)

A change to Recommended (dm/host/dm_picrec.c, its cases in dm/host/tests/t_picrec_cases.h, front_video_firstboot or front_video_rec in front_mac.m
or front_ios.m, dm_video.c's 3D-scale tables) updates this file in the same commit; test_picture.py ties them together.
Standard library only.
"""

CONV = {
    'DM_GAME_RES': '2560x1440',
    'DM_3D_SCALE': '1.0',
    'DM_MFX': '1',
    'DM_MFX3D': '1',
    'DM_AA_MODE': '2',
    'DM_NVN_ANISO': '16',
    'DM_COLORSPACE': 'srgb',
    'DM_ENV_SHADOWS': '1',
    'DM_SHADOW_SIZE': '4096',
    'DM_ENV_DETAIL': '1',
    'DM_3D_SCALE_DYN': '0',
}
REC75 = dict(CONV, DM_3D_SCALE='0.75')
RECT = dict(CONV, DM_MFXT='1', DM_AA_MODE='0')   # the temporal scaler at the real frame rate: the 3D target at 99%, the game's own AA off with it (hle_oe.c scale3d_tick, aa_tick)
REC = dict(RECT, DM_FG='1')
REC75T = dict(REC75, DM_MFXT='1', DM_AA_MODE='0')   # REC75 with the temporal scaler in place of the spatial one
PHONE = {
    'DM_GAME_RES': '2622x1206',
    'DM_3D_SCALE': '0.5',
    'DM_MFX': '1',
    'DM_MFX3D': '1',
    'DM_MFXT': '1',
    'DM_FG': '1',
    'DM_AA_MODE': '0',
    'DM_NVN_ANISO': '16',
    'DM_COLORSPACE': 'srgb',
    'DM_ENV_SHADOWS': '0',
    'DM_SHADOW_SIZE': '2100',
    'DM_ENV_DETAIL': '0',
    'DM_3D_SCALE_DYN': '0',
}
MAC_APP_REC = {'DM_TEST_VIDEO_FIRSTBOOT': '1'}
FULLSCREEN = {'DM_TEST_FULLSCREEN_AT': '1'}
SETS = {'rec': REC, 'rect': RECT, 'conv': CONV, 'rec75': REC75, 'rec75t': REC75T, 'phone': PHONE}
KEYS = tuple(CONV)   # every set pins every one of these; REC, RECT, REC75T and PHONE also pin DM_MFXT (nothing turns the temporal scaler on in the others: the CLI host's Upscaling is 0)


def env(name, backend='cli', user=None):
    """The environment of picture `name` on `backend`, with the caller's own env winning:
    'orig' = nothing pinned (the host's own defaults, windowed); on the cli backend a set plus FULLSCREEN, unless the caller sizes or
    switches the window itself (DM_TEST_WINSIZE / DM_TEST_FULLSCREEN_AT); on the mac-app backend 'rec' = MAC_APP_REC (the test copy's own
    Recommended), and the other sets are pinned like on cli (the App then starts windowed: DM_TEST_FULLSCREEN_AT puts it in full screen)."""
    user = dict(user or {})
    if name == 'orig': return user
    if name not in SETS: raise ValueError(f'unknown picture {name!r} (orig, {", ".join(SETS)})')
    if backend == 'mac-app' and name == 'rec': out = dict(MAC_APP_REC)
    else:
        out = dict(SETS[name])
        if not ({'DM_TEST_WINSIZE', 'DM_TEST_FULLSCREEN_AT'} & set(user)): out.update(FULLSCREEN)
    out.update(user)
    return out


if __name__ == '__main__':
    import sys
    if len(sys.argv) != 2 or sys.argv[1] not in SETS: sys.exit('usage: picture.py rec|rec75|phone   (prints the set as K=V words)')
    print(' '.join(f'{k}={v}' for k, v in SETS[sys.argv[1]].items()))
