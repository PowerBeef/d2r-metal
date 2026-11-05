// The Mac App's Recommended picture (dm_picrec.c), checked two ways.
// Expected rows: what each machine gets from the model (t_picrec_cases.h; the M6 rows are the measured ones and must stay within the capacity).
// Invariants: properties the model must keep whatever its constants, over a grid of chips, GPU core counts, screens and MetalFX support, and over
// frame rates (50 to 100 fps) and drawables larger than the Native cap (4.5K to 6K).
// Also checked: the chip as read from its name, per-core speeds and core-count fallbacks, dm_picrec_real (the gate that keeps the CLI host
// and test copies on the original picture) and dm_picrec_id (the rec_id dm_video.c stores with the recommendation). Pass -v to print every machine's choice.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../dm_picrec.h"

typedef struct { const char *what, *gpu; int cores, w, h, mfx; int native, tier_res, s3d, envsh, shq, q3d; int dw, dh; double hz; } want;   // tier_res counts only without Native;
// dw x dh: the present's viewport (0 = the picture's size); hz: the frame rate on that screen (0 = 60)
// rec_id of the M6 at Native 3840 x 2160 with MetalFX into a 5K viewport, and at 72 fps: the formula in dm_picrec.h, worked out by hand
#define ID_M6_5K 566077671
#define ID_M6_72 978830642
static const int k_pct[5] = { 100, 85, 75, 67, 50 };
static const double k_scale[5] = { 1.0, 0.85, 0.75, 0.67, 0.5 };
static const int k_fixed_w[3] = { 1600, 1920, 2560 };
static int n, fails;

// The cases give the in-game picture at Native (front_mac.m native_picture); a picture narrower than 16:9 is true full screen, whose aspect a fixed
// resolution takes too (spec.aspect), so it is derived from the picture's own shape here
static void rec_at(const char *gpu, int cores, int w, int h, int mfx, int dw, int dh, double hz, dm_mac_rec *r) {
    dm_mac_spec s = { .gpu = gpu, .cores = cores, .out_w = w, .out_h = h, .mfx = mfx, .aspect = (long)w * 9 < (long)h * 16 ? (double)w / h : 0, .drw_w = dw, .drw_h = dh, .pace_hz = hz };
    dm_mac_recommend(&s, r);
}
static void rec_of(const char *gpu, int cores, int w, int h, int mfx, dm_mac_rec *r) { rec_at(gpu, cores, w, h, mfx, 0, 0, 0, r); }

