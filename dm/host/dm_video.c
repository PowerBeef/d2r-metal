// Picture settings: the rows of the Options Video tab (layout touch_layout.py _VIDEO), stored in sw_video.txt beside the custom settings (dm_custom_path).
// The Mac page has two sections: Display (Display Mode, Resolution, Color) and Graphics (Graphic Presets, 3D Resolution, Upscaling, Anti-Aliasing,
// Anisotropic Filtering, Environment Shadows, Shadow Quality, Frame Rate, Frame Drops). The phone page (DM_PHONE, overlay DM_OVERLAY_IOS) has the Graphics
// section only: no Display Mode or Resolution (phones render at the screen's native size), and its 3D Resolution is the res row.
// Keys, saved by name (new keys go at the end); value 0 is always the original look, which the CLI host and Mac test copies
// (without DM_TEST_VIDEO_FIRSTBOOT=1) keep:
//   mode   0 windowed / 1 full screen (Mac). Switched at once on Apply; the menu and ⌃⌘F write it back (dm_video_fullscreen_note)
//   res    Mac: the output width, 0 = 1600 / 1 = 1920 / 2 = 2560 / 3 = Native (the main screen's backing-store width, front_mac.m native_w), at the next
//          launch. Phones: the 3D resolution, stored 0..4 = 75 / 85 / 100 / 67 / 50 % (the first three are the oldest values) and listed 50 .. 100 %
//          through car_of / val_of, live
//   up     0 Standard / 1 MetalFX / 2 MetalFX Temporal: the present's enlargement on the Mac, and the game's 3D upscale pass (nvn_mtl.m mfx3d_draw), from the next frame.
//          2 is the temporal scaler in that pass (nvn_mv.inc), which also takes over from Anti-Aliasing; the present's enlargement stays the spatial scaler
//   aa     0 FXAA (the original) / 1 SMAA: the renderer's mode byte, written by hle_oe.c aa_tick (dm_video_aa_mode)
//   aniso  0 the game's own / 4x / 8x / 16x: the sampler override, latched per submission by nvn_mtl.m aniso_latch
//   color  0 Vivid (the present layer untagged) / 1 Accurate (tagged sRGB, front_video_color), at once
//   s3d    Mac: the 3D Resolution with Native output, 0 = 100 / 85 / 75 / 67 / 50 %, read every tick by hle_oe.c s3_cfg (dm_video_scale3d)
//   envsh  environment shadows; shq the shadow map, 0 Standard 2100 / 1 High 3072 / 2 Ultra 4096 texels. Both are latched at launch, because every guest
//          thread gets them through thread.c's KESHADOW and KSHSIZE slots (dm_video_env_shadows / dm_video_shadow_size); a change shows the restart hint
//          (none while DM_ENV_SHADOWS / DM_SHADOW_SIZE force them; DM_SHADOW_SIZE is clamped to 512..4096 and made even)
// Recommended: rec_<key> holds the recommendation, rec_v its version (REC_V) and rec_id the machine it was worked out for (front_video_rec_id: the Mac
// App's GPU and main screen; none on iOS, the CLI host and test copies without DM_TEST_VIDEO_FIRSTBOOT=1). It is worked out (front_video_firstboot for
// the first three keys, front_video_rec for the others) on first boot, for a missing or out-of-range key (only that key is filled), and when rec_v is
// older or rec_id differs (a move): a player still on Recommended moves with it, changed settings stay. A file that cannot be read, or that a newer
// version wrote, is not rewritten at load; lines with a key this build does not know are kept, and lines longer than 62 characters are skipped.
// Display preferences (pref_key: Display Mode, and Color on the Mac) never decide whether the picture is on Recommended: first launch and Default set
// them, the Graphic Presets row leaves them alone, and a move changes one only while it still equals its old recommendation.
// Graphic Presets is derived: the preset the player last picked while the values still match it, else Recommended, Performance or Quality (tier_of),
// whichever matches first, else Custom.
// On the page a selector changes a pending value. Apply makes it take effect and saves: Display Mode and Color at once, Upscaling, Anti-Aliasing,
// Anisotropic Filtering and 3D Resolution from the next frame, Resolution, Environment Shadows and Shadow Quality at the next launch ("Takes effect after
// restarting the game" under the row). Discard drops the pending values; Default sets every key back to the recommendation, and Frame Rate / Frame Drops
// to their defaults. The "has changes" bit is shared with the custom settings (dm_custom.c lights Apply from dm_video_dirty too). Events
// (dm/verify/event_stream.md): 'picture' at load and after an Apply that saved, 'video_row' for each change the player makes.
// Frame Rate and Frame Drops are custom keys (eco_fps / q3d in sw_custom.txt, dm_custom.c): this file only drives their selectors, pending values and
// Apply go through the custom settings, and the presets leave them alone. The Frame Rate row's two options name the real rates on the window's
// screen (fps_labels: 72 / 36 fps on a 144 Hz screen), the layout's 60 / 30 fps where those are the rates.
// Phones: Recommended is 75% + MetalFX + SMAA + 16x + Accurate on every device (front_ios.m); no Display Mode, Native or s3d.
// Selectors are the game's CarouselWidget (read on the host, 3.3.94012): the option count at +0x7e8, the current option at +0x7ec, and from +0x7d8 one byte
// per option saying whether it can be chosen (0 = skipped). The current option is set through the game's own setter main+0xab0c84 (widget, index), which
// clamps to the count, skips options that cannot be chosen and shows the option's text in the current language: it looks the option's string (from +0x558,
// 0x28 bytes each) up in the string table, shows the string itself when no entry has that name, and writes the text into its text box (the pointer at
// +0x7f8). The vtable and the option count are checked before any read or write.
#include "hle.h"
#include "dm.h"
#include "dm_cursor.h"
#include "dm_i18n.h"
#include "dm_widget.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t dm_custom_find_w(uint64_t w, const char *name);
int dm_custom_shown(void);
int dm_custom_path(const char *name, char *p, size_t n);
enum { K_MODE, K_RES, K_UP, K_AA, K_ANISO, K_COLOR, K_S3D, K_ESH, K_SHQ, K_FG, NV };   // new keys go at the end (saved by name; keys an older file lacks are filled at load)
static const char *const k_keys[NV] = { "mode", "res", "up", "aa", "aniso", "color", "s3d", "envsh", "shq", "fg" };
static const int k_n[NV] = { 2, 4, 3, 2, 4, 2, 5, 2, 3, 2 };
#define REC_V 9   // version of the recommendation: 2 = native output + 3D resolution (Mac); 3 = iOS also recommends SMAA, 16x, accurate colour; 4 = environment shadows (Mac); 5 = Ultra shadows (Mac);
                  // 6 = a Mac on an iPhone chip (MacBook Neo) gets the phone's (front_mac.m); 7 = chosen from the chip's generation, GPU cores and the screen (dm_picrec.c);
                  // 8 = Native's light ladder stops at 67%, an iPhone chip keeps the phone's shadows, A-chip table and smallest-part core counts (dm_picrec.c);
                  // 9 = MetalFX Temporal with Frame Generation wherever the chip has both (front_mac.m, front_ios.m rec_fg)
