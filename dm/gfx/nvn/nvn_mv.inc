// Motion vectors, the input MetalFX's temporal scaler needs and the Switch build never makes (no pass draws the game's velocity shaders, and no velocity
// target exists). Included by nvn_mtl.m; replay thread only. On with DM_MV=1.
//
// mv_note_draw runs at each depth prepass draw (a depth-only pass on the D32S8 target with depth writes, with a vertex shader that has the dm_velocity
// entry, dmsh_rules.py velocity_entry): it keeps a copy of the draw's constant buffers (the object's block, its bone palette, the view and frame blocks;
// shared blocks once a frame) and what it takes to draw it again. Copies, because the game's pool memory is reused later. A draw is matched to last
// frame's by shader, index and vertex ranges and its occurrence among draws alike.
//
// mv_resolve runs at the game's 3D upscale draw, before the scaler (nvn_mtl.m mfx3d_encode). It fills the RG16F velocity target in two passes:
//   1. every pixel from the depth buffer: the camera's motion, from the two frames' view-projections (scene constants +336). Everything that didn't move
//      in the world is right after this pass, which in this game is nearly every draw: the fixed camera follows the hero;
//   2. the draws whose object block or palette changed since last frame, drawn again with dm_velocity (the shader's own code on both frames' constants),
//      under a depth-equal test against the prepass. Skinning, cloth and the object's transform come out of the game's own vertex code.
// A velocity is the offset from this pixel to where it was last frame, in fractions of the content (uv, y down). Draws new this frame keep the camera's motion.
//
// DM_MV_DUMP=<first frame>:<count>:<path prefix> writes the velocity target and the 3D picture of those frames as raw files for an offline check
// (dm/gfx/tools/mv_check.py warps one frame onto the next), at the next frame's first prepass draw.
static int mv_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_MV"); v = e && atoi(e) > 0; } return v; }

enum { MV_MAX_DRAWS = 4096, MV_TAB = 16384, MV_MAX_CB = 8, MV_RING = 5, MV_SNAP_BYTES = 8 << 20, MV_DEDUPE = 4096, MV_VIEW_REG = 3, MV_FRAME_REG = 10, MV_VP_OFF = 336 };
typedef struct { uint32_t off, size; int8_t reg, aid; } mv_cb;
typedef struct {
    __unsafe_unretained NvShader *vs; __unsafe_unretained id<MTLRenderPipelineState> pso;
    uint64_t key, vb[15], ib_gpu; NSUInteger ib_len; mv_cb cb[MV_MAX_CB]; int ncb, count, itype, instances, base_vertex, base_instance, cull, wind; float bias[3];
} mv_draw;
typedef struct { uint64_t key; int idx; } mv_slot;
static struct {
    __strong id<MTLBuffer> snap[MV_RING]; uint64_t gpu[MV_RING]; size_t used; int ri;
    mv_draw *d[2]; int n[2], cur; mv_slot *tab[2]; struct { uint64_t key; int n; } *occ; struct { uint64_t addr, size; uint32_t off; uint32_t stamp; } *dd; uint32_t stamp;
    uint64_t frame[2]; float vp[2][16]; int has_vp[2]; double view[6]; int moved_last, stat_overflow, stat_full; uint64_t stat_frames, stat_moved, stat_draws; size_t stat_peak;
    __strong id<MTLTexture> depth, vel; __strong id<MTLLibrary> lib; __strong id<MTLRenderPipelineState> cam; __strong id<MTLDepthStencilState> dss;
    __strong id<MTL4ArgumentTable> vt, ft; __strong id<MTLBuffer> zero, unif;
    long dump_first, dump_count; const char *dump_path; __strong id<MTLTexture> dump_col; __strong id<MTLTexture> dump_vel; uint64_t dump_frame; int inited;
} g_mv;
static nvn_hmap g_mv_pso = {.klen = sizeof(pso_key)};

