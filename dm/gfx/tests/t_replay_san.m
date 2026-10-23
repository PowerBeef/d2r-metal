// 回放层要靠消毒器才看得见的几处缺陷的回归（编两份：线程消毒版 t_replay_tsan、地址 + 未定义行为消毒版 t_replay_asan；有报告即非 0 退出）：
//   treg   （线程）贴图登记表被游戏线程扩容 / 改写时，翻译线程上的读者（写回前瞻的子资源判定、零源跳过的视图判定）不加锁读它
//   gfxuse （线程）DM_GFXUSE 开关第一次出图时才在出图线程上写，提交线程同时在读
//   rtrange（线程）DM_NVN_RT_RANGE 开关懒读：预热线程与翻译线程都在 shader_load 里第一次读写它
//   bloom  （地址）泛光 1/4 起点记下的是登记表里那一项的地址，登记表扩容搬家后截取时读到已释放的内存
//   fx9    （未定义）写回前瞻表 9 项，8 颜色 + 深度 + 模板 = 10 项
//   fx19   （未定义）游戏给的负下标（视口 / 顶点缓冲的起始号、阶段号、槽号）在录制与回放两侧都要挡住
//   fx11   顶点流 15 在 Metal 自己的描述符设置里中止进程（缓冲号 31 越界）
// Regression tests for replay defects that only a sanitizer makes visible (built twice: the ThreadSanitizer t_replay_tsan and the AddressSanitizer + UndefinedBehaviorSanitizer t_replay_asan; any report exits non-0):
//   treg   (thread) while the game thread grows / rewrites the texture registration table, the readers on the translation thread (the store lookahead's subresource test, the zero-source skip's view test) read it without the lock
//   gfxuse (thread) the DM_GFXUSE switch is written on the present thread at the first present, while the submit thread reads it
//   rtrange (thread) the DM_NVN_RT_RANGE switch is read lazily: the warm-up thread and the translation thread both read and write it for the first time in shader_load
//   bloom  (address) the bloom 1/4 start remembered the address of the table entry; after the table grows and moves, the capture reads freed memory
//   fx9    (undefined) the store lookahead table has 9 entries, 8 colors + depth + stencil = 10
//   fx19   (undefined) negative indices from the game (first viewport / vertex buffer, stage, slot) must be stopped on both the record and the replay side
//   fx11   vertex stream 15 aborts the process inside Metal's own descriptor setter (buffer index 31 out of range)
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"
#include <pthread.h>
uint64_t g_t0_ns;   // 宿主 main.c 的进程起点（hle_nvn 逐帧 @t_s 用），单测替身 | process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); count++; printf("%s 期望 %llu → 真结果 %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w == g ? "✓" : "✗"); fail += w != g; } while (0)
typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(void) { chain c = {0}; if (posix_memalign((void **)&c.p, getpagesize(), 65536)) abort(); memset(c.p, 0, 65536); mtl_pool_add(c.p, 65536); return c; }
static void emit(chain *c, unsigned op, const void *a, size_t n) { size_t z = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)z}; memcpy(c->p + c->n, &h, 8); if (n) memcpy(c->p + c->n + 8, a, n); c->n += z; }
static tex_t tex(unsigned w, int fmt) { static uint64_t s = 9000; tex_t t = {.serial = s++}; t.d.width = t.d.height = w; t.d.depth = t.d.samples = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = fmt; return t; }
static void set_rt(chain *c, const tex_t *a) { p_set_rt r = {.n = 1}; r.color[0] = a; emit(c, OP_SET_RT, &r, sizeof r); p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = 8; emit(c, OP_VIEWPORTS, &v, sizeof v); p_scissor sc = {0, 0, 8, 8}; emit(c, OP_SCISSOR, &sc, sizeof sc); }
static void exec(chain *c) { emit(c, OP_END, NULL, 0); state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t hh = (uint64_t)c->p; mtl_sub_index(&hh, 1); mtl_exec(c->p); }
static id<MTLLibrary> g_lib; static NvProg *t_keep[16]; static int t_nkeep;
static NvShader *shader(NSString *fn, int stage) { NvShader *s = [NvShader new]; s->fn = [g_lib newFunctionWithName:fn]; s->stage = stage; s->rf.pos_attr = -1; if (!s->fn) abort(); return s; }
static void prog(prog_t *pg, uint64_t ser, NvShader *vs, NvShader *fs) { memset(pg, 0, sizeof *pg); pg->serial = ser; NvProg *p = [NvProg new]; p->sh[0] = vs; p->sh[1] = fs; if (t_nkeep < 16) t_keep[t_nkeep++] = p; serial_put(ser, p); }
static void lib_init(void) { NSError *e = nil; g_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
    "vertex float4 v(uint n [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[n], 0, 1); }\n"
    "struct VI { float4 a [[attribute(0)]]; }; vertex float4 va(VI i [[stage_in]]) { return i.a; }\n"
    "fragment float4 red() { return float4(1, 0, 0, 1); }\n" options:nil error:&e];
    if (!g_lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); abort(); } }

