// MetalFX policy and pixel checks on this Mac's GPU (nvn_mtl.m's MetalFX section, included whole). The cases run in a fixed order;
// ./t_mfx_on <name> runs one. Counters (builds, tries) are compared with a snapshot taken at the start of each case, and the mfx events
// are read through a recorder installed as the event hook. Run under MTL_DEBUG_LAYER=1 (make run does).
//   compiler  every Metal 4 scaler is built with one shared compiler; mtl_mfx_supported answers yes on this Mac
//   fail      a failed build keeps the installed set, is remembered per request key and tried again after MFX_RETRY requests, or at once at
//             another size; turning Upscaling off forgets the failure
//   alloc     the 3D set's input allocation (mfx3d_alloc): sized for the configured 3D scale + MFX_PAD, grow-only without one, and an Apply
//             (configured scale changed) rebuilds once, also when replayed through mfx_ready
//   equiv     content inside a larger allocation gives the same picture as an exact-size set (inputContentWidth), and no texel of the unused part of
//             the allocation (filled with NaN by an earlier frame) reaches the output
//   content   Frame Drops steps inside the allocation reuse the set: no build, one content event each
//   idle      unused sets are put down: the 3D set after MFX_IDLE_3D submissions without a 3D draw, the present and screenshot sets after MFX_IDLE
//             presents without use; then released once the frames that may have used them have run
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

// ---- 3D set helpers: a source / mask texture with given texels, a colour-0 target, one encode in its own submission, read-backs
static id<MTLTexture> mk_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, const void *px, NSUInteger row) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:row];
    res_add(t); res_commit(); return t;
}
static id<MTLTexture> mk_h4(NSUInteger w, NSUInteger h, void (^f)(NSUInteger x, NSUInteger y, __fp16 v[4])) {   // RGBA16F
    __fp16 *px = calloc(w * h * 4, sizeof *px); for (NSUInteger y = 0; y < h; y++) for (NSUInteger x = 0; x < w; x++) f(x, y, px + (y * w + x) * 4);
    id<MTLTexture> t = mk_tex(MTLPixelFormatRGBA16Float, w, h, px, w * 8); free(px); return t;
}
static id<MTLTexture> mk_r8(NSUInteger w, NSUInteger h, const uint8_t *px) { return mk_tex(MTLPixelFormatR8Unorm, w, h, px, w); }
static id<MTLTexture> mk_r8_flat(NSUInteger w, NSUInteger h, uint8_t v) { uint8_t *px = malloc(w * h); memset(px, v, w * h); id<MTLTexture> t = mk_r8(w, h, px); free(px); return t; }
static tex_t mk_dst(uint64_t serial, unsigned w, unsigned h, int fmt) {
    tex_t t = {.serial = serial}; t.d.width = w; t.d.height = h; t.d.depth = t.d.levels = t.d.samples = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = fmt; return t;
}
static id<MTLBuffer> rb_copy(id<MTLTexture> t, NSUInteger bpp) {   // in the open submission: copy t into a shared buffer
    id<MTLBuffer> b = [g_dev newBufferWithLength:t.width * t.height * bpp options:MTLResourceStorageModeShared]; res_add(b);
    [blit() copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
                  toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * bpp destinationBytesPerImage:t.width * t.height * bpp];
    return b;
}
static void sub_done(const char *what) {
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    uint64_t f = mtl_sub_commit(); int ok = mtl_wait(f, 5000000000ull);
    if (!ok || mtl_last_commit_error()) { char n[160]; snprintf(n, sizeof n, "%s: submission completes without error", what); EXPECT(n, 1, ok && !mtl_last_commit_error()); }
}
// One 3D encode into colour 0 = dst in its own submission; returns mfx3d_encode's answer, *px = dst read back (RGBA8), *mo = the set's output (RGBA16F)
static int enc3d(tex_t *dst, id<MTLTexture> src, NSUInteger cw, NSUInteger ch, NSUInteger aw, NSUInteger ah, id<MTLTexture> mask, MTLViewport vp, id<MTLBuffer> *px, id<MTLBuffer> *mo) {
    memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = dst;
    mtl_sub_begin();
    int r = mfx3d_encode(src, cw, ch, aw, ah, mask, vp);
    if (px) *px = rb_copy(tex_mtl(dst), 4);
    if (mo) *mo = r ? rb_copy(g_mfx_3d.out, 8) : nil;
    sub_done("3D encode"); return r;
}

