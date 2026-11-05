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
//
// The temporal scaler (Upscaling = MetalFX Temporal; dm_video_temporal_active, DM_MFXT=0/1 forces it) replaces the spatial MetalFX of the 3D pass. It takes the game's 3D picture and depth directly, with these velocities, and needs a camera that moves by a fraction of a pixel each frame: the game's own
// Halton(2,3) mode (32 steps), which the Switch build never selects and the rewriter's KJITTER slot does (patch_nso.py SWITCHES, the csel rows). The offset of this frame
// comes out of the jittered projection, so the velocities can leave it out: MetalFX wants the motion of the surface, not of the sampling. SMAA and FXAA are off with it
// (hle_oe.c aa_tick): jittered frames are what it antialiases. Nothing here is built on the frame thread: the libraries, pipelines and the scaler (0.7 s the first time)
// come from a background queue, and until they exist a draw keeps the camera's motion and the frame uses the spatial scaler.
__attribute__((weak)) int dm_video_temporal_active(void) { return 0; }   // the host's dm_video.c
static int g_mt_want, g_mv_want;   // read at the start of each submission (mv_sub_begin), replay thread
static inline int mv_on(void) { return g_mv_want; }
int mtl_mfx_temporal_supported(void) {   // any thread, before the renderer has a device too (the recommendation asks at launch)
    static int ok; static dispatch_once_t once;
    dispatch_once(&once, ^{ id<MTLDevice> d = MTLCreateSystemDefaultDevice(); ok = d && [MTLFXTemporalScalerDescriptor supportsMetal4FX:d]; MLOG("MetalFX: Metal 4 temporal scaler supported: %s", ok ? "yes" : "no"); });
    return ok;
}

enum { MV_MAX_DRAWS = 4096, MV_TAB = 16384, MV_MAX_CB = 8, MV_RING = 5, MV_SNAP_BYTES = 8 << 20, MV_DEDUPE = 4096, MV_VIEW_REG = 3, MV_FRAME_REG = 10, MV_VP_OFF = 336, MV_PROJ_OFF = 208 };
typedef struct { uint32_t off, size; int8_t reg, aid; } mv_cb;
typedef struct {
    __unsafe_unretained NvShader *vs; __unsafe_unretained id<MTLRenderPipelineState> pso;
    uint64_t key, base, vb[15], ib_gpu; NSUInteger ib_len; mv_cb cb[MV_MAX_CB]; int ncb, occ, used, count, itype, instances, base_vertex, base_instance, cull, wind; float bias[3];
} mv_draw;
typedef struct { uint64_t key; int idx; } mv_slot;
static struct {
    __strong id<MTLBuffer> snap[MV_RING]; uint64_t gpu[MV_RING]; size_t used; int ri;
    mv_draw *d[2]; int n[2], cur; mv_slot *tab[2]; struct { uint64_t key; int n; } *occ; struct { uint64_t addr, size; uint32_t off; uint32_t stamp; } *dd; uint32_t stamp;
    uint64_t frame[2]; float vp[2][16], jit[2][2], view_m[2][16], proj_m[2][16], nearf[2], farf[2]; int has_vp[2]; double view[6]; int have_prev, moved_last, stat_overflow, stat_full; uint64_t stat_frames, stat_moved, stat_draws, last_use; size_t stat_peak; int stat_cuts, stat_far;
    __strong id<MTLTexture> depth, vel; __strong id<MTLLibrary> lib; __strong id<MTLFunction> obj_fn; __strong id<MTLRenderPipelineState> cam; __strong id<MTLDepthStencilState> dss;
    __strong id<MTL4ArgumentTable> vt, ft; __strong id<MTLBuffer> zero, unif; __strong id<MTLFence> fence;
    long dump_first, dump_count; const char *dump_path; __strong id<MTLTexture> dump_col; __strong id<MTLTexture> dump_vel; __strong id<MTLTexture> dump_out; __strong id<MTLTexture> dump_gen; __strong id<MTLTexture> dump_real; __strong id<MTLTexture> dump_comp; __strong id<MTLTexture> dump_swap; uint64_t dump_frame; int inited;
} g_mv;
static nvn_hmap g_mv_pso = {.klen = sizeof(pso_key)};

static const char *g_mv_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct U { float4x4 m; float4 view; float4 jit; };\n"
    "vertex float4 mv_fs_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    // the camera's motion: the pixel's clip position (from the viewport and the depth buffer), through last frame's view-projection x the inverse of this frame's
    "fragment float2 mv_cam(float4 p [[position]], constant U& u [[buffer(0)]], depth2d<float> d [[texture(0)]]) {\n"
    "  float2 ndc = float2(2 * (p.x - u.view.x) / u.view.z - 1, 1 - 2 * (p.y - u.view.y) / u.view.w) - u.jit.xy;\n"
    "  float4 q = u.m * float4(ndc, d.read(uint2(p.xy)), 1);\n"
    "  if (abs(q.w) < 1e-6) return float2(0);\n"
    "  return (q.xy / q.w - ndc) * float2(0.5, -0.5); }\n"
    "struct O { float4 p [[position]]; float4 cur [[user(dm_cur)]]; float4 prev [[user(dm_prev)]]; };\n"
    "fragment float2 mv_obj(O in [[stage_in]], constant U& u [[buffer(0)]]) { return (in.prev.xy / in.prev.w - in.cur.xy / in.cur.w - u.jit.zw) * float2(0.5, -0.5); }\n";

