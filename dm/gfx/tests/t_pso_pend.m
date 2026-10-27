// 并行预编槽位回收单测：384 个槽被没人领的预测占满后，新提交里的预测仍能丢出（回收更早提交里已编完的最老一条），
// 过了 PSO_PEND_AGE 次提交没人领的在提交开头被回收；同一次提交里丢满时不回收本次的（表满没丢计数 +1）。
// Unit test for parallel precompile slot recycling: once the 384 slots are full of predictions nobody takes, a prediction in a new submission can still be kicked (the oldest finished entry from an earlier submission is recycled),
// entries untaken for PSO_PEND_AGE submissions are recycled at the start of a submission; when the table fills within one submission, entries of that submission are not recycled (the not-kicked-table-full count goes up by 1).
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
    printf("== %d 项，失败 %d ==\n", count, fail); return fail != 0; } }
