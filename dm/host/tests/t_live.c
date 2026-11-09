// Live hooks (dm_live.c) on a fake module: a block of small functions copied into fresh pages and sealed read + execute, as the loader does.
// Each relocation case (adrp, b.cond, cbz, tbz, bl, ldr literal, b) must give the same result hooked as unhooked; flags and floating-point
// registers survive a hook; captures read registers and typed memory; filters, every, max and the rate budget work; a return hook records the
// return value; four threads hammering one hook lose no hit; a rewritten gate and an exclusive load are refused; disarm puts the instruction
// back. The output file has the header and one record per recorded hit
#include "../dm_live.c"
#include <assert.h>
module_t g_mod[MAX_MOD]; int g_nmod;
void dm_ctl_register(const char *n, dm_ctl_fn f) { (void)n; (void)f; }
const char *dm_ctl_get(const ctl_args *a, const char *k) { (void)a; (void)k; return NULL; }
double dm_ctl_num(const ctl_args *a, const char *k, double d) { (void)a; (void)k; return d; }

__asm__(
    ".text\n.p2align 12\n.globl _lvt_start\n_lvt_start:\n"
    "_lvt_add: add x0, x0, x1\n ret\n"
    "_lvt_add2: add x0, x0, x1\n ret\n"
    "_lvt_f2: add x0, x0, #1\n ret\n"
    "_lvt_f3: add x0, x0, #1\n ret\n"
    "_lvt_f4: add x0, x0, #1\n ret\n"
    "_lvt_answer: stp x29, x30, [sp, #-16]!\n mov x29, sp\n mov x0, #42\n ldp x29, x30, [sp], #16\n ret\n"
    "_lvt_adrp: adrp x0, _lvt_data@PAGE\n add x0, x0, _lvt_data@PAGEOFF\n ret\n"
    "_lvt_bcond: cmp x0, #5\n_lvt_bcond_br: b.eq 1f\n mov x0, #1\n ret\n1: mov x0, #2\n ret\n"
    "_lvt_cbz: cbz x0, 1f\n mov x0, #7\n ret\n1: mov x0, #9\n ret\n"
    "_lvt_tbz: tbz x0, #3, 1f\n mov x0, #13\n ret\n1: mov x0, #11\n ret\n"
    "_lvt_bl: stp x29, x30, [sp, #-16]!\n_lvt_bl_site: bl _lvt_answer\n add x0, x0, #1\n ldp x29, x30, [sp], #16\n ret\n"
    "_lvt_ldr: ldr x0, 1f\n ret\n.p2align 3\n1: .quad 0x1122334455667788\n"
    "_lvt_ldrw: ldr w0, 1f\n ret\n.p2align 2\n1: .long 0x89abcdef\n"
    "_lvt_ldrsw: ldrsw x0, 1f\n ret\n.p2align 2\n1: .long 0x89abcdef\n"
    "_lvt_b: b 1f\n mov x0, #0\n ret\n1: mov x0, #3\n ret\n"
    "_lvt_excl: ldxr x0, [x1]\n ret\n"
    "_lvt_gate: b _lvt_patch\n"
    "_lvt_flags: cmp x0, #5\n_lvt_flags_site: mov x2, #0\n b.eq 1f\n mov x0, #1\n ret\n1: mov x0, #2\n ret\n"
    "_lvt_fp: fadd d0, d0, d0\n ret\n"
    ".p2align 3\n_lvt_data: .quad 0, 0\n"
    ".p2align 12\n_lvt_patch: ret\n"
    ".p2align 12\n.globl _lvt_end\n_lvt_end:\n");
extern char lvt_start[], lvt_end[], lvt_add[], lvt_add2[], lvt_f2[], lvt_f3[], lvt_f4[], lvt_answer[], lvt_adrp[], lvt_bcond[], lvt_bcond_br[], lvt_cbz[], lvt_tbz[],
    lvt_bl[], lvt_bl_site[], lvt_ldr[], lvt_ldrw[], lvt_ldrsw[], lvt_b[], lvt_excl[], lvt_gate[], lvt_flags[], lvt_flags_site[], lvt_fp[], lvt_patch[];
