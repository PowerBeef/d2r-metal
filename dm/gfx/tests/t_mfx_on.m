// MetalFX policy and pixel checks on this Mac's GPU (nvn_mtl.m's MetalFX section, included whole). The cases run in a fixed order;
// ./t_mfx_on <name> runs one. Counters (builds, tries) are compared with a snapshot taken at the start of each case, and the mfx events
// are read through a recorder installed as the event hook. Run under MTL_DEBUG_LAYER=1 (make run does).
//   compiler  every Metal 4 scaler is built with one shared compiler; mtl_mfx_supported answers yes on this Mac
//   fail      a failed build keeps the installed set, is remembered per request key and tried again after MFX_RETRY requests, or at once at
//             another size; turning Upscaling off forgets the failure
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); count++; \
    printf("%s: expected %lld, got %lld %s\n", name, w_, g_, w_ == g_ ? "ok" : "FAIL"); fail += w_ != g_; } while (0)

// Event recorder: each event as "ev k=v k=v ..." (strings unquoted, doubles as %g), the last 64 kept
static char g_tev[64][256]; static int g_ntev;
static void rec_ev(const char *ev, const char *sig, ...) {
    char *b = g_tev[g_ntev++ % 64]; size_t n = (size_t)snprintf(b, 256, "%s", ev);
    va_list ap; va_start(ap, sig);
    for (const char *s = sig; s && *s && n < 250;) {
        while (*s == ' ') s++;
        const char *c = strchr(s, ':'); if (!c || !c[1]) break;
        n += (size_t)snprintf(b + n, 256 - n, " %.*s=", (int)(c - s), s);
        switch (c[1]) {
            case 's': { const char *v = va_arg(ap, const char *); n += (size_t)snprintf(b + n, 256 - n, "%s", v ? v : "null"); break; }
            case 'd': case 'b': n += (size_t)snprintf(b + n, 256 - n, "%d", va_arg(ap, int)); break;
            case 'l': n += (size_t)snprintf(b + n, 256 - n, "%lld", va_arg(ap, long long)); break;
            case 'u': n += (size_t)snprintf(b + n, 256 - n, "%llu", va_arg(ap, unsigned long long)); break;
            case 'f': n += (size_t)snprintf(b + n, 256 - n, "%g", va_arg(ap, double)); break;
            default: va_end(ap); return;
        }
        s = c + 2;
    }
    va_end(ap);
}
// Events since mark whose text holds every space-separated "k=v" of want (and starts with want's first word)
static int ev_count(int mark, const char *want) {
    int hits = 0;
    for (int i = mark; i < g_ntev; i++) {
        const char *e = g_tev[i % 64]; char w[256]; strlcpy(w, want, sizeof w); int ok = 1; char *save = NULL;
        for (char *t = strtok_r(w, " ", &save), *first = t; t; t = strtok_r(NULL, " ", &save)) {
            char pat[260]; snprintf(pat, sizeof pat, t == first ? "%s " : " %s", t);
            size_t pl = strlen(pat);
            if (t == first ? strncmp(e, pat, pl) != 0 : !(strstr(e, pat) && (strstr(e, pat)[pl] == ' ' || strstr(e, pat)[pl] == 0))) { ok = 0; break; }
        }
        hits += ok;
    }
    return hits;
}

static void c_compiler(void) {
    EXPECT("compiler: one Metal 4 compiler, created once", 1, mfx_compiler() != nil && mfx_compiler() == mfx_compiler());
    EXPECT("compiler: mtl_mfx_supported answers yes on this Mac", 1, mtl_mfx_supported());
}

