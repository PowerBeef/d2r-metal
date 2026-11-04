// Picture geometry shared by the present (dm/gfx/nvn/nvn_mtl.m present_encode, present_encode_m4) and the front ends: where a picture of the game
// goes in a drawable, and which part of it is shown. Pure C11, header-only, shared by the Mac and iOS builds; unit-tested by t_geom.
#pragma once
#include <math.h>

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
    double s = fmin(dw / cw, dh / ch);
    f.kind = DM_GEOM_FIT; f.x = floor((dw - cw * s) / 2); f.y = floor((dh - ch * s) / 2); f.w = round(cw * s); f.h = round(ch * s);
    return f;
}
// The viewport alone, for a picture of pw x ph pixels in a drawable of dw x dh (out = x, y, w, h): the present's viewport for that picture
static inline void dm_geom_fit(double dw, double dh, double pw, double ph, double out[4]) {
    out[0] = 0; out[1] = 0; out[2] = dw; out[3] = dh;
    if (dw <= 0 || dh <= 0 || pw < 1 || ph < 1) return;
    dm_geom_pfit f = dm_geom_present(dw, dh, 0, 0, (unsigned)pw, (unsigned)ph);
    out[0] = f.x; out[1] = f.y; out[2] = f.w; out[3] = f.h;
}