static const char *g_mv_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct U { float4x4 m; float4 view; };\n"
    "vertex float4 mv_fs_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    // the camera's motion: the pixel's clip position (from the viewport and the depth buffer), through last frame's view-projection x the inverse of this frame's
    "fragment float2 mv_cam(float4 p [[position]], constant U& u [[buffer(0)]], depth2d<float> d [[texture(0)]]) {\n"
    "  float2 ndc = float2(2 * (p.x - u.view.x) / u.view.z - 1, 1 - 2 * (p.y - u.view.y) / u.view.w);\n"
    "  float4 q = u.m * float4(ndc, d.read(uint2(p.xy)), 1);\n"
    "  if (abs(q.w) < 1e-6) return float2(0);\n"
    "  return (q.xy / q.w - ndc) * float2(0.5, -0.5); }\n"
    "struct O { float4 p [[position]]; float4 cur [[user(dm_cur)]]; float4 prev [[user(dm_prev)]]; };\n"
    "fragment float2 mv_obj(O in [[stage_in]]) { return (in.prev.xy / in.prev.w - in.cur.xy / in.cur.w) * float2(0.5, -0.5); }\n";

static int mv_init(void) {
    if (g_mv.inited) return g_mv.inited > 0;
    g_mv.inited = -1;
    for (int i = 0; i < MV_RING; i++) { g_mv.snap[i] = [g_dev newBufferWithLength:MV_SNAP_BYTES options:MTLResourceStorageModeShared]; if (!g_mv.snap[i]) return 0; res_add(g_mv.snap[i]); g_mv.gpu[i] = g_mv.snap[i].gpuAddress; }
    g_mv.zero = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(g_mv.zero);
    g_mv.unif = [g_dev newBufferWithLength:(sizeof(float) * 20 + 255) / 256 * 256 * MV_RING options:MTLResourceStorageModeShared]; res_add(g_mv.unif);
    res_commit();
    for (int i = 0; i < 2; i++) { g_mv.d[i] = calloc(MV_MAX_DRAWS, sizeof(mv_draw)); g_mv.tab[i] = calloc(MV_TAB, sizeof(mv_slot)); }
    g_mv.occ = calloc(MV_TAB, sizeof *g_mv.occ); g_mv.dd = calloc(MV_DEDUPE, sizeof *g_mv.dd);
    NSError *e = nil; uint64_t t0 = bp_now(); g_mv.lib = [g_dev newLibraryWithSource:@(g_mv_msl) options:nil error:&e];
    if (!g_mv.lib) { MLOG("【红】motion vectors: the shaders did not compile (%s)", e.localizedDescription.UTF8String); return 0; }
    host_built(0, t0, "motion vectors");
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_mv.lib newFunctionWithName:@"mv_fs_vs"]; d.fragmentFunction = [g_mv.lib newFunctionWithName:@"mv_cam"]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
    t0 = bp_now(); g_mv.cam = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!g_mv.cam) { MLOG("【红】motion vectors: the camera pipeline failed (%s)", e.localizedDescription.UTF8String); return 0; }
    host_built(1, t0, "motion vectors camera");
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionEqual; dd.depthWriteEnabled = NO; g_mv.dss = [g_dev newDepthStencilStateWithDescriptor:dd];
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 31; g_mv.vt = [g_dev newArgumentTableWithDescriptor:ad error:&e];
    ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 1; g_mv.ft = [g_dev newArgumentTableWithDescriptor:ad error:&e];
    if (!g_mv.vt || !g_mv.ft) { MLOG("【红】motion vectors: no argument tables (%s)", e.localizedDescription.UTF8String); return 0; }
    const char *de = getenv("DM_MV_DUMP"); g_mv.dump_first = -1;
    if (de) { char *q; g_mv.dump_first = strtol(de, &q, 10); if (*q == ':') { g_mv.dump_count = strtol(q + 1, &q, 10); if (*q == ':') g_mv.dump_path = q + 1; } if (!g_mv.dump_path) g_mv.dump_first = -1; }
    g_mv.inited = 1; g_mv.stamp = 1; MLOG("motion vectors: on (%d MB of constant copies per frame at most)", MV_SNAP_BYTES >> 20);
    return 1;
}

static void mv_dump_raw(id<MTLTexture> t, size_t bpp, const char *suffix, uint64_t frame) {
    if (!t) return;
    size_t row = t.width * bpp, n = row * t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:n];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    char path[1024]; snprintf(path, sizeof path, "%s%llu_%s.raw", g_mv.dump_path, (unsigned long long)frame, suffix);
    FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %zu\n", (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, row); fwrite(b.contents, 1, n, f); fclose(f); }
}

