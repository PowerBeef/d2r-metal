// The Mac App's Recommended picture for made-up machines (dm_picrec.c): each real configuration's chip name, GPU cores and Native screen size,
// and the 3D Resolution, shadows and Frame Drops it should get. Expectations follow the measured costs and the per-generation speeds in
// dm_picrec.c; when those change, so do these. Also checks dm_picrec_real, the gate that keeps the CLI host and test copies on the original picture.
// Pass -v to print every machine's choice.
#include <stdio.h>
#include <string.h>
#include "../dm_picrec.h"

typedef struct { const char *what, *gpu; int cores, w, h, mfx; int native, tier_res, s3d, envsh, shq, q3d; } want;   // tier_res counts only without Native
static const int k_pct[5] = { 100, 85, 75, 67, 50 };

int main(int argc, char **argv) {
    int verbose = argc > 1 && !strcmp(argv[1], "-v"), fails = 0, n = 0;
    static const want k[] = {
#include "t_picrec_cases.h"
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++, n++) {
        dm_mac_spec s = { k[i].gpu, k[i].cores, 16ull << 30, k[i].w, k[i].h, k[i].mfx }; dm_mac_rec r; dm_mac_recommend(&s, &r);
        int ok = r.native == k[i].native && (r.native || r.tier_res == k[i].tier_res) && r.s3d == k[i].s3d && r.envsh == k[i].envsh && r.shq == k[i].shq && r.q3d == k[i].q3d;
        if (!ok || verbose)
            printf("%s %-34s %-18s %2d cores %4dx%-4d speed %.2f → 3D %3d%% envsh %d shq %d q3d %d (%.0f%%)%s\n", ok ? "  " : "✗", k[i].what, k[i].gpu, k[i].cores,
                   k[i].w, k[i].h, r.speed, k_pct[r.s3d], r.envsh, r.shq, r.q3d, r.load * 100, ok ? "" : "  ← expected otherwise");
        fails += !ok;
    }
    // the chip as read: generation and tier from the name
    dm_mac_spec s = { "Apple M4 Max", 40, 0, 3456, 2234, 1 }; dm_mac_rec r; dm_mac_recommend(&s, &r);
    n++; if (r.gen != 4 || r.tier != 2) { fails++; printf("✗ Apple M4 Max read as generation %d tier %d\n", r.gen, r.tier); }
    s.gpu = "Apple A18 Pro GPU"; dm_mac_recommend(&s, &r);
    n++; if (r.gen != 18 || r.tier != -1) { fails++; printf("✗ Apple A18 Pro GPU read as generation %d tier %d\n", r.gen, r.tier); }
    // who gets the real recommendation: the Mac App (DM_APP), and a test copy (DM_APP_SUPPORT) only with DM_TEST_VIDEO_FIRSTBOOT=1; never the CLI host
    for (int bits = 0; bits < 8; bits++) {
        int app = bits >> 2 & 1, sup = bits >> 1 & 1, fb = bits & 1, want_real = app && (!sup || fb), got = dm_picrec_real(app, sup, fb);
        n++; if (got != want_real) { fails++; printf("✗ dm_picrec_real(app %d, support %d, firstboot %d) = %d, expected %d\n", app, sup, fb, got, want_real); }
    }
    printf("t_picrec: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
