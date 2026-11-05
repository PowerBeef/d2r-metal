// NVN frame capture, CPU side (nvn_capture.c): every record op the walk knows encodes its pointers into region offsets and object indices and decodes back to
// the same values, an unknown op is refused, and a capture file round-trips its chunks (raw and LZ4) and rejects a corrupt one. No GPU, no game data.
// Adapted from nxnative's t_capture.c (github.com/seedds/nxnative, MIT, (c) 2026 seedds), rewritten for d2r-metal's record set.
#include "../nvn/nvn_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", why, __LINE__); } } while (0)

// One pool region at base, objects by pointer value: the encoder maps an address inside [base, base + size) to region 0 and any non-null object pointer to its
// value (index = pointer low bits); the decoder reverses both. Counts what each walk touched
static uint8_t g_pool[4096]; static uint64_t g_naddr, g_nobj, g_kinds[CAP_K_N];
static int enc_addr(void *ctx, uint64_t *f, uint64_t len, int w) {
    (void)ctx; (void)len; (void)w; g_naddr++;
    uint64_t base = (uint64_t)(uintptr_t)g_pool;
    if (*f >= base && *f < base + sizeof g_pool) *f = cap_addr(0, *f - base);
    return 1;
}
static int enc_obj(void *ctx, int kind, uint64_t *f) { (void)ctx; g_nobj++; if (kind > 0 && kind < CAP_K_N) g_kinds[kind]++; if (*f) *f = cap_obj(kind, *f & 0xffff); return 1; }
static int dec_addr(void *ctx, uint64_t *f, uint64_t len, int w) {
    (void)ctx; (void)len; (void)w;
    if (cap_is_addr(*f)) { if (cap_addr_region(*f) != 0) return 0; *f = (uint64_t)(uintptr_t)g_pool + cap_addr_off(*f); }
    return 1;
}
static int dec_obj(void *ctx, int kind, uint64_t *f) { (void)ctx; if (!*f) return 1; if (cap_obj_kind(*f) != kind) return 0; *f = 0x100000000ull | cap_obj_index(*f); return 1; }
static const cap_ops ENC = {enc_addr, enc_obj}, DEC = {dec_addr, dec_obj};

// A record: header + payload of the given size, payload copied from p
static uint8_t g_rec[512];
static uint32_t rec(uint32_t op, const void *p, uint32_t n) { op_hdr h = {op, (8 + n + 7) & ~7u}; memset(g_rec, 0, sizeof g_rec); memcpy(g_rec, &h, 8); memcpy(g_rec + 8, p, n); return h.size; }
// Encode, check every pointer field left the host range, decode, compare with the original
static void roundtrip(const char *name, uint32_t op, const void *p, uint32_t n, int want_addr, int want_obj) {
    uint32_t size = rec(op, p, n); uint8_t orig[512]; memcpy(orig, g_rec, size);
    g_naddr = g_nobj = 0;
    int ok = cap_rec_fields(op, g_rec + 8, size, &ENC, NULL);
    char why[160]; snprintf(why, sizeof why, "%s: the walk knows it", name); CHECK(ok, why);
    snprintf(why, sizeof why, "%s: %d address and %d object fields", name, want_addr, want_obj); CHECK((int)g_naddr == want_addr && (int)g_nobj == want_obj, why);
    uint8_t enc[512]; memcpy(enc, g_rec, size);
    ok = cap_rec_fields(op, g_rec + 8, size, &DEC, NULL);
    snprintf(why, sizeof why, "%s: decodes", name); CHECK(ok, why);
    // addresses come back exactly; object pointers come back as 0x1_0000_xxxx with the same low 16 bits (the test's object table)
    int same = 1;
    for (uint32_t o = 8; o + 8 <= size; o += 8) {
        uint64_t a, b, e; memcpy(&a, orig + o, 8); memcpy(&b, g_rec + o, 8); memcpy(&e, enc + o, 8);
        if (cap_obj_kind(e)) same &= b == (0x100000000ull | (a & 0xffff)); else same &= a == b;
    }
    snprintf(why, sizeof why, "%s: round trip restores every field", name); CHECK(same, why);
}

