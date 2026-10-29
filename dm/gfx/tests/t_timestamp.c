// NVN raw timestamp units (nvn_timestamp.h): known vectors, round trips over long uptimes, and how the game reads a report.
// The game's GPU profiler divides report differences by the queue's timestamp rate, 614,400,000 per second, so a 16 ms GPU
// span must read 16 ms; the old 19.2 MHz reports read 32 times too short. Adapted from nxnative's t_timestamp.c (MIT, (c) 2026 seedds).
#include "../nvn/nvn_timestamp.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", why); } } while (0)

static double game_ms(uint64_t ticks) { return (double)ticks * 1000.0 / 614400000.0; }   // the game's own conversion rate

int main(void) {
    const struct { uint64_t ns, ticks; } exact[] = {
        {0, 0}, {625, 384}, {1000000, 614400}, {16000000, 9830400}, {1000000000, 614400000},
        {UINT64_C(5000000000000000000), UINT64_C(3072000000000000000)},
    };
    for (unsigned i = 0; i < sizeof exact / sizeof *exact; i++) {
        CHECK(nvn_timestamp_from_ns(exact[i].ns) == exact[i].ticks, "known ns -> 614.4 MHz ticks");
        CHECK(nvn_timestamp_to_ns(exact[i].ticks) == exact[i].ns, "known raw ticks -> ns");
    }
    CHECK(nvn_timestamp_from_ns(1) == 0 && nvn_timestamp_from_ns(2) == 1, "ns -> ticks rounds down");
    CHECK(nvn_timestamp_to_ns(1) == 1 && nvn_timestamp_to_ns(2) == 3, "ticks -> ns rounds down");
    const uint64_t ns[] = {1, 2, 624, 626, 16666667, UINT64_C(1234567890123456789), UINT64_MAX};
    for (unsigned i = 0; i < sizeof ns / sizeof *ns; i++) {
        uint64_t back = nvn_timestamp_to_ns(nvn_timestamp_from_ns(ns[i]));
        CHECK(back <= ns[i] && ns[i] - back <= 2, "ns round trip loses at most 2 ns, including long uptimes");
    }
    uint64_t t0 = nvn_timestamp_from_ns(UINT64_C(3600000000000)), t1 = nvn_timestamp_from_ns(UINT64_C(3600016000000));
    CHECK(game_ms(t1 - t0) > 15.999 && game_ms(t1 - t0) < 16.001, "a 16 ms GPU span reads 16 ms at the game's rate");
    CHECK(game_ms(UINT64_C(16000000) * 12 / 625) == 0.5, "the old 19.2 MHz report of 16 ms read as 0.5 ms");
    printf("timestamp: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
