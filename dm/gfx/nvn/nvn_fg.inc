// Frame generation: the screen shows twice as many frames as the game draws, the extra ones made by MetalFX's frame interpolator. Included by nvn_mtl.m; replay thread
// unless a comment says otherwise. The Video tab's Frame Generation row (dm_video.c dm_video_fg_active; DM_FG=0/1 forces it) turns it on, live, and it needs the temporal scaler
// (nvn_mv.inc): its motion vectors, depth and camera offset are the interpolator's inputs. The power saver's rate has no room for it.
//
// The game keeps the world and the interface apart until the last moment: the world's Final Result and the Offscreen UI target are blended by its compose pass (ps_compose_p,
// key 9a650561e5c40791, main+0x16831a4) into the swap texture. So at that draw fg_compose interpolates the world alone, between last frame's Final Result and this one, and
// blends this frame's interface over the result the way the compose shader does. The interface is never warped. Screens where the compose shader does more than that
// (colour-blind correction, HDR) get no generated frames.
//
// Each frame then queues two presents (mtl_present_fg): the generated one, then the real one, one display-link tick apart. Since a real frame takes two ticks of the screen's
// rate, the game's own back-pressure (the swap texture waits for its last present) halves its rate without a limiter: 60 Hz shows 60, the game draws 30.
//
// DM_MV_DUMP also writes the frame's generated world (_gen), real world (_real), generated frame with the interface (_comp) and real frame (_swap) as raw files.
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
    NSUInteger iw, ih, ow, oh; MTLPixelFormat cf, bf, vf; void *scaler; int bad, building, reset, idx, ready; uint64_t world_frame, ready_frame; double last_t;   // scaler: the temporal scaler it was built with, never followed
    __strong id<MTL4FXFrameInterpolator> ns; __strong id<MTLTexture> nwout; __strong id<MTLTexture> nout[FG_RING]; __strong id<MTLTexture> nrv[FG_RING]; __strong id<MTLRenderPipelineState> npso; NSUInteger niw, nih, now, noh; MTLPixelFormat ncf, nbf, nvf; void *nscaler; _Atomic int next_ok;
    uint64_t made, skipped;
} g_fg;
static _Atomic int g_fg_fifo;   // 1 = the display link takes one queued present per tick instead of the newest

