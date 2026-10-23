// 回放层几处修正的回归（真实 Metal 回放，闭式期望，不依赖游戏）：预编扫描不改剪裁、换格式清除的视图留到提交跑完、顶点流 14 不被零属性槽盖掉、出图槽等本槽上次的命令缓冲真正跑完才复用。
// Regression tests for several replay fixes (real Metal replay, closed-form expectations, no dependence on the game): the prediction scan keeps the scissor, a format-changing clear view is kept until the submit has run, vertex stream 14 is not overwritten by the zero-attribute slot, a present slot is reused only after its previous command buffer has really finished.
#include "../nvn/nvn_mtl.m"
#include <pthread.h>
static CAMetalLayer *t_layer;   // 出图那一例才建（其余各例与别的单测一样没有图层） | Created only for the present case (the other cases have no layer, like the other unit tests)
void *front_metal_layer(void) { return (__bridge void *)t_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); count++; printf("%s 期望 %llu → 真结果 %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w == g ? "✓" : "✗"); fail += w != g; } while (0)
typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(void) { chain c = {0}; if (posix_memalign((void **)&c.p, getpagesize(), 65536)) abort(); memset(c.p, 0, 65536); mtl_pool_add(c.p, 65536); return c; }
static void emit(chain *c, unsigned op, const void *a, size_t n) { size_t z = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)z}; memcpy(c->p + c->n, &h, 8); if (n) memcpy(c->p + c->n + 8, a, n); c->n += z; }
static uint8_t *pool_mem(size_t n) { uint8_t *m; if (posix_memalign((void **)&m, getpagesize(), n)) abort(); memset(m, 0, n); mtl_pool_add(m, n); return m; }
static tex_t tex(unsigned w, int fmt) { static uint64_t s = 9000; tex_t t = {.serial = s++}; t.d.width = t.d.height = w; t.d.depth = t.d.samples = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = fmt; return t; }
static void scissor(chain *c, int x, int y, int w, int h) { p_scissor sc = {x, y, w, h}; emit(c, OP_SCISSOR, &sc, sizeof sc); }
static void set_rt(chain *c, const tex_t *a, const tex_t *ds) { p_set_rt r = {.n = a ? 1 : 0}; r.color[0] = a; r.depth = ds; emit(c, OP_SET_RT, &r, sizeof r); p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = 8; emit(c, OP_VIEWPORTS, &v, sizeof v); scissor(c, 0, 0, 8, 8); }
static void clear_color(chain *c, float r, float g, float b, float a) { p_clear_color x = {.index = 0, .mask = 15, .rgba = {r, g, b, a}}; emit(c, OP_CLEAR_COLOR, &x, sizeof x); }
static void bind_prog(chain *c, prog_t *p) { p_bind_program b = {p, 3}; emit(c, OP_BIND_PROGRAM, &b, sizeof b); }
static void bind_tex(chain *c, int id) { p_bind_handles b = {.stage = 1, .first = 0, .n = 1}; b.h[0] = id; emit(c, OP_BIND_TEX, &b, sizeof b); }
static void draw_cmd(chain *c) { p_draw d = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3}; emit(c, OP_DRAW, &d, sizeof d); }
static void ds_off(chain *c) { ds_st d = {0, 0, NVN_DEPTH_FUNC_ALWAYS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP}; p_bind_state x = {.kind = 5}; memcpy(x.bytes, &d, sizeof d); emit(c, OP_BIND_STATE, &x, sizeof x); }
// 顶点属性 0 取自流 st（float4，步长 16），流缓冲在 vb | Vertex attribute 0 comes from stream st (float4, stride 16), the stream buffer is vb
static void vertex_stream(chain *c, int st, const void *vb, uint64_t len) {
    va_st at = {0x2E, 0, (uint32_t)st}; p_bind_vattr a = {.first = 0, .n = 1}; memcpy(&a.st[0], &at, 4); emit(c, OP_BIND_VATTR, &a, sizeof a);
    p_bind_vstream s = {.first = 0, .n = st + 1}; s.st[st] = (vs_st){16, 0}; emit(c, OP_BIND_VSTREAM, &s, sizeof s);
    p_bind_vbuf b = {.first = st, .n = 1}; b.r[0].address = (uint64_t)vb; b.r[0].size = len; emit(c, OP_BIND_VBUF, &b, sizeof b);
}
static void exec(chain *c) { emit(c, OP_END, NULL, 0); state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t hh = (uint64_t)c->p; mtl_sub_index(&hh, 1); mtl_exec(c->p); }
static void commit(void) { uint64_t f = mtl_sub_commit(); EXPECT("提交完成 | submit done", 1, mtl_wait(f, 5000000000ull)); }
static void readback(const tex_t *t, uint8_t out[256]) { id<MTLBuffer> b = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; [blit() copyFromTexture:tex_mtl(t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256]; commit(); memcpy(out, b.contents, 256); }
static int count_px(const uint8_t px[256], uint8_t r, uint8_t g, uint8_t b) { int n = 0; for (int i = 0; i < 64; i++) n += abs(px[4 * i] - r) <= 2 && abs(px[4 * i + 1] - g) <= 2 && abs(px[4 * i + 2] - b) <= 2; return n; }
// 测试里的程序对象常驻（同 t_xv：释放后新对象可能落在同一地址，管线缓存按对象地址认） | Program objects in the tests are permanent (as in t_xv: after a free a new object may land at the same address, and the pipeline cache is keyed by object address)
static NvProg *t_keep[64]; static int t_nkeep; static id<MTLLibrary> g_lib; static uint64_t g_pser = 8300;
static NvShader *shader(NSString *fn, int stage) { NvShader *s = [NvShader new]; s->fn = [g_lib newFunctionWithName:fn]; s->stage = stage; s->rf.pos_attr = -1; if (!s->fn) abort(); return s; }
static void prog(prog_t *pg, NvShader *vs, NvShader *fs) { memset(pg, 0, sizeof *pg); pg->serial = g_pser++; NvProg *p = [NvProg new]; p->sh[0] = vs; p->sh[1] = fs; if (t_nkeep < 64) t_keep[t_nkeep++] = p; serial_put(pg->serial, p); }
static void lib_init(void) { NSError *e = nil; g_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
    "vertex float4 v(uint n [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[n], 0, 1); }\n"
    "struct VI { float4 a [[attribute(0)]]; }; vertex float4 va(VI i [[stage_in]]) { return i.a; }\n"
    "fragment float4 red() { return float4(1, 0, 0, 1); }\n"
    "struct A { texture2d<float> t [[id(0)]]; }; fragment float4 sample(constant A &a [[buffer(0)]]) { return a.t.read(uint2(0)); }\n" options:nil error:&e];
    if (!g_lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); abort(); } }