static dispatch_queue_t mv_queue(void) {   // pipelines and the scaler: one at a time, never on the frame thread
    static dispatch_queue_t q; static dispatch_once_t o; dispatch_once(&o, ^{ q = dispatch_queue_create("dm.mv.build", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)); });
    return q;
}
static _Atomic int g_mv_lib_ok;   // 1 = g_mv.lib and g_mv.cam can be used, -1 = they failed (for the session)
static int mv_init(void) {
    if (g_mv.inited) return g_mv.inited > 0;
    g_mv.inited = -1;
    for (int i = 0; i < MV_RING; i++) { g_mv.snap[i] = [g_dev newBufferWithLength:MV_SNAP_BYTES options:MTLResourceStorageModeShared]; if (!g_mv.snap[i]) return 0; res_add(g_mv.snap[i]); g_mv.gpu[i] = g_mv.snap[i].gpuAddress; }
    if (!g_mv.zero) { g_mv.zero = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(g_mv.zero); }
    g_mv.unif = [g_dev newBufferWithLength:256 * MV_RING options:MTLResourceStorageModeShared]; res_add(g_mv.unif);
    res_commit(); if (!g_mv.fence) g_mv.fence = [g_dev newFence];
    for (int i = 0; i < 2; i++) { if (!g_mv.d[i]) g_mv.d[i] = calloc(MV_MAX_DRAWS, sizeof(mv_draw)); if (!g_mv.tab[i]) g_mv.tab[i] = calloc(MV_TAB, sizeof(mv_slot)); g_mv.n[i] = 0; g_mv.has_vp[i] = 0; g_mv.frame[i] = 0; }
    if (!g_mv.occ) g_mv.occ = calloc(MV_TAB, sizeof *g_mv.occ);
    if (!g_mv.dd) g_mv.dd = calloc(MV_DEDUPE, sizeof *g_mv.dd);
    if (!g_mv.vt) { NSError *e = nil; MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 31; g_mv.vt = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 1; g_mv.ft = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        if (!g_mv.vt || !g_mv.ft) { MLOG("【红】motion vectors: no argument tables (%s)", e.localizedDescription.UTF8String); return 0; } }
    if (!g_mv.dss) { MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionEqual; dd.depthWriteEnabled = NO; g_mv.dss = [g_dev newDepthStencilStateWithDescriptor:dd]; }
    const char *de = getenv("DM_MV_DUMP"); g_mv.dump_first = -1;
    if (de) { char *q; g_mv.dump_first = strtol(de, &q, 10); if (*q == ':') { g_mv.dump_count = strtol(q + 1, &q, 10); if (*q == ':') g_mv.dump_path = q + 1; } if (!g_mv.dump_path) g_mv.dump_first = -1; }
    if (!g_mv.lib && !atomic_load(&g_mv_lib_ok)) dispatch_async(mv_queue(), ^{
        NSError *e = nil; uint64_t t0 = bp_now(); id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(g_mv_msl) options:nil error:&e];
        if (!lib) { MLOG("【红】motion vectors: the shaders did not compile (%s)", e.localizedDescription.UTF8String); atomic_store(&g_mv_lib_ok, -1); return; }
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"mv_fs_vs"]; d.fragmentFunction = [lib newFunctionWithName:@"mv_cam"]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
        id<MTLRenderPipelineState> cam = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!cam) { MLOG("【红】motion vectors: the camera pipeline failed (%s)", e.localizedDescription.UTF8String); atomic_store(&g_mv_lib_ok, -1); return; }
        g_mv.obj_fn = [lib newFunctionWithName:@"mv_obj"]; g_mv.cam = cam; g_mv.lib = lib; atomic_store(&g_mv_lib_ok, 1);
        MLOG("motion vectors: shaders built in %.1f ms (background)", (bp_now() - t0) / 1e6); });
    g_mv.inited = 1; g_mv.stamp = 1; MLOG("motion vectors: on (%d MB of constant copies per frame at most)", MV_SNAP_BYTES >> 20);
    return 1;
}

static void mv_dump_raw(id<MTLTexture> t, size_t bpp, const char *suffix, uint64_t frame) {
    if (!t) return;
    { const char *only = getenv("DM_MV_DUMP_ONLY"); if (only && !strstr(only, suffix)) return; }   // a list of the suffixes to write: comp,swap for a clip
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
        mtl_finish_gpu(); mv_dump_raw(g_mv.dump_vel, 4, "mv", g_mv.dump_frame); mv_dump_raw(g_mv.dump_col, 4, "col", g_mv.dump_frame); mv_dump_raw(g_mv.dump_out, 8, "out", g_mv.dump_frame); mv_dump_raw(g_mv.dump_gen, g_mv.dump_gen.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4, "gen", g_mv.dump_frame); mv_dump_raw(g_mv.dump_real, g_mv.dump_real.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4, "real", g_mv.dump_frame); mv_dump_raw(g_mv.dump_comp, 4, "comp", g_mv.dump_frame); mv_dump_raw(g_mv.dump_swap, 4, "swap", g_mv.dump_frame);
        g_mv.dump_comp = nil; g_mv.dump_swap = nil; g_mv.dump_vel = nil; g_mv.dump_col = nil; g_mv.dump_out = nil; g_mv.dump_gen = nil; g_mv.dump_real = nil;
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

// A moving draw's pipeline, or nil until the background queue has built it (the draw keeps the camera's motion until then). In the map a key with a build under way holds
// the marker; finished builds wait in g_mv_done until mv_sub_begin puts them in (the map belongs to the replay thread)
static NSMutableArray *g_mv_done; static os_unfair_lock g_mv_done_lk = OS_UNFAIR_LOCK_INIT;
static id g_mv_marker;
static id<MTLRenderPipelineState> mv_pso(NvShader *vs) {
    uint32_t cfmt[8] = {MTLPixelFormatRG16Float}; pso_key k; pso_key_fill(&k, vs, nil, nil, cfmt, MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatInvalid, 1, NULL);   // depth only: the stencil plane stays as it is
    k.blend_en = 0; k.chmask = 0xffffffff; k.a2c = 0; memset(k.blend, 0, sizeof k.blend);
    id ps = hm_get_id(&g_mv_pso, &k);
    if (ps) return ps == g_mv_marker ? nil : ps;
    if (!g_mv_marker) { g_mv_marker = [NSObject new]; g_mv_done = [NSMutableArray array]; }
    hm_put_id(&g_mv_pso, &k, g_mv_marker);
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, nil, nil, 0);
    d.vertexFunction = vs->vel_fn; d.fragmentFunction = g_mv.obj_fn; d.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen; d.colorAttachments[0].blendingEnabled = NO;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k]; NSString *who = @(vs->key);
    dispatch_async(mv_queue(), ^{ NSError *e = nil; id<MTLRenderPipelineState> r = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!r) { MLOG("【红】motion vectors: pipeline for %s failed (%s)", who.UTF8String, e.localizedDescription.UTF8String); return; }
        os_unfair_lock_lock(&g_mv_done_lk); [g_mv_done addObject:@[key, r]]; os_unfair_lock_unlock(&g_mv_done_lk); });
    return nil;
}

