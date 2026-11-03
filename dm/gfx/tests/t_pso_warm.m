// Pipeline key and warm-up unit tests (includes nvn_mtl.m; no game files): pso_key_fill hands pso_desc normalised keys (pso_key_norm), and a
// normalised key builds the same descriptor as the raw one, for two hand-made keys and the first 2,000 random keys of t_pso_skey's generator;
// where the user manifest lives (DM_PSO_USER, off, beside DM_SAVE), which manifests the warm-up reads (DM_PSO_MANIFEST off / path / default), and
// the user manifest's dedupe treating an old raw row and its normalised form as one; the warm-up plan (priority order, background marker, dedupe
// across files, a file of another layout), the warm-up end to end with every shader missing (counters end at 0, one done line, background QoS) for
// each source combination, marker position and worker count, and the two waits; host pass builds counted apart from the game's pipelines.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL "); } else printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

static NvShader *t_vsh, *t_fsh;
static blend_st pat(unsigned i) { return (blend_st){i, 5, 6, 2, 1, 1, 1}; }
static int blend_zero(const pso_key *k, int from) { blend_st z; memset(&z, 0, sizeof z); for (int i = from; i < 8; i++) if (memcmp(&k->blend[i], &z, sizeof z)) return 0; return 1; }

// t_pso_skey's random keys (xorshift64, the same byte layout); a second stream picks the fragment shader's output reflection
static uint64_t g_x = 0x9e3779b97f4a7c15ull, g_y = 0x2545f4914f6cdd1dull;
static uint64_t xs(uint64_t *x) { *x ^= *x << 13; *x ^= *x >> 7; *x ^= *x << 17; return *x; }
static void put32(uint8_t *b, size_t at, uint32_t v) { memcpy(b + at, &v, 4); }
static void put64(uint8_t *b, size_t at, uint64_t v) { memcpy(b + at, &v, 8); }
static void gen(pso_skey *sk) {
    uint8_t *b = (uint8_t *)sk; memset(b, 0, sizeof *sk);
    memcpy(b, "aaaaaaaaaaaaaaaa_b", 18);
    uint64_t r = xs(&g_x); int merged = (int)(r & 1); b[50] = (uint8_t)merged;
    for (int i = 0; i < 8; i++) put32(b, 104 + 4 * (size_t)i, (r >> (8 + i) & 1) ? 70u + (uint32_t)i : 0);
    put32(b, 148, (uint32_t)xs(&g_x)); put32(b, 152, (uint32_t)xs(&g_x));
    for (int i = 0; i < 8; i++) put64(b, 160 + 8 * (size_t)i, xs(&g_x));
    r = xs(&g_x);
    for (int i = 0; i < 8; i++) { unsigned v = (unsigned)(r >> (8 * i)) & 0xff; b[416 + i] = (uint8_t)(merged ? (v % 9 == 8 ? 0xff : v % 9) : v); }
}
// pso_desc of the raw and of the normalised key agree on every colour attachment
static int same_desc(const pso_key *raw, int merged, const char *what) {
    pso_key n = *raw; pso_key_norm(&n, merged);
    MTLRenderPipelineDescriptor *a = pso_desc(raw, t_vsh, t_fsh, nil, merged), *b = pso_desc(&n, t_vsh, t_fsh, nil, merged);
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *x = a.colorAttachments[i], *y = b.colorAttachments[i];
        if (x.pixelFormat != y.pixelFormat || x.writeMask != y.writeMask || x.blendingEnabled != y.blendingEnabled
            || x.sourceRGBBlendFactor != y.sourceRGBBlendFactor || x.destinationRGBBlendFactor != y.destinationRGBBlendFactor
            || x.sourceAlphaBlendFactor != y.sourceAlphaBlendFactor || x.destinationAlphaBlendFactor != y.destinationAlphaBlendFactor
            || x.rgbBlendOperation != y.rgbBlendOperation || x.alphaBlendOperation != y.alphaBlendOperation) {
            if (what) printf("    %s: attachment %d differs (write mask %lu / %lu, blending %d / %d)\n", what, i, (unsigned long)x.writeMask, (unsigned long)y.writeMask, x.blendingEnabled, y.blendingEnabled);
            return 0;
        }
    }
    return 1;
}

