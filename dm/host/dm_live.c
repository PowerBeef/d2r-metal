// dm_live.c: live hooks for the engine-analysis harness (dm/tools/re, the private notes' runtime checks). On a running game, one instruction
// of the game's code becomes a branch to a trampoline that saves every register, records the hit (registers, the caller, typed memory reads)
// into the calling thread's ring, runs the displaced instruction (relocated when it is PC-relative) and branches back. A collector thread
// drains the rings into a file (DM_LIVE_OUT, default live.bin); dm/tools/re reads it with the analysis map.
// - Patching goes through a second, writable mapping of the code pages: the executing pages never change protection, so other threads keep
//   running while hooks come and go (2,000 patches under 3 calling threads without a fault, 2026-11-09).
// - A hit costs a few ns (about 4 ns in a tight loop, against 1.8 µs for DM_PROBE's trap and signal); a capture that reads memory adds a
//   system call (vm_read, so a bad pointer is a zero, never a crash).
// - Return hooks swap the return address for a shared return trampoline (a per-thread shadow stack, matched by the stack pointer).
// - Budgets per hook: max (records), every (1 in N), rate (hits a second, then the hook disarms itself), and a filter on one value.
// Mac development runs only (DM_LIVE=1): never in a released app, and the iPhone's signed pages can't be written. No game addresses of its own.
#include <TargetConditionals.h>
#include "dm_live.h"
#if TARGET_OS_IPHONE
void dm_live_boot(void) {}
void dm_live_ctl_init(void) {}
int dm_live_attach(uint8_t *b, size_t s, uint64_t t, uint64_t p, const char *n) { (void)b; (void)s; (void)t; (void)p; (void)n; return -1; }
int dm_live_add(const char *spec, char *err, size_t ecap) { (void)spec; if (ecap) { err[0] = 0; } return -1; }
int dm_live_del(int id) { (void)id; return -1; }
void dm_live_flush(void) {}
int dm_live_counts_of(int id, dm_live_counts *o) { (void)id; (void)o; return 0; }
uint64_t dm_live_tramp(int id) { (void)id; return 0; }
size_t dm_live_arena_size(void) { return 0; }
void dm_live_set_arena(uint8_t *p, size_t sz) { (void)p; (void)sz; }
#else
#include "dm.h"
#include "dm_ctl.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LV_MAX 65536            // hooks (the call-trace mode arms every function)
#define LV_RING 8192            // records per thread ring
#define LV_SS 64                // return-hook shadow stack depth per thread
#define LV_FRAME 768            // trampoline frame: x0-x30, nzcv, q0-q7, q16-q31 (640 bytes) and 128 for a red zone below the caller's sp
#define LV_ARENA (32u << 20)    // trampoline arena, within branch range of the whole module
#define LV_TRAMP_MAX 384        // bytes reserved per trampoline: about 70 instructions and a literal pool
enum { LV_INSN, LV_ENTRY, LV_RET };

typedef struct { int8_t reg; uint8_t n, size; int32_t off[4]; } lv_src;   // n = 0: the register; else n levels of [value + off], the last read with size bytes
typedef struct {
    _Atomic uint64_t hits, matched, recorded, dropped, win_t, win_hits;
    _Atomic int armed; int disarmed_for_rate;
    uint64_t site; uint32_t orig; uint32_t *tramp;
    int id, kind, ncap, filt; char fop;
    lv_src cap[4], fsrc; uint64_t fval, max, every, rate;
    char name[48], spec[256];
} lv_hook;
typedef struct { uint64_t lr, sp, t; lv_hook *h; } lv_ss;
typedef struct lv_ring { _Atomic uint64_t head, tail; uint64_t dropped; uint32_t tid; int ssn; lv_ss ss[LV_SS]; struct lv_ring *next; dm_live_rec rec[LV_RING]; } lv_ring;

static uint8_t *g_base, *g_text_rw, *g_arena, *g_arena_rw; static size_t g_size, g_rx_len, g_arena_used; static uint64_t g_text_end, g_patch_off;
static char g_modname[16];
static lv_hook *g_hooks; static _Atomic int g_nhooks;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_drain_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_key; static lv_ring *_Atomic g_rings;
static uint32_t *g_ret_tramp;
static uint8_t *g_arena_pre;   // the loader's reservation after main (dm_live_set_arena)
static FILE *g_out; static char g_out_path[1024]; static uint64_t g_written;
static uint64_t g_timer_hz;

static inline uint64_t ticks(void) { uint64_t t; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t)); return t; }
static uint64_t timer_hz(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f; }

