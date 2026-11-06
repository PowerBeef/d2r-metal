// Frame generation: the screen shows twice as many frames as the game draws, the extra ones made by MetalFX's frame interpolator. Included by nvn_mtl.m; replay thread
// unless a comment says otherwise. It is always on wherever the system offers it, with no row (dm_video.c dm_video_fg_active; DM_FG=0/1 forces it), and it needs the temporal scaler
// (nvn_mv.inc): its motion vectors, depth and camera offset are the interpolator's inputs. The test switch DM_FPS=30 has no room for it.
//
// The game keeps the world and the interface apart until the last moment: the world's Final Result and the Offscreen UI target are blended by its compose pass (ps_compose_p,
// key 9a650561e5c40791, main+0x16831a4) into the swap texture. So at that draw fg_compose interpolates the world alone, between last frame's Final Result and this one, and
// blends this frame's interface over the result the way the compose shader does. The interface is never warped. Screens where the compose shader does more than that
// (colour-blind correction, HDR) get no generated frames.
//
// Each frame then queues two presents (mtl_present_fg): the generated one, then the real one, one display-link tick apart. Since a real frame takes two ticks of the screen's
// rate, the game's own back-pressure (the swap texture waits for its last present) halves its rate without a limiter: 60 Hz shows 60, the game draws 30.
//
// The interpolator loses thin things that move against the scenery: while the hero runs, the camera follows her and the ground slides behind her staff, and in the generated frame the staff is
// nearly gone, so at the shown rate it is solid every other frame and looks ghosted and doubled. The velocity target knows those pixels exactly, so they go back in (DM_FG_FORE=0 leaves them out):
// fg_splat moves every pixel of a thing that moves on its own (the mask mv_resolve draws) half way back along its motion and records, per pixel of the generated frame, the nearest source; the blend takes those pixels from the
// real frame. Only this frame's real picture is used, so what is hidden in it stays as the interpolator made it. The world's camera is orthographic, so its motion is the same for every pixel.
// Putting them back isn't enough: the interpolator also keeps a pale copy of them, carried along with the ground, half a camera step away. So it never gets to see them: fg_plate paints the moving
// pixels (and a little around them, where the antialiasing mixes them with the ground) out of the picture it interpolates, using last frame's painted picture one camera step on, which is the ground
// that slid under them; its motion input is the camera's alone, and its depth is flat: the real depth would still show it their shape (a pale silhouette just ahead of a running hero).
//
// DM_MV_DUMP also writes the frame's generated world (_gen, the interpolator's own output), real world (_real), generated frame with the interface (_comp, what is shown) and real frame (_swap) as raw files.
__attribute__((weak)) int dm_video_fg_active(void) { return 0; }
// Any thread, before the renderer has a device too (the recommendation asks at launch). DM_TEST_NO_FG=1: as on a chip without it
int mtl_mfx_fg_supported(void) {
    static int ok; static dispatch_once_t once;
    dispatch_once(&once, ^{ id<MTLDevice> d = MTLCreateSystemDefaultDevice(); ok = d && !getenv("DM_TEST_NO_FG") && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:d];
        MLOG("MetalFX: Metal 4 frame interpolator supported: %s", ok ? "yes" : "no"); });
    return ok;
}
static int fg_on(void) { return g_mt_want && dm_video_fg_active() && mtl_fps_target() != 30; }
enum { FG_RING = 4 };
static struct {
    __strong id<MTL4FXFrameInterpolator> s; __strong id<MTLTexture> wout; __strong id<MTLTexture> out[FG_RING]; __strong id<MTLTexture> rv[FG_RING]; __strong id<MTLRenderPipelineState> pso; __strong id<MTL4ArgumentTable> args; __strong id<MTLFence> fence;
    __strong id<MTLComputePipelineState> cpso, ncpso; __strong id<MTLBuffer> map, nmap; __strong id<MTLTexture> plate[2], nplate[2], pmv, npmv, pdep, npdep; int pdep_clear; __strong id<MTLRenderPipelineState> ppso, nppso, mpso, nmpso; __strong id<MTL4ArgumentTable> cargs, pargs; int fore, platei;   // the moving pixels' way back in; fore 0 = DM_FG_FORE=0
    NSUInteger iw, ih, ow, oh; MTLPixelFormat cf, bf, vf; void *scaler; int bad, building, reset, idx, ready; uint64_t world_frame, ready_frame; double last_t;   // scaler: the temporal scaler it was built with, never followed
    __strong id<MTL4FXFrameInterpolator> ns; __strong id<MTLTexture> nwout; __strong id<MTLTexture> nout[FG_RING]; __strong id<MTLTexture> nrv[FG_RING]; __strong id<MTLRenderPipelineState> npso; NSUInteger niw, nih, now, noh; MTLPixelFormat ncf, nbf, nvf; void *nscaler; _Atomic int next_ok;
    uint64_t made, skipped;
} g_fg;
static void fgp_reset(void);
static _Atomic int g_fg_fifo;   // 1 = the display link takes one queued present per tick instead of the newest