static NvShader *vs_attr(void) { NvShader *s = shader(@"va", 0); s->rf.nv = 1; s->rf.v[0].loc = 0; s->rf.v[0].nvn = 0; s->rf.v[0].ct = NVN_CT_FLOAT; s->rf.pos_attr = 0; return s; }
static NvShader *fs_red(void) { NvShader *s = shader(@"red", 1); s->rf.color_mask = 1; return s; }
static NvShader *fs_sample(void) { NvShader *s = shader(@"sample", 1); s->rf.nb = 1; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = 2}; s->ae = [s->fn newArgumentEncoderWithBufferIndex:0]; return s; }

// F-GFX-3：本提交第一次缺管线时预编扫描把余下的状态过一遍；扫描若改了当前剪裁，这次绘制就用上后面那条 1×1 剪裁
// F-GFX-3: at the first pipeline miss of a submit the prediction scan runs over the remaining state; if the scan changes the current scissor, this draw uses the later 1×1 scissor
static void scissor_case(void) {
    prog_t pg; prog(&pg, shader(@"v", 0), fs_red()); tex_t a = tex(8, NVN_FORMAT_RGBA8); uint8_t px[256];
    chain c = chain_new(); set_rt(&c, &a, NULL); clear_color(&c, 0, 0, 0, 1); ds_off(&c); bind_prog(&c, &pg); draw_cmd(&c); scissor(&c, 0, 0, 1, 1); exec(&c);
    readback(&a, px); EXPECT("剪裁：缺管线的那次绘制按当时的 8×8 剪裁画满 | scissor: the draw at the miss covers its 8×8 scissor", 64, count_px(px, 255, 0, 0));
}
// F-GFX-6：换格式视图上的清除在 Metal 4 下命令缓冲不持有视图，要交延迟释放留到提交跑完
// F-GFX-6: under Metal 4 the command buffer does not retain the view a format-changing clear uses, so it must go to deferred release until the submit has run
static void clear_view_case(void) {
    tex_t t = tex(8, NVN_FORMAT_RGBA8); state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); id<MTLTexture> base0 = tex_mtl(&t); (void)base0;
    p_clear_tex c = {.tex = &t, .has_view = 1, .is_uint = 1, .r = {0, 0, 0, 8, 8, 1}, .mask = 0xf}; c.view.set_format = 1; c.view.format = NVN_FORMAT_R32UI; for (int i = 0; i < 4; i++) c.c.u[i] = 0x11223344u;
    clear_tex(&c);
    int held = 0; pthread_mutex_lock(&g_deferred_mu);
    for (NvDeferredRelease *d in g_deferred) if ([d->object conformsToProtocol:@protocol(MTLTexture)]) { id<MTLTexture> v = d->object; held += v.parentTexture && v.pixelFormat == MTLPixelFormatR32Uint && d->keep_resident; }
    pthread_mutex_unlock(&g_deferred_mu);
    EXPECT("换格式清除：视图交给延迟释放（只放引用）| format-changing clear: the view is handed to deferred release (reference only)", 1, held);
    uint8_t px[256]; readback(&t, px); int ok = 0; for (int i = 0; i < 64; i++) { uint32_t v; memcpy(&v, px + 4 * i, 4); ok += v == 0x11223344u; }
    EXPECT("换格式清除：64 个纹素都是清除值 | format-changing clear: all 64 texels hold the clear value", 64, ok);
}
// F-GFX-11：NVN 顶点流 s 绑在 Metal 顶点缓冲 16 + s；零属性缓冲原先固定在 30，盖掉流 14
// F-GFX-11: NVN vertex stream s is bound at Metal vertex buffer 16 + s; the zero-attribute buffer used to sit at 30 and overwrote stream 14
static void stream14_case(void) {
    prog_t pg; prog(&pg, vs_attr(), fs_red()); tex_t a = tex(8, NVN_FORMAT_RGBA8); uint8_t px[256];
    float *vb = (float *)pool_mem(4096); const float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1}; memcpy(vb, tri, sizeof tri);
    chain c = chain_new(); set_rt(&c, &a, NULL); clear_color(&c, 0, 0, 0, 1); ds_off(&c); vertex_stream(&c, 14, vb, 48); bind_prog(&c, &pg); draw_cmd(&c); exec(&c);
    readback(&a, px); EXPECT("顶点流 14：位置取自流 14，全屏三角画满 | stream 14: positions come from stream 14, the fullscreen triangle covers all", 64, count_px(px, 255, 0, 0));
}
// F-GFX-2：显示节拍合并旧帧时把票号直接推到「本帧 − 1」，在途的上一帧也被算成做完；出图槽（Metal 4 的命令分配器）复用前要等本槽上次的命令缓冲真正完成。
// 这里让 GPU 队列先等一个事件，票 1 的命令缓冲就一直在途；照合并的写法把 g_pq_done 推到 3，再出票 4（与票 1 同槽）：事件发出之前它不能返回
// F-GFX-2: when the display link merges old frames it bumps the ticket straight to "this frame - 1", so an in-flight earlier frame is counted as done too; a present slot (a Metal 4 command allocator) may be reused only after that slot's previous command buffer has really completed.
// Here the GPU queue first waits for an event, so ticket 1's command buffer stays in flight; g_pq_done is bumped to 3 as the merge does, then ticket 4 (same slot as ticket 1) is presented: it must not return before the event is signaled
static tex_t t_psrc; static id<CAMetalDrawable> t_d4; static _Atomic int t_p4_done;
static void *present4(void *arg) { (void)arg; @autoreleasepool { present_now(&t_psrc, 0, 4, 4, NULL, t_d4, 0); } atomic_store(&t_p4_done, 1); return NULL; }
static void present_slot_case(void) {
    t_layer = [CAMetalLayer layer]; t_layer.device = g_dev; t_layer.pixelFormat = MTLPixelFormatBGRA8Unorm; t_layer.drawableSize = CGSizeMake(16, 16);
    t_psrc = tex(8, NVN_FORMAT_RGBA8); state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); tex_mtl(&t_psrc); commit();
    id<MTLSharedEvent> ev = [g_dev newSharedEvent];
