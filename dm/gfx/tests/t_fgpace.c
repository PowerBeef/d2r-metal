// Frame starts under frame generation without a display link (nvn_fgpace.h): the rule, then the rule against a model of the screen
#include <stdio.h>
#include <stdlib.h>
#include "../nvn/nvn_fgpace.h"
static int bad;
static void near(const char *what, double got, double want) { if (fabs(got - want) > 1e-6) { printf("FAIL %s: got %.6f, want %.6f\n", what, got, want); bad = 1; } }
static void within(const char *what, double got, double lo, double hi) { if (got < lo - 1e-9 || got > hi + 1e-9) { printf("FAIL %s: got %.4f, want %.4f to %.4f\n", what, got, lo, hi); bad = 1; } }

// The screen: frame f's generated image shows at the first refresh at least `latch` after its GPU work ends and two refreshes after the last one's. The work ends `cost(f)` after the frame starts;
// the handler reports each image `report` frames later, as real ones are still on their way when the next frame starts
#define NF 6000
#define P (1.0 / 60)
#define LATCH 0.0165
typedef struct { int holds, drawn; double max_gap, t_end, lead_end, delay_end, delay_max_after; } sim_out;
static sim_out simulate(double (*cost)(int), double (*stall)(int), int settle) {
    static double T[NF], S[NF], plan[NF];
    fg_lead l = { .base = 0.05 }; double prev = 0, anchor = 0; uint64_t anchor_f = 0; sim_out o = { 0 };
    for (int f = 1; f < NF; f++) {
        double now = f > 1 ? S[f - 1] + 0.012 : 100;   // the game is ready this long after the last start, at the earliest
        // what has reached the screen by now
        static int seen; if (f == 1) seen = 0;
        while (seen + 1 < f && T[seen + 1] <= now) { seen++; anchor = T[seen]; anchor_f = seen; fg_lead_shown(&l, S[seen], plan[seen], T[seen], P); }
        fg_lead_tick(&l, now);
        double lead = fg_lead_now(&l), t = fg_pace_next(prev, anchor, anchor_f, f, now, P, lead, 0.25);
        S[f] = prev = t > now ? t : now; plan[f] = S[f] + lead;
        double ready = S[f] + cost(f) + (stall ? stall(f) : 0) + LATCH, r = ceil(ready / P) * P;
        T[f] = f > 1 && r < T[f - 1] + 2 * P ? T[f - 1] + 2 * P : r;
        if (f > settle) { double g = T[f] - T[f - 1]; if (g > 2 * P + 1e-6) o.holds++; if (g > o.max_gap) o.max_gap = g; if (T[f] - S[f] > o.delay_max_after) o.delay_max_after = T[f] - S[f]; }
    }
    o.drawn = NF; o.t_end = T[NF - 1]; o.lead_end = fg_lead_now(&l); o.delay_end = T[NF - 1] - S[NF - 1];
    return o;
}
static double light(int f) { (void)f; return 0.030; }
static double tight(int f) { (void)f; return 0.033; }
static double heavy(int f) { (void)f; return 0.040; }
static double very(int f) { (void)f; return 0.060; }
static double varies(int f) { return f / 2000 % 2 ? 0.040 : 0.028; }
static double hitch_once(int f) { return f == 1000 ? 0.200 : 0; }
static double hitch_some(int f) { return f % 700 == 0 ? 0.120 : 0; }
static double wobble(int f) { return 0.030 + 0.003 * ((f * 7919 % 100) / 100.0); }

