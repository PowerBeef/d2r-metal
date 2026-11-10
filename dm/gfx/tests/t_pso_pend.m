// Unit test for parallel precompile slot recycling: once the 384 slots are full of predictions nobody takes, a prediction in a new submission can still be kicked (the oldest finished entry from an earlier submission is recycled),
// entries untaken for PSO_PEND_AGE submissions are recycled at the start of a submission; when the table fills within one submission, entries of that submission are not recycled (the not-kicked-table-full count goes up by 1).
// The predictor's cutoff (pso_predict_on / pso_predict_allowed: auto, always, never), its [bootprof] label, and the merged-pass attribution of untaken predictions.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w=(want), g=(got); count++; printf("%s 期望 %llu → 真结果 %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w==g?"✓":"✗"); fail += w != g; } while (0)
static NvShader *t_vsh, *t_fsh;
// 第 i 把键：只改一个顶点着色器不读的属性偏移，键各不相同、编出来的管线相同（Metal 自己的缓存让后面的很快）
// Key number i: only an attribute offset the vertex shader does not read changes, so the keys all differ and the compiled pipelines are the same (Metal's own cache makes the later ones fast)
static pso_key key(int i) { pso_key k; memset(&k, 0, sizeof k); k.vs = (__bridge void *)t_vsh; k.fs = (__bridge void *)t_fsh; k.cfmt[0] = MTLPixelFormatRGBA8Unorm; k.samples = 1; k.chmask = 0xf; k.attr[15].offset = (uint32_t)i; return k; }
static void kick(int i) { pso_key k = key(i); pso_pend_kick(&k, t_vsh, t_fsh); }
static int in_table(int i) { pso_key k = key(i); pthread_mutex_lock(&g_pp_mu); int at = pso_pend_find(&k); pthread_mutex_unlock(&g_pp_mu); return at >= 0; }
static void wait_done(void) {   // 等后台把表里的都编完 | wait for the background to finish compiling everything in the table
    pthread_mutex_lock(&g_pp_mu);
    for (;;) { int busy = 0; for (int i = 0; i < PSO_PEND_MAX; i++) busy |= g_pp[i].used && !g_pp[i].done; if (!busy) break; pthread_cond_wait(&g_pp_cv, &g_pp_mu); }
    pthread_mutex_unlock(&g_pp_mu);
}
int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();
    NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 v(uint n [[vertex_id]]) { return float4(0, 0, 0, 1); }\nfragment float4 red() { return float4(1, 0, 0, 1); }\n" options:nil error:&e];
    if (!l) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); return 1; }
    t_vsh = [NvShader new]; t_vsh->fn = [l newFunctionWithName:@"v"]; t_fsh = [NvShader new]; t_fsh->stage = 1; t_fsh->fn = [l newFunctionWithName:@"red"];
    // ① 第 1 次提交：丢满 384 条，都编完、都没人领
    // ① Submission 1: kick 384 entries, all compiled, none taken
    g_sub_serial = 1;
    for (int i = 0; i < PSO_PEND_MAX; i++) kick(i);
    wait_done();
    EXPECT("丢满：在表槽数", PSO_PEND_MAX, (uint64_t)g_pp_live); EXPECT("丢满：丢出条数", PSO_PEND_MAX, g_pp_kicked);
    // ② 同一次提交里再丢：表满且都是本次的 → 不回收、不丢，表满没丢 +1
    // ② Kick again in the same submission: the table is full of this submission's entries → nothing recycled, not kicked, not-kicked-table-full +1
    kick(1000);
    EXPECT("同一提交表满：新预测不在表里", 0, (uint64_t)in_table(1000)); EXPECT("同一提交表满：表满没丢 1 条", 1, g_pp_full); EXPECT("同一提交表满：不回收", 0, g_pp_aged);
    // ③ 下一次提交：表满时回收更早提交里最老的一条已编完的，新预测照样丢出、能领到管线（原先这里永远丢不出去）
    // ③ Next submission: with the table full, the oldest finished entry from an earlier submission is recycled, the new prediction is kicked and its pipeline can be taken (it used to never be kicked here)
    g_sub_serial = 2; pso_pend_age();
    EXPECT("未到期：提交开头不回收", 0, g_pp_aged);
    kick(1001);
    EXPECT("下一提交表满：新预测进表", 1, (uint64_t)in_table(1001)); EXPECT("下一提交表满：回收 1 条", 1, g_pp_aged);
    { pso_key k = key(1001); id<MTLRenderPipelineState> ps = pso_pend_take(&k); EXPECT("领到新预测编出的管线", 1, (uint64_t)(ps != nil)); }
    EXPECT("领走后在表槽数", PSO_PEND_MAX - 1, (uint64_t)g_pp_live);
    // ④ 过了 PSO_PEND_AGE 次提交：提交开头把没人领的全部回收，再丢新的
    // ④ PSO_PEND_AGE submissions later: the start of a submission recycles every untaken entry, then new ones are kicked again
    g_sub_serial = 1 + PSO_PEND_AGE; pso_pend_age();
    EXPECT("到期：在表槽数归 0", 0, (uint64_t)g_pp_live); EXPECT("到期：累计回收 = 384", PSO_PEND_MAX, g_pp_aged);
    kick(2000); wait_done();
    EXPECT("回收后再丢：进表", 1, (uint64_t)in_table(2000));
    { pso_key k = key(2000); EXPECT("回收后再丢：领到管线", 1, (uint64_t)(pso_pend_take(&k) != nil)); }
    // ⑤ 还在编的那条不回收（后台按槽号写回）：直接把一条标成「没编完」再到期
    // ⑤ An entry still compiling is not recycled (the background writes back by slot index): mark one as "not finished" and let it expire
    kick(3000); wait_done();
    pthread_mutex_lock(&g_pp_mu); int at; { pso_key k = key(3000); at = pso_pend_find(&k); } if (at >= 0) g_pp[at].done = 0; pthread_mutex_unlock(&g_pp_mu);
    g_sub_serial += 2 * PSO_PEND_AGE; pso_pend_age();
    EXPECT("在编的那条到期也不回收", 1, (uint64_t)in_table(3000));
    pthread_mutex_lock(&g_pp_mu); if (at >= 0) g_pp[at].done = 1; pthread_mutex_unlock(&g_pp_mu);
    g_sub_serial++; pso_pend_age();
    EXPECT("编完后下次提交开头回收", 0, (uint64_t)in_table(3000));
    // The predictor's cutoff: on until 24 predictions are resolved, then only while at least 10% of them were taken
    EXPECT("cutoff (0,0) on", 1, (uint64_t)pso_predict_on(0, 0)); EXPECT("cutoff (0,23) on", 1, (uint64_t)pso_predict_on(0, 23));
    EXPECT("cutoff (0,24) off", 0, (uint64_t)pso_predict_on(0, 24)); EXPECT("cutoff (3,21) on", 1, (uint64_t)pso_predict_on(3, 21));
    EXPECT("cutoff (2,25) off", 0, (uint64_t)pso_predict_on(2, 25)); EXPECT("cutoff (4,36) on", 1, (uint64_t)pso_predict_on(4, 36));
    EXPECT("cutoff (4,37) off", 0, (uint64_t)pso_predict_on(4, 37));
    g_pp_mode = 2; g_pp_off = 0; g_pp_taken = 2; g_pp_aged = 25;
    EXPECT("auto: 2 of 27 taken switches it off", 0, (uint64_t)pso_predict_allowed()); EXPECT("auto: the switch is recorded", 1, (uint64_t)g_pp_off);
    g_pp_taken = 100; EXPECT("auto: off stays off for the run", 0, (uint64_t)pso_predict_allowed());
    g_pp_mode = 1; EXPECT("DM_PSO_PREDICT=1: always on", 1, (uint64_t)pso_predict_allowed());
    g_pp_mode = 0; EXPECT("DM_PSO_PREDICT=0: never", 0, (uint64_t)pso_predict_allowed());
    // The [bootprof] label reads DM_PSO_PREDICT itself, so it is right before the first miss has resolved the mode
    setenv("DM_PSO_PREDICT", "0", 1); g_pp_mode = -1; g_pp_off = 0;
    EXPECT("DM_PSO_PREDICT=0: the [bootprof] label is off before any miss", 1, (uint64_t)!strcmp(pso_predict_label(), "off"));
    setenv("DM_PSO_PREDICT", "1", 1); g_pp_mode = -1;
    EXPECT("DM_PSO_PREDICT=1: the label is on", 1, (uint64_t)!strcmp(pso_predict_label(), "on"));
    unsetenv("DM_PSO_PREDICT"); g_pp_mode = -1;
    EXPECT("auto: the label is on until it switches off", 1, (uint64_t)!strcmp(pso_predict_label(), "on"));
    g_pp_off = 1; EXPECT("auto, switched off: the label is off", 1, (uint64_t)!strcmp(pso_predict_label(), "off")); g_pp_off = 0;
    // The cutoff is judged when predictions age out, not only at the next miss
    g_pp_mode = 1; g_sub_serial += 2 * PSO_PEND_AGE; pso_pend_age();
    g_pp_mode = 2; g_pp_off = 0; g_pp_taken = g_pp_aged = 0;
    for (int i = 0; i < 30; i++) kick(6000 + i);
    wait_done(); g_sub_serial += PSO_PEND_AGE; pso_pend_age();
    EXPECT("30 untaken predictions aged out: off at the start of that submission", 1, (uint64_t)g_pp_off);
    // Attribution: an untaken prediction whose program then misses in a merged pass is counted once
    g_pp_merged = 0; g_sub_serial += 2 * PSO_PEND_AGE; pso_pend_age();
    kick(5000); wait_done();
    pso_pend_note_merged((__bridge void *)t_vsh, (__bridge void *)t_fsh); EXPECT("merged-pass miss of a predicted program: counted", 1, g_pp_merged);
    pso_pend_note_merged((__bridge void *)t_vsh, (__bridge void *)t_fsh); EXPECT("the same prediction again: not counted twice", 1, g_pp_merged);
    pso_pend_note_merged((__bridge void *)t_fsh, (__bridge void *)t_vsh); EXPECT("another program: no match", 1, g_pp_merged);
    // ---- Pipelines a draw may wait for (nvn_mtl.m): which draws may, and the wait itself ----
    // An effect draw: it blends onto its one output, it is depth-tested, it writes no depth and no stencil, and its pass has a depth plane
    #define FX_KEY(i) ({ pso_key k_ = key(i); k_.dfmt = MTLPixelFormatDepth32Float; k_.blend_en = 1; k_.blend[0] = (blend_st){0, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; k_; })
    #define CACHED(k) ((uint64_t)(hm_get_ptr(&g_pso, &(k)) != NULL))
    #define TABLED(k) ({ pthread_mutex_lock(&g_pp_mu); int a_ = pso_pend_find(&(k)); pthread_mutex_unlock(&g_pp_mu); (uint64_t)(a_ >= 0); })
    state_reset(); S.ds.dt = 1; S.ds.df = NVN_DEPTH_FUNC_LESS; S.ds.dw = 0; S.ds.st = 0; g_pso_async = 1; g_pa_test_ms = 0;
    { pso_key k = FX_KEY(7000), o;
      EXPECT("an effect draw may wait for its pipeline", 1, (uint64_t)pso_can_wait(&k, 0));
      S.ds.dw = 1; EXPECT("a draw that writes depth: compiled in the frame", 0, (uint64_t)pso_can_wait(&k, 0)); S.ds.dw = 0;
      S.ds.dt = 0; EXPECT("no depth test: in the frame", 0, (uint64_t)pso_can_wait(&k, 0)); S.ds.dt = 1;
      S.ds.df = NVN_DEPTH_FUNC_ALWAYS; EXPECT("a depth test that always passes: in the frame", 0, (uint64_t)pso_can_wait(&k, 0)); S.ds.df = NVN_DEPTH_FUNC_LESS;
      S.ds.st = 1; EXPECT("stencil on: in the frame", 0, (uint64_t)pso_can_wait(&k, 0)); S.ds.st = 0;
      o = k; o.dfmt = 0; EXPECT("a pass with no depth plane (full-screen steps, the interface): in the frame", 0, (uint64_t)pso_can_wait(&o, 0));
      o = k; o.blend_en = 0; EXPECT("not blended: in the frame", 0, (uint64_t)pso_can_wait(&o, 0));
      o = k; o.blend[0].dst_rgb = NVN_BLEND_FUNC_ZERO; EXPECT("blending that overwrites the target: in the frame", 0, (uint64_t)pso_can_wait(&o, 0));
      o = k; o.chmask = 0; EXPECT("a draw that writes no colour: in the frame", 0, (uint64_t)pso_can_wait(&o, 0));
      o = k; o.cfmt[1] = MTLPixelFormatRGBA8Unorm; o.chmask = 0xff; EXPECT("a second output written without blending: in the frame", 0, (uint64_t)pso_can_wait(&o, 0));
      o.blend_en = 3; o.blend[1] = o.blend[0]; o.blend[1].target = 1; EXPECT("both outputs blended: may wait", 1, (uint64_t)pso_can_wait(&o, 0));
      o = k; o.fsfn = (const void *)(uintptr_t)1; EXPECT("a specialised fragment: in the frame", 0, (uint64_t)pso_can_wait(&o, 0));
      g_defer_replaying = 1; EXPECT("the replay of a deferred pass: in the frame", 0, (uint64_t)pso_can_wait(&k, 0)); g_defer_replaying = 0;
      g_pso_async = 0; EXPECT("DM_PSO_ASYNC=0: in the frame", 0, (uint64_t)pso_can_wait(&k, 0)); g_pso_async = 1;
      o = k; memset(o.lmap, 0xff, sizeof o.lmap); o.lmap[0] = 0; EXPECT("a merged pass, the output mapped: may wait", 1, (uint64_t)pso_can_wait(&o, 1));
      o.lmap[0] = 0xff; EXPECT("a merged pass, no output mapped: in the frame", 0, (uint64_t)pso_can_wait(&o, 1)); }
    g_pp_mode = 0; wait_done(); g_sub_serial += 2 * PSO_PEND_AGE; pso_pend_age();   // an empty table
    EXPECT("the table is empty before the waits", 0, (uint64_t)g_pp_live);
    { id<MTLRenderPipelineState> out = nil; uint64_t landed0 = g_pa_landed, draws0 = g_pa_draws, new0 = g_bp_pso_n; pso_key k = FX_KEY(7100);
      g_pa_test_ms = 200;
      EXPECT("first draw of a missing pipeline: it waits", 1, (uint64_t)pso_wait_for(&k, t_vsh, t_fsh, 0, &out)); EXPECT("the build is in the table", 1, TABLED(k));
      EXPECT("the same draw again: it still waits", 1, (uint64_t)pso_wait_for(&k, t_vsh, t_fsh, 0, &out)); EXPECT("two draws were left out", 2, g_pa_draws - draws0);
      wait_done(); EXPECT("built, but not in the cache before a submission starts", 0, CACHED(k));
      g_sub_serial++; pso_pend_age();
      EXPECT("the next submission's start puts it in the cache", 1, CACHED(k)); EXPECT("and counts it", 1, g_pa_landed - landed0); EXPECT("as one new pipeline", 1, g_bp_pso_n - new0); EXPECT("and frees its table entry", 0, TABLED(k));
      pso_key k2 = FX_KEY(7101);
      EXPECT("another missing pipeline: its draw waits", 1, (uint64_t)pso_wait_for(&k2, t_vsh, t_fsh, 0, &out)); wait_done();
      EXPECT("built before the submission ends: the next draw gets it at once", 2, (uint64_t)pso_wait_for(&k2, t_vsh, t_fsh, 0, &out)); EXPECT("the pipeline is handed over", 1, (uint64_t)(out != nil)); EXPECT("and is in the cache", 1, CACHED(k2));
      pso_key k3 = FX_KEY(7102); g_pa_test_ms = 0; uint64_t aged0;
      EXPECT("a third: its draw waits", 1, (uint64_t)pso_wait_for(&k3, t_vsh, t_fsh, 0, &out)); wait_done();
      aged0 = g_pp_aged; g_sub_serial += 3 * PSO_PEND_AGE; pso_pend_age();
      EXPECT("many submissions later it is not thrown away as stale", 0, g_pp_aged - aged0); EXPECT("it is in the cache", 1, CACHED(k3));
      kick(7200); wait_done(); pso_key p = key(7200);
      EXPECT("a finished prediction is left to the compile path", 0, (uint64_t)pso_wait_for(&p, t_vsh, t_fsh, 0, &out)); EXPECT("which can still take it", 1, (uint64_t)(pso_pend_take(&p) != nil));
      EXPECT("the table is empty after the waits", 0, (uint64_t)g_pp_live); }
    printf("== %d 项，失败 %d ==\n", count, fail); return fail != 0; } }