// ---- the per-thread rings (one producer: the thread; one consumer: the collector under g_drain_mu) ----
static lv_ring *ring_get(void) {
    lv_ring *r = pthread_getspecific(g_key); if (r) return r;
    r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);   // mmap, not malloc: a hook may fire anywhere
    if (r == MAP_FAILED) abort();
    r->tid = pthread_mach_thread_np(pthread_self());
    pthread_setspecific(g_key, r);
    lv_ring *old = atomic_load(&g_rings); do r->next = old; while (!atomic_compare_exchange_weak(&g_rings, &old, r));
    return r;
}
static int src_eval(const lv_src *s, const uint64_t *ctx, uint64_t *out) {
    uint64_t v = s->reg == 31 ? (uint64_t)ctx + LV_FRAME : ctx[s->reg];
    for (int i = 0; i < s->n; i++) {
        uint64_t a = v + (int64_t)s->off[i], r = 0; mach_vm_size_t sz = i == s->n - 1 ? s->size : 8, got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), a, sz, (mach_vm_address_t)&r, &got) != KERN_SUCCESS || got != sz) { *out = 0; return 0; }
        v = r;
    }
    *out = v; return 1;
}
static int ring_put(lv_ring *r, lv_hook *h, const uint64_t *ctx, uint64_t t, uint32_t flag, uint64_t lr, uint64_t aux) {
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed), tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail >= LV_RING) { r->dropped++; return 0; }
    dm_live_rec *x = &r->rec[head & (LV_RING - 1)];
    x->hook = (uint32_t)h->id | flag; x->tid = r->tid; x->t = t; x->lr = lr; x->sp = (uint64_t)ctx + LV_FRAME; x->aux = aux;
    for (int i = 0; i < 4; i++) x->x[i] = ctx[i];
    for (int i = 0; i < 4; i++) { uint64_t v = 0; if (!flag && i < h->ncap) src_eval(&h->cap[i], ctx, &v); x->cap[i] = v; }
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return 1;
}

// ---- patching through the writable alias ----
static void put_insn(uint64_t at, uint32_t w) {   // one aligned word: other threads see the old or the new instruction, never a mix
    __atomic_store_n((uint32_t *)(g_text_rw + (at - (uint64_t)g_base)), w, __ATOMIC_RELEASE);
    sys_icache_invalidate((void *)at, 4);
}
static void disarm(lv_hook *h) { if (atomic_exchange(&h->armed, 0)) put_insn(h->site, h->orig); }
static void rate_check(lv_hook *h) {   // every 1024 hits: over its rate a hook disarms itself (the collector reports it; logging here could run on a small game stack)
    uint64_t t = ticks(), w = atomic_load(&h->win_t), n = atomic_load(&h->hits);
    if (!w) { atomic_store(&h->win_t, t); atomic_store(&h->win_hits, n); return; }
    if (t - w < g_timer_hz / 20) return;   // judged over 50 ms windows
    double rate = (double)(n - atomic_load(&h->win_hits)) * g_timer_hz / (double)(t - w);
    atomic_store(&h->win_t, t); atomic_store(&h->win_hits, n);
    if (rate > (double)h->rate) { h->disarmed_for_rate = 1; disarm(h); }
}

// ---- what the trampolines call ----
__attribute__((used)) static void lv_hit(uint64_t *ctx, lv_hook *h) {
    uint64_t n = atomic_fetch_add_explicit(&h->hits, 1, memory_order_relaxed) + 1;
    if (h->rate && !(n & 1023)) rate_check(h);
    if (h->filt) { uint64_t v; if (!src_eval(&h->fsrc, ctx, &v)) return; if (h->fop == '=' ? v != h->fval : !(v & h->fval)) return; }
    uint64_t m = atomic_fetch_add_explicit(&h->matched, 1, memory_order_relaxed);   // 0-based among the matching hits
    if (h->every > 1 && m % h->every) return;
    if (h->max && (h->every > 1 ? m / h->every : m) >= h->max) return;
    lv_ring *r = ring_get(); uint64_t t = ticks();
    if (ring_put(r, h, ctx, t, 0, ctx[30], 0)) atomic_fetch_add_explicit(&h->recorded, 1, memory_order_relaxed); else atomic_fetch_add_explicit(&h->dropped, 1, memory_order_relaxed);
    if (h->kind == LV_RET && r->ssn < LV_SS) { r->ss[r->ssn++] = (lv_ss){ctx[30], (uint64_t)ctx + LV_FRAME, t, h}; ctx[30] = (uint64_t)g_ret_tramp; }
}
__attribute__((used)) static void lv_ret(uint64_t *ctx) {
    lv_ring *r = ring_get(); uint64_t sp = (uint64_t)ctx + LV_FRAME;
    while (r->ssn > 1 && r->ss[r->ssn - 1].sp < sp) r->ssn--;   // deeper frames left without returning here (a longjmp): drop them
    if (!r->ssn) { LOG("【红】live hooks: a return came back with an empty shadow stack"); abort(); }   // no way back to the caller: a ret hook not on a function's first instruction
    lv_ss e = r->ss[--r->ssn]; uint64_t t = ticks();
    if (ring_put(r, e.h, ctx, t, DM_LIVE_RET, e.lr, t - e.t)) atomic_fetch_add_explicit(&e.h->recorded, 1, memory_order_relaxed); else atomic_fetch_add_explicit(&e.h->dropped, 1, memory_order_relaxed);
    ctx[30] = e.lr;
}

