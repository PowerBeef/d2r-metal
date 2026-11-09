// GPU watchdog (nvn_mtl.m gpu_watch, g_gpu_lost): with work waiting and none finished for DM_GPU_LOST_S seconds, the GPU is taken as lost, every wait
// on it returns and nothing more is sent to it. The stall is the control port's (mtl_test_gpu_stall): submissions vanish, as they did in the driver
// that stopped taking a Mac's submissions on 2026-11-08. Checks: an idle GPU is never taken as lost, a waiting submission is, after the limit and not
// long before; the fence wait returns; the front end is called once; later submissions finish without the GPU; drain and finish return at once
#define NVN_OWN_FRONT_GPU_LOST   // front_gpu_lost below counts the calls
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static _Atomic int g_front_calls;
void front_gpu_lost(void) { atomic_fetch_add(&g_front_calls, 1); }
static int fail, count;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); count++; printf("%s: want %lld, got %lld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); fail += w_ != g_; } while (0)
static uint64_t submit_empty(void) {   // an empty submission, the way nvn_replay.m runs one; not waited for
    static const op_hdr end = {OP_END, 8}; uint64_t eh = (uint64_t)(uintptr_t)&end;
    mtl_sub_begin(); mtl_sub_index(&eh, 1); mtl_exec_rest(NULL, 0); mtl_exec((const uint8_t *)&end); mtl_exec_rest(NULL, 0);
    return mtl_sub_commit();
}
static double secs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
int main(void) {
    @autoreleasepool {
        setenv("DM_GPU_LOST_S", "1", 1);
        mtl_init();
        uint64_t f = submit_empty();
        EXPECT("a normal submission finishes", 1, mtl_wait(f, 5000000000ull));
        usleep(1600 * 1000);   // idle past the limit: nothing waits, so nothing is lost
        EXPECT("an idle GPU is not taken as lost", 0, mtl_gpu_lost());
        mtl_test_gpu_stall();
        double t0 = secs(); uint64_t s = submit_empty();
        EXPECT("the vanished submission has not finished", 0, mtl_wait(s, 0));
        EXPECT("the fence wait returns once the GPU is lost", 1, mtl_wait(s, 10000000000ull));
        double dt = secs() - t0;
        printf("taken as lost after %.2f s (limit 1 s, polled every 0.25 s)\n", dt);
        EXPECT("not before the limit", 1, dt >= 0.95);
        EXPECT("not long after it", 1, dt < 2.0);
        EXPECT("the GPU counts as lost", 1, mtl_gpu_lost());
        usleep(100 * 1000);
        EXPECT("the front end was told once", 1, atomic_load(&g_front_calls));
        uint64_t s2 = submit_empty();
        EXPECT("a later submission finishes without the GPU", 1, mtl_wait(s2, 2000000000ull));
        for (int i = 0; i < 200; i++) { pthread_mutex_lock(&g_mu); uint64_t d = g_done; pthread_mutex_unlock(&g_mu); if (d >= s2) break; usleep(10 * 1000); }
        pthread_mutex_lock(&g_mu); uint64_t d = g_done; pthread_mutex_unlock(&g_mu);
        EXPECT("its feedback ran (the fence counts as done)", 1, d >= s2);
        t0 = secs(); mtl_drain(); mtl_finish();
        EXPECT("drain and finish return at once", 1, secs() - t0 < 0.5);
        usleep(600 * 1000);
        EXPECT("the front end is told only once", 1, atomic_load(&g_front_calls));
    }
    printf("%s %d/%d\n", fail ? "FAIL" : "OK", count - fail, count);
    return fail != 0;
}
