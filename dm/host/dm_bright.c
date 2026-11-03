// Dynamic exposure for the 3D picture brightening (the snowy summit at noon is overexposed after brightening; only dark places need it). The graphics layer's
// nvn_mtl.m bright_lut calls dm_bright_k() once per LUT dispatch for the exposure k (1–4).
// Rules, in order:
//   (1) Out of game (dm_uiscreen_ingame = 0 and no loading screen: the front-end pages) → k = 1; during a loading screen k holds (entering or leaving a game and
//       walking into a cave do not drop back to 1 first);
//   off: the Extras switch "Brighten dark areas" (custom key bright, dm_custom.c; on by default, live) is off → k = 1, the game's own exposure;
//   (2) maze-type levels (Levels.txt DrlgType = 1: caves, tombs, crypts, sewers...) → k = 4;
//   (3) biomes whose picture does not follow day and night (the day and night versions in hd/env/vis are identical), where the game gives no fixed ambient
//       brightness → k = 4;
//   (4) the rest follow the ambient brightness I (0–255) the game computes itself: k = 4^((255 − I) / 191), clamped to 1–4 (noon I = 255 → 1, midnight I = 64 → 4).
// "The game gives a fixed ambient brightness" follows the branches of the game's ambient-light update (main+0x55a140): Act IV (act index 3; 128 / 64 / 56 / 48 by
// its table, 16 for the rest), areas 120 (Arreat Summit) and 137 pinned at 200, environment flag +0x34 set (converges to 32).
// The game's environment: client current act = [main+0x2d20bd0], environment = [act + 8]; +0x8 float accumulated time, +0xc int ambient brightness,
// +0x2c int rate (sun angle = time ÷ rate, 0–360°), +0x34 int flags. DM_TEST_DAYTIME writes the time periods the way the game's period advance
// (main+0x55a9d0) reads them, from its normal period table (main+0x1dab370).
// The level table comes from the game's own data (dm/tools/gen_bright_levels.py: dm_bright_levels.inc on iOS, bright_levels.tsv beside the Mac host, dm_tables.h).
// k is smoothed in log space with a 0.33 s time constant (about 1 s to settle, about 1.9 s from 4 down to 1 when the switch is turned off), so map changes,
// entering or leaving a game and the switch never jump.
// Test switches: DM_BRIGHT_K=<k> pins the exposure, whatever the Extras switch says (1 = the game's own brightness, for the screenshot gates);
// DM_TEST_DAYTIME=<angle>@<tick>: at tick <tick>, write that sun angle into every act's environment on the server and into the client's (to see noon / dusk /
// midnight; needs the tick-entry observation gate, attached by any of DM_C2S_LOG and the like). Writing only the client's copy gets synced back by the server.
#include "dm.h"
#include "hle.h"
#include "dm_hud.h"
#include "dm_uiscreen.h"
#include "dm_custom.h"
#include <limits.h>
#include <math.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#if TARGET_OS_IOS
static const struct { uint16_t lv; uint8_t act, drlg; int8_t varies; } k_lv[] = {
#include "dm_bright_levels.inc"
};
static int lv_count(void) { return (int)(sizeof k_lv / sizeof *k_lv); }
int dm_bright_level_rows(void) { return lv_count(); }
#else
#include "dm_tables.h"
#include <pthread.h>
static struct { uint16_t lv; uint8_t act, drlg; int8_t varies; } k_lv[512];
static int g_nlv = -2;   // rows read from bright_levels.tsv: -2 not read yet, -1 missing, 0 empty
static char g_lv_path[PATH_MAX];
static pthread_once_t g_lv_once = PTHREAD_ONCE_INIT;
static void lv_load(void) {   // the level table from bright_levels.tsv (dm_tables.h); without it every level is treated as one whose picture follows day and night
    static long v[512 * 4]; int n = dm_table_read("bright_levels.tsv", 4, v, 512, g_lv_path);
    g_nlv = n < 0 ? -1 : n;
    if (n <= 0) { LOG("Tables: %s %s: the indoor brightening rule is off", g_lv_path, n < 0 ? "is missing" : "is empty"); return; }
    for (int i = 0; i < n; i++) { k_lv[i].lv = (uint16_t)v[4 * i]; k_lv[i].act = (uint8_t)v[4 * i + 1]; k_lv[i].drlg = (uint8_t)v[4 * i + 2]; k_lv[i].varies = (int8_t)v[4 * i + 3]; }
}
int dm_bright_level_rows(void) { pthread_once(&g_lv_once, lv_load); return g_nlv; }
static int lv_count(void) { int n = dm_bright_level_rows(); return n > 0 ? n : 0; }
#endif