#if DM_NVN_M3
    id<MTLCommandBuffer> gate = [g_q commandBuffer]; [gate encodeWaitForEvent:ev value:1]; [gate commit];
#else
    [g_q4 waitForEvent:ev value:1];
#endif
    int ok1; @autoreleasepool { ok1 = present_now(&t_psrc, 0, 1, 1, NULL, [t_layer nextDrawable], 0); }
    EXPECT("出图：票 1 已提交（GPU 上被事件挡着）| present: ticket 1 committed (held by the event on the GPU)", 1, ok1);
    pthread_mutex_lock(&g_pq_mu); if (g_pq_done < 3) g_pq_done = 3; pthread_mutex_unlock(&g_pq_mu);   // 同显示节拍合并票 2、3 时的 g_pq_done = ticket - 1 | Same as g_pq_done = ticket - 1 when the display link merges tickets 2 and 3
    t_d4 = [t_layer nextDrawable]; pthread_t th; pthread_create(&th, NULL, present4, NULL);
    usleep(300000); int early = atomic_load(&t_p4_done);
    ev.signaledValue = 1;
    for (int i = 0; i < 500 && !atomic_load(&t_p4_done); i++) usleep(10000);
    EXPECT("出图：同槽的票 4 等票 1 真正跑完才复用 | present: ticket 4 reuses the slot only after ticket 1 has really finished", 0, early);
    EXPECT("出图：事件发出后票 4 照常出图 | present: ticket 4 goes out once the event is signaled", 1, atomic_load(&t_p4_done));
    if (!atomic_load(&t_p4_done)) { printf("== 票 4 卡住 | ticket 4 is stuck ==\n"); _exit(1); }
    pthread_join(th, NULL);
}
int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init(); lib_init();
    scissor_case(); clear_view_case(); stream14_case(); present_slot_case();
    printf("== %d 项，失败 %d ==\n", count, fail); return fail != 0; } }