static int kn(int i) { return i == K_RES && DM_PHONE ? 5 : k_n[i]; }   // on phones res is the five 3D-resolution tiers; on the Mac res has the extra "Native" tier
// Display preferences: the Mac's Display section (Display Mode, Color). First launch and Default set them from the recommendation, but they never decide
// whether the picture is on Recommended, the Graphic Presets row leaves them alone (as Performance and Quality always did), and a rec_v or screen move
// changes one only while it still equals its old recommendation. On phones Color sits in the Graphics section next to Graphic Presets, so it stays part
// of Recommended there; phones have no Display Mode row (mode is always 0, its recommendation)
static int pref_key(int k) { return k == K_MODE || (k == K_COLOR && !DM_PHONE); }
#define NR 13
static const char *const k_rows[NR] = { "mode", "res", "color", "preset", "s3d", "upscale", "aa", "aniso", "envsh", "shq", "fps", "fg", "drops" };   // row order on the page (rows the page lacks are skipped)
static const int k_row_key[NR] = { K_MODE, K_RES, K_COLOR, -1, K_S3D, K_UP, K_AA, K_ANISO, K_ESH, K_SHQ, -2, K_FG, -2 };   // -1 = graphic presets (derived); -2 = a custom settings key (k_row_ckey)
static const char *const k_row_ckey[NR] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "eco_fps", 0, "q3d" };
static int row_n(int i) { return k_row_key[i] >= 0 ? kn(k_row_key[i]) : k_row_key[i] == -1 ? 4 : 2; }   // options in the selector: the key's tiers; presets 4; custom settings keys 2 (options 0 / 1 = values 0 / 1: 60 / 30 fps; lower / keep)
int dm_custom_shown_value(const char *key); void dm_custom_set_pending(const char *key, int v); int dm_custom_default(const char *key);
static _Atomic int g_crows;   // the custom-key rows the page has (one bit per row): Default resets only those
enum { P_REC, P_PERF, P_QUAL, P_CUSTOM };
static _Atomic int g_val[NV], g_pend[NV] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, g_rec[NV], g_loaded, g_repaint;
_Static_assert(NV == 10, "g_pend initialiser count");
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // load / save: written by both the front end's main thread (launch, full-screen changes) and the game thread (Apply)

// Provided by the front end (front_mac.m, front_ios.m); builds without one (unit tests) use the weak definitions here
__attribute__((weak)) int front_video_firstboot(int rec[3], int cur[3]) { (void)rec; (void)cur; return 0; }
__attribute__((weak)) void front_video_fullscreen(int on) { (void)on; }
__attribute__((weak)) int front_video_running_res(void) { return -1; }
__attribute__((weak)) int front_video_mfx_ok(void) { return 0; }
__attribute__((weak)) int front_video_rec(const char *key) { (void)key; return -1; }   // the recommendation for the keys after front_video_firstboot's three; -1 = none (0, the old look)
__attribute__((weak)) void front_video_color(int accurate) { (void)accurate; }   // Color applied, or Accurate read at load
// Which machine the recommendation is worked out for (front_mac.m: dm_picrec_id of the GPU and the main screen); -1 = the recommendation does not depend on
// the machine (iOS, the CLI host, test copies without DM_TEST_VIDEO_FIRSTBOOT=1)
__attribute__((weak)) int front_video_rec_id(void) { return -1; }
// 1 = the window's screen no longer suits the launch picture and a restart would fix it (front_mac.m picture_screen_check): the Resolution row shows
// the restart hint even though its value did not change; 0 = no (iOS, the unit tests)
__attribute__((weak)) int front_video_screen_stale(void) { return 0; }
// The refresh rate of the window's screen the Frame Rate row is labelled for (Hz, 0 = no window): front_mac.m lets a test stand in for it
// (DM_TEST_SCREEN_HZ); elsewhere the present's own reading (nvn_mtl.m mtl_display_refresh)
double mtl_display_refresh(void); double dm_pace_hz(double screen_hz, double floor_hz); double dm_eco_hz(double screen_hz);
__attribute__((weak)) double front_video_screen_hz(void) { return mtl_display_refresh(); }

