// Picture geometry shared by the present (dm/gfx/nvn/nvn_mtl.m present_encode, present_encode_m4) and the front ends: where a picture of the game
// goes in a drawable, and which part of it is shown; for the iOS front end (dm/ios/Sources/front_ios.m) also which picture its game view is
// placed by, where the game view (the UI band) sits inside it, and the render size a window gets. Pure C11, header-only, shared by the Mac and
// iOS builds; unit-tested by t_geom.
#pragma once
#include <math.h>
#include <stdint.h>

#define DM_GEOM_NEAR_PX 16   // a picture up to this many pixels larger than the drawable each way is shown 1:1, centred
enum { DM_GEOM_ONE, DM_GEOM_FILL, DM_GEOM_FIT };

// The present's fit of a picture (the source crop cx, cy, cw x ch) into a drawable of dw x dh pixels: the viewport (x, y, w, h, in the drawable) and
// the part of the crop that is sampled (sx, sy, sw x sh). Rules, in order:
//   ONE   the picture is at most DM_GEOM_NEAR_PX larger each way (equal sizes included): its centre is shown pixel for pixel over the whole drawable,
//         cropping at most 8 px a side, so a picture a few pixels larger (a notched MacBook's full-screen content one point shorter than the picture)
//         is not resampled as a whole
//   FILL  the aspects differ by less than 1%: the whole drawable, the crop unchanged (a few pixels' difference from the game's rounding stretched,
//         rather than bars a few pixels wide)
//   FIT   otherwise: the largest viewport of the crop's aspect, centred, with black bars around it
// A picture smaller than the drawable is never padded: FILL or FIT enlarge it.
typedef struct { double x, y, w, h; unsigned sx, sy, sw, sh; int kind; } dm_geom_pfit;
static inline dm_geom_pfit dm_geom_present(double dw, double dh, unsigned cx, unsigned cy, unsigned cw, unsigned ch) {
    dm_geom_pfit f = { 0, 0, dw, dh, cx, cy, cw, ch, DM_GEOM_FILL };
    if (dw <= 0 || dh <= 0 || !cw || !ch) return f;
    if (cw >= dw && ch >= dh && cw - dw <= DM_GEOM_NEAR_PX && ch - dh <= DM_GEOM_NEAR_PX) {
        f.kind = DM_GEOM_ONE; f.sx = cx + (cw - (unsigned)dw) / 2; f.sy = cy + (ch - (unsigned)dh) / 2; f.sw = (unsigned)dw; f.sh = (unsigned)dh;
        return f;
    }
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) return f;
    double s = fmin(dw / cw, dh / ch);   // the bars from the rounded size: the limiting side's crop x s can round just above the drawable, and floor would put the bar at -1
    f.kind = DM_GEOM_FIT; f.w = round(cw * s); f.h = round(ch * s); f.x = floor((dw - f.w) / 2); f.y = floor((dh - f.h) / 2);
    return f;
}
// The viewport alone, for a picture of pw x ph pixels in a drawable of dw x dh (out = x, y, w, h): the present's viewport for that picture
static inline void dm_geom_fit(double dw, double dh, double pw, double ph, double out[4]) {
    out[0] = 0; out[1] = 0; out[2] = dw; out[3] = dh;
    if (dw <= 0 || dh <= 0 || pw < 1 || ph < 1) return;
    dm_geom_pfit f = dm_geom_present(dw, dh, 0, 0, (unsigned)pw, (unsigned)ph);
    out[0] = f.x; out[1] = f.y; out[2] = f.w; out[3] = f.h;
}
// The picture the iOS game view is placed by (out = width, height): the real picture rw x rh once the game has written its size, which is when its
// UI band is shorter than it (in a run, true full screen) or it is at least 16:9 (out of a run the game renders 16:9 of the width, or wider);
// otherwise 16:9 (the game's boot default of 800x600, before the game writes its size). Returns 1 when it used the real picture
static inline int dm_geom_pic(int rw, int rh, int ui_h, double out[2]) {
    if (rw > 0 && rh > 0 && ((ui_h > 0 && ui_h < rh) || (int64_t)rw * 9 >= (int64_t)rh * 16)) { out[0] = rw; out[1] = rh; return 1; }
    out[0] = 16; out[1] = 9; return 0;
}
// The game view inside a picture rect p (x, y, w, h): the UI band at ui_y, ui_h of the real picture's rh rows, in points (k = scale; edges
// rounded to whole pixels) or in pixels (k = 1). A band that is the whole picture (or no band) gives p unchanged
static inline void dm_geom_ui_view(const double p[4], int rh, int ui_y, int ui_h, double k, double out[4]) {
    for (int i = 0; i < 4; i++) out[i] = p[i];
    if (rh > 0 && ui_h > 0 && ui_h < rh && k > 0) { out[1] = p[1] + round(p[3] * ui_y / rh * k) / k; out[3] = round(p[3] * ui_h / rh * k) / k; }
}
// The render size for a window of vw x vh points at scale k (out = width, height in pixels), scaled down into the cap (capw x caph, the size the game
// built its window at; it only takes sizes within it) and truncated to even sizes.
//   min_aspect <= 0 (phones): the long side is the width, as a landscape phone (or a foldable's) window always is
//   min_aspect > 0 (iPad: 4/3): never narrower than that aspect; a narrower window keeps its width and gets a lower picture, shown with bars above
//   and below (4:3 is the narrowest picture the game has run at). Returns 1 when that rule lowered the height
static inline int dm_geom_window_px(double vw, double vh, double k, double capw, double caph, double min_aspect, uint32_t out[2]) {
    double w = round(vw * k), h = round(vh * k); int clamp = 0;
    out[0] = out[1] = 0;
    if (w <= 0 || h <= 0 || capw <= 0 || caph <= 0) return 0;
    if (min_aspect <= 0) { double a = fmax(w, h); h = fmin(w, h); w = a; }
    else if (w < h * min_aspect - 0.5) { h = round(w / min_aspect); clamp = 1; }
    double f = fmin(1, fmin(capw / w, caph / h));
    out[0] = (uint32_t)(w * f) & ~1u; out[1] = (uint32_t)(h * f) & ~1u;
    return clamp;
}
// Two rects (x, y, w, h) whose left, top, right and bottom edges each agree within tol
static inline int dm_geom_same(const double a[4], const double b[4], double tol) {
    return fabs(a[0] - b[0]) <= tol && fabs(a[1] - b[1]) <= tol && fabs(a[0] + a[2] - b[0] - b[2]) <= tol && fabs(a[1] + a[3] - b[1] - b[3]) <= tol;
}
