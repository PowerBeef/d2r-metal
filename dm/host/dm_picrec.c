// The Mac App's Recommended picture, from this Mac's chip: its family and generation (from the GPU's name), its GPU core count, the main screen.
// 1. Speed: GPU cores × the per-core speed of that generation, against the M6 (12 cores) the model is anchored on. Per-core speeds are Geekbench 6
//    Metal points per core of base chips (public results, 2025–2026): M1 31.8k / 8 cores, M2 45k / 10, M3 46.9k / 10, M4 53k / 10, M5 70k / 10,
//    M6 102k / 12. iPhone chips: every A18 as the A18 Pro measured in the MacBook Neo (31.3k / 5, about an M1's); the others as the M generation
//    with the same GPU design (A14 = M1, A15 and A16 = M2, A17 = M3, A19 = M5, A20 = M6). A newer generation than the table knows counts as the
//    newest one known (it is at least that fast). Without a core count, the smallest part of the tier.
// 2. Load of a picture: 3D megapixels + half the output megapixels (the interface, the MetalFX enlargement and the final compose run at the
//    output size, lighter per pixel). The light picture (no environment shadows, Standard shadows) carries 15% more: at Native 2560 x 1440, 67%,
//    the M6 took 6.4 ms of GPU time against 7.4 with the full one (bench.py town route, p50).
// 3. Capacity: 14 load units × speed, from the one measured machine: the M6 holds 60 fps at Native 3840 x 2160, 3D 100%, full picture (12.44
//    units; GPU p50 12.4 ms, p95 14.1 ms on the town route, 2026-11-03). Its p95 frame scales to about 14.7 units in 16.7 ms, and 14 leaves a
//    small margin. Nothing slower than the M6 has been measured: on every other Mac the line between the full and light picture, and between 3D
//    steps, is an estimate from public benchmark speeds, and t_picrec checks the model's shape (invariants), not real machines. The GPU's own
//    time could not give costs directly: it lowers its clock when it has room, so halving the 3D resolution at 2560 x 1440 only took the M6 from
//    8.4 to 6.6 ms.
// 4. Choice: the full picture at the highest 3D Resolution down to 75% that fits, else the light one down to 67% (LIGHT_MIN); when even that does
//    not fit at Native, a fixed resolution (the highest of 1600 / 1920 / 2560 up to 3/4 of the screen's width) enlarged by MetalFX, with the 3D at
//    100%. Frame Drops lowers the 3D resolution for a while (as on the phone) when the light picture was needed or the choice uses more than 85%
//    of the capacity. An iPhone chip (tier -1) keeps the phone's shadows (no environment shadows, the Standard map): their cost on an A-series
//    GPU is not measured, on the phone or the MacBook Neo.
// 5. The present's enlargement: past the Native cap (3840 x 2160, DM_SCR_MAX_PX) the present enlarges the picture into a larger viewport (a 4.5K iMac's
//    4480 x 2520, a 5K display's 5120 x 2880, front_mac.m mac_rec). It costs per viewport pixel, above the 1:1 present the output weight already
//    carries: with the 3D at 100% the present enlarges with MetalFX (no 3D MetalFX frame before it, nvn_mtl.m mfx_want), with the 3D lower it enlarges
//    with bilinear filtering after the 3D pass's MetalFX (one MetalFX pass on the world, MFX-2). PRES_MFX / PRES_BIL below are from the M6 (present GPU
//    p50 over 300 presents). A fixed resolution is enlarged into its viewport too (MetalFX, or bilinear filtering without it): that cost does not
//    change the picture the fixed path picks (the full one when its scene fits), but counts toward Frame Drops, so a slow Mac on a big screen (a
//    7-core M1 iMac at 4.5K, about 124% of its capacity) lowers its 3D for a while instead of dropping frames (decided 2026-11-03, wave 4).
// 6. Frame rate: the capacity is per 60 fps. A screen whose frame rate is another even fraction of its refresh (72 fps on 144 Hz, 100 on 100 Hz, 50 on
//    50 Hz; nvn_pace.h) scales every load by rate / 60, so a 144 Hz screen gets a correspondingly lighter recommendation.
// 7. Frame Generation: where the system offers it and the full picture still fits at Native, down to 75%, what the game draws at half the rate with what MetalFX Temporal and the interpolator cost on top.
//    Measured on the M6 at full clock (bench.py headroom, open ground, Recommended, 2026-11-05): 11.8 ms a drawn frame at 2560 x 1440, 17.9 at 3024 x 1890, 23.9 at 3840 x 2160, out of 33.3: FG_BASE +
//    FG_PER_MP per output megapixel; 3D Resolution saves FG_3D_PER_MP of it (100% to 50% at 2560 x 1440: 11.9 to 8.4 ms). So at this Mac's sizes it costs more GPU per second than real frames do, and
//    what it buys is free time per drawn frame (21.5 ms against 9.4 at 2560 x 1440) and smooth motion on a chip that holds it. On a Mac where it doesn't fit, the conventional choice below stands, at real
//    frame rate, rather than a smaller picture: only the M6 has been measured, and elsewhere this is the same estimate from public benchmark speeds as the rest.
// No MetalFX: a fixed resolution, the picture by its load. A fixed resolution has the game picture's shape (s->aspect: 1920 x 1200 on a 16:10 screen,
// as front_mac.m front_game_res renders it), not 16:9.
// Memory is not an input: no 8 GB Mac has been measured (the GPU heap is a fixed 768 MiB).
#include "dm_picrec.h"
#include <math.h>
#include <stdint.h>
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
// The present into a viewport larger than the Native picture (5. above), in load units per megapixel (one unit is about 1 ms of the M6's GPU time:
// 12.44 units took 12.4 ms at p50). Measured on the M6 on 2026-11-03 (CLI host in front, real full screen, Native 3840 x 2160 into DM_TEST_DRAWABLE_PX,
// present GPU p50 over 300 presents, run/w4b/pres): 1:1 0.22-0.24 ms; bilinear 0.28 ms into 4480 x 2520 and 0.335 into 5120 x 2880; MetalFX 1.73 and 2.14 ms
#define PRES_ONE 0.0277           // the 1:1 present of the picture, which OUT_WEIGHT already carries: 0.23 ms at 3840 x 2160
#define PRES_MFX 0.149            // MetalFX at the present, per viewport megapixel (in place of the 1:1 present)
#define PRES_BIL 0.0145           // bilinear filtering, per megapixel the viewport adds to the picture
// The light picture's lowest 3D Resolution (index 3 = 67%): the light picture always comes with Frame Drops = lower, and the 3D scale controller
// (hle_oe.c, floor 0.60) keeps two steps below 67% and none below 50%. Below it the fixed resolution takes over (t_picrec invariants I3, I8).
#define LIGHT_MIN 3
#define FG_BASE 2.1               // load units (about ms of the M6's GPU) of a generated pair at full clock, 7. above
#define FG_PER_MP 2.63
#define FG_3D_PER_MP 1.26
#define FG_MIN 2                  // Frame Generation's lowest 3D Resolution: 75%
static const float k_scale[5] = { 1.0f, 0.85f, 0.75f, 0.67f, 0.5f };   // the 3D Resolution row
static const int k_fixed_w[3] = { 1600, 1920, 2560 };                   // the Resolution row's fixed widths

