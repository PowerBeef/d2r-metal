// Bloom 1/4 and 3D brightening on a real Metal replay with closed-form expectations: the full A→B→C→D chain, every abort and mid-chain reset leaving B0 black,
// the cutoff viewport guard, a broken capture pipeline falling back to the game's chain, the health summary's windows, background helper-kernel readiness,
// the LUT rewrite's output at k = 2 and the exposure k the magenta probe reads.
// Built twice: t_bloom_lut (run under MTL_DEBUG_LAYER=1) and t_bloom_lut_asan (AddressSanitizer + UndefinedBehaviorSanitizer, the first report aborts).
#include "../nvn/nvn_mtl.m"
#include <math.h>
#include <stdarg.h>
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
typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(void) { chain c = {0}; if (posix_memalign((void **)&c.p, getpagesize(), 65536)) abort(); memset(c.p, 0, 65536); mtl_pool_add(c.p, 65536); return c; }
static void emit(chain *c, unsigned op, const void *a, size_t n) { size_t z = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)z}; memcpy(c->p + c->n, &h, 8); if (n) memcpy(c->p + c->n + 8, a, n); c->n += z; }
static uint8_t *pool_mem(size_t n) { uint8_t *m; if (posix_memalign((void **)&m, getpagesize(), n)) abort(); memset(m, 0, n); mtl_pool_add(m, n); return m; }
static tex_t tex(unsigned w, int fmt) { static uint64_t s = 9000; tex_t t = {.serial = s++}; t.d.width = t.d.height = w; t.d.depth = t.d.samples = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = fmt; return t; }
// A w x w target with a matching viewport and scissor
static void set_rt_sz(chain *c, const tex_t *a, int w) { p_set_rt r = {.n = 1}; r.color[0] = a; emit(c, OP_SET_RT, &r, sizeof r); p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = w; emit(c, OP_VIEWPORTS, &v, sizeof v); p_scissor sc = {0, 0, w, w}; emit(c, OP_SCISSOR, &sc, sizeof sc); }
static void viewport(chain *c, float x, float y, float w, float h) { p_viewports v = {.n = 1}; v.v[0][0] = x; v.v[0][1] = y; v.v[0][2] = w; v.v[0][3] = h; emit(c, OP_VIEWPORTS, &v, sizeof v); }
static void clear_color(chain *c, float r, float g, float b, float a) { p_clear_color x = {.index = 0, .mask = 15, .rgba = {r, g, b, a}}; emit(c, OP_CLEAR_COLOR, &x, sizeof x); }
static void bind_prog(chain *c, prog_t *p) { p_bind_program b = {p, 3}; emit(c, OP_BIND_PROGRAM, &b, sizeof b); }
static void bind_tex(chain *c, int id) { p_bind_handles b = {.stage = 1, .first = 0, .n = 1}; b.h[0] = id; emit(c, OP_BIND_TEX, &b, sizeof b); }
static float *g_ub;   // the cutoff's c7: threshold 0.5
static void bind_ubo7(chain *c) { p_bind_buf u = {1, 7, (uint64_t)g_ub, 256}; emit(c, OP_BIND_UBO, &u, sizeof u); }
static void draw_cmd(chain *c) { p_draw d = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3}; emit(c, OP_DRAW, &d, sizeof d); }
static void ds_off(chain *c) { ds_st d = {0, 0, NVN_DEPTH_FUNC_ALWAYS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP}; p_bind_state x = {.kind = 5}; memcpy(x.bytes, &d, sizeof d); emit(c, OP_BIND_STATE, &x, sizeof x); }
// One chain run as part of a submit: first = 1 begins the submit (the state between chains carries over, as within one command buffer)
static void run(chain *c, int first) { emit(c, OP_END, NULL, 0); if (first) { state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); } uint64_t hh = (uint64_t)c->p; mtl_sub_index(&hh, 1); mtl_exec(c->p); }
static void readback(const tex_t *t, uint8_t out[256]) {
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); id<MTLBuffer> b = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t->d.width, t->d.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4 * t->d.width destinationBytesPerImage:4 * t->d.width * t->d.height];
    commit(); memcpy(out, b.contents, 4 * t->d.width * t->d.height); }