// A new frame's first prepass draw: last frame's draws become the previous ones, the next copy buffer starts
static void mv_frame_begin(uint64_t frame) {
    if (g_mv.dump_vel) {   // the velocity and picture targets of a dumped frame: nothing has written them yet this frame
        mtl_finish_gpu(); mv_dump_raw(g_mv.dump_vel, 4, "mv", g_mv.dump_frame); mv_dump_raw(g_mv.dump_col, 4, "col", g_mv.dump_frame);
        g_mv.dump_vel = nil; g_mv.dump_col = nil;
    }
    g_mv.cur ^= 1; g_mv.ri = (g_mv.ri + 1) % MV_RING; g_mv.used = 0; g_mv.stamp++;
    int c = g_mv.cur; g_mv.n[c] = 0; g_mv.frame[c] = frame; g_mv.has_vp[c] = 0; memset(g_mv.tab[c], 0, MV_TAB * sizeof(mv_slot)); memset(g_mv.occ, 0, MV_TAB * sizeof *g_mv.occ);
}

// A copy of a constant buffer in this frame's snapshot buffer: its offset + 1, or 0 when it can't be read now (a write still pending in this submission) or there is no room
static uint32_t mv_snap(const p_bind_buf *u) {
    if (!u->addr || !u->size) return 0;
    uint32_t h = (uint32_t)((u->addr ^ u->size) * 11400714819323198485ull >> 40) % MV_DEDUPE;
    for (int i = 0; i < 8; i++) { typeof(&g_mv.dd[0]) e = &g_mv.dd[(h + i) % MV_DEDUPE]; if (e->stamp != g_mv.stamp) break; if (e->addr == u->addr && e->size == u->size) return e->off; }
    void *mb; uint64_t mo; if (pend_hits(u->addr, u->size) || !mtl_addr(u->addr, u->size, &mb, &mo)) return 0;
    const uint8_t *base = [(__bridge id<MTLBuffer>)mb contents]; if (!base) return 0;
    size_t n = (u->size + 255) & ~(size_t)255; if (g_mv.used + n > MV_SNAP_BYTES) { g_mv.stat_full++; return 0; }
    uint32_t off = (uint32_t)g_mv.used; memcpy((uint8_t *)g_mv.snap[g_mv.ri].contents + off, base + mo, u->size); g_mv.used += n;
    for (int i = 0; i < 8; i++) { typeof(&g_mv.dd[0]) e = &g_mv.dd[(h + i) % MV_DEDUPE]; if (e->stamp != g_mv.stamp) { e->stamp = g_mv.stamp; e->addr = u->addr; e->size = u->size; e->off = off + 1; break; } }
    return off + 1;
}

static id<MTLRenderPipelineState> mv_pso(NvShader *vs) {
    uint32_t cfmt[8] = {MTLPixelFormatRG16Float}; pso_key k; pso_key_fill(&k, vs, nil, nil, cfmt, MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatInvalid, 1, NULL);   // depth only: the stencil plane stays as it is
    k.blend_en = 0; k.chmask = 0xffffffff; k.a2c = 0; memset(k.blend, 0, sizeof k.blend);
    id<MTLRenderPipelineState> ps = hm_get_id(&g_mv_pso, &k); if (ps) return ps;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, nil, nil, 0);
    d.vertexFunction = vs->vel_fn; d.fragmentFunction = [g_mv.lib newFunctionWithName:@"mv_obj"]; d.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen; d.colorAttachments[0].blendingEnabled = NO;
    NSError *e = nil; uint64_t t0 = bp_now(); ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { RED_ONCE(61, "motion vectors: pipeline for %s failed (%s)", vs->key, e.localizedDescription.UTF8String); return nil; }
    host_built(1, t0, "motion vectors object"); hm_put_id(&g_mv_pso, &k, ps);
    return ps;
}