static void mv_note_draw(NvProg *pr, const p_draw_idx *di) {
    NvShader *vs = pr->sh[0];
    if (!g_mv_want || !di || !vs || !vs->vel_fn || R.rt.n != 0 || !R.rt.depth || !S.ds.dw || g_pass.dfmt != MTLPixelFormatDepth32Float_Stencil8 || !di->count || di->itype == 0) return;
    if (!mv_init() || atomic_load(&g_mv_lib_ok) != 1) return;
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
        if (b->reg == MV_VIEW_REG && !g_mv.has_vp[c] && S.ubo[0][b->reg].size >= MV_VP_OFF + 64) { const float *pj = (const float *)((uint8_t *)g_mv.snap[g_mv.ri].contents + o - 1 + MV_PROJ_OFF);
            memcpy(g_mv.vp[c], (uint8_t *)g_mv.snap[g_mv.ri].contents + o - 1 + MV_VP_OFF, 64); g_mv.has_vp[c] = 1;
            // the frustum is centred, so these hold nothing but the offset: the world's camera is orthographic (w comes from the fourth row), the character screen's a perspective one (the third)
            int wr = pj[11] != 0 ? 8 : 12; g_mv.jit[c][0] = pj[wr + 3] != 0 ? pj[wr] / pj[wr + 3] : 0; g_mv.jit[c][1] = pj[wr + 3] != 0 ? pj[wr + 1] / pj[wr + 3] : 0;
            // for the frame interpolator (nvn_fg.inc): the view, the projection without the offset, and the depth range (block +0x50, +0xd0; +0x250 / +0x254 hold far / near for an orthographic camera)
            { const uint8_t *blk = (const uint8_t *)g_mv.snap[g_mv.ri].contents + o - 1; memcpy(g_mv.view_m[c], blk + 0x50, 64); memcpy(g_mv.proj_m[c], pj, 64); g_mv.proj_m[c][wr] = 0; g_mv.proj_m[c][wr + 1] = 0; g_mv.farf[c] = ((const float *)(blk + 0x250))[0]; g_mv.nearf[c] = ((const float *)(blk + 0x250))[1]; } }
    }
    d->pso = mv_pso(vs);   // nil for now: nothing to draw again, but next frame's match still finds this one's constants
    uint64_t h = 1469598103934665603ull;
    #define MV_H(x) do { uint64_t v_ = (uint64_t)(x); for (int q_ = 0; q_ < 8; q_++) { h = (h ^ (v_ & 0xff)) * 1099511628211ull; v_ >>= 8; } } while (0)
    MV_H((uintptr_t)(__bridge void *)vs); MV_H(di->ibuf); MV_H(di->count); MV_H(S.vbuf[0].address);
    int occ = 0; { uint32_t x = (uint32_t)(h >> 20) % MV_TAB; for (int i = 0; i < 16; i++) { typeof(&g_mv.occ[0]) e = &g_mv.occ[(x + i) % MV_TAB]; if (!e->key || e->key == h) { e->key = h; occ = e->n++; break; } } }
    d->base = h; d->occ = occ; MV_H(occ);
    #undef MV_H
    d->key = h ? h : 1;
    { uint32_t x = (uint32_t)(d->key >> 20) % MV_TAB; for (int i = 0; i < 64; i++) { mv_slot *e = &g_mv.tab[c][(x + i) % MV_TAB]; if (!e->key) { e->key = d->key; e->idx = g_mv.n[c]; break; } } }
    g_mv.n[c]++;
}