static void c_alloc(void) {
    struct { NSUInteger cw, ch, ow, oh; float cfg; NSUInteger siw, sih; NSUInteger aw, ah; } k[] = {
        {2151, 990, 2868, 1320, 0.75f, 0, 0, 2153, 992},     // level 0 at 75%: content + pad
        {1721, 792, 2868, 1320, 0.75f, 0, 0, 2153, 992},     // a Frame Drops level inside the 75% allocation
        {2491, 1146, 2622, 1206, 1.0f, 0, 0, 2622, 1206},    // 100%: never above the output
        {1721, 792, 2868, 1320, 0, 2153, 992, 2153, 992},    // cfg unknown: grow only, keep the set
        {2200, 1000, 2868, 1320, 0, 2153, 992, 2202, 1002},  // cfg unknown, content past the set: content + pad
        {1200, 675, 1600, 900, 0, 0, 0, 1202, 677},          // cfg unknown, no set
        {2300, 1000, 2868, 1320, 0.75f, 0, 0, 2302, 1002},   // content ahead of cfg
        {2438, 1122, 2868, 1320, 0.75f, 2440, 1124, 2440, 1124},   // Apply 85% -> 75%, first frame: keep until the content fits
        {2151, 990, 2868, 1320, 0.75f, 2440, 1124, 2153, 992},     // ... then the 75% allocation
        {2438, 1122, 2868, 1320, 0.75f, 2153, 992, 2440, 1124},    // Apply 75% -> 85% seen as content first: content + pad
        {2438, 1122, 2868, 1320, 0.85f, 2440, 1124, 2440, 1124},   // ... and the 85% allocation is the same
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        mfx_t m = {0}; if (k[i].siw) { m.scaler = [NSObject new]; m.iw = k[i].siw; m.ih = k[i].sih; m.ow = k[i].ow; m.oh = k[i].oh; m.fmt = MTLPixelFormatRGBA16Float; }
        NSUInteger aw = 0, ah = 0; mfx3d_alloc(k[i].cw, k[i].ch, k[i].ow, k[i].oh, k[i].cfg, &m, &aw, &ah);
        char n[160]; snprintf(n, sizeof n, "alloc: (%lu, %lu, %lux%lu, cfg %.2f, set %lux%lu) -> %lux%lu", (unsigned long)k[i].cw, (unsigned long)k[i].ch, (unsigned long)k[i].ow, (unsigned long)k[i].oh,
                              k[i].cfg, (unsigned long)k[i].siw, (unsigned long)k[i].sih, (unsigned long)k[i].aw, (unsigned long)k[i].ah);
        EXPECT(n, 1, aw == k[i].aw && ah == k[i].ah);
    }
    // Both Apply sequences through mfx_ready on 1/8-scale sizes (output 359x165): 85% content 305x140, 75% content 269x124
    #define REQ(cw_, ch_, cfg_) do { NSUInteger aw_, ah_; mfx3d_alloc(cw_, ch_, 359, 165, cfg_, &g_mfx_3d, &aw_, &ah_); mfx_ready(&g_mfx_3d, MFX_M4, MTLPixelFormatRGBA16Float, aw_, ah_, 359, 165); } while (0)
    REQ(305, 140, 0.85f);
    int b0 = g_mfx_3d.builds, ev0 = g_ntev;
    REQ(305, 140, 0.75f); REQ(305, 140, 0.75f); REQ(269, 124, 0.75f); REQ(269, 124, 0.75f); REQ(254, 116, 0.75f);
    EXPECT("alloc: Apply 85% -> 75% (then a Frame Drops level) rebuilds once", 1, g_mfx_3d.builds - b0);
    EXPECT("alloc: ... why = alloc", 1, ev_count(ev0, "mfx who=3d op=build why=alloc iw=271 ih=126"));
    b0 = g_mfx_3d.builds; ev0 = g_ntev;
    REQ(269, 124, 0.85f); REQ(305, 140, 0.85f); REQ(305, 140, 0.85f); REQ(286, 132, 0.85f);
    EXPECT("alloc: Apply 75% -> 85% (then a Frame Drops level) rebuilds once", 1, g_mfx_3d.builds - b0);
    EXPECT("alloc: ... why = alloc", 1, ev_count(ev0, "mfx who=3d op=build why=alloc iw=307 ih=142"));
    #undef REQ
    mtl_sub_begin(); mfx_drop(&g_mfx_3d, "off"); sub_done("alloc: put the set down");   // released at the next submission's start (never fake g_done: later cases submit)
}