static void mv_note_draw(NvProg *pr, const p_draw_idx *di) {
    NvShader *vs = pr->sh[0];
    if (!mv_on() || !di || !vs || !vs->vel_fn || R.rt.n != 0 || !R.rt.depth || !S.ds.dw || g_pass.dfmt != MTLPixelFormatDepth32Float_Stencil8 || !di->count || di->itype == 0) return;
    if (!mv_init()) return;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (g_mv.frame[g_mv.cur] != frame) mv_frame_begin(frame);
    int c = g_mv.cur;
    if (g_mv.n[c] >= MV_MAX_DRAWS) { if (!g_mv.stat_overflow++) MLOG("【红】motion vectors: more than %d prepass draws; the rest keep the camera's motion", MV_MAX_DRAWS); return; }
    if (!g_mv.n[c]) {
        g_mv.depth = tex_mtl(R.rt.depth); g_mv_depth_ser = R.rt.depth->serial;
        g_mv.view[0] = S.has_vp ? S.vp[0][0] : 0; g_mv.view[1] = S.has_vp ? S.vp[0][1] : 0; g_mv.view[2] = S.has_vp ? S.vp[0][2] : g_pass.tw; g_mv.view[3] = S.has_vp ? S.vp[0][3] : g_pass.th;
        g_mv.view[4] = S.has_vp ? S.dr[0] : 0; g_mv.view[5] = S.has_vp ? S.dr[1] : 1;
    }
    mv_draw *d = &g_mv.d[c][g_mv.n[c]]; memset(d, 0, sizeof *d);
    d->vs = vs; d->count = di->count; d->itype = di->itype; d->instances = di->instances ? di->instances : 1; d->base_vertex = di->base_vertex; d->base_instance = di->base_instance;
    d->cull = S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone;
    d->wind = S.poly.front == NVN_FRONT_FACE_CCW ? MTLWindingCounterClockwise : MTLWindingClockwise;
    if (S.poly.offset_en & 4) { d->bias[0] = S.po.units; d->bias[1] = S.po.factor; d->bias[2] = S.po.clamp; }
    void *mb; uint64_t mo; if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &d->ib_gpu)) return;
    d->ib_len = ((__bridge id<MTLBuffer>)mb).length - mo;
    for (int s = 0; s < 15; s++) { if (S.vbuf[s].address && mtl_gpu_addr(S.vbuf[s].address, 1, &mb, &mo, &d->vb[s])) {} }
    for (int i = 0; i < vs->rf.nb; i++) { const nvn_rbind *b = &vs->rf.b[i];
        if (b->kind != NVN_RB_CBUF || b->reg >= 16 || d->ncb >= MV_MAX_CB) return;   // a vertex shader that also reads textures keeps the camera's motion
        uint32_t o = mv_snap(&S.ubo[0][b->reg]); if (!o) return;
        d->cb[d->ncb++] = (mv_cb){o - 1, (uint32_t)S.ubo[0][b->reg].size, (int8_t)b->reg, (int8_t)b->aid};
        if (b->reg == MV_VIEW_REG && !g_mv.has_vp[c] && S.ubo[0][b->reg].size >= MV_VP_OFF + 64) { memcpy(g_mv.vp[c], (uint8_t *)g_mv.snap[g_mv.ri].contents + o - 1 + MV_VP_OFF, 64); g_mv.has_vp[c] = 1; }
    }
    if (!(d->pso = mv_pso(vs))) return;
    uint64_t h = 1469598103934665603ull;
    #define MV_H(x) do { uint64_t v_ = (uint64_t)(x); for (int q_ = 0; q_ < 8; q_++) { h = (h ^ (v_ & 0xff)) * 1099511628211ull; v_ >>= 8; } } while (0)
    MV_H((uintptr_t)(__bridge void *)vs); MV_H(di->ibuf); MV_H(di->count); MV_H(S.vbuf[0].address);
    int occ = 0; { uint32_t x = (uint32_t)(h >> 20) % MV_TAB; for (int i = 0; i < 16; i++) { typeof(&g_mv.occ[0]) e = &g_mv.occ[(x + i) % MV_TAB]; if (!e->key || e->key == h) { e->key = h; occ = e->n++; break; } } }
    MV_H(occ);
    #undef MV_H
    d->key = h ? h : 1;
    { uint32_t x = (uint32_t)(d->key >> 20) % MV_TAB; for (int i = 0; i < 64; i++) { mv_slot *e = &g_mv.tab[c][(x + i) % MV_TAB]; if (!e->key) { e->key = d->key; e->idx = g_mv.n[c]; break; } } }
    g_mv.n[c]++;
}

