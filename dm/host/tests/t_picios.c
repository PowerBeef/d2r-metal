// The iPhone's and iPad's Recommended Picture Size (dm_picios.c): the sizes, the picks for every device, and the model's shape
#include "dm_picios.h"
#include "t_picios_devices.h"
#include <stdio.h>
#include <string.h>
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void size_is(unsigned w, unsigned h, int t, unsigned ew, unsigned eh) { unsigned o[2]; dm_picios_size(w, h, t, o); CHECK(o[0] == ew && o[1] == eh, "%ux%u tier %d: want %ux%u, got %ux%u", w, h, t, ew, eh, o[0], o[1]); }
static int tier(const char *m, const char *g, unsigned w, unsigned h) { dm_ios_spec s = { m, g, w, h, 1 }; return dm_picios_tier(&s); }
static void table(void) {   // t_picios --table: what the README's and docs' tables are made from (device, screen, pick, picture, world at 3D 50%, estimated GPU ms)
    static const char *const n[DM_OSZ_N] = { "Full", "High", "Medium", "Low" };
    for (unsigned i = 0; i < sizeof k_dev / sizeof *k_dev; i++) { dm_ios_spec s = { k_dev[i].m, k_dev[i].g, k_dev[i].w, k_dev[i].h, 1 }; unsigned o[2]; int t = dm_picios_tier(&s); dm_picios_size(k_dev[i].w, k_dev[i].h, t, o);
        printf("%s|%s|%ux%u|%s|%ux%u|%ux%u|%.1f\n", k_dev[i].name, k_dev[i].g, k_dev[i].w, k_dev[i].h, n[t], o[0], o[1], o[0] / 2, o[1] / 2, dm_picios_ms(&s, o[0] * (double)o[1] / 1e6)); }
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--table")) { table(); return 0; }
    // Sizes on the phone the costs were measured on
    size_is(2622, 1206, DM_OSZ_FULL, 2622, 1206); size_is(2622, 1206, DM_OSZ_HIGH, 1966, 904); size_is(2622, 1206, DM_OSZ_MEDIUM, 1640, 754); size_is(2622, 1206, DM_OSZ_LOW, 1566, 720);
    // Nothing goes below Low: a small screen's Medium would be 675 rows
    size_is(2340, 1080, DM_OSZ_MEDIUM, 1560, 720); size_is(2340, 1080, DM_OSZ_LOW, 1560, 720);
    // A screen at or under 720 rows stays as it is, whatever the tier
    size_is(1334, 750, DM_OSZ_LOW, 1280, 720); size_is(1280, 720, DM_OSZ_LOW, 1280, 720); size_is(1136, 640, DM_OSZ_LOW, 1136, 640);
    // A picture narrower than 16:9 lays its interface out 16:9 across its width: Low keeps that interface at 720 rows
    size_is(2732, 2048, DM_OSZ_LOW, 1280, 960); size_is(2732, 2048, DM_OSZ_HIGH, 2050, 1536); size_is(2360, 1640, DM_OSZ_LOW, 1280, 890);
    for (unsigned i = 0; i < sizeof k_dev / sizeof *k_dev; i++) {
        unsigned w = k_dev[i].w, h = k_dev[i].h, o[2]; dm_ios_spec s = { k_dev[i].m, k_dev[i].g, w, h, 1 };
        int t = dm_picios_tier(&s);
        CHECK(t == k_dev[i].pick, "%s: want tier %d, got %d", k_dev[i].name, k_dev[i].pick, t);
        for (int q = DM_OSZ_FULL; q < DM_OSZ_N; q++) { dm_picios_size(w, h, q, o);
            // Every picture has an intact interface when the screen itself can give one and keeps the screen's shape; below Full its sides are even (a screen's own rows may not be: 1179)
            double ui = (double)o[0] / o[1] < 16.0 / 9 ? o[0] * 9.0 / 16 : o[1];
            CHECK(ui >= 719.5 || h < 720, "%s tier %d: %ux%u has %.0f interface rows", k_dev[i].name, q, o[0], o[1], ui);
            CHECK(q == DM_OSZ_FULL || (!(o[0] & 1) && !(o[1] & 1)), "%s tier %d: %ux%u has an odd side", k_dev[i].name, q, o[0], o[1]);
            double da = ((double)o[0] / o[1]) / ((double)w / h); CHECK(da > 0.99 && da < 1.01, "%s tier %d: %ux%u off the screen's shape", k_dev[i].name, q, o[0], o[1]);
            CHECK(o[0] <= w && o[1] <= h, "%s tier %d: %ux%u larger than the screen", k_dev[i].name, q, o[0], o[1]); }
        CHECK(dm_picios_id(&s) >= 0, "%s: rec_id must not be negative", k_dev[i].name);
        // What was picked fits, unless nothing did (Low); and the next size up does not
        dm_picios_size(w, h, t, o); double ms = dm_picios_ms(&s, o[0] * (double)o[1] / 1e6);
        CHECK(t == DM_OSZ_LOW || ms <= 28.34, "%s: picked tier %d at %.1f ms", k_dev[i].name, t, ms);
        if (t > DM_OSZ_FULL) { unsigned up[2]; dm_picios_size(w, h, t - 1, up); if (up[0] != o[0]) CHECK(dm_picios_ms(&s, up[0] * (double)up[1] / 1e6) > 28.33, "%s: tier %d also fits", k_dev[i].name, t - 1); }
    }
    // The one measured device: the estimate is the measurement
    { dm_ios_spec s = { "iPhone18,1", "Apple A19 Pro GPU", 2622, 1206, 1 }; double ms = dm_picios_ms(&s, 2622 * 1206 / 1e6); CHECK(ms > 23.4 && ms < 23.6, "iPhone 17 Pro at Full: %.2f ms, measured 23.5", ms); }
    // A faster chip never gets a smaller picture on the same screen
    { static const char *const g[] = { "Apple A14 GPU", "Apple A15 GPU", "Apple A16 GPU", "Apple A17 Pro GPU", "Apple A18 GPU", "Apple A18 Pro GPU", "Apple A19 GPU", "Apple A19 Pro GPU", "Apple A20 GPU", "Apple A20 Pro GPU" };
      int last = DM_OSZ_LOW; for (unsigned i = 0; i < sizeof g / sizeof *g; i++) { int t = tier("iPhone99,1", g[i], 2622, 1206); CHECK(t <= last, "%s: tier %d after %d", g[i], t, last); last = t; } }
    // A chip the table doesn't know is newer: the newest known. A device that isn't an iPhone or iPad is the iOS package on a Mac, on its own window: Full, as before
    CHECK(tier("iPhone99,1", "Apple A30 GPU", 2868, 1320) == DM_OSZ_FULL, "a newer chip counts as the newest known");
    CHECK(tier("iPad99,1", "Apple M9 GPU", 2752, 2064) == DM_OSZ_FULL, "a newer M chip counts as the newest known");
    CHECK(tier("Mac17,1", "Apple M6 GPU", 3840, 2160) == DM_OSZ_FULL, "the iOS package on a Mac is left alone");
    CHECK(tier("iPhone99,1", "something else", 2622, 1206) == DM_OSZ_FULL, "a GPU name that can't be read: Full");
    // The chip's name alone will do (a test's stand-in)
    CHECK(tier("iPhone13,2", "A14", 2532, 1170) == DM_OSZ_LOW, "a bare chip name");
    // An identifier is matched whole: iPhone14,4 is the 4-core A15, iPhone14,44 would not be
    CHECK(tier("iPhone14,44", "Apple A15 GPU", 2532, 1170) == DM_OSZ_MEDIUM && tier("iPhone14,4", "Apple A15 GPU", 2532, 1170) == DM_OSZ_LOW, "identifiers match whole");
    // Without Frame Generation a frame has half the time
    { dm_ios_spec s = { "iPhone18,1", "Apple A19 Pro GPU", 2622, 1206, 0 }; CHECK(dm_picios_tier(&s) != DM_OSZ_FULL, "no Frame Generation: Full can't fit"); }
    printf("%s t_picios: %d checks, %d failed\n", fails ? "RED" : "OK", checks, fails);
    return fails != 0;
}
