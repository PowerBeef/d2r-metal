// Rate-limited log lines for error storms (nvn_mtl.m: GPU errors of the main, upload and present command buffers). Each kind keeps three counters: events seen,
// the time of its last printed line, and events reported so far. The first `first` events print in full; after that at most one summary line per period reports
// how many came since the last line; a flush (called periodically and when the GPU is drained) reports whatever a quiet window left behind, so a storm that stops
// still has every error counted exactly once. Lock-free: the feedback handlers of several queues may call it at once (t_ratelog runs it under ThreadSanitizer).
#pragma once
#include <stdatomic.h>
#include <stdint.h>

typedef struct { _Atomic uint64_t n, at_ns, n_at; } nvn_ratelog;   // n: events; at_ns: when the last line printed; n_at: events reported (only ever grows)
enum { NVN_RL_QUIET, NVN_RL_FULL, NVN_RL_SUMMARY };

// Raises n_at to n; returns how many events that reports (0 when another caller already reported past n). The counted intervals never overlap
static inline uint64_t nvn_rl_raise(nvn_ratelog *r, uint64_t n) {
    uint64_t was = atomic_load(&r->n_at);
    while (was < n && !atomic_compare_exchange_weak(&r->n_at, &was, n)) {}
    return was < n ? n - was : 0;
}
// One event at now_ns: FULL (print it in full), SUMMARY (print *more events since the last line) or QUIET (print nothing); *total = events so far
static inline int nvn_ratelog_take(nvn_ratelog *r, uint64_t first, uint64_t period_ns, uint64_t now_ns, uint64_t *more, uint64_t *total) {
    uint64_t n = atomic_fetch_add(&r->n, 1) + 1; *total = n; *more = 0;
    if (n <= first) { atomic_store(&r->at_ns, now_ns); nvn_rl_raise(r, n); return NVN_RL_FULL; }
    uint64_t at = atomic_load(&r->at_ns);
    if (now_ns < at + period_ns || !atomic_compare_exchange_strong(&r->at_ns, &at, now_ns)) return NVN_RL_QUIET;
    *more = nvn_rl_raise(r, n);
    return *more ? NVN_RL_SUMMARY : NVN_RL_QUIET;
}
// Reports the events no line has counted yet: returns 1 with *more of them (and *total events in all), or 0 when there are none
static inline int nvn_ratelog_flush(nvn_ratelog *r, uint64_t *more, uint64_t *total) {
    *total = atomic_load(&r->n); *more = nvn_rl_raise(r, *total);
    return *more != 0;
}
