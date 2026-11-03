// Serializable pipeline key unit tests (header only, no Metal): the pso_skey hex round trip is byte-identical and tampering / wrong length / illegal
// characters are detected; pso_key_norm clears exactly the state pso_desc never reads (N1-N5, expectations given by construction), is idempotent, and
// agrees byte for byte with dm/tools/pso_seed.py norm_row through a hash over 5,000 random keys (PSO_NORM_FNV, also asserted by test_pso_seed.py);
// pso_skey_check refuses each kind of bad row (1-10); the lint passes the shipped seed (dm/gfx/nvn/pso_manifest.tsv against shader_map.tsv) and
// finds exactly the 6 problems of a synthetic bad manifest.
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdlib.h>
#include "../nvn/nvn_obj.h"
@interface NvShader : NSObject { @public int stage; char key[24]; } @end
@implementation NvShader @end
#include "../nvn/nvn_pso_skey.h"

// FNV-1a 64 over the 5,000 normalised keys of the shared generator below; test_pso_seed.py checks the Python port against the same value
#define PSO_NORM_FNV 0xe615b40ab55121b6ull

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL "); } else printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

static void fill(pso_key *k, unsigned seed) {
    memset(k, 0, sizeof *k);
    for (size_t i = 0; i < sizeof *k; i++) ((unsigned char *)k)[i] = (unsigned char)(seed + i * 13);
    k->vs = (void *)1; k->fs = (void *)2; k->fsfn = (void *)3;
}
static blend_st pat(unsigned i) { return (blend_st){i, 5, 6, 2, 1, 1, 1}; }
static int blend_eq(blend_st a, blend_st b) { return !memcmp(&a, &b, sizeof a); }
static int blend_zero(const pso_key *k, int from) { blend_st z; memset(&z, 0, sizeof z); for (int i = from; i < 8; i++) if (!blend_eq(k->blend[i], z)) return 0; return 1; }
static int lmap_zero(const pso_key *k) { for (int i = 0; i < 8; i++) if (k->lmap[i]) return 0; return 1; }
static pso_key n1(void) {   // normal pass: attachments 0 and 1, blending asked on outputs 0 and 2, every write mask on
    pso_key k; memset(&k, 0, sizeof k);
    k.cfmt[0] = k.cfmt[1] = 70; k.blend_en = 0x5; k.chmask = 0xffffffff;
    for (unsigned i = 0; i < 8; i++) { k.blend[i] = pat(i); k.lmap[i] = 3; }
    return k;
}
static pso_key n2(void) {   // merged pass: physical 0 -> output 0, physical 1 unmapped, physical 2 -> output 1
    pso_key k; memset(&k, 0, sizeof k);
    k.cfmt[0] = k.cfmt[1] = k.cfmt[2] = 70; k.blend_en = 0x7; k.chmask = 0x87654321;
    const uint8_t lm[8] = {0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff}; memcpy(k.lmap, lm, 8);
    for (unsigned i = 0; i < 8; i++) k.blend[i] = pat(i);
    return k;
}
// The shared random keys: xorshift64, one 432-byte row per key, built byte-wise exactly as test_pso_seed.py does
static uint64_t g_x = 0x9e3779b97f4a7c15ull;
static uint64_t xs(void) { g_x ^= g_x << 13; g_x ^= g_x >> 7; g_x ^= g_x << 17; return g_x; }
static void put32(uint8_t *b, size_t at, uint32_t v) { memcpy(b + at, &v, 4); }
static void put64(uint8_t *b, size_t at, uint64_t v) { memcpy(b + at, &v, 8); }
static void gen(pso_skey *sk) {
    uint8_t *b = (uint8_t *)sk; memset(b, 0, sizeof *sk);
    memcpy(b, "aaaaaaaaaaaaaaaa_b", 18);
    uint64_t r = xs(); int merged = (int)(r & 1); b[50] = (uint8_t)merged;
    for (int i = 0; i < 8; i++) put32(b, 104 + 4 * (size_t)i, (r >> (8 + i) & 1) ? 70u + (uint32_t)i : 0);
    put32(b, 148, (uint32_t)xs()); put32(b, 152, (uint32_t)xs());
    for (int i = 0; i < 8; i++) put64(b, 160 + 8 * (size_t)i, xs());
    r = xs();
    for (int i = 0; i < 8; i++) { unsigned v = (unsigned)(r >> (8 * i)) & 0xff; b[416 + i] = (uint8_t)(merged ? (v % 9 == 8 ? 0xff : v % 9) : v); }
}