static const mv_cb *mv_cb_of(const mv_draw *d, int reg) { for (int i = 0; i < d->ncb; i++) if (d->cb[i].reg == reg) return &d->cb[i]; return NULL; }
// The world-space position a draw's constants say it has: the object's translation (block 0) and its first bone's (block 2); only compared with the same draw's in another frame
static void mv_where(const mv_draw *d, const uint8_t *base, float o[6]) {
    memset(o, 0, 6 * sizeof(float));
    const mv_cb *a = mv_cb_of(d, 0); if (a && a->size >= 60) memcpy(o, base + a->off + 48, 3 * sizeof(float));
    const mv_cb *b = mv_cb_of(d, 2); if (b && b->size >= 48) { const float *f = (const float *)(base + b->off); o[3] = f[3]; o[4] = f[7]; o[5] = f[11]; }
}
// Last frame's draw of this one: among the draws of the same mesh, the nearest one not taken yet. Draws of one mesh (several monsters of a kind) can come in another order
// each frame, and going by their order alone gave one a velocity of hundreds of pixels. Too far away for one frame = not the same object: it keeps the camera's motion
static mv_draw *mv_prev_of(const mv_draw *d, const float *vp, int prev_ri) {
    int p = !g_mv.cur; mv_draw *best = NULL; float bs = 0, w[6], w2[6];
    mv_where(d, g_mv.snap[g_mv.ri].contents, w);
    for (int occ = 0; occ < 64; occ++) {
        uint64_t h = d->base; { uint64_t v_ = (uint64_t)occ; for (int q_ = 0; q_ < 8; q_++) { h = (h ^ (v_ & 0xff)) * 1099511628211ull; v_ >>= 8; } } if (!h) h = 1;
        const mv_slot *e = NULL; uint32_t x = (uint32_t)(h >> 20) % MV_TAB;
        for (int i = 0; i < 64; i++) { const mv_slot *t = &g_mv.tab[p][(x + i) % MV_TAB]; if (!t->key) break; if (t->key == h) { e = t; break; } }
        if (!e) break;
        mv_draw *o = &g_mv.d[p][e->idx]; if (o->used) continue;
        mv_where(o, g_mv.snap[prev_ri].contents, w2);
        float s = 0; for (int i = 0; i < 6; i++) s += fabsf(w[i] - w2[i]);
        if (!best || s < bs) { best = o; bs = s; if (s == 0) break; }
    }
    if (!best) return NULL;
    mv_where(best, g_mv.snap[prev_ri].contents, w2);
    for (int k = 0; k < 2; k++) { float dx = w[k * 3] - w2[k * 3], dy = w[k * 3 + 1] - w2[k * 3 + 1], dz = w[k * 3 + 2] - w2[k * 3 + 2];
        float nx = vp[0] * dx + vp[4] * dy + vp[8] * dz, ny = vp[1] * dx + vp[5] * dy + vp[9] * dz;
        if (fabsf(nx) > 0.5f || fabsf(ny) > 0.5f) { g_mv.stat_far++; return NULL; } }   // a quarter of the screen in one frame
    best->used = 1;
    return best;
}
// Whether a draw's own constants (everything but the shared view and frame blocks) changed since last frame
static int mv_moved(const mv_draw *d, const mv_draw *p, int prev_ri) {
    const uint8_t *cur = g_mv.snap[g_mv.ri].contents, *old = g_mv.snap[prev_ri].contents;
    for (int i = 0; i < d->ncb; i++) { const mv_cb *a = &d->cb[i]; if (a->reg == MV_VIEW_REG || a->reg == MV_FRAME_REG) continue;
        const mv_cb *b = mv_cb_of(p, a->reg); if (!b || b->size != a->size || memcmp(cur + a->off, old + b->off, a->size)) return 1; }
    return 0;
}