static const char *g_fg_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
    // as the game's compose: the interface's alpha is how much of the world shows through
    // Generated frames come out softer than real ones where things move (scenery at a run keeps about 76% of a real frame's detail, so the picture would pulse at half the shown rate) and match them
    // where nothing moves (98%), so the world gets a contrast-adaptive sharpen that follows each pixel's motion, here, under the interface: the less room a pixel has before clipping, the less it moves
    "fragment float4 fs(V in [[stage_in]], texture2d<float> w [[texture(0)]], texture2d<float> u [[texture(1)]], texture2d<float> mv [[texture(2)]], sampler s [[sampler(0)]]) {\n"
    "  float4 ui = u.sample(s, in.uv); float2 t = 1.0 / float2(w.get_width(), w.get_height()); float3 c = w.sample(s, in.uv).rgb;\n"
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
    fg_bury(g_fg.s); fg_bury(g_fg.wout);
    for (int i = 0; i < FG_RING; i++) { fg_bury(g_fg.out[i]); if (g_fg.rv[i] != g_fg.out[i]) fg_bury(g_fg.rv[i]); g_fg.out[i] = nil; g_fg.rv[i] = nil; }
    g_fg.s = nil; g_fg.wout = nil;
}
// The setting no longer wants it (replay thread): the screen goes back to one present per frame and everything goes down. A build under way lands and is thrown away here
static void fg_drop(void) {
    atomic_store(&g_fg_fifo, 0); g_fg.reset = 1; g_fg.ready = 0;
    if (atomic_load(&g_fg.next_ok) == 1) { g_fg.ns = nil; g_fg.nwout = nil; g_fg.npso = nil; for (int i = 0; i < FG_RING; i++) { g_fg.nout[i] = nil; g_fg.nrv[i] = nil; } atomic_store(&g_fg.next_ok, 0); g_fg.building = 0; }
    if (!g_fg.s) return;
    MLOG("frame generation: put down (%llu frames made)", (unsigned long long)g_fg.made);
    fg_bury_all(); g_fg.iw = g_fg.ih = g_fg.ow = g_fg.oh = 0; g_fg.scaler = NULL;
}
static void fg_build(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh, MTLPixelFormat cf, MTLPixelFormat bf, MTLPixelFormat vf) {
    g_fg.building = 1;
    MLOG("frame generation: building for %lux%lu -> %lux%lu, world format %lu, swap format %lu (view %lu)", (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf, (unsigned long)bf, (unsigned long)vf);
    id<MTL4Compiler> comp = mfx_compiler(); id<MTLFXFrameInterpolatableScaler> scaler = (id<MTLFXFrameInterpolatableScaler>)g_mt.s; void *scaler_id = (__bridge void *)g_mt.s;
    dispatch_async(mv_queue(), ^{
        uint64_t t0 = bp_now(); NSError *e = nil;
        MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
        d.colorTextureFormat = cf; d.outputTextureFormat = cf; d.depthTextureFormat = MTLPixelFormatDepth32Float_Stencil8; d.motionTextureFormat = MTLPixelFormatRG16Float;
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh; d.requiresPrevColorTexture = NO;
        d.scaler = scaler;
        id<MTL4FXFrameInterpolator> s = (id<MTL4FXFrameInterpolator>)[d newFrameInterpolatorWithDevice:g_dev compiler:comp];
        if (!s) { MLOG("【红】frame generation: the interpolator could not be created (%lux%lu -> %lux%lu, format %lu)", (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cf); g_fg.bad = 1; return; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cf width:ow height:oh mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = s.outputTextureUsage | MTLTextureUsageShaderRead; id<MTLTexture> wout = [g_dev newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bf width:ow height:oh mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        for (int i = 0; i < FG_RING; i++) { g_fg.nout[i] = [g_dev newTextureWithDescriptor:td]; g_fg.nrv[i] = vf != bf ? [g_fg.nout[i] newTextureViewWithPixelFormat:vf] : g_fg.nout[i]; }
        const char *sh = getenv("DM_FG_SHARP");
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithFormat:@"#define FG_SHARP %.3f\n#define FG_SHARP_FULL_PX 8.0\n%s", sh ? atof(sh) : 0.5, g_fg_msl] options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = vf;
        id<MTLRenderPipelineState> pso = lib ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
        if (!wout || !pso) { MLOG("【红】frame generation: no output target or blend pipeline (%s)", e.localizedDescription.UTF8String); g_fg.bad = 1; return; }
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
    atomic_store(&g_fg_fifo, 1);
    int c = g_mv.cur; double now = CACurrentMediaTime(), dt = g_fg.last_t > 0 ? now - g_fg.last_t : 1.0 / 30; g_fg.last_t = now;
    int reset = g_fg.reset || !g_mv.have_prev; g_fg.reset = 0;
    rp_end();
    // the interpolator can wait on a fence only: this empty pass stands after everything the frame has drawn, and it updates that fence
    CENC ce = enc_c_begin(NULL);
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [ce updateFence:g_fg.fence afterEncoderStages:MTLStageDispatch]; m4_end(ce);
    id<MTL4FXFrameInterpolator> s = g_fg.s;
    s.colorTexture = world; s.depthTexture = g_mv.depth; s.motionTexture = g_mv.vel; s.outputTexture = g_fg.wout;
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
    if (!g_fg.args) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 3; ad.maxSamplerStateBindCount = 1; NSError *e = nil; g_fg.args = [g_dev newArgumentTableWithDescriptor:ad error:&e]; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en waitForFence:g_fg.fence beforeEncoderStages:MTLStageFragment];
    [en setArgumentTable:g_fg.args atStages:MTLRenderStageFragment];
    [g_fg.args setTexture:g_fg.wout.gpuResourceID atIndex:0]; [g_fg.args setTexture:ui.gpuResourceID atIndex:1]; [g_fg.args setTexture:g_mv.vel.gpuResourceID atIndex:2]; [g_fg.args setSamplerState:mfx_samp(1).gpuResourceID atIndex:0];
    [en setViewport:(MTLViewport){0, 0, g_fg.out[k].width, g_fg.out[k].height, 0, 1}]; [en setRenderPipelineState:g_fg.pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_host_r_end(en);
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    g_fg.ready = !reset; g_fg.ready_frame = frame; if (!reset) g_fg.made++;
    if (g_mv.dump_vel) { g_mv.dump_gen = g_fg.wout; g_mv.dump_real = world; g_mv.dump_comp = g_fg.out[k]; g_mv.dump_swap = base; }
    { static int once; if (!once++) MLOG("frame generation: world %lux%lu format %lu, interface %lux%lu format %lu, swap %lux%lu; near %.2f far %.2f", (unsigned long)world.width, (unsigned long)world.height, (unsigned long)world.pixelFormat, (unsigned long)ui.width, (unsigned long)ui.height, (unsigned long)ui.pixelFormat, (unsigned long)base.width, (unsigned long)base.height, g_mv.nearf[c], g_mv.farf[c]); }
    if (g_fg.made && g_fg.made % 600 == 0) MLOG("frame generation: %llu frames made, %llu without (no world or not the plain blend)", (unsigned long long)g_fg.made, (unsigned long long)g_fg.skipped);
}