// The shader map: key base (16 hex digits) -> stage number (shader_map.tsv columns 4 and 3)
static NSDictionary<NSString *, NSNumber *> *stage_map(const char *map) {
    NSString *t = [NSString stringWithContentsOfFile:@(map) encoding:NSUTF8StringEncoding error:nil];
    NSDictionary *names = @{@"顶点": @0, @"像素": @1, @"外壳": @3, @"域": @4, @"计算": @5};
    NSMutableDictionary *m = [NSMutableDictionary dictionary];
    for (NSString *ln in [t componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count > 3 && names[c[2]]) m[c[3]] = names[c[2]];
    }
    return m;
}
// A key of the map with the given stage: <16 hex>_b
static int key_ok(NSDictionary<NSString *, NSNumber *> *m, NSString *key, int stage) {
    if (key.length != 18 || ![key hasSuffix:@"_b"]) return 0;
    NSNumber *st = m[[key substringToIndex:16]];
    return st && st.intValue == stage;
}
// Lints a manifest: the layout line first, at most one background marker, rows valid and canonical with every shader key in the map at its
// stage, no repeated line. At most one problem per line (the first failed check). Returns the number of problems
static int lint(const char *manifest, const char *map, int quiet) {
    NSDictionary<NSString *, NSNumber *> *m = stage_map(map);
    NSString *t = [NSString stringWithContentsOfFile:@(manifest) encoding:NSUTF8StringEncoding error:nil];
    if (!t || !m.count) { printf("  lint: cannot read %s or %s\n", manifest, map); return 1; }
    NSArray<NSString *> *lines = [t componentsSeparatedByString:@"\n"];
    int bad = 0, marks = 0, ns = 0, nr = 0, nc = 0; NSMutableSet *seen = [NSMutableSet set];
#define PROBLEM(...) do { bad++; if (!quiet) { printf("    line %lu: ", (unsigned long)i + 1); printf(__VA_ARGS__); printf("\n"); } } while (0)
    if (![lines.firstObject isEqualToString:@PSO_MANIFEST_HEAD]) { bad++; if (!quiet) printf("    line 1: not the layout line %s\n", PSO_MANIFEST_HEAD); }
    for (NSUInteger i = 0; i < lines.count; i++) {
        NSString *ln = lines[i];
        if (!ln.length) continue;
        if ([ln hasPrefix:@"#"]) { if ([ln isEqualToString:@PSO_WARM_BG_MARK] && ++marks > 1) PROBLEM("a second background marker"); continue; }
        if ([seen containsObject:ln]) { PROBLEM("repeated line"); continue; }
        [seen addObject:ln];
        NSArray<NSString *> *c = [ln componentsSeparatedByString:@"\t"];
        if ([c[0] isEqualToString:@"S"]) {
            if (c.count != 3) PROBLEM("S row without 3 fields");
            else if (!key_ok(m, c[1], c[2].intValue)) PROBLEM("S %s: not a stage-%d key of the shader map", c[1].UTF8String, c[2].intValue);
            else ns++;
        } else if ([c[0] isEqualToString:@"C"]) {
            if (c.count < 2 || !c[1].length) PROBLEM("C row without a key");
            else if (!key_ok(m, c[1], 5)) PROBLEM("C %s: not a compute key of the shader map", c[1].UTF8String);
            else nc++;
        } else if ([c[0] isEqualToString:@"R"]) {
            pso_skey sk, raw; int r = -1;
            if (c.count < 2 || pso_skey_parse(c[1].UTF8String, &sk)) { PROBLEM("R row does not parse"); continue; }
            raw = sk;
            if ((r = pso_skey_check(&sk))) { PROBLEM("R row refused by pso_skey_check (%d)", r); continue; }
            if (memcmp(&raw, &sk, sizeof sk)) { PROBLEM("R row not normalised"); continue; }
            NSString *vk = [[NSString alloc] initWithBytes:sk.vs length:strlen(sk.vs) encoding:NSUTF8StringEncoding];
            NSString *fk = [[NSString alloc] initWithBytes:sk.fs length:strlen(sk.fs) encoding:NSUTF8StringEncoding];
            if (!key_ok(m, vk, sk.vs_stage)) PROBLEM("R vertex %s: not a stage-%d key of the shader map", vk.UTF8String, sk.vs_stage);
            else if (fk.length && !key_ok(m, fk, sk.fs_stage)) PROBLEM("R fragment %s: not a stage-%d key of the shader map", fk.UTF8String, sk.fs_stage);
            else nr++;
        } else PROBLEM("unknown row tag %s", c[0].UTF8String);
    }
#undef PROBLEM
    if (!quiet) { if (bad) printf("  PSO_SEED RED %d problems in %s\n", bad, manifest); else printf("  PSO_SEED OK S=%d R=%d C=%d\n", ns, nr, nc); }
    return bad;
}