int main(void) {
    const double p = P, lead = 0.05, age = 0.25;
    near("the first frame starts at once", fg_pace_next(0, 0, 0, 1, 10, p, lead, age), 10);
    near("no anchor: two refreshes later", fg_pace_next(10, 0, 0, 2, 10.001, p, lead, age), 10 + 2 * p);
    near("a stale anchor runs free", fg_pace_next(10, 9.5, 1, 2, 10.001, p, lead, age), 10 + 2 * p);
    near("on schedule", fg_pace_next(10, 10 + lead - 2 * p, 4, 6, 10.01, p, lead, age), 10 + 2 * p);
    near("an older anchor says the same", fg_pace_next(10, 10 + lead - 4 * p, 3, 6, 10.01, p, lead, age), 10 + 2 * p);
    near("the anchor a refresh late: a refresh later", fg_pace_next(10, 10 + lead - 2 * p + p, 4, 6, 10.01, p, lead, age), 10 + 3 * p);
    near("frames queued: the start waits for them", fg_pace_next(10, 10 + lead + 2 * p, 5, 6, 10.01, p, lead, age), 10 + 4 * p);
    near("never much sooner than two refreshes", fg_pace_next(10, 9.95, 2, 3, 10.01, p, lead, age), 10 + 1.5 * p);
    near("never a long wait", fg_pace_next(10, 10.5, 5, 6, 10.51, p, lead, age), 10 + 16 * p);

    fg_lead l = { .base = lead };
    near("no extra at first", fg_lead_now(&l), lead);
    fg_lead_shown(&l, 20, 20 + lead, 20 + lead + 0.001, p);
    near("a tiny lateness changes nothing", l.extra, 0);
    fg_lead_shown(&l, 20, 20 + lead, 20 + lead + p, p);
    near("a refresh late: a refresh more", l.extra, p);
    near("the floor sits just above the lead that missed", l.floor, FG_LEAD_MARGIN);
    fg_lead_shown(&l, 20 + 2 * p, 20 + 2 * p + lead, 20 + lead + 3 * p, p);
    near("a frame already in flight doesn't count again", l.extra, p);
    double t = l.retry_at; fg_lead_tick(&l, t - 0.5); near("not yet time for a step", l.extra, p);
    fg_lead_tick(&l, t); near("a step down, a quarter while that's more than the smallest", l.extra, p * 3 / 4);
    for (int i = 0; i < 6; i++) { t = l.retry_at; fg_lead_tick(&l, t); }
    near("it stops at the floor", l.extra, FG_LEAD_MARGIN);
    double hold0 = l.hold, st = l.changed + 0.01, pl = st + fg_lead_now(&l);
    fg_lead_shown(&l, st, pl, pl + p, p);
    near("missing again: a refresh more", l.extra, FG_LEAD_MARGIN + p);
    near("missing again: the floor rises", l.floor, 2 * FG_LEAD_MARGIN);
    near("missing again: the floor holds longer", l.hold, 2 * hold0);
    for (int i = 0; i < 8; i++) { t = l.retry_at; fg_lead_tick(&l, t); }
    near("still the floor", l.extra, 2 * FG_LEAD_MARGIN);
    t = l.floor_until + 0.01; l.retry_at = t; fg_lead_tick(&l, t);
    near("the floor's time is up: back to no extra", l.extra, 0);
    { fg_lead c = { .base = lead }; fg_lead_shown(&c, 21, 21 + lead, 21 + lead + 4 * p, p); near("a hitch asks for nothing", c.extra, 0); near("a hitch: no floor", c.floor, 0);
      fg_lead_shown(&c, 21 + 2 * p, 21 + 2 * p + lead, 21 + lead + 5 * p, p); near("a frame behind a hitch doesn't count", c.extra, 0);
      fg_lead_shown(&c, 22, 22 + lead, 22 + lead + 2 * p, p); fg_lead_shown(&c, 23, 23 + lead, 23 + lead + 2 * p, p); near("the extra has a ceiling", c.extra, 2 * p); }

    // Against the screen. All frames in about 100 s
    sim_out o = simulate(light, NULL, 20);
    if (o.holds) { printf("FAIL a light load held %d times\n", o.holds); bad = 1; }
    within("a light load: what a press takes", o.delay_max_after, 0.0499, 0.0501);
    o = simulate(wobble, NULL, 20);
    if (o.holds) { printf("FAIL a wobbling load inside the lead held %d times\n", o.holds); bad = 1; }
    o = simulate(tight, NULL, 20);
    if (o.holds > 0) { printf("FAIL a load just inside the lead held %d times\n", o.holds); bad = 1; }
    o = simulate(heavy, NULL, 400);
    if (o.holds > 6) { printf("FAIL a load past the lead, after settling: %d holds in 100 s\n", o.holds); bad = 1; }
    within("a load past the lead settles just above what it needs", o.delay_end, 0.0565, 0.0834);
    o = simulate(very, NULL, 400);
    if (o.holds > 6) { printf("FAIL a load near the frame: %d holds in 100 s\n", o.holds); bad = 1; }
    o = simulate(varies, NULL, 400);
    if (o.holds > 30) { printf("FAIL a load that comes and goes: %d holds in 100 s\n", o.holds); bad = 1; }
    o = simulate(light, hitch_once, 1100);
    if (o.holds) { printf("FAIL after a hitch: %d holds\n", o.holds); bad = 1; }
    within("after a hitch: back to what a press takes", o.delay_max_after, 0.0499, 0.0501);
    o = simulate(light, hitch_some, 20);
    within("hitches every 12 s: the end of the run", o.delay_end, 0.0499, 0.0501);
    if (!bad) printf("t_fgpace OK\n");
    return bad;
}
