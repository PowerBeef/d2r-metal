// Serializable pipeline key (the R rows of pso_manifest.tsv / pso_user.tsv): pso_key holds the pipeline state that pso_desc (nvn_mtl.m) reads,
// pso_skey adds the shader keys and stages, the merged-pass flag and the specialisation source, and is written as 864 hex digits.
// pso_key_norm clears state pso_desc never reads; pso_skey_check refuses rows of another layout or with impossible fields and normalises the rest.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nvn_obj.h"

typedef struct {
    const void *vs, *fs; uint32_t cfmt[8], dfmt, sfmt, samples, blend_en, chmask, a2c; blend_st blend[8];
    va_st attr[16]; vs_st stream[16];
    uint8_t lmap[8]; const void *fsfn;
} pso_key;

typedef struct {
    uint8_t kind, fbmask;
    int8_t fx[32];
} pso_fz_desc;

typedef struct { char vs[24], fs[24]; uint8_t vs_stage, fs_stage, merged, fz; pso_fz_desc fz_desc; pso_key k; } pso_skey;
_Static_assert(sizeof(pso_key) == 344 && offsetof(pso_skey, k) == 88 && sizeof(pso_skey) == 432, "manifest rows are 432-byte pso_skey");
// A manifest's first line names the key layout. Files without it are layout v2 (they predate the line); a future change of pso_skey bumps the
// version here, and must then treat headerless files as the old layout. Rows after PSO_WARM_BG_MARK in the seed warm in the background: the first
// loading screen waits only for the rows before it (and the player's own manifest)
#define PSO_MANIFEST_HEAD "# pso_skey v2 432"
#define PSO_WARM_BG_MARK "# warm: background"
enum { PSO_SKEY_OK = 0, PSO_SKEY_BAD_PAD = -2, PSO_SKEY_BAD_PTR = -3, PSO_SKEY_BAD_FLAGS = -4, PSO_SKEY_BAD_FZ = -5, PSO_SKEY_BAD_KIND = -6, PSO_SKEY_BAD_KEY = -7, PSO_SKEY_BAD_LMAP = -8 };

// Clears the key fields pso_desc (nvn_mtl.m) never reads, so keys that build the same pipeline are equal: the write mask and the blend state of
// an output with no attachment in a normal pass, or with no physical attachment mapped to it in a merged pass, and the blend factors of an output
// whose blending is off; a normal pass also gets an all-zero lmap. lmap entries are 0..7 or 0xff (pso_skey_check refuses others in merged rows).
// Idempotent. pso_desc and pso_key_norm must change together, and so must norm_row in dm/tools/pso_seed.py (t_pso_skey and test_pso_seed.py share
// a hash over the same random keys).
static inline void pso_key_norm(pso_key *k, int merged) {
    uint32_t en = 0, ch = 0; blend_st bl[8]; memset(bl, 0, sizeof bl);
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        unsigned l = merged ? k->lmap[i] : (unsigned)i;
        if (l > 7) continue;
        ch |= k->chmask & (0xfu << (4 * l));
        if (k->blend_en >> l & 1) { en |= 1u << l; bl[l] = k->blend[l]; }
    }
    k->blend_en = en; k->chmask = ch; memcpy(k->blend, bl, sizeof bl);
    if (!merged) memset(k->lmap, 0, sizeof k->lmap);
}

static inline void pso_skey_from(const pso_key *k, NvShader *vs, NvShader *fs, int merged, const pso_fz_desc *fz, pso_skey *out) {
    memset(out, 0, sizeof *out);
    memcpy(&out->k, k, sizeof out->k);
    out->k.vs = out->k.fs = out->k.fsfn = NULL;
    if (vs) { memcpy(out->vs, vs->key, sizeof out->vs); out->vs_stage = (uint8_t)vs->stage; }
    if (fs) { memcpy(out->fs, fs->key, sizeof out->fs); out->fs_stage = (uint8_t)fs->stage; }
    out->merged = merged != 0;
    out->fz = k->fsfn != NULL;
    if (fz && k->fsfn) memcpy(&out->fz_desc, fz, sizeof out->fz_desc);   // only a specialised row carries its source (an unspecialised merged draw may still hold the last one)
}

// Checks a parsed row (in order: padding, pointers, flags, fz without merged, fz kind, shader keys, lmap of a merged row) and returns the first
// failure. A row that passes is made canonical: the shader keys are zero after their NUL, an unspecialised row has no fz_desc, and the key is
// normalised (pso_key_norm). Merged rows' lmap entries must be 0..7 or 0xff, which is what pso_desc can index
static inline int pso_skey_check(pso_skey *sk) {
    const uint8_t *p = (const uint8_t *)sk;
    for (size_t i = offsetof(pso_skey, fz_desc) + sizeof sk->fz_desc; i < offsetof(pso_skey, k); i++) if (p[i]) return PSO_SKEY_BAD_PAD;
    if (sk->k.vs || sk->k.fs || sk->k.fsfn) return PSO_SKEY_BAD_PTR;
    if (sk->merged > 1 || sk->fz > 1) return PSO_SKEY_BAD_FLAGS;
    if (sk->fz && !sk->merged) return PSO_SKEY_BAD_FZ;
    if (sk->fz && sk->fz_desc.kind != 1) return PSO_SKEY_BAD_KIND;
    if (!sk->vs[0] || !memchr(sk->vs, 0, sizeof sk->vs) || !memchr(sk->fs, 0, sizeof sk->fs)) return PSO_SKEY_BAD_KEY;
    if (sk->merged) for (int i = 0; i < 8; i++) if (sk->k.lmap[i] > 7 && sk->k.lmap[i] != 0xff) return PSO_SKEY_BAD_LMAP;
    size_t n = strlen(sk->vs); memset(sk->vs + n, 0, sizeof sk->vs - n);
    n = strlen(sk->fs); memset(sk->fs + n, 0, sizeof sk->fs - n);
    if (!sk->fz) memset(&sk->fz_desc, 0, sizeof sk->fz_desc);
    pso_key_norm(&sk->k, sk->merged);
    return PSO_SKEY_OK;
}

static inline void pso_skey_hex(const pso_skey *key, char *out) {
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof *key; i++) { out[i * 2] = hex[p[i] >> 4]; out[i * 2 + 1] = hex[p[i] & 15]; }
    out[sizeof *key * 2] = 0;
}

static inline int pso_skey_parse(const char *hex, pso_skey *out) {
    size_t n = sizeof *out * 2;
    if (!hex || strlen(hex) != n) return -1;
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; i += 2) {
        int a = hex[i] >= '0' && hex[i] <= '9' ? hex[i] - '0' : hex[i] >= 'a' && hex[i] <= 'f' ? hex[i] - 'a' + 10 : hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] - 'A' + 10 : -1;
        int b = hex[i + 1] >= '0' && hex[i + 1] <= '9' ? hex[i + 1] - '0' : hex[i + 1] >= 'a' && hex[i + 1] <= 'f' ? hex[i + 1] - 'a' + 10 : hex[i + 1] >= 'A' && hex[i + 1] <= 'F' ? hex[i + 1] - 'A' + 10 : -1;
        if (a < 0 || b < 0) return -1;
        p[i / 2] = (uint8_t)(a << 4 | b);
    }
    return 0;
}
