// Serializable pipeline key unit tests (header only, no Metal): the pso_skey hex round trip is byte-identical and tampering / wrong length / illegal
// characters are detected; pso_key_norm clears exactly the state pso_desc never reads (N1-N5, expectations given by construction), is idempotent, and
// agrees byte for byte with dm/tools/pso_seed.py norm_row through a hash over 5,000 random keys (PSO_NORM_FNV, also asserted by test_pso_seed.py).
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

    printf("== t_pso_skey %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
