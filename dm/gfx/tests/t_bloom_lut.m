// Bloom 1/4 and 3D brightening on a real Metal replay with closed-form expectations: background helper-kernel readiness and the LUT rewrite's output at k = 2.
// Built twice: t_bloom_lut (run under MTL_DEBUG_LAYER=1) and t_bloom_lut_asan (AddressSanitizer + UndefinedBehaviorSanitizer, the first report aborts).
#include "../nvn/nvn_mtl.m"
#include <math.h>
#include <pthread.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); count++; printf("%s want %llu got %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w == g ? "✓" : "✗"); fail += w != g; } while (0)
#define EXPECT_NEAR(name, want, got, tol) do { double w = (want), g = (got); count++; printf("%s want %.4f got %.4f %s\n", name, w, g, fabs(w - g) <= (tol) ? "✓" : "✗"); fail += !(fabs(w - g) <= (tol)); } while (0)
static void commit(void) { uint64_t f = mtl_sub_commit(); EXPECT("submit done", 1, mtl_wait(f, 5000000000ull)); }

// The helper kernels are compiled on a background queue that mtl_init started; the first wait returns once they are built
static void aux_case(void) {
    EXPECT("helper kernels: ready after the wait", 1, aux_wait());
    os_unfair_lock_lock(&g_aux_lk);
    EXPECT("helper kernels: the LUT kernel is built", 1, g_aux.bright_ps != nil);
    EXPECT("helper kernels: the bloom capture library and sampler are built", 1, g_aux.bloom_lib != nil && g_aux.bloom_samp != nil);
    EXPECT("helper kernels: the bloom capture pipeline is prebuilt for RG11B10Float", 1, g_aux.bloom_fmt == MTLPixelFormatRG11B10Float && g_aux.bloom_pso != nil);
    os_unfair_lock_unlock(&g_aux_lk);
}

// The LUT rewrite on a 4x4x4 identity table: each cell (x, y, z) holds ((x + .5) / 4, (y + .5) / 4, (z + .5) / 4). At k = 2 the closed form is
// u2 = clamp(PQ(2 PQ^-1(u)), 0.125, 0.875) (a trilinear sample of the identity table), s = smoothstep(0.65, 1, luma(u2)), out = max(mix(u2, u, s), u),
// computed in double precision; the tolerance covers half-float storage and the GPU's filter-weight precision. The off-diagonal cells exercise the
// sample path and the cross-channel highlight mix
static id<MTLBuffer> lut_upload(const tex_t *t) {
    id<MTLBuffer> up = [g_dev newBufferWithLength:4 * 4 * 4 * 8 options:MTLResourceStorageModeShared]; __fp16 *h = up.contents;
    for (int z = 0; z < 4; z++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { __fp16 *c = h + 4 * (x + 4 * y + 16 * z); c[0] = (x + .5f) / 4; c[1] = (y + .5f) / 4; c[2] = (z + .5f) / 4; c[3] = 1; }
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:32 sourceBytesPerImage:128 sourceSize:MTLSizeMake(4, 4, 4) toTexture:tex_mtl(t) destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; return up;
}
static void lut_read(const tex_t *t, __fp16 out[256]) {
    id<MTLBuffer> dn = [g_dev newBufferWithLength:4 * 4 * 4 * 8 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:tex_mtl(t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 4, 4) toBuffer:dn destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:128];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; memcpy(out, dn.contents, 512);
}
static void lut_run(float k) {
    NvShader *cs = [NvShader new]; cs->stage = 5; snprintf(cs->key, sizeof cs->key, "552f45c348a61b86_t"); cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .reg = 0};
    g_bright_fixed = k; state_reset(); memset(&R, 0, sizeof R); S.img[5][0] = 1701; mtl_sub_begin(); bright_lut(cs); commit();
}
static void cell_expect(const __fp16 *px, int x, int y, int z, double r, double g, double b) {
    const __fp16 *c = px + 4 * (x + 4 * y + 16 * z); char n[96];
    snprintf(n, sizeof n, "LUT k=2: cell (%d,%d,%d) red", x, y, z); EXPECT_NEAR(n, r, (double)c[0], 0.003);
    snprintf(n, sizeof n, "LUT k=2: cell (%d,%d,%d) green", x, y, z); EXPECT_NEAR(n, g, (double)c[1], 0.003);
    snprintf(n, sizeof n, "LUT k=2: cell (%d,%d,%d) blue", x, y, z); EXPECT_NEAR(n, b, (double)c[2], 0.003);
}
static void bright_case(void) {
    tex_t t = {.serial = 9700}; t.d.width = t.d.height = t.d.depth = 4; t.d.samples = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_3D; t.d.format = NVN_FORMAT_RGBA16F; t.d.flags = NVN_TEXTURE_FLAGS_IMAGE;
    mtl_reg_tex(1701, &t, NULL); __fp16 px[256];
    lut_upload(&t); lut_run(2.0f); lut_read(&t, px);
    cell_expect(px, 0, 0, 0, 0.1589, 0.1589, 0.1589); cell_expect(px, 1, 1, 1, 0.4386, 0.4386, 0.4386); cell_expect(px, 2, 2, 2, 0.6954, 0.6954, 0.6954); cell_expect(px, 3, 3, 3, 0.8750, 0.8750, 0.8750);
    cell_expect(px, 3, 3, 0, 0.8750, 0.8750, 0.1422);   // blue pulled back by the luma of the other two (s 0.493)
    cell_expect(px, 1, 3, 0, 0.4301, 0.8750, 0.1543);   // s 0.134
    // No-dispatch control: at k = 1 bright_lut returns at the k check, so the cells keep their values. This proves only that k ~= 1 leaves the table alone
    lut_upload(&t); lut_run(1.0f); lut_read(&t, px);
    EXPECT_NEAR("LUT k=1 (no-dispatch control): cell (0,0,0) red", 0.125, (double)px[0], 0.001);
    EXPECT_NEAR("LUT k=1 (no-dispatch control): cell (3,3,3) green", 0.875, (double)px[4 * 63 + 1], 0.001);
    g_bright_fixed = -1;
}
int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();
    aux_case(); bright_case();
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0; } }
