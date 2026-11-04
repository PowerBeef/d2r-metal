// Screen geometry of the Mac App (front_mac.m) as pure functions: the game picture's shape on the main screen, the Native output width and the game
// resolution. front_mac.m reads AppKit and passes plain numbers in pixels; t_screen_mac checks every rule. Mac only (the _mac suffix keeps it out of
// the iOS build).
#pragma once
#include <stdint.h>

// The game picture on a main screen of w x h pixels (h = the full-screen content height, below the notch): aspect = width / height; full = 1 when
// the screen is narrower than 16:9 (true full screen: the 3D fills the screen, the interface is a 16:9 band along the bottom, dm_uiscreen.c);
// otherwise 16:9 throughout
typedef struct { double aspect; int full; } dm_scr_pic;
dm_scr_pic dm_scr_picture(double w, double h);
// The Native output width (Resolution tier "Native") for a main screen of w_px x h_px pixels and the picture's aspect
int dm_scr_native_w(double w_px, double h_px, double aspect);
// The game resolution for an output width render_w, packed (height << 32 | width): docked (the Mac's mode) or handheld; full = true full screen
uint64_t dm_scr_game_res(int render_w, double aspect, int full, int docked);