static int count_px(const uint8_t px[256], uint8_t r, uint8_t g, uint8_t b) { int n = 0; for (int i = 0; i < 64; i++) n += abs(px[4 * i] - r) <= 2 && abs(px[4 * i + 1] - g) <= 2 && abs(px[4 * i + 2] - b) <= 2; return n; }
// Program objects stay alive for the whole run (the pipeline cache is keyed by object address)
static NvProg *t_keep[32]; static int t_nkeep; static id<MTLLibrary> g_lib; static uint64_t g_pser = 8700;
static NvShader *shader(NSString *fn, int stage) { NvShader *s = [NvShader new]; s->fn = [g_lib newFunctionWithName:fn]; s->stage = stage; s->rf.pos_attr = -1; if (!s->fn) abort(); return s; }
static void prog(prog_t *pg, NvShader *vs, NvShader *fs) { memset(pg, 0, sizeof *pg); pg->serial = g_pser++; NvProg *p = [NvProg new]; p->sh[0] = vs; p->sh[1] = fs; if (t_nkeep < 32) t_keep[t_nkeep++] = p; serial_put(pg->serial, p); }
static void lib_init(void) { NSError *e = nil; g_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
    "vertex float4 v(uint n [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[n], 0, 1); }\n"
    "fragment float4 red() { return float4(1, 0, 0, 1); }\n"
    "fragment float4 blue() { return float4(0, 0, 1, 1); }\n" options:nil error:&e];
    if (!g_lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); abort(); } }
// The bloom chain's fragment shaders, identified by key; each draws red when it is really drawn
static NvShader *fs_key(const char *key, int ntex) { NvShader *s = shader(@"red", 1); s->rf.color_mask = 1; snprintf(s->key, sizeof s->key, "%s", key);
    if (ntex) { s->rf.nb = 1; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = 2}; } return s; }
