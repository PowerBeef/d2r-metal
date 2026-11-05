// Ray-traced key-light shadows and ambient occlusion, first version: a test switch (DM_RT=1), off for players. Included by nvn_mtl.m after nvn_mv.inc; replay thread. Needs hardware ray tracing and the motion
// vector pass, whose record of the depth prepass it reuses (mv_note_draw): every opaque object the prepass drew, with what it takes to draw it again.
//
// Before the first forward draw of a frame, rtx_frame:
//   1. draws again what is new or has moved, with the shader's own vertex code (dm_capture, dmsh_rules.py capture_entry) and no rasterization: skinning, cloth and the object's transform come out of the game's
//      code, as world positions in a buffer;
//   2. builds one primitive acceleration structure per object from those positions and the draw's own index buffer. What hasn't moved keeps the one it has, so a static scene costs nothing here;
//   3. builds one instance acceleration structure over them (the geometry is already in world space, the transforms are identity);
//   4. traces one ray per pixel of the depth buffer toward the key light, into the mask in the support buffer (support_buf), which every shader has bound;
//   5. with DM_RT_AO=<rays>, traces short rays around each pixel's normal (from depth) into a plane of their own, then a box over the 4 x 4 pattern of their directions into the plane behind the mask.
// The forward shaders read both at their own pixel (dmsh_rules.py rt_shadow_hook, rt_ao_hook): for layer 0 the darker of the mask and the game's shadow map (DM_RT=2: the mask alone), and the colour times the occlusion,
// all of it for now, not only its ambient part: a first look. Alpha-tested casters (foliage, hair) aren't in the structures, as opaque triangles would cast solid cards: they stay in the map. Characters (what the game draws
// into its layer 1, nvn_mv.inc rtu_*) keep that shadow and are left out of the key light's rays by their instance mask; they do occlude.
// DM_RT_BIAS=<world units> moves the ray's start off the surface, DM_RT_AO_R=<world units> is the occlusion's reach, DM_RT_AO_P=<power> its curve, DM_RT_DUMP=<frame>:<file.pgm> writes the planes as they stand.
@interface RtBlas : NSObject { @public __strong id<MTLAccelerationStructure> as; uint64_t last, hash; NSUInteger tris, bytes; int built, chr; } @end
@implementation RtBlas @end
enum { RT_WORLD = 1, RT_CHAR = 2, RT_MAX_INST = 8192, RT_CAP_BYTES = 32 << 20, RT_SCRATCH_BYTES = 96 << 20, RT_U_STRIDE = 256, RT_KEEP_FRAMES = 600 };
typedef struct { float ivp[16]; float view[4]; float dir[4]; uint32_t size[2]; float bias, maxd; float ao_r, ao_cam[3], ao_pow; uint32_t ao_rays; } rtx_u;
static struct {
    int inited, bad; NSMutableDictionary<NSNumber *, RtBlas *> *blas;
    __strong id<MTLBuffer> cap, scratch, inst, unif; __strong id<MTLAccelerationStructure> tlas; NSUInteger tlas_bytes; __strong id<MTLComputePipelineState> kpso, aopso, blurpso; __strong id<MTLBuffer> aoraw; __strong id<MTL4ArgumentTable> ct;
    uint64_t built_frame, frames, built_blas, built_tris, reused; float dir[3]; size_t as_bytes; float bias, ao_r, ao_p; int dir_logged, mode, ao_rays;
} g_rtx;
static const char *g_rtx_who = "";
static const char *g_rtx_msl =
    "#include <metal_stdlib>\n#include <metal_raytracing>\nusing namespace metal;\nusing namespace metal::raytracing;\n"
    "struct U { float4x4 ivp; float4 view; float4 dir; uint2 size; float bias; float maxd; float ao_r; packed_float3 ao_cam; float ao_pow; uint ao_rays; };\n"
    "kernel void rt_shadow(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  if (id.x >= u.size.x || id.y >= u.size.y) return; uint at = id.y * u.size.x + id.x; float d = dp.read(id);\n"
    "  if (d <= 0.0) { out[at] = 255; return; }\n"
    "  float2 ndc = float2(2 * (id.x + 0.5 - u.view.x) / u.view.z - 1, 1 - 2 * (id.y + 0.5 - u.view.y) / u.view.w);\n"
    "  float4 w = u.ivp * float4(ndc, d, 1); float3 p = w.xyz / w.w;\n"
    "  ray r; r.origin = p + u.dir.xyz * u.bias; r.direction = u.dir.xyz; r.min_distance = 0; r.max_distance = u.maxd;\n"
    "  intersector<instancing, triangle_data> i; i.accept_any_intersection(true); i.force_opacity(forced_opacity::opaque);\n"
    "  auto h = i.intersect(r, as, 1); out[at] = h.type == intersection_type::none ? 255 : 0; }\n"
    "constant float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};\n"
    "static float3 rt_world(constant U& u, uint2 id, float d) { float2 ndc = float2(2 * (id.x + 0.5 - u.view.x) / u.view.z - 1, 1 - 2 * (id.y + 0.5 - u.view.y) / u.view.w); float4 w = u.ivp * float4(ndc, d, 1); return w.xyz / w.w; }\n"
    "kernel void rt_ao(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  if (id.x >= u.size.x || id.y >= u.size.y) return; uint at = id.y * u.size.x + id.x; float d = dp.read(id);\n"
    "  if (d <= 0.0 || id.x == 0 || id.y == 0 || id.x + 1 >= u.size.x || id.y + 1 >= u.size.y) { out[at] = 255; return; }\n"
    "  float3 p = rt_world(u, id, d);\n"
    "  float dl = dp.read(uint2(id.x - 1, id.y)), dr = dp.read(uint2(id.x + 1, id.y)), du = dp.read(uint2(id.x, id.y - 1)), dd = dp.read(uint2(id.x, id.y + 1));\n"
    "  float3 ax = abs(dl - d) < abs(dr - d) ? p - rt_world(u, uint2(id.x - 1, id.y), dl) : rt_world(u, uint2(id.x + 1, id.y), dr) - p;\n"
    "  float3 ay = abs(du - d) < abs(dd - d) ? p - rt_world(u, uint2(id.x, id.y - 1), du) : rt_world(u, uint2(id.x, id.y + 1), dd) - p;\n"
    "  float3 n = cross(ax, ay); float nl = length(n); if (nl < 1e-9) { out[at] = 255; return; } n /= nl; float3 cam = float3(u.ao_cam); if (dot(n, cam) < 0) n = -n;\n"
    "  float3 t = abs(n.y) < 0.9 ? normalize(cross(n, float3(0, 1, 0))) : normalize(cross(n, float3(1, 0, 0))); float3 b = cross(n, t);\n"
    "  float dith = (bayer[(id.y & 3) * 4 + (id.x & 3)] + 0.5) / 16.0; float occ = 0;\n"
    "  intersector<instancing, triangle_data> i; i.force_opacity(forced_opacity::opaque);\n"
    "  for (uint k = 0; k < u.ao_rays; k++) {\n"
    "    float a = 6.2831853 * fract(dith + k * 0.61803398), rr = sqrt((k + fract(dith * 7.0 + 0.5)) / float(u.ao_rays)); float3 dir = normalize(t * (rr * cos(a)) + b * (rr * sin(a)) + n * sqrt(max(0.0, 1 - rr * rr)));\n"
    "    ray r; r.origin = p + n * u.bias; r.direction = dir; r.min_distance = 0; r.max_distance = u.ao_r;\n"
    "    auto h = i.intersect(r, as, 0xff); if (h.type != intersection_type::none) { float f = 1 - h.distance / u.ao_r; occ += f * f; } }\n"
    "  out[at] = uchar(clamp(1 - occ / float(u.ao_rays), 0.0, 1.0) * 255 + 0.5); }\n"
    "kernel void rt_ao_blur(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], constant U& u [[buffer(0)]], const device uchar *in [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  if (id.x >= u.size.x || id.y >= u.size.y) return; uint at = id.y * u.size.x + id.x; float d = dp.read(id), sum = 0, wt = 0;\n"
    "  for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) { int2 q = int2(id) + int2(x, y); if (q.x < 0 || q.y < 0 || q.x >= int(u.size.x) || q.y >= int(u.size.y)) continue; float dq = dp.read(uint2(q)); float w = abs(dq - d) < 0.0015 ? 1.0 : 0.0; sum += w * float(in[uint(q.y) * u.size.x + uint(q.x)]); wt += w; }\n"
    "  float v = (wt > 0 ? sum / wt : float(in[at])) / 255.0; out[at] = uchar(pow(v, u.ao_pow) * 255 + 0.5); }\n";
