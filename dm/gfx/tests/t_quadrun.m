// 2D quad runs (nvn_mtl.m, "2D quad runs"): the look-ahead's rules on hand-made command streams, the check that the verify mode makes, and a
// real Metal replay that must draw the same pixels with the merge off, on and checked. No dependence on the game.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (uint64_t)(want), g = (uint64_t)(got); count++; printf("%s: expected %llu, got %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w == g ? "ok" : "FAILED"); fail += w != g; } while (0)
typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(size_t cap) { chain c = {0}; if (posix_memalign((void **)&c.p, getpagesize(), cap)) abort(); memset(c.p, 0, cap); mtl_pool_add(c.p, cap); return c; }
static const uint8_t *emit(chain *c, unsigned op, const void *a, size_t n) { size_t z = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)z}; uint8_t *at = c->p + c->n; memcpy(at, &h, 8); if (n) memcpy(at + 8, a, n); c->n += z; return at; }
static uint8_t *pool_mem(size_t n) { uint8_t *m; if (posix_memalign((void **)&m, getpagesize(), n)) abort(); memset(m, 0, n); mtl_pool_add(m, n); return m; }
static void vbuf(chain *c, uint64_t addr, uint64_t size) { p_bind_vbuf b = {.first = 0, .n = 1}; b.r[0].address = addr; b.r[0].size = size; emit(c, OP_BIND_VBUF, &b, sizeof b); }
static void texs(chain *c, unsigned op, uint64_t h0) { p_bind_handles b = {.stage = 1, .first = 0, .n = 2}; b.h[0] = h0; b.h[1] = 7; emit(c, op, &b, sizeof b); }
static void ubo(chain *c, uint64_t addr) { p_bind_buf b = {.stage = 1, .index = 0, .addr = addr, .size = 64}; emit(c, OP_BIND_UBO, &b, sizeof b); }
static void scissor(chain *c, int x, int y, int w, int h) { p_scissor sc = {x, y, w, h}; emit(c, OP_SCISSOR, &sc, sizeof sc); }
static const uint8_t *strip(chain *c, int prim, int first, int n) { p_draw d = {prim, first, n}; return emit(c, OP_DRAW, &d, sizeof d); }