// ---- code generation ----
typedef struct { uint32_t w[LV_TRAMP_MAX / 4]; int n; uint64_t lit[16]; int nlit, lfix[16]; uint64_t pc; } tb;   // pc = where w[0] will run
static void emit(tb *b, uint32_t w) { b->w[b->n++] = w; }
static void emit_ldr_lit(tb *b, int rt, uint64_t v) { b->lfix[b->nlit] = b->n; b->lit[b->nlit++] = v; emit(b, 0x58000000u | (uint32_t)rt); }
static int b_fits(uint64_t from, uint64_t to) { int64_t d = (int64_t)(to - from); return !(d & 3) && d >= -(1ll << 27) && d < (1ll << 27); }
static uint32_t b_to(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)(to - from) >> 2) & 0x3ffffff); }
static void emit_b(tb *b, uint64_t to) { emit(b, b_to(b->pc + 4 * (uint64_t)b->n, to)); }
static void emit_save(tb *b) {
    emit(b, 0xD1000000u | (LV_FRAME << 10) | (31 << 5) | 31);   // sub sp, sp, #LV_FRAME
    for (int r = 0; r < 30; r += 2) emit(b, 0xA9000000u | ((uint32_t)r << 15) | ((uint32_t)(r + 1) << 10) | (31 << 5) | (uint32_t)r);   // stp xr, xr+1, [sp, #8r]
    emit(b, 0xF9000000u | (30 << 10) | (31 << 5) | 30);   // str x30, [sp, #240]
    emit(b, 0xD53B4210u); emit(b, 0xF9000000u | (31 << 10) | (31 << 5) | 16);   // mrs x16, nzcv; str x16, [sp, #248]
    for (int k = 0; k < 4; k++) { int q = 2 * k; emit(b, 0xAD000000u | ((uint32_t)(16 + 2 * k) << 15) | ((uint32_t)(q + 1) << 10) | (31 << 5) | (uint32_t)q); }   // stp q0-q7 at 256
    for (int k = 0; k < 8; k++) { int q = 16 + 2 * k; emit(b, 0xAD000000u | ((uint32_t)(24 + 2 * k) << 15) | ((uint32_t)(q + 1) << 10) | (31 << 5) | (uint32_t)q); }   // stp q16-q31 at 384
}
static void emit_restore(tb *b) {
    for (int k = 0; k < 8; k++) { int q = 16 + 2 * k; emit(b, 0xAD400000u | ((uint32_t)(24 + 2 * k) << 15) | ((uint32_t)(q + 1) << 10) | (31 << 5) | (uint32_t)q); }
    for (int k = 0; k < 4; k++) { int q = 2 * k; emit(b, 0xAD400000u | ((uint32_t)(16 + 2 * k) << 15) | ((uint32_t)(q + 1) << 10) | (31 << 5) | (uint32_t)q); }
    emit(b, 0xF9400000u | (31 << 10) | (31 << 5) | 16); emit(b, 0xD51B4210u);   // ldr x16, [sp, #248]; msr nzcv, x16
    for (int r = 0; r < 30; r += 2) emit(b, 0xA9400000u | ((uint32_t)r << 15) | ((uint32_t)(r + 1) << 10) | (31 << 5) | (uint32_t)r);
    emit(b, 0xF9400000u | (30 << 10) | (31 << 5) | 30);   // ldr x30, [sp, #240]
    emit(b, 0x91000000u | (LV_FRAME << 10) | (31 << 5) | 31);   // add sp, sp, #LV_FRAME
}
static void emit_call(tb *b, uint64_t fn, int with_hook, uint64_t hook) {
    emit(b, 0x910003E0u);   // mov x0, sp
    if (with_hook) emit_ldr_lit(b, 1, hook);
    emit_ldr_lit(b, 16, fn); emit(b, 0xD63F0200u);   // blr x16
}
static int finish(tb *b) {   // the literal pool after the code (8-byte aligned), then each ldr-literal's offset
    if ((b->pc + 4 * (uint64_t)b->n) & 7) emit(b, 0xD503201Fu);   // nop
    int lit0 = b->n;
    if ((lit0 + 2 * b->nlit) * 4 > LV_TRAMP_MAX) return 0;
    for (int i = 0; i < b->nlit; i++) { memcpy(&b->w[b->n], &b->lit[i], 8); b->n += 2; }
    for (int i = 0; i < b->nlit; i++) { int at = b->lfix[i], d = lit0 + 2 * i - at; b->w[at] |= (uint32_t)(d & 0x7ffff) << 5; }
    return 1;
}
static int sext(uint64_t v, int bits) { return (int)((int64_t)(v << (64 - bits)) >> (64 - bits)); }
// The displaced instruction, run from the trampoline at b->pc + 4n as if at pc; then on to pc + 4 (or wherever it branches). 0 = refused
static int relocate(tb *b, uint64_t pc, uint32_t op, char *err, size_t ecap) {
    uint64_t back = pc + 4;
    if ((op & 0x9F000000u) == 0x10000000u || (op & 0x9F000000u) == 0x90000000u) {   // adr / adrp
        int64_t imm = sext(((op >> 5) & 0x7ffff) << 2 | ((op >> 29) & 3), 21);
        uint64_t v = (op & 0x80000000u) ? (pc & ~0xfffull) + ((uint64_t)imm << 12) : pc + (uint64_t)imm;
        emit_ldr_lit(b, (int)(op & 31), v); emit_b(b, back); return 1;
    }
    if ((op & 0xFC000000u) == 0x14000000u || (op & 0xFC000000u) == 0x94000000u) {   // b / bl
        uint64_t to = pc + (uint64_t)((int64_t)sext(op & 0x3ffffff, 26) * 4);
        if (op & 0x80000000u) emit_ldr_lit(b, 30, back);
        if (!b_fits(b->pc + 4 * (uint64_t)b->n, to)) { snprintf(err, ecap, "its branch target is out of the trampoline's range"); return 0; }
        emit_b(b, to); return 1;
    }
    int cond = (op & 0xFF000010u) == 0x54000000u, cbz = (op & 0x7E000000u) == 0x34000000u, tbz = (op & 0x7E000000u) == 0x36000000u;
    if (cond || cbz || tbz) {   // the same test, taken to a branch to the old target
        uint64_t to = tbz ? pc + (uint64_t)((int64_t)sext((op >> 5) & 0x3fff, 14) * 4) : pc + (uint64_t)((int64_t)sext((op >> 5) & 0x7ffff, 19) * 4);
        uint32_t w = tbz ? (op & ~(0x3fffu << 5)) | (2u << 5) : (op & ~(0x7ffffu << 5)) | (2u << 5);   // + 8: over the branch back
        emit(b, w); emit_b(b, back);
        if (!b_fits(b->pc + 4 * (uint64_t)b->n, to)) { snprintf(err, ecap, "its branch target is out of the trampoline's range"); return 0; }
        emit_b(b, to); return 1;
    }
    if ((op & 0x3B000000u) == 0x18000000u) {   // ldr (literal)
        if (op & 0x04000000u) { snprintf(err, ecap, "a floating-point literal load is not relocated"); return 0; }
        unsigned opc = op >> 30, rt = op & 31; uint64_t at = pc + (uint64_t)((int64_t)sext((op >> 5) & 0x7ffff, 19) * 4);
        if (opc == 3) { emit_b(b, back); return 1; }   // prfm: a hint, dropped
        emit_ldr_lit(b, (int)rt, at);
        emit(b, (opc == 0 ? 0xB9400000u : opc == 1 ? 0xF9400000u : 0xB9800000u) | (rt << 5) | rt);   // ldr wt / xt, ldrsw xt, [xt]
        emit_b(b, back); return 1;
    }
    if ((op & 0x3F000000u) == 0x08000000u) { snprintf(err, ecap, "an exclusive load / store (its loop would never succeed through a hook)"); return 0; }
    if ((op & 0xFFE0001Fu) == 0xD4200000u) { snprintf(err, ecap, "a brk (a DM_PROBE or coverage trap is there)"); return 0; }
    emit(b, op); emit_b(b, back); return 1;
}
static uint32_t *arena_take(void) {
    if (g_arena_used + LV_TRAMP_MAX > LV_ARENA) return NULL;
    uint32_t *p = (uint32_t *)(g_arena + g_arena_used); g_arena_used += LV_TRAMP_MAX; return p;
}
static void arena_put(uint32_t *at, const tb *b) {
    memcpy(g_arena_rw + ((uint8_t *)at - g_arena), b->w, (size_t)b->n * 4);
    sys_icache_invalidate(at, (size_t)b->n * 4);
}