// DM_MV_TRACE=<file>: one line per drawn frame about the hero's limbs, from the velocity target. The camera follows her, so her body stays still on the screen while the ground slides under her, and
// what moves in her area is her limbs: a pose that freezes reads near zero however the picture looks, with no video or capture involved. The copy of her area goes in the frame's own command
// buffer and is read a few frames later, when it has surely run, so this never waits for the GPU. dm/tools/motion_scan.py --trace judges the result
enum { MVT_RING = 8 };
static struct { FILE *f; int on; __strong id<MTLBuffer> buf[MVT_RING]; uint64_t frame[MVT_RING]; double cw[MVT_RING], ch[MVT_RING], t[MVT_RING]; NSUInteger x0, y0, bw, bh; } g_mvt;
static float mvt_half(uint16_t h) { uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023; union { uint32_t u; float f; } v; if (!e) { v.f = (float)m * (1.0f / (1 << 24)); v.u |= s; return v.f; } if (e == 31) { v.u = s | 0x7f800000u | (m << 13); return v.f; } v.u = s | ((e + 112) << 23) | (m << 13); return v.f; }
static int mvt_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void mvt_read(int k) {
    if (!g_mvt.frame[k]) return;
    const uint16_t *p = g_mvt.buf[k].contents; size_t n = g_mvt.bw * g_mvt.bh; static float *xs, *ys, *limb; if (!xs) { xs = malloc(n * sizeof(float)); ys = malloc(n * sizeof(float)); limb = malloc(n * sizeof(float)); }
    double cw = g_mvt.cw[k], ch = g_mvt.ch[k]; size_t m = 0;
    for (size_t i = 0; i < n; i += 3) { xs[m] = mvt_half(p[i * 2]) * (float)cw; ys[m] = mvt_half(p[i * 2 + 1]) * (float)ch; m++; }
    // the ground: the commonest motion in her area, read at the edges of the area where she isn't
    size_t e = 0; static float *ex, *ey; if (!ex) { ex = malloc(n * sizeof(float)); ey = malloc(n * sizeof(float)); }
    for (NSUInteger y = 0; y < g_mvt.bh; y += 2) for (NSUInteger x = 0; x < g_mvt.bw; x += 2) if (x < g_mvt.bw / 8 || x >= g_mvt.bw * 7 / 8) { size_t i = y * g_mvt.bw + x; ex[e] = mvt_half(p[i * 2]) * (float)cw; ey[e] = mvt_half(p[i * 2 + 1]) * (float)ch; e++; }
    qsort(ex, e, sizeof(float), mvt_cmp); qsort(ey, e, sizeof(float), mvt_cmp); float gx = e ? ex[e / 2] : 0, gy = e ? ey[e / 2] : 0, cam = hypotf(gx, gy);
    size_t h = 0; for (size_t i = 0; i < m; i++) if (hypotf(xs[i] - gx, ys[i] - gy) > fmaxf(1.5f, cam * 0.35f)) limb[h++] = hypotf(xs[i], ys[i]);
    float p50 = 0, p95 = 0; if (h) { qsort(limb, h, sizeof(float), mvt_cmp); p50 = limb[h / 2]; p95 = limb[h * 95 / 100]; }
    fprintf(g_mvt.f, "%llu\t%.2f\t%.2f\t%zu\t%.2f\t%.2f\n", (unsigned long long)g_mvt.frame[k], g_mvt.t[k], cam, h * 3, p50, p95); fflush(g_mvt.f);
    g_mvt.frame[k] = 0;
}
static void mv_trace(uint64_t frame, double cw, double ch) {
    if (!g_mvt.on) { static int tried; if (tried) return; tried = 1; const char *e = getenv("DM_MV_TRACE"); if (!e) return; g_mvt.f = fopen(e, "w"); if (!g_mvt.f) return; fprintf(g_mvt.f, "frame\tt_ms\tcam_px\thero_px\tlimb_p50\tlimb_p95\n"); g_mvt.on = 1;
        g_mvt.bw = g_mv.vel.width * 14 / 100; g_mvt.bh = g_mv.vel.height * 30 / 100; g_mvt.x0 = g_mv.vel.width / 2 - g_mvt.bw / 2; g_mvt.y0 = g_mv.vel.height * 49 / 100 - g_mvt.bh / 2;
        for (int i = 0; i < MVT_RING; i++) { g_mvt.buf[i] = [g_dev newBufferWithLength:g_mvt.bw * g_mvt.bh * 4 options:MTLResourceStorageModeShared]; res_add(g_mvt.buf[i]); } res_commit(); }
    if (g_mvt.x0 + g_mvt.bw > g_mv.vel.width || g_mvt.y0 + g_mvt.bh > g_mv.vel.height) return;   // the 3D target shrank below the area: no line rather than a wrong one
    static int k; k = (k + 1) % MVT_RING; mvt_read(k);
    CENC e = enc_c_begin(NULL);
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    enc_b_copy_texture_buffer(e, g_mv.vel, 0, 0, MTLOriginMake(g_mvt.x0, g_mvt.y0, 0), MTLSizeMake(g_mvt.bw, g_mvt.bh, 1), g_mvt.buf[k], 0, g_mvt.bw * 4, g_mvt.bw * g_mvt.bh * 4);
    m4_end(e);
    g_mvt.frame[k] = frame; g_mvt.cw[k] = cw; g_mvt.ch[k] = ch; g_mvt.t[k] = bp_now() / 1e6;
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
    if (!g_mv_want || g_mv.inited <= 0 || atomic_load(&g_mv_lib_ok) != 1) return 0;
    int c = g_mv.cur, p = !c; uint64_t frame = atomic_load(&g_cur_frame);
    if (g_mv.frame[c] != frame || !g_mv.n[c] || !g_mv.has_vp[c] || !g_mv.depth) return 0;
    if (!g_mv.vel || g_mv.vel.width != g_mv.depth.width || g_mv.vel.height != g_mv.depth.height) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:g_mv.depth.width height:g_mv.depth.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; g_mv.vel = [g_dev newTextureWithDescriptor:td]; if (!g_mv.vel) return 0;
        res_add(g_mv.vel); res_commit(); }
    int have_prev = g_mv.has_vp[p] && g_mv.frame[p] + 2 >= frame && g_mv.frame[p] != frame;   // last frame's draws, unless the game drew no prepass for a while (a loading screen)
    g_mv.last_use = g_sub_serial;
    double vc[16], vl[16], inv[16], m[16];
    double jc[2] = {g_mv.jit[c][0], g_mv.jit[c][1]}, jl[2] = {have_prev ? g_mv.jit[p][0] : jc[0], have_prev ? g_mv.jit[p][1] : jc[1]};
    for (int i = 0; i < 16; i++) { vc[i] = g_mv.vp[c][i]; vl[i] = have_prev ? g_mv.vp[p][i] : g_mv.vp[c][i]; }
    for (int k = 0; k < 4; k++) for (int r = 0; r < 2; r++) { vc[k * 4 + r] -= jc[r] * vc[k * 4 + 3]; vl[k * 4 + r] -= jl[r] * vl[k * 4 + 3]; }   // the unjittered camera of each frame (clip.xy += offset x clip.w)
    if (!mat4_inv(vc, inv)) return 0;
    for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) { double s = 0; for (int k = 0; k < 4; k++) s += vl[k * 4 + r] * inv[q * 4 + k]; m[q * 4 + r] = s; }
    if (have_prev) {   // a camera cut (a teleport, a waypoint without a loading screen): the centre of the picture moved by a quarter of the screen or more. Nothing to reproject
        double qw = m[11] * 0.5 + m[15], qx = m[8] * 0.5 + m[12], qy = m[9] * 0.5 + m[13];
        if (fabs(qw) < 1e-9 || fabs(qx / qw) > 0.5 || fabs(qy / qw) > 0.5) { have_prev = 0; g_mv.stat_cuts++; for (int i = 0; i < 16; i++) m[i] = (i % 5) == 0; }
    }
    float u[24]; for (int i = 0; i < 16; i++) u[i] = (float)m[i]; for (int i = 0; i < 4; i++) u[16 + i] = (float)g_mv.view[i];
    u[20] = (float)jc[0]; u[21] = (float)jc[1]; u[22] = (float)(jl[0] - jc[0]); u[23] = (float)(jl[1] - jc[1]);
    g_mv.have_prev = have_prev;
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
    [en updateFence:g_mv.fence afterEncoderStages:MTLStageFragment];
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
            const mv_draw *d = &g_mv.d[c][i]; const mv_draw *o = mv_prev_of(d, g_mv.vp[c], prev_ri);
            if (!o || !d->pso || !mv_moved(d, o, prev_ri)) continue;
            size_t need = (d->vs->arg_len * 2 + 15) & ~(size_t)15; if (g_mv.used + need > MV_SNAP_BYTES) break;
            uint64_t *cs = (uint64_t *)(ab + g_mv.used), *ps = (uint64_t *)((uint8_t *)cs + d->vs->arg_len); uint64_t cg = ab_gpu + g_mv.used, pg = cg + d->vs->arg_len; g_mv.used += need;
            memset(cs, 0, need);
            for (int k = 0; k < d->ncb; k++) { const mv_cb *a = &d->cb[k], *b = mv_cb_of(o, a->reg); cs[a->aid] = ab_gpu + a->off; ps[a->aid] = b ? old_gpu + b->off : ab_gpu + a->off; }
            if (!en) {
                pg_render(rp, 1); en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) { pg_end_render(); return 0; }
                pg_render_begin(en);
                [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
                [en setArgumentTable:g_mv.vt atStages:MTLRenderStageVertex]; [en setArgumentTable:g_mv.ft atStages:MTLRenderStageFragment]; [en setDepthStencilState:g_mv.dss];
                [en setViewport:(MTLViewport){g_mv.view[0], g_mv.view[1], g_mv.view[2], g_mv.view[3], g_mv.view[4], g_mv.view[5]}];
                [g_mv.vt setAddress:g_mv.zero.gpuAddress atIndex:VB_ZERO];
            }
            [en setRenderPipelineState:d->pso]; [en setCullMode:(MTLCullMode)d->cull]; [en setFrontFacingWinding:(MTLWinding)d->wind]; [en setDepthBias:d->bias[0] slopeScale:d->bias[1] clamp:d->bias[2]];
            for (int s = 0; s < 15; s++) if (d->vb[s]) [g_mv.vt setAddress:d->vb[s] atIndex:16 + s];
            [g_mv.vt setAddress:cg atIndex:d->vs->rf.abi]; [g_mv.vt setAddress:pg atIndex:1];
            [en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d->count indexType:d->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:d->ib_gpu indexBufferLength:d->ib_len instanceCount:d->instances baseVertex:d->base_vertex baseInstance:d->base_instance];
            moved++;
        }
        if (en) { [en updateFence:g_mv.fence afterEncoderStages:MTLStageFragment]; pg_render_finish(en); enc_host_r_end(en); pg_end_render(); }
    }
    if (moved != g_mv.moved_last) { g_mv.moved_last = moved; PD("V %llu motion vectors: %d of %d prepass draws moved", g_stat_pass, moved, g_mv.n[c]); }
    mv_trace(frame, g_mv.view[2], g_mv.view[3]);
    { static int once; if (!once++) MLOG("motion vectors: velocity target %lux%lu, content viewport %.0f,%.0f %.0fx%.0f", (unsigned long)g_mv.vel.width, (unsigned long)g_mv.vel.height, g_mv.view[0], g_mv.view[1], g_mv.view[2], g_mv.view[3]); }
    g_mv.stat_frames++; g_mv.stat_moved += moved; g_mv.stat_draws += g_mv.n[c]; if (g_mv.used > g_mv.stat_peak) g_mv.stat_peak = g_mv.used;
    if (g_mv.stat_frames % 600 == 0) { MLOG("motion vectors: %llu frames, %.0f prepass draws a frame, %.1f moved, copies at most %.1f MB of %d, draws left with the camera's motion for lack of room %d, too far for one frame %d; camera cuts %d", (unsigned long long)g_mv.stat_frames, (double)g_mv.stat_draws / g_mv.stat_frames, (double)g_mv.stat_moved / g_mv.stat_frames, g_mv.stat_peak / 1048576.0, MV_SNAP_BYTES >> 20, g_mv.stat_full, g_mv.stat_far, g_mv.stat_cuts); }
    if (g_mv.dump_first >= 0 && (long)frame >= g_mv.dump_first && (long)frame < g_mv.dump_first + g_mv.dump_count) { g_mv.dump_vel = g_mv.vel; g_mv.dump_col = src; g_mv.dump_frame = frame; }
    return 1;
}

