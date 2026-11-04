// Rate-limited error lines (nvn/nvn_ratelog.h): the first N of a kind print in full, then at most one summary per period, and a flush reports whatever a quiet
// window left uncounted, so every error is counted exactly once. Built with ThreadSanitizer: the concurrent case calls it from four threads at once, as the
// GPU feedback handlers of several queues do; a race report exits non-zero. Expectations are worked out by hand from the rules in the header.
#include "../nvn/nvn_ratelog.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", why); } while (0)
#define S 1000000000ull
static const uint64_t P = 10 * S;   // the period nvn_mtl.m uses

static nvn_ratelog g_c;
static _Atomic int g_full, g_sum;
static void *burst(void *p) {
    (void)p; uint64_t more, total;
    for (int i = 0; i < 10000; i++) { int t = nvn_ratelog_take(&g_c, 8, P, 0, &more, &total); if (t == NVN_RL_FULL) atomic_fetch_add(&g_full, 1); else if (t == NVN_RL_SUMMARY) atomic_fetch_add(&g_sum, 1); }
    return NULL;
}

int main(void) {
    nvn_ratelog r; memset(&r, 0, sizeof r); uint64_t more = 0, total = 0; int full = 1;
    for (int i = 1; i <= 8; i++) full &= nvn_ratelog_take(&r, 8, P, (uint64_t)(i - 1) * S, &more, &total) == NVN_RL_FULL && total == (uint64_t)i;
    CHECK(full, "calls 1-8 (0-7 s) print in full, each with its running total");
    CHECK(nvn_ratelog_take(&r, 8, P, 8 * S, &more, &total) == NVN_RL_QUIET, "call 9 at 8 s: quiet (within 10 s of the last line)");
    CHECK(nvn_ratelog_take(&r, 8, P, 16900000000ull, &more, &total) == NVN_RL_QUIET, "call 10 at 16.9 s: quiet");
    CHECK(nvn_ratelog_take(&r, 8, P, 17 * S, &more, &total) == NVN_RL_SUMMARY && more == 3 && total == 11, "call 11 at 17.0 s: a summary of 3 more, 11 in all");
    CHECK(nvn_ratelog_take(&r, 8, P, 17500000000ull, &more, &total) == NVN_RL_QUIET, "call 12 at 17.5 s: quiet");
    CHECK(nvn_ratelog_take(&r, 8, P, 27 * S, &more, &total) == NVN_RL_SUMMARY && more == 2, "call 13 at 27.0 s: a summary of 2 more");
    CHECK(nvn_ratelog_take(&r, 8, P, 26 * S, &more, &total) == NVN_RL_QUIET, "call 14 at 26.0 s (a clock behind the last line): quiet");
    CHECK(nvn_ratelog_flush(&r, &more, &total) == 1 && more == 1 && total == 14, "flush: the 1 uncounted error, 14 in all");
    CHECK(nvn_ratelog_flush(&r, &more, &total) == 0, "a second flush has nothing to report");

    nvn_ratelog v; memset(&v, 0, sizeof v);   // innocent victims: one full line, then summaries
    CHECK(nvn_ratelog_take(&v, 1, P, 0, &more, &total) == NVN_RL_FULL, "victim call 1: full");
    CHECK(nvn_ratelog_take(&v, 1, P, 1 * S, &more, &total) == NVN_RL_QUIET, "victim call 2 at 1 s: quiet");
    CHECK(nvn_ratelog_take(&v, 1, P, 11 * S, &more, &total) == NVN_RL_SUMMARY && more == 2, "victim call 3 at 11 s: a summary of 2 more");

    nvn_ratelog q; memset(&q, 0, sizeof q); int nf = 0, nq = 0;   // a storm that stops inside a quiet window
    for (int i = 0; i < 9; i++) { int t = nvn_ratelog_take(&q, 8, P, (uint64_t)i * S, &more, &total); nf += t == NVN_RL_FULL; nq += t == NVN_RL_QUIET; }
    CHECK(nf == 8 && nq == 1, "quiet tail: 8 full, the 9th quiet");
    CHECK(nvn_ratelog_flush(&q, &more, &total) == 1 && more == 1 && total == 9, "quiet tail: the flush reports the 9th");

    memset(&g_c, 0, sizeof g_c);   // four threads at once, all at the same instant
    pthread_t th[4]; for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, burst, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(atomic_load(&g_full) == 8 && atomic_load(&g_sum) == 0, "concurrent: exactly 8 full lines, no summary within the period");
    CHECK(atomic_load(&g_c.n) == 40000, "concurrent: every call counted");
    CHECK(nvn_ratelog_flush(&g_c, &more, &total) == 1 && more == 39992, "concurrent: one flush reports the other 39992");
    printf("t_ratelog: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
