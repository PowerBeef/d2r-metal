// When a frame starts under frame generation without a display link (DM_PRESENT_LINK=0): pure functions, unit test tests/t_fgpace.c.
// A frame's generated image reaches the screen at T, and T - start is what a press takes to show from that frame on. Two refreshes later the next one is due, however early its work ends, so
// T - start never comes down by itself: frames queued behind a hitch stay queued for good if every frame just starts two refreshes after the last. So frames don't count from the last start but
// from the screen: the last generated image's real time (the presented handler), plus two refreshes per frame since, minus the lead. Frames queued behind a hitch simply wait for the queue to run
// out, unseen, and a late image moves the schedule with it.
#pragma once
#include <math.h>
#include <stdint.h>

// The start of frame f. prev: the previous start (s, 0 = none). anchor: when frame anchor_f's generated image reached the screen (s, 0 = none). p: one shown frame (s). lead: from a frame's start to
// its generated image's refresh (s). max_age: an older anchor is no use (a pause, a run nobody shows): the start then runs free, two refreshes after the last
static inline double fg_pace_next(double prev, double anchor, uint64_t anchor_f, uint64_t f, double now, double p, double lead, double max_age) {
    if (prev <= 0) return now;
    double want = prev + 2 * p;
    if (anchor > 0 && now - anchor < max_age && f > anchor_f) {
        want = anchor + (double)(f - anchor_f) * 2 * p - lead;
        if (want < prev + 1.5 * p) want = prev + 1.5 * p;
        if (want > prev + 16 * p) want = prev + 16 * p;
    }
    return want;
}

// The lead: its base, plus what late images have shown this place takes. The next start then comes when it would have, and nothing but that one image shows it (a hold of a refresh); starting later
// would make every later image late too. The extra comes back down in steps: each makes the next start a little later, unseen, and only a step too far shows, as one more hold. A miss marks the lead
// just above it as a floor for a while, longer each time, so a scene that needs a longer lead settles there instead of missing over and over. More than FG_LEAD_HITCH refreshes late is a hitch (a
// stall, a loading screen), which says nothing about the lead.
typedef struct {
    double base, extra;           // s
    double changed;               // when the lead last changed or a hitch ended: frames started before that have an out-of-date plan
    double retry_at;              // the next step down
    double floor, floor_until;    // the extra stays at floor or above until then
    double hold;                  // how long the last floor held
} fg_lead;
#define FG_LEAD_STEP 0.004        // s, the smallest step down
#define FG_LEAD_WAIT 1.0          // s between steps
#define FG_LEAD_MARGIN 0.002      // s, the floor above the lead that missed
#define FG_LEAD_HOLD0 10.0
#define FG_LEAD_HOLDMAX 320.0
#define FG_LEAD_HITCH 2.5
static inline double fg_lead_now(const fg_lead *l) { return l->base + l->extra; }
// A generated image of a frame started at `start` and planned for `plan` reached the screen at `shown`. p: one shown frame
static inline void fg_lead_shown(fg_lead *l, double start, double plan, double shown, double p) {
    double late = shown - plan;
    if (start < l->changed || late <= p / 4) return;
    l->changed = shown;
    if (late > FG_LEAD_HITCH * p) return;
    l->hold = l->hold > 0 && shown < l->floor_until + l->hold ? (l->hold * 2 > FG_LEAD_HOLDMAX ? FG_LEAD_HOLDMAX : l->hold * 2) : FG_LEAD_HOLD0;
    l->floor = l->extra + FG_LEAD_MARGIN; l->floor_until = shown + l->hold;
    l->extra += late; if (l->extra > 2 * p) l->extra = 2 * p;
    if (l->floor > l->extra) l->floor = l->extra;
    l->retry_at = shown + FG_LEAD_WAIT;
}
// At each frame start: a step down, if it's time
static inline void fg_lead_tick(fg_lead *l, double now) {
    if (l->extra <= 0 || now < l->retry_at) return;
    double f = now < l->floor_until ? l->floor : 0, st = l->extra / 4 > FG_LEAD_STEP ? l->extra / 4 : FG_LEAD_STEP, e = l->extra - st; if (e < f) e = f; if (e < 0) e = 0;
    l->retry_at = now + FG_LEAD_WAIT;
    if (e < l->extra) { l->extra = e; l->changed = now; }
}