// The two game-derived tables at a glance: "bright_levels.tsv N rows, hud_exp.tsv 100 rows (<folder>)", 'missing' / 'empty' / '(needs 100)' when not.
// Returns 1 when both are usable. The Mac host reads them at start (dm_tables.h); the iOS app compiles them in
int dm_tables_status(char *buf, size_t n) {
    int dm_hud_exp_rows(void);   // diag.c
    int b = dm_bright_level_rows(), h = dm_hud_exp_rows(); char bs[32], hs[48], where[PATH_MAX + 8];
    if (b < 0) snprintf(bs, sizeof bs, "missing"); else if (!b) snprintf(bs, sizeof bs, "empty"); else snprintf(bs, sizeof bs, "%d rows", b);
    if (h < 0) snprintf(hs, sizeof hs, "missing"); else if (!h) snprintf(hs, sizeof hs, "empty"); else snprintf(hs, sizeof hs, h == 100 ? "%d rows" : "%d rows (needs 100)", h);
#if TARGET_OS_IOS
    snprintf(where, sizeof where, " (built in)");
#else
    { char d[PATH_MAX]; strlcpy(d, g_lv_path, sizeof d); char *sl = strrchr(d, '/'); if (sl) *sl = 0; else strlcpy(d, ".", sizeof d); snprintf(where, sizeof where, " (%s)", d); }
#endif
    snprintf(buf, n, "bright_levels.tsv %s, hud_exp.tsv %s%s", bs, hs, where);
    return b > 0 && h == 100;
}
// One line at host start (main.c host_preload). Red only in the Mac App, where a missing table means a broken install; developer CLI hosts may run without them
void dm_tables_report(void) {
    int dm_hud_exp_rows(void);
    char s[PATH_MAX + 96]; int ok = dm_tables_status(s, sizeof s), b = dm_bright_level_rows(), h = dm_hud_exp_rows();
    LOG("%sTables: %s%s%s%s", !ok && getenv("DM_APP") ? "【红】" : "", s, b > 0 ? "" : ": dark-area brightening treats every level as one that follows day and night",
        h == 100 ? "" : b > 0 ? ": " : "; ", h == 100 ? "" : "the experience bar stays empty");
}

static int rd(uint64_t a, void *buf, size_t n) { vm_size_t got = 0; return a && !vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)buf, &got) && got == n; }
static uint64_t main_base(void) { static uint64_t b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; return b; }
static uint64_t env_ptr(void) { uint64_t act = 0, env = 0, mb = main_base(); return mb && rd(mb + 0x2d20bd0, &act, 8) && rd(act + 8, &env, 8) ? env : 0; }

// Logarithm of the target k (log k); *why receives the rule number, *I / *ang receive the game's values read (-1 if unreadable)
static double target_logk(int *why, int *area, int *I, double *ang) {
    *I = -1; *ang = -1; *area = -1;
    if (!dm_uiscreen_ingame()) { *why = 1; return 0; }
    dm_hud_info h; dm_hud_get(&h); *area = h.valid ? h.area : -1;
    int act = -1, drlg = -1, varies = -1;
    for (int i = 0, n = lv_count(); i < n; i++) if (k_lv[i].lv == *area) { act = k_lv[i].act; drlg = k_lv[i].drlg; varies = k_lv[i].varies; break; }
    uint64_t env = env_ptr(); int32_t iv = -1, flag = 0, rate = 0; float t = 0;
    if (env && rd(env + 0xc, &iv, 4) && rd(env + 0x34, &flag, 4) && rd(env + 0x2c, &rate, 4) && rd(env + 0x8, &t, 4)) { *I = iv; if (rate > 0) *ang = t / rate; }
    if (drlg == 1) { *why = 2; return log(4.0); }
    int pinned = act == 3 || *area == 120 || *area == 137 || flag != 0;
    if (varies == 0 && !pinned) { *why = 3; return log(4.0); }
    *why = 4;
    if (*I < 0) return log(4.0);   // the environment cannot be read (the first few frames of a game): full boost
    double e = (255.0 - (*I > 255 ? 255 : *I)) / 191.0; if (e < 0) e = 0; if (e > 1) e = 1;
    return e * log(4.0);
}