static const char *g_fg_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
    // as the game's compose: the interface's alpha is how much of the world shows through
    // Generated frames come out softer than real ones where things move (scenery at a run keeps about 87% of a real frame's detail, so the picture would pulse at half the shown rate) and match them
    // where nothing moves, so the world gets a contrast-adaptive sharpen that follows each pixel's motion, here, under the interface: the less room a pixel has before clipping, the less it moves.
    // Strength 0.2 brings a run to about 1.0 (0.15: 0.95, 0.3: 1.10); without the painted input, DM_FG_FORE=0, the interpolator is softer (76%) and it takes 0.5
    // A kernel, once per pixel of the picture: the map holds, per pixel of the generated frame, depth (nearest = smallest, the game's depth is reversed) and the offset to the source
    "struct FU { float2 cam; float2 size; float hist; };\n"
    "fragment float2 fg_cam(constant FU& f [[buffer(0)]]) { return f.cam; }\n"
    "fragment float4 fg_plate(V in [[stage_in]], texture2d<float> c [[texture(0)]], texture2d<float> last [[texture(1)]], texture2d<float> m [[texture(2)]], constant FU& f [[buffer(0)]], sampler s [[sampler(0)]]) {\n"
    "  float2 t = 1.0 / float2(m.get_width(), m.get_height());\n"
    "  if (f.hist > 0.5) { const float2 o[9] = {float2(0, 0), float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2), float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};\n"
    "    for (int i = 0; i < 9; i++) if (m.sample(s, in.uv + o[i] * t).r > 0.0) return last.sample(s, in.uv + f.cam); }\n"
    "  return c.sample(s, in.uv); }\n"
    // Each moving pixel covers what it and its right and lower neighbours cover half way back, as a warped mesh does, so that it leaves no holes where it stretches; a neighbour too far away is another
    // part (an arm over the body) and doesn't count. A pixel just outside a moving one goes along with it: that is where the antialiasing mixed them
    "static float2 fg_dst(int2 p, float2 v, float2 size) { return float2(p) + 0.5 + 0.5 * v * size; }\n"
    "static float2 fg_vel(int2 p, texture2d<float> mv, float2 size) { return mv.read(uint2((float2(p) + 0.5) * float2(mv.get_width(), mv.get_height()) / size)).xy; }\n"
    "static bool fg_moves(int2 p, texture2d<float> m, float2 size) { return m.read(uint2((float2(p) + 0.5) * float2(m.get_width(), m.get_height()) / size)).r > 0.5; }\n"
    "kernel void fg_splat(uint2 id [[thread_position_in_grid]], texture2d<float> mv [[texture(0)]], depth2d<float> dp [[texture(1)]], texture2d<float> m [[texture(2)]], constant FU& f [[buffer(0)]], device atomic_uint *map [[buffer(1)]]) {\n"
    "  int2 c = int2(id), sz = int2(f.size); float2 v = float2(0); int2 from = c; bool own = fg_moves(c, m, f.size);\n"
    "  if (!own) { const int2 o[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)}; bool near = false;\n"
    "    for (int i = 0; i < 4 && !near; i++) { int2 n = c + o[i]; if (n.x < 0 || n.y < 0 || n.x >= sz.x || n.y >= sz.y) continue; if (fg_moves(n, m, f.size)) { near = true; v = fg_vel(n, mv, f.size); from = n; } }\n"
    "    if (!near) return; } else v = fg_vel(c, mv, f.size);\n"
    "  float2 d0 = fg_dst(c, v, f.size), lo = d0, hi = d0;\n"
    "  if (own) for (int i = 0; i < 3; i++) { int2 n = c + int2(i != 1, i != 0); if (n.x >= sz.x || n.y >= sz.y || !fg_moves(n, m, f.size)) continue; float2 w = fg_vel(n, mv, f.size);\n"
    "    float2 d = fg_dst(n, w, f.size); if (all(abs(d - d0) < FG_FORE_REACH)) { lo = min(lo, d); hi = max(hi, d); } }\n"
    "  uint2 ip = uint2((float2(from) + 0.5) * float2(mv.get_width(), mv.get_height()) / f.size); uint key = uint((1.0 - saturate(dp.read(ip))) * 65535.0); if (!own && key < 65535u) key++;\n"
    "  int2 a = max(int2(floor(lo)), int2(0)), b = min(int2(floor(hi)), sz - 1);\n"
    "  for (int y = a.y; y <= b.y; y++) for (int x = a.x; x <= b.x; x++) { int2 off = c - int2(x, y); if (abs(off.x) > 127 || abs(off.y) > 127) continue;\n"
    "    atomic_fetch_min_explicit(map + y * sz.x + x, key << 16 | uint(off.x + 128) << 8 | uint(off.y + 128), memory_order_relaxed); } }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> w [[texture(0)]], texture2d<float> u [[texture(1)]], texture2d<float> mv [[texture(2)]], texture2d<float> real [[texture(3)]], constant uint *map [[buffer(0)]], sampler s [[sampler(0)]]) {\n"
    "  float4 ui = u.sample(s, in.uv); float2 t = 1.0 / float2(w.get_width(), w.get_height()); float3 c = w.sample(s, in.uv).rgb;\n"
    "#if FG_FORE\n"
    "  { int W = int(w.get_width()), H = int(w.get_height()); int2 p = int2(in.p.xy); uint e = map[p.y * W + p.x];\n"
    // the source's own motion gives where it falls exactly, between pixels: rounded to whole pixels, moving things come out jagged in generated frames alone
    "    if (e != 0xffffffffu) { int2 off = int2((e >> 8) & 255u, e & 255u) - 128; float2 sz = float2(W, H), vs = float2(mv.get_width(), mv.get_height()); float2 v = mv.read(uint2((float2(p + off) + 0.5) * vs / sz)).xy;\n"
    "      float2 at = in.uv - 0.5 * v; float2 near = (float2(p + off) + 0.5) * t; if (any(abs(at - near) > 1.5 * t)) at = near;\n"
    "      return float4(mix(ui.rgb, real.sample(s, at).rgb, ui.a), 1.0); } }\n"
    "#endif\n"
    "  float k = FG_SHARP * saturate(length(mv.sample(s, in.uv).xy / t) / FG_SHARP_FULL_PX);\n"
    "  if (k > 0.0) { float3 a = w.sample(s, in.uv + float2(0, -t.y)).rgb, b = w.sample(s, in.uv + float2(-t.x, 0)).rgb, d = w.sample(s, in.uv + float2(t.x, 0)).rgb, e = w.sample(s, in.uv + float2(0, t.y)).rgb;\n"
    "    float3 mn = min(c, min(a, min(b, min(d, e)))), mx = max(c, max(a, max(b, max(d, e)))); float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4))); float3 lobe = amp * -k * 0.2;\n"
    "    c = saturate(((a + b + d + e) * lobe + c) / (1.0 + 4.0 * lobe)); }\n"
    "  return float4(mix(ui.rgb, c, ui.a), 1.0); }\n";