// ---- attach: the writable alias of the code, the arena in branch range, the return trampoline, the output ----
static uint8_t *alias_rw(uint8_t *p, size_t sz) {
    mach_vm_address_t a = 0; vm_prot_t cur = 0, max = 0;
    if (mach_vm_remap(mach_task_self(), &a, sz, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)p, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) return NULL;
    if (mach_vm_protect(mach_task_self(), a, sz, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) return NULL;
    return (uint8_t *)a;
}
// The loader asks how much to reserve after main (DM_LIVE=1 runs only) and hands the reservation over
size_t dm_live_arena_size(void) { const char *e = getenv("DM_LIVE"); return e && !strcmp(e, "1") ? LV_ARENA : 0; }
void dm_live_set_arena(uint8_t *p, size_t sz) { if (sz >= LV_ARENA) g_arena_pre = p; }
static void *collector(void *u);
int dm_live_attach(uint8_t *base, size_t size, uint64_t text_end, uint64_t patch_off, const char *name) {
    pthread_mutex_lock(&g_mu);
    if (g_base) { int same = g_base == base; pthread_mutex_unlock(&g_mu); return same ? 0 : -1; }
    g_timer_hz = timer_hz(); pthread_key_create(&g_key, NULL);
    g_rx_len = (text_end + 0x3FFF) & ~0x3FFFull;
    if (!(g_text_rw = alias_rw(base, g_rx_len))) { pthread_mutex_unlock(&g_mu); LOG("【红】live hooks: no writable alias of %s's code", name); return -1; }
    // the arena: the loader's reservation right after the module (DM_LIVE=1 runs), else the first free gap the system gives near it,
    // after then before; every address in it must be within ±128 MB of every code address (a single b reaches it)
    uint64_t lo = (uint64_t)base, hi = lo + size;
    if (g_arena_pre) { mprotect(g_arena_pre, LV_ARENA, PROT_READ | PROT_EXEC); if ((g_arena_rw = alias_rw(g_arena_pre, LV_ARENA))) g_arena = g_arena_pre; }
    for (int k = 0; k < 16 && !g_arena; k++) {
        uint64_t hint = k < 8 ? ((hi + 0x3FFF) & ~0x3FFFull) + (uint64_t)k * (8u << 20) : ((lo - LV_ARENA - (uint64_t)(k - 8) * (8u << 20)) & ~0x3FFFull);
        uint8_t *a = mmap((void *)hint, LV_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) continue;
        uint64_t c = (uint64_t)a;
        if (c + LV_ARENA - lo >= (128u << 20) - 64 || hi - (c < hi ? c : hi) >= (128u << 20) - 64 || (c < lo && c + LV_ARENA > lo)) { munmap(a, LV_ARENA); continue; }
        mprotect(a, LV_ARENA, PROT_READ | PROT_EXEC);
        if (!(g_arena_rw = alias_rw(a, LV_ARENA))) { munmap(a, LV_ARENA); continue; }
        g_arena = a;
    }
    if (!g_arena) { pthread_mutex_unlock(&g_mu); LOG("【红】live hooks: no arena within branch range of %s", name); return -1; }
    g_hooks = mmap(NULL, sizeof(lv_hook) * LV_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_base = base; g_size = size; g_text_end = text_end; g_patch_off = patch_off; strlcpy(g_modname, name, sizeof g_modname);
    { tb b = {.pc = 0}; uint32_t *at = arena_take(); b.pc = (uint64_t)at;   // the shared return trampoline
      emit_save(&b); emit_call(&b, (uint64_t)lv_ret, 0, 0); emit_restore(&b); emit(&b, 0xD65F03C0u); finish(&b); arena_put(at, &b); g_ret_tramp = at; }
    const char *o = getenv("DM_LIVE_OUT"); strlcpy(g_out_path, o && *o ? o : "live.bin", sizeof g_out_path);
    if ((g_out = fopen(g_out_path, "wb"))) {
        dm_live_hdr h = {{'D', 'M', 'L', 'I', 'V', 'E', '0', '1'}, (uint64_t)base, size, g_timer_hz, sizeof(dm_live_rec), 1};
        fwrite(&h, sizeof h, 1, g_out); fflush(g_out);
        char p[1100]; snprintf(p, sizeof p, "%s.hooks.tsv", g_out_path); FILE *f = fopen(p, "w"); if (f) { fputs("id\tname\tat\tkind\tspec\n", f); fclose(f); }
    } else LOG("【红】live hooks: cannot write %s", g_out_path);
    pthread_t th; pthread_create(&th, NULL, collector, NULL); pthread_detach(th);
    pthread_mutex_unlock(&g_mu);
    LOG("live hooks: on for %s (code %#llx, arena %#llx, output %s)", name, (unsigned long long)lo, (unsigned long long)(uint64_t)g_arena, g_out_path);
    return 0;
}

// ---- the spec: at=main+0x… kind=entry|insn|ret cap=src,src filt=src=value|src&mask max=N every=N rate=N name=text ----
static int parse_src(const char *s, lv_src *o) {
    memset(o, 0, sizeof *o); int n = 0; while (s[n] == '[') n++;
    const char *p = s + n;
    if (!strncmp(p, "sp", 2)) { o->reg = 31; p += 2; } else if (!strncmp(p, "lr", 2)) { o->reg = 30; p += 2; }
    else if (*p == 'x') { char *e; long r = strtol(p + 1, &e, 10); if (e == p + 1 || r < 0 || r > 30) return 0; o->reg = (int8_t)r; p = e; } else return 0;
    for (int i = 0; i < n; i++) {
        long off = 0; if (*p == '+' || *p == '-') { char *e; off = strtol(p + 1, &e, 16); if (*p == '-') off = -off; p = e; }
        if (*p++ != ']') return 0;
        o->off[i] = (int32_t)off;
    }
    o->n = (uint8_t)n; o->size = 8;
    if (n) { if (*p++ != '.') return 0; switch (*p++) { case 'b': o->size = 1; break; case 'h': o->size = 2; break; case 'w': o->size = 4; break; case 'q': o->size = 8; break; default: return 0; } }
    return *p == 0 || *p == ',' || *p == ' ' || *p == '=' || *p == '&';
}
static uint64_t parse_addr(const char *v) {
    if (!strncmp(v, "main+", 5) || !strncmp(v, g_modname, strlen(g_modname))) { const char *q = strchr(v, '+'); return (uint64_t)g_base + strtoull(q + 1, NULL, 16); }
    return strtoull(v, NULL, 16);
}
static const char *kv(const char *spec, const char *key, char *buf, size_t cap) {   // the value of key= in a space-separated spec
    size_t kl = strlen(key);
    for (const char *p = spec; p && *p; p = strchr(p, ' ') ? strchr(p, ' ') + 1 : NULL) {
        while (*p == ' ') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') { const char *e = strchr(p + kl + 1, ' '); size_t n = e ? (size_t)(e - (p + kl + 1)) : strlen(p + kl + 1); if (n >= cap) n = cap - 1; memcpy(buf, p + kl + 1, n); buf[n] = 0; return buf; }
    }
    return NULL;
}
int dm_live_add(const char *spec, char *err, size_t ecap) {
    char v[256];
    if (!g_base) { snprintf(err, ecap, "live hooks are not attached"); return -1; }
    if (!kv(spec, "at", v, sizeof v)) { snprintf(err, ecap, "missing at=main+0x…"); return -1; }
    uint64_t at = parse_addr(v);
    if (at < (uint64_t)g_base || at >= (uint64_t)g_base + g_text_end || (at & 3)) { snprintf(err, ecap, "%s is not an instruction of %s's code", v, g_modname); return -1; }
    uint32_t op = *(volatile uint32_t *)at;
    if ((op & 0x7C000000u) == 0x14000000u) {   // a b / bl the rewriter put there (a gate, an observation gate) points into the patch area
        uint64_t to = at + (uint64_t)((int64_t)sext(op & 0x3ffffff, 26) * 4);
        if (to >= (uint64_t)g_base + g_patch_off && to < (uint64_t)g_base + g_size) { snprintf(err, ecap, "%s was rewritten (it branches into the patch area)", v); return -1; }
    }
    lv_hook t; memset(&t, 0, sizeof t);
    t.kind = kv(spec, "kind", v, sizeof v) ? (!strcmp(v, "ret") ? LV_RET : !strcmp(v, "entry") ? LV_ENTRY : !strcmp(v, "insn") ? LV_INSN : -1) : LV_ENTRY;
    if (t.kind < 0) { snprintf(err, ecap, "kind is entry, insn or ret"); return -1; }
    if (kv(spec, "cap", v, sizeof v)) for (char *p = v; *p && t.ncap < 4;) {
        if (!parse_src(p, &t.cap[t.ncap])) { snprintf(err, ecap, "cap %s: x<r>, sp, lr or [x<r>+0x…].b|h|w|q (nested [[…]+0x…])", p); return -1; }
        t.ncap++; char *c = strchr(p, ','); if (!c) break; p = c + 1;
    }
    if (kv(spec, "filt", v, sizeof v)) {
        if (!parse_src(v, &t.fsrc)) { snprintf(err, ecap, "filt %s: <source>=<hex> or <source>&<hex>", v); return -1; }
        char *o = strpbrk(v + 1, "=&");
        if (!o) { snprintf(err, ecap, "filt %s: no = or &", v); return -1; }
        t.filt = 1; t.fop = *o; t.fval = parse_addr(o + 1);
    }
    if (kv(spec, "max", v, sizeof v)) t.max = strtoull(v, NULL, 10);
    if (kv(spec, "every", v, sizeof v)) t.every = strtoull(v, NULL, 10);
    t.rate = kv(spec, "rate", v, sizeof v) ? strtoull(v, NULL, 10) : 2000000;   // default: a hook over 2 million hits a second disarms itself; rate=0 = no limit
    if (kv(spec, "name", v, sizeof v)) strlcpy(t.name, v, sizeof t.name); else snprintf(t.name, sizeof t.name, "%s+%#llx", g_modname, (unsigned long long)(at - (uint64_t)g_base));
    strlcpy(t.spec, spec, sizeof t.spec);
    pthread_mutex_lock(&g_mu);
    int id = atomic_load(&g_nhooks);
    for (int i = 0; i < id; i++) if (g_hooks[i].site == at && atomic_load(&g_hooks[i].armed)) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "hook %d is already armed there", i); return -1; }
    if (id >= LV_MAX) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "too many hooks (%d)", LV_MAX); return -1; }
    uint32_t *tr = arena_take(); if (!tr) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "the trampoline arena is full"); return -1; }
    lv_hook *h = &g_hooks[id]; *h = t; h->id = id; h->site = at; h->orig = op; h->tramp = tr;
    tb b; memset(&b, 0, sizeof b); b.pc = (uint64_t)tr;
    emit_save(&b); emit_call(&b, (uint64_t)lv_hit, 1, (uint64_t)h); emit_restore(&b);
    if (!relocate(&b, at, op, err, ecap) || !finish(&b)) { g_arena_used -= LV_TRAMP_MAX; pthread_mutex_unlock(&g_mu); if (!*err) snprintf(err, ecap, "the trampoline does not fit"); return -1; }
    if (!b_fits(at, (uint64_t)tr)) { g_arena_used -= LV_TRAMP_MAX; pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "the arena is out of branch range"); return -1; }
    arena_put(tr, &b);
    atomic_store(&g_nhooks, id + 1);
    atomic_store(&h->armed, 1); put_insn(at, b_to(at, (uint64_t)tr));
    pthread_mutex_unlock(&g_mu);
    if (g_out) { char p[1100]; snprintf(p, sizeof p, "%s.hooks.tsv", g_out_path); FILE *f = fopen(p, "a");
        if (f) { fprintf(f, "%d\t%s\t%s+%#llx\t%s\t%s\n", id, h->name, g_modname, (unsigned long long)(at - (uint64_t)g_base), h->kind == LV_RET ? "ret" : h->kind == LV_ENTRY ? "entry" : "insn", spec); fclose(f); } }
    return id;
}
int dm_live_del(int id) {
    int n = atomic_load(&g_nhooks);
    if (id < 0) { for (int i = 0; i < n; i++) disarm(&g_hooks[i]); return n; }
    if (id >= n) return -1;
    disarm(&g_hooks[id]); return 1;
}
int dm_live_counts_of(int id, dm_live_counts *o) {
    if (id < 0 || id >= atomic_load(&g_nhooks)) return 0;
    lv_hook *h = &g_hooks[id];
    *o = (dm_live_counts){atomic_load(&h->hits), atomic_load(&h->matched), atomic_load(&h->recorded), atomic_load(&h->dropped), atomic_load(&h->armed), h->disarmed_for_rate};
    return 1;
}
uint64_t dm_live_tramp(int id) { return id >= 0 && id < atomic_load(&g_nhooks) ? (uint64_t)g_hooks[id].tramp : 0; }

