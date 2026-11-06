// The iPhone's and iPad's Recommended Picture Size, from the chip. The 3D is at 50% on every device; what gives way on a slower chip is the size of the picture,
// since the upscaler, the frame interpolator and the interface work at that size.
// 1. Cost: measured on an iPhone 17 Pro at 3D 50% (2026-11-06; own GPU time by the DM_TEST_LOAD method, as the GPU otherwise fills whatever time it has):
//    2622 x 1206 23.5 ms, 1952 x 900 13.7, 1640 x 754 10.2, 1568 x 720 9.3, 1280 x 590 7.1. They fit 1.4 + 7.0 ms per megapixel.
// 2. Speed: Geekbench 7 Metal against the A19 Pro (public results, 2026). A chip's name doesn't give its GPU cores, so a few devices go by their model identifier. An iPad
//    counts PAD_GAIN more: it has more room to shed heat than a phone, which is a guess.
// 3. Choice: the largest tier whose estimate stays under BUDGET, 85% of a drawn frame's 33.3 ms, the Mac's margin (dm_picrec.c); Low when none does, as nothing below it
//    has an intact interface. A chip without Frame Generation has 16.7 ms a frame.
// Only the iPhone 17 Pro has been measured. Everything else is an estimate from public benchmarks, and the README and docs say so
#include "dm_picios.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REF_SCORE 41595.0   // the A19 Pro of the iPhone 17 Pro the costs were measured on
#define COST_BASE 1.4
#define COST_PER_MP 7.0
#define BUDGET (33.33 * 0.85)
#define PAD_GAIN (0.8 / 0.7)

// The picture's rows: the screen's, or a share of them, never under the Low size. Low is 720 rows of interface (below that the belt and the orbs lose their art): a picture
// narrower than 16:9 (every iPad) lays its interface out 16:9 across its width, so it needs more rows than that
void dm_picios_size(unsigned w, unsigned h, int tier, unsigned out[2]) {
    static const double share[3] = { 1, 0.75, 0.625 };
    double a = h ? (double)w / h : 1, low = a < 16.0 / 9 ? 720 * (16.0 / 9) / a : 720, r = tier >= 0 && tier < 3 ? h * share[tier] : low;
    if (r < low) r = low;
    if (r >= h) { out[0] = w; out[1] = h; return; }
    unsigned rows = (unsigned)llround(r / 2) * 2;
    out[0] = (unsigned)llround(rows * a / 2) * 2; out[1] = rows;
}
static int pad(const dm_ios_spec *s) { return s->machine && !strncmp(s->machine, "iPad", 4); }
static int machine_is(const dm_ios_spec *s, const char *list) {   // list: space-separated identifiers
    if (!s->machine || !*s->machine) return 0;
    size_t n = strlen(s->machine);
    for (const char *p = list; (p = strstr(p, s->machine)); p += n) if ((p == list || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return 1;
    return 0;
}
int dm_picios_score(const dm_ios_spec *s) {
    if (s->machine && strncmp(s->machine, "iPhone", 6) && strncmp(s->machine, "iPad", 4)) return 0;   // the iOS package on a Mac
    // Devices with fewer GPU cores than their chip's usual count, which only the model identifier tells
    if (machine_is(s, "iPhone18,4")) return 34662;   // iPhone Air: A19 Pro, 5 cores
    if (machine_is(s, "iPhone18,5")) return 27773;   // iPhone 17e: A19, 4 cores
    if (machine_is(s, "iPhone17,5")) return 19601;   // iPhone 16e: A18, 4 cores
    if (machine_is(s, "iPhone14,4 iPhone14,5 iPhone14,6")) return 12507;   // iPhone 13 mini, 13, SE (3rd generation): A15, 4 cores
    if (machine_is(s, "iPad16,1 iPad16,2")) return 18652;   // iPad mini: A17 Pro, 5 cores
    if (machine_is(s, "iPad14,8 iPad14,9 iPad14,10 iPad14,11")) return 36360;   // iPad Air: M2, 9 cores
    const char *g = s->gpu ? s->gpu : ""; if (!strncmp(g, "Apple ", 6)) g += 6;
    int n = (*g == 'A' || *g == 'M') ? atoi(g + 1) : 0, pro = strstr(g, " Pro") != NULL;
    if (*g == 'A' && n >= 14) {
        static const int k[][2] = { { 12474, 12474 }, { 15634, 15634 }, { 18726, 18726 }, { 22382, 22382 }, { 24501, 27078 }, { 34716, 41595 }, { 51100, 61228 } };   // A14 ... A20: plain, Pro
        int i = n - 14; if (i > 6) { i = 6; pro = 1; }   // a newer generation than the table knows counts as the newest known
        return k[i][pro];
    }
    if (*g == 'M' && n >= 1) { static const int k[] = { 27200, 40400, 42930, 49322, 70000 };   /* M3 and M4 as the 9-core part: an iPad Air's, and the iPad Pro's that only its storage tells from the 10-core one */ return k[n > 5 ? 4 : n - 1]; }
    return 0;
}
double dm_picios_ms(const dm_ios_spec *s, double mp) {
    int sc = dm_picios_score(s);
    double rel = sc ? sc / REF_SCORE : 1;
    return (COST_BASE + COST_PER_MP * mp) / (pad(s) ? rel * PAD_GAIN : rel);
}
int dm_picios_tier(const dm_ios_spec *s) {
    if (!dm_picios_score(s)) return DM_OSZ_FULL;   // not a chip the table knows: a newer one, or a Mac running the iOS package
    unsigned w = s->w > s->h ? s->w : s->h, h = s->w > s->h ? s->h : s->w, o[2];
    double budget = s->fg ? BUDGET : BUDGET / 2;
    for (int t = DM_OSZ_FULL; t < DM_OSZ_LOW; t++) { dm_picios_size(w, h, t, o); if (dm_picios_ms(s, o[0] * (double)o[1] / 1e6) <= budget) return t; }
    return DM_OSZ_LOW;
}
int dm_picios_id(const dm_ios_spec *s) {
    unsigned x = 2166136261u; for (const char *p = s->machine ? s->machine : ""; *p; p++) x = (x ^ (unsigned char)*p) * 16777619u;
    return (int)(x & 0xffffff);
}
