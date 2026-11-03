// The iOS front end's picture-setting hooks for t_video_phone, the phone build of dm_video.c (the single iOS stub file).
// The values must follow dm/ios/Sources/front_ios.m: front_video_firstboot (first boot = 3D resolution 75% + MetalFX when available)
// and front_video_rec (SMAA, 16x anisotropy, Accurate colour; no recommendation for the other keys). When front_ios.m, dm_video.c's
// REC_V or the phone presets change, change this file and t_video_phone.c's expectations in the same commit.
// P2's tests of dm_video_on_uimsg (Apply / Discard / Default) add their dm_custom_* stubs here rather than in a second phone build.
#include <stdio.h>
#include <string.h>
#include "../hle.h"

module_t g_mod[MAX_MOD]; int g_nmod;
char g_dir[512];   // the settings directory (one per test run, under TMPDIR)
int g_fb_calls, g_color_calls, g_color_last = -1;   // what the code under test asked of the front end (read back in each forked case)

int dm_custom_path(const char *name, char *p, size_t n) { snprintf(p, n, "%s/%s", g_dir, name); return 1; }
int front_video_mfx_ok(void) { return 1; }   // every phone the game runs on has MetalFX
int front_video_firstboot(int rec[3], int cur[3]) {   // front_ios.m: mode 0, res 0 (75%), MetalFX when available; the current values are the recommendation
    g_fb_calls++; rec[0] = 0; rec[1] = 0; rec[2] = front_video_mfx_ok() ? 1 : 0; for (int i = 0; i < 3; i++) cur[i] = rec[i];
    return 1;
}
int front_video_rec(const char *key) {   // front_ios.m: aa SMAA, aniso 16x, color Accurate; -1 = no recommendation (0, the original look)
    if (!strcmp(key, "aa")) return 1;
    if (!strcmp(key, "aniso")) return 3;
    if (!strcmp(key, "color")) return 1;
    return -1;
}
void front_video_color(int accurate) { g_color_calls++; g_color_last = accurate; }   // front_ios.m tags the present layer; here it is only counted