// ---- the collector ----
void dm_live_flush(void) {
    pthread_mutex_lock(&g_drain_mu);
    for (lv_ring *r = atomic_load(&g_rings); r; r = r->next) {
        uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed), head = atomic_load_explicit(&r->head, memory_order_acquire);
        while (tail < head) {
            uint64_t i = tail & (LV_RING - 1), n = head - tail; if (n > LV_RING - i) n = LV_RING - i;
            if (g_out) fwrite(&r->rec[i], sizeof(dm_live_rec), n, g_out);
            g_written += n; tail += n;
        }
        atomic_store_explicit(&r->tail, tail, memory_order_release);
    }
    if (g_out) fflush(g_out);
    pthread_mutex_unlock(&g_drain_mu);
}
static void *collector(void *u) {
    (void)u; pthread_setname_np("dm-live-collect");
    static int told[LV_MAX / 32];
    for (;;) {
        usleep(20 * 1000);
        dm_live_flush();
        int n = atomic_load(&g_nhooks);
        for (int i = 0; i < n; i++) if (g_hooks[i].disarmed_for_rate && !(told[i / 32] & (1 << (i % 32)))) {
            told[i / 32] |= 1 << (i % 32);
            LOG("live hooks: hook %d (%s) disarmed itself: over %llu hits a second", i, g_hooks[i].name, (unsigned long long)g_hooks[i].rate);
        }
    }
    return NULL;
}