static int grab(NSUInteger iw, NSUInteger ih) { return mfx_ready(&g_mfx_grab, 0, MTLPixelFormatRGBA8Unorm, iw, ih, 64, 36); }
static void c_fail(void) {
    mfx_t *m = &g_mfx_grab;   // older API, no settle: every request is one build attempt
    EXPECT("fail: the working set builds", 1, grab(56, 32));
    int t0 = m->tries, b0 = m->builds, ev0 = g_ntev; id s0 = m->scaler;
    g_mfx_fail_inject = 1;
    EXPECT("fail: a failed build answers 0", 0, grab(60, 34));
    EXPECT("fail: the installed set is kept", 1, m->scaler == s0 && m->iw == 56);
    EXPECT("fail: no permanent off", 0, m->bad);
    EXPECT("fail: the failure is remembered", 1, m->fail_on);
    EXPECT("fail: one attempt", 1, m->tries - t0);
    EXPECT("fail: one fail event (textures)", 1, ev_count(ev0, "mfx who=grab op=fail why=textures iw=60 ih=34"));
    int r = 0; for (int i = 0; i < 10; i++) r += grab(60, 34);
    EXPECT("fail: the same key again: no build", 0, r);
    EXPECT("fail: ... and no attempt", 1, m->tries - t0);
    r = 0; for (int i = 0; i < MFX_RETRY - 11; i++) r += grab(60, 34);
    EXPECT("fail: up to MFX_RETRY - 1 requests after the failure, still no attempt", 1, m->tries - t0 + r);
    EXPECT("fail: request MFX_RETRY after the failure tries again and builds", 1, grab(60, 34));
    EXPECT("fail: ... one more attempt", 2, m->tries - t0);
    EXPECT("fail: ... the new set is installed", 60, m->iw);
    EXPECT("fail: ... one build", 1, m->builds - b0);
    EXPECT("fail: ... why = retry", 1, m->why && !strcmp(m->why, "retry"));
    EXPECT("fail: ... build event why=retry", 1, ev_count(ev0, "mfx who=grab op=build why=retry iw=60 ih=34"));
    EXPECT("fail: ... the failure is forgotten", 0, m->fail_on);
    g_mfx_fail_inject = 1;
    EXPECT("fail: a failing key at 62x35", 0, grab(62, 35));
    int t1 = m->tries;
    EXPECT("fail: another size is tried at once", 1, grab(63, 35));
    EXPECT("fail: ... one attempt", 1, m->tries - t1);
    g_mfx_fail_inject = 1; grab(61, 35);
    EXPECT("fail: before mfx_off a failure is remembered", 1, m->fail_on);
    mfx_off(m);
    EXPECT("fail: mfx_off forgets it", 0, m->fail_on);
    EXPECT("fail: mfx_off puts the set down", 1, m->scaler == nil);
    EXPECT("fail: an off drop event", 1, ev_count(ev0, "mfx who=grab op=drop why=off"));
    for (int i = 0; i < 4; i++) mfx_retire_tick();
}

static const struct { const char *name; void (*fn)(void); } k_cases[] = {
    {"compiler", c_compiler}, {"fail", c_fail},
};
int main(int argc, char **argv) { setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_MFX", "1", 1); setenv("DM_MFX3D", "1", 1);   // before any mfx_on / mfx3d_on call: both latch their switch once
    mtl_init();
    if (![MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) { printf("no Metal 4 MetalFX spatial scaler on this machine: skipped\n== 0 checks, 0 failed ==\n"); return 0; }
    g_dm_event_fn = rec_ev;
    int ran = 0;
    for (size_t i = 0; i < sizeof k_cases / sizeof *k_cases; i++) {
        if (argc > 1 && strcmp(argv[1], k_cases[i].name)) continue;
        printf("-- %s\n", k_cases[i].name); @autoreleasepool { k_cases[i].fn(); } ran++;
    }
    if (!ran) { fprintf(stderr, "usage: t_mfx_on [case]; cases:"); for (size_t i = 0; i < sizeof k_cases / sizeof *k_cases; i++) fprintf(stderr, " %s", k_cases[i].name); fprintf(stderr, "\n"); return 2; }
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0;
}