// ---------- the look-ahead ----------
// The state every case starts from: buffer 0 holds a 64-byte quad (stride 16), two textures and two samplers in stage 1, one uniform buffer, a scissor.
static void scan_state(void) {
    state_reset(); memset(&R, 0, sizeof R);
    S.vbuf[0] = (NVNbufferRange){0x1000, 64}; S.tex[1][0] = 5; S.tex[1][1] = 7; S.samp[1][0] = 9; S.samp[1][1] = 7;
    S.ubo[1][0] = (p_bind_buf){.stage = 1, .index = 0, .addr = 0x9000, .size = 64}; R.sc = (p_scissor){0, 0, 8, 8}; R.has_sc = 1;
}
// One more quad the way the game issues it: its vertex range, the same samplers and textures again, the draw.
static const uint8_t *quad(chain *c, uint64_t addr) { vbuf(c, addr, 64); texs(c, OP_BIND_SAMP, 9); texs(c, OP_BIND_TEX, 5); return strip(c, 5, 0, 4); }
static int scan(chain *c, const uint8_t **last) { emit(c, OP_END, NULL, 0); p_draw first = {5, 0, 4}; g_exec_next = c->p; int n = qr_scan(&first, last); g_exec_next = NULL; return n; }
static void scan_cases(void) {
    const uint8_t *last, *want;
    { scan_state(); chain c = chain_new(65536); quad(&c, 0x1100); quad(&c, 0x1200); want = quad(&c, 0x1300);
      EXPECT("three more quads that change only the vertex range: all four are one run", 4, scan(&c, &last)); EXPECT("the run ends at the last quad's draw command", 1, last == want);
      EXPECT("the look-ahead read each quad's own vertex range", 1, g_qr.addr[0] == 0x1000 && g_qr.addr[1] == 0x1100 && g_qr.addr[2] == 0x1200 && g_qr.addr[3] == 0x1300); }
    { scan_state(); chain c = chain_new(65536); want = quad(&c, 0x1100); vbuf(&c, 0x1200, 64); texs(&c, OP_BIND_TEX, 6); strip(&c, 5, 0, 4);
      EXPECT("another texture before the third quad: the run stops at the second", 2, scan(&c, &last)); EXPECT("and ends at the second quad's draw command", 1, last == want); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); texs(&c, OP_BIND_SAMP, 8); strip(&c, 5, 0, 4); EXPECT("another sampler: no run", 1, scan(&c, &last)); EXPECT("no run has no last command", 1, last == NULL); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 48); strip(&c, 5, 0, 4); EXPECT("a vertex range of another size: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); p_bind_vbuf b = {.first = 1, .n = 1}; b.r[0].address = 0x1100; b.r[0].size = 64; emit(&c, OP_BIND_VBUF, &b, sizeof b); strip(&c, 5, 0, 4); EXPECT("a range for another vertex buffer: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); ubo(&c, 0x9000); strip(&c, 5, 0, 4); EXPECT("the same uniform buffer bound again: still a run", 2, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); ubo(&c, 0x9100); strip(&c, 5, 0, 4); EXPECT("another uniform buffer: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); scissor(&c, 0, 0, 8, 8); strip(&c, 5, 0, 4); EXPECT("the same scissor set again: still a run", 2, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); scissor(&c, 0, 0, 4, 8); strip(&c, 5, 0, 4); EXPECT("another scissor: no run", 1, scan(&c, &last)); }
    { scan_state(); R.has_sc = 0; chain c = chain_new(65536); vbuf(&c, 0x1100, 64); scissor(&c, 0, 0, 8, 8); strip(&c, 5, 0, 4); EXPECT("a first scissor where none was set: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); strip(&c, 5, 0, 6); EXPECT("a draw of six vertices: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); strip(&c, 4, 0, 4); EXPECT("a draw of another primitive type: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); strip(&c, 5, 4, 4); EXPECT("a draw that starts at another vertex: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); vbuf(&c, 0x1100, 64); p_bind_program b = {NULL, 3}; emit(&c, OP_BIND_PROGRAM, &b, sizeof b); strip(&c, 5, 0, 4); EXPECT("a program bind in between: no run", 1, scan(&c, &last)); }
    { scan_state(); chain c = chain_new(65536); strip(&c, 5, 0, 4); EXPECT("the same quad drawn again without a new range: a run of two", 2, scan(&c, &last)); EXPECT("with the same range twice", 1, g_qr.addr[1] == 0x1000); }
    { scan_state(); chain c = chain_new(1 << 20); for (int i = 0; i < QR_MAX + 80; i++) { vbuf(&c, 0x2000 + 0x100 * (uint64_t)i, 64); strip(&c, 5, 0, 4); }
      EXPECT("more quads than a run holds: the run stops at its limit", QR_MAX, scan(&c, &last)); }
}

