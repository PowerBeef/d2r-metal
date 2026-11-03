// Front-end stubs for t_video, the Mac build of dm_video.c: the strong definitions of dm_video.c's weak front_video_* hooks (they must sit in another
// translation unit than the included dm_video.c, so the linker picks them; weak definitions are never inlined), the settings directory, and a small
// fake of the custom settings store (dm_custom.c) that the Video tab's Frame Rate / Frame Drops rows go through. Two front ends, chosen by g_fe:
//   'G' the generic stub the load cases were written against: recommendation 1 / g_rec_res / 1, first-boot current values 1 / 2 / 0, and for the later
//       keys aa 1, aniso 3, envsh 1, shq 2 (color, s3d: none, so 0)
//   'M' front_mac.m on this Mac (a Mac mini M6 at 2560 x 1440, real recommendation): full screen, Native, MetalFX when g_mfx; SMAA, 16x, Accurate,
//       3D 100%, environment shadows, Ultra shadows (the t_picrec row "Mac mini M6, 2560 display (measured)")
// The phone build has its own stub file (t_video_phone_stubs.c, mirroring front_ios.m). The knobs are set by a case before it forks.
#include <stdio.h>
#include <string.h>
#include "../hle.h"

module_t g_mod[MAX_MOD]; int g_nmod;
char g_dir[512];   // the settings directory (one per test run, under TMPDIR)
int g_nodir;       // 1 = the settings directory is not known yet (dm_custom_path answers 0, as before the save is mounted)
int g_fe = 'G', g_mfx = 1, g_rec_id = -1, g_rec_res = 1;   // which front end, MetalFX support, the machine's rec_id (-1 = none), G's recommended res
int g_fb_calls, g_full_calls, g_full_last = -1, g_color_calls, g_color_last = -1;   // what the code under test asked of the front end

int dm_custom_path(const char *name, char *p, size_t n) { if (g_nodir) return 0; snprintf(p, n, "%s/%s", g_dir, name); return 1; }
uint64_t dm_custom_find_w(uint64_t w, const char *name) { (void)w; (void)name; return 0; }   // no game widgets in a unit test

int front_video_firstboot(int rec[3], int cur[3]) {
    g_fb_calls++;
    if (g_fe == 'M') { rec[0] = 1; rec[1] = 3; rec[2] = g_mfx; for (int i = 0; i < 3; i++) cur[i] = rec[i]; return 1; }
    rec[0] = 1; rec[1] = g_rec_res; rec[2] = 1; cur[0] = 1; cur[1] = 2; cur[2] = 0; return 1;
}
int front_video_rec(const char *key) {
    if (!strcmp(key, "aa")) return 1;
    if (!strcmp(key, "aniso")) return 3;
    if (!strcmp(key, "envsh")) return 1;
    if (!strcmp(key, "shq")) return 2;
    if (g_fe == 'M' && !strcmp(key, "color")) return 1;
    if (g_fe == 'M' && !strcmp(key, "s3d")) return 0;
    return -1;
}
int front_video_mfx_ok(void) { return g_mfx; }
int front_video_rec_id(void) { return g_rec_id; }
int front_video_running_res(void) { return -1; }
void front_video_fullscreen(int on) { g_full_calls++; g_full_last = on; }
void front_video_color(int accurate) { g_color_calls++; g_color_last = accurate; }

// The custom settings store (dm_custom.c), reduced to the two keys the Video tab shows: eco_fps (default 0) and q3d (default g_cdef_q3d).
// set_pending records the last value set per key (-9 = never called)
int g_tshown;                                           // dm_custom_shown: 1 = the picture settings' page is on screen
int g_cdef_q3d = 1, g_cval[2], g_cpend[2] = { -1, -1 }, g_cset[2] = { -9, -9 };
static int ckey(const char *k) { return !strcmp(k, "eco_fps") ? 0 : !strcmp(k, "q3d") ? 1 : -1; }
int dm_custom_shown(void) { return g_tshown; }
int dm_custom_shown_value(const char *key) { int i = ckey(key); return i < 0 ? 0 : g_cpend[i] >= 0 ? g_cpend[i] : g_cval[i]; }
void dm_custom_set_pending(const char *key, int v) { int i = ckey(key); if (i >= 0) { g_cpend[i] = v < 0 ? -1 : v; g_cset[i] = v; } }
int dm_custom_default(const char *key) { int i = ckey(key); return i == 1 ? g_cdef_q3d : 0; }