float dm_bright_k(void) {   // called once per LUT dispatch by the graphics layer (bright_lut) and per submit by the frame capture, both on the translation / replay thread; other threads read nvn_mtl.m's g_bright_applied
    static int fixed = -2; static float kf;
    if (fixed == -2) { const char *e = getenv("DM_BRIGHT_K"); fixed = e != NULL; if (e) { kf = (float)atof(e); LOG("DM_BRIGHT_K=%g: the 3D exposure is pinned by the test switch", kf); } }
    if (fixed) return kf;
    static double logk = 0; static uint64_t last; static int lwhy = -1, larea = -2; static uint64_t llog; static double ltgt = -1;
    uint64_t t = now_ns();
    // loading screen (entering or leaving a game, walking into a cave): hold the current k; once it is gone, move smoothly from it to the new target without dropping to 1 first
    { int dm_loadscreen_up(void); if (dm_loadscreen_up()) { last = t; static int tr = -1; static unsigned n; if (tr < 0) tr = getenv("DM_BRIGHT_TRACE") != NULL; if (tr && n++ % 6 == 0) LOG("3D brightening: k %.3f (loading screen, held)", exp(logk)); return (float)exp(logk); } }
    int why, area, I; double ang; double tgt = target_logk(&why, &area, &I, &ang);
    if (why != 1 && !dm_custom_get("bright")) { tgt = 0; why = 0; }   // the Extras switch, read only in game (never before the save is mounted); turning it off fades k to 1 with the same smoother
    double dt = last ? (t - last) / 1e9 : 0; last = t; if (dt > 0.034) dt = 0.034;   // not called during a loading screen (no LUT dispatch): advance by at most one frame of the 30 fps tier each call, so k starts from its old value after the screen goes instead of jumping
    logk += (tgt - logk) * (1 - exp(-dt / 0.33));
    if (why != lwhy || area != larea || fabs(tgt - ltgt) > 0.05 || t - llog > 10000000000ull) {   // the rule / area / target changed, or every 10 s: one line with the game's values and k
        LOG("3D brightening: target k %.2f (rule %s, area %d, ambient %d, sun %.0f°), current k %.2f", exp(tgt),
            why == 0 ? "off (Extras)" : why == 1 ? "1 out of game" : why == 2 ? "2 maze" : why == 3 ? "3 no day-night picture" : "4 ambient light", area, I, ang, exp(logk));
        lwhy = why; larea = area; llog = t; ltgt = tgt; }
    { static int tr = -1; static unsigned n; if (tr < 0) tr = getenv("DM_BRIGHT_TRACE") != NULL; if (tr && n++ % 6 == 0) LOG("3D brightening: k %.3f (target %.3f, rule %d)", exp(logk), exp(tgt), why); }   // diagnostics: one line every 6 calls
    return (float)exp(logk);
}

void dm_bright_server_tick(uint64_t G) {   // server per-tick entry (diag.c, while the tick-entry observation gate is attached): the test switch DM_TEST_DAYTIME
    static int ta = -2; static unsigned at;
    if (ta == -2) { const char *e = getenv("DM_TEST_DAYTIME"); ta = -1; if (e) { int a; unsigned t; if (sscanf(e, "%d@%u", &a, &t) == 2) { ta = a; at = t; } } }
    uint32_t tick = 0; if (ta < 0 || !rd(G + 0x170, &tick, 4) || tick < at) return;
    uint64_t acts = 0; int n = 0; rd(G + 0x190, &acts, 8);
    // The game's period advance (main+0x55a9d0) moves one period at a time, and once the time passes the next period's start it is pulled back to that start:
    // write both the period index (+0) and the period type (+4). The period table is the game's normal one (main+0x1dab370, 6 entries of start angle, type,
    // light color): the last entry whose start is at or below the target angle
    int idx = -1, kind = 0; uint64_t mb = main_base(); int32_t tbl[18];
    if (mb && rd(mb + 0x1dab370, tbl, sizeof tbl)) { int best = -1; for (int i = 0; i < 6; i++) if (tbl[3 * i] <= ta && tbl[3 * i] > best) { best = tbl[3 * i]; idx = i; kind = tbl[3 * i + 1]; } }
    uint64_t envs[6]; int ne = 0;
    for (int i = 0; acts && i < 5; i++) { uint64_t act = 0, env = 0; if (rd(acts + 8 * i, &act, 8) && rd(act + 8, &env, 8)) envs[ne++] = env; }
    uint64_t ce = env_ptr(); if (ce) envs[ne++] = ce;
    for (int i = 0; i < ne; i++) { int32_t rate = 0, fl = 0; if (!rd(envs[i] + 0x2c, &rate, 4) || rate <= 0 || !rd(envs[i] + 0x34, &fl, 4) || fl) continue;   // environments with the special flag set are left alone
        if (idx >= 0) { *(int32_t *)(uintptr_t)envs[i] = idx; *(int32_t *)(uintptr_t)(envs[i] + 4) = kind; }
        *(float *)(uintptr_t)(envs[i] + 8) = (float)(ta * rate); n++; }
    LOG("DM_TEST_DAYTIME：拍 %u 把 %d 份环境（服务端各幕 + 客户端%s）写成太阳角 %d°、时段 %d / 类型 %d", tick, n, ce ? "" : "，客户端没取到", ta, idx, kind);
    ta = -1;
}