int main(void) {
    for (unsigned seed = 1; seed <= 3; seed++) {
        pso_key k; fill(&k, seed);
        NvShader *vs = [NvShader new], *fs = [NvShader new];
        vs->stage = (int)seed; fs->stage = (int)(seed + 1);
        snprintf(vs->key, sizeof vs->key, "vs-%u", seed); snprintf(fs->key, sizeof fs->key, "fs-%u", seed);
        pso_fz_desc fz = {.kind = 1, .fbmask = 0xa5};
        for (int i = 0; i < 32; i++) fz.fx[i] = (int8_t)(i - 7);
        pso_skey a, b; char hex[sizeof a * 2 + 1];
        pso_skey_from(&k, vs, fs, seed & 1, &fz, &a); pso_skey_hex(&a, hex);
        assert(pso_skey_parse(hex, &b) == 0); assert(memcmp(&a, &b, sizeof a) == 0);
        hex[0] = hex[0] == '0' ? '1' : '0'; assert(pso_skey_parse(hex, &b) == 0); assert(memcmp(&a, &b, sizeof a) != 0);
        assert(pso_skey_parse(hex + 1, &b) == -1); hex[0] = 'g'; assert(pso_skey_parse(hex, &b) == -1);
    }
    puts("PSO_SKEY OK 3 例往返一致；篡改一字符不等、缺位 / 非法字符拒收");

    // N1: normal pass
    pso_key k = n1(); pso_key_norm(&k, 0);
    CHECK(k.blend_en == 0x1 && k.chmask == 0x000000ff && blend_eq(k.blend[0], pat(0)) && blend_zero(&k, 1) && lmap_zero(&k),
          "N1 normal pass: blend_en %#x (want 0x1), chmask %#x (want 0xff), blend[0] kept, blend[1..7] and lmap cleared", k.blend_en, k.chmask);
    pso_key once = k; pso_key_norm(&k, 0);
    CHECK(!memcmp(&k, &once, sizeof k), "N3 normal pass: normalising twice gives the same bytes");
    // N2: merged pass
    k = n2(); pso_key_norm(&k, 1);
    const uint8_t lm[8] = {0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(k.blend_en == 0x3 && k.chmask == 0x00000021 && blend_eq(k.blend[0], pat(0)) && blend_eq(k.blend[1], pat(1)) && blend_zero(&k, 2) && !memcmp(k.lmap, lm, 8),
          "N2 merged pass: blend_en %#x (want 0x3), chmask %#x (want 0x21), blend[0..1] kept, blend[2..7] cleared, lmap unchanged", k.blend_en, k.chmask);
    once = k; pso_key_norm(&k, 1);
    CHECK(!memcmp(&k, &once, sizeof k), "N3 merged pass: normalising twice gives the same bytes");
    // N4: the SMAA upsample case, two keys that differ only in the blend factors of an output whose blending is off
    pso_key a = n1(), b = n1(); a.blend_en = b.blend_en = 0x1; b.blend[1] = (blend_st){1, 9, 9, 9, 9, 2, 2};
    CHECK(memcmp(&a, &b, sizeof a) != 0, "N4 the raw keys differ");
    pso_key_norm(&a, 0); pso_key_norm(&b, 0);
    CHECK(!memcmp(&a, &b, sizeof a), "N4 equal after normalising");
    // N5: the write mask of an output with no attachment
    k = n1(); k.cfmt[3] = 0; k.chmask = 0x0000f0ff; pso_key_norm(&k, 0);
    CHECK((k.chmask >> 12 & 0xf) == 0, "N5 nibble 3 without an attachment is cleared (chmask %#x)", k.chmask);

    // P: random keys, idempotence and the hash shared with the Python port
    uint64_t h = 0xcbf29ce484222325ull; int idem = 0;
    for (int n = 0; n < 5000; n++) {
        pso_skey sk; gen(&sk);
        pso_key_norm(&sk.k, sk.merged);
        pso_skey again = sk; pso_key_norm(&again.k, again.merged); idem += !memcmp(&sk, &again, sizeof sk);
        const uint8_t *p = (const uint8_t *)&sk;
        for (size_t i = 0; i < sizeof sk; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    }
    CHECK(idem == 5000, "P idempotent on %d of 5000 random keys", idem);
    CHECK(h == PSO_NORM_FNV, "P hash of the 5000 normalised random keys %#llx (want %#llx, the Python port's)", (unsigned long long)h, (unsigned long long)PSO_NORM_FNV);

    // pso_skey_check (1-10)
    NvShader *gv = [NvShader new], *gf = [NvShader new]; gv->stage = 0; gf->stage = 1; strcpy(gv->key, "vvvvvvvvvvvvvvvv_b"); strcpy(gf->key, "ffffffffffffffff_b");
    pso_key gk = n1(); gk.vs = (__bridge void *)gv; gk.fs = (__bridge void *)gf; pso_key_norm(&gk, 0);
    pso_skey good, t; pso_skey_from(&gk, gv, gf, 0, NULL, &good);
    t = good; CHECK(pso_skey_check(&t) == PSO_SKEY_OK && !memcmp(&t, &good, sizeof t), "(1) a row made by pso_skey_from from a normalised key passes and is unchanged");
    t = good; ((uint8_t *)&t)[86] = 0xff; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_PAD, "(2) padding byte 86 = 0xff: -2");
    t = good; t.k.vs = (void *)1; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_PTR, "(3) k.vs set: -3");
    t = good; t.merged = 2; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_FLAGS, "(4) merged = 2: -4");
    t = good; t.fz = 1; t.merged = 0; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_FZ, "(5) fz without merged: -5");
    t = good; t.fz = 1; t.merged = 1; t.fz_desc.kind = 2; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_KIND, "(6) fz with kind 2: -6");
    t = good; t.vs[0] = 0; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_KEY, "(7) empty vertex key: -7");
    t = good; t.merged = 1; t.k.lmap[0] = 9; CHECK(pso_skey_check(&t) == PSO_SKEY_BAD_LMAP, "(8) merged with lmap[0] = 9: -8");
    t = good; t.k.lmap[0] = 9; CHECK(pso_skey_check(&t) == PSO_SKEY_OK && !t.k.lmap[0], "(8) unmerged with lmap[0] = 9: accepted, lmap cleared");
    t = good; t.fz_desc.kind = 1; t.fz_desc.fx[0] = 3; { pso_fz_desc z; memset(&z, 0, sizeof z);
        CHECK(pso_skey_check(&t) == PSO_SKEY_OK && !memcmp(&t.fz_desc, &z, sizeof z), "(9) an unspecialised row's stray fz_desc is cleared"); }
    { pso_fz_desc fz = {.kind = 1, .fbmask = 3}; pso_fz_desc z; memset(&z, 0, sizeof z); pso_skey_from(&gk, gv, gf, 1, &fz, &t);
      CHECK(!t.fz && !memcmp(&t.fz_desc, &z, sizeof z), "(10) pso_skey_from without a specialised function keeps no fz_desc"); }

    // (11) the shipped seed, (12) a synthetic bad manifest
    int seed_bad = lint("../nvn/pso_manifest.tsv", "../nvn/shader_map.tsv", 0);
    CHECK(seed_bad == 0, "(11) the shipped seed lints clean (%d problems)", seed_bad);
    {
        NSDictionary<NSString *, NSNumber *> *m = stage_map("../nvn/shader_map.tsv"); NSString *vkey = nil;
        NSString *mt = [NSString stringWithContentsOfFile:@"../nvn/shader_map.tsv" encoding:NSUTF8StringEncoding error:nil];
        for (NSString *ln in [mt componentsSeparatedByString:@"\n"]) { NSArray<NSString *> *c = [ln componentsSeparatedByString:@"\t"]; if (c.count > 3 && [c[2] isEqualToString:@"顶点"]) { vkey = [c[3] stringByAppendingString:@"_b"]; break; } }
        (void)m;
        NvShader *v = [NvShader new]; v->stage = 0; strlcpy(v->key, vkey.UTF8String, sizeof v->key);
        pso_key pk; memset(&pk, 0, sizeof pk); pk.cfmt[0] = 70; pk.chmask = 0xf; pk.samples = 1;
        pso_skey a1, a2; char h1[sizeof a1 * 2 + 1], h2[sizeof a2 * 2 + 1];
        pso_skey_from(&pk, v, nil, 0, NULL, &a1); ((uint8_t *)&a1)[86] = ((uint8_t *)&a1)[87] = 0xff; pso_skey_hex(&a1, h1);
        pk.blend[1] = pat(1); pso_skey_from(&pk, v, nil, 0, NULL, &a2); pso_skey_hex(&a2, h2);
        NSString *bad = [NSString stringWithFormat:@"S\t%@\t0\nS\t0000000000000000_b\t0\nR\t%s\nR\t%s\nC\t\nS\t%@\t0\n", vkey, h1, h2, vkey];
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_pso_skey_%d.tsv", getpid()]];
        [bad writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
        int n = lint(path.fileSystemRepresentation, "../nvn/shader_map.tsv", 1);   // quiet: its problems are the expected ones
        CHECK(n == 6, "(12) negative lint: %d problems (want 6: no layout line, unknown S key, padded R, unnormalised R, empty C, repeated line)", n);
        if (n == 6) printf("  negative lint: 6 problems as expected\n");
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }

    printf("== t_pso_skey %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
