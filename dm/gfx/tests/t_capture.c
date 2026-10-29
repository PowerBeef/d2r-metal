// NVN frame capture, CPU side (nvn_capture.c): every record op the walk knows encodes its pointers into region offsets and object indices and decodes back to
// the same values, an unknown op is refused, and a capture file round-trips its chunks (raw and LZ4) and rejects a corrupt one. No GPU, no game data.
// Adapted from nxnative's t_capture.c (github.com/seedds/nxnative, MIT, (c) 2026 seedds), rewritten for d2r-metal's record set.
#include "../nvn/nvn_capture.h"
#include <stdio.h>
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
    printf("t_capture: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