enum { ID_S = 1601, ID_B0, ID_B1, ID_B2, ID_T, ID_F };
static tex_t tS, tB0, tB1, tB2, tT, tF;
static prog_t p_cut, p_down, p_up, p_apply, p_other, p_blue;
static void bloom_init(void) {
    g_ub = (float *)pool_mem(4096); g_ub[0] = 0.5f;
    tS = tex(8, NVN_FORMAT_RGBA8); tB0 = tex(8, NVN_FORMAT_RGBA8); tB1 = tex(4, NVN_FORMAT_RGBA8); tB2 = tex(2, NVN_FORMAT_RGBA8); tT = tex(8, NVN_FORMAT_RGBA8); tF = tex(8, NVN_FORMAT_RGBA8);
    mtl_reg_tex(ID_S, &tS, NULL); mtl_reg_tex(ID_B0, &tB0, NULL); mtl_reg_tex(ID_B1, &tB1, NULL); mtl_reg_tex(ID_B2, &tB2, NULL); mtl_reg_tex(ID_T, &tT, NULL); mtl_reg_tex(ID_F, &tF, NULL);
    NvShader *cut = fs_key("2938a5b71b8e21b2_t", 1); cut->rf.nb = 2; cut->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 7}; cut->rf.c1_len = 256; float c1[2] = {0.7152f, 0.0722f}; memcpy(cut->rf.c1, c1, sizeof c1);
    prog(&p_cut, shader(@"v", 0), cut); prog(&p_down, shader(@"v", 0), fs_key("019ce2f82edbda84_t", 1)); prog(&p_up, shader(@"v", 0), fs_key("cc264583f2000c81_t", 1));
    prog(&p_apply, shader(@"v", 0), fs_key("2e2d6700a2569795_t", 1)); prog(&p_other, shader(@"v", 0), fs_key("0123456789abcdef_t", 0));
    NvShader *blue = shader(@"blue", 1); blue->rf.color_mask = 1; snprintf(blue->key, sizeof blue->key, "fedcba9876543210_t"); prog(&p_blue, shader(@"v", 0), blue);
}
// One draw of program pg into a w x w target rt, sampling texture id tid (0 = none)
static void step(const tex_t *rt, int w, int tid, prog_t *pg, int first) { chain c = chain_new(); set_rt_sz(&c, rt, w); ds_off(&c); bind_ubo7(&c); if (tid) bind_tex(&c, tid); bind_prog(&c, pg); draw_cmd(&c); run(&c, first); }
static void clear_green(const tex_t *t) { chain c = chain_new(); set_rt_sz(&c, t, (int)t->d.width); clear_color(&c, 0, 1, 0, 1); run(&c, 1); commit(); }
static uint64_t t_frame = 100;
static void case_begin(void) {
    mtl_set_frame(++t_frame); bloomq_reset(); g_bloomq_last_b0 = g_bloomq_last_b1h = g_bloomq_last_b1s = 0;
    memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat);
    g_bloomq_nseen = 0;
}

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
    EXPECT("applied k after the k = 2 rewrite (what the magenta probe reads)", 1, atomic_load(&g_bright_applied) == 2.0f);
    // No-dispatch control: at k = 1 bright_lut returns at the k check, so the cells keep their values. This proves only that k ~= 1 leaves the table alone
    lut_upload(&t); lut_run(1.0f); lut_read(&t, px);
    EXPECT_NEAR("LUT k=1 (no-dispatch control): cell (0,0,0) red", 0.125, (double)px[0], 0.001);
    EXPECT_NEAR("LUT k=1 (no-dispatch control): cell (3,3,3) green", 0.875, (double)px[4 * 63 + 1], 0.001);
    EXPECT("applied k after the k = 1 control", 1, atomic_load(&g_bright_applied) == 1.0f);
    g_bright_fixed = -1;
}
// The full chain in one submit: A (the cutoff, skipped: state 1), B (the first downsample, drawn by the capture: state 2), the downsample that reads B1
// (its handle is noted), C (the upsample into B0, skipped: state 3), D (the apply reads B1 in place of B0)
static void chain_once(void) {
    step(&tB0, 8, ID_S, &p_cut, 1); EXPECT("chain: the cutoff engages (state 1)", 1, g_bloomq.state);
    step(&tB1, 4, ID_B0, &p_down, 0); EXPECT("chain: the capture draws B1 (state 2)", 2, g_bloomq.state);
    step(&tB2, 2, ID_B1, &p_down, 0); step(&tB1, 4, ID_B2, &p_up, 0);
    step(&tB0, 8, ID_B1, &p_up, 0); EXPECT("chain: the upsample into B0 is skipped (state 3)", 3, g_bloomq.state);
    step(&tF, 8, ID_B0, &p_apply, 0); EXPECT("chain: the apply reads B1 in place of B0", ID_B1, S.tex[1][0]);
    commit();
}
static void chain_case(void) {
    case_begin(); chain_once();
    EXPECT("chain: the last B1 handle is kept for frames without a chain", ID_B1, g_bloomq_last_b1h);
    EXPECT("chain: state back to 0", 0, g_bloomq.state);
    EXPECT("chain: one apply counted", 1, g_bloomq_stat.applied);
    EXPECT("chain: no abort", 0, g_bloomq_stat.ab61 + g_bloomq_stat.ab62 + g_bloomq_stat.ab63);
    EXPECT("chain: one new target set announced", 1, g_bloomq_nseen);
    mtl_set_frame(++t_frame); chain_once();
    EXPECT("chain, next frame: the same targets are not announced again", 1, g_bloomq_nseen);
    EXPECT("chain, next frame: the apply counts into the same window", 2, g_bloomq_stat.applied);
}
// RED 61: another draw comes in the middle of the chain. B0 (never drawn: its cutoff was skipped) is cleared to black; the other draw still draws
static void abort61_case(void) {
    uint8_t px[256]; clear_green(&tB0); clear_green(&tT); case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tT, 8, 0, &p_other, 0); commit();
    EXPECT("abort 61: counted", 1, g_bloomq_stat.ab61);
    EXPECT("abort 61: state back to 0", 0, g_bloomq.state);
    readback(&tB0, px); EXPECT("abort 61: B0 cleared to black", 64, count_px(px, 0, 0, 0));
    readback(&tT, px); EXPECT("abort 61: the other draw still drew", 64, count_px(px, 255, 0, 0));
}
// RED 62: the first downsample does not read B0, so the capture is not drawn
static void abort62_case(void) {
    uint8_t px[256]; clear_green(&tB0); case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tB1, 4, ID_T, &p_down, 0); commit();
    EXPECT("abort 62: counted", 1, g_bloomq_stat.ab62);
    readback(&tB0, px); EXPECT("abort 62: B0 cleared to black", 64, count_px(px, 0, 0, 0));
}
// RED 63: the upsample into B0 comes without a downsample that read B1, so the apply has no B1 handle
static void abort63_case(void) {
    uint8_t px[256]; clear_green(&tB0); case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tB1, 4, ID_B0, &p_down, 0); step(&tB0, 8, ID_B2, &p_up, 0); step(&tF, 8, ID_B0, &p_apply, 0); commit();
    EXPECT("abort 63: counted", 1, g_bloomq_stat.ab63);
    readback(&tB0, px); EXPECT("abort 63: B0 cleared to black", 64, count_px(px, 0, 0, 0));
}
// A chain left in the middle: (a) at a frame boundary, (b) by a second cutoff on the same B0, (c) by a second cutoff on another target
static void midchain_case(void) {
    uint8_t px[256];
    clear_green(&tB0); case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); commit(); mtl_set_frame(++t_frame); step(&tB1, 4, ID_B0, &p_down, 1); commit();
    EXPECT("left mid-chain at a frame boundary: counted", 1, g_bloomq_stat.midchain);
    EXPECT("left mid-chain at a frame boundary: state 0", 0, g_bloomq.state);
    readback(&tB0, px); EXPECT("left mid-chain at a frame boundary: B0 black before the state-0 downsample read it", 64, count_px(px, 0, 0, 0));
    clear_green(&tB0); case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tB0, 8, ID_S, &p_cut, 0); commit();
    EXPECT("second cutoff, same B0: counted", 1, g_bloomq_stat.midchain);
    EXPECT("second cutoff, same B0: engaged again (state 1)", 1, g_bloomq.state);
    readback(&tB0, px); EXPECT("second cutoff, same B0: B0 neither drawn nor cleared", 64, count_px(px, 0, 255, 0));
    clear_green(&tB0); clear_green(&tT); case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tT, 8, ID_S, &p_cut, 0); commit();
    EXPECT("second cutoff, another target: counted", 1, g_bloomq_stat.midchain);
    EXPECT("second cutoff, another target: engaged on it (state 1)", 1, g_bloomq.state);
    EXPECT("second cutoff, another target: B0 is now that target", tT.serial, g_bloomq.b0_tex.serial);
    readback(&tB0, px); EXPECT("second cutoff, another target: the first B0 cleared to black", 64, count_px(px, 0, 0, 0));
    readback(&tT, px); EXPECT("second cutoff, another target: the new B0 is not drawn", 64, count_px(px, 0, 255, 0));
}
// A capture pipeline that could not be built: bloom 1/4 is off and the game's own cutoff draws
static void broken_case(void) {
    uint8_t px[256]; clear_green(&tB0); case_begin();
    g_bloomq_broken = 1;
    step(&tB0, 8, ID_S, &p_cut, 1); commit();
    EXPECT("broken pipeline: not engaged", 0, g_bloomq.state);
    readback(&tB0, px); EXPECT("broken pipeline: the game's cutoff drew", 64, count_px(px, 255, 0, 0));
    g_bloomq_broken = 0;
}
// The cutoff's viewport must cover the whole target (1 px of slack): a half-width viewport leaves bloom 1/4 off and the game's cutoff draws the left half
static void vp_case(void) {
    uint8_t px[256]; clear_green(&tB0); case_begin();
    { chain c = chain_new(); set_rt_sz(&c, &tB0, 8); viewport(&c, 0, 0, 4, 8); ds_off(&c); bind_ubo7(&c); bind_tex(&c, ID_S); bind_prog(&c, &p_cut); draw_cmd(&c); run(&c, 1); commit(); }
    EXPECT("viewport 4x8 on an 8x8 target: not engaged", 0, g_bloomq.state);
    EXPECT("viewport 4x8 on an 8x8 target: counted", 1, g_bloomq_stat.vp);
    readback(&tB0, px); EXPECT("viewport 4x8: the game's cutoff drew the left half", 32, count_px(px, 255, 0, 0)); EXPECT("viewport 4x8: the right half untouched", 32, count_px(px, 0, 255, 0));
    case_begin();
    { chain c = chain_new(); set_rt_sz(&c, &tB0, 8); viewport(&c, 0, 0, 7, 8); ds_off(&c); bind_ubo7(&c); bind_tex(&c, ID_S); bind_prog(&c, &p_cut); draw_cmd(&c); run(&c, 1); commit(); }
    EXPECT("viewport 7x8 on an 8x8 target (within 1 px): engaged", 1, g_bloomq.state);
    bloomq_reset();
}
// The apply at state 0 reuses the last B1 only in a frame whose chain never started (the extrapolated frame). After a cutoff that did not engage
// the game's own chain drew B0, and after an abort B0 is black (that frame has no bloom): both read B0. A second apply after a completed chain still
// reads B1, as B0 was never drawn in that frame
static void reuse_case(void) {
    case_begin(); chain_once();
    mtl_set_frame(++t_frame); memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat);
    step(&tF, 8, ID_B0, &p_apply, 1); EXPECT("reuse: a frame without a chain reads the last B1", ID_B1, S.tex[1][0]); commit();
    EXPECT("reuse: counted", 1, g_bloomq_stat.reused);
    mtl_set_frame(++t_frame); memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat);
    { chain c = chain_new(); set_rt_sz(&c, &tB0, 8); viewport(&c, 0, 0, 4, 8); ds_off(&c); bind_ubo7(&c); bind_tex(&c, ID_S); bind_prog(&c, &p_cut); draw_cmd(&c); run(&c, 1); }
    EXPECT("cutoff not engaged after an engaged frame: counted", 1, g_bloomq_stat.vp);
    step(&tB1, 4, ID_B0, &p_down, 0); step(&tB0, 8, ID_B1, &p_up, 0);   // the game's own chain
    step(&tF, 8, ID_B0, &p_apply, 0); EXPECT("cutoff not engaged: the apply reads the game's B0", ID_B0, S.tex[1][0]); commit();
    EXPECT("cutoff not engaged: not counted as reused", 0, g_bloomq_stat.reused);
    mtl_set_frame(++t_frame); memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat);
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tB1, 4, ID_B0, &p_down, 0); step(&tT, 8, 0, &p_other, 0);   // abort 61 at state 2
    step(&tF, 8, ID_B0, &p_apply, 0); EXPECT("abort at state 2: the apply reads B0 (black)", ID_B0, S.tex[1][0]); commit();
    EXPECT("abort at state 2: counted", 1, g_bloomq_stat.ab61);
    EXPECT("abort at state 2: not counted as reused", 0, g_bloomq_stat.reused);
    mtl_set_frame(++t_frame); memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat); chain_once();
    step(&tF, 8, ID_B0, &p_apply, 1); EXPECT("a second apply after a completed chain still reads B1", ID_B1, S.tex[1][0]); commit();
    EXPECT("a second apply after a completed chain: counted as reused", 1, g_bloomq_stat.reused);
}
// B0 under bloom 1/4 is often never allocated. The cutoff's pipeline check takes B0's format from its description and does not allocate it; a chain left at
// a frame boundary still clears it (allocating it, as its next reader would) so the rest of the chain reads black; a B0 the game released since is not recreated
static void fresh_b0_case(void) {
    uint8_t px[256]; tex_t b0 = tex(8, NVN_FORMAT_RGBA8); case_begin();
    step(&b0, 8, ID_S, &p_cut, 1); EXPECT("fresh B0: the cutoff engages", 1, g_bloomq.state);
    EXPECT("fresh B0: the cutoff's pipeline check does not allocate B0", 0, serial_get(b0.serial) != nil); commit();
    mtl_set_frame(++t_frame); step(&tT, 8, 0, &p_other, 1); commit();
    EXPECT("fresh B0 left at a frame boundary: counted", 1, g_bloomq_stat.midchain);
    EXPECT("fresh B0 left at a frame boundary: allocated and cleared", 1, serial_get(b0.serial) != nil);
    readback(&b0, px); EXPECT("fresh B0 left at a frame boundary: black", 64, count_px(px, 0, 0, 0));
    tex_t b0r = tex(8, NVN_FORMAT_RGBA8); case_begin();
    step(&b0r, 8, ID_S, &p_cut, 1); commit(); mtl_tex_release(&b0r);
    mtl_set_frame(++t_frame); step(&tT, 8, 0, &p_other, 1); commit();
    EXPECT("released B0 left at a frame boundary: counted", 1, g_bloomq_stat.midchain);
    EXPECT("released B0 left at a frame boundary: not recreated", 0, serial_get(b0r.serial) != nil);
}
// The B0 clear can come inside an open pass (here a second draw into the same targets aborts the chain). That pass ends early and its draws go on in a reopened
// pass, which loads its attachments: they must be stored, even though the store lookahead sees D cleared later in the submit. The pass [T, D] writes depth 0
// with its first draw; the aborting draw is blue, depth-tested LESS at depth 0, and must fail. D holds 1.0 in memory from before. The GPU may store a DontCare
// attachment anyway (this Mac's did), so the store decision is checked directly: no attachment may be dropped by the lookahead (dropped bytes other than
// the unwritten-attachment rule's, which is safe because memory already holds that content)
static void ds_state(chain *c, int dt, int dw, int df) { ds_st d = {dt, dw, df, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP}; p_bind_state x = {.kind = 5}; memcpy(x.bytes, &d, sizeof d); emit(c, OP_BIND_STATE, &x, sizeof x); }
static void set_rt_ds(chain *c, const tex_t *a, const tex_t *ds) { p_set_rt r = {.n = 1}; r.color[0] = a; r.depth = ds; emit(c, OP_SET_RT, &r, sizeof r); p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = 8; emit(c, OP_VIEWPORTS, &v, sizeof v); p_scissor sc = {0, 0, 8, 8}; emit(c, OP_SCISSOR, &sc, sizeof sc); }
static void split_case(void) {
    uint8_t px[256]; tex_t d = tex(8, NVN_FORMAT_DEPTH32F), x = tex(8, NVN_FORMAT_RGBA8); p_clear_ds one = {1.0f, 1, 0, 0};
    clear_green(&tT); { chain c = chain_new(); set_rt_ds(&c, &x, &d); emit(&c, OP_CLEAR_DS, &one, sizeof one); run(&c, 1); commit(); }   // D = 1.0 in memory
    case_begin();
    step(&tB0, 8, ID_S, &p_cut, 1); step(&tB1, 4, ID_B0, &p_down, 0); EXPECT("split pass: the chain reaches state 2", 2, g_bloomq.state);
    chain c = chain_new(); set_rt_ds(&c, &tT, &d); ds_state(&c, 1, 1, NVN_DEPTH_FUNC_ALWAYS); bind_ubo7(&c); bind_tex(&c, ID_B1); bind_prog(&c, &p_down); draw_cmd(&c);   // opens [T, D]: red, depth 0
    ds_state(&c, 1, 0, NVN_DEPTH_FUNC_LESS); bind_prog(&c, &p_blue); draw_cmd(&c);   // another draw mid-chain, same pass: abort 61 clears B0 inside the open pass
    set_rt_ds(&c, &x, &d); emit(&c, OP_CLEAR_DS, &one, sizeof one);   // D cleared later in the same submit
    uint64_t dropped0 = g_sd_dead_bytes - g_udc_saved_bytes; run(&c, 0); commit();
    EXPECT("split pass: abort counted", 1, g_bloomq_stat.ab61);
    EXPECT("split pass: the interrupted pass's depth is stored (no lookahead DontCare)", 0, g_sd_dead_bytes - g_udc_saved_bytes - dropped0);
    readback(&tT, px); EXPECT("split pass: the blue draw fails its depth test against the stored depth", 64, count_px(px, 255, 0, 0));
}
// The health summary: the first window prints after 600 frames, later ones after 3600; new targets flush the window early; an empty window stays closed
static char g_log[8192];
static void log_cap(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", b); if (strlen(g_log) + strlen(b) + 2 < sizeof g_log) { strcat(g_log, b); strcat(g_log, "\n"); } }
static int log_count(const char *needle) { int n = 0; for (const char *q = g_log; (q = strstr(q, needle)); q++) n++; return n; }
static void summary_case(void) {
    memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat); g_log[0] = 0; g_dm_log_hook = log_cap;
    g_bloomq_stat.applied = 1; bloomq_summary(10, 1); EXPECT("summary: a flush at new targets prints at once", 1, log_count("bloom 1/4, frames 10-10: 1 applied"));
    g_bloomq_stat.applied = 2; bloomq_summary(11, 0); bloomq_summary(611, 0); EXPECT("summary: after a flush the first full window is still 600 frames", 1, log_count("bloom 1/4, frames 11-611: 2 applied"));
    memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat); g_log[0] = 0;
    bloomq_summary(900, 0); EXPECT("summary: nothing counted, the window stays closed", 0, g_bloomq_stat.f0);
    g_bloomq_stat.applied = 5; g_bloomq_stat.vp = 1; bloomq_summary(1000, 0); bloomq_summary(1599, 0);
    EXPECT("summary: no line before 600 frames", 0, log_count("bloom 1/4, frames"));
    bloomq_summary(1600, 0);
    EXPECT("summary: the first window prints after 600 frames", 1, log_count("bloom 1/4, frames 1000-1600: 5 applied, 0 reused the last B1, 1 not engaged (viewport 1, pipeline 0), 0 aborted"));
    EXPECT("summary: the counters start again", 0, g_bloomq_stat.applied + g_bloomq_stat.vp);
    g_bloomq_stat.applied = 1; bloomq_summary(1601, 0); bloomq_summary(2201, 0);
    EXPECT("summary: later windows do not print after 600 frames", 1, log_count("bloom 1/4, frames"));
    bloomq_summary(5201, 0); EXPECT("summary: later windows print after 3600 frames", 1, log_count("bloom 1/4, frames 1601-5201: 1 applied"));
    g_bloomq_stat.ab62 = 1; bloomq_summary(5300, 0); bloomq_summary(5301, 1); EXPECT("summary: a flush prints the window so far", 1, log_count("bloom 1/4, frames 5300-5301: 0 applied, 0 reused the last B1, 0 not engaged (viewport 0, pipeline 0), 1 aborted (other draw 0, capture 1, no B1 0)"));
    g_dm_log_hook = NULL; memset(&g_bloomq_stat, 0, sizeof g_bloomq_stat);
}
int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init(); lib_init(); bloom_init();
    aux_case(); bright_case(); chain_case(); abort61_case(); abort62_case(); abort63_case(); midchain_case(); broken_case(); vp_case(); summary_case();
    reuse_case(); fresh_b0_case(); split_case();
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0; } }