// ---------- what the verify mode catches ----------
static void verify_case(void) {
    static uint8_t mem[256], copy[128]; for (int i = 0; i < 256; i++) mem[i] = (uint8_t)i;
    chain c = chain_new(65536); const uint8_t *first = strip(&c, 5, 0, 4), *second = strip(&c, 5, 0, 4), *third = strip(&c, 5, 0, 4), *after = strip(&c, 5, 0, 4);
    p_draw dw = {5, 0, 4}; uint64_t bad0;
    #define ARM() do { scan_state(); g_quadrun = 2; g_qr.addr[0] = (uint64_t)mem; g_qr.addr[1] = (uint64_t)mem + 64; g_qr.addr[2] = (uint64_t)mem + 128; g_qr.stride = 16; memcpy(copy, mem, 64); memcpy(copy + 64, mem + 64, 64); g_qr.copy = copy; \
        S.vbuf[0] = (NVNbufferRange){(uint64_t)mem, 64}; g_exec_cur = first; qr_commit(2, second); bad0 = g_qr_bad; S.vbuf[0].address = (uint64_t)mem + 64; g_exec_cur = second; } while (0)
    ARM(); EXPECT("the run's second draw command is covered", 1, qr_covered(&dw)); EXPECT("nothing differs: no red line", 0, g_qr_bad - bad0); EXPECT("the run is over after its last command", 0, g_qr.n);
    g_exec_cur = third; EXPECT("a draw after the run is not covered", 0, qr_covered(&dw));
    ARM(); g_exec_cur = after; EXPECT("a draw command outside the run is not covered", 0, qr_covered(&dw)); g_qr.n = 0;
    ARM(); S.tex[1][0] = 6; qr_covered(&dw); EXPECT("another texture at a merged quad: a red line", 1, g_qr_bad - bad0);
    ARM(); S.vbuf[0].address = (uint64_t)mem + 128; qr_covered(&dw); EXPECT("another vertex range than the look-ahead read: a red line", 1, g_qr_bad - bad0);
    ARM(); mem[64] ^= 0xff; qr_covered(&dw); EXPECT("vertices changed after the copy: a red line", 1, g_qr_bad - bad0); mem[64] ^= 0xff;
    ARM(); R.sc.w = 4; qr_covered(&dw); EXPECT("another scissor at a merged quad: a red line", 1, g_qr_bad - bad0);
    #undef ARM
    g_qr.n = 0; g_qr_bad = 0; g_exec_cur = NULL;
}