static int px_diff(id<MTLBuffer> a, id<MTLBuffer> b, size_t n) { int d = 0; const uint8_t *p = a.contents, *q = b.contents; for (size_t i = 0; i < n; i++) { int e = abs((int)p[i] - (int)q[i]); if (e > d) d = e; } return d; }
static void c_equiv(void) {
    id<MTLTexture> pat = mk_h4(48, 27, ^(NSUInteger x, NSUInteger y, __fp16 v[4]) { v[0] = x / 47.0f; v[1] = y / 26.0f; v[2] = x < 24 ? 0 : 1; v[3] = 1; });
    id<MTLTexture> nan = mk_h4(48, 27, ^(NSUInteger x, NSUInteger y, __fp16 v[4]) { (void)x; (void)y; uint16_t q = 0x7E00; for (int i = 0; i < 4; i++) memcpy(&v[i], &q, 2); });
    id<MTLTexture> mask = mk_r8_flat(48, 27, 255);
    tex_t dst = mk_dst(7101, 64, 36, NVN_FORMAT_RGBA8); MTLViewport vp = {0, 0, 64, 36, 0, 1};
    id<MTLBuffer> a = nil, b = nil, mo = nil;
    EXPECT("equiv: A, an exact-size set (48x27 in 48x27)", 1, enc3d(&dst, pat, 48, 27, 48, 27, mask, vp, &a, NULL));
    int b0 = g_mfx_3d.builds, ev0 = g_ntev;
    EXPECT("equiv: B, NaN content 48x27 in a 56x32 allocation", 1, enc3d(&dst, nan, 48, 27, 56, 32, mask, vp, NULL, NULL));
    EXPECT("equiv: ... builds the 56x32 set (why alloc)", 1, g_mfx_3d.builds - b0 == 1 && ev_count(ev0, "mfx who=3d op=build why=alloc iw=56 ih=32 ow=64 oh=36") == 1);
    id s0 = g_mfx_3d.scaler;
    EXPECT("equiv: B, the pattern 48x27 in the same allocation", 1, enc3d(&dst, pat, 48, 27, 56, 32, mask, vp, &b, &mo));
    EXPECT("equiv: ... same scaler", 1, g_mfx_3d.scaler == s0);
    EXPECT("equiv: ... inputContentWidth 48", 48, ((id<MTLFXSpatialScalerBase>)g_mfx_3d.scaler).inputContentWidth);
    int nonfinite = 0; const __fp16 *h = mo.contents; for (size_t i = 0; i < 64 * 36 * 4; i++) nonfinite += !isfinite((float)h[i]);
    EXPECT("equiv: no NaN / Inf in the MetalFX output", 0, nonfinite);
    int d = px_diff(a, b, 64 * 36 * 4); printf("  (largest A/B difference: %d)\n", d);
    EXPECT("equiv: A and B agree within 2 per channel", 1, d <= 2);
}
static void c_content(void) {
    id<MTLTexture> pat = mk_h4(48, 27, ^(NSUInteger x, NSUInteger y, __fp16 v[4]) { v[0] = x / 47.0f; v[1] = y / 26.0f; v[2] = 0.5f; v[3] = 1; });
    id<MTLTexture> mask = mk_r8_flat(48, 27, 255);
    tex_t dst = mk_dst(7102, 64, 36, NVN_FORMAT_RGBA8); MTLViewport vp = {0, 0, 64, 36, 0, 1};
    enc3d(&dst, pat, 48, 27, 56, 32, mask, vp, NULL, NULL);   // the set at 56x32 (built here when the case runs alone)
    int b0 = g_mfx_3d.builds, k0 = g_mfx_3d.kc, ev0 = g_ntev; id s0 = g_mfx_3d.scaler;
    EXPECT("content: 45x25 drawn", 1, enc3d(&dst, pat, 45, 25, 56, 32, mask, vp, NULL, NULL));
    EXPECT("content: 42x24 drawn", 1, enc3d(&dst, pat, 42, 24, 56, 32, mask, vp, NULL, NULL));
    EXPECT("content: same scaler", 1, g_mfx_3d.scaler == s0 && s0 != nil);
    EXPECT("content: no build", 0, g_mfx_3d.builds - b0);
    EXPECT("content: two content changes", 2, g_mfx_3d.kc - k0);
    EXPECT("content: inputContentWidth 42", 42, ((id<MTLFXSpatialScalerBase>)g_mfx_3d.scaler).inputContentWidth);
    EXPECT("content: content events k+1, k+2", 2, ev_count(ev0, "mfx who=3d op=content cw=45 ch=25 iw=56 ih=32") + ev_count(ev0, "mfx who=3d op=content cw=42 ch=24 iw=56 ih=32"));
    EXPECT("content: no build event", 0, ev_count(ev0, "mfx who=3d op=build"));
}