int rtx_want(void) {
    static int w = -1; if (w >= 0) return w;
    const char *e = getenv("DM_RT"); w = e && atoi(e) > 0 && [MTLCreateSystemDefaultDevice() supportsRaytracing];
    if (e && atoi(e) > 0 && !w) MLOG("ray tracing: DM_RT is set, and this GPU has none");
    return w;
}
static int rtx_init(void) {
    if (g_rtx.inited) return !g_rtx.bad; g_rtx.inited = 1;
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(g_rtx_msl) options:nil error:&e]; id<MTLFunction> fn = [lib newFunctionWithName:@"rt_shadow"], fa = [lib newFunctionWithName:@"rt_ao"], fb = [lib newFunctionWithName:@"rt_ao_blur"];
    g_rtx.kpso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    if (getenv("DM_RT_AO") && atoi(getenv("DM_RT_AO")) > 0) { g_rtx.ao_rays = atoi(getenv("DM_RT_AO")); const char *r = getenv("DM_RT_AO_R"), *pw = getenv("DM_RT_AO_P"); g_rtx.ao_r = r ? (float)atof(r) : 4.0f; g_rtx.ao_p = pw ? (float)atof(pw) : 2.0f;
        g_rtx.aopso = fa ? [g_dev newComputePipelineStateWithFunction:fa error:&e] : nil; g_rtx.blurpso = fb ? [g_dev newComputePipelineStateWithFunction:fb error:&e] : nil; g_rtx.aoraw = [g_dev newBufferWithLength:(NSUInteger)3840 * 2160 options:MTLResourceStorageModePrivate];
        if (!g_rtx.aopso || !g_rtx.blurpso || !g_rtx.aoraw) { MLOG("【红】ray tracing: can't start ambient occlusion (%s)", e.localizedDescription.UTF8String); g_rtx.ao_rays = 0; } else { res_add(g_rtx.aoraw); MLOG("ray tracing: ambient occlusion on, %d rays, reach %.2f, power %.2f", g_rtx.ao_rays, g_rtx.ao_r, g_rtx.ao_p); } }
    g_rtx.cap = [g_dev newBufferWithLength:RT_CAP_BYTES options:MTLResourceStorageModePrivate]; g_rtx.scratch = [g_dev newBufferWithLength:RT_SCRATCH_BYTES options:MTLResourceStorageModePrivate];
    g_rtx.inst = [g_dev newBufferWithLength:RT_MAX_INST * sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) * MV_RING options:MTLResourceStorageModeShared]; g_rtx.unif = [g_dev newBufferWithLength:RT_U_STRIDE * MV_RING options:MTLResourceStorageModeShared];
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; ad.maxBufferBindCount = 3; g_rtx.ct = [g_dev newArgumentTableWithDescriptor:ad error:&e];
    if (!g_rtx.kpso || !g_rtx.cap || !g_rtx.scratch || !g_rtx.inst || !g_rtx.unif || !g_rtx.ct) { MLOG("【红】ray tracing: can't start (%s)", e.localizedDescription.UTF8String); g_rtx.bad = 1; return 0; }
    res_add(g_rtx.cap); res_add(g_rtx.scratch); res_add(g_rtx.inst); res_add(g_rtx.unif); res_commit();
    g_rtx.blas = [NSMutableDictionary dictionary];
    const char *b = getenv("DM_RT_BIAS"); g_rtx.bias = b ? (float)atof(b) : 0.02f; g_rtx.mode = atoi(getenv("DM_RT"));
    MLOG("ray tracing: on, ray start %.3f toward the light", g_rtx.bias);
    return 1;
}
// A moving draw's capture pipeline: the shader's dm_capture entry, no rasterization. Built on the background queue, as mv_pso
static id<MTLRenderPipelineState> rtx_cap_pso(NvShader *vs) {
    uint32_t cfmt[8] = {0}; pso_key k; pso_key_fill(&k, vs, nil, nil, cfmt, MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1, NULL);
    k.blend_en = 0; k.chmask = 0xfffffffe; k.a2c = 0; memset(k.blend, 0, sizeof k.blend);
    id ps = hm_get_id(&g_mv_pso, &k);
    if (ps) return ps == g_mv_marker ? nil : ps;
    if (!g_mv_marker) { g_mv_marker = [NSObject new]; g_mv_done = [NSMutableArray array]; }
    hm_put_id(&g_mv_pso, &k, g_mv_marker);
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, nil, nil, 0);
    d.vertexFunction = vs->cap_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO;
    for (int i = 0; i < 8; i++) d.colorAttachments[i].pixelFormat = MTLPixelFormatInvalid;
    d.depthAttachmentPixelFormat = MTLPixelFormatInvalid; d.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k]; NSString *who = @(vs->key);
    dispatch_async(mv_queue(), ^{ NSError *e = nil; id<MTLRenderPipelineState> r = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!r) { MLOG("【红】ray tracing: capture pipeline for %s failed (%s)", who.UTF8String, e.localizedDescription.UTF8String); return; }
        os_unfair_lock_lock(&g_mv_done_lk); [g_mv_done addObject:@[key, r]]; os_unfair_lock_unlock(&g_mv_done_lk); });
    return nil;
}
static uint64_t rtx_fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
// What says an object is where it was: its position and its bones
static uint64_t rtx_state_hash(const mv_draw *d) {
    const uint8_t *base = g_mv.snap[g_mv.ri].contents; float w[6]; mv_where(d, base, w); uint64_t h = rtx_fnv(1469598103934665603ull, w, 3 * sizeof(float));
    const mv_cb *b = mv_cb_of(d, 2); if (b) h = rtx_fnv(h, base + b->off, b->size);
    return h;
}
static uint32_t rtx_vmax(const mv_draw *d) {
    uint32_t m = 0; if (d->itype == 1) { const uint16_t *p = d->ib_cpu; for (int i = 0; i < d->count; i++) if (p[i] > m) m = p[i]; } else { const uint32_t *p = d->ib_cpu; for (int i = 0; i < d->count; i++) if (p[i] > m) m = p[i]; }
    return m;
}
// Replay thread, at a draw that reads the shadow map, before its pass opens: this frame's mask, once. Leaves the state so that the draw goes on as usual
static void rtx_frame(void) {
    uint64_t frame = atomic_load(&g_cur_frame); int c = g_mv.cur;
    if (g_rtx.built_frame == frame || g_mv.frame[c] != frame || !g_mv.n[c] || !g_mv.depth || !g_mv.has_vp[c] || !rtx_init()) return;
    g_rtx.built_frame = frame;
    { static long want = -2; static char path[512]; if (want == -2) { const char *e = getenv("DM_RT_DUMP"), *q = e ? strchr(e, ':') : NULL; want = q ? atol(e) : -1; if (q) strlcpy(path, q + 1, sizeof path); }   // DM_RT_DUMP=<frame>:<file.pgm>: the mask as it stands, for looking at it alone
      if (want >= 0 && (long)frame >= want) { want = -1; const uint32_t *h = (const uint32_t *)((const uint8_t *)support_buf().contents + 2048); FILE *f = h[0] ? fopen(path, "wb") : NULL; if (f) { fprintf(f, "P5\n%u %u\n255\n", h[1], h[2]); fwrite((const uint8_t *)support_buf().contents + 4096, 1, (size_t)h[1] * h[2], f); fclose(f); MLOG("ray tracing: mask written to %s", path); if (h[3]) { char p2[540]; snprintf(p2, sizeof p2, "%s.ao.pgm", path); f = fopen(p2, "wb"); if (f) { fprintf(f, "P5\n%u %u\n255\n", h[1], h[2]); fwrite((const uint8_t *)support_buf().contents + 4096 + (size_t)h[1] * h[2], 1, (size_t)h[1] * h[2], f); fclose(f); } } } } }
    // The key light: the axis of the game's own shadow view (scene block rows 14..17, columns), so that the rays agree with its map. Those rows take a position in view space, as all the forward lighting does, and
    // the structures are in world space, hence the camera's rotation. Rows 0 and 1 of that matrix say where in the map a point lands: what they leave unchanged is the light's axis. The light comes from above
    float q[12]; for (int r = 0; r < 3; r++) for (int i = 0; i < 3; i++) if (!ubo_float(1, (uint16_t)(10 << 8 | (14 + r) << 2 | i), &q[r * 3 + i])) { static int once; if (!once++) MLOG("ray tracing: can't read the shadow view, shader %s", g_rtx_who); return; }
    double r0[3] = {q[0], q[3], q[6]}, r1[3] = {q[1], q[4], q[7]}, ax[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    const float *vm = g_mv.view_m[c]; double wd[3]; for (int i = 0; i < 3; i++) wd[i] = vm[i * 4] * ax[0] + vm[i * 4 + 1] * ax[1] + vm[i * 4 + 2] * ax[2];
    double len = sqrt(wd[0] * wd[0] + wd[1] * wd[1] + wd[2] * wd[2]); if (len < 1e-12) return; if (wd[1] < 0) len = -len;
    for (int i = 0; i < 3; i++) g_rtx.dir[i] = (float)(wd[i] / len);
    if (!g_rtx.dir_logged++) MLOG("ray tracing: toward the key light %.3f %.3f %.3f", g_rtx.dir[0], g_rtx.dir[1], g_rtx.dir[2]);
    double vd[16], inv[16]; for (int i = 0; i < 16; i++) vd[i] = g_mv.vp[c][i]; if (!mat4_inv(vd, inv)) return;
    rtx_u u; for (int i = 0; i < 16; i++) u.ivp[i] = (float)inv[i];
    for (int i = 0; i < 4; i++) u.view[i] = (float)g_mv.view[i];
    for (int i = 0; i < 3; i++) u.dir[i] = g_rtx.dir[i]; u.dir[3] = 0; u.size[0] = (uint32_t)g_mv.depth.width; u.size[1] = (uint32_t)g_mv.depth.height; u.bias = g_rtx.bias; u.maxd = 1000; u.ao_r = g_rtx.ao_r; u.ao_rays = (uint32_t)g_rtx.ao_rays; u.ao_pow = g_rtx.ao_p;
    { double c2[3]; for (int i = 0; i < 3; i++) c2[i] = inv[8 + i]; double ln = sqrt(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]); for (int i = 0; i < 3; i++) u.ao_cam[i] = (float)(ln > 0 ? c2[i] / ln : 0); }   // the orthographic camera's axis: depth grows toward the viewer
    if (rtx_mask_bytes() < 4096 + (size_t)u.size[0] * u.size[1] * 2) { if (!g_rtx.bad++) MLOG("【红】ray tracing: the mask doesn't fit a %ux%u picture", u.size[0], u.size[1]); return; }
    NSUInteger ui = (NSUInteger)g_mv.ri * RT_U_STRIDE; memcpy((uint8_t *)g_rtx.unif.contents + ui, &u, sizeof u); uint64_t u_gpu = g_rtx.unif.gpuAddress + ui;
    rp_end();
    // 1. what is new or has moved, drawn again with no rasterization
    enum { MAXB = 2048 }; static mv_draw *todo[MAXB]; static uint64_t toff[MAXB], tlen[MAXB], thash[MAXB]; static RtBlas *tb[MAXB]; int nt = 0; size_t cap_at = 0, scr_at = 0;
    static uint64_t ikey[RT_MAX_INST]; int ni = 0;
    for (int i = 0; i < g_mv.n[c] && ni < RT_MAX_INST; i++) {
        mv_draw *d = &g_mv.d[c][i]; if (!d->rt_opaque || !d->ib_cpu || d->count < 3 || (d->rt_char && !g_rtx.ao_rays)) continue;
        NSNumber *k = @(d->key); RtBlas *b = g_rtx.blas[k]; uint64_t h = rtx_state_hash(d); if (b) b->chr = d->rt_char;
        if (b && b->built && b->hash == h) { b->last = frame; ikey[ni++] = d->key; g_rtx.reused++; continue; }
        if (!d->cap_pso || nt >= MAXB) { if (b && b->built) { b->last = frame; ikey[ni++] = d->key; } continue; }   // its pipeline is still building: last frame's shape, or none yet
        uint32_t vm = rtx_vmax(d); uint64_t len = (uint64_t)(vm + 1) * 16; if (cap_at + len > RT_CAP_BYTES) { if (b && b->built) { b->last = frame; ikey[ni++] = d->key; } continue; }
        if (!b) { b = [RtBlas new]; g_rtx.blas[k] = b; b->chr = d->rt_char; }
        todo[nt] = d; toff[nt] = cap_at; tlen[nt] = len; thash[nt] = h; tb[nt] = b; nt++; cap_at += len; ikey[ni++] = d->key; b->last = frame;
    }
    if (nt) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new]; rp.renderTargetWidth = 16; rp.renderTargetHeight = 16; rp.defaultRasterSampleCount = 1;
        RENC en = [g_cb renderCommandEncoderWithDescriptor:rp];
        if (en) {
            [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
            [en setArgumentTable:g_mv.vt atStages:MTLRenderStageVertex]; [g_mv.vt setAddress:g_mv.zero.gpuAddress atIndex:VB_ZERO]; [g_mv.vt setAddress:u_gpu atIndex:3];
            uint8_t *ab = g_mv.snap[g_mv.ri].contents; uint64_t ab_gpu = g_mv.gpu[g_mv.ri];
            for (int i = 0; i < nt; i++) {
                mv_draw *d = todo[i]; size_t need = (d->vs->arg_len + 15) & ~(size_t)15; if (g_mv.used + need > MV_SNAP_BYTES) { nt = i; break; }
                uint64_t *cs = (uint64_t *)(ab + g_mv.used); uint64_t cg = ab_gpu + g_mv.used; g_mv.used += need; memset(cs, 0, need);
                for (int k = 0; k < d->ncb; k++) { const mv_cb *a = &d->cb[k]; cs[a->aid] = ab_gpu + a->off; }
                [en setRenderPipelineState:d->cap_pso];
                for (int s = 0; s < 15; s++) if (d->vb[s]) [g_mv.vt setAddress:d->vb[s] atIndex:16 + s];
                [g_mv.vt setAddress:cg atIndex:d->vs->rf.abi]; [g_mv.vt setAddress:g_rtx.cap.gpuAddress + toff[i] atIndex:2];
                [en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d->count indexType:d->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:d->ib_gpu indexBufferLength:d->ib_len instanceCount:1 baseVertex:d->base_vertex baseInstance:d->base_instance];
            }
            enc_host_r_end(en);
        } else nt = 0;
    }
    CENC ce = enc_c_begin(NULL);
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    // 2. their structures
    for (int i = 0; i < nt; i++) {
        mv_draw *d = todo[i]; RtBlas *b = tb[i];
        MTL4AccelerationStructureTriangleGeometryDescriptor *g = [MTL4AccelerationStructureTriangleGeometryDescriptor new];
        g.vertexBuffer = MTL4BufferRangeMake(g_rtx.cap.gpuAddress + toff[i], tlen[i]); g.vertexFormat = MTLAttributeFormatFloat3; g.vertexStride = 16;
        g.indexBuffer = MTL4BufferRangeMake(d->ib_gpu, d->ib_len); g.indexType = d->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32; g.triangleCount = (NSUInteger)d->count / 3; g.opaque = YES;
        MTL4PrimitiveAccelerationStructureDescriptor *desc = [MTL4PrimitiveAccelerationStructureDescriptor new]; desc.geometryDescriptors = @[g]; desc.usage = MTLAccelerationStructureUsagePreferFastBuild;
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:desc];
        if (scr_at + sz.buildScratchBufferSize > RT_SCRATCH_BYTES) { if (!b->built) { for (int j = 0; j < ni; j++) if (ikey[j] == d->key) { ikey[j] = ikey[--ni]; break; } } continue; }
        if (!b->as || b->bytes < sz.accelerationStructureSize) { if (b->as) { g_rtx.as_bytes -= b->bytes; fg_bury(b->as); } b->as = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize]; b->bytes = sz.accelerationStructureSize; g_rtx.as_bytes += b->bytes; res_add(b->as); }
        if (!b->as) continue;
        [ce buildAccelerationStructure:b->as descriptor:desc scratchBuffer:MTL4BufferRangeMake(g_rtx.scratch.gpuAddress + scr_at, sz.buildScratchBufferSize)];
        scr_at += (sz.buildScratchBufferSize + 255) & ~(size_t)255; b->built = 1; b->hash = thash[i]; b->tris = g.triangleCount; g_rtx.built_blas++; g_rtx.built_tris += g.triangleCount;
    }
    if (nt) res_commit();
    [ce barrierAfterEncoderStages:MTLStageAccelerationStructure beforeEncoderStages:MTLStageAccelerationStructure | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    // 3. the scene
    MTLIndirectAccelerationStructureInstanceDescriptor *ins = (MTLIndirectAccelerationStructureInstanceDescriptor *)g_rtx.inst.contents + (NSUInteger)g_mv.ri * RT_MAX_INST; int nu = 0;
    for (int i = 0; i < ni; i++) { RtBlas *b = g_rtx.blas[@(ikey[i])]; if (!b || !b->built || !b->as) continue;
        MTLIndirectAccelerationStructureInstanceDescriptor *x = &ins[nu++]; memset(x, 0, sizeof *x); x->transformationMatrix.columns[0] = (MTLPackedFloat3){1, 0, 0}; x->transformationMatrix.columns[1] = (MTLPackedFloat3){0, 1, 0}; x->transformationMatrix.columns[2] = (MTLPackedFloat3){0, 0, 1};
        x->mask = b->chr ? RT_CHAR : RT_WORLD; x->options = MTLAccelerationStructureInstanceOptionOpaque; x->accelerationStructureID = b->as.gpuResourceID; }
    MTL4InstanceAccelerationStructureDescriptor *td = [MTL4InstanceAccelerationStructureDescriptor new];
    td.instanceDescriptorBuffer = MTL4BufferRangeMake(g_rtx.inst.gpuAddress + (uint64_t)g_mv.ri * RT_MAX_INST * sizeof *ins, (uint64_t)nu * sizeof *ins); td.instanceDescriptorStride = sizeof *ins; td.instanceCount = (NSUInteger)nu; td.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect; td.usage = MTLAccelerationStructureUsagePreferFastBuild;
    MTLAccelerationStructureSizes ts = [g_dev accelerationStructureSizesWithDescriptor:td];
    if (!g_rtx.tlas || g_rtx.tlas_bytes < ts.accelerationStructureSize) { if (g_rtx.tlas) fg_bury(g_rtx.tlas); g_rtx.tlas_bytes = ts.accelerationStructureSize * 2; g_rtx.tlas = [g_dev newAccelerationStructureWithSize:g_rtx.tlas_bytes]; res_add(g_rtx.tlas); res_commit(); }
    if (scr_at + ts.buildScratchBufferSize <= RT_SCRATCH_BYTES && nu) {
        [ce buildAccelerationStructure:g_rtx.tlas descriptor:td scratchBuffer:MTL4BufferRangeMake(g_rtx.scratch.gpuAddress + scr_at, ts.buildScratchBufferSize)];
        [ce barrierAfterEncoderStages:MTLStageAccelerationStructure beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        // 4. the mask
        [ce setArgumentTable:g_rtx.ct]; [g_rtx.ct setTexture:g_mv.depth.gpuResourceID atIndex:0]; [g_rtx.ct setAddress:u_gpu atIndex:0]; [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1]; [g_rtx.ct setAddress:support_gpu() + 4096 atIndex:2];
        [ce setComputePipelineState:g_rtx.kpso]; [ce dispatchThreads:MTLSizeMake(u.size[0], u.size[1], 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
        if (g_rtx.ao_rays) {   // 5. the occlusion: its rays into a plane of its own, then a box over the 4 x 4 pattern of their directions, into the plane behind the mask
            uint64_t plane = support_gpu() + 4096 + (uint64_t)u.size[0] * u.size[1];
            [g_rtx.ct setAddress:g_rtx.aoraw.gpuAddress atIndex:2]; [ce setComputePipelineState:g_rtx.aopso]; [ce dispatchThreads:MTLSizeMake(u.size[0], u.size[1], 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            [g_rtx.ct setAddress:g_rtx.aoraw.gpuAddress atIndex:1]; [g_rtx.ct setAddress:plane atIndex:2]; [ce setComputePipelineState:g_rtx.blurpso]; [ce dispatchThreads:MTLSizeMake(u.size[0], u.size[1], 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
        }
        uint32_t *hdr = (uint32_t *)((uint8_t *)support_buf().contents + 2048); hdr[1] = u.size[0]; hdr[2] = u.size[1]; hdr[3] = g_rtx.ao_rays > 0; hdr[0] = (uint32_t)g_rtx.mode;
    }
    m4_end(ce);
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    if (++g_rtx.frames % 600 == 1) MLOG("ray tracing: frame %llu, %d objects, %d rebuilt this frame; %llu builds, %llu triangles, %llu reused so far; structures %.1f MiB", (unsigned long long)frame, nu, nt, (unsigned long long)g_rtx.built_blas, (unsigned long long)g_rtx.built_tris, (unsigned long long)g_rtx.reused, g_rtx.as_bytes / 1048576.0);
    // what hasn't been drawn for a while goes
    if (g_rtx.frames % 120 == 0) { NSMutableArray *gone = [NSMutableArray array]; for (NSNumber *k in g_rtx.blas) { RtBlas *b = g_rtx.blas[k]; if (frame - b->last > RT_KEEP_FRAMES) { if (b->as) { g_rtx.as_bytes -= b->bytes; fg_bury(b->as); } [gone addObject:k]; } } [g_rtx.blas removeObjectsForKeys:gone]; }
}
static void rtx_who(const char *k) { g_rtx_who = k; }