// The present queue holds generated frames by pointer, and a queued one may outlast the frame's submission: what goes down waits until the queue has passed it (fg_reap)
static NSMutableArray *g_fg_old; static uint64_t g_fg_old_after;
static uint64_t fg_queue_in(void), fg_queue_done(void);   // nvn_mtl.m
static void fg_bury(id o) { if (!o) return; if (!g_fg_old) g_fg_old = [NSMutableArray array]; [g_fg_old addObject:o]; g_fg_old_after = fg_queue_in(); }
static void fg_reap(void) {
    if (!g_fg_old.count || fg_queue_done() < g_fg_old_after) return;
    for (id o in g_fg_old) defer_obj_release(o);
    [g_fg_old removeAllObjects];
}
static void fg_bury_all(void) {
    fg_bury(g_fg.s); fg_bury(g_fg.wout); fg_bury(g_fg.map); g_fg.map = nil; g_fg.cpso = nil; g_fg.ppso = nil; g_fg.mpso = nil; fg_bury(g_fg.pmv); g_fg.pmv = nil; fg_bury(g_fg.pdep); g_fg.pdep = nil; for (int i = 0; i < 2; i++) { fg_bury(g_fg.plate[i]); g_fg.plate[i] = nil; }
    for (int i = 0; i < FG_RING; i++) { fg_bury(g_fg.out[i]); if (g_fg.rv[i] != g_fg.out[i]) fg_bury(g_fg.rv[i]); g_fg.out[i] = nil; g_fg.rv[i] = nil; }
    g_fg.s = nil; g_fg.wout = nil;
}
// The setting no longer wants it (replay thread): the screen goes back to one present per frame and everything goes down. A build under way lands and is thrown away here
static void fg_drop(void) {
    atomic_store(&g_fg_fifo, 0); g_fg.reset = 1; g_fg.ready = 0;
    if (atomic_load(&g_fg.next_ok) == 1) { g_fg.ns = nil; g_fg.nwout = nil; g_fg.npso = nil; g_fg.ncpso = nil; g_fg.nmap = nil; g_fg.nppso = nil; g_fg.nmpso = nil; g_fg.npmv = nil; g_fg.npdep = nil; g_fg.nplate[0] = g_fg.nplate[1] = nil; for (int i = 0; i < FG_RING; i++) { g_fg.nout[i] = nil; g_fg.nrv[i] = nil; } atomic_store(&g_fg.next_ok, 0); g_fg.building = 0; }
    if (!g_fg.s) return;
    MLOG("frame generation: put down (%llu frames made)", (unsigned long long)g_fg.made);
    fg_bury_all(); g_fg.iw = g_fg.ih = g_fg.ow = g_fg.oh = 0; g_fg.scaler = NULL;
}
static void fg_build(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh, MTLPixelFormat cf, MTLPixelFormat bf, MTLPixelFormat vf) {
    g_fg.building = 1;
    MLOG("frame generation: building for %lux%lu -> %lux%lu, world format %lu, swap format %lu (view %lu)", (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf, (unsigned long)bf, (unsigned long)vf);
    id<MTL4Compiler> comp = mfx_compiler(); id<MTLFXFrameInterpolatableScaler> scaler = (id<MTLFXFrameInterpolatableScaler>)g_mt.s; void *scaler_id = (__bridge void *)g_mt.s;
    dispatch_async(mv_queue(), ^{
        uint64_t t0 = bp_now(); NSError *e = nil; const char *fe = getenv("DM_FG_FORE"); int fore = !(fe && *fe && !atoi(fe));
        MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
        d.colorTextureFormat = cf; d.outputTextureFormat = cf; d.depthTextureFormat = MTLPixelFormatDepth32Float_Stencil8; d.motionTextureFormat = MTLPixelFormatRG16Float;
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh; d.requiresPrevColorTexture = fore ? YES : NO;
        d.scaler = scaler;
        id<MTL4FXFrameInterpolator> s = (id<MTL4FXFrameInterpolator>)[d newFrameInterpolatorWithDevice:g_dev compiler:comp];
        if (!s) { MLOG("【红】frame generation: the interpolator could not be created (%lux%lu -> %lux%lu, format %lu)", (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf); g_fg.bad = 1; return; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cf width:ow height:oh mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = s.outputTextureUsage | MTLTextureUsageShaderRead; id<MTLTexture> wout = [g_dev newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bf width:ow height:oh mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        for (int i = 0; i < FG_RING; i++) { g_fg.nout[i] = [g_dev newTextureWithDescriptor:td]; g_fg.nrv[i] = vf != bf ? [g_fg.nout[i] newTextureViewWithPixelFormat:vf] : g_fg.nout[i]; }
        const char *sh = getenv("DM_FG_SHARP");
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithFormat:@"#define FG_SHARP %.3f\n#define FG_SHARP_FULL_PX 8.0\n#define FG_FORE %d\n#define FG_FORE_REACH 8.0\n%s", sh ? atof(sh) : (fore ? 0.2 : 0.5), fore, g_fg_msl] options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = vf;
        id<MTLRenderPipelineState> pso = lib ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
        id<MTLFunction> splat = [lib newFunctionWithName:@"fg_splat"]; id<MTLComputePipelineState> cpso = splat ? [g_dev newComputePipelineStateWithFunction:splat error:&e] : nil;
        id<MTLBuffer> map = [g_dev newBufferWithLength:ow * oh * 4 options:MTLResourceStorageModePrivate];
        if (!wout || !pso || !cpso || !map) { MLOG("【红】frame generation: no output target or blend pipeline (%s)", e.localizedDescription.UTF8String); g_fg.bad = 1; return; }
        // the picture and the motion the interpolator is given when the moving pixels are painted out
        MTLRenderPipelineDescriptor *ppd = [MTLRenderPipelineDescriptor new]; ppd.vertexFunction = pd.vertexFunction; ppd.fragmentFunction = [lib newFunctionWithName:@"fg_plate"]; ppd.colorAttachments[0].pixelFormat = cf;
        id<MTLRenderPipelineState> ppso = [g_dev newRenderPipelineStateWithDescriptor:ppd error:&e];
        ppd.fragmentFunction = [lib newFunctionWithName:@"fg_cam"]; ppd.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;   // a draw, not a clear: a clear loses the negative values
        id<MTLRenderPipelineState> mpso = [g_dev newRenderPipelineStateWithDescriptor:ppd error:&e];
        MTLTextureDescriptor *kd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cf width:ow height:oh mipmapped:NO]; kd.storageMode = MTLStorageModePrivate; kd.usage = s.colorTextureUsage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        g_fg.nplate[0] = [g_dev newTextureWithDescriptor:kd]; g_fg.nplate[1] = [g_dev newTextureWithDescriptor:kd];
        kd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:iw height:ih mipmapped:NO]; kd.storageMode = MTLStorageModePrivate; kd.usage = s.motionTextureUsage | MTLTextureUsageRenderTarget;
        g_fg.npmv = [g_dev newTextureWithDescriptor:kd];
        kd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:iw height:ih mipmapped:NO]; kd.storageMode = MTLStorageModePrivate; kd.usage = s.depthTextureUsage | MTLTextureUsageRenderTarget;
        g_fg.npdep = [g_dev newTextureWithDescriptor:kd];
        if (!ppso || !mpso || !g_fg.nplate[0] || !g_fg.nplate[1] || !g_fg.npmv || !g_fg.npdep) { MLOG("【红】frame generation: no room for the painted picture (%s)", e.localizedDescription.UTF8String); g_fg.bad = 1; return; }
        g_fg.fore = fore; g_fg.ncpso = cpso; g_fg.nmap = map; g_fg.nppso = ppso; g_fg.nmpso = mpso;
        s.depthReversed = YES;
        g_fg.ns = s; g_fg.nwout = wout; g_fg.npso = pso; g_fg.niw = iw; g_fg.nih = ih; g_fg.now = ow; g_fg.noh = oh; g_fg.ncf = cf; g_fg.nbf = bf; g_fg.nvf = vf; g_fg.nscaler = scaler_id;
        MLOG("frame generation: interpolator %lux%lu -> %lux%lu, world format %lu, swap format %lu (view %lu), built in %.1f ms (background)", (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf, (unsigned long)bf, (unsigned long)vf, (bp_now() - t0) / 1e6);
        atomic_store(&g_fg.next_ok, 1);
    });
}
// Replay thread, at the game's compose draw, before it is encoded. Leaves the state so that the compose draw goes on as usual
static void fg_compose(NvShader *f) {
    uint64_t frame = atomic_load(&g_cur_frame);
    fg_reap();
    if (!fg_on()) { fg_drop(); return; }
    // Screens with no world (the menus, a loading screen) stay at the real rate: the game is held to half of it only from here on, while there is a generated frame to show
    if (g_defer_replaying || g_fg.bad || !R.has_rt || R.rt.n < 1 || !R.rt.color[0] || g_fg.world_frame != frame || !g_mv.vel || !g_mt.s) { atomic_store(&g_fg_fifo, 0); g_fg.reset = 1; g_fg.skipped++; return; }   // the temporal scaler didn't draw this frame's world (a loading screen)
    // the plain blend only: colour-blind correction and HDR are the compose shader's other paths
    int cb = -1; uint64_t h10 = 0, h11 = 0;
    for (int i = 0; i < f->rf.nb; i++) { nvn_rbind *b = &f->rf.b[i];
        if (b->kind == NVN_RB_TEX && b->reg == 10) h10 = S.tex[1][10];
        if (b->kind == NVN_RB_TEX && b->reg == 11) h11 = S.tex[1][11];
        if (b->kind == NVN_RB_CBUF && b->reg == 10) cb = b->reg; }
    if (!h10 || !h11) return;
    if (cb >= 0) { float c47, c51; if (ubo_float(1, (uint16_t)(cb << 8 | (47 << 2)), &c47) && ubo_float(1, (uint16_t)(cb << 8 | (51 << 2 | 3)), &c51) && (*(int *)&c47 > 0 || *(int *)&c51 != 0)) { atomic_store(&g_fg_fifo, 0); g_fg.reset = 1; g_fg.skipped++; return; } }
    tex_t t10, t11; if (!treg_copy((int)(h10 & 0xfffff), &t10, NULL, NULL) || !treg_copy((int)(h11 & 0xfffff), &t11, NULL, NULL)) return;
    id<MTLTexture> world = tex_mtl(&t10), ui = tex_mtl(&t11), base = tex_mtl(R.rt.color[0]); if (!world || !ui || !base) return;
    RPD *probe = [RPD new]; attach(probe.colorAttachments[0], R.rt.color[0], &R.rt.cview[0], R.rt.has_cview[0], 1);
    MTLPixelFormat bf = base.pixelFormat, vf = probe.colorAttachments[0].texture ? probe.colorAttachments[0].texture.pixelFormat : bf;
    NSUInteger iw = g_mv.vel.width, ih = g_mv.vel.height, ow = world.width, oh = world.height;
    void *scaler = (__bridge void *)g_mt.s;   // the interpolator is built with the temporal scaler: a new one means a new interpolator
    #define FG_SAME(a, b, c, d, e, g, h, k) ((a) == iw && (b) == ih && (c) == ow && (d) == oh && (e) == world.pixelFormat && (g) == bf && (h) == vf && (k) == scaler)
    if (!(g_fg.s && FG_SAME(g_fg.iw, g_fg.ih, g_fg.ow, g_fg.oh, g_fg.cf, g_fg.bf, g_fg.vf, g_fg.scaler))) {
        if (atomic_load(&g_fg.next_ok) == 1 && FG_SAME(g_fg.niw, g_fg.nih, g_fg.now, g_fg.noh, g_fg.ncf, g_fg.nbf, g_fg.nvf, g_fg.nscaler)) {
            if (g_fg.s) fg_bury_all();
            g_fg.cpso = g_fg.ncpso; g_fg.map = g_fg.nmap; g_fg.ppso = g_fg.nppso; g_fg.mpso = g_fg.nmpso; g_fg.nmpso = nil; g_fg.pmv = g_fg.npmv; g_fg.pdep = g_fg.npdep; g_fg.pdep_clear = 1; g_fg.ncpso = nil; g_fg.nmap = nil; g_fg.nppso = nil; g_fg.npmv = nil; g_fg.npdep = nil; res_add(g_fg.map); res_add(g_fg.pmv); res_add(g_fg.pdep);
            for (int i = 0; i < 2; i++) { g_fg.plate[i] = g_fg.nplate[i]; g_fg.nplate[i] = nil; res_add(g_fg.plate[i]); }
            g_fg.scaler = g_fg.nscaler; g_fg.s = g_fg.ns; g_fg.wout = g_fg.nwout; g_fg.pso = g_fg.npso; g_fg.iw = iw; g_fg.ih = ih; g_fg.ow = ow; g_fg.oh = oh; g_fg.cf = g_fg.ncf; g_fg.bf = bf; g_fg.vf = vf;
            res_add(g_fg.wout); for (int i = 0; i < FG_RING; i++) { g_fg.out[i] = g_fg.nout[i]; g_fg.rv[i] = g_fg.nrv[i]; g_fg.nout[i] = nil; g_fg.nrv[i] = nil; res_add(g_fg.out[i]); } res_commit();
            if (!g_fg.fence) g_fg.fence = [g_dev newFence];
            g_fg.s.fence = g_fg.fence; g_fg.ns = nil; g_fg.nwout = nil; g_fg.npso = nil; atomic_store(&g_fg.next_ok, 0); g_fg.building = 0; g_fg.reset = 1;
        } else {
            if (atomic_load(&g_fg.next_ok) == 1) { atomic_store(&g_fg.next_ok, 0); g_fg.building = 0; }   // the size moved on while it was built
            if (!g_fg.building) fg_build(iw, ih, ow, oh, world.pixelFormat, bf, vf);
            return;
        }
    }
    #undef FG_SAME
    if (!atomic_exchange(&g_fg_fifo, 1)) fgp_reset();
    int c = g_mv.cur; double now = CACurrentMediaTime(), dt = g_fg.last_t > 0 ? now - g_fg.last_t : 1.0 / 30; g_fg.last_t = now;
    int reset = g_fg.reset || !g_mv.have_prev; g_fg.reset = 0;
    rp_end();
    if (g_fg.fore) {   // the picture it interpolates, the moving pixels painted out, and the camera's motion alone
        float *fu = (float *)((uint8_t *)g_mv.unif.contents + g_mv.cam_off); fu[2] = (float)ow; fu[3] = (float)oh; fu[4] = reset ? 0.0f : 1.0f;
        g_fg.platei ^= 1;
        if (!g_fg.pargs) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 3; ad.maxBufferBindCount = 1; ad.maxSamplerStateBindCount = 1; NSError *e = nil; g_fg.pargs = [g_dev newArgumentTableWithDescriptor:ad error:&e]; }
        MTL4RenderPassDescriptor *pp = [MTL4RenderPassDescriptor new];
        pp.colorAttachments[0].texture = g_fg.plate[g_fg.platei]; pp.colorAttachments[0].loadAction = MTLLoadActionDontCare; pp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTL4RenderCommandEncoder> pe = [g_cb renderCommandEncoderWithDescriptor:pp]; if (!pe) return;
        [pe barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
        [pe setArgumentTable:g_fg.pargs atStages:MTLRenderStageFragment];
        [g_fg.pargs setTexture:world.gpuResourceID atIndex:0]; [g_fg.pargs setTexture:g_fg.plate[g_fg.platei ^ 1].gpuResourceID atIndex:1]; [g_fg.pargs setTexture:g_mv.fgm.gpuResourceID atIndex:2]; [g_fg.pargs setAddress:g_mv.unif.gpuAddress + g_mv.cam_off atIndex:0]; [g_fg.pargs setSamplerState:mfx_samp(1).gpuResourceID atIndex:0];
        [pe setViewport:(MTLViewport){0, 0, g_fg.plate[0].width, g_fg.plate[0].height, 0, 1}]; [pe setRenderPipelineState:g_fg.ppso]; [pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_host_r_end(pe);
        pp = [MTL4RenderPassDescriptor new];
        pp.colorAttachments[0].texture = g_fg.pmv; pp.colorAttachments[0].loadAction = MTLLoadActionDontCare; pp.colorAttachments[0].storeAction = MTLStoreActionStore;
        pe = [g_cb renderCommandEncoderWithDescriptor:pp]; if (!pe) return;
        [pe setArgumentTable:g_fg.pargs atStages:MTLRenderStageFragment];
        [pe setViewport:(MTLViewport){0, 0, g_fg.pmv.width, g_fg.pmv.height, 0, 1}]; [pe setRenderPipelineState:g_fg.mpso]; [pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_host_r_end(pe);
        if (g_fg.pdep_clear) {   // a flat depth: the real one would still show it the shape of what was painted out
            g_fg.pdep_clear = 0; pp = [MTL4RenderPassDescriptor new]; pp.depthAttachment.texture = g_fg.pdep; pp.depthAttachment.loadAction = MTLLoadActionClear; pp.depthAttachment.storeAction = MTLStoreActionStore; pp.depthAttachment.clearDepth = 0.5;
            pp.stencilAttachment.texture = g_fg.pdep; pp.stencilAttachment.loadAction = MTLLoadActionClear; pp.stencilAttachment.storeAction = MTLStoreActionStore;
            pe = [g_cb renderCommandEncoderWithDescriptor:pp]; if (!pe) return; enc_host_r_end(pe);
        }
    }
    // the interpolator can wait on a fence only: this empty pass stands after everything the frame has drawn, and it updates that fence
    CENC ce = enc_c_begin(NULL);
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    if (g_fg.fore) {   // this frame's moving pixels, half way back
        if (!g_fg.cargs) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 3; ad.maxBufferBindCount = 2; NSError *e = nil; g_fg.cargs = [g_dev newArgumentTableWithDescriptor:ad error:&e]; }
        [ce fillBuffer:g_fg.map range:NSMakeRange(0, g_fg.map.length) value:0xff];
        [ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        [g_fg.cargs setTexture:g_mv.vel.gpuResourceID atIndex:0]; [g_fg.cargs setTexture:g_mv.depth.gpuResourceID atIndex:1]; [g_fg.cargs setTexture:g_mv.fgm.gpuResourceID atIndex:2]; [g_fg.cargs setAddress:g_mv.unif.gpuAddress + g_mv.cam_off atIndex:0]; [g_fg.cargs setAddress:g_fg.map.gpuAddress atIndex:1];
        [ce setArgumentTable:g_fg.cargs]; [ce setComputePipelineState:g_fg.cpso]; [ce dispatchThreads:MTLSizeMake(ow, oh, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
    }
    [ce updateFence:g_fg.fence afterEncoderStages:MTLStageDispatch]; m4_end(ce);
    id<MTL4FXFrameInterpolator> s = g_fg.s;
    s.colorTexture = g_fg.fore ? g_fg.plate[g_fg.platei] : world; s.depthTexture = g_fg.fore ? g_fg.pdep : g_mv.depth; s.motionTexture = g_fg.fore ? g_fg.pmv : g_mv.vel; s.outputTexture = g_fg.wout;
    if (g_fg.fore) s.prevColorTexture = g_fg.plate[g_fg.platei ^ 1];
    s.motionVectorScaleX = ow; s.motionVectorScaleY = oh;
    // the camera goes through the matrices (the world's is orthographic); field of view is only for a perspective matrix the interpolator isn't given
    s.deltaTime = (float)dt; s.nearPlane = g_mv.nearf[c]; s.farPlane = g_mv.farf[c]; s.fieldOfView = 60.0f; s.aspectRatio = (float)ow / oh;
    simd_float4x4 v, p; memcpy(&v, g_mv.view_m[c], 64); memcpy(&p, g_mv.proj_m[c], 64); s.worldToViewMatrix = v; s.viewToClipMatrix = p;
    s.jitterOffsetX = g_mv.jit[c][0] * iw * 0.5f; s.jitterOffsetY = -g_mv.jit[c][1] * ih * 0.5f; s.shouldResetHistory = reset;
    [s encodeToCommandBuffer:g_cb];
    // this frame's interface over the generated world
    int k = g_fg.idx = (g_fg.idx + 1) % FG_RING;
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = g_fg.rv[k]; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) return;
    if (!g_fg.args) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 4; ad.maxBufferBindCount = 1; ad.maxSamplerStateBindCount = 1; NSError *e = nil; g_fg.args = [g_dev newArgumentTableWithDescriptor:ad error:&e]; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en waitForFence:g_fg.fence beforeEncoderStages:MTLStageFragment];
    [en setArgumentTable:g_fg.args atStages:MTLRenderStageFragment];
    [g_fg.args setTexture:g_fg.wout.gpuResourceID atIndex:0]; [g_fg.args setTexture:ui.gpuResourceID atIndex:1]; [g_fg.args setTexture:g_mv.vel.gpuResourceID atIndex:2]; [g_fg.args setTexture:world.gpuResourceID atIndex:3]; [g_fg.args setAddress:g_fg.map.gpuAddress atIndex:0]; [g_fg.args setSamplerState:mfx_samp(1).gpuResourceID atIndex:0];
    [en setViewport:(MTLViewport){0, 0, g_fg.out[k].width, g_fg.out[k].height, 0, 1}]; [en setRenderPipelineState:g_fg.pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_host_r_end(en);
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    g_fg.ready = !reset; g_fg.ready_frame = frame; if (!reset) g_fg.made++;
    if (g_mv.dump_vel) { g_mv.dump_gen = g_fg.wout; g_mv.dump_real = world; g_mv.dump_comp = g_fg.out[k]; g_mv.dump_swap = base; }
    { static int once; if (!once++) MLOG("frame generation: world %lux%lu format %lu, interface %lux%lu format %lu, swap %lux%lu; near %.2f far %.2f", (unsigned long)world.width, (unsigned long)world.height, (unsigned long)world.pixelFormat, (unsigned long)ui.width, (unsigned long)ui.height, (unsigned long)ui.pixelFormat, (unsigned long)base.width, (unsigned long)base.height, g_mv.nearf[c], g_mv.farf[c]); }
    if (g_fg.made && g_fg.made % 600 == 0) MLOG("frame generation: %llu frames made, %llu without (no world or not the plain blend)", (unsigned long long)g_fg.made, (unsigned long long)g_fg.skipped);
}
