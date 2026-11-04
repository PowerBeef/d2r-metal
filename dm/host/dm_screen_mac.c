// Screen geometry of the Mac App as pure functions (dm_screen_mac.h): front_mac.m's full_setup, native_w and front_game_res go through them, and
// t_screen_mac pins every rule. No game addresses.
#include "dm_screen_mac.h"
#include <math.h>

dm_scr_pic dm_scr_picture(double w, double h) {
    if (h <= 0 || w / h >= 16.0 / 9 - 0.01) return (dm_scr_pic){ 16.0 / 9, 0, 0 };   // 16:9 or wider (an external 16:9 screen, an ultrawide): 16:9 throughout
    if (w / h < DM_SCR_MIN_ASPECT) return (dm_scr_pic){ DM_SCR_MIN_ASPECT, 1, 1 };   // narrower than 4:3: a 4:3 picture with bars (the iPad's layout)
    return (dm_scr_pic){ w / h, 1, 0 };
}

int dm_scr_native_w(double w_px, double h_px, double aspect) {
    if (!(aspect > 0)) aspect = 16.0 / 9;
    double pw = w_px, fit = floor(h_px * aspect + 0.5), cap = floor(sqrt(DM_SCR_MAX_PX * aspect) + 1e-6);   // the epsilon keeps 3840 x 2160 at 16:9 whole
    if (h_px > 0 && fit < pw) pw = fit;   // the picture's width on this screen: a screen wider than the picture shows it with side bars
    if (cap < pw) pw = cap;
    int w = (int)pw & ~1;
    return w < 1600 ? 1600 : w > 3840 ? 3840 : w;
}

uint64_t dm_scr_game_res(int render_w, double aspect, int full, int docked) {
    if (full)   // true full screen: width = the output width, height by the screen's aspect (16:10 -> 1600x1000); handheld 720 high
        return docked ? (uint64_t)(llround(render_w / aspect / 2) * 2) << 32 | (uint64_t)render_w : 720ull << 32 | (uint64_t)(llround(720 * aspect / 8) * 8);
    return docked ? (uint64_t)(llround(render_w * 9.0 / 16 / 2) * 2) << 32 | (uint64_t)render_w : 720ull << 32 | 1280;   // 16:9: 1600x900 by default
}

void dm_scr_fit_window(double aspect, double max_w, double max_h, double *w, double *h) {
    double k = 1;
    if (max_w > 0 && max_h > 0) k = fmin(1, fmin(max_w / 1280, max_h / (1280 / aspect)));
    *w = floor(1280 * k); *h = floor(*w / aspect);
}

int dm_scr_matches(int tier, int render_w, int full, double aspect, double w, double h) {
    dm_scr_pic p = dm_scr_picture(w, h);
    if (p.full != full || (full && fabs(p.aspect / aspect - 1) > 0.01)) return 0;
    return tier != 3 || dm_scr_native_w(w, h, p.aspect) == render_w;
}
