// The shared picture geometry (dm_geom.h): the present's fit of a picture into a drawable (1:1 near match, fill, fit) and its viewport; the iOS
// front end's picture choice, game-view placement inside the picture, render size for a window (the iPad 4:3 minimum) and edge comparison.
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

// the picture the game view is placed by: (real picture, UI band height) -> picture, whether it is the real one
static void pic(int rw, int rh, int ui_h, double w, double h, int real) {
    double o[2] = { -1, -1 }; int r = dm_geom_pic(rw, rh, ui_h, o); n++;
    if (r == real && o[0] == w && o[1] == h) return;
    fails++; printf("✗ dm_geom_pic(%dx%d, ui_h %d) = %.0fx%.0f real %d, expected %.0fx%.0f real %d\n", rw, rh, ui_h, o[0], o[1], r, w, h, real);
}
// the game view (the UI band) inside a picture rect p, in points (k = scale) or pixels (k = 1)
static void uiv(double px, double py, double pw, double ph, int rh, int ui_y, int ui_h, double k, double x, double y, double w, double h) {
    const double p[4] = { px, py, pw, ph }; double o[4]; dm_geom_ui_view(p, rh, ui_y, ui_h, k, o); n++;
    if (o[0] == x && o[1] == y && o[2] == w && o[3] == h) return;
    fails++; printf("✗ dm_geom_ui_view(%.1f,%.1f %.1fx%.1f, rh %d, ui %d+%d, k %.0f) = %.1f,%.1f %.1fx%.1f, expected %.1f,%.1f %.1fx%.1f\n", px, py, pw, ph, rh, ui_y, ui_h, k,
           o[0], o[1], o[2], o[3], x, y, w, h);
}
// the render size for a window of vw x vh points at scale k under a cap (min_aspect 0 = the phone rule, 4/3 = the iPad minimum)
static void wpx(double vw, double vh, double k, double cw, double ch, double ma, unsigned w0, unsigned w1, unsigned h, int clamp) {
    uint32_t o[2] = { 0, 0 }; int r = dm_geom_window_px(vw, vh, k, cw, ch, ma, o); n++;
    if (o[0] >= w0 && o[0] <= w1 && o[1] == h && (clamp < 0 || r == clamp)) return;
    fails++; printf("✗ dm_geom_window_px(%.0fx%.0f pt, k %.0f, cap %.0fx%.0f, min aspect %.3f) = %ux%u clamp %d, expected %u..%ux%u clamp %d\n", vw, vh, k, cw, ch, ma,
           o[0], o[1], r, w0, w1, h, clamp);
}
static void same(const double a[4], const double b[4], double tol, int want) {
    n++; if (dm_geom_same(a, b, tol) == want) return;
    fails++; printf("✗ dm_geom_same(%.0f,%.0f %.0fx%.0f; %.0f,%.0f %.0fx%.0f; %.0f) != %d\n", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], tol, want);
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
    present(1334, 750, 0, 0, 2868, 1320, DM_GEOM_FIT, 0, 68, 1334, 614, 0, 0, 2868, 1320);        // width-limited: crop x scale rounds above the drawable, the bar must not go to -1
    present(750, 1334, 0, 0, 1320, 2868, DM_GEOM_FIT, 68, 0, 614, 1334, 0, 0, 1320, 2868);        // the same, height-limited
    fit(2560, 1440, 2304, 1440, 128, 0, 2304, 1440);
    fit(2560, 1440, 2560, 1440, 0, 0, 2560, 1440);
    fit(2560, 1440, 0, 0, 0, 0, 2560, 1440);
    // iPad windows (IOS-2): pictures in drawables the window sizes give
    fit(2732, 2048, 2732, 2048, 0, 0, 2732, 2048);       // an iPad full screen in a run: 1:1
    fit(2732, 2048, 2732, 1536, 0, 256, 2732, 1536);     // out of a run: 16:9 of the width, bars above and below
    fit(2732, 1200, 2048, 1536, 566, 0, 1600, 1200);     // a 4:3 picture in a wide window: bars at the sides
    fit(1000, 750, 1000, 562, 0, 94, 1000, 562);
    fit(800, 750, 800, 600, 0, 75, 800, 600);
    fit(1800, 1600, 1800, 1350, 0, 125, 1800, 1350);     // a window narrower than 4:3: the 4:3 picture with bars
    pic(2732, 2048, 1536, 2732, 2048, 1);                // in a run: the UI band is shorter than the picture
    pic(800, 600, 600, 16, 9, 0);                        // the game's boot default before it writes its size
    pic(2732, 1200, 1200, 2732, 1200, 1);                // wider than 16:9
    pic(1000, 562, 562, 1000, 562, 1);                   // 16:9 within the rounding (9000 >= 8992)
    pic(0, 0, 0, 16, 9, 0);
    uiv(0, 0, 1366, 1024, 2048, 512, 1536, 2, 0, 256, 1366, 768);
    uiv(283, 0, 800, 600, 1200, 300, 900, 1, 283, 150, 800, 450);
    uiv(0, 37.5, 400, 300, 600, 150, 450, 2, 0, 112.5, 400, 225);
    uiv(0, 125, 1800, 1350, 1350, 338, 1012, 1, 0, 463, 1800, 1012);   // pixels: the 4:3 picture's band
    uiv(0, 125, 1800, 1350, 1350, 0, 1350, 2, 0, 125, 1800, 1350);     // the band is the whole picture: unchanged
    wpx(1366, 1024, 2, 2732, 2048, 4.0 / 3, 2732, 2732, 2048, 0);
    wpx(1100, 700, 2, 2732, 2048, 4.0 / 3, 2200, 2200, 1400, 0);
    wpx(900, 800, 2, 2732, 2048, 4.0 / 3, 1800, 1800, 1350, 1);         // narrower than 4:3: the height lowered
    wpx(800, 900, 2, 2732, 2048, 4.0 / 3, 1600, 1600, 1200, 1);         // portrait-shaped: 4:3 of the width
    wpx(1366, 600, 2, 2732, 2048, 4.0 / 3, 2732, 2732, 1200, 0);
    wpx(667, 375, 2, 2732, 2048, 4.0 / 3, 1334, 1334, 750, 0);           // the minimum scene size
    wpx(1920, 1080, 2, 2732, 2048, 4.0 / 3, 2730, 2732, 1536, 0);        // larger than the cap: scaled into it
    wpx(400, 375, 2, 2048, 1536, 4.0 / 3, 800, 800, 600, 1);
    wpx(500, 375, 2, 2048, 1536, 4.0 / 3, 1000, 1000, 750, 0);           // exactly 4:3: no clamp
    wpx(375, 667, 3, 2868, 1320, 0, 2000, 2000, 1124, 0);                // phones: the long side is the width, even sizes
    wpx(678, 466, 3, 3142, 2160, 0, 2034, 2034, 1398, -1);               // a foldable's outer screen
    wpx(900, 700, 2, 3142, 2160, 0, 1800, 1800, 1400, 0);                // phones are never clamped to 4:3
    { const double a[4] = { 0, 256, 2732, 1536 }, b[4] = { 1, 257, 2731, 1535 }, c[4] = { 0, 262, 2732, 1536 };
      same(a, b, 2, 1); same(a, c, 2, 0); }
    printf("t_geom: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
