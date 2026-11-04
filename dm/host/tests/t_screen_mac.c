// The Mac App's screen geometry (dm_screen_mac.c): the picture's shape on a main screen, the Native output width and the game resolution.
#include <math.h>
#include <stdio.h>
#include "../dm_screen_mac.h"

static int n, fails;
#define CHECK(ok, ...) do { n++; if (!(ok)) { fails++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pic(double w, double h, double aspect, int full, int clamped) {
    dm_scr_pic p = dm_scr_picture(w, h);
    CHECK(fabs(p.aspect - aspect) < 5e-5 && p.full == full && p.clamped == clamped, "dm_scr_picture(%.0f, %.0f) = aspect %.4f full %d clamped %d, expected %.4f full %d clamped %d",
          w, h, p.aspect, p.full, p.clamped, aspect, full, clamped);
}
static void native(double w, double h, double aspect, int want) {
    int got = dm_scr_native_w(w, h, aspect);
    CHECK(got == want, "dm_scr_native_w(%.0f, %.0f, %.4f) = %d, expected %d", w, h, aspect, got, want);
}
static void res(int rw, double aspect, int full, int docked, unsigned w, unsigned h) {
    uint64_t r = dm_scr_game_res(rw, aspect, full, docked);
    CHECK((uint32_t)r == w && (uint32_t)(r >> 32) == h, "dm_scr_game_res(%d, %.4f, full %d, docked %d) = %ux%u, expected %ux%u", rw, aspect, full, docked,
          (unsigned)(uint32_t)r, (unsigned)(r >> 32), w, h);
}

int main(void) {
    // the picture: 16:9 or wider keeps 16:9 throughout; narrower gets true full screen at the screen's own aspect, down to 4:3
    pic(2560, 1440, 16.0 / 9, 0, 0);
    pic(1512, 950, 1512 / 950.0, 1, 0);
    pic(1440, 900, 1.6, 1, 0);
    pic(1024, 768, 4.0 / 3, 1, 0);
    pic(1280, 1024, 4.0 / 3, 1, 1);   // 5:4: a 4:3 picture with bars
    pic(1440, 2560, 4.0 / 3, 1, 1);   // a rotated display
    pic(3440, 1440, 16.0 / 9, 0, 0);
    pic(1770, 1000, 16.0 / 9, 0, 0);   // within 1% of 16:9
    pic(0, 0, 16.0 / 9, 0, 0);
    // Native: the picture's own pixel width on the screen, within DM_SCR_MAX_PX, even, 1600-3840
    native(2940, 1838, 2940 / 1838.0, 2940);   // the 13-inch Air at its default mode: no longer 2928
    native(3420, 2150, 3420 / 2150.0, 3420);   // the 15-inch Air at its default mode: no longer 3408
    native(2560, 1440, 16.0 / 9, 2560);
    native(3024, 1890, 1.6, 3024);
    native(3456, 2160, 1.6, 3456);
    native(3840, 2160, 16.0 / 9, 3840);
    native(5120, 2880, 16.0 / 9, 3840);
    native(3440, 1440, 16.0 / 9, 2560);        // a 21:9 ultrawide: the 16:9 picture's width, not 3440 minified into the pillarbox
    native(2559, 1440, 16.0 / 9, 2558);
    native(1440, 900, 1.6, 1600);
    native(3840, 2400, 1.6, 3642);             // the heap cap: a 16:10 4K screen
    native(4112, 2584, 4112 / 2584.0, 3632);   // a 16-inch MacBook Pro at its "more space" mode (2056 x 1329 pt), below the notch
    native(3840, 2880, 4.0 / 3, 3324);         // the heap cap at 4:3
    native(6016, 3384, 6016 / 3384.0, 3840);   // a Pro Display XDR
    // the game resolution
    res(2940, 2940 / 1838.0, 1, 1, 2940, 1838);
    res(2560, 1.6, 1, 1, 2560, 1600);
    res(3024, 1.6, 1, 1, 3024, 1890);
    res(1600, 16.0 / 9, 0, 1, 1600, 900);
    res(2560, 16.0 / 9, 0, 0, 1280, 720);
    res(1600, 1.6, 1, 0, 1152, 720);
    // a rotated 5K: 2880 x 5120 used to render 2880 x 5120 (14.7 Mpx) against heap 0; now a 4:3 picture within the cap
    { dm_scr_pic p = dm_scr_picture(2880, 5120); int w = dm_scr_native_w(2880, 5120, p.aspect); uint64_t r = dm_scr_game_res(w, p.aspect, p.full, 1);
      CHECK(w == 2880 && (uint32_t)r == 2880 && (uint32_t)(r >> 32) == 2160, "a rotated 5K: Native %d, game resolution %ux%u, expected 2880x2160", w, (unsigned)(uint32_t)r, (unsigned)(r >> 32)); }
    // the bound: no screen of any shape gives a Native picture larger than DM_SCR_MAX_PX, nor do the fixed widths at the narrowest picture (4:3)
    { static const double k_shape[] = { 0.5, 0.5625, 1.0, 1.25, 4.0 / 3, 1.5, 1.6, 16.0 / 9 - 0.011, 16.0 / 9, 2.39 };
      int over = 0; double worst = 0;
      for (double w = 1000; w <= 6016; w += 8) for (unsigned k = 0; k < sizeof k_shape / sizeof *k_shape; k++) {
          double h = w / k_shape[k]; dm_scr_pic p = dm_scr_picture(w, h); int nw = dm_scr_native_w(w, h, p.aspect); uint64_t r = dm_scr_game_res(nw, p.aspect, p.full, 1);
          double px = (double)(uint32_t)r * (double)(uint32_t)(r >> 32); if (px > worst) worst = px; if (px > DM_SCR_MAX_PX) over++; }
      CHECK(!over, "%d screens give a Native picture above DM_SCR_MAX_PX (largest %.0f px)", over, worst);
      for (int fw = 1600; fw <= 2560; fw += fw == 1600 ? 320 : 640) { uint64_t r = dm_scr_game_res(fw, DM_SCR_MIN_ASPECT, 1, 1);
          CHECK((double)(uint32_t)r * (double)(uint32_t)(r >> 32) <= DM_SCR_MAX_PX, "the fixed width %d at 4:3 is above DM_SCR_MAX_PX", fw); } }
    printf("t_screen_mac: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
