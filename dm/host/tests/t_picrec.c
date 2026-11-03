// The Mac App's Recommended picture (dm_picrec.c) for made-up machines: what each machine gets from the model (t_picrec_cases.h; the M6 rows are
// the measured ones and must stay within the capacity). Also checked: the chip as read from its name, per-core speeds and core-count fallbacks, and
// dm_picrec_real (the gate that keeps the CLI host and test copies on the original picture). Pass -v to print every machine's choice.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../dm_picrec.h"

typedef struct { const char *what, *gpu; int cores, w, h, mfx; int native, tier_res, s3d, envsh, shq, q3d; } want;   // tier_res counts only without Native
static const int k_pct[5] = { 100, 85, 75, 67, 50 };
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
    printf("t_picrec: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
