// The Mac App's Recommended picture (dm_picrec.c), checked two ways.
// Expected rows: what each machine gets from the model (t_picrec_cases.h; the M6 rows are the measured ones and must stay within the capacity).
// Invariants: properties the model must keep whatever its constants, over a grid of chips, GPU core counts, screens and MetalFX support.
// Also checked: the chip as read from its name, per-core speeds and core-count fallbacks, dm_picrec_real (the gate that keeps the CLI host
// and test copies on the original picture) and dm_picrec_id (the rec_id dm_video.c stores with the recommendation). Pass -v to print every machine's choice.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../dm_picrec.h"

typedef struct { const char *what, *gpu; int cores, w, h, mfx; int native, tier_res, s3d, envsh, shq, q3d; } want;   // tier_res counts only without Native
static const int k_pct[5] = { 100, 85, 75, 67, 50 };
static const double k_scale[5] = { 1.0, 0.85, 0.75, 0.67, 0.5 };
static const int k_fixed_w[3] = { 1600, 1920, 2560 };
static int n, fails;

static void rec_of(const char *gpu, int cores, int w, int h, int mfx, dm_mac_rec *r) {
    dm_mac_spec s = { .gpu = gpu, .cores = cores, .out_w = w, .out_h = h, .mfx = mfx };
    dm_mac_recommend(&s, r);
}