// ---- boot and the control port ----
static int live_on(void) { const char *e = getenv("DM_LIVE"); return e && !strcmp(e, "1"); }
static int attach_main(char *err, size_t ecap) {
    if (g_base) return 1;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        return dm_live_attach(g_mod[i].base, g_mod[i].size, g_mod[i].text_end, g_mod[i].patch_off, "main") == 0 ? 1 : (snprintf(err, ecap, "live hooks could not attach to main (see the log)"), 0);
    snprintf(err, ecap, "module main is not loaded yet"); return 0;
}
// DM_LIVE_HOOKS="<spec>;<spec>…", or "@<file>" with one spec a line (# comments): armed before the game's threads start. Each armed hook is
// logged while there are 50 or fewer; past that only the count (the call-trace mode arms thousands)
void dm_live_boot(void) {
    const char *e = getenv("DM_LIVE_HOOKS"); if (!live_on() || !e || !*e) return;
    char err[256] = ""; if (!attach_main(err, sizeof err)) { LOG("【红】live hooks: %s", err); return; }
    char *s = NULL;
    if (e[0] == '@') { FILE *f = fopen(e + 1, "r"); if (!f) { LOG("【红】live hooks: cannot read %s", e + 1); return; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); s = calloc((size_t)n + 1, 1); if (fread(s, 1, (size_t)n, f) != (size_t)n) n = 0; s[n] = 0; fclose(f);
        for (char *c = s; *c; c++) if (*c == '\n') *c = ';'; }
    else s = strdup(e);
    int armed = 0, refused = 0;
    for (char *p = strtok(s, ";"); p; p = strtok(NULL, ";")) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        err[0] = 0; int id = dm_live_add(p, err, sizeof err);
        if (id < 0) { if (refused++ < 50) LOG("live hooks: refused %s: %s", p, err); }   // a long list (the call-trace mode) meets refusals by design: the count says how many
        else if (armed++ < 50) LOG("live hooks: %d armed: %s", id, p);
    }
    LOG("live hooks: %d armed at boot, %d refused", armed, refused);
    free(s);
}
#define ERR(...) do { snprintf(err, ecap, __VA_ARGS__); return 0; } while (0)
// live do=add at=main+0x… [kind=entry|insn|ret] [cap=…] [filt=…] [max=N] [every=N] [rate=N] [name=…] → hook (its number; id is the control port's own)
// live do=del hook=N|all; live do=list [from=N]; live do=stats; live do=flush
static int cmd_live(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    if (!live_on()) ERR("live hooks need DM_LIVE=1 (Mac development runs)");
    if (!attach_main(err, ecap)) return 0;
    const char *d = dm_ctl_get(a, "do"); if (!d) d = "add";
    if (!strcmp(d, "add")) {
        char spec[1024] = ""; static const char *const keys[] = {"at", "kind", "cap", "filt", "max", "every", "rate", "name"};
        for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) { const char *v = dm_ctl_get(a, keys[i]); if (v) { size_t l = strlen(spec); snprintf(spec + l, sizeof spec - l, "%s%s=%s", l ? " " : "", keys[i], v); } }
        int id = dm_live_add(spec, err, ecap); if (id < 0) return 0;
        snprintf(out, cap, ",\"hook\":%d,\"name\":\"%s\"", id, g_hooks[id].name); LOG("live hooks: %d armed: %s", id, spec); return 1;
    }
    if (!strcmp(d, "del")) {
        const char *i = dm_ctl_get(a, "hook"); if (!i) ERR("missing hook (a number or all)");
        int n = dm_live_del(!strcmp(i, "all") ? -1 : atoi(i)); if (n < 0) ERR("no hook %s", i);
        snprintf(out, cap, ",\"disarmed\":%d", n); return 1;
    }
    if (!strcmp(d, "flush")) { dm_live_flush(); snprintf(out, cap, ",\"written\":%llu", (unsigned long long)g_written); return 1; }
    if (!strcmp(d, "stats") || !strcmp(d, "list")) {
        int n = atomic_load(&g_nhooks), from = (int)dm_ctl_num(a, "from", 0); uint64_t hits = 0, rec = 0, drop = 0; int armed = 0, rings = 0;
        for (int i = 0; i < n; i++) { hits += atomic_load(&g_hooks[i].hits); rec += atomic_load(&g_hooks[i].recorded); drop += atomic_load(&g_hooks[i].dropped); armed += atomic_load(&g_hooks[i].armed); }
        for (lv_ring *r = atomic_load(&g_rings); r; r = r->next) rings++;
        size_t o = (size_t)snprintf(out, cap, ",\"hooks\":%d,\"armed\":%d,\"hits\":%llu,\"recorded\":%llu,\"dropped\":%llu,\"written\":%llu,\"threads\":%d,\"out\":\"%s\"",
                                    n, armed, (unsigned long long)hits, (unsigned long long)rec, (unsigned long long)drop, (unsigned long long)g_written, rings, g_out_path);
        if (!strcmp(d, "list")) {
            o += (size_t)snprintf(out + o, cap - o, ",\"list\":[");
            for (int i = from < 0 ? 0 : from, k = 0; i < n && o + 200 < cap; i++, k++) { lv_hook *h = &g_hooks[i];
                o += (size_t)snprintf(out + o, cap - o, "%s{\"hook\":%d,\"name\":\"%s\",\"armed\":%d,\"hits\":%llu,\"matched\":%llu,\"recorded\":%llu,\"dropped\":%llu%s}", k ? "," : "", i, h->name,
                                      atomic_load(&h->armed), (unsigned long long)atomic_load(&h->hits), (unsigned long long)atomic_load(&h->matched), (unsigned long long)atomic_load(&h->recorded),
                                      (unsigned long long)atomic_load(&h->dropped), h->disarmed_for_rate ? ",\"rate_disarmed\":1" : ""); }
            if (o + 2 < cap) { out[o++] = ']'; out[o] = 0; }
        }
        return 1;
    }
    ERR("do is add, del, list, stats or flush");
}
void dm_live_ctl_init(void) { dm_ctl_register("live", cmd_live); }
#endif