static _Atomic int g_stop; static tex_t g_rt_tex;
static void *treg_reader(void *a) {   // 翻译线程替身：调真实的两个读者 | Translation-thread stand-in: calls the two real readers
    (void)a; p_set_rt rt = {.n = 1}; rt.color[0] = &g_rt_tex; unsigned long hits = 0;
    while (!atomic_load(&g_stop)) for (int tid = 1; tid < 40000; tid += 97) { hits += defer_handle_sub((uint64_t)tid, &rt, 0, 1); hits += zskip_view_zero(tid); }
    return (void *)hits;
}
static void *use_reader(void *a) { (void)a; for (int i = 0; i < 200000; i++) use_rec("buf", 0x1000, 16, 0, NULL); return NULL; }
static void *rt_reader(void *a) { (void)a; int n = 0; for (int i = 0; i < 1000; i++) n += rt_range_on(); return (void *)(intptr_t)n; }

int main(int argc, char **argv) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "";
    mtl_init(); lib_init();
    if (!strcmp(mode, "treg")) {
        g_rt_tex = tex(8, NVN_FORMAT_RGBA8); tex_t t0 = tex(8, NVN_FORMAT_RGBA8); mtl_reg_tex(1, &t0, NULL);
        pthread_t th; pthread_create(&th, NULL, treg_reader, NULL);
        for (int id = 2; id < 40000; id++) { tex_t t = tex(8, NVN_FORMAT_RGBA8); mtl_reg_tex(id, &t, NULL); }   // 游戏线程：号一路往上登记，登记表扩容约 39 次 | Game thread: registers ever higher IDs, the table grows about 39 times
        atomic_store(&g_stop, 1); pthread_join(th, NULL);
        EXPECT("登记表：并发登记与读取跑完 | registration table: concurrent registration and reads finished", 1, 1);
    } else if (!strcmp(mode, "gfxuse")) {
        pthread_t th; pthread_create(&th, NULL, use_reader, NULL);
        use_tick();   // 出图线程上的第一次出图 | The first present, on the present thread
        pthread_join(th, NULL);
        EXPECT("DM_GFXUSE：未设置时保持关 | DM_GFXUSE: stays off when unset", USE_OFF, (uint64_t)g_use);
    } else if (!strcmp(mode, "rtrange")) {
        pthread_t th; pthread_create(&th, NULL, rt_reader, NULL);   // 预热线程替身 | Warm-up thread stand-in
        int on = rt_range_on();   // 翻译线程第一次读 | The translation thread's first read
        pthread_join(th, NULL);
        EXPECT("DM_NVN_RT_RANGE：未设置时关 | DM_NVN_RT_RANGE: off when unset", 0, (uint64_t)on);
    } else if (!strcmp(mode, "bloom")) {
        // 泛光 1/4：起点（片元 2938a5b71b8e21b2，采样 S 画进 B0）只记下状态、不画；原生截取（019ce2f82edbda84，采样 B0）时从记下的 S 画。两次之间游戏登记了更高的贴图号（登记表搬家）
        // Bloom 1/4: the start (fragment 2938a5b71b8e21b2, samples S into B0) only records state and does not draw; the native capture (019ce2f82edbda84, samples B0) draws from the recorded S. Between the two the game registers a higher texture ID (the table moves)
        NvShader *f0 = shader(@"red", 1), *f1 = shader(@"red", 1); snprintf(f0->key, sizeof f0->key, "2938a5b71b8e21b2_b"); snprintf(f1->key, sizeof f1->key, "019ce2f82edbda84_b");
        f0->rf.nb = 2; f0->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = 2}; f0->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 7}; f0->rf.c1_len = 256; float c1[2] = {0.7152f, 0.0722f}; memcpy(f0->rf.c1, c1, sizeof c1);
        f1->rf.nb = 1; f1->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = 2};
        prog_t p0, p1; prog(&p0, 8401, shader(@"v", 0), f0); prog(&p1, 8402, shader(@"v", 0), f1);
        tex_t s = tex(8, NVN_FORMAT_RGBA8), b0 = tex(8, NVN_FORMAT_RGBA8), b1 = tex(4, NVN_FORMAT_RGBA8); mtl_reg_tex(5, &s, NULL); mtl_reg_tex(6, &b0, NULL);
        float *ub = NULL; if (posix_memalign((void **)&ub, getpagesize(), 4096)) abort(); memset(ub, 0, 4096); ub[0] = 0.5f; mtl_pool_add(ub, 4096);
        chain c = chain_new(); set_rt(&c, &b0); p_bind_buf u = {1, 7, (uint64_t)ub, 256}; emit(&c, OP_BIND_UBO, &u, sizeof u);
        p_bind_handles h = {.stage = 1, .first = 0, .n = 1}; h.h[0] = 5; emit(&c, OP_BIND_TEX, &h, sizeof h); p_bind_program bp = {&p0, 3}; emit(&c, OP_BIND_PROGRAM, &bp, sizeof bp);
        p_draw d = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3}; emit(&c, OP_DRAW, &d, sizeof d); emit(&c, OP_END, NULL, 0);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t hh = (uint64_t)c.p; mtl_sub_index(&hh, 1); mtl_exec(c.p);
        EXPECT("泛光：起点生效 | bloom: the start took effect", 1, g_bloomq.state);
        tex_t hi = tex(8, NVN_FORMAT_RGBA8); mtl_reg_tex(5000, &hi, NULL);   // 游戏线程：新的最高号，登记表搬家 | Game thread: a new highest ID, the table moves
        chain c2 = chain_new(); set_rt(&c2, &b1); h.h[0] = 6; emit(&c2, OP_BIND_TEX, &h, sizeof h); bp.prog = &p1; emit(&c2, OP_BIND_PROGRAM, &bp, sizeof bp); emit(&c2, OP_DRAW, &d, sizeof d); emit(&c2, OP_END, NULL, 0);
        hh = (uint64_t)c2.p; mtl_sub_index(&hh, 1); mtl_exec(c2.p);
        EXPECT("泛光：原生截取画成 | bloom: the native capture was drawn", 2, g_bloomq.state);
        EXPECT("泛光：提交完成 | bloom: submit done", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    } else if (!strcmp(mode, "fx9")) {
        prog_t pg; prog(&pg, 8403, shader(@"v", 0), shader(@"red", 1));
        static tex_t col[8]; for (int i = 0; i < 8; i++) col[i] = tex(8, NVN_FORMAT_RGBA8); static tex_t ds; ds = tex(8, NVN_FORMAT_DEPTH32F_STENCIL8);
        chain c = chain_new(); p_set_rt r = {.n = 8}; for (int i = 0; i < 8; i++) r.color[i] = &col[i]; r.depth = &ds; emit(&c, OP_SET_RT, &r, sizeof r);
        p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = 8; emit(&c, OP_VIEWPORTS, &v, sizeof v); p_scissor sc = {0, 0, 8, 8}; emit(&c, OP_SCISSOR, &sc, sizeof sc);
        for (int i = 0; i < 8; i++) { p_clear_color x = {.index = i, .mask = 15, .rgba = {0, 0, 0, 1}}; emit(&c, OP_CLEAR_COLOR, &x, sizeof x); }
        p_clear_ds cd = {1.0f, 1, 0, 0xff}; emit(&c, OP_CLEAR_DS, &cd, sizeof cd);
        ds_st dss = {1, 1, NVN_DEPTH_FUNC_ALWAYS, 1, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_REPLACE, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_REPLACE};
        p_bind_state x = {.kind = 5}; memcpy(x.bytes, &dss, sizeof dss); emit(&c, OP_BIND_STATE, &x, sizeof x);
        p_bind_program bp = {&pg, 3}; emit(&c, OP_BIND_PROGRAM, &bp, sizeof bp); p_draw dw = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3}; emit(&c, OP_DRAW, &dw, sizeof dw);
        tex_t other = tex(8, NVN_FORMAT_RGBA8); p_set_rt r2 = {.n = 1}; r2.color[0] = &other; emit(&c, OP_SET_RT, &r2, sizeof r2);   // 10 个附件的通道在命令流里结束 | The 10-attachment pass ends inside the command stream
        exec(&c); EXPECT("10 个附件的通道：写回前瞻收尾 | 10-attachment pass: store lookahead finished", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    } else if (!strcmp(mode, "fx19")) {
        // 录制侧：负的起始号不录 | Record side: a negative first index is not recorded
        static uint8_t mem[65536] __attribute__((aligned(4096))); pool_t pool = {.mem = mem, .size = sizeof mem}; mtl_pool_add(mem, sizeof mem);
        NVNcommandBuffer cb; h_nvnCommandBufferInitialize(&cb, NULL); h_nvnCommandBufferAddCommandMemory(&cb, (const NVNmemoryPool *)&pool, 0, sizeof mem); h_nvnCommandBufferBeginRecording(&cb);
        float vp[4] = {0, 0, 8, 8}; NVNbufferRange rg = {0x1000, 64};
        h_nvnCommandBufferSetViewports(&cb, -1, 1, vp); h_nvnCommandBufferBindVertexBuffers(&cb, -1, 1, &rg);
        const uint8_t *p = (const uint8_t *)h_nvnCommandBufferEndRecording(&cb); int bad = 0;
        for (int guard = 0; guard < 1000; guard++) { op_hdr oh; memcpy(&oh, p, sizeof oh); if (oh.op == OP_END) break; if (oh.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; } bad += oh.op == OP_VIEWPORTS || oh.op == OP_BIND_VBUF; p += oh.size; }
        EXPECT("录制侧：起始号为负的视口 / 顶点缓冲不录 | record side: viewports / vertex buffers with a negative first index are not recorded", 0, bad);
        // 回放侧：命令内存是游戏的，回放照样不信下标 | Replay side: the command memory belongs to the game, so the replay does not trust the indices either
        state_reset();
        p_bind_buf u = {.stage = -1, .index = 0, .addr = 0x1000, .size = 16}; state_op(OP_BIND_UBO, &u);
        p_bind_buf sb = {.stage = 0, .index = -3, .addr = 0x1000, .size = 16}; state_op(OP_BIND_SSBO, &sb);
        p_bind_handles th = {.stage = 0, .first = -2, .n = 1}; th.h[0] = 5; state_op(OP_BIND_TEX, &th); state_op(OP_BIND_SAMP, &th);
        p_bind_handles ih = {.stage = -1, .first = 0, .n = 1}; ih.h[0] = 5; state_op(OP_BIND_IMG, &ih);
        p_viewports v = {.first = -1, .n = 1}; state_op(OP_VIEWPORTS, &v);
        p_bind_vbuf b = {.first = -1, .n = 1}; b.r[0].address = 0x2000; b.r[0].size = 64; state_op(OP_BIND_VBUF, &b);
        EXPECT("回放侧：负下标全部挡住（消毒器无报告）| replay side: every negative index is stopped (no sanitizer report)", 1, 1);
    } else if (!strcmp(mode, "fx11")) {
        NvShader *vs = shader(@"va", 0); vs->rf.nv = 1; vs->rf.v[0].loc = 0; vs->rf.v[0].nvn = 0; vs->rf.v[0].ct = NVN_CT_FLOAT; NvShader *fs = shader(@"red", 1);
        int streams[] = {14, 15, 20};
        for (int q = 0; q < 3; q++) { pso_key k; memset(&k, 0, sizeof k); k.cfmt[0] = MTLPixelFormatRGBA8Unorm; k.samples = 1; k.chmask = 0xffffffff;
            k.attr[0].fmt = 0x2E; k.attr[0].stream = streams[q]; if (streams[q] < 16) k.stream[streams[q]].stride = 16;
            MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0); NSError *e = nil; id ps = d ? [g_dev newRenderPipelineStateWithDescriptor:d error:&e] : nil;
            char n[128]; snprintf(n, sizeof n, "顶点流 %d：管线建得出、进程不中止 | stream %d: the pipeline builds and the process does not abort", streams[q], streams[q]); EXPECT(n, 1, ps != nil); }
    } else { fprintf(stderr, "用法 | usage: t_replay_san treg|gfxuse|rtrange|bloom|fx9|fx19|fx11\n"); return 2; }
    printf("== %s：%d 项，失败 %d ==\n", mode, count, fail); return fail != 0; } }
