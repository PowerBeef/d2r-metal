// Hang recorder (nvn_mtl.m hr_dump, hr_slow, the pass marks): a submission whose middle block keeps the GPU busy for about 400 ms stands in for a hung one.
// Read while it runs, the record must say what a hang's would: the block before it finished and the busy block is the first that did not; the note for
// work encoding its own passes (hr_mark) is listed, and the block after it is not finished either. This test is also where it was found that the GPU
// writes a pass's timestamps only once its work has run: the busy block's start mark reads 0 while it runs. Once it finishes, its feedback names it as
// the longest pass of a slow submission. Also: with nothing waiting there is no record to give, and the queue log has the submission. No GPU hang is
// made: the busy block is an ordinary kernel that loops, well under the driver's own limit (about 4 s)
#define NVN_OWN_FRONT_GPU_LOST   // the watchdog stays quiet (DM_GPU_LOST_S below), but the front end hook must exist
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void front_gpu_lost(void) {}
static int fail, count;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); count++; printf("%s: want %lld, got %lld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); fail += w_ != g_; } while (0)
static id<MTLComputePipelineState> g_spin; static id<MTL4ArgumentTable> g_at; static id<MTLBuffer> g_buf;
// Each block's three words at its own offset: result, seed, loop count. The table's address is taken at each dispatch
static void spin_block(CENC ce, int k, const char *label, uint32_t n) {
    uint32_t *w = (uint32_t *)((uint8_t *)g_buf.contents + 64 * k); w[1] = 12345u + (uint32_t)k; w[2] = n;
    pg_host_begin(ce, label); [g_at setAddress:g_buf.gpuAddress + 64 * k atIndex:0]; [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; pg_host_end(ce);
}
static CENC open_enc(void) {
    CENC ce = [(CBUF)g_cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [ce setArgumentTable:g_at]; [ce setComputePipelineState:g_spin]; return ce;
}
// One submission: "quick", then "spin" (n loops), in one encoder; a note; then "tail" in a second encoder, which starts after the first one's end barrier
static uint64_t submit_spin(uint32_t n) {
    static const op_hdr end = {OP_END, 8}; uint64_t eh = (uint64_t)(uintptr_t)&end;
    mtl_sub_begin(); mtl_sub_index(&eh, 1); mtl_exec_rest(NULL, 0); mtl_exec((const uint8_t *)&end); mtl_exec_rest(NULL, 0);
    CENC ce = open_enc(); spin_block(ce, 0, "quick", 1000);
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    spin_block(ce, 1, "spin", n); m4_end(ce);
    hr_mark("mark");
    CENC c2 = open_enc(); spin_block(c2, 2, "tail", 1000); m4_end(c2);
    return mtl_sub_commit();
}
static double secs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
int main(void) {
    @autoreleasepool {
        setenv("DM_HANGREC_SLOW_MS", "150", 1); setenv("DM_GPU_LOST_S", "30", 1); setenv("DM_HANGREC_EARLY_S", "30", 1);
        mtl_init();
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                              "kernel void spin(device uint *o [[buffer(0)]]) { uint x = o[1], n = o[2]; for (uint i = 0; i < n; i++) x = x * 1664525u + 1013904223u; o[0] = x; }"
                                                  options:nil error:&e];
        g_spin = lib ? [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&e] : nil;
        if (!g_spin) { printf("FAIL: the spin kernel: %s\n", e.localizedDescription.UTF8String); return 1; }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1;
        g_at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        g_buf = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(g_buf);
        hr_verdict v;
        hr_dump("test: nothing waiting", &v);
        EXPECT("nothing waiting: no record to give", 0, v.known);
        // calibrate the loop count to about 400 ms of GPU time (a short one: well under DM_HANGREC_SLOW_MS)
        uint32_t n0 = 2000000; double t0 = secs(); uint64_t f = submit_spin(n0);
        EXPECT("the calibration submission finishes", 1, mtl_wait(f, 5000000000ull));
        double dt = secs() - t0; uint32_t n = (uint32_t)fmin(4.0e9, n0 * 0.4 / fmax(dt, 1e-4));
        printf("calibration: %u loops in %.1f ms, so %u for about 400 ms\n", n0, dt * 1e3, n);
        uint64_t slow0 = atomic_load(&g_hr_slow_n);
        t0 = secs(); f = submit_spin(n);
        usleep(150 * 1000);
        hr_dump("test: a submission still on the GPU", &v);
        int running = !mtl_wait(f, 0);
        EXPECT("read while the busy block still runs", 1, running);
        EXPECT("the record is there", 1, v.known);
        EXPECT("three passes", 3, v.passes);
        EXPECT("and one note", 1, v.notes);
        EXPECT("one finished (quick)", 1, v.finished);
        EXPECT("the first that did not is the busy block", 1, !strcmp(v.first_open, "spin"));
        EXPECT("the busy submission finishes", 1, mtl_wait(f, 5000000000ull));
        printf("it took %.0f ms\n", (secs() - t0) * 1e3);
        for (int i = 0; i < 100 && atomic_load(&g_hr_slow_n) == slow0; i++) usleep(10 * 1000);   // its feedback runs on Metal's queue
        EXPECT("its feedback reported a slow submission", 1, atomic_load(&g_hr_slow_n) - slow0);
        EXPECT("naming the busy block as its longest pass", 1, !strcmp(g_hr_slow_top, "spin"));
        os_unfair_lock_lock(&g_hq_lk); uint64_t nq = g_hq_n; hq_ent last = g_hq[(nq + HQ_N - 1) % HQ_N]; os_unfair_lock_unlock(&g_hq_lk);
        EXPECT("the queue log's last entry is that main submission", 1, nq && last.kind == HQ_MAIN && last.id == f);
        hr_dump("test: nothing waiting again", &v);
        EXPECT("then nothing waits again", 0, v.known);
    }
    printf("%s %d/%d\n", fail ? "FAIL" : "OK", count - fail, count);
    return fail != 0;
}