static struct { __strong id<MTL4FXTemporalScaler> s; __strong id<MTLTexture> out; __strong id<MTLRenderPipelineState> wb; NSUInteger aw, ah, ow, oh; MTLPixelFormat cf, df; int bad, reset, building, seen, still; float min_scale;
                __strong id<MTL4FXTemporalScaler> ns; __strong id<MTLTexture> nout; __strong id<MTLRenderPipelineState> nwb; NSUInteger naw, nah, now, noh; MTLPixelFormat ncf, ndf; float nmin; _Atomic int next_ok; } g_mt;
// The write-back of the temporal scaler's picture: as the spatial path's (rgb x the game's mask), plus a contrast-adaptive sharpen. The scaler's picture is clean but
// soft, and it has no sharpening of its own: each pixel is pushed away from its four neighbours by an amount that shrinks where the local contrast is already high
// How hard goes by the enlargement, as much as it reads about as sharp as Native with SMAA does (2026-11-04, town, fine detail against Native's 0.081: 0.079 at 75% with 6.5; at 99%
// 0.092 with 6.5, 0.072 with 9, so 8), without the crawl of the spatial scaler's
static double mt_sharp_div(double enlarge) { double t = (enlarge - 1) / 0.33; t = t < 0 ? 0 : t > 1 ? 1 : t; return 8.0 + (6.5 - 8.0) * t; }
static id<MTLRenderPipelineState> mt_wb_build(MTLPixelFormat fmt, double enlarge) {   // background queue
    NSError *e = nil;
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> c [[texture(0)]], texture2d<float> m [[texture(1)]], sampler sl [[sampler(0)]], sampler sn [[sampler(1)]]) {\n"
        "  int2 z = int2(c.get_width() - 1, c.get_height() - 1), q = clamp(int2(in.uv * float2(z + 1)), int2(0), z);\n"
        "  float3 e = c.read(uint2(q)).rgb, a = c.read(uint2(clamp(q + int2(0, -1), int2(0), z))).rgb, b = c.read(uint2(clamp(q + int2(0, 1), int2(0), z))).rgb, l = c.read(uint2(clamp(q + int2(-1, 0), int2(0), z))).rgb, r = c.read(uint2(clamp(q + int2(1, 0), int2(0), z))).rgb;\n"
        "  float3 mn = min(e, min(a, min(b, min(l, r)))), mx = max(e, max(a, max(b, max(l, r))));\n"
        "  float3 w = -sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4))) / %f;\n"
        "  return float4(max((e + w * (a + b + l + r)) / (1.0 + 4.0 * w), 0.0) * m.sample(sn, in.uv).r, 1.0); }\n", mt_sharp_div(enlarge)];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&e];
    if (!lib) { MLOG("【红】temporal scaler: the write-back did not compile (%s)", e.localizedDescription.UTF8String); return nil; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = fmt;
    id<MTLRenderPipelineState> r = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!r) MLOG("【红】temporal scaler: the write-back pipeline could not be built (format %lu): %s", (unsigned long)fmt, e.localizedDescription.UTF8String);
    return r;
}
static void mt_drop(void) {   // replay thread: released after the submissions that used them have run
    if (g_mt.s) MLOG("temporal scaler: put down");
    if (g_mt.s) defer_obj_release(g_mt.s);
    if (g_mt.out) defer_obj_release(g_mt.out);
    g_mt.s = nil; g_mt.out = nil; g_mt.wb = nil; g_mt.aw = g_mt.ah = g_mt.ow = g_mt.oh = 0;
}
// 1 = the temporal scaler can take this frame: built for the 3D target's allocation and this output, the velocities ready, and a depth prepass drawn this frame. Otherwise the
// frame uses the spatial scaler, and a build starts in the background when none is under way for this size
static int mt_ready(id<MTLTexture> src, MTLPixelFormat dfmt, NSUInteger ow, NSUInteger oh, NSUInteger cw) {
    if (!g_mt_want || g_mt.bad || !mv_init() || atomic_load(&g_mv_lib_ok) != 1) return 0;
    if (g_mv.frame[g_mv.cur] != atomic_load(&g_cur_frame) || !g_mv.n[g_mv.cur] || !g_mv.has_vp[g_mv.cur]) return 0;   // no prepass (a screen without the world): nothing to reproject
    float want_scale = (float)ow / src.width;
    #define MT_SAME(a, b, o, h, c, d) ((a) == src.width && (b) == src.height && (o) == ow && (h) == oh && (c) == src.pixelFormat && (d) == dfmt)
    if (g_mt.s && MT_SAME(g_mt.aw, g_mt.ah, g_mt.ow, g_mt.oh, g_mt.cf, g_mt.df)) return 1;
    if (atomic_load(&g_mt.next_ok) == 1) {
        if (MT_SAME(g_mt.naw, g_mt.nah, g_mt.now, g_mt.noh, g_mt.ncf, g_mt.ndf)) {   // install it: the history starts anew
            mt_drop(); res_add(g_mt.nout); res_commit();
            g_mt.s = g_mt.ns; g_mt.out = g_mt.nout; g_mt.wb = g_mt.nwb; g_mt.aw = g_mt.naw; g_mt.ah = g_mt.nah; g_mt.ow = g_mt.now; g_mt.oh = g_mt.noh; g_mt.cf = g_mt.ncf; g_mt.df = g_mt.ndf; g_mt.min_scale = g_mt.nmin; g_mt.reset = 1;
            g_mt.ns = nil; g_mt.nout = nil; g_mt.nwb = nil; atomic_store(&g_mt.next_ok, 0); g_mt.building = 0;
            return 1;
        }
        g_mt.ns = nil; g_mt.nout = nil; g_mt.nwb = nil; atomic_store(&g_mt.next_ok, 0); g_mt.building = 0;   // the size moved on while it was built
    }
    if (g_mt.building) return 0;
    if (cw > src.width) return 0;
    g_mt.building = 1;
    NSUInteger aw = src.width, ah = src.height; MTLPixelFormat cf = src.pixelFormat; id<MTL4Compiler> comp = mfx_compiler(); id fence = g_mv.fence;
    dispatch_async(mv_queue(), ^{
        uint64_t t0 = bp_now();
        MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
        d.colorTextureFormat = cf; d.depthTextureFormat = MTLPixelFormatDepth32Float_Stencil8; d.motionTextureFormat = MTLPixelFormatRG16Float; d.outputTextureFormat = MTLPixelFormatRGBA16Float;
        d.inputWidth = aw; d.inputHeight = ah; d.outputWidth = ow; d.outputHeight = oh; d.autoExposureEnabled = NO;
        // Frame Drops lowers the content inside this allocation (at most three steps of x 0.93, dm_s3lv.h)
        float smin = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:g_dev], smax = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev];
        float lo = MAX(smin, want_scale), hi = MIN(smax, want_scale / 0.8f); if (hi < lo) hi = lo;
        d.inputContentPropertiesEnabled = YES; d.inputContentMinScale = lo; d.inputContentMaxScale = hi;
        id<MTL4FXTemporalScaler> s = (id<MTL4FXTemporalScaler>)[d newTemporalScalerWithDevice:g_dev compiler:comp];
        id<MTLTexture> out = nil; id<MTLRenderPipelineState> wb = nil;
        if (s) { MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:ow height:oh mipmapped:NO];
                 td.storageMode = MTLStorageModePrivate; td.usage = s.outputTextureUsage | MTLTextureUsageShaderRead; out = [g_dev newTextureWithDescriptor:td]; wb = mt_wb_build(dfmt, (double)ow / aw); }
        if (!s || !out || !wb) { MLOG("【红】temporal scaler: could not be created (%lux%lu format %lu -> %lux%lu); the spatial scaler draws for the session", (unsigned long)aw, (unsigned long)ah, (unsigned long)cf, (unsigned long)ow, (unsigned long)oh); g_mt.bad = 1; return; }
        s.fence = fence; s.depthReversed = YES;
        g_mt.ns = s; g_mt.nout = out; g_mt.nwb = wb; g_mt.naw = aw; g_mt.nah = ah; g_mt.now = ow; g_mt.noh = oh; g_mt.ncf = cf; g_mt.ndf = dfmt; g_mt.nmin = lo;
        MLOG("temporal scaler: %lux%lu format %lu -> %lux%lu, scale %.2f..%.2f, built in %.1f ms (background)", (unsigned long)aw, (unsigned long)ah, (unsigned long)cf, (unsigned long)ow, (unsigned long)oh, lo, hi, (bp_now() - t0) / 1e6);
        DM_EV("mfx", "who:s op:s why:s iw:d ih:d ow:d oh:d ms:f", "temporal", "build", "first", (int)aw, (int)ah, (int)ow, (int)oh, (bp_now() - t0) / 1e6);
        atomic_store(&g_mt.next_ok, 1);
    });
    return 0;
    #undef MT_SAME
}
// After mv_resolve returned 1, in the same place as mfx_scale
static void mt_encode(id<MTLTexture> src, NSUInteger cw, NSUInteger ch) {
    int c = g_mv.cur; id<MTL4FXTemporalScaler> s = g_mt.s;
    s.colorTexture = src; s.depthTexture = g_mv.depth; s.motionTexture = g_mv.vel; s.outputTexture = g_mt.out;
    s.inputContentWidth = cw; s.inputContentHeight = ch; s.motionVectorScaleX = cw; s.motionVectorScaleY = ch;
    // the picture sits offset x clip.w to the right and up: in pixels, y down. This sign is the stable one (2026-11-04: frame-to-frame change of a still scene 0.0030 against 0.0033..0.0045 with either sign flipped)
    s.jitterOffsetX = g_mv.jit[c][0] * cw * 0.5f; s.jitterOffsetY = -g_mv.jit[c][1] * ch * 0.5f;
    s.reset = g_mt.reset || !g_mv.have_prev; g_mt.reset = 0;
    // The scaler can't work without the jitter, and the KJITTER slot is set on the thread that records commands (dm_jitter_refresh): say so when it never shows
    if (g_mv.jit[c][0] != 0 || g_mv.jit[c][1] != 0) { g_mt.still = 0; if (!g_mt.seen) { g_mt.seen = 1; MLOG("temporal scaler: camera jitter seen (frame %llu, %.3f %.3f px)", (unsigned long long)g_mv.frame[c], s.jitterOffsetX, s.jitterOffsetY); } }
    else if (++g_mt.still == 300) MLOG("【红】temporal scaler: 300 frames in a row without camera jitter: the picture stays soft (is the KJITTER slot in this product?)");
    [s encodeToCommandBuffer:g_cb];
}
// At the start of every submission (replay thread): whether the setting wants it now; put everything down when it no longer does
static void mv_sub_begin(void) {
    static int env = -1; if (env < 0) { const char *e = getenv("DM_MV"); env = e && atoi(e) > 0; }
    int want = dm_video_temporal_active();
    if (want != g_mt_want) { g_mt_want = want; MLOG("temporal scaler: %s", want ? "wanted" : "no longer wanted"); if (!want) mt_drop(); }
    g_mv_want = want || env;
    if (g_mv_done.count) { os_unfair_lock_lock(&g_mv_done_lk); NSArray *a = [g_mv_done copy]; [g_mv_done removeAllObjects]; os_unfair_lock_unlock(&g_mv_done_lk);
        for (NSArray *e in a) hm_put_id(&g_mv_pso, ((NSData *)e[0]).bytes, e[1]); }
    if (!g_mv_want && g_mv.inited > 0) {   // the copies and the velocity target go at once; the pipelines stay for the next time
        for (int i = 0; i < MV_RING; i++) { defer_obj_release(g_mv.snap[i]); g_mv.snap[i] = nil; }
        defer_obj_release(g_mv.unif); g_mv.unif = nil;
        if (g_mv.vel) { defer_obj_release(g_mv.vel); g_mv.vel = nil; }
        g_mv.depth = nil; g_mv_depth_ser = 0; g_mv.inited = 0; MLOG("motion vectors: off");
    }
}