// An R row of the plan tests: vertex key aaaa…_b, one colour attachment of format f, junk blend factors on output 1 (normalised away)
static NSString *rrow(uint32_t f, unsigned junk, int pad) {
    NvShader *v = [NvShader new]; strcpy(v->key, "aaaaaaaaaaaaaaaa_b");
    pso_key k; memset(&k, 0, sizeof k); k.cfmt[0] = f; k.chmask = 0xf; k.samples = 1; k.blend[1] = (blend_st){1, junk, junk, junk, junk, 1, 1};
    pso_skey sk; char h[sizeof sk * 2 + 1]; pso_skey_from(&k, v, nil, 0, NULL, &sk);
    if (pad) ((uint8_t *)&sk)[86] = ((uint8_t *)&sk)[87] = 0xff;
    pso_skey_hex(&sk, h); return @(h);
}
static NSString *plan_str(NSArray<WarmRow *> *plan) {
    NSMutableArray *o = [NSMutableArray array];
    for (WarmRow *r in plan) [o addObject:r->tag == 'R' ? [NSString stringWithFormat:@"R%u", r->sk.k.cfmt[0]] : [NSString stringWithFormat:@"%c %@", r->tag, [r->key substringToIndex:4]]];
    return [o componentsJoinedByString:@","];
}
static void warm_reset(void) {
    g_warm_started = 0; g_warm_shaders = g_warm_rps = g_warm_cps = g_warm_skip_fz = g_warm_bad = g_warm_dup = g_warm_missing = g_warm_files = 0;
    atomic_store(&g_warm_underflow, 0); atomic_store(&g_warm_done_lines, 0); atomic_store(&g_warm_bg_qos_seen, 0);
    atomic_store(&g_warm_fg_done_ns, 0); atomic_store(&g_warm_all_done_ns, 0); g_warm_pass = 0;
}
// n distinct rows (S, R and C in turn) for a manifest body
static NSString *rows(int from, int n) {
    NSMutableString *t = [NSMutableString string];
    for (int i = from; i < from + n; i++) {
        if (i % 3 == 0) [t appendFormat:@"S\ts%015d_b\t0\n", i];
        else if (i % 3 == 1) [t appendFormat:@"R\t%@\n", rrow(100u + (uint32_t)i, 9, 0)];
        else [t appendFormat:@"C\tc%015d_b\n", i];
    }
    return t;
}
// One end-to-end warm-up with every shader missing: seed (nil = no file), user (nil = DM_PSO_USER=off), worker count; checks the counters
static void e2e(const char *name, NSString *dir, NSString *seed, NSString *user, const char *workers, int want_missing, int want_lines, int want_qos, int pass) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *sp = [dir stringByAppendingPathComponent:@"pso_manifest.tsv"], *up = [dir stringByAppendingPathComponent:@"user.tsv"];
    [fm removeItemAtPath:sp error:nil]; [fm removeItemAtPath:up error:nil];
    if (seed) [seed writeToFile:sp atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if (user) { [user writeToFile:up atomically:YES encoding:NSUTF8StringEncoding error:nil]; setenv("DM_PSO_USER", up.fileSystemRepresentation, 1); } else setenv("DM_PSO_USER", "off", 1);
    if (workers) setenv("DM_PSO_WARM_WORKERS", workers, 1); else unsetenv("DM_PSO_WARM_WORKERS");
    warm_reset(); g_warm_pass = pass; g_shmap_path = [dir stringByAppendingPathComponent:@"shader_map.tsv"];
    warm_start();
    uint64_t t0 = bp_now(); int fgw = mtl_warm_wait(10000); uint64_t t_fg = bp_now(); int allw = warm_wait_all(10000); uint64_t t_all = bp_now(); (void)fgw; (void)allw; (void)t0;
    for (int i = 0; i < 200 && atomic_load(&g_warm_done_lines) < want_lines; i++) usleep(1000);   // (the done work runs before the last release, so this never waits)
    int ok = atomic_load(&g_warm_running) == 0 && atomic_load(&g_warm_fg_left) == 0 && atomic_load(&g_warm_underflow) == 0 && atomic_load(&g_warm_done_lines) == want_lines
        && (int)g_warm_missing == want_missing && t_fg <= t_all && g_warm_dict == nil
        && (!want_lines || atomic_load(&g_warm_fg_done_ns) <= atomic_load(&g_warm_all_done_ns)) && (!want_qos || atomic_load(&g_warm_bg_qos_seen) == want_qos);
    CHECK(ok, "end to end %s: running %d, foreground left %d, underflow %d, done lines %d (want %d), missing %llu (want %d), dictionary %s, background QoS %#x (want %#x)",
          name, atomic_load(&g_warm_running), atomic_load(&g_warm_fg_left), atomic_load(&g_warm_underflow), atomic_load(&g_warm_done_lines), want_lines,
          (unsigned long long)g_warm_missing, want_missing, g_warm_dict ? "kept" : "released", atomic_load(&g_warm_bg_qos_seen), want_qos);
}