static const char *vals_str(const int *v) {   // "mode=1 res=1 ...": for logs (static buffer, not thread-safe; only used under g_mu or at launch)
    static char b[160]; size_t L = 0; b[0] = 0;
    for (int i = 0; i < NV && L < sizeof b; i++) L += (size_t)snprintf(b + L, sizeof b - L, "%s%s=%d", i ? " " : "", k_keys[i], v[i]);
    return b;
}
static _Atomic int g_rv_keep = REC_V;   // the rec_v saved: REC_V, or a newer file's own (an older build must not claim a newer file's values as its own)
static char g_extra[512];               // whole lines of the file whose key this build does not know (a newer version's), written back after ours (under g_mu)
static _Atomic int g_rec_id = -1;       // the rec_id saved with the recommendation: the machine it was worked out for (front_video_rec_id), or a newer file's own; -1 = none
static void save_locked(void) {
    char p[1100], t[1120]; if (!dm_custom_path("sw_video.txt", p, sizeof p)) { LOG("【红】画面设置：设置目录还不知道，没存盘"); return; }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); int ok = f != NULL;
    for (int i = 0; ok && i < NV; i++) ok = fprintf(f, "%s=%d\nrec_%s=%d\n", k_keys[i], atomic_load(&g_val[i]), k_keys[i], atomic_load(&g_rec[i])) > 0;
    if (ok) ok = fprintf(f, "rec_v=%d\n", atomic_load(&g_rv_keep)) > 0;
    if (ok && atomic_load(&g_rec_id) >= 0) ok = fprintf(f, "rec_id=%d\n", atomic_load(&g_rec_id)) > 0;
    if (ok && g_extra[0]) ok = fputs(g_extra, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(t, p) == 0;   // write a temporary file, then rename (as sw_custom.txt)
    int v[NV], r[NV]; for (int i = 0; i < NV; i++) { v[i] = atomic_load(&g_val[i]); r[i] = atomic_load(&g_rec[i]); }
    char rs[160]; strlcpy(rs, vals_str(r), sizeof rs);
    LOG("画面设置：存盘%s %s（推荐 %s）", ok ? "" : "【红】失败", vals_str(v), rs);
}
static void picture_ev(const char *how) {   // the 'picture' event: the values in effect, the rec_v and rec_id saved
    DM_EV("picture", "how:s mode:d res:d up:d aa:d aniso:d color:d s3d:d envsh:d shq:d fg:d rec_v:d rec_id:d", how, atomic_load(&g_val[K_MODE]), atomic_load(&g_val[K_RES]),
          atomic_load(&g_val[K_UP]), atomic_load(&g_val[K_AA]), atomic_load(&g_val[K_ANISO]), atomic_load(&g_val[K_COLOR]), atomic_load(&g_val[K_S3D]), atomic_load(&g_val[K_ESH]),
          atomic_load(&g_val[K_SHQ]), atomic_load(&g_val[K_FG]), atomic_load(&g_rv_keep), atomic_load(&g_rec_id));
}
_Static_assert(NV == 10, "the picture event lists every key");
static void load_once(void) {
    if (atomic_load(&g_loaded)) return;
    pthread_mutex_lock(&g_mu);
    char p[1100];
    if (!atomic_load(&g_loaded) && dm_custom_path("sw_video.txt", p, sizeof p)) {
        // Read key by key: only a missing or out-of-range key is filled from the first-boot values, the rest keep what was read (one missing rec_* used to
        // treat the whole file as a first boot). A value that is not a whole number counts as out of range. An unreadable file (anything but "no such
        // file") counts as a first boot for this launch and is not rewritten; a newer version's file is filled in memory only. Lines with a key this build
        // does not know are kept (g_extra); a line longer than 62 characters cannot hold a known key and is skipped whole
        int v[NV], r[NV], rv_file = 1, id_file = -1; for (int i = 0; i < NV; i++) v[i] = r[i] = -1;   // rv_file: the recommendation version in the file (absent = 1); id_file: its rec_id
        errno = 0; FILE *f = fopen(p, "r"); int err = errno, ro = !f && err != ENOENT;
        char *ln = NULL; size_t cap = 0, xn = 0; ssize_t got; g_extra[0] = 0;
        while (f && (got = getline(&ln, &cap, f)) > 0) {
            // Over-long: more than 62 characters before the newline (the 64-byte buffer this parser used to read with), judged by the bytes read, so a
            // NUL inside a line does not make it look like the first piece of a longer one. Past that, the line is read as a string, up to a NUL
            if (got - (ln[got - 1] == '\n') > 62) continue;
            size_t L = strlen(ln); int nl = L && ln[L - 1] == '\n';
            char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0;
            char *end; long lx = strtol(eq + 1, &end, 10);
            int num = end != eq + 1 && (*end == '\n' || *end == '\r' || !*end) && lx >= INT_MIN && lx <= INT_MAX, x = num ? (int)lx : -1;
            int rec = !strncmp(ln, "rec_", 4), known = 0;
            for (int i = 0; i < NV; i++) if (!strcmp(rec ? ln + 4 : ln, k_keys[i])) { known = 1; if (x >= 0 && x < kn(i)) { if (rec) r[i] = x; else v[i] = x; } }
            if (!strcmp(ln, "rec_v")) { known = 1; if (num) rv_file = x; }   // junk = absent: version 1, worked out again
            else if (!strcmp(ln, "rec_id")) { known = 1; if (x >= 0) id_file = x; }   // junk = absent
            *eq = '=';
            if (!known && xn + L + !nl < sizeof g_extra) { memcpy(g_extra + xn, ln, L); xn += L; if (!nl) g_extra[xn++] = '\n'; g_extra[xn] = 0; }
        }
        free(ln); if (f) fclose(f);
        if (ro) LOG("【红】picture settings: cannot read %s (%s): this launch uses the recommendation and leaves the file as it is", p, strerror(err));
        int newer = rv_file > REC_V;   // written by a newer version: filled in memory, never rewritten at load, and its rec_v is kept by a later Apply
        atomic_store(&g_rv_keep, newer ? rv_file : REC_V);
        int miss = 0, first = 1; for (int i = 0; i < NV; i++) { miss |= (v[i] < 0) << i | (r[i] < 0) << (i + NV); first &= v[i] < 0 && r[i] < 0; }   // first = not a single key read (a real first boot)
        // Worked out again (a "bump") when the recommendation is from an older version, or from another machine: rec_id names the GPU and the main screen it
        // was worked out for, and Native already follows the current screen, so the stored 3D Resolution and shadows must too. A newer file keeps its own rec_id
        // (an older build must not stamp this machine's id onto values it did not work out)
        int id_now = front_video_rec_id(), moved = !first && !newer && !ro && id_now >= 0 && id_file != id_now;
        atomic_store(&g_rec_id, newer ? id_file : id_now >= 0 ? id_now : id_file);
        int bump = !first && (rv_file < REC_V || moved), s3d_missing = v[K_S3D] < 0, fg_missing = v[K_FG] < 0, kept = 0;
        if (miss || bump) {   // work out the recommendation (no front end = the original: windowed, 1600, Standard), filling only the missing keys
            int rec[NV] = { 0 }, cur[NV] = { 0 };   // the front end sets the first three together (front_video_firstboot), the later keys one by one (front_video_rec; current = recommended)
            if (!front_video_firstboot(rec, cur)) memcpy(cur, rec, 3 * sizeof cur[0]);
            for (int i = 3; i < NV; i++) { int x = front_video_rec(k_keys[i]); rec[i] = cur[i] = x >= 0 && x < kn(i) ? x : 0; }
            if (bump) {   // still on the old recommendation (every graphics key read equals it): move to the new one; changed settings stay, only the recommendation changes
                int on_rec = 1; for (int i = 0; i < NV; i++) if (!pref_key(i) && v[i] >= 0 && r[i] >= 0 && v[i] != r[i]) on_rec = 0;   // judged by the graphics keys
                kept = !on_rec;
                for (int i = 0; i < NV; i++) { int nr = rec[i] >= 0 && rec[i] < kn(i) ? rec[i] : 0, was = v[i] >= 0 && r[i] >= 0 && v[i] == r[i];
                    if (v[i] >= 0 && (pref_key(i) ? was : on_rec)) v[i] = nr; r[i] = nr; }   // a display preference moves only while it equals its old recommendation
                if (rv_file < REC_V) LOG("picture settings: recommendation version %d → %d; %s", rv_file, REC_V, on_rec ? "moved to the new Recommended" : "changed settings kept");
                else LOG("picture settings: worked out again for this screen and GPU (rec_id %d → %d); %s", id_file, id_now, on_rec ? "moved to the new Recommended" : "changed settings kept");
            }
            for (int i = 0; i < NV; i++) { int rv = rec[i] >= 0 && rec[i] < kn(i) ? rec[i] : 0, had_r = r[i] >= 0;
                if (r[i] < 0) r[i] = rv;
                if (v[i] < 0) v[i] = had_r ? r[i] : cur[i] >= 0 && cur[i] < kn(i) ? cur[i] : r[i]; }   // current value missing: back to the stored recommendation; only when both are missing, the first-boot current value (incl. older preferences)
            if (!first && s3d_missing && v[K_RES] != 3) v[K_S3D] = 0;   // 3D Resolution only goes with Native output: older settings not on Native get 100% (the old look)
            if (kept && fg_missing) v[K_FG] = 0;   // a player's own settings keep the frames they had
        }
        if (v[K_FG] == 1 && v[K_UP] != 2) v[K_FG] = 0;   // it needs the temporal scaler (older preferences can give MetalFX here, or a hand-edited file)
        for (int i = 0; i < NV; i++) { atomic_store(&g_val[i], v[i]); atomic_store(&g_rec[i], r[i]); }
        atomic_store(&g_loaded, 1);
        if (miss && !first) { char mk[128] = ""; for (int i = 0; i < 2 * NV; i++) if (miss >> i & 1) snprintf(mk + strlen(mk), sizeof mk - strlen(mk), "%s%s%s", mk[0] ? " " : "", i >= NV ? "rec_" : "", k_keys[i % NV]);
            LOG("画面设置：%s 缺 / 越界的键 %s → 只补这几个（其余照读）", p, mk); }
        { char rs[160]; strlcpy(rs, vals_str(r), sizeof rs); LOG("画面设置：%s %s %s，推荐 %s", first ? "首次启动定推荐值" : miss ? "读盘（补过缺的键）" : "读盘", p, vals_str(v), rs); }
        if ((miss || bump) && !ro && !newer) save_locked();   // write the completed / updated file back (not over a file this build could not read, or a newer one)
        picture_ev(ro ? "unreadable" : first ? "first" : newer ? "newer" : rv_file < REC_V ? "update" : moved ? "rescreen" : miss ? "filled" : "read");
        if (v[K_COLOR] == 1) front_video_color(1);   // Accurate is stored: hand it to the front end for the present layer (the front end may not know the settings directory when it creates the layer, see front_ios.m)
    }
    pthread_mutex_unlock(&g_mu);
}
int dm_video_get(const char *key) {   // the value in effect; the original value while the settings directory is unknown
    load_once(); for (int i = 0; i < NV; i++) if (key && !strcmp(key, k_keys[i])) return atomic_load(&g_loaded) ? atomic_load(&g_val[i]) : 0;
    return 0;
}
int dm_video_upscale(void) { return dm_video_get("up"); }
int dm_video_aa_mode(void) { return dm_video_get("aa") == 1 ? 2 : 1; }   // the game renderer's AA mode byte (read every tick by aa_tick in hle_oe.c): 1 FXAA (original) / 2 SMAA 1x
int dm_video_aniso(void) { static const int k[4] = { 0, 4, 8, 16 }; int a = dm_video_get("aniso"); return a >= 0 && a < 4 ? k[a] : 0; }   // read at the start of each submission (nvn_mtl.m aniso_latch); 0 = the original
float dm_video_scale3d(void) {   // the 3D resolution (read every tick by s3_cfg in hle_oe.c; only > 0 counts): phones from res: 50 / 67 / 75 / 85 / 100 % (stored 4 / 3 / 0 / 1 / 2), the Mac from s3d (0 = 100% answers 0, as before)
    static const float k[5] = { 0.75f, 0.85f, 1.0f, 0.67f, 0.5f }, m[5] = { 0, 0.85f, 0.75f, 0.67f, 0.5f };
    load_once(); if (!atomic_load(&g_loaded)) return 0;
    if (DM_PHONE) { int t = atomic_load(&g_val[K_RES]); return t >= 0 && t < 5 ? k[t] : 0; }
    int t = atomic_load(&g_val[K_S3D]); return t > 0 && t < 5 ? m[t] : 0;
}
static _Atomic int g_esh_launch = -1;   // environment shadows as latched at launch (what the game's threads got); -1 = not latched yet
static _Atomic int g_esh_forced, g_shq_forced;   // 1 = DM_ENV_SHADOWS / DM_SHADOW_SIZE fixed the value: the Options page's choice does not take effect, so no restart hint
int dm_video_env_shadows(void) {   // every guest thread at its start (thread.c, KESHADOW slot): the first answer once the settings are known holds for the session
    int x = atomic_load(&g_esh_launch); if (x >= 0) return x;
    const char *e = getenv("DM_ENV_SHADOWS");   // DM_ENV_SHADOWS=0/1 forces it (tests, A/B runs)
    if (e && *e) { x = atoi(e) != 0; atomic_store(&g_esh_forced, 1); }
    else { load_once(); if (!atomic_load(&g_loaded)) return 0; x = atomic_load(&g_val[K_ESH]) == 1; }
    int exp = -1; if (atomic_compare_exchange_strong(&g_esh_launch, &exp, x)) LOG("Environment shadows: %s (%s)", x ? "on" : "off", e && *e ? "DM_ENV_SHADOWS" : "picture setting");
    return atomic_load(&g_esh_launch);
}
static const int k_shq_px[3] = { 2100, 3072, 4096 };   // Standard = the Switch's level-1 edge; High / Ultra = the game's own edges at levels 2 and 3
static _Atomic int g_shq_launch = -1;   // the shadow map edge as latched at launch (what the game's threads got); -1 = not latched yet
// Upscaling = MetalFX Temporal, at every 3D Resolution (at 100% the 3D target is 99% of the output: hle_oe.c scale3d_tick, so that the game's upscale pass exists for it to replace);
// DM_MFXT=0/1 forces it. Read by the replay thread each submission (nvn_mv.inc), by the threads that record commands (the KJITTER slot: thread.c dm_jitter_refresh), by aa_tick and scale3d_tick
__attribute__((weak)) int mtl_mfx_temporal_supported(void) { return 0; }
int dm_video_temporal_active(void) {
    static int env = -2; if (env == -2) { const char *e = getenv("DM_MFXT"); env = e && *e ? atoi(e) > 0 : -1; }
    if (!env) return 0;
    return env > 0 || (dm_video_get("up") == 2 && mtl_mfx_temporal_supported());
}
// Frame Generation = MetalFX's frame interpolator, live. Its motion, depth and camera offset come from the temporal scaler, so it needs that; the power saver's rate is left to the
// graphics layer (nvn_fg.inc fg_on). DM_FG=0/1 forces it. Read by the replay thread at each compose draw
__attribute__((weak)) int mtl_mfx_fg_supported(void) { return 0; }
int dm_video_fg_active(void) {
    static int env = -2; if (env == -2) { const char *e = getenv("DM_FG"); env = e && *e ? atoi(e) > 0 : -1; }
    if (!env || !dm_video_temporal_active()) return 0;
    return env > 0 || (dm_video_get("fg") == 1 && mtl_mfx_fg_supported());
}
int dm_video_jitter_mode(void) { return dm_video_temporal_active() ? 3 : 0; }   // 3 = the game's Halton mode (patch_nso.py SWITCHES, the csel rows)
int dm_video_shadow_size(void) {   // every guest thread at its start (thread.c, KSHSIZE slot): the shadow init reads it once, so the first answer holds for the session
    int x = atomic_load(&g_shq_launch); if (x > 0) return x;
    // DM_SHADOW_SIZE=<px> forces the edge (tests, A/B runs), clamped to 512..4096 and made even: 4096 is the game's own Ultra edge, and 8192 took
    // 256 MiB of GPU heap 0
    const char *e = getenv("DM_SHADOW_SIZE"); int raw = 0;
    if (e && *e) { raw = atoi(e); x = (raw < 512 ? 512 : raw > 4096 ? 4096 : raw) & ~1; atomic_store(&g_shq_forced, 1); }
    else { load_once(); if (!atomic_load(&g_loaded)) return k_shq_px[0]; int t = atomic_load(&g_val[K_SHQ]); x = k_shq_px[t >= 0 && t < 3 ? t : 0]; }
    int exp = -1; if (atomic_compare_exchange_strong(&g_shq_launch, &exp, x)) {
        if (!(e && *e)) LOG("Shadow map: %d px (picture setting)", x);
        else if (x != raw) LOG("Shadow map: %d px (DM_SHADOW_SIZE, clamped from %d)", x, raw);
        else LOG("Shadow map: %d px (DM_SHADOW_SIZE)", x); }
    return atomic_load(&g_shq_launch);
}
void dm_video_fullscreen_note(int on) {   // the player switched full screen from the menu / ⌃⌘F: the setting follows the window (as the PC Display Mode)
    load_once(); if (!atomic_load(&g_loaded) || atomic_load(&g_val[K_MODE]) == (on != 0)) return;
    pthread_mutex_lock(&g_mu); atomic_store(&g_val[K_MODE], on != 0); save_locked(); pthread_mutex_unlock(&g_mu);
    atomic_store(&g_repaint, 1);
}
static int shown(int k) { int p = atomic_load(&g_pend[k]); return p >= 0 ? p : atomic_load(&g_val[k]); }
static int perf_up(void) { return DM_PHONE ? 0 : front_video_mfx_ok() ? 1 : 0; }   // Performance's upscaling: phone = Standard (saves power), Mac = MetalFX (cheap there)
// Each key's value in the Performance / Quality presets (t 0 = Performance, 1 = Quality); -1 = the preset leaves this key alone. Anti-Aliasing and
// Anisotropic Filtering follow the same tiers on both platforms (the phone page has both rows too: touch_layout.py _VIDEO, iOS list): Performance =
// FXAA and leaves anisotropy alone, Quality = SMAA and 16x. On phones the presets leave Environment Shadows and Shadow Quality alone until their phone
// cost is measured, and the phone Recommended already gives SMAA and 16x (front_ios.m front_video_rec)
static int tier_of(int k, int t) {
    switch (k) {
    case K_RES: return t ? (DM_PHONE ? 2 : 3) : 0;   // Quality: 100% 3D on phones, Native output on the Mac
    case K_UP: return t ? (front_video_mfx_ok() ? 1 : 0) : perf_up();
    case K_AA: return t;   // Performance FXAA (SMAA was about 18% of the phone's GPU work before e070b94), Quality SMAA
    case K_ANISO: return t ? 3 : -1;
    case K_S3D: return DM_PHONE ? -1 : 0;   // Mac: both Performance (1600 output) and Quality (Native) draw the 3D at 100%
    case K_ESH: return DM_PHONE ? -1 : t;   // Mac: Performance = the Switch look, Quality = environment shadows; phone presets leave it alone (cost not measured there yet)
    case K_SHQ: return DM_PHONE ? -1 : t ? 2 : 0;   // Mac: Performance = Standard (the Switch's map), Quality = Ultra; phone presets leave it alone
    case K_FG: return 0;   // neither preset uses the temporal scaler it needs
    default: return -1;
    }
}
// The preset the player last applied (kept in memory for the session only) and the one picked on the page but not applied yet; -1 = none.
// While the values still match it, the row keeps showing the player's pick: on a Mac whose Recommended equals Quality, picking Quality used to snap back to Recommended
static _Atomic int g_pick = -1, g_pick_pend = -1;
static int matches(int p) {   // the values shown match preset p: Recommended over every key but the display preferences, Performance / Quality over the keys tier_of lists
    for (int k = 0; k < NV; k++) {
        int x = p == P_REC ? (pref_key(k) ? -1 : atomic_load(&g_rec[k])) : tier_of(k, p == P_QUAL);
        if (x >= 0 && shown(k) != x) return 0;
    }
    return 1;
}
static int preset_of(void) {   // the Graphic Presets row: the player's pick while it matches, else the first match of Recommended, Performance, Quality; else Custom
    int p = atomic_load(&g_pick_pend); if (p < 0) p = atomic_load(&g_pick);
    if (p >= 0 && p != P_CUSTOM && matches(p)) return p;
    for (int q = P_REC; q < P_CUSTOM; q++) if (matches(q)) return q;
    return P_CUSTOM;
}
static void apply_preset(int p) {   // a preset was picked: set the pending values (Recommended: every key but the display preferences back to the recommendation; Performance / Quality: the keys tier_of lists)
    if (p == P_REC) { for (int i = 0; i < NV; i++) if (!pref_key(i)) atomic_store(&g_pend[i], atomic_load(&g_rec[i])); }
    else if (p == P_PERF || p == P_QUAL) for (int k = 0; k < NV; k++) { int x = tier_of(k, p == P_QUAL); if (x >= 0) atomic_store(&g_pend[k], x); }
    if (p >= 0 && p != P_CUSTOM) atomic_store(&g_pick_pend, p);
}
int dm_video_dirty(void) { int d = 0; for (int i = 0; i < NV; i++) { int p = atomic_load(&g_pend[i]); d |= p >= 0 && p != atomic_load(&g_val[i]); } return d; }
void dm_video_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (!cat || !act || strcmp(cat, "OptionsPanelMessage")) return;
    load_once(); if (!atomic_load(&g_loaded)) return;
    if (!strcmp(act, "AcceptChanges")) { int ch = 0, mode = -1, color = -1;
        for (int i = 0; i < NV; i++) { int p = atomic_exchange(&g_pend[i], -1); if (p >= 0 && p != atomic_load(&g_val[i])) { atomic_store(&g_val[i], p); ch = 1; if (i == K_MODE) mode = p; if (i == K_COLOR) color = p; } }
        if (ch) { pthread_mutex_lock(&g_mu); save_locked(); pthread_mutex_unlock(&g_mu); picture_ev("apply"); }
        { int pk = atomic_exchange(&g_pick_pend, -1); if (pk >= 0) atomic_store(&g_pick, pk); }   // the picked preset is now the applied one
        if (mode >= 0) front_video_fullscreen(mode);   // Display Mode switches at once
        if (color >= 0) front_video_color(color);   // Color changes at once (anti-aliasing and anisotropy follow from where they read the setting, next frame)
        atomic_store(&g_repaint, 1); }
    else if (!strcmp(act, "CancelChanges")) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1); atomic_store(&g_pick_pend, -1); atomic_store(&g_repaint, 1); }
    // Default covers only the current tab's section (on the Extras tab it leaves the picture settings alone, as each original tab covers its own)
    else if (!strcmp(act, "ResetToDefaults") && (dm_custom_shown() & 1)) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], atomic_load(&g_rec[i]));   // every key, the display preferences included
        atomic_store(&g_pick_pend, P_REC);
        for (int i = 0; i < NR; i++) if (k_row_ckey[i] && (atomic_load(&g_crows) >> i & 1)) dm_custom_set_pending(k_row_ckey[i], dm_custom_default(k_row_ckey[i]));
        atomic_store(&g_repaint, 1); LOG("画面设置：默认 → 推荐值、帧率与掉帧时回缺省（点「应用」生效）"); }
}