// ---------- a real replay ----------
static NvProg *t_keep[8]; static int t_nkeep; static id<MTLLibrary> g_lib; static uint64_t g_pser = 8300;
static NvShader *shader(NSString *fn, int stage) { NvShader *s = [NvShader new]; s->fn = [g_lib newFunctionWithName:fn]; s->stage = stage; s->rf.pos_attr = -1; if (!s->fn) abort(); return s; }
static void prog(prog_t *pg, NvShader *vs, NvShader *fs) { memset(pg, 0, sizeof *pg); pg->serial = g_pser++; NvProg *p = [NvProg new]; p->sh[0] = vs; p->sh[1] = fs; t_keep[t_nkeep++] = p; serial_put(pg->serial, p); }
static NvShader *vs_attr(void) { NvShader *s = shader(@"va", 0); s->rf.nv = 1; s->rf.v[0].loc = 0; s->rf.v[0].nvn = 0; s->rf.v[0].ct = NVN_CT_FLOAT; s->rf.pos_attr = 0; return s; }
static NvShader *fs_flat(NSString *fn) { NvShader *s = shader(fn, 1); s->rf.color_mask = 1; return s; }
static tex_t target(void) { static uint64_t s = 9000; tex_t t = {.serial = s++}; t.d.width = t.d.height = 8; t.d.depth = t.d.samples = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8; return t; }
static int count_px(const uint8_t px[256], uint8_t r, uint8_t g, uint8_t b) { int n = 0; for (int i = 0; i < 64; i++) n += abs(px[4 * i] - r) <= 2 && abs(px[4 * i + 1] - g) <= 2 && abs(px[4 * i + 2] - b) <= 2; return n; }
// Quadrant q of the target as a 4-vertex strip, each quad in its own 256-byte slot of the pool, as the game lays them out.
static void quad_vertices(uint8_t *mem, int q) { float x0 = (q & 1) ? 0 : -1, y0 = (q & 2) ? 0 : -1, v[16] = {x0, y0, 0, 1, x0 + 1, y0, 0, 1, x0, y0 + 1, 0, 1, x0 + 1, y0 + 1, 0, 1}; memcpy(mem + 256 * q, v, 64); }
// Red quads in quadrants 0, 1 and 2, then a green draw with no new vertex range: it must land on quadrant 2, the range the game bound last.
// The first quad opens the pass and is never merged, so the run is quads 1 and 2.
static void replay(int mode, uint8_t px[256], uint64_t *runs, uint64_t *merged) {
    static prog_t red, green; static int made; if (!made) { made = 1; prog(&red, vs_attr(), fs_flat(@"red")); prog(&green, vs_attr(), fs_flat(@"green")); }
    uint8_t *mem = pool_mem(16384); for (int q = 0; q < 4; q++) quad_vertices(mem, q);
    tex_t t = target(); chain c = chain_new(65536);
    p_set_rt r = {.n = 1}; r.color[0] = &t; emit(&c, OP_SET_RT, &r, sizeof r); p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = 8; emit(&c, OP_VIEWPORTS, &v, sizeof v); scissor(&c, 0, 0, 8, 8);
    p_clear_color cc = {.index = 0, .mask = 15, .rgba = {0, 0, 0, 1}}; emit(&c, OP_CLEAR_COLOR, &cc, sizeof cc);
    ds_st d = {0, 0, NVN_DEPTH_FUNC_ALWAYS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
    p_bind_state ds = {.kind = 5}; memcpy(ds.bytes, &d, sizeof d); emit(&c, OP_BIND_STATE, &ds, sizeof ds);
    p_bind_program bp = {&red, 3}; emit(&c, OP_BIND_PROGRAM, &bp, sizeof bp);
    va_st at = {0x2E, 0, 0}; p_bind_vattr a = {.first = 0, .n = 1}; memcpy(&a.st[0], &at, 4); emit(&c, OP_BIND_VATTR, &a, sizeof a);
    p_bind_vstream s = {.first = 0, .n = 1}; s.st[0] = (vs_st){16, 0}; emit(&c, OP_BIND_VSTREAM, &s, sizeof s);
    for (int q = 0; q < 3; q++) { vbuf(&c, (uint64_t)mem + 256 * (uint64_t)q, 64); strip(&c, 5, 0, 4); }
    p_bind_program bg = {&green, 3}; emit(&c, OP_BIND_PROGRAM, &bg, sizeof bg); strip(&c, 5, 0, 4);
    emit(&c, OP_END, NULL, 0);
    g_quadrun = mode; uint64_t r0, m0; mtl_quadrun_stats(&r0, &m0);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t hh = (uint64_t)c.p; mtl_sub_index(&hh, 1); mtl_exec(c.p);
    id<MTLBuffer> b = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    uint64_t f = mtl_sub_commit(); EXPECT("the submission completed", 1, mtl_wait(f, 5000000000ull)); EXPECT("with no GPU error", 0, mtl_last_commit_error()); memcpy(px, b.contents, 256);
    mtl_quadrun_stats(runs, merged); *runs -= r0; *merged -= m0;
}
static void replay_cases(void) {
    NSError *e = nil; g_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VI { float4 a [[attribute(0)]]; }; vertex float4 va(VI i [[stage_in]]) { return i.a; }\n"
        "fragment float4 red() { return float4(1, 0, 0, 1); }\nfragment float4 green() { return float4(0, 1, 0, 1); }\n" options:nil error:&e];
    if (!g_lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); abort(); }
    uint8_t off[256], on[256], chk[256]; uint64_t runs, merged;
    replay(0, off, &runs, &merged);
    EXPECT("merge off: two quadrants red", 32, count_px(off, 255, 0, 0)); EXPECT("merge off: the last-bound quadrant green", 16, count_px(off, 0, 255, 0)); EXPECT("merge off: one quadrant untouched", 16, count_px(off, 0, 0, 0));
    EXPECT("merge off: no run", 0, runs);
    replay(1, on, &runs, &merged);
    EXPECT("merge on: one run", 1, runs); EXPECT("merge on: one quad merged into it", 1, merged); EXPECT("merge on: the same pixels, byte for byte", 0, memcmp(on, off, 256) != 0);
    uint64_t bad0 = g_qr_bad; replay(2, chk, &runs, &merged);
    EXPECT("checked: one run", 1, runs); EXPECT("checked: the same pixels, byte for byte", 0, memcmp(chk, off, 256) != 0); EXPECT("checked: no red line", 0, g_qr_bad - bad0);
}
int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0); setenv("DM_NVN_TBDR", "1", 1); mtl_init();
    scan_cases(); verify_case(); replay_cases();
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0; } }