int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();
    t_vsh = [NvShader new]; t_fsh = [NvShader new]; t_fsh->stage = 1;

    // (a) Wiring: pso_key_fill normalises what it copies from the state
    memset(&S, 0, sizeof S); S.color.blend_en = 1; S.chmask.mask = 0xffffffff; S.blend[1] = (blend_st){1, 9, 9, 9, 9, 2, 2};
    const uint32_t cf1[8] = {70}, cf2[8] = {70, 70};
    pso_key k; pso_key_fill(&k, t_vsh, t_fsh, nil, cf1, 0, 0, 1, NULL);
    CHECK(k.chmask == 0xf && k.blend_en == 1 && blend_zero(&k, 1), "(a) normal pass: chmask %#x (want 0xf), blend_en %#x (want 0x1), blend[1..7] zero", k.chmask, k.blend_en);
    const uint8_t lm[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    pso_key_fill(&k, t_vsh, t_fsh, nil, cf2, 0, 0, 1, lm);
    CHECK(k.chmask == 0xf && k.lmap[1] == 0xff && k.fsfn == NULL, "(a) merged pass: chmask %#x (want 0xf), lmap[1] %#x (want 0xff), no specialised function", k.chmask, k.lmap[1]);

    // (b) Descriptor equivalence: the hand-made keys of t_pso_skey, then random keys
    pso_key r1; memset(&r1, 0, sizeof r1); r1.cfmt[0] = r1.cfmt[1] = 70; r1.blend_en = 0x5; r1.chmask = 0xffffffff;
    for (unsigned i = 0; i < 8; i++) { r1.blend[i] = pat(i); r1.lmap[i] = 3; }
    r1.vs = (__bridge void *)t_vsh; r1.fs = (__bridge void *)t_fsh;
    CHECK(same_desc(&r1, 0, "N1"), "(b) N1 (normal pass): same descriptor raw and normalised");
    pso_key r2; memset(&r2, 0, sizeof r2); r2.cfmt[0] = r2.cfmt[1] = r2.cfmt[2] = 70; r2.blend_en = 0x7; r2.chmask = 0x87654321;
    const uint8_t lm2[8] = {0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff}; memcpy(r2.lmap, lm2, 8);
    for (unsigned i = 0; i < 8; i++) r2.blend[i] = pat(i);
    r2.vs = (__bridge void *)t_vsh; r2.fs = (__bridge void *)t_fsh;
    CHECK(same_desc(&r2, 1, "N2"), "(b) N2 (merged pass): same descriptor raw and normalised");
    int same = 0, merged_n = 0;
    for (int n = 0; n < 2000; n++) {
        pso_skey sk; gen(&sk);
        uint64_t r = xs(&g_y); t_fsh->rf.color_known = (uint8_t)(r & 1); t_fsh->rf.color_mask = (uint8_t)((r >> 1) & 0xff);
        pso_key raw = sk.k; raw.vs = (__bridge void *)t_vsh; raw.fs = (__bridge void *)t_fsh;
        merged_n += sk.merged;
        same += same_desc(&raw, sk.merged, same == n ? "random key" : NULL);
    }
    t_fsh->rf.color_known = 0; t_fsh->rf.color_mask = 0;
    CHECK(same == 2000, "(b) %d of 2000 random keys (%d merged) build the same descriptor raw and normalised", same, merged_n);

    // (c) Blend factors of an output whose blending is off do not make a new key
    memset(&S, 0, sizeof S); S.color.blend_en = 1; S.chmask.mask = 0xff;
    pso_key c1, c2;
    S.blend[1] = (blend_st){1, 2, 3, 4, 5, 1, 1}; pso_key_fill(&c1, t_vsh, t_fsh, nil, cf2, 0, 0, 1, NULL);
    S.blend[1] = (blend_st){2, 9, 9, 9, 9, 2, 2}; pso_key_fill(&c2, t_vsh, t_fsh, nil, cf2, 0, 0, 1, NULL);
    CHECK(!memcmp(&c1, &c2, sizeof c1), "(c) two fills that differ only in blend[1] with its blending off give equal keys");

    // pso_user_path
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_pso_warm_%d", getpid()]];
    NSFileManager *fm = NSFileManager.defaultManager; [fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil];
    unsetenv("DM_PSO_MANIFEST");
    setenv("DM_PSO_USER", "off", 1); CHECK(pso_user_path() == nil, "DM_PSO_USER=off: no user manifest");
    setenv("DM_PSO_USER", "/tmp/x.tsv", 1); CHECK([pso_user_path() isEqualToString:@"/tmp/x.tsv"], "DM_PSO_USER=<path>: that path");
    unsetenv("DM_PSO_USER"); setenv("DM_SAVE", "/a/b/save", 1); CHECK([pso_user_path() isEqualToString:@"/a/b/pso_user.tsv"], "default: pso_user.tsv beside the save folder");
    // pso_warm_sources
    NSString *seedf = [tmp stringByAppendingPathComponent:@"pso_manifest.tsv"], *userf = [tmp stringByAppendingPathComponent:@"user.tsv"], *other = [tmp stringByAppendingPathComponent:@"other.tsv"];
    [@PSO_MANIFEST_HEAD "\n" writeToFile:seedf atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [@PSO_MANIFEST_HEAD "\n" writeToFile:userf atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [@PSO_MANIFEST_HEAD "\n" writeToFile:other atomically:YES encoding:NSUTF8StringEncoding error:nil];
    g_shmap_path = [tmp stringByAppendingPathComponent:@"shader_map.tsv"]; setenv("DM_PSO_USER", userf.fileSystemRepresentation, 1);
    NSString *sd, *us;
    setenv("DM_PSO_MANIFEST", "off", 1); pso_warm_sources(&sd, &us); CHECK(!sd && !us, "DM_PSO_MANIFEST=off: no manifest at all");
    setenv("DM_PSO_MANIFEST", other.fileSystemRepresentation, 1); pso_warm_sources(&sd, &us); CHECK([sd isEqualToString:other] && !us, "DM_PSO_MANIFEST=<path>: only that file, no user manifest");
    unsetenv("DM_PSO_MANIFEST"); pso_warm_sources(&sd, &us); CHECK([sd isEqualToString:seedf] && [us isEqualToString:userf], "default: the seed beside the shader map and the user manifest");
    [fm removeItemAtPath:seedf error:nil]; pso_warm_sources(&sd, &us); CHECK(!sd && [us isEqualToString:userf], "default without a seed file: the user manifest alone");
    // The user manifest's dedupe: an old raw row and its normalised form are one row
    {
        NvShader *v = [NvShader new]; strcpy(v->key, "aaaaaaaaaaaaaaaa_b");
        pso_key raw; memset(&raw, 0, sizeof raw); raw.cfmt[0] = 70; raw.chmask = 0xffffffff; raw.blend[3] = pat(3); raw.samples = 1;
        pso_skey a; char hraw[sizeof a * 2 + 1], hn[sizeof a * 2 + 1];
        pso_skey_from(&raw, v, nil, 0, NULL, &a); pso_skey_hex(&a, hraw);
        pso_skey_check(&a); pso_skey_hex(&a, hn);
        CHECK(strcmp(hraw, hn) != 0, "the raw row and its normalised form differ as text");
        NSString *body = [NSString stringWithFormat:@"R\t%s\n", hraw];
        [body writeToFile:userf atomically:YES encoding:NSUTF8StringEncoding error:nil];   // a headerless file from before the layout line
        g_pso_user = NULL; g_pso_user_checked = 0; g_pso_user_seen = nil;
        pso_user_record("R\t%s\n", hn);
        NSString *after = [NSString stringWithContentsOfFile:userf encoding:NSUTF8StringEncoding error:nil];
        CHECK([after isEqualToString:body], "recording the normalised form of the user file's raw row writes nothing (%lu bytes before, %lu after)", (unsigned long)body.length, (unsigned long)after.length);
        pso_user_record("S\tbbbbbbbbbbbbbbbb_b\t1\n");
        after = [NSString stringWithContentsOfFile:userf encoding:NSUTF8StringEncoding error:nil];
        CHECK([after hasSuffix:@"S\tbbbbbbbbbbbbbbbb_b\t1\n"] && [after hasPrefix:body], "a new row is appended to the old file");
        if (g_pso_user) fclose(g_pso_user);
        // an empty file gets the layout line; a file of another layout starts again
        [fm removeItemAtPath:userf error:nil]; g_pso_user = NULL; g_pso_user_checked = 0; g_pso_user_seen = nil;
        pso_user_record("S\tbbbbbbbbbbbbbbbb_b\t1\n"); fclose(g_pso_user);
        after = [NSString stringWithContentsOfFile:userf encoding:NSUTF8StringEncoding error:nil];
        CHECK([after isEqualToString:@PSO_MANIFEST_HEAD "\nS\tbbbbbbbbbbbbbbbb_b\t1\n"], "a new user manifest starts with the layout line");
        [@"# pso_skey v3 432\nS\tcccccccccccccccc_b\t1\n" writeToFile:userf atomically:YES encoding:NSUTF8StringEncoding error:nil];
        g_pso_user = NULL; g_pso_user_checked = 0; g_pso_user_seen = nil;
        pso_user_record("S\tbbbbbbbbbbbbbbbb_b\t1\n"); fclose(g_pso_user);
        after = [NSString stringWithContentsOfFile:userf encoding:NSUTF8StringEncoding error:nil];
        CHECK([after isEqualToString:@PSO_MANIFEST_HEAD "\nS\tbbbbbbbbbbbbbbbb_b\t1\n"], "a user manifest of another key layout starts again");
        g_pso_user = NULL; g_pso_user_checked = 0; g_pso_user_seen = nil;
    }
    unsetenv("DM_PSO_USER"); unsetenv("DM_SAVE"); g_shmap_path = nil;

    // warm_plan
    {
        NSString *R1 = rrow(70, 3, 0), *R1v = rrow(70, 7, 0), *R2 = rrow(80, 0, 0), *R3 = rrow(81, 0, 0), *R4 = rrow(90, 0, 0), *Rbad = rrow(70, 0, 1);
        NSString *seed = [NSString stringWithFormat:@"%s\n# c\nS\tssssssssssssssss_b\t0\nR\t%@\nR\t%@\n%s\nR\t%@\n", PSO_MANIFEST_HEAD, R1, R2, PSO_WARM_BG_MARK, R3];
        NSString *user = [NSString stringWithFormat:@"R\t%@\nR\t%@\nR\t%@\nC\t\nC\tcccccccccccccccc_b\nR\t%@\n", R1v, R4, Rbad, R3];
        warm_counts n = {0}; int fg = -1;
        NSArray<WarmRow *> *plan = warm_plan(seed, user, nil, nil, &fg, &n);
        // R3 sits in the user manifest and after the seed's marker: it builds in the foreground, and the seed's copy is the second duplicate
        CHECK([plan_str(plan) isEqualToString:@"S ssss,R70,R80,R90,C cccc,R81"] && fg == 6 && n.dup == 2 && n.bad == 2 && n.files_skipped == 0,
              "(1) plan %s, foreground %d (want S ssss,R70,R80,R90,C cccc,R81 / 6), dup %llu (want 2: R1v and the seed's R3), bad %llu (want 2), files %llu", plan_str(plan).UTF8String, fg,
              (unsigned long long)n.dup, (unsigned long long)n.bad, (unsigned long long)n.files_skipped);
        n = (warm_counts){0}; plan = warm_plan(seed, [@"# pso_skey v3 432\n" stringByAppendingString:user], nil, nil, &fg, &n);
        CHECK([plan_str(plan) isEqualToString:@"S ssss,R70,R80,R81"] && fg == 3 && n.files_skipped == 1, "(2) a user text of key layout v3 is skipped: plan %s, foreground %d, files %llu", plan_str(plan).UTF8String, fg, (unsigned long long)n.files_skipped);
        NSString *nomark = [NSString stringWithFormat:@"%s\nS\tssssssssssssssss_b\t0\nR\t%@\nR\t%@\n", PSO_MANIFEST_HEAD, R1, R2];
        n = (warm_counts){0}; plan = warm_plan(nomark, [NSString stringWithFormat:@"R\t%@\n", R4], nil, nil, &fg, &n);
        CHECK(fg == 4 && plan.count == 4, "(3) a seed without a marker: every row is foreground (%d of %lu)", fg, (unsigned long)plan.count);
        NSString *early = [NSString stringWithFormat:@"%s\n%s\nS\tssssssssssssssss_b\t0\nR\t%@\n", PSO_MANIFEST_HEAD, PSO_WARM_BG_MARK, R1];
        n = (warm_counts){0}; plan = warm_plan(early, nil, nil, nil, &fg, &n);
        CHECK(fg == 0 && plan.count == 2, "(4) the marker right after the layout line: no foreground row (%d of %lu)", fg, (unsigned long)plan.count);
        NSString *late = [NSString stringWithFormat:@"%s\nS\tssssssssssssssss_b\t0\nR\t%@\n%s\n", PSO_MANIFEST_HEAD, R1, PSO_WARM_BG_MARK];
        n = (warm_counts){0}; plan = warm_plan(late, nil, nil, nil, &fg, &n);
        CHECK(fg == 2 && plan.count == 2, "(5) the marker as the last line: every row is foreground (%d of %lu)", fg, (unsigned long)plan.count);
    }

    // The warm-up end to end, every shader missing (an empty library folder)
    {
        NSString *msl = [tmp stringByAppendingPathComponent:@"msl"]; [fm createDirectoryAtPath:msl withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *keep_msl = g_msl_dir; g_msl_dir = msl;
        NSString *H = @PSO_MANIFEST_HEAD "\n", *M = @PSO_WARM_BG_MARK "\n";
        NSString *seed5 = [NSString stringWithFormat:@"%@%@%@%@", H, rows(0, 3), M, rows(3, 2)];
        NSString *user4 = rows(10, 4);
        e2e("(a) seed only, marker after 3 rows", tmp, seed5, nil, NULL, 5, 1, QOS_CLASS_UTILITY, 0);
        e2e("(a) the same in the launcher's pass", tmp, seed5, nil, NULL, 5, 1, QOS_CLASS_USER_INITIATED, 1);
        e2e("(b) user only", tmp, nil, user4, NULL, 4, 1, 0, 0);
        e2e("(c) both", tmp, seed5, user4, NULL, 9, 1, QOS_CLASS_UTILITY, 0);
        e2e("(d) neither", tmp, nil, nil, NULL, 0, 0, 0, 0);
        CHECK(mtl_warm_wait(1000) == 0 && warm_wait_all(1000) == 0, "(d) nothing started: both waits return 0 at once");
        e2e("(e) marker on line 2", tmp, [NSString stringWithFormat:@"%@%@%@", H, M, rows(0, 3)], nil, NULL, 3, 1, QOS_CLASS_UTILITY, 0);
        e2e("(f) marker as the last line", tmp, [NSString stringWithFormat:@"%@%@%@", H, rows(0, 3), M], nil, NULL, 3, 1, 0, 0);
        setenv("DM_PSO_MANIFEST", "off", 1);
        e2e("(g) DM_PSO_MANIFEST=off", tmp, seed5, user4, NULL, 0, 0, 0, 0);
        unsetenv("DM_PSO_MANIFEST");
        e2e("(h) both, 1 worker", tmp, seed5, user4, "1", 9, 1, QOS_CLASS_UTILITY, 0);
        e2e("(h) both, 4 workers", tmp, seed5, user4, "4", 9, 1, QOS_CLASS_UTILITY, 0);
        e2e("(h) both, 16 workers asked (2 used)", tmp, seed5, user4, "16", 9, 1, QOS_CLASS_UTILITY, 0);
        CHECK(g_warm_workers == 2, "an out-of-range DM_PSO_WARM_WORKERS gives 2 workers (%d)", g_warm_workers);
        g_msl_dir = keep_msl; unsetenv("DM_PSO_USER"); unsetenv("DM_PSO_WARM_WORKERS"); g_shmap_path = nil;
    }

    // Host pass builds are counted apart from the game's shaders and pipelines
    {
        uint64_t l0, p0, l1, p1, hl0, hp0, hln0, hpn0, hl1, hp1, hln1, hpn1, hl2, hp2, hln2, hpn2;
        mtl_create_stats(&l0, &p0); mtl_host_create_stats(&hl0, &hp0, &hln0, &hpn0);
        host_built(1, bp_now() - 5000000, "test");
        mtl_create_stats(&l1, &p1); mtl_host_create_stats(&hl1, &hp1, &hln1, &hpn1);
        CHECK(hp1 - hp0 == 1 && hpn1 - hpn0 >= 5000000 && hl1 == hl0 && l1 == l0 && p1 == p0, "a host pipeline build: host pipelines +%llu (%.1f ms), host libraries +%llu, game counters +%llu / +%llu",
              (unsigned long long)(hp1 - hp0), (hpn1 - hpn0) / 1e6, (unsigned long long)(hl1 - hl0), (unsigned long long)(l1 - l0), (unsigned long long)(p1 - p0));
        host_built(0, bp_now(), "test");
        mtl_host_create_stats(&hl2, &hp2, &hln2, &hpn2);
        CHECK(hl2 - hl1 == 1 && hp2 == hp1, "a host library build: host libraries +%llu", (unsigned long long)(hl2 - hl1));
    }

    // The two waits, counters set directly
    atomic_store(&g_warm_running, 1); atomic_store(&g_warm_fg_left, 0);
    CHECK(mtl_warm_wait(50) == 0, "background rows still building, foreground done: the loading screen does not wait");
    atomic_store(&g_warm_fg_left, 1);
    { int w = mtl_warm_wait(50); CHECK(w >= 50, "foreground rows pending: the loading screen waits (%d ms)", w); }
    { int w = warm_wait_all(50); CHECK(w >= 50, "workers live: the launcher's pass waits (%d ms)", w); }
    atomic_store(&g_warm_running, 0); atomic_store(&g_warm_fg_left, 0);
    [fm removeItemAtPath:tmp error:nil];

    printf("== t_pso_warm %d checks, %d failed ==\n", checks, fails);
    return fails != 0; } }
