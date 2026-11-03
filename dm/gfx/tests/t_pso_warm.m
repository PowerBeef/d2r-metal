// Pipeline key and warm-up unit tests (includes nvn_mtl.m; no game files): pso_key_fill hands pso_desc normalised keys (pso_key_norm), and a
// normalised key builds the same descriptor as the raw one, for two hand-made keys and the first 2,000 random keys of t_pso_skey's generator;
// where the user manifest lives (DM_PSO_USER, off, beside DM_SAVE), which manifests the warm-up reads (DM_PSO_MANIFEST off / path / default), and
// the user manifest's dedupe treating an old raw row and its normalised form as one.
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
    [fm removeItemAtPath:tmp error:nil];

    printf("== t_pso_warm %d checks, %d failed ==\n", checks, fails);
    return fails != 0; } }