static const mv_draw *mv_prev_of(const mv_draw *d) {
    int p = !g_mv.cur; uint32_t x = (uint32_t)(d->key >> 20) % MV_TAB;
    for (int i = 0; i < 64; i++) { const mv_slot *e = &g_mv.tab[p][(x + i) % MV_TAB]; if (!e->key) return NULL; if (e->key == d->key) return &g_mv.d[p][e->idx]; }
    return NULL;
}
static const mv_cb *mv_cb_of(const mv_draw *d, int reg) { for (int i = 0; i < d->ncb; i++) if (d->cb[i].reg == reg) return &d->cb[i]; return NULL; }
// Whether a draw's own constants (everything but the shared view and frame blocks) changed since last frame
static int mv_moved(const mv_draw *d, const mv_draw *p, int prev_ri) {
    const uint8_t *cur = g_mv.snap[g_mv.ri].contents, *old = g_mv.snap[prev_ri].contents;
    for (int i = 0; i < d->ncb; i++) { const mv_cb *a = &d->cb[i]; if (a->reg == MV_VIEW_REG || a->reg == MV_FRAME_REG) continue;
        const mv_cb *b = mv_cb_of(p, a->reg); if (!b || b->size != a->size || memcmp(cur + a->off, old + b->off, a->size)) return 1; }
    return 0;
}

static int mat4_inv(const double m[16], double o[16]) {   // column-major; 0 = singular
    double a[4][8]; for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { a[r][c] = m[c * 4 + r]; a[r][c + 4] = r == c; }
    for (int c = 0; c < 4; c++) { int p = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return 0;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[p][k]; a[p][k] = a[c][k]; a[c][k] = t; }
        double f = 1 / a[c][c]; for (int k = 0; k < 8; k++) a[c][k] *= f;
        for (int r = 0; r < 4; r++) if (r != c) { double g = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= g * a[c][k]; } }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[c * 4 + r] = a[r][c + 4];
    return 1;
}

