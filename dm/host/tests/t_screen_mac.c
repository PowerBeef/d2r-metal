// The Mac App's screen geometry (dm_screen_mac.c): the picture's shape on a main screen, the Native output width and the game resolution.
#include <math.h>
#include <stdio.h>
#include "../dm_screen_mac.h"

static int n, fails;
#define CHECK(ok, ...) do { n++; if (!(ok)) { fails++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pic(double w, double h, double aspect, int full) {
    dm_scr_pic p = dm_scr_picture(w, h);
    CHECK(fabs(p.aspect - aspect) < 5e-5 && p.full == full, "dm_scr_picture(%.0f, %.0f) = aspect %.4f full %d, expected %.4f full %d", w, h, p.aspect, p.full, aspect, full);
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
    // the picture: 16:9 or wider keeps 16:9 throughout; narrower gets true full screen at the screen's own aspect
    pic(2560, 1440, 16.0 / 9, 0);
    pic(1512, 950, 1512 / 950.0, 1);
    pic(1440, 900, 1.6, 1);
    pic(1024, 768, 4.0 / 3, 1);
    pic(1280, 1024, 1.25, 1);
    pic(1440, 2560, 0.5625, 1);
    pic(3440, 1440, 16.0 / 9, 0);
    pic(1770, 1000, 16.0 / 9, 0);   // within 1% of 16:9
    pic(0, 0, 16.0 / 9, 0);
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
    printf("t_screen_mac: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