int main(void) {
    uint64_t pa = (uint64_t)(uintptr_t)g_pool + 64, pb = (uint64_t)(uintptr_t)g_pool + 1024, obj = 0x7000001234ull;
    { uint64_t p[2] = {obj, 0}; roundtrip("jump", OP_JUMP, p, 8, 0, 1); }
    { struct { uint64_t val; uint32_t mode, v; } p = {pa, 0, 1}; roundtrip("signal_event", OP_SIGNAL_EVENT, &p, 16, 1, 0); }
    { uint64_t p[2] = {1, pa}; roundtrip("report_counter", OP_REPORT_COUNTER, p, 16, 1, 0); }
    { uint64_t p[3] = {pa, pb, 256}; roundtrip("copy_b2b", OP_COPY_B2B, p, 24, 2, 0); }
    { uint8_t p[32] = {0}; uint64_t d = pa, nn = 8; memcpy(p, &d, 8); memcpy(p + 8, &nn, 8); roundtrip("update_ub", OP_UPDATE_UB, p, 24, 1, 0); }
    { p_set_rt p; memset(&p, 0, sizeof p); p.n = 2; p.color[0] = (const tex_t *)(uintptr_t)obj; p.color[1] = (const tex_t *)(uintptr_t)(obj + 8); p.depth = (const tex_t *)(uintptr_t)(obj + 16);
      roundtrip("set_rt", OP_SET_RT, &p, sizeof p, 0, 3); }
    { p_bind_program p = {(const prog_t *)(uintptr_t)obj, 31}; roundtrip("bind_program", OP_BIND_PROGRAM, &p, sizeof p, 0, 1); }
    { p_bind_vbuf p; memset(&p, 0, sizeof p); p.n = 2; p.r[0].address = pa; p.r[0].size = 64; p.r[1].address = pb; p.r[1].size = 32;
      roundtrip("bind_vbuf (2 of 16, short record)", OP_BIND_VBUF, &p, (uint32_t)(offsetof(p_bind_vbuf, r) + 2 * sizeof p.r[0]), 2, 0); }
    { p_bind_buf p = {0, 3, pa, 256}; roundtrip("bind_ubo", OP_BIND_UBO, &p, sizeof p, 1, 0); roundtrip("bind_ssbo", OP_BIND_SSBO, &p, sizeof p, 1, 0); }
    { p_bind_handles p; memset(&p, 0, sizeof p); p.n = 2; p.h[0] = 5; p.h[1] = 6; roundtrip("bind_tex (handles stay)", OP_BIND_TEX, &p, (uint32_t)(offsetof(p_bind_handles, h) + 16), 0, 0); }
    { p_draw_idx p = {4, 1, 36, 0, 0, 1, pa}; roundtrip("draw_idx", OP_DRAW_IDX, &p, sizeof p, 1, 0); }
    { p_dispatch p = {8, 8, 1, 0x7100123456ull}; roundtrip("dispatch (caller kept)", OP_DISPATCH, &p, sizeof p, 0, 0); }
    { p_set_pool p = {(const dpool_t *)(uintptr_t)obj}; roundtrip("set_tex_pool", OP_SET_TEX_POOL, &p, sizeof p, 0, 1); }
    { p_copy_b2t p; memset(&p, 0, sizeof p); p.src = pa; p.tex = (const tex_t *)(uintptr_t)obj; roundtrip("copy_b2t", OP_COPY_B2T, &p, sizeof p, 1, 1); roundtrip("copy_t2b", OP_COPY_T2B, &p, sizeof p, 1, 1); }
    { p_copy_t2t p; memset(&p, 0, sizeof p); p.src = (const tex_t *)(uintptr_t)obj; p.dst = (const tex_t *)(uintptr_t)(obj + 8); roundtrip("copy_t2t", OP_COPY_T2T, &p, sizeof p, 0, 2); }
    { p_clear_tex p; memset(&p, 0, sizeof p); p.tex = (const tex_t *)(uintptr_t)obj; roundtrip("clear_tex", OP_CLEAR_TEX, &p, sizeof p, 0, 1); }
    { p_draw p = {4, 0, 3}; roundtrip("draw (no pointers)", OP_DRAW, &p, sizeof p, 0, 0); }
    CHECK(g_kinds[CAP_K_TEX] && g_kinds[CAP_K_PROG] && g_kinds[CAP_K_DPOOL] && g_kinds[CAP_K_SEG], "every object kind was walked");
    uint8_t junk[16] = {0};
    CHECK(!cap_rec_fields(OP_COUNT_, junk, 24, &ENC, NULL), "an unknown op is refused");
    CHECK(!cap_rec_fields(OP_SET_RT, junk, 16, &ENC, NULL), "a set_rt too short for its depth field is refused");
    { tex_t t; memset(&t, 0, sizeof t); t.mem = g_pool + 128; t.pool = (pool_t *)(uintptr_t)obj; t.snap = &t; t.has_layout = 1; t.L.size = 512;
      CHECK(cap_tex_fields(&t, &ENC, NULL) && cap_is_addr((uint64_t)(uintptr_t)t.mem) && cap_obj_kind((uint64_t)(uintptr_t)t.pool) == CAP_K_POOL && !t.snap, "a texture snapshot: storage, pool, no host snapshot pointer"); }
    CHECK(strstr(cap_layout(), "ops 40\n") != NULL, "the layout fingerprint names the op count");

    // The file: chunks in, chunks out, compressed when that is smaller; a cut file is refused
    char path[] = "/tmp/t_capture_XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0, "temporary file"); close(fd);
    cap_writer w; CHECK(cap_open(&w, path), "open for writing");
    static uint8_t big[1 << 16]; for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i / 97);   // compressible
    const char *text = "chain\tb-test\n";
    cap_put(&w, CAP_HEAD, 0, text, strlen(text), 0); cap_put(&w, CAP_REGN, 1, big, sizeof big, 1); cap_put(&w, CAP_SUBM, 1, "\0\0\0\0", 4, 0);
    CHECK(w.stored < w.raw, "the large chunk was compressed"); CHECK(cap_close(&w), "close");
    cap_reader r; CHECK(cap_load(&r, path) == 1, "load");
    cap_chunk c; uint8_t *d; int n = 0, okc = 1;
    while (cap_next(&r, &c, &d) == 1) {
        n++;
        if (c.tag == CAP_HEAD) okc &= c.raw == strlen(text) && !memcmp(d, text, c.raw) && c.codec == CAP_RAW;
        if (c.tag == CAP_REGN) okc &= c.raw == sizeof big && !memcmp(d, big, sizeof big) && c.codec == CAP_LZ4 && c.submit == 1;
        free(d);
    }
    CHECK(n == 3 && okc, "three chunks back, bytes and codecs as written");
    size_t full = r.n; cap_unload(&r);
    FILE *f = fopen(path, "r+b"); CHECK(f && ftruncate(fileno(f), (off_t)(full - 20)) == 0, "cut the file"); if (f) fclose(f);
    CHECK(cap_load(&r, path) == 1, "a cut file still loads its header");
    int last = 0; while ((last = cap_next(&r, &c, &d)) == 1) free(d);
    CHECK(last == -1, "a cut file ends in a corrupt chunk, not a clean end");
    cap_unload(&r); unlink(path);

    // Per-submit state text (CAP_STAT): the latched switches, the anisotropy level and the MetalFX 3D pass the submit is drawn with
    char st[160]; size_t sl = cap_stat_text(st, sizeof st, 0x100000002ull, 16, 1);
    CHECK(!strcmp(st, "host_latch\t4294967298\naniso\t16\nmfx3d\t1\n"), "cap_stat_text writes the three key/value lines");
    CHECK(sl == strlen(st), "cap_stat_text returns the text's length");
    char v[32];
    CHECK(cap_kv(st, sl, "host_latch", v, sizeof v) && !strcmp(v, "4294967298"), "cap_kv: host_latch");
    CHECK(cap_kv(st, sl, "aniso", v, sizeof v) && !strcmp(v, "16"), "cap_kv: aniso");
    CHECK(cap_kv(st, sl, "mfx3d", v, sizeof v) && !strcmp(v, "1"), "cap_kv: mfx3d");
    CHECK(!cap_kv(st, sl, "tex_pool_n", v, sizeof v), "cap_kv: a missing key gives NULL");
    { const char *t2 = "anisox\t3\naniso\t8\n"; CHECK(cap_kv(t2, strlen(t2), "aniso", v, sizeof v) && !strcmp(v, "8"), "cap_kv: whole-key match, not a prefix"); }
    // Precedence in the replayer: the recorded level beats the live run's env line (applied after it, both without overwriting); a value given to the replayer wins
    unsetenv("DM_NVN_ANISO"); unsetenv("DM_MFX3D");
    cap_stat_env(st, sl);
    CHECK(getenv("DM_NVN_ANISO") && !strcmp(getenv("DM_NVN_ANISO"), "16"), "cap_stat_env sets DM_NVN_ANISO from the record");
    CHECK(getenv("DM_MFX3D") && !strcmp(getenv("DM_MFX3D"), "1"), "cap_stat_env sets DM_MFX3D from the record");
    setenv("DM_NVN_ANISO", "4", 0);   // apply_env's live env line, applied afterwards
    CHECK(!strcmp(getenv("DM_NVN_ANISO"), "16"), "the recorded level beats the live run's env line");
    unsetenv("DM_NVN_ANISO"); unsetenv("DM_MFX3D"); setenv("DM_NVN_ANISO", "4", 1);   // given to the replayer
    cap_stat_env(st, sl);
    CHECK(!strcmp(getenv("DM_NVN_ANISO"), "4"), "a value given to the replayer wins over the record");
    unsetenv("DM_NVN_ANISO"); unsetenv("DM_MFX3D");
    { const char *old = "host_latch\t0\nbright_k\t1\n"; cap_stat_env(old, strlen(old)); }
    CHECK(!getenv("DM_NVN_ANISO") && !getenv("DM_MFX3D"), "an older capture without the keys changes nothing");
    // An older capture also holds a key this build no longer has (bright_k, the 3D brightening factor): it is skipped and the keys after it still read
    { const char *old = "host_latch\t7\nbright_k\t1.25\naniso\t8\nmfx3d\t0\n"; size_t ol = strlen(old);
      CHECK(cap_kv(old, ol, "host_latch", v, sizeof v) && !strcmp(v, "7") && cap_kv(old, ol, "aniso", v, sizeof v) && !strcmp(v, "8") && cap_kv(old, ol, "mfx3d", v, sizeof v) && !strcmp(v, "0"), "an older capture's unknown key is skipped"); }

    // Two-part chunks (cap_put2): a region header and the region's bytes compressed as one LZ4 stream straight from where they lie. 16 MiB with 13 MiB of noise
    // forces several full 4 MiB output flushes; 5 + 5 MiB of noise puts a flush inside part a and the a → b switch on a partly full output buffer
    char path2[] = "/tmp/t_capture_XXXXXX"; fd = mkstemp(path2); CHECK(fd >= 0, "temporary file 2"); close(fd);
    CHECK(cap_open(&w, path2), "open file 2");
    size_t big_n = 16u << 20, half = 5u << 20; uint8_t hd[32]; for (int i = 0; i < 32; i++) hd[i] = (uint8_t)i;
    uint8_t *b1 = malloc(big_n), *n2 = malloc(half), *n3 = malloc(half);
    CHECK(b1 && n2 && n3, "test buffers");
    for (size_t i = 0; i < (3u << 20); i++) b1[i] = (uint8_t)(i / 97);
    uint64_t x = 1; for (size_t i = 3u << 20; i < big_n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; b1[i] = (uint8_t)x; }
    x = 2; for (size_t i = 0; i < half; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; n2[i] = (uint8_t)x; }
    x = 3; for (size_t i = 0; i < half; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; n3[i] = (uint8_t)x; }
    uint64_t raw0 = w.raw;
    cap_put(&w, CAP_HEAD, 0, text, strlen(text), 0);
    cap_put2(&w, CAP_REGN, 1, hd, sizeof hd, b1, big_n);
    cap_put2(&w, CAP_REGN, 2, n2, half, n3, half);
    cap_put(&w, CAP_SUBM, 2, "\0\0\0\0", 4, 0);
    CHECK(w.raw - raw0 == strlen(text) + sizeof hd + big_n + 2 * half + 4, "w.raw counts every chunk's raw size");
    CHECK(!w.err && cap_close(&w), "close file 2");
    CHECK(cap_load(&r, path2) == 1, "load file 2");
    n = 0; int ok2 = 1, nregn = 0, subm_ok = 0;
    while (cap_next(&r, &c, &d) == 1) {
        n++;
        if (c.tag == CAP_REGN && c.submit == 1) { nregn++; ok2 &= c.codec == CAP_LZ4 && c.raw == sizeof hd + big_n && !memcmp(d, hd, sizeof hd) && !memcmp(d + sizeof hd, b1, big_n); }
        else if (c.tag == CAP_REGN && c.submit == 2) { nregn++; ok2 &= c.codec == CAP_LZ4 && c.raw == 2 * half && !memcmp(d, n2, half) && !memcmp(d + half, n3, half); }
        else if (c.tag == CAP_SUBM) subm_ok = c.submit == 2 && c.raw == 4 && !memcmp(d, "\0\0\0\0", 4);
        free(d);
    }
    CHECK(n == 4, "four chunks back from file 2"); CHECK(nregn == 2 && ok2, "both two-part chunks decode to a || b, LZ4, with their submit and raw size");
    CHECK(subm_ok, "the chunk after them reads intact (each header patch-back left the file position right)");
    cap_unload(&r); unlink(path2); free(b1); free(n2); free(n3);
    printf("t_capture: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