// one check; a failure prints its line (the invariants print only the first 5 of each kind)
__attribute__((format(printf, 3, 4))) static void check(int ok, int *shown, const char *fmt, ...) {
    n++; if (ok) return;
    fails++;
    if (shown && (*shown)++ >= 5) return;
    va_list ap; va_start(ap, fmt); printf("✗ "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}

// Frame Drops levels the 3D scale controller keeps below a chosen 3D scale: the controller's own rule (dm_s3lv.h dm_3d_scale_levels, which hle_oe.c
// uses), so invariant I3 checks the levels the game really has
#include "dm_s3lv.h"
static int drop_levels(double scale) { return dm_3d_scale_levels((float)scale); }
#define MIN_DROP_LEVELS 2   // Frame Drops = lower must have somewhere to go: dm_picrec.c LIGHT_MIN is chosen so that it does

// The grid: every M generation the model knows and one past it, in each tier; the iPhone chips' Pro parts; core counts from below the smallest
// real part to above the largest; in-game pictures at Native (as front_mac.m native_picture feeds them) from the 1600 floor to the heap cap's 16:9 4K,
// 16:9 and 16:10 (below the notch) mixed, sorted by area (then width).
static const int k_grid_cores[] = { 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 19, 20, 24, 30, 32, 38, 40, 48, 60, 64, 76, 80 };
static const int k_grid_scr[][2] = { { 1600, 900 }, { 2400, 1500 }, { 2560, 1440 }, { 2560, 1600 }, { 2880, 1800 }, { 2940, 1838 }, { 3024, 1890 },
                                     { 3420, 2150 }, { 3456, 2160 }, { 3642, 2276 }, { 3840, 2160 } };
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
                    // I1: a GPU at least as fast as the slowest real chip (the 7-core M1, speed 0.273) always gets a picture within its capacity; a
                    // fixed resolution's scene fits by itself, and Frame Drops lowers the 3D when its enlargement into the screen takes it above
                    check(x->speed < 0.27 || x->load <= 1.0 || (!x->native && !x->q3d && x->load - x->pres <= 1.0), &shown[1], "I1 load %.3f (present %.3f) at speed %.3f" AT,
                          x->load, x->pres, x->speed, AT_ARGS);
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

// A choice at least as light as another: not Native where the other is fixed, no higher 3D Resolution with Native, no environment shadows the other
// lacks with the same output, and never a larger fixed width
static int lighter_or_same(const dm_mac_rec *a, const dm_mac_rec *b) {   // b is no heavier than a
    if (b->native && !a->native) return 0;
    if (a->native && b->native && k_pct[b->s3d] > k_pct[a->s3d]) return 0;
    if (a->native == b->native && b->envsh && !a->envsh) return 0;
    if (!a->native && !b->native && b->tier_res > a->tier_res) return 0;
    return 1;
}
// Frame rate and drawable over the same chips and core counts:
//   I10  a faster frame rate (50, 60, 72, 82.5, 100 fps) never gives a heavier choice, and at 60 the answer is the one without a rate (0)
//   I11  a larger viewport for the Native picture (3840 x 2160 into itself, 4480 x 2520, 5120 x 2880, 6016 x 3384) never gives a heavier choice, a
//        viewport no more than 5% larger counts as none, and the enlargement is counted (pres > 0) exactly when the picture is Native and enlarged
//   I1 and I2 hold there too (I1 at 60 fps: a faster screen can leave a slow GPU with nothing light enough)
static void grid_rate_drawable(void) {
    static const char *const k_tiers[4] = { "", " Pro", " Max", " Ultra" };
    static const double k_hz[] = { 50, 60, 72, 82.5, 100 };
    static const int k_drw[][2] = { { 3840, 2160 }, { 4000, 2250 }, { 4480, 2520 }, { 5120, 2880 }, { 6016, 3384 } };
    static const int k_scr[][2] = { { 2560, 1440 }, { 3024, 1890 }, { 3840, 2160 } };
    int shown[16] = { 0 };
    for (int g = 1; g <= 7; g++) for (int t = 0; t < 4; t++) for (int mfx = 0; mfx < 2; mfx++) for (int k = 0; k < (int)(sizeof k_grid_cores / sizeof *k_grid_cores); k++) {
        char chip[32]; snprintf(chip, sizeof chip, "Apple M%d%s", g, k_tiers[t]); int cores = k_grid_cores[k];
        for (int s = 0; s < 3; s++) {
            int w = k_scr[s][0], h = k_scr[s][1]; dm_mac_rec r0, r60, prev;
            rec_of(chip, cores, w, h, mfx, &r0); rec_at(chip, cores, w, h, mfx, 0, 0, 60, &r60);
            check(!memcmp(&r0, &r60, sizeof r0), &shown[10], "I10 60 fps differs from no rate (%s, %d cores, %dx%d, mfx %d)", chip, cores, w, h, mfx);
            for (int i = 0; i < (int)(sizeof k_hz / sizeof *k_hz); i++) {
                dm_mac_rec r; rec_at(chip, cores, w, h, mfx, 0, 0, k_hz[i], &r);
                check(r.q3d == !(!r.envsh || r.load > 0.85), &shown[2], "I2 q3d %d with envsh %d, load %.3f (%s, %d cores, %dx%d, mfx %d, %.1f fps)", r.q3d, r.envsh, r.load, chip, cores, w, h, mfx, k_hz[i]);
                if (i) check(lighter_or_same(&prev, &r), &shown[10], "I10 %.1f → %.1f fps gave a heavier choice: native %d → %d, 3D %d%% → %d%%, envsh %d → %d (%s, %d cores, %dx%d, mfx %d)",
                             k_hz[i - 1], k_hz[i], prev.native, r.native, k_pct[prev.s3d], k_pct[r.s3d], prev.envsh, r.envsh, chip, cores, w, h, mfx);
                prev = r;
            }
        }
        dm_mac_rec prev;
        for (int d = 0; d < (int)(sizeof k_drw / sizeof *k_drw); d++) {
            dm_mac_rec r; int dw = k_drw[d][0], dh = k_drw[d][1]; rec_at(chip, cores, 3840, 2160, mfx, dw, dh, 0, &r);
            dm_mac_spec sp = { .gpu = chip, .cores = cores, .out_w = 3840, .out_h = 2160, .mfx = mfx, .drw_w = dw, .drw_h = dh };
            int enl = dm_picrec_enlarges(&sp);
            check(enl == (d >= 2), &shown[11], "I11 %dx%d %s an enlargement of 3840x2160", dw, dh, enl ? "counts as" : "is not");
            check(r.native ? (r.pres > 0) == enl : r.pres > 0, &shown[11], "I11 pres %.3f with native %d into %dx%d (%s, %d cores, mfx %d)", r.pres, r.native, dw, dh, chip, cores, mfx);   // a fixed picture is always enlarged into these
            check(r.speed < 0.27 || r.load <= 1.0 || (!r.native && !r.q3d && r.load - r.pres <= 1.0), &shown[1], "I1 load %.3f (present %.3f) at speed %.3f into %dx%d (%s, %d cores, mfx %d)",
                  r.load, r.pres, r.speed, dw, dh, chip, cores, mfx);
            check(r.q3d == !(!r.envsh || r.load > 0.85), &shown[2], "I2 q3d %d with envsh %d, load %.3f into %dx%d (%s, %d cores, mfx %d)", r.q3d, r.envsh, r.load, dw, dh, chip, cores, mfx);
            if (d) check(lighter_or_same(&prev, &r), &shown[11], "I11 %dx%d → %dx%d gave a heavier choice: native %d → %d, 3D %d%% → %d%%, envsh %d → %d (%s, %d cores, mfx %d)",
                         k_drw[d - 1][0], k_drw[d - 1][1], dw, dh, prev.native, r.native, k_pct[prev.s3d], k_pct[r.s3d], prev.envsh, r.envsh, chip, cores, mfx);
            prev = r;
        }
    }
}

int main(int argc, char **argv) {
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    static const want k[] = {
#include "t_picrec_cases.h"
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        dm_mac_rec r; rec_at(k[i].gpu, k[i].cores, k[i].w, k[i].h, k[i].mfx, k[i].dw, k[i].dh, k[i].hz, &r);
        int ok = r.native == k[i].native && (r.native || r.tier_res == k[i].tier_res) && r.s3d == k[i].s3d && r.envsh == k[i].envsh && r.shq == k[i].shq && r.q3d == k[i].q3d;
        char out[16]; if (r.native) snprintf(out, sizeof out, "Native"); else snprintf(out, sizeof out, "fixed %d", k_fixed_w[r.tier_res]);
        if (!ok || verbose)
            printf("%s %-50s %-24s %2d cores %4dx%-4d speed %.2f → %-10s 3D %3d%% envsh %d shq %d q3d %d (%.0f%%%s)%s\n", ok ? "  " : "✗", k[i].what, k[i].gpu,
                   k[i].cores, k[i].w, k[i].h, r.speed, out, k_pct[r.s3d], r.envsh, r.shq, r.q3d, r.load * 100, r.pres > 0 ? ", present enlargement counted" : "", ok ? "" : "  ← expected otherwise");
        n++; fails += !ok;
        // the measured machines: the model must keep what they really held at 60 fps within its capacity
        if (strstr(k[i].what, "(measured)")) check(r.load <= 1.0, NULL, "%s: load %.3f above the capacity", k[i].what, r.load);
    }
    // Frame Generation: where the system offers it and the full picture fits, at half the drawn rate with what it costs counted; else the conventional choice stands, unchanged.
    // Anchored on what the M6 measured at full clock: 11.8 ms a drawn frame at 2560 x 1440, 23.9 at 3840 x 2160, out of 33.3
    struct { const char *what, *gpu; int cores, w, h, fg, s3d; } fgs[] = {
        { "M6, 2560 display (measured)", "Apple M6", 12, 2560, 1440, 1, 0 },
        { "M6 on a 4K display (measured)", "Apple M6", 12, 3840, 2160, 1, 0 },
        { "MacBook Air 13 M5, default", "Apple M5", 10, 2940, 1838, 1, 0 },
        { "M5 on a 4K display: real frames", "Apple M5", 10, 3840, 2160, 0, 0 },
        { "Mac mini M4, 2560 display", "Apple M4", 10, 2560, 1440, 1, 0 },
        { "MacBook Air 13 M4, default: 85%", "Apple M4", 10, 2940, 1838, 1, 1 },
        { "MacBook Air 13 M4 (8-core), default: real frames", "Apple M4", 8, 2940, 1838, 0, 0 },
        { "Mac mini M4 Pro (16-core) on a 4K display: 85%", "Apple M4 Pro", 16, 3840, 2160, 1, 1 },
        { "MacBook Pro 14 M4 Pro", "Apple M4 Pro", 20, 3024, 1890, 1, 0 },
        { "MacBook Air 13 M3 (8-core), default: real frames", "Apple M3", 8, 2940, 1838, 0, 0 },
        { "MacBook Pro 14 M3 Pro", "Apple M3 Pro", 14, 3024, 1890, 1, 0 },
    };
    for (size_t i = 0; i < sizeof fgs / sizeof *fgs; i++) {
        dm_mac_spec sp = { .gpu = fgs[i].gpu, .cores = fgs[i].cores, .out_w = fgs[i].w, .out_h = fgs[i].h, .mfx = 1, .fg = 1 }; dm_mac_rec r, c; dm_mac_recommend(&sp, &r);
        sp.fg = 0; dm_mac_recommend(&sp, &c);
        check(r.fg == fgs[i].fg, NULL, "%s: Frame Generation %d, expected %d (load %.2f)", fgs[i].what, r.fg, fgs[i].fg, r.load);
        check(!c.fg, NULL, "%s: Frame Generation where the system doesn't offer it", fgs[i].what);
        if (r.fg) { check(r.native && r.s3d == fgs[i].s3d && r.envsh == 1 && r.shq == 2 && r.load <= 1, NULL, "%s: Native, 3D index %d (got %d), the full picture, within capacity; native %d envsh %d shq %d load %.2f", fgs[i].what, fgs[i].s3d, r.s3d, r.native, r.envsh, r.shq, r.load); }
        else check(!memcmp(&r, &c, sizeof r), NULL, "%s: without room for it the conventional choice stands", fgs[i].what);
        if (strstr(fgs[i].what, "(measured)")) check(r.fg && r.load <= 1.0, NULL, "%s: what the M6 really held must fit", fgs[i].what);
    }
    { dm_mac_spec a = { .gpu = "Apple M6", .cores = 12, .out_w = 2560, .out_h = 1440, .mfx = 1 }, b = a; b.fg = 1; check(dm_picrec_id(&a) != dm_picrec_id(&b), NULL, "rec_id: the offer of Frame Generation counts as an input"); }
    { dm_mac_spec sp = { .gpu = "Apple M6", .cores = 12, .out_w = 2560, .out_h = 1440, .mfx = 0, .fg = 1 }; dm_mac_rec r; dm_mac_recommend(&sp, &r); check(!r.fg, NULL, "no MetalFX: no Frame Generation"); }
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
    // The drawable and the frame rate count only where they change the model's input: a viewport of the picture's own size (or no more than 5%
    // larger) and 60 fps keep a plain machine's id; a 5K viewport, a 4.5K one or 72 fps give another, and a 4K display moved from its 1920 x 1080
    // to its 2560 x 1440 mode (the same 3840 Native, a 5120 x 2880 drawable) is worked out again
    { dm_mac_spec a = { "Apple M6", 12, 3840, 2160, 1 }, same = a, near = a, k5 = a, k45 = a, r60 = a, r0 = a, r72 = a, both = a;
      same.drw_w = 3840; same.drw_h = 2160; near.drw_w = 4000; near.drw_h = 2250; k5.drw_w = 5120; k5.drw_h = 2880; k45.drw_w = 4480; k45.drw_h = 2520;
      r60.pace_hz = 60; r0.pace_hz = 0; r72.pace_hz = 72; both = k5; both.pace_hz = 72;
      int id = dm_picrec_id(&a);
      check(id == 926858990 && dm_picrec_id(&same) == id && dm_picrec_id(&near) == id && dm_picrec_id(&r60) == id && dm_picrec_id(&r0) == id, NULL,
            "rec_id: a viewport the picture's size (%d), 5%% larger (%d) or 60 fps (%d) changes the plain id %d", dm_picrec_id(&same), dm_picrec_id(&near), dm_picrec_id(&r60), id);
      int i5 = dm_picrec_id(&k5), i45 = dm_picrec_id(&k45), i72 = dm_picrec_id(&r72), ib = dm_picrec_id(&both);
      check(i5 != id && i45 != id && i72 != id && ib != id && i5 != i45 && i5 != ib && i72 != ib && i5 >= 0 && i45 >= 0 && i72 >= 0 && ib >= 0, NULL,
            "rec_id: 5K %d, 4.5K %d, 72 fps %d, 5K at 72 fps %d should differ from each other and from %d", i5, i45, i72, ib, id);
      check(i5 == ID_M6_5K && i72 == ID_M6_72, NULL, "rec_id formula: 5K viewport %d (expected %d), 72 fps %d (expected %d)", i5, ID_M6_5K, i72, ID_M6_72); }
    // dm_picrec_enlarges: the present's own rule, more than 5% larger both ways
    { struct { int w, h, dw, dh, want; } e[] = { { 3840, 2160, 5120, 2880, 1 }, { 3840, 2160, 4480, 2520, 1 }, { 3840, 2160, 4032, 2268, 0 }, { 3840, 2160, 5120, 2160, 0 },
                                                 { 2560, 1440, 2560, 1440, 0 }, { 3840, 2160, 0, 0, 0 }, { 3840, 1620, 5120, 2160, 1 } };
      for (size_t i = 0; i < sizeof e / sizeof *e; i++) { dm_mac_spec sp = { "Apple M6", 12, e[i].w, e[i].h, 1, 0, e[i].dw, e[i].dh, 0 };
          check(dm_picrec_enlarges(&sp) == e[i].want, NULL, "dm_picrec_enlarges(%dx%d into %dx%d) = %d, expected %d", e[i].w, e[i].h, e[i].dw, e[i].dh, dm_picrec_enlarges(&sp), e[i].want); } }
    grid();
    grid_rate_drawable();
    printf("t_picrec: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