static double core_speed(int iphone, int gen) {
    if (iphone) {
        // A14 … A20: the M generation built on the same GPU design. The A18's slot (M4) is never read: the A18 uses its own measurement
        static const int k_a_to_m[7] = { 1, 2, 2, 3, 4, 5, 6 };
        if (gen == 18) return 6257;   // the A18 Pro in the MacBook Neo: 31.3k / 5 cores
        gen = gen < 14 ? 0 : gen > 20 ? M_KNOWN : k_a_to_m[gen - 14];
    }
    if (gen < 1) return k_core_m[1];
    return k_core_m[gen > M_KNOWN ? M_KNOWN : gen];
}

int dm_picrec_real(int app, int app_support, int test_firstboot) { return app && (!app_support || test_firstboot); }

int dm_picrec_id(const dm_mac_spec *s) {
    uint32_t h = 2166136261u;
    for (const unsigned char *g = (const unsigned char *)(s->gpu ? s->gpu : ""); *g; g++) h = (h ^ *g) * 16777619u;
    int v[8] = { s->cores, s->out_w, s->out_h, s->mfx }, n = 4;
    if (s->fg) v[n++] = 1;
    long hz = s->pace_hz > 0 ? lround(s->pace_hz * 100) : 6000;
    if (dm_picrec_enlarges(s)) { v[n++] = s->drw_w; v[n++] = s->drw_h; }   // only an input that changes the model: a plain 60 Hz machine keeps its id
    if (hz != 6000) v[n++] = (int)hz;
    for (int i = 0; i < n; i++) for (int b = 0; b < 4; b++) h = (h ^ ((uint32_t)v[i] >> (8 * b) & 0xff)) * 16777619u;
    return (int)(h & 0x7fffffff);
}

// load of a picture: 3D at `scale` of an output w x h
static double load(int w, int h, double scale) { double out = (double)w * h / 1e6; return out * scale * scale + OUT_WEIGHT * out; }

int dm_picrec_enlarges(const dm_mac_spec *s) { return s->drw_w > s->out_w * 1.05 && s->drw_h > s->out_h * 1.05; }
// The present's enlargement of the Native picture into its viewport, in load units (0 when it does not enlarge): MetalFX with the 3D at 100%,
// bilinear filtering after a 3D MetalFX pass; both above the 1:1 present of the picture
static double pres_load(const dm_mac_spec *s, int full_3d) {
    if (!dm_picrec_enlarges(s)) return 0;
    double d = (double)s->drw_w * s->drw_h / 1e6, o = (double)s->out_w * s->out_h / 1e6;
    return full_3d ? PRES_MFX * d - PRES_ONE * o : PRES_BIL * (d - o);
}

