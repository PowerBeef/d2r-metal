// Screen geometry of the Mac App (front_mac.m) as pure functions: the game picture's shape on the main screen, the Native output width and the game
// resolution. front_mac.m reads AppKit and passes plain numbers in pixels; t_screen_mac checks every rule. Mac only (the _mac suffix keeps it out of
// the iOS build).
#pragma once
#include <stdint.h>

// The game picture on a main screen of w x h pixels (h = the full-screen content height, below the notch): aspect = width / height; full = 1 when
// the screen is not 16:9 (true full screen: the 3D fills the screen; on a narrower screen the interface is a 16:9 band along the bottom, on a wider
// one it spans the screen as the game lays it out; the front-end pages keep their own shape with bars, dm_uiscreen.h dm_front_res); a 16:9 screen
// (within 0.01) stays 16:9 throughout. A screen narrower than 4:3 (a rotated display, a 5:4 monitor) gets a 4:3 picture with bars (clamped = 1): no
// taller shape has ever run, and at high resolution a tall picture overflows GPU heap 0. A screen wider than DM_SCR_MAX_ASPECT (32:9) gets a 2.4:1
// picture with side bars (clamped = 1): 21:9 (3440 x 1440, 2.39:1) is the widest shape that has run (COL-9 probe, 2026-11-03; the user chose the
// wide picture for 21:9 the same day)
#define DM_SCR_MIN_ASPECT (4.0 / 3)
#define DM_SCR_MAX_ASPECT 2.4
typedef struct { double aspect; int full, clamped; } dm_scr_pic;
dm_scr_pic dm_scr_picture(double w, double h);
// The largest picture Native renders, in pixels: GPU heap 0 (768 MiB, never grows, first-fit; dm_gpuheap.c) must keep room for the next rebuild.
// Chosen from heap readings on 2026-11-03 (CLI host, Ultra and environment shadows, SMAA and FXAA switched in game, 3D 100% <-> 60% rebuilds, run
// exits and entries): at this size the largest free gap never fell under 64 MiB (16:9 84, 16:10 83, 4:3 76 MiB at the smallest); 3840x2400 left 14
// and 3504x2628 3 MiB, and 3840x2880 crashed entering a run. It is 16:9 4K: a 16:10 picture stops at 3642x2276, a 4:3 one at 3324x2494. The rule
// (the user's choice, 2026-11-03): at least 64 MiB contiguous left at the peak after every kind of rebuild has run (3D Resolution 100% <-> 60%,
// the Options-page cycle, ten run exits and entries); the stricter 'largest rebuild + 64 MiB' does not hold even at 16:9 4K (85 MiB left), but
// every rebuild was measured to free before it allocates
#define DM_SCR_MAX_PX (3840.0 * 2160)
// The Native output width (Resolution tier "Native") for a main screen of w_px x h_px pixels (h_px below the notch) and the picture's aspect: the
// picture's own pixel width on that screen (a 21:9 screen's own width; on a 32:9 one the 2.4:1 picture's), within DM_SCR_MAX_PX, even, 1600-3840. Not
// 16-aligned: the engine takes any even width (the iPhone renders 2622 wide), and an aligned width made the present stretch the whole frame by a
// fraction of a percent
int dm_scr_native_w(double w_px, double h_px, double aspect);
// The game resolution for an output width render_w, packed (height << 32 | width): docked (the Mac's mode) or handheld; full = true full screen
uint64_t dm_scr_game_res(int render_w, double aspect, int full, int docked);
// The first window's content size (points) for a picture of this aspect: 1280 points wide, smaller when the screen's visible area (max_w x max_h:
// below the menu bar, beside the Dock, below the title bar) is smaller, never larger; in the picture's shape so it has no bars. 0 x 0 = unknown
void dm_scr_fit_window(double aspect, double max_w, double max_h, double *w, double *h);
// Whether a screen of w x h pixels (h below the notch) still suits the picture the game was launched with (Resolution tier, render width, true full
// screen, aspect): the same shape, and for Native the same width. A fixed tier only needs the same shape (the present scales it)
int dm_scr_matches(int tier, int render_w, int full, double aspect, double w, double h);
// The PC pointer's height in points (front_mac.m pc_hand) for a picture of pic_w x pic_h points on screen: a share of the interface band's height
// (the gauntlet about one inventory cell tall), so on a 16:10 screen it follows the 16:9 band, not the whole picture; 24-160 points
#define DM_SCR_CURSOR_SHARE 0.06
int dm_scr_cursor_pt(double pic_w, double pic_h);
