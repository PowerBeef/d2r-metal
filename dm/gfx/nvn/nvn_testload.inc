// Known work on the GPU, the control for measurements: DM_TEST_LOAD=gpu:<n> runs a compute pass of 1M threads of n math steps at every compose draw, gspike:<every>:<n> only at every
// <every>-th one. bench.py headroom raises gpu:<n> until frames start to miss, which is the only way to read free GPU time when the GPU lowers its clock to fill the frame; bench_why.py
// has to name the GPU for the spikes. Replay thread, at the compose draw. The CPU's half is in hle_nvn.c (dm_test_load_cpu). Off and one load when unset
static struct { int parsed, n, spike_every, spike_n; uint64_t count, written; __strong id<MTLComputePipelineState> pso; __strong id<MTLBuffer> buf; } g_tl;
static void test_load_gpu(void) {
    if (!g_tl.parsed) { g_tl.parsed = 1; const char *e = getenv("DM_TEST_LOAD"), *p;
        if (e && (p = strstr(e, "gpu:"))) g_tl.n = atoi(p + 4);
        if (e && (p = strstr(e, "gspike:"))) sscanf(p + 7, "%d:%d", &g_tl.spike_every, &g_tl.spike_n); }
    if (!g_tl.n && !g_tl.spike_every) return;
    int n = g_tl.n; g_tl.count++; if (g_tl.spike_every && g_tl.count % (uint64_t)g_tl.spike_every == 0) n += g_tl.spike_n;
    if (!n) return;
    if (!g_tl.pso) {
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void burn(uint3 id [[thread_position_in_grid]], device float *out [[buffer(0)]], device const uint *n [[buffer(1)]]) {\n"
            "  float x = float(id.x ^ id.y) * 0.001; uint c = n[0]; for (uint i = 0; i < c; i++) x = sin(x) * 1.01 + 0.1; out[(id.x + id.y * 64) % 4096] = x; }\n" options:nil error:&e];
        id<MTLFunction> fn = [lib newFunctionWithName:@"burn"]; g_tl.pso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        g_tl.buf = [g_dev newBufferWithLength:4096 * 4 + 16384 options:MTLResourceStorageModeShared];
        if (!g_tl.pso || !g_tl.buf) { MLOG("【红】DM_TEST_LOAD: no pipeline (%s)", e.localizedDescription.UTF8String); g_tl.n = g_tl.spike_every = 0; return; }
        res_add(g_tl.buf); res_commit(); MLOG("DM_TEST_LOAD: GPU load %d, spike %d every %d frames", g_tl.n, g_tl.spike_n, g_tl.spike_every);
    }
    // a slot per frame in flight: the next frame's count must not reach back into one the GPU hasn't run yet
    uint64_t slot = g_tl.written++ % 64; ((uint32_t *)g_tl.buf.contents)[4096 + slot * 64] = (uint32_t)n;
    rp_end();
    CENC e = enc_c_begin(NULL); enc_c_pso(e, g_tl.pso); enc_c_args(e, g_tl.buf.gpuAddress, 0); enc_c_args(e, g_tl.buf.gpuAddress + (4096 + slot * 64) * 4, 1);
    enc_c_dispatch(e, MTLSizeMake(64, 64, 1), MTLSizeMake(16, 16, 1)); m4_end(e);
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
}