// An iPhone chip keeps the phone's shadows whatever its speed: environment shadows off and the Standard map (their cost on an A-series GPU is not
// measured). The 3D Resolution the model picked stays; Frame Drops lowers it, as with any light picture. scene: the full picture's load at that 3D
// Resolution; rate and cap as the choice counted them
static void iphone_chip_light(dm_mac_rec *r, double scene, double rate, double cap) {
    if (r->tier == -1 && r->envsh) { r->envsh = 0; r->shq = 0; r->load = scene / LIGHT_GAIN * rate / cap + r->pres; r->q3d = 0; }
}

void dm_mac_recommend(const dm_mac_spec *s, dm_mac_rec *r) {
    memset(r, 0, sizeof *r);
    const char *g = s->gpu ? s->gpu : "";
    int iphone = !strncmp(g, "Apple A", 7), mchip = !strncmp(g, "Apple M", 7);
    r->gen = iphone || mchip ? atoi(g + 7) : 0;
    r->tier = iphone ? -1 : !mchip ? 0 : strstr(g, " Ultra") ? 3 : strstr(g, " Max") ? 2 : strstr(g, " Pro") ? 1 : 0;
    static const int k_min_cores[4] = { 7, 14, 24, 48 };   // no count from the IORegistry: the smallest part of each tier (7-core M1, 14-core M1 / M3 Pro, 24-core M1 Max, 48-core M1 Ultra)
    int cores = s->cores > 0 ? s->cores : iphone ? 5 : k_min_cores[r->tier];
    r->speed = cores * core_speed(iphone, r->gen) / REF_SCORE;
    double cap = CAPACITY * r->speed, rate = s->pace_hz > 0 ? s->pace_hz / 60 : 1;
    if (s->mfx && s->fg) {   // Frame Generation: the full picture, half the drawn frames; the present still runs at the shown rate
        double out = (double)s->out_w * s->out_h / 1e6;
        for (int i = 0; i <= FG_MIN; i++) {
            double pr = pres_load(s, i == 0), l = ((FG_BASE + out * (FG_PER_MP - FG_3D_PER_MP * (1 - (double)k_scale[i] * k_scale[i]))) / 2 + pr) * rate;
            if (l > cap) continue;
            r->fg = 1; r->native = 1; r->s3d = i; r->load = l / cap; r->pres = pr * rate / cap; r->q3d = r->tier != -1 && r->load <= CLOSE;
            r->envsh = r->tier != -1; r->shq = r->tier != -1 ? 2 : 0;   // an iPhone chip keeps the phone's shadows, as below
            return;
        }
    }
    if (s->mfx)   // Native: the full picture down to 75%, else the light one down to 67%; the present's enlargement and the frame rate counted
        for (int light = 0; light < 2; light++)
            for (int i = 0; i < (light ? LIGHT_MIN + 1 : 3); i++) {
                double scene = load(s->out_w, s->out_h, k_scale[i]), pr = pres_load(s, i == 0), l = (scene / (light ? LIGHT_GAIN : 1) + pr) * rate;
                if (l <= cap) {
                    r->native = 1; r->s3d = i; r->envsh = !light; r->shq = light ? 0 : 2; r->load = l / cap; r->pres = pr * rate / cap; r->q3d = !(light || r->load > CLOSE);
                    iphone_chip_light(r, scene, rate, cap); return;
                }
            }
    // a fixed resolution: the highest width up to 3/4 of the screen (the 2560 one from half the M6's speed), the 3D at 100%, the picture by its load
    for (int i = 0; i < 3; i++) if (k_fixed_w[i] <= (s->out_w * 3 / 4 > 1600 ? s->out_w * 3 / 4 : 1600) && (i < 2 || r->speed >= 0.5)) r->tier_res = i;
    int w = k_fixed_w[r->tier_res], h = s->aspect > 0 ? (int)(w / s->aspect / 2 + 0.5) * 2 : w * 9 / 16;   // the game renders the fixed width at the picture's shape
    double scene = load(w, h, 1), full = scene * rate, light = full / LIGHT_GAIN;
    // the present's enlargement of the fixed picture into its viewport (the Native picture's viewport, or the screen's own size when that is not
    // past the cap): MetalFX, or bilinear filtering on a Mac without it; above the 1:1 present the output weight already carries
    double vw = s->drw_w > 0 ? s->drw_w : s->out_w, vh = s->drw_h > 0 ? s->drw_h : s->out_h, vp = vw * vh / 1e6, fp = (double)w * h / 1e6;
    double pres = vw > w * 1.05 && vh > h * 1.05 ? (s->mfx ? PRES_MFX * vp - PRES_ONE * fp : PRES_BIL * (vp - fp)) * rate : 0;
    r->native = 0; r->s3d = 0; r->envsh = full <= cap; r->shq = r->envsh ? 2 : 0; r->pres = pres / cap;
    r->load = ((r->envsh ? full : light) + pres) / cap; r->q3d = !(!r->envsh || r->load > CLOSE);
    iphone_chip_light(r, scene, rate, cap);
}