// one check; a failure prints its line (the invariants print only the first 5 of each kind)
__attribute__((format(printf, 3, 4))) static void check(int ok, int *shown, const char *fmt, ...) {
    n++; if (ok) return;
    fails++;
    if (shown && (*shown)++ >= 5) return;
    va_list ap; va_start(ap, fmt); printf("✗ "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}

// Frame Drops levels the 3D scale controller keeps below a chosen 3D scale. Today this copies hle_oe.c's rule (s3_max_level: the table's 0.05 steps
// below the chosen scale, down to S3_FLOOR_DEF 0.60). When the controller's levels follow the chosen scale (dm_3d_scale_levels), call that here
// instead, so invariant I3 checks the levels the game really has.
static int drop_levels(double scale) {
    static const double k_lv[] = { 0.95, 0.90, 0.85, 0.80, 0.75, 0.70, 0.65, 0.60, 0.55, 0.50 };
    const double floor = 0.60; int m = 0;
    for (size_t i = 0; i < sizeof k_lv / sizeof *k_lv; i++) m += k_lv[i] < scale - 1e-4 && k_lv[i] >= floor - 1e-4;
    return m;
}
#define MIN_DROP_LEVELS 2   // Frame Drops = lower must have somewhere to go: dm_picrec.c LIGHT_MIN is chosen so that it does

// The grid: every M generation the model knows and one past it, in each tier; the iPhone chips' Pro parts; core counts from below the smallest
// real part to above the largest; screens from the 1600 floor to 16:10 4K, sorted by area (then width).
static const int k_grid_cores[] = { 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 19, 20, 24, 30, 32, 38, 40, 48, 60, 64, 76, 80 };
static const int k_grid_scr[][2] = { { 1600, 900 }, { 2400, 1500 }, { 2560, 1440 }, { 2560, 1600 }, { 2560, 1664 }, { 2880, 1800 }, { 2928, 1904 },
                                     { 3024, 1964 }, { 3408, 2216 }, { 3456, 2234 }, { 3840, 2160 }, { 3840, 2400 } };
#define NCORES ((int)(sizeof k_grid_cores / sizeof *k_grid_cores))
#define NSCR ((int)(sizeof k_grid_scr / sizeof *k_grid_scr))

static void grid(void) {
    static const char *const k_tiers[4] = { "", " Pro", " Max", " Ultra" };
    char chips[64][32]; int nchips = 0;
    for (int g = 1; g <= 7; g++) for (int t = 0; t < 4; t++) snprintf(chips[nchips++], sizeof chips[0], "Apple M%d%s", g, k_tiers[t]);
    for (int a = 14; a <= 21; a++) snprintf(chips[nchips++], sizeof chips[0], "Apple A%d Pro GPU", a);
    int shown[10] = { 0 };
    static dm_mac_rec r[NCORES][NSCR];
    for (int c = 0; c < nchips; c++)
        for (int mfx = 0; mfx < 2; mfx++) {
            for (int k = 0; k < NCORES; k++)
                for (int s = 0; s < NSCR; s++) {
                    const dm_mac_rec *x = &r[k][s]; int cores = k_grid_cores[k], w = k_grid_scr[s][0], h = k_grid_scr[s][1];
                    rec_of(chips[c], cores, w, h, mfx, &r[k][s]);
#define AT " (%s, %d cores, %dx%d, mfx %d)"
#define AT_ARGS chips[c], cores, w, h, mfx
                    // I1: a GPU at least as fast as the slowest real chip (the 7-core M1, speed 0.273) always gets a picture within its capacity
                    check(x->speed < 0.27 || x->load <= 1.0, &shown[1], "I1 load %.3f at speed %.3f" AT, x->load, x->speed, AT_ARGS);
                    // I2: Frame Drops lowers the 3D exactly when the light picture was needed or the choice is close to the capacity
                    check(x->q3d == !(!x->envsh || x->load > 0.85), &shown[2], "I2 q3d %d with envsh %d, load %.3f" AT, x->q3d, x->envsh, x->load, AT_ARGS);
                    // I3: wherever Frame Drops = lower is recommended, the controller has levels below the chosen 3D scale to lower to
                    check(x->q3d || drop_levels(k_scale[x->s3d]) >= MIN_DROP_LEVELS, &shown[3], "I3 Frame Drops lower at 3D %d%% with %d levels below" AT,
                          k_pct[x->s3d], drop_levels(k_scale[x->s3d]), AT_ARGS);
                    // I6: an iPhone chip keeps the phone's shadows (environment shadows off, the Standard map)
                    check(x->tier != -1 || (!x->envsh && !x->shq), &shown[6], "I6 iPhone chip with envsh %d shq %d" AT, x->envsh, x->shq, AT_ARGS);
                    // I8: Native never goes down to a 50% 3D (the fixed resolution takes over before that)
                    check(!x->native || x->s3d != 4, &shown[8], "I8 Native at 3D 50%%" AT, AT_ARGS);
                    // I9: Shadow Quality is Standard or Ultra, and Ultra exactly with the environment shadows
                    check((x->shq == 0 || x->shq == 2) && (x->shq == 2) == (x->envsh != 0), &shown[9], "I9 shq %d with envsh %d" AT, x->shq, x->envsh, AT_ARGS);
#undef AT
#undef AT_ARGS
                }
            // I4: for one chip, screen and MetalFX, more GPU cores never give a lesser recommendation
            for (int k = 1; k < NCORES; k++)
                for (int s = 0; s < NSCR; s++) {
                    const dm_mac_rec *a = &r[k - 1][s], *b = &r[k][s];
#define AT " (%s, %d → %d cores, %dx%d, mfx %d)"
#define AT_ARGS chips[c], k_grid_cores[k - 1], k_grid_cores[k], k_grid_scr[s][0], k_grid_scr[s][1], mfx
                    check(!a->native || b->native, &shown[4], "I4 Native lost" AT, AT_ARGS);
                    check(!(a->native && b->native) || k_pct[b->s3d] >= k_pct[a->s3d], &shown[4], "I4 3D %d%% → %d%%" AT, k_pct[a->s3d], k_pct[b->s3d], AT_ARGS);
                    check(a->native != b->native || !a->envsh || b->envsh, &shown[4], "I4 environment shadows lost" AT, AT_ARGS);
                    check(a->native || b->native || b->tier_res >= a->tier_res, &shown[4], "I4 fixed %d → %d" AT, k_fixed_w[a->tier_res], k_fixed_w[b->tier_res], AT_ARGS);
                    check(b->speed >= a->speed, &shown[4], "I4 speed %.3f → %.3f" AT, a->speed, b->speed, AT_ARGS);
#undef AT
#undef AT_ARGS
                }
            // I5: for one chip, core count and MetalFX, a screen at least as large both ways never gives a heavier choice
            for (int k = 0; k < NCORES; k++)
                for (int s = 1; s < NSCR; s++)
                    for (int p = 0; p < s; p++) {
                        if (k_grid_scr[s][0] < k_grid_scr[p][0] || k_grid_scr[s][1] < k_grid_scr[p][1]) continue;
                        const dm_mac_rec *a = &r[k][p], *b = &r[k][s];
#define AT " (%s, %d cores, %dx%d → %dx%d, mfx %d)"
#define AT_ARGS chips[c], k_grid_cores[k], k_grid_scr[p][0], k_grid_scr[p][1], k_grid_scr[s][0], k_grid_scr[s][1], mfx
                        check(a->native || !b->native, &shown[5], "I5 fixed turned Native" AT, AT_ARGS);
                        check(!(a->native && b->native) || k_pct[b->s3d] <= k_pct[a->s3d], &shown[5], "I5 3D %d%% → %d%%" AT, k_pct[a->s3d], k_pct[b->s3d], AT_ARGS);
                        check(!(a->native && b->native) || a->envsh || !b->envsh, &shown[5], "I5 environment shadows turned on" AT, AT_ARGS);
#undef AT
#undef AT_ARGS
                    }
        }
}

int main(int argc, char **argv) {
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    static const want k[] = {
#include "t_picrec_cases.h"
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        dm_mac_rec r; rec_of(k[i].gpu, k[i].cores, k[i].w, k[i].h, k[i].mfx, &r);
        int ok = r.native == k[i].native && (r.native || r.tier_res == k[i].tier_res) && r.s3d == k[i].s3d && r.envsh == k[i].envsh && r.shq == k[i].shq && r.q3d == k[i].q3d;
        char out[16]; if (r.native) snprintf(out, sizeof out, "Native"); else snprintf(out, sizeof out, "fixed %d", k_fixed_w[r.tier_res]);
        if (!ok || verbose)
            printf("%s %-50s %-24s %2d cores %4dx%-4d speed %.2f → %-10s 3D %3d%% envsh %d shq %d q3d %d (%.0f%%)%s\n", ok ? "  " : "✗", k[i].what, k[i].gpu,
                   k[i].cores, k[i].w, k[i].h, r.speed, out, k_pct[r.s3d], r.envsh, r.shq, r.q3d, r.load * 100, ok ? "" : "  ← expected otherwise");
        n++; fails += !ok;
        // the measured machines: the model must keep what they really held at 60 fps within its capacity
        if (strstr(k[i].what, "(measured)")) check(r.load <= 1.0, NULL, "%s: load %.3f above the capacity", k[i].what, r.load);
    }
    // the chip as read: generation and tier from the name
    struct { const char *gpu; int cores, gen, tier; double speed; } names[] = {
        { "Apple M4 Max", 40, 4, 2, 40 * 5300 / 102000.0 },
        { "Apple A18 Pro GPU", 5, 18, -1, 5 * 6257 / 102000.0 },
        { "Apple A18 GPU", 5, 18, -1, 5 * 6257 / 102000.0 },     // every A18 takes the Neo's measurement, not the A-to-M table's M4 slot
        { "Apple A19 Pro GPU", 6, 19, -1, 0.41176 },   // the A19 is an M5's GPU design
        { "Apple A15 GPU", 5, 15, -1, 0.22059 },       // the A15 is an M2's
        { "Apple A21 Pro GPU", 6, 21, -1, 0.5 },       // newer than the table: counted as an M6
        { "Apple M2", 0, 2, 0, 0.30882 },              // no core count: the smallest part of the tier (7 cores)
        { "Apple M1 Max", 0, 1, 2, 0.93529 },          // 24 cores
        { "Apple M1 Ultra", 0, 1, 3, 1.87059 },        // 48 cores
        { "Apple M3 Pro", 0, 3, 1, 0.64373 },          // 14 cores
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        dm_mac_rec r; rec_of(names[i].gpu, names[i].cores, 3456, 2234, 1, &r);
        check(r.gen == names[i].gen && r.tier == names[i].tier, NULL, "%s read as generation %d tier %d", names[i].gpu, r.gen, r.tier);
        check(fabs(r.speed - names[i].speed) < 1e-4, NULL, "%s with %d cores: speed %.5f, expected %.5f", names[i].gpu, names[i].cores, r.speed, names[i].speed);
    }
    // who gets the real recommendation: the Mac App (DM_APP), and a test copy (DM_APP_SUPPORT) only with DM_TEST_VIDEO_FIRSTBOOT=1; never the CLI host
    for (int bits = 0; bits < 8; bits++) {
        int app = bits >> 2 & 1, sup = bits >> 1 & 1, fb = bits & 1, want_real = app && (!sup || fb);
        check(dm_picrec_real(app, sup, fb) == want_real, NULL, "dm_picrec_real(app %d, support %d, firstboot %d) = %d, expected %d", app, sup, fb,
              dm_picrec_real(app, sup, fb), want_real);
    }
    // rec_id (dm_picrec_id): the machine the recommendation was worked out for; dm_video.c works it out again when it changes. 32-bit FNV-1a over the GPU
    // name, then cores, out_w, out_h and mfx as 4 little-endian bytes each, top bit cleared
    struct { dm_mac_spec s; int id; } ids[] = {
        { { "Apple M6", 12, 2560, 1440, 1 }, 1043353910 },
        { { "Apple M6", 12, 3840, 2160, 1 }, 926858990 },
        { { "Apple M1", 8, 2880, 1800, 1 }, 1049100262 },
        { { NULL, 0, 0, 0, 0 }, 1768495365 },   // no name hashes as ""
        { { "", 0, 0, 0, 0 }, 1768495365 },
    };
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) { int id = dm_picrec_id(&ids[i].s);
        check(id == ids[i].id && id >= 0, NULL, "dm_picrec_id(%s, %d, %dx%d, mfx %d) = %d, expected %d", ids[i].s.gpu ? ids[i].s.gpu : "NULL", ids[i].s.cores, ids[i].s.out_w, ids[i].s.out_h, ids[i].s.mfx, id, ids[i].id); }
    { dm_mac_spec a = { "Apple M6", 12, 2560, 1440, 1 }, b = { "Apple M1", 8, 2560, 1440, 1 }, c = a, d = a; c.mfx = 0; d.out_w = 2544;
      check(dm_picrec_id(&a) != dm_picrec_id(&b), NULL, "rec_id: another GPU at the same screen gives the same id");
      check(dm_picrec_id(&a) != dm_picrec_id(&c) && dm_picrec_id(&a) != dm_picrec_id(&d), NULL, "rec_id: MetalFX or the screen does not change the id"); }
    grid();
    printf("t_picrec: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
