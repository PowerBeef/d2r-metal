// Anisotropy override (nvn_mtl.m aniso_eff / samp_desc / aniso_want / aniso_latch) and sampler re-registration (mtl_reg_samp): the effective level per sampler kind,
// the level mapping, the mid-run level change (old samplers released only after the submissions that used them, new state, g_reg_gen) and the deferred release when
// the game registers a sampler id again. Run under the Metal validation layer (MTL_DEBUG_LAYER=1). Expectations are written by hand from the rules in nvn_mtl.m.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); count++; printf("%s: want %lld, got %lld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); fail += w_ != g_; } while (0)
static samp_par sp(int minf, int magf, int cmp, float aniso) { samp_par p = {0}; p.minf = (uint8_t)minf; p.magf = (uint8_t)magf; p.ws = p.wt = p.wr = 1; p.cmp_mode = (uint8_t)cmp; p.cmp_func = cmp ? 3 : 0; p.lod_max = 1000; p.aniso = aniso; return p; }
static NSUInteger eff(samp_par p, int level) { return aniso_eff(&p, level); }
static NSUInteger desc(samp_par p, int level) { return samp_desc(&p, level).maxAnisotropy; }
// The deferred-release record of this object: its fence, or 0 when it isn't waiting
static uint64_t deferred_fence(void *obj) {
    uint64_t f = 0; pthread_mutex_lock(&g_deferred_mu);
    for (NvDeferredRelease *d in g_deferred) if ((__bridge void *)d->object == obj) f = d->fence;
    pthread_mutex_unlock(&g_deferred_mu); return f;
}
static uint64_t submit_empty(void) {   // an empty submission, the way nvn_replay.m runs one
    static const op_hdr end = {OP_END, 8}; uint64_t eh = (uint64_t)(uintptr_t)&end;
    mtl_sub_begin(); mtl_sub_index(&eh, 1); mtl_exec_rest(NULL, 0); mtl_exec((const uint8_t *)&end); mtl_exec_rest(NULL, 0);
    uint64_t f = mtl_sub_commit(); mtl_wait(f, 5000000000ull); return f;
}
static void build(int sid) { os_unfair_lock_lock(&g_rmu); (void)samp_by_id(sid); os_unfair_lock_unlock(&g_rmu); }
int main(void) { setvbuf(stdout, NULL, _IONBF, 0);
    unsetenv("DM_NVN_ANISO");   // aniso_want caches it on its first call
    @autoreleasepool {
    // (a) aniso_eff and the descriptor samp_desc builds
    samp_par tri = sp(5, 1, 0, 1);
    EXPECT("trilinear at 16", 16, eff(tri, 16)); EXPECT("trilinear at 8", 8, eff(tri, 8)); EXPECT("trilinear at 4", 4, eff(tri, 4)); EXPECT("trilinear at 2", 2, eff(tri, 2));
    EXPECT("trilinear at 0 (the original)", 1, eff(tri, 0));
    EXPECT("compare at 16", 1, eff(sp(5, 1, 1, 1), 16));
    EXPECT("point mip (minf 2, mag nearest) at 16", 1, eff(sp(2, 0, 0, 1), 16)); EXPECT("linear, nearest mip (minf 3) at 16", 1, eff(sp(3, 1, 0, 1), 16));
    EXPECT("nearest, linear mip (minf 4) at 16", 1, eff(sp(4, 1, 0, 1), 16)); EXPECT("linear, no mip (minf 1) at 16", 1, eff(sp(1, 1, 0, 1), 16));
    EXPECT("minf 5 with mag nearest at 16", 1, eff(sp(5, 0, 0, 1), 16));
    EXPECT("authored 4 at 2", 4, eff(sp(5, 1, 0, 4), 2)); EXPECT("authored 4 at 16", 16, eff(sp(5, 1, 0, 4), 16)); EXPECT("authored 4 at 0", 4, eff(sp(5, 1, 0, 4), 0));
    EXPECT("authored 16 at 8", 16, eff(sp(5, 1, 0, 16), 8));
    { samp_par b = tri; b.ws = b.wt = 5; EXPECT("clamp-to-border trilinear at 16 (wrap mode not consulted)", 16, eff(b, 16)); }
    { samp_par m = sp(5, 1, 0, 4); m.ws = m.wt = 6; EXPECT("mirrored repeat, authored 4, at 16", 16, eff(m, 16)); }
    EXPECT("authored 0.0 at 0", 1, eff(sp(5, 1, 0, 0.0f), 0)); EXPECT("authored 32.0 at 0", 16, eff(sp(5, 1, 0, 32.0f), 0)); EXPECT("authored NaN at 0", 1, eff(sp(5, 1, 0, NAN), 0));
    EXPECT("samp_desc: trilinear at 16", 16, desc(tri, 16)); EXPECT("samp_desc: trilinear at 0", 1, desc(tri, 0)); EXPECT("samp_desc: game aniso 4 at 2", 4, desc(sp(5, 1, 0, 4), 2));
    EXPECT("samp_desc: compare at 16", 1, desc(sp(5, 1, 1, 1), 16)); EXPECT("samp_desc: minf 3 at 16", 1, desc(sp(3, 1, 0, 1), 16));
    EXPECT("samp_desc: minf 1 at 16", 1, desc(sp(1, 1, 0, 1), 16)); EXPECT("samp_desc: mag nearest at 16", 1, desc(sp(5, 0, 0, 1), 16));
    EXPECT("samp_desc: mip filter linear for minf 5", MTLSamplerMipFilterLinear, samp_desc(&tri, 16).mipFilter);

    // (b) aniso_want: g_aniso maps to 0 / 2 / 4 / 8 / 16; unset with DM_NVN_ANISO unset = the weak dm_video_aniso (0)
    static const int in[] = {0, 1, 2, 3, 4, 5, 8, 12, 16, 99}, out[] = {0, 0, 2, 2, 4, 4, 8, 8, 16, 16};
    for (int i = 0; i < 10; i++) { char nm[64]; snprintf(nm, sizeof nm, "aniso_want with g_aniso %d", in[i]); g_aniso = in[i]; EXPECT(nm, out[i], aniso_want()); }
    g_aniso = -1; EXPECT("aniso_want unset: the weak dm_video_aniso", 0, aniso_want());

    // (c) The level changes mid-run
    mtl_init();
    samp_par cmp = sp(5, 1, 1, 1);
    mtl_reg_samp(5, &tri); mtl_reg_samp(6, &cmp);
    g_aniso = 0; submit_empty();
    build(5); build(6);
    void *old5 = g_sview[5], *old6 = g_sview[6]; uint64_t rid5 = g_srid[5];
    EXPECT("both samplers built", 1, old5 && old6);
    g_aniso = 16; uint64_t gen0 = g_reg_gen;
    mtl_sub_begin();
    EXPECT("0 → 16: the level latched", 16, g_aniso_cur); EXPECT("0 → 16: both built samplers released", 2, g_aniso_rebuilt);
    EXPECT("0 → 16: sampler 5 dropped", 0, g_sview[5] != NULL); EXPECT("0 → 16: its resource id cleared", 0, g_srid[5]);
    EXPECT("0 → 16: argument blocks refill (g_reg_gen moved)", 1, g_reg_gen > gen0);
    EXPECT("0 → 16: old sampler 5 waits for this submission", g_fence + 1, deferred_fence(old5)); EXPECT("0 → 16: old sampler 6 waits for this submission", g_fence + 1, deferred_fence(old6));
    EXPECT("0 → 16: that submission isn't done", 1, g_fence + 1 > g_done);
    build(5); build(6);
    EXPECT("16: sampler 5 rebuilt", 1, g_srid[5] != 0 && g_srid[5] != rid5);
    uint64_t f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
    defer_drain();   // the feedback handler wakes waiters before it drains, so drain here instead of racing it
    EXPECT("after the submission: old sampler 5 released", 0, deferred_fence(old5)); EXPECT("after the submission: old sampler 6 released", 0, deferred_fence(old6));
    void *cur5 = g_sview[5]; g_aniso_rebuilt = -1;
    submit_empty();
    EXPECT("16 again: no latch", -1, g_aniso_rebuilt); EXPECT("16 again: sampler 5 kept", 1, g_sview[5] == cur5);
    g_aniso = 0; submit_empty();
    EXPECT("16 → 0: both samplers (rebuilt at 16) released", 2, g_aniso_rebuilt); EXPECT("16 → 0: the level latched", 0, g_aniso_cur);
    EXPECT("level changes: no command buffer error", 0, mtl_last_commit_error());

    // (d) The game registers sampler 5 again while it is built: the old state is released after the fence, not at once
    build(5); void *old = g_sview[5];
    EXPECT("sampler 5 rebuilt before re-registration", 1, old != NULL);
    mtl_sub_begin();
    samp_par tri2 = tri; tri2.lod_bias = 0.5f; mtl_reg_samp(5, &tri2);
    EXPECT("re-registration: sampler 5 dropped", 0, g_sview[5] != NULL); EXPECT("re-registration: its resource id cleared", 0, g_srid[5]);
    EXPECT("re-registration: old state waits for this submission", g_fence + 1, deferred_fence(old));
    f = mtl_sub_commit(); mtl_wait(f, 5000000000ull); defer_drain();
    EXPECT("re-registration: old state released after the submission", 0, deferred_fence(old));

    // (e) aniso_latch on its own: two built samplers, one level change, then no change
    build(5); build(6); g_aniso = 8; gen0 = g_reg_gen;
    aniso_latch();
    EXPECT("latch to 8: sampler 5 dropped", 0, g_sview[5] != NULL); EXPECT("latch to 8: sampler 6 dropped", 0, g_sview[6] != NULL);
    EXPECT("latch to 8: g_reg_gen +1", gen0 + 1, g_reg_gen); EXPECT("latch to 8: level", 8, g_aniso_cur);
    aniso_latch();
    EXPECT("latch to 8 again: g_reg_gen unchanged", gen0 + 1, g_reg_gen);
    submit_empty(); defer_drain();
    EXPECT("all submissions: no command buffer error", 0, mtl_last_commit_error());
    }
    printf("t_aniso: %d checks, %d failed\n", count, fail); return fail != 0;
}