// The present set, built through mfx_want (it settles: MFX_SETTLE presents at the new size)
static int pres_build(NSUInteger cw, NSUInteger ch, double vw, double vh) {
    int r = 0; for (int i = 0; i < MFX_SETTLE; i++) { mfx_pres_tick(); r = mfx_want(&g_mfx_pres, MFX_M4, cw, ch, (MTLViewport){0, 0, vw, vh, 0, 1}); } return r;
}
static void c_idle(void) {
    int ev0 = g_ntev; __weak id ws, wi, wo;
    // 3D set: released through the replay's deferred release, once the submission it was put down in has run
    @autoreleasepool { mtl_sub_begin(); sub_done("idle: a submission before");
        mfx_ready(&g_mfx_3d, MFX_M4, MTLPixelFormatRGBA16Float, 48, 27, 64, 36); g_mfx_3d.last_use = g_sub_serial;
        ws = g_mfx_3d.scaler; wi = g_mfx_3d.in; wo = g_mfx_3d.out;
        g_sub_serial += MFX_IDLE_3D; mfx3d_sub_begin(); }
    EXPECT("idle 3D: after MFX_IDLE_3D submissions without a draw: still installed", 1, g_mfx_3d.scaler != nil);
    @autoreleasepool { g_sub_serial++; mfx3d_sub_begin(); }
    EXPECT("idle 3D: one more: put down", 1, g_mfx_3d.scaler == nil);
    EXPECT("idle 3D: ... still alive (frames in flight)", 1, ws != nil && wi != nil && wo != nil);
    EXPECT("idle 3D: ... an idle drop event", 1, ev_count(ev0, "mfx who=3d op=drop why=idle"));
    @autoreleasepool { mtl_sub_begin(); sub_done("idle: the submission it was put down in"); }
    @autoreleasepool { mtl_sub_begin(); sub_done("idle: the next one (deferred release drained)"); }
    EXPECT("idle 3D: released after that submission ran", 1, ws == nil);
    @autoreleasepool { res_commit(); }
    EXPECT("idle 3D: both textures released after the residency commit", 1, wi == nil && wo == nil);
    // Present set: MFX_IDLE presents unused keep it, one more puts it down, 4 more release it
    @autoreleasepool { EXPECT("idle present: built", 1, pres_build(64, 36, 96, 54)); ws = g_mfx_pres.scaler; wi = g_mfx_pres.in;
        for (int i = 0; i < MFX_IDLE; i++) mfx_pres_tick(); }
    EXPECT("idle present: after MFX_IDLE unused presents: still installed", 1, g_mfx_pres.scaler != nil);
    @autoreleasepool { mfx_pres_tick(); }
    EXPECT("idle present: one more: put down, still alive", 1, g_mfx_pres.scaler == nil && ws != nil && wi != nil);
    EXPECT("idle present: ... an idle drop event", 1, ev_count(ev0, "mfx who=pres op=drop why=idle"));
    for (int i = 0; i < 4; i++) @autoreleasepool { mfx_pres_tick(); }
    EXPECT("idle present: released 4 presents later", 1, ws == nil && wi == nil);
    // Screenshot set: the same, built directly (no settle)
    @autoreleasepool { EXPECT("idle screenshot: built", 1, mfx_ready(&g_mfx_grab, 0, MTLPixelFormatRGBA8Unorm, 64, 36, 96, 54)); ws = g_mfx_grab.scaler;
        for (int i = 0; i < MFX_IDLE; i++) mfx_pres_tick(); }
    EXPECT("idle screenshot: after MFX_IDLE presents: still installed", 1, g_mfx_grab.scaler != nil);
    @autoreleasepool { mfx_pres_tick(); }
    EXPECT("idle screenshot: one more: put down", 1, g_mfx_grab.scaler == nil && ws != nil);
    for (int i = 0; i < 4; i++) @autoreleasepool { mfx_pres_tick(); }
    EXPECT("idle screenshot: released 4 presents later", 1, ws == nil);
}

static const struct { const char *name; void (*fn)(void); } k_cases[] = {
    {"compiler", c_compiler}, {"fail", c_fail}, {"alloc", c_alloc}, {"equiv", c_equiv}, {"content", c_content}, {"idle", c_idle},
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