// Replay thread, between the game's upscale draw and the scaler, with no encoder open. src: the 3D picture, for DM_MV_DUMP. 1 = the velocity target is ready for this frame
static int mv_resolve(id<MTLTexture> src) {
    if (!mv_on() || g_mv.inited <= 0) return 0;
    int c = g_mv.cur, p = !c; uint64_t frame = atomic_load(&g_cur_frame);
    if (g_mv.frame[c] != frame || !g_mv.n[c] || !g_mv.has_vp[c] || !g_mv.depth) return 0;
    if (!g_mv.vel || g_mv.vel.width != g_mv.depth.width || g_mv.vel.height != g_mv.depth.height) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:g_mv.depth.width height:g_mv.depth.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; g_mv.vel = [g_dev newTextureWithDescriptor:td]; if (!g_mv.vel) return 0;
        res_add(g_mv.vel); res_commit(); }
    int have_prev = g_mv.has_vp[p] && g_mv.frame[p] + 2 >= frame && g_mv.frame[p] != frame;   // last frame's draws, unless the game drew no prepass for a while (a loading screen)
    double vc[16], vl[16], inv[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = g_mv.vp[c][i]; vl[i] = have_prev ? g_mv.vp[p][i] : g_mv.vp[c][i]; }
    if (!mat4_inv(vc, inv)) return 0;
    for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) { double s = 0; for (int k = 0; k < 4; k++) s += vl[k * 4 + r] * inv[q * 4 + k]; m[q * 4 + r] = s; }
    float u[20]; for (int i = 0; i < 16; i++) u[i] = (float)m[i]; for (int i = 0; i < 4; i++) u[16 + i] = (float)g_mv.view[i];
    NSUInteger uo = (NSUInteger)g_mv.ri * 256; memcpy((uint8_t *)g_mv.unif.contents + uo, u, sizeof u);
    // pass 1: the camera's motion, every pixel
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = g_mv.vel; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    pg_render(rp, 1);   // DM_NVN_PASSGPU times both passes
    RENC en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) { pg_end_render(); return 0; }
    pg_render_begin(en);
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en setArgumentTable:g_mv.ft atStages:MTLRenderStageFragment];
    [g_mv.ft setTexture:g_mv.depth.gpuResourceID atIndex:0]; [g_mv.ft setAddress:g_mv.unif.gpuAddress + uo atIndex:0];
    [en setViewport:(MTLViewport){0, 0, g_mv.vel.width, g_mv.vel.height, 0, 1}]; [en setRenderPipelineState:g_mv.cam]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    pg_render_finish(en); enc_host_r_end(en); pg_end_render();
    // pass 2: the draws that moved
    int moved = 0, prev_ri = (g_mv.ri + MV_RING - 1) % MV_RING;
    if (have_prev) {
        rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = g_mv.vel; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = g_mv.depth; rp.depthAttachment.loadAction = MTLLoadActionLoad; rp.depthAttachment.storeAction = MTLStoreActionStore;
        en = nil;
        uint8_t *ab = g_mv.snap[g_mv.ri].contents; uint64_t ab_gpu = g_mv.gpu[g_mv.ri], old_gpu = g_mv.gpu[prev_ri];
        for (int i = 0; i < g_mv.n[c]; i++) {
            const mv_draw *d = &g_mv.d[c][i]; const mv_draw *o = mv_prev_of(d);
            if (!o || !mv_moved(d, o, prev_ri)) continue;
            size_t need = (d->vs->arg_len * 2 + 15) & ~(size_t)15; if (g_mv.used + need > MV_SNAP_BYTES) break;
            uint64_t *cs = (uint64_t *)(ab + g_mv.used), *ps = (uint64_t *)((uint8_t *)cs + d->vs->arg_len); uint64_t cg = ab_gpu + g_mv.used, pg = cg + d->vs->arg_len; g_mv.used += need;
            memset(cs, 0, need);
            for (int k = 0; k < d->ncb; k++) { const mv_cb *a = &d->cb[k], *b = mv_cb_of(o, a->reg); cs[a->aid] = ab_gpu + a->off; ps[a->aid] = b ? old_gpu + b->off : ab_gpu + a->off; }
            if (!en) {
                pg_render(rp, 1); en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) { pg_end_render(); return 0; }
                pg_render_begin(en);
                [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
                [en setArgumentTable:g_mv.vt atStages:MTLRenderStageVertex]; [en setDepthStencilState:g_mv.dss];
                [en setViewport:(MTLViewport){g_mv.view[0], g_mv.view[1], g_mv.view[2], g_mv.view[3], g_mv.view[4], g_mv.view[5]}];
                [g_mv.vt setAddress:g_mv.zero.gpuAddress atIndex:VB_ZERO];
            }
            [en setRenderPipelineState:d->pso]; [en setCullMode:(MTLCullMode)d->cull]; [en setFrontFacingWinding:(MTLWinding)d->wind]; [en setDepthBias:d->bias[0] slopeScale:d->bias[1] clamp:d->bias[2]];
            for (int s = 0; s < 15; s++) if (d->vb[s]) [g_mv.vt setAddress:d->vb[s] atIndex:16 + s];
            [g_mv.vt setAddress:cg atIndex:d->vs->rf.abi]; [g_mv.vt setAddress:pg atIndex:1];
            [en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d->count indexType:d->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:d->ib_gpu indexBufferLength:d->ib_len instanceCount:d->instances baseVertex:d->base_vertex baseInstance:d->base_instance];
            moved++;
        }
        if (en) { pg_render_finish(en); enc_host_r_end(en); pg_end_render(); }
    }
    if (moved != g_mv.moved_last) { g_mv.moved_last = moved; PD("V %llu motion vectors: %d of %d prepass draws moved", g_stat_pass, moved, g_mv.n[c]); }
    { static int once; if (!once++) MLOG("motion vectors: velocity target %lux%lu, content viewport %.0f,%.0f %.0fx%.0f", (unsigned long)g_mv.vel.width, (unsigned long)g_mv.vel.height, g_mv.view[0], g_mv.view[1], g_mv.view[2], g_mv.view[3]); }
    g_mv.stat_frames++; g_mv.stat_moved += moved; g_mv.stat_draws += g_mv.n[c]; if (g_mv.used > g_mv.stat_peak) g_mv.stat_peak = g_mv.used;
    if (g_mv.stat_frames % 600 == 0) { MLOG("motion vectors: %llu frames, %.0f prepass draws a frame, %.1f moved, copies at most %.1f MB of %d, draws left with the camera's motion for lack of room %d", (unsigned long long)g_mv.stat_frames, (double)g_mv.stat_draws / g_mv.stat_frames, (double)g_mv.stat_moved / g_mv.stat_frames, g_mv.stat_peak / 1048576.0, MV_SNAP_BYTES >> 20, g_mv.stat_full); }
    if (g_mv.dump_first >= 0 && (long)frame >= g_mv.dump_first && (long)frame < g_mv.dump_first + g_mv.dump_count) { g_mv.dump_vel = g_mv.vel; g_mv.dump_col = src; g_mv.dump_frame = frame; }
    return 1;
}
