// The Mac App's Recommended picture, from this Mac's chip: its family and generation (from the GPU's name), its GPU core count, the main screen.
// 1. Speed: GPU cores × the per-core speed of that generation, against the M6 (12 cores) the model is anchored on. Per-core speeds are Geekbench 6
//    Metal points per core of base chips (public results, 2025–2026): M1 31.8k / 8 cores, M2 45k / 10, M3 46.9k / 10, M4 53k / 10, M5 70k / 10,
//    M6 102k / 12. iPhone chips: the A18 Pro as measured in the MacBook Neo (31.3k / 5, about an M1's); the others as the M generation of their
//    year. A newer generation than the table knows counts as the newest one known (it is at least that fast).
// 2. Load of a picture: 3D megapixels + half the output megapixels (the interface, the MetalFX enlargement and the final compose run at the
//    output size, lighter per pixel). The light picture (no environment shadows, Standard shadows) carries 15% more: at Native 2560 x 1440, 67%,
//    the M6 took 6.4 ms of GPU time against 7.4 with the full one (bench.py town route, p50).
// 3. Capacity: 14 load units × speed. Two anchors: the M6 holds 60 fps with no slow frame at Native 3840 x 2160, 3D 100%, full picture (12.4
//    units; GPU p50 12.4 ms, p95 14.1 on the town route, 2026-11-03), and an M1-class base chip keeps the Recommended every base chip had before
//    (full picture, 3D 75% of a 2560-wide screen: 4.35 units at speed 0.31). The GPU's own time could not give costs directly: it lowers its
//    clock when it has room, so halving the 3D resolution at 2560 x 1440 only took the M6 from 8.4 to 6.6 ms.
// 4. Choice: the full picture at the highest 3D Resolution down to 75% that fits, else the light one down to 50%; when even that does not fit at
//    Native, a fixed resolution (the highest of 1600 / 1920 / 2560 up to 3/4 of the screen's width) enlarged by MetalFX. Frame Drops lowers the 3D
//    resolution for a while (as on the phone) when the light picture was needed or the choice uses more than 85% of the capacity.
// No MetalFX: a fixed resolution, the picture by its load.
#include "dm_picrec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double k_core_m[] = { 0, 3975, 4500, 4690, 5300, 7000, 8500 };   // index = M generation
#define M_KNOWN 6
#define REF_SCORE (12 * 8500.0)   // the M6 the model is anchored on: 12 GPU cores
#define CAPACITY 14.0             // load units at speed 1.0
#define OUT_WEIGHT 0.5            // an output pixel against a 3D pixel
#define LIGHT_GAIN 1.15           // the light picture carries this much more
#define CLOSE 0.85                // above this share of the capacity, Frame Drops lowers the 3D for a while
static const float k_scale[5] = { 1.0f, 0.85f, 0.75f, 0.67f, 0.5f };   // the 3D Resolution row
static const int k_fixed_w[3] = { 1600, 1920, 2560 };                   // the Resolution row's fixed widths

static double core_speed(int iphone, int gen) {
    if (iphone) {
        if (gen == 18) return 6257;
        gen -= 13;   // A14 = M1 … A20 = M6
    }
    if (gen < 1) return k_core_m[1];
    return k_core_m[gen > M_KNOWN ? M_KNOWN : gen];
}

int dm_picrec_real(int app, int app_support, int test_firstboot) { return app && (!app_support || test_firstboot); }

// load of a picture: 3D at `scale` of an output w x h
static double load(int w, int h, double scale) { double out = (double)w * h / 1e6; return out * scale * scale + OUT_WEIGHT * out; }

void dm_mac_recommend(const dm_mac_spec *s, dm_mac_rec *r) {
    memset(r, 0, sizeof *r);
    const char *g = s->gpu ? s->gpu : "";
    int iphone = !strncmp(g, "Apple A", 7), mchip = !strncmp(g, "Apple M", 7);
    r->gen = iphone || mchip ? atoi(g + 7) : 0;
    r->tier = iphone ? -1 : !mchip ? 0 : strstr(g, " Ultra") ? 3 : strstr(g, " Max") ? 2 : strstr(g, " Pro") ? 1 : 0;
    static const int k_min_cores[4] = { 8, 14, 30, 60 };   // the smallest of each tier, when the IORegistry gives no count
    int cores = s->cores > 0 ? s->cores : iphone ? 5 : k_min_cores[r->tier];
    r->speed = cores * core_speed(iphone, r->gen) / REF_SCORE;
    double cap = CAPACITY * r->speed;
    if (s->mfx)   // Native: the full picture down to 75%, else the light one down to 50%
        for (int light = 0; light < 2; light++)
            for (int i = light ? 0 : 0; i < (light ? 5 : 3); i++) {
                double l = load(s->out_w, s->out_h, k_scale[i]) / (light ? LIGHT_GAIN : 1);
                if (l <= cap) { r->native = 1; r->s3d = i; r->envsh = !light; r->shq = light ? 0 : 2; r->load = l / cap; r->q3d = !(light || r->load > CLOSE); return; }
            }
    // a fixed resolution: the highest width up to 3/4 of the screen (the 2560 one from half the M6's speed), the 3D at 100%, the picture by its load
    for (int i = 0; i < 3; i++) if (k_fixed_w[i] <= (s->out_w * 3 / 4 > 1600 ? s->out_w * 3 / 4 : 1600) && (i < 2 || r->speed >= 0.5)) r->tier_res = i;
    int w = k_fixed_w[r->tier_res], h = w * 9 / 16;
    double full = load(w, h, 1), light = full / LIGHT_GAIN;
    r->native = 0; r->s3d = 0; r->envsh = full <= cap; r->shq = r->envsh ? 2 : 0;
    r->load = (r->envsh ? full : light) / cap; r->q3d = !(!r->envsh || r->load > CLOSE);
}
