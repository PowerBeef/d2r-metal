// The shared picture geometry (dm_geom.h): the present's fit of a picture into a drawable (1:1 near match, fill, fit) and its viewport.
#include <stdio.h>
#include "../dm_geom.h"

static int n, fails;
static const char *const k_kind[3] = { "1:1", "fill", "fit" };

// (drawable; picture crop) -> kind, viewport, source
static void present(double dw, double dh, unsigned cx, unsigned cy, unsigned cw, unsigned ch, int kind, double x, double y, double w, double h,
                    unsigned sx, unsigned sy, unsigned sw, unsigned sh) {
    dm_geom_pfit f = dm_geom_present(dw, dh, cx, cy, cw, ch); n++;
    if (f.kind == kind && f.x == x && f.y == y && f.w == w && f.h == h && f.sx == sx && f.sy == sy && f.sw == sw && f.sh == sh) return;
    fails++;
    printf("✗ %.0fx%.0f; %u,%u %ux%u -> %s vp %.0f,%.0f %.0fx%.0f src %u,%u %ux%u, expected %s vp %.0f,%.0f %.0fx%.0f src %u,%u %ux%u\n", dw, dh, cx, cy, cw, ch,
           k_kind[f.kind], f.x, f.y, f.w, f.h, f.sx, f.sy, f.sw, f.sh, k_kind[kind], x, y, w, h, sx, sy, sw, sh);
}
static void fit(double dw, double dh, double pw, double ph, double x, double y, double w, double h) {
    double o[4]; dm_geom_fit(dw, dh, pw, ph, o); n++;
    if (o[0] == x && o[1] == y && o[2] == w && o[3] == h) return;
    fails++; printf("✗ dm_geom_fit(%.0fx%.0f, %.0fx%.0f) = %.0f,%.0f %.0fx%.0f, expected %.0f,%.0f %.0fx%.0f\n", dw, dh, pw, ph, o[0], o[1], o[2], o[3], x, y, w, h);
}

int main(void) {
    present(3024, 1898, 0, 0, 3024, 1900, DM_GEOM_ONE, 0, 0, 3024, 1898, 0, 1, 3024, 1898);       // a notched MacBook: content one point shorter
    present(2940, 1848, 0, 0, 2940, 1848, DM_GEOM_ONE, 0, 0, 2940, 1848, 0, 0, 2940, 1848);       // equal sizes
    present(2940, 1848, 0, 0, 2928, 1840, DM_GEOM_FILL, 0, 0, 2940, 1848, 0, 0, 2928, 1840);      // the old 16-aligned Native stays a stretch
    present(2560, 1600, 0, 0, 2560, 1440, DM_GEOM_FIT, 0, 80, 2560, 1440, 0, 0, 2560, 1440);      // 16:9 on a 16:10 drawable: bars
    present(5120, 2880, 0, 0, 3840, 2160, DM_GEOM_FILL, 0, 0, 5120, 2880, 0, 0, 3840, 2160);      // an enlargement (MetalFX decides later)
    present(2622, 1206, 0, 0, 2622, 1206, DM_GEOM_ONE, 0, 0, 2622, 1206, 0, 0, 2622, 1206);       // the iPhone 17 Pro
    present(2868, 1320, 0, 0, 1952, 900, DM_GEOM_FILL, 0, 0, 2868, 1320, 0, 0, 1952, 900);        // within 1% (the rounding of the game width)
    present(2560, 1440, 0, 0, 2304, 1440, DM_GEOM_FIT, 128, 0, 2304, 1440, 0, 0, 2304, 1440);     // 16:10 on 16:9: pillarbox
    present(2300, 1440, 0, 0, 2304, 1442, DM_GEOM_ONE, 0, 0, 2300, 1440, 2, 1, 2300, 1440);       // up to 16 px larger: the centre 1:1
    present(2300, 1440, 0, 0, 2320, 1440, DM_GEOM_FILL, 0, 0, 2300, 1440, 0, 0, 2320, 1440);      // 20 px larger: past the near match
    present(2304, 1440, 0, 0, 2304, 1296, DM_GEOM_FIT, 0, 72, 2304, 1296, 0, 0, 2304, 1296);      // the 16:9 front end on a 16:10 screen
    present(7, 7, 0, 0, 8, 8, DM_GEOM_ONE, 0, 0, 7, 7, 0, 0, 7, 7);
    present(6, 6, 0, 0, 8, 8, DM_GEOM_ONE, 0, 0, 6, 6, 1, 1, 6, 6);
    present(6, 6, 2, 3, 8, 8, DM_GEOM_ONE, 0, 0, 6, 6, 3, 4, 6, 6);                                // a crop offset carries over
    present(0, 0, 0, 0, 8, 8, DM_GEOM_FILL, 0, 0, 0, 0, 0, 0, 8, 8);                                // no drawable: nothing computed
    fit(2560, 1440, 2304, 1440, 128, 0, 2304, 1440);
    fit(2560, 1440, 2560, 1440, 0, 0, 2560, 1440);
    fit(2560, 1440, 0, 0, 0, 0, 2560, 1440);
    printf("t_geom: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