static uint8_t *M; static int fail, count;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); count++; if (w_ != g_) { printf("✗ %s: want %#llx, got %#llx\n", name, w_, g_); fail++; } else printf("✓ %s\n", name); } while (0)
static uint64_t off(const char *p) { return (uint64_t)(p - lvt_start); }
static void *fn(const char *p) { return M + off(p); }
typedef uint64_t (*f2)(uint64_t, uint64_t);
typedef double (*fd)(double);
static int add(const char *site, const char *extra) { char spec[512], err[256] = ""; snprintf(spec, sizeof spec, "at=0x%llx %s", (unsigned long long)(uint64_t)(M + off(site)), extra); int id = dm_live_add(spec, err, sizeof err); if (id < 0) printf("  (add %s: %s)\n", extra, err); return id; }
static const char *refusal(const char *site) { static char err[256]; char spec[128]; err[0] = 0; snprintf(spec, sizeof spec, "at=0x%llx", (unsigned long long)(uint64_t)(M + off(site))); return dm_live_add(spec, err, sizeof err) < 0 ? err : ""; }
static dm_live_counts cnt(int id) { dm_live_counts c = {0}; dm_live_counts_of(id, &c); return c; }
static void *hammer(void *u) { f2 f = (f2)fn(lvt_add2); uint64_t s = 0; for (int i = 0; i < 2000000; i++) s += f((uint64_t)i, 1); return (void *)(uintptr_t)s; }
int main(void) {
    char out[1024]; snprintf(out, sizeof out, "%s/t_live.bin", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp"); setenv("DM_LIVE_OUT", out, 1);
    size_t size = (size_t)(lvt_end - lvt_start), alloc = (size + 0x3FFF) & ~0x3FFFull;
    M = mmap(NULL, alloc + LV_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(M != MAP_FAILED);   // the module and, after it, the arena (as loader.c reserves it)
    memcpy(M, lvt_start, size); mprotect(M, alloc, PROT_READ | PROT_EXEC); sys_icache_invalidate(M, alloc);
    dm_live_set_arena(M + alloc, LV_ARENA);
    uint64_t adrp_plain = ((uint64_t (*)(void))fn(lvt_adrp))();
    EXPECT("attach", 0, dm_live_attach(M, alloc, off(lvt_patch), off(lvt_patch), "lvt"));
    // entry hook with captures of registers and typed memory
    uint32_t words[4] = {0x11, 0x22, 0x33, 0x44}; struct { uint32_t *p; uint64_t v; } obj = {words, 0xfeedfacecafebeefull};
    int h_add = add(lvt_add, "kind=entry cap=x1,[x0+0x8].q,[[x0]+0x4].w name=add");
    EXPECT("arena in branch range", 1, (int64_t)(dm_live_tramp(h_add) - (uint64_t)M) < (128ll << 20) && (int64_t)((uint64_t)M - dm_live_tramp(h_add)) < (128ll << 20));
    EXPECT("hooked add still adds", (uint64_t)&obj + 5, ((f2)fn(lvt_add))((uint64_t)&obj, 5));
    EXPECT("one hit, one record", 1, cnt(h_add).hits == 1 && cnt(h_add).recorded == 1);
    EXPECT("a second hook on the same place is refused", 1, add(lvt_add, "") < 0);
    // relocation cases: hooked = unhooked
    add(lvt_adrp, ""); EXPECT("adrp relocated", adrp_plain, ((uint64_t (*)(void))fn(lvt_adrp))());
    add(lvt_bcond_br, "kind=insn"); EXPECT("b.cond taken", 2, ((f2)fn(lvt_bcond))(5, 0)); EXPECT("b.cond not taken", 1, ((f2)fn(lvt_bcond))(4, 0));
    add(lvt_cbz, ""); EXPECT("cbz taken", 9, ((f2)fn(lvt_cbz))(0, 0)); EXPECT("cbz not taken", 7, ((f2)fn(lvt_cbz))(1, 0));
    add(lvt_tbz, ""); EXPECT("tbz taken", 11, ((f2)fn(lvt_tbz))(0, 0)); EXPECT("tbz not taken", 13, ((f2)fn(lvt_tbz))(8, 0));
    add(lvt_bl_site, "kind=insn"); EXPECT("bl relocated (call, then back)", 43, ((f2)fn(lvt_bl))(0, 0));
    add(lvt_ldr, ""); EXPECT("ldr x literal", 0x1122334455667788ull, ((f2)fn(lvt_ldr))(0, 0));
    add(lvt_ldrw, ""); EXPECT("ldr w literal", 0x89abcdefull, ((f2)fn(lvt_ldrw))(0, 0));
    add(lvt_ldrsw, ""); EXPECT("ldrsw literal", 0xffffffff89abcdefull, ((f2)fn(lvt_ldrsw))(0, 0));
    add(lvt_b, ""); EXPECT("b relocated", 3, ((f2)fn(lvt_b))(0, 0));
    add(lvt_flags_site, "kind=insn"); EXPECT("flags survive (equal)", 2, ((f2)fn(lvt_flags))(5, 0)); EXPECT("flags survive (not equal)", 1, ((f2)fn(lvt_flags))(4, 0));
    add(lvt_fp, ""); EXPECT("d0 survives", 1, ((fd)fn(lvt_fp))(1.5) == 3.0);
    // refusals
    EXPECT("an exclusive load is refused", 1, strstr(refusal(lvt_excl), "exclusive") != NULL);
    EXPECT("a rewritten gate is refused", 1, strstr(refusal(lvt_gate), "rewritten") != NULL);
    // filter, every / max, return value, rate budget
    int h_f2 = add(lvt_f2, "filt=x0=0x5");
    for (int i = 0; i < 10; i++) ((f2)fn(lvt_f2))((uint64_t)i, 0);
    EXPECT("filter: 10 hits", 10, cnt(h_f2).hits); EXPECT("filter: 1 match recorded", 1, cnt(h_f2).recorded);
    int h_f3 = add(lvt_f3, "every=2 max=3");
    for (int i = 0; i < 20; i++) ((f2)fn(lvt_f3))((uint64_t)i, 0);
    EXPECT("every=2 max=3: 3 records of 20 hits", 3, cnt(h_f3).recorded);
    int h_ret = add(lvt_answer, "kind=ret");
    EXPECT("a return hook keeps the result", 42, ((f2)fn(lvt_answer))(0, 0));
    EXPECT("entry and return recorded", 2, cnt(h_ret).recorded);
    int h_rate = add(lvt_f4, "rate=100000");
    for (int i = 0; i < 10000000; i++) ((f2)fn(lvt_f4))((uint64_t)i, 0);
    EXPECT("over its rate a hook disarms itself", 1, cnt(h_rate).disarmed_for_rate && !cnt(h_rate).armed);
    EXPECT("and its instruction is back", 1, *(uint32_t *)fn(lvt_f4) == *(uint32_t *)lvt_f4);
    // four threads on one hook
    int h_mt = add(lvt_add2, "rate=0 max=1000");
    pthread_t th[4]; for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, hammer, NULL); for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    EXPECT("four threads: every hit counted", 8000000, cnt(h_mt).hits);
    EXPECT("four threads: max=1000 recorded", 1000, cnt(h_mt).recorded + cnt(h_mt).dropped);
    // disarm
    EXPECT("disarm", 1, dm_live_del(h_add));
    EXPECT("the original instruction is back", 1, *(uint32_t *)fn(lvt_add) == *(uint32_t *)lvt_add);
    ((f2)fn(lvt_add))(1, 1); EXPECT("no more hits once disarmed", 1, cnt(h_add).hits);
    // the file
    dm_live_flush();
    FILE *f = fopen(out, "rb"); dm_live_hdr hd = {0}; size_t got = f ? fread(&hd, sizeof hd, 1, f) : 0;
    EXPECT("header", 1, got == 1 && !memcmp(hd.magic, "DMLIVE01", 8) && hd.base == (uint64_t)M && hd.rec_size == sizeof(dm_live_rec));
    int n_add = 0, ret_ok = 0, cap_ok = 0; dm_live_rec r;
    while (f && fread(&r, sizeof r, 1, f) == 1) {
        if (r.hook == (uint32_t)h_add) { n_add++; cap_ok = r.cap[0] == 5 && r.cap[1] == 0xfeedfacecafebeefull && r.cap[2] == 0x22 && r.x[0] == (uint64_t)&obj; }
        if (r.hook == ((uint32_t)h_ret | DM_LIVE_RET)) ret_ok = r.x[0] == 42 && r.aux > 0;
    }
    if (f) fclose(f);
    EXPECT("the add record's captures (x1, [x0+8].q, [[x0]+4].w)", 1, n_add == 1 && cap_ok);
    EXPECT("the return record holds 42 and a duration", 1, ret_ok);
    printf("%s %d/%d\n", fail ? "FAIL" : "OK", count - fail, count);
    return fail != 0;
}