// ---- Selectors on the page
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
DM_WIDGET_RED_DEF
static uint64_t row_w(uint64_t sp, int i) { char nm[48]; snprintf(nm, sizeof nm, "TouchVideo_%s", k_rows[i]); return dm_custom_find_w(sp, nm); }
static int car_ok(uint64_t w, int n) {
    if (!w || !*(uint8_t *)(w + 0x50) || !DM_WIDGET_OK(main_base(), w, DM_VT_CAROUSEL, "画面设置")) return 0;
    if (*(int32_t *)(w + 0x7e8) != n) { static int k; if (k++ < 3) LOG("【红】画面设置：切换框「%s」选项个数 %d，应为 %d，不读写", *(const char **)(w + 8), *(int32_t *)(w + 0x7e8), n); return 0; }
    return 1;
}
static int car_get(uint64_t w) { return *(int32_t *)(w + 0x7ec); }
static void car_set(uint64_t w, int i) { if (car_get(w) != i) ((void (*)(uint64_t, int))(main_base() + 0xab0c84))(w, i); }
static void preset_enable(uint64_t w, int custom) { *(uint8_t *)(w + 0x7d8 + P_CUSTOM) = custom != 0; }   // Custom can only be chosen while the row shows it (no preset matches; the arrows skip it)
static int present_rows(uint64_t sp, int idx[NR]) {   // the rows the page really has, in page order: thirteen on the Mac; eleven on phones (no Display Mode, and 3D Resolution is the res row, with no separate s3d row)
    int n = 0; for (int i = 0; i < NR && sp; i++) { uint64_t w = row_w(sp, i); if (w && *(uint8_t *)(w + 0x50)) idx[n++] = i; } return n;
}
int dm_video_rows(uint64_t sp) { int idx[NR]; return present_rows(sp, idx); }
void dm_video_row(uint64_t sp, int i, uint64_t *tw, uint64_t *cw) {   // controller selection frame: label and selector of the i-th picture row
    int idx[NR], n = present_rows(sp, idx); *tw = *cw = 0; if (i < 0 || i >= n) return;
    char nm[48]; snprintf(nm, sizeof nm, "TouchVideo_%s_Text", k_rows[idx[i]]); *tw = dm_custom_find_w(sp, nm); *cw = row_w(sp, idx[i]);
}
void dm_video_pad(uint64_t sp, int r, int dir) {   // controller: left / right = previous / next option, A = next (wrapping to the first); dm_video_pump then handles it as a player change
    int idx[NR], nr = present_rows(sp, idx); if (r < 0 || r >= nr) return; int i = idx[r];
    uint64_t w = row_w(sp, i); if (!car_ok(w, row_n(i))) return;
    int n = row_n(i), cur = car_get(w), to = cur;
    for (int k = 0; k < n; k++) { to = dir == 2 ? (to + 1) % n : to + dir; if (to < 0 || to >= n) { to = cur; break; } if (*(uint8_t *)(w + 0x7d8 + to)) break; }
    car_set(w, to);
}
static void hint_at(uint64_t sp, const char *hint, const char *label, int show) {   // show: move the hint under the row's label (x of the label); otherwise off screen
    uint64_t hw = dm_custom_find_w(sp, hint), lw = dm_custom_find_w(sp, label); if (!hw || !lw) return;
    int x = show ? ((int32_t *)(lw + 0x70))[0] : 99999; int32_t *r = (int32_t *)(hw + 0x70);
    if (r[0] != x) { r[0] = x; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)hw + 0x10))(hw); }
}
static void hint(uint64_t sp) {   // a change waiting for a restart: "Takes effect after restarting the game" under the row (resolution, environment shadows, shadow quality); also under Resolution when the window's screen no longer suits the launch picture
    int run = front_video_running_res(), esh = atomic_load(&g_esh_launch);
    hint_at(sp, "TouchVideoResHint", "TouchVideo_res_Text", run >= 0 && (shown(K_RES) != run || front_video_screen_stale()));
    hint_at(sp, "TouchVideoEnvshHint", "TouchVideo_envsh_Text", esh >= 0 && !atomic_load(&g_esh_forced) && shown(K_ESH) != esh);   // a forced value never follows the page: no hint
    int shq = atomic_load(&g_shq_launch), sq = shown(K_SHQ);
    hint_at(sp, "TouchVideoShqHint", "TouchVideo_shq_Text", shq > 0 && !atomic_load(&g_shq_forced) && sq >= 0 && sq < 3 && k_shq_px[sq] != shq);
}
// The phone's 3D Resolution row lists 50 / 67 / 75 / 85 / 100 % while its stored values keep their meaning (0 = 75, 1 = 85, 2 = 100, 3 = 67, 4 = 50):
// car_of turns a row's value into the selector index, val_of back; every other row is the identity
static int car_of(int row, int v) { static const int m[5] = { 2, 3, 4, 1, 0 }; return DM_PHONE && k_row_key[row] == K_RES && v >= 0 && v < 5 ? m[v] : v; }
static int val_of(int row, int c) { static const int m[5] = { 4, 3, 0, 1, 2 }; return DM_PHONE && k_row_key[row] == K_RES && c >= 0 && c < 5 ? m[c] : c; }
// The Frame Rate row's labels for a screen of hz: the normal rate (the smallest even fraction of the refresh at or above 60, dm_pace_hz) and the power
// saver's (dm_eco_hz), each as a whole number when within 0.1 of one, else with one decimal, put into fmt (dmVideoFpsRate, "%s fps"). Returns 0 when
// both read 60 / 30 (60 and 120 Hz screens, NTSC 59.94, no window): the layout's own options (dmVideoFps60 / dmVideoFps30) stay
static void fps_num(char *b, size_t n, double hz) { if (fabs(hz - round(hz)) < 0.1) snprintf(b, n, "%ld", lround(hz)); else snprintf(b, n, "%.1f", hz); }
static int fps_labels(double screen_hz, const char *fmt, char *hi, char *lo, size_t n) {
    char a[16], b[16]; fps_num(a, sizeof a, dm_pace_hz(screen_hz, 60)); fps_num(b, sizeof b, dm_eco_hz(screen_hz));
    if (!strcmp(a, "60") && !strcmp(b, "30")) return 0;
    if (!fmt || !strstr(fmt, "%s")) fmt = "%s fps";   // the format must carry its one placeholder (dm_i18n.py checks every language)
    snprintf(hi, n, fmt, a); snprintf(lo, n, fmt, b); return 1;
}
// Writes the Frame Rate selector's two options (our own TouchVideo_fps, a CarouselWidget whose vtable and option count car_ok checked): the labels for the
// screen, or back to the layout's own keys, then has the game's setter show the current option again. Only on a change: the strings are compared first
static void fps_row(uint64_t w) {
    static const char *const k_own[2] = { "dmVideoFps60", "dmVideoFps30" };   // the layout's options (touch_layout.py _VIDEO, row fps)
    const char *nm = *(const char **)(w + 8); if (!nm || strcmp(nm, "TouchVideo_fps")) return;
    char hi[48], lo[48]; const char *want[2] = { k_own[0], k_own[1] };
    double hz = front_video_screen_hz();
    if (fps_labels(hz, dm_str(main_base(), "dmVideoFpsRate"), hi, lo, sizeof hi)) { want[0] = hi; want[1] = lo; }
    int ch = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t st = w + 0x558 + 0x28 * (uint64_t)i; const char *cur = *(const char **)st; uint64_t len = *(uint64_t *)(st + 8); size_t n = strlen(want[i]);
        if (cur && len == n && !memcmp(cur, want[i], n)) continue;
        ((void *(*)(uint64_t, const char *, uint64_t))(main_base() + 0x136e5c))(st, want[i], n); ch = 1;
    }
    if (!ch) return;
    ((void (*)(uint64_t, int))(main_base() + 0xab0c84))(w, car_get(w));   // show the current option with its new text
    LOG("picture settings: Frame Rate row labelled %s / %s (screen %.2f Hz)", want[0], want[1], hz);
}
// Frame Generation takes its inputs from the temporal scaler and has no room at the power saver's rate. Turning it on selects Temporal and the normal rate; leaving Temporal turns it
// off. The power saver only overrides it (nvn_fg.inc fg_on), so the row reads Off meanwhile and the setting comes back with the normal rate. The presets have their own value (tier_of)
static void fg_rules(int row, int v) {
    int k = k_row_key[row];
    if (k == K_FG && v == 1) { atomic_store(&g_pend[K_UP], 2); if (dm_custom_shown_value("eco_fps")) dm_custom_set_pending("eco_fps", 0); }
    else if (k == K_UP && v != 2 && shown(K_FG)) atomic_store(&g_pend[K_FG], 0);
}
static void fg_enable(uint64_t w) { *(uint8_t *)(w + 0x7d8 + 1) = mtl_mfx_fg_supported() != 0; }   // a chip without the interpolator can't choose On (the arrows skip it)
static int row_want(int i) { if (k_row_key[i] == K_FG && dm_custom_shown_value("eco_fps")) return 0; return k_row_ckey[i] ? dm_custom_shown_value(k_row_ckey[i]) : k_row_key[i] < 0 ? preset_of() : shown(k_row_key[i]); }   // the option this row should show
void dm_video_pump(uint64_t sp) {   // called every frame while an Options page of ours is on screen (dm_custom.c); sp = 0: the page is not on screen
    static dm_wref seen[NR];
    uint64_t w[NR] = { 0 }; int ok = sp != 0 && dm_video_rows(sp) > 0;
    for (int i = 0; ok && i < NR; i++) { w[i] = row_w(sp, i); if (w[i] && !*(uint8_t *)(w[i] + 0x50)) w[i] = 0; }   // rows the page lacks = 0, skipped below
    if (!ok) { int was = 0; for (int i = 0; i < NR; i++) was |= seen[i].w != 0;   // the page is gone: changes without Apply are dropped (as on the custom page)
        if (was) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1); atomic_store(&g_pick_pend, -1); for (int i = 0; i < NR; i++) if (k_row_ckey[i]) dm_custom_set_pending(k_row_ckey[i], -1); }
        memset(seen, 0, sizeof seen); atomic_store(&g_crows, 0); return; }
    for (int i = 0; i < NR; i++) if (w[i] && !car_ok(w[i], row_n(i))) return;
    { int cr = 0; for (int i = 0; i < NR; i++) if (w[i] && k_row_ckey[i]) cr |= 1 << i; atomic_store(&g_crows, cr); }
    load_once(); if (!atomic_load(&g_loaded)) return;
    if (atomic_exchange(&g_repaint, 0)) memset(seen, 0, sizeof seen);
    int want[NR], ch = -1;   // first work out what each row should show from the values before any change, then find the row the player moved (one per frame): comparing while changing used to let the later rows read a preset's fresh pending values as the player changing them back
    for (int i = 0; i < NR; i++) want[i] = car_of(i, row_want(i));
    for (int i = 0; i < NR; i++) {
        if (!w[i]) continue;
        if (dm_wref_get(&seen[i]) != w[i]) { dm_wref_set(&seen[i], w[i]); if (k_row_key[i] == -1) preset_enable(w[i], want[i] == P_CUSTOM); if (k_row_key[i] == K_FG) fg_enable(w[i]); car_set(w[i], want[i]); continue; }   // the page just appeared: show the value it should, and do not treat this frame as a player change
        if (ch < 0 && car_get(w[i]) != want[i]) ch = i;
    }
    if (ch >= 0) {
        int k = k_row_key[ch], cur = val_of(ch, car_get(w[ch]));
        if (k_row_ckey[ch]) { dm_custom_set_pending(k_row_ckey[ch], cur); LOG("画面设置：%s → %d（自定义设置里的键；点「应用」生效）", k_row_ckey[ch], cur); }
        else if (k < 0) { if (cur != P_CUSTOM) apply_preset(cur); LOG("画面设置：图形预设 → %d（点「应用」生效）", cur); }
        else { atomic_store(&g_pend[k], cur); LOG("画面设置：%s → %d（点「应用」生效）", k_keys[k], cur); }
        fg_rules(ch, cur);
        for (int i = 0; i < NR; i++) { if (!w[i]) continue; int v = row_want(i); if (k_row_key[i] == -1) preset_enable(w[i], v == P_CUSTOM); car_set(w[i], car_of(i, v)); }   // presets move the other rows, and the other rows move the preset
        DM_EV("video_row", "row:s value:d preset:d", k_rows[ch], cur, preset_of());   // the player's change and what the Graphic Presets row shows after it
    }
    for (int i = 0; i < NR; i++) if (w[i] && !strcmp(k_rows[i], "fps")) fps_row(w[i]);   // the Frame Rate row names this screen's rates
    hint(sp);
}
