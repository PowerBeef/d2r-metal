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
void dm_live_thread_start(void) {}
#else
#include "dm.h"
#include "dm_ctl.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_status.h>
#include <sys/ucontext.h>
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
#define LV_FRAME 896            // trampoline frame: x0-x30, nzcv, q0-q31 (768 bytes) and 128 for the red zone below the caller's sp
#define LV_ARENA (32u << 20)    // trampoline arena, within branch range of the whole module
#define LV_TRAMP_MAX 448        // bytes reserved per trampoline: about 80 instructions and a literal pool
enum { LV_INSN, LV_ENTRY, LV_RET };

typedef struct { int8_t reg; uint8_t n, size; int32_t off[4]; } lv_src;   // n = 0: the register; else n levels of [value + off], the last read with size bytes
typedef struct {
    _Atomic uint64_t hits, matched, recorded, dropped, win_t, win_hits;
    _Atomic int armed; int disarmed_for_rate;
    uint64_t site; uint32_t orig; uint32_t *tramp;
    int id, kind, ncap, filt, norec, strcap; char fop, ftext[32];   // strcap: cap=str:<source> = 32 bytes of the text it points at, in the four capture slots
    lv_src cap[4], fsrc; uint64_t fval, max, every, rate;
    char name[48], spec[256];
} lv_hook;
typedef struct { uint64_t lr, sp, t; lv_hook *h; } lv_ss;
typedef struct lv_ring { _Atomic uint64_t head, tail; uint64_t dropped; uint32_t tid; int ssn; uint64_t w_pc, w_far, w_lr, w_wr, w_x[4]; lv_ss ss[LV_SS]; struct lv_ring *next; dm_live_rec rec[LV_RING]; } lv_ring;

static uint8_t *g_base, *g_text_rw, *g_arena, *g_arena_rw; static size_t g_size, g_rx_len, g_arena_used; static uint64_t g_text_end, g_patch_off;
static char g_modname[16];
static lv_hook *g_hooks; static _Atomic int g_nhooks;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_drain_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_key; static lv_ring *_Atomic g_rings;
static uint32_t *g_ret_tramp;
static uint8_t *g_arena_pre;   // the loader's reservation after main (dm_live_set_arena)
static FILE *g_out, *g_hooks_f; static char g_out_path[1024]; static uint64_t g_written;
#define LV_SITES (1u << 18)
static int32_t *g_site_ix;   // open addressing: hook id + 1 by site, so arming is not a scan and a site armed again reuses its hook and trampoline
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
    if (!flag && h->strcap) {   // a short text (a name, a path): up to 32 bytes, as far as they can be read
        uint64_t p = 0; memset(x->cap, 0, sizeof x->cap);
        if (src_eval(&h->cap[0], ctx, &p) && p) for (mach_vm_size_t n = 32, got; n; n /= 2) if (mach_vm_read_overwrite(mach_task_self(), p, n, (mach_vm_address_t)x->cap, &got) == KERN_SUCCESS) break;
    } else for (int i = 0; i < 4; i++) { uint64_t v = 0; if (!flag && i < h->ncap) src_eval(&h->cap[i], ctx, &v); x->cap[i] = v; }
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
    if (h->filt) { uint64_t v; if (!src_eval(&h->fsrc, ctx, &v)) return;
        if (h->fop == '~') {   // the text the value points at contains ftext (the first 96 bytes of it)
            char t[97]; mach_vm_size_t got = 0; memset(t, 0, sizeof t);
            for (mach_vm_size_t n = 96; n >= 12; n /= 2) if (mach_vm_read_overwrite(mach_task_self(), v, n, (mach_vm_address_t)t, &got) == KERN_SUCCESS) break;
            if (!got || !strstr(t, h->ftext)) return;
        } else if (h->fop == '=' ? v != h->fval : !(v & h->fval)) return; }
    uint64_t m = atomic_fetch_add_explicit(&h->matched, 1, memory_order_relaxed);   // 0-based among the matching hits
    if (h->norec) return;   // count=1: the counters only (the call-trace census)
    if (h->every > 1 && m % h->every) return;
    if (h->max && (h->every > 1 ? m / h->every : m) >= h->max) return;
    lv_ring *r = ring_get(); uint64_t t = ticks();
    if (ring_put(r, h, ctx, t, 0, ctx[30], 0)) atomic_fetch_add_explicit(&h->recorded, 1, memory_order_relaxed); else atomic_fetch_add_explicit(&h->dropped, 1, memory_order_relaxed);
    if (h->kind == LV_RET && r->ssn < LV_SS) { r->ss[r->ssn++] = (lv_ss){ctx[30], (uint64_t)ctx + LV_FRAME, t, h}; ctx[30] = (uint64_t)g_ret_tramp; }
}
__attribute__((used)) static void lv_ret(uint64_t *ctx) {
    lv_ring *r = ring_get(); uint64_t sp = (uint64_t)ctx + LV_FRAME;
    while (r->ssn > 0 && r->ss[r->ssn - 1].sp < sp) r->ssn--;   // deeper frames left without returning here (a longjmp): drop them
    if (!r->ssn || r->ss[r->ssn - 1].sp != sp) { LOG("【红】live hooks: a return came back that the shadow stack does not hold"); abort(); }   // no way back to the caller: a ret hook not on a function's first instruction, or a stack switched under it
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
    for (int k = 0; k < 4; k++) { int q = 8 + 2 * k; emit(b, 0xAD000000u | ((uint32_t)(40 + 2 * k) << 15) | ((uint32_t)(q + 1) << 10) | (31 << 5) | (uint32_t)q); }    // stp q8-q15 at 640: a callee keeps only their low halves
}
static void emit_restore(tb *b) {
    for (int k = 0; k < 4; k++) { int q = 8 + 2 * k; emit(b, 0xAD400000u | ((uint32_t)(40 + 2 * k) << 15) | ((uint32_t)(q + 1) << 10) | (31 << 5) | (uint32_t)q); }
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
    if ((op & 0x3F800000u) == 0x08000000u) { snprintf(err, ecap, "an exclusive load / store (its loop would never succeed through a hook)"); return 0; }
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
int dm_live_trap(ucontext_t *uc);
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
    g_site_ix = mmap(NULL, sizeof(int32_t) * LV_SITES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_base = base; g_size = size; g_text_end = text_end; g_patch_off = patch_off; strlcpy(g_modname, name, sizeof g_modname);
    { tb b = {.pc = 0}; uint32_t *at = arena_take(); b.pc = (uint64_t)at;   // the shared return trampoline
      emit_save(&b); emit_call(&b, (uint64_t)lv_ret, 0, 0); emit_restore(&b); emit(&b, 0xD65F03C0u); finish(&b); arena_put(at, &b); g_ret_tramp = at; }
    const char *o = getenv("DM_LIVE_OUT"); strlcpy(g_out_path, o && *o ? o : "live.bin", sizeof g_out_path);
    if ((g_out = fopen(g_out_path, "wb"))) {
        dm_live_hdr h = {{'D', 'M', 'L', 'I', 'V', 'E', '0', '1'}, (uint64_t)base, size, g_timer_hz, sizeof(dm_live_rec), 1};
        fwrite(&h, sizeof h, 1, g_out); fflush(g_out);
        char p[1100]; snprintf(p, sizeof p, "%s.hooks.tsv", g_out_path); if ((g_hooks_f = fopen(p, "w"))) fputs("id\tname\tat\tkind\tspec\n", g_hooks_f);
    } else LOG("【红】live hooks: cannot write %s", g_out_path);
    { void crash_set_trap_hook(int (*fn)(ucontext_t *uc)); crash_set_trap_hook(dm_live_trap); }
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
        if (!strncmp(p, "str:", 4)) { if (t.ncap || !parse_src(p + 4, &t.cap[0])) { snprintf(err, ecap, "cap str:<source> stands alone (it fills the four slots)"); return -1; } t.strcap = 1; t.ncap = 1; break; }
        if (!parse_src(p, &t.cap[t.ncap])) { snprintf(err, ecap, "cap %s: x<r>, sp, lr or [x<r>+0x…].b|h|w|q (nested [[…]+0x…]), or str:<source>", p); return -1; }
        t.ncap++; char *c = strchr(p, ','); if (!c) break; p = c + 1;
    }
    if (kv(spec, "filt", v, sizeof v)) {
        char *o = strpbrk(v + 1, "=&~"), keep = o ? *o : 0;
        if (!o) { snprintf(err, ecap, "filt %s: <source>=<hex>, <source>&<hex> or <source>~<text>", v); return -1; }
        *o = 0; if (!parse_src(v, &t.fsrc)) { snprintf(err, ecap, "filt %s: the source is x<r> or [x<r>+0x…].b|h|w|q", v); return -1; }
        t.filt = 1; t.fop = keep;
        if (keep == '~') { if (!o[1]) { snprintf(err, ecap, "filt: ~ needs a text"); return -1; } strlcpy(t.ftext, o + 1, sizeof t.ftext); } else t.fval = parse_addr(o + 1);
    }
    if (kv(spec, "max", v, sizeof v)) t.max = strtoull(v, NULL, 10);
    if (kv(spec, "every", v, sizeof v)) t.every = strtoull(v, NULL, 10);
    if (kv(spec, "count", v, sizeof v)) t.norec = atoi(v) != 0;
    if (t.norec && t.kind == LV_RET) { snprintf(err, ecap, "count=1 records nothing, so it has no returns: use kind=entry"); return -1; }
    t.rate = kv(spec, "rate", v, sizeof v) ? strtoull(v, NULL, 10) : 2000000;   // default: a hook over 2 million hits a second disarms itself; rate=0 = no limit
    if (kv(spec, "name", v, sizeof v)) strlcpy(t.name, v, sizeof t.name); else snprintf(t.name, sizeof t.name, "%s+%#llx", g_modname, (unsigned long long)(at - (uint64_t)g_base));
    strlcpy(t.spec, spec, sizeof t.spec);
    // an exclusive pair open before this instruction: a hook between the load and the store would make the store fail every time
    for (uint64_t q = at - 4, k = 0; k < 12 && q >= (uint64_t)g_base; k++, q -= 4) { uint32_t w = *(volatile uint32_t *)q;
        if ((w & 0x3FC00000u) == 0x08000000u || (w & 0xFC000000u) == 0x14000000u || (w & 0xFFFFFC1Fu) == 0xD65F0000u || (w & 0xFFFFFC1Fu) == 0xD61F0000u) break;   // a store-exclusive closes it; b, ret, br: other code
        if ((w & 0x3FC00000u) == 0x08400000u) { snprintf(err, ecap, "%s+%#llx is inside an exclusive load / store sequence", g_modname, (unsigned long long)(at - (uint64_t)g_base)); return -1; } }
    pthread_mutex_lock(&g_mu);
    uint32_t slot = (uint32_t)(((at - (uint64_t)g_base) >> 2) * 2654435761u) & (LV_SITES - 1); int old = -1;
    for (; g_site_ix[slot]; slot = (slot + 1) & (LV_SITES - 1)) if (g_hooks[g_site_ix[slot] - 1].site == at) { old = g_site_ix[slot] - 1; break; }
    lv_hook *h; int id;
    if (old >= 0) {   // armed here before: the same hook and trampoline, with the new settings and fresh counters
        h = &g_hooks[id = old];
        if (atomic_load(&h->armed)) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "hook %d is already armed there", old); return -1; }
        if (op != h->orig) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "the instruction there changed since hook %d (a coverage trap?)", old); return -1; }
    } else {
        id = atomic_load(&g_nhooks);
        if (id >= LV_MAX) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "too many hooks (%d)", LV_MAX); return -1; }
        uint32_t *tr = arena_take(); if (!tr) { pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "the trampoline arena is full"); return -1; }
        h = &g_hooks[id]; memset(h, 0, sizeof *h); h->id = id; h->site = at; h->orig = op; h->tramp = tr;
        tb b; memset(&b, 0, sizeof b); b.pc = (uint64_t)tr;
        emit_save(&b); emit_call(&b, (uint64_t)lv_hit, 1, (uint64_t)h); emit_restore(&b);
        if (!relocate(&b, at, op, err, ecap) || !finish(&b)) { g_arena_used -= LV_TRAMP_MAX; pthread_mutex_unlock(&g_mu); if (!*err) snprintf(err, ecap, "the trampoline does not fit"); return -1; }
        if (!b_fits(at, (uint64_t)tr)) { g_arena_used -= LV_TRAMP_MAX; pthread_mutex_unlock(&g_mu); snprintf(err, ecap, "the arena is out of branch range"); return -1; }
        arena_put(tr, &b);
        g_site_ix[slot] = id + 1;
        atomic_store(&g_nhooks, id + 1);
    }
    h->kind = t.kind; h->ncap = t.ncap; h->filt = t.filt; h->norec = t.norec; h->strcap = t.strcap; memcpy(h->ftext, t.ftext, sizeof h->ftext); h->fop = t.fop; memcpy(h->cap, t.cap, sizeof h->cap); h->fsrc = t.fsrc;
    h->fval = t.fval; h->max = t.max; h->every = t.every; h->rate = t.rate; h->disarmed_for_rate = 0; memcpy(h->name, t.name, sizeof h->name); memcpy(h->spec, t.spec, sizeof h->spec);
    atomic_store(&h->hits, 0); atomic_store(&h->matched, 0); atomic_store(&h->recorded, 0); atomic_store(&h->dropped, 0); atomic_store(&h->win_t, 0); atomic_store(&h->win_hits, 0);
    atomic_store(&h->armed, 1); put_insn(at, b_to(at, (uint64_t)h->tramp));
    if (g_hooks_f) fprintf(g_hooks_f, "%d\t%s\t%s+%#llx\t%s\t%s\n", id, h->name, g_modname, (unsigned long long)(at - (uint64_t)g_base), h->kind == LV_RET ? "ret" : h->kind == LV_ENTRY ? "entry" : "insn", spec);
    pthread_mutex_unlock(&g_mu);
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
    if (g_hooks_f) fflush(g_hooks_f);
    pthread_mutex_unlock(&g_drain_mu);
}
static void *collector(void *u) {
    (void)u; pthread_setname_np("dm-live-collect");
    for (;;) {
        usleep(20 * 1000);
        dm_live_flush();
        int n = atomic_load(&g_nhooks);
        for (int i = 0; i < n; i++) if (g_hooks[i].disarmed_for_rate == 1) {
            g_hooks[i].disarmed_for_rate = 2;   // told
            LOG("live hooks: hook %d (%s) disarmed itself: over %llu hits a second", i, g_hooks[i].name, (unsigned long long)g_hooks[i].rate);
        }
    }
    return NULL;
}

// ---- files beside the output, and many specs at once ----
static void beside_out(const char *name, char *p, size_t n) {   // a bare file name lands beside DM_LIVE_OUT
    if (strchr(name, '/')) { strlcpy(p, name, n); return; }
    strlcpy(p, g_out_path, n); char *sl = strrchr(p, '/'); if (sl) sl[1] = 0; else p[0] = 0; strlcat(p, name, n);
}
static char *slurp_text(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *s = calloc((size_t)n + 1, 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); fclose(f); return NULL; }
    fclose(f); return s;
}
// One spec a line or between semicolons; # starts a comment line. The first 50 armed and refused are logged, then the counts
static void load_specs(char *s, int *armed_out, int *refused_out) {
    int armed = 0, refused = 0; char err[256], *save = NULL;
    for (char *c = s; *c; c++) if (*c == '#' && (c == s || c[-1] == '\n')) while (*c && *c != '\n') *c++ = ' ';   // comment lines, whatever they hold
    for (char *p = strtok_r(s, ";\n", &save); p; p = strtok_r(NULL, ";\n", &save)) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        err[0] = 0; int id = dm_live_add(p, err, sizeof err);
        if (id < 0) { if (refused++ < 50) LOG("live hooks: refused %s: %s", p, err); }   // a long list (the call-trace mode) meets refusals by design
        else if (armed++ < 50) LOG("live hooks: %d armed: %s", id, p);
    }
    LOG("live hooks: %d armed, %d refused", armed, refused);
    if (armed_out) *armed_out = armed; if (refused_out) *refused_out = refused;
}
static int counts_dump(const char *path) {   // every hook's counters as a table (the call-trace census: too many for a reply)
    FILE *f = fopen(path, "w"); if (!f) return -1;
    fputs("hook\tat\tname\thits\tmatched\trecorded\tarmed\n", f); int n = atomic_load(&g_nhooks);
    for (int i = 0; i < n; i++) { lv_hook *h = &g_hooks[i];
        fprintf(f, "%d\t%s+%#llx\t%s\t%llu\t%llu\t%llu\t%d\n", i, g_modname, (unsigned long long)(h->site - (uint64_t)g_base), h->name, (unsigned long long)atomic_load(&h->hits),
                (unsigned long long)atomic_load(&h->matched), (unsigned long long)atomic_load(&h->recorded), atomic_load(&h->armed)); }
    fclose(f); return n;
}

// ---- block coverage: a one-shot trap on each listed instruction (a basic block's first) ----
// The trap's handler puts the original instruction back and lets it run, so a block costs one trap the first time it runs and nothing after.
// Any instruction can carry one (it runs in place afterwards). The list is offsets into the module, hex, one a line (live.py blocks).
#define COV_BRK 0xD43E0020u   // brk #0xf001 (DM_PROBE's is #0xf000)
static uint32_t *g_cov_off, *g_cov_orig; static _Atomic uint32_t *g_cov_seq; static int g_cov_n; static _Atomic uint32_t g_cov_next; static _Atomic int g_cov_on;
static int cov_cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int cov_arm(const char *path, int *skipped_out, char *err, size_t ecap) {
    if (atomic_load(&g_cov_on)) { snprintf(err, ecap, "coverage is already on (cover_off first)"); return -1; }
    char *s = slurp_text(path); if (!s) { snprintf(err, ecap, "cannot read %s", path); return -1; }
    int cap = 1 << 16, n = 0; uint32_t *off = malloc((size_t)cap * 4);
    for (char *p = s; *p;) { char *e; unsigned long v = strtoul(p, &e, 16);
        if (e != p && !(v & 3) && v < g_text_end) { if (n == cap) off = realloc(off, (size_t)(cap *= 2) * 4); off[n++] = (uint32_t)v; }
        p = strchr(e, '\n'); if (!p) break; p++; }
    free(s); if (!n) { free(off); snprintf(err, ecap, "no block offsets in %s", path); return -1; }
    qsort(off, (size_t)n, 4, cov_cmp); int m = 0; for (int i = 0; i < n; i++) if (!m || off[i] != off[m - 1]) off[m++] = off[i];
    n = m; uint32_t *orig = calloc((size_t)n, 4); _Atomic uint32_t *seq = calloc((size_t)n, sizeof *seq); int skipped = 0;
    pthread_mutex_lock(&g_mu);
    g_cov_off = off; g_cov_orig = orig; g_cov_seq = seq; g_cov_n = n; atomic_store(&g_cov_next, 0); atomic_store(&g_cov_on, 1);   // the table is live before the first trap can come
    for (int i = 0; i < n; i++) { uint64_t at = (uint64_t)g_base + off[i]; uint32_t op = *(volatile uint32_t *)at;
        int hooked = (op & 0xFC000000u) == 0x14000000u && at + (uint64_t)((int64_t)sext(op & 0x3ffffff, 26) * 4) - (uint64_t)g_arena < LV_ARENA;
        if (hooked || (op & 0xFFE0001Fu) == 0xD4200000u) { skipped++; continue; }   // a live hook's branch or someone's brk: left alone (orig stays 0)
        orig[i] = op; __atomic_store_n((uint32_t *)(g_text_rw + off[i]), COV_BRK, __ATOMIC_RELEASE); }
    sys_icache_invalidate(g_base, g_text_end);
    pthread_mutex_unlock(&g_mu);
    if (skipped_out) *skipped_out = skipped;
    return n - skipped;
}
static int cov_find(uint32_t off) { int lo = 0, hi = g_cov_n - 1; while (lo <= hi) { int mid = (lo + hi) / 2; if (g_cov_off[mid] == off) return mid; if (g_cov_off[mid] < off) lo = mid + 1; else hi = mid - 1; } return -1; }
static int cov_dump(const char *path) {   // the blocks that ran: offset and the order they first ran in
    FILE *f = fopen(path, "w"); if (!f) return -1; int hit = 0;
    for (int i = 0; i < g_cov_n; i++) { uint32_t q = atomic_load(&g_cov_seq[i]); if (q) { fprintf(f, "%x\t%u\n", g_cov_off[i], q); hit++; } }
    fclose(f); return hit;
}
static int cov_rearm(void) {   // trap again the blocks that ran, for a second pass (what runs only during the next action)
    int k = 0; pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_cov_n; i++) if (g_cov_orig[i] && atomic_exchange(&g_cov_seq[i], 0)) { put_insn((uint64_t)g_base + g_cov_off[i], COV_BRK); k++; }
    atomic_store(&g_cov_next, 0); pthread_mutex_unlock(&g_mu); return k;
}
static void cov_off(void) {   // every original instruction back; the table stays for a late trap on another thread
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_cov_n; i++) if (g_cov_orig[i]) __atomic_store_n((uint32_t *)(g_text_rw + g_cov_off[i]), g_cov_orig[i], __ATOMIC_RELEASE);
    if (g_cov_n) sys_icache_invalidate(g_base, g_text_end);
    atomic_store(&g_cov_on, 0); pthread_mutex_unlock(&g_mu);
}

// ---- data watchpoints: who writes these bytes ----
// The processor's own debug registers, set by the process on its threads (thread_set_state, no debugger): a write to the watched bytes traps
// before it happens. The handler notes the writer, lets that one instruction run with the watch off and a breakpoint on the next instruction,
// and there records the write (writer, address, the value now there) and turns the watch back on. New game threads get the watches when they
// start (dm_live_thread_start). Up to 4 at once, each within one 8-byte word
#define LV_WATCH 4
static struct { uint64_t addr; int len, on, mode; _Atomic uint64_t hits; uint64_t max; } g_watch[LV_WATCH];   // mode: 1 reads, 2 writes, 3 both; past max hits a watch turns itself off (a field read every draw would stall the game)
static void debug_apply(thread_t th, int watches, uint64_t bp) {
    arm_debug_state64_t ds; memset(&ds, 0, sizeof ds);
    for (int i = 0; watches && i < LV_WATCH; i++) if (g_watch[i].on) {
        ds.__wvr[i] = g_watch[i].addr & ~7ull;
        ds.__wcr[i] = (uint64_t)(((1u << g_watch[i].len) - 1) << (g_watch[i].addr & 7)) << 5 | ((uint64_t)g_watch[i].mode << 3) | (2u << 1) | 1u;   // the bytes, loads / stores, user mode, on
    }
    if (bp) { ds.__bvr[0] = bp; ds.__bcr[0] = (0xfu << 5) | (2u << 1) | 1u; }
    thread_set_state(th, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
}
static void watch_apply_all(void) {
    thread_act_array_t th; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < n; i++) { debug_apply(th[i], 1, 0); mach_port_deallocate(mach_task_self(), th[i]); }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
}
void dm_live_thread_start(void) { for (int i = 0; i < LV_WATCH; i++) if (g_watch[i].on) { debug_apply(pthread_mach_thread_np(pthread_self()), 1, 0); return; } }
static int watch_add(uint64_t addr, int len, int mode, uint64_t max, char *err, size_t ecap) {
    if (len != 1 && len != 2 && len != 4 && len != 8) { snprintf(err, ecap, "len is 1, 2, 4 or 8"); return -1; }
    if ((addr & 7) + (uint64_t)len > 8) { snprintf(err, ecap, "the watched bytes must lie within one 8-byte word"); return -1; }
    for (int i = 0; i < LV_WATCH; i++) if (!g_watch[i].on) { g_watch[i].addr = addr; g_watch[i].len = len; g_watch[i].mode = mode; g_watch[i].max = max ? max : 20000; atomic_store(&g_watch[i].hits, 0); g_watch[i].on = 1; watch_apply_all(); return i; }
    snprintf(err, ecap, "all %d watchpoints are in use", LV_WATCH); return -1;
}
static void watch_del(int slot) { for (int i = 0; i < LV_WATCH; i++) if (slot < 0 || slot == i) g_watch[i].on = 0; watch_apply_all(); }

// ---- the trap handler (crash.c's SIGTRAP chain): coverage traps, watchpoints and their step breakpoints ----
int dm_live_trap(ucontext_t *uc) {
    uint64_t pc = uc->uc_mcontext->__ss.__pc; uint32_t ec = (uint32_t)(uc->uc_mcontext->__es.__esr >> 26);
    if (ec == 0x34 || ec == 0x35) {   // a watched write, not done yet
        int any = 0; for (int i = 0; i < LV_WATCH; i++) any |= g_watch[i].on;
        if (!any && !g_base) return 0;
        lv_ring *r = ring_get();
        r->w_wr = (uc->uc_mcontext->__es.__esr >> 6) & 1;
        r->w_pc = pc; r->w_far = uc->uc_mcontext->__es.__far; r->w_lr = uc->uc_mcontext->__ss.__lr; for (int i = 0; i < 4; i++) r->w_x[i] = uc->uc_mcontext->__ss.__x[i];
        debug_apply(pthread_mach_thread_np(pthread_self()), 0, pc + 4);
        return 1;
    }
    if (ec == 0x30 || ec == 0x31) {   // the breakpoint after a watched write
        lv_ring *r = g_base || g_watch[0].on || g_watch[1].on || g_watch[2].on || g_watch[3].on ? pthread_getspecific(g_key) : NULL;
        if (!r || !r->w_pc || pc != r->w_pc + 4) return 0;
        int slot = -1; for (int i = 0; i < LV_WATCH; i++) if (g_watch[i].on && (r->w_far & ~7ull) == (g_watch[i].addr & ~7ull)) { slot = i; break; }
        if (slot < 0) { r->w_pc = 0; debug_apply(pthread_mach_thread_np(pthread_self()), 1, 0); return 1; }   // its watch was turned off meanwhile: no record
        uint64_t v = 0; mach_vm_size_t got = 0; mach_vm_read_overwrite(mach_task_self(), g_watch[slot].addr, (mach_vm_size_t)g_watch[slot].len, (mach_vm_address_t)&v, &got);
        uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed), tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (head - tail < LV_RING) { dm_live_rec *x = &r->rec[head & (LV_RING - 1)];
            x->hook = DM_LIVE_WATCH | (uint32_t)slot; x->aux = r->w_wr;   // aux: 1 = this access was a write x->tid = r->tid; x->t = ticks(); x->lr = r->w_lr; x->sp = uc->uc_mcontext->__ss.__sp;
            for (int i = 0; i < 4; i++) x->x[i] = r->w_x[i];
            x->cap[0] = r->w_far; x->cap[1] = r->w_pc; x->cap[2] = v; x->cap[3] = 0;
            atomic_store_explicit(&r->head, head + 1, memory_order_release); }
        r->w_pc = 0;
        if (atomic_fetch_add(&g_watch[slot].hits, 1) + 1 >= g_watch[slot].max) g_watch[slot].on = 0;   // each other thread drops it at its next hit
        debug_apply(pthread_mach_thread_np(pthread_self()), 1, 0);
        return 1;
    }
    if (g_cov_n && g_base && pc - (uint64_t)g_base < g_text_end) {   // a coverage trap: the block's first run
        int i = cov_find((uint32_t)(pc - (uint64_t)g_base));
        if (i < 0 || !g_cov_orig[i]) return 0;
        uint32_t z = 0; if (atomic_compare_exchange_strong(&g_cov_seq[i], &z, atomic_fetch_add(&g_cov_next, 1) + 1)) put_insn(pc, g_cov_orig[i]);
        else if (*(volatile uint32_t *)pc == COV_BRK && !atomic_load(&g_cov_on)) put_insn(pc, g_cov_orig[i]);
        return 1;   // run it again, now the original (another thread may have put it back first)
    }
    return 0;
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
    char *s = e[0] == '@' ? slurp_text(e + 1) : strdup(e);
    if (!s) { LOG("【红】live hooks: cannot read %s", e + 1); return; }
    load_specs(s, NULL, NULL); free(s);
}
#define ERR(...) do { snprintf(err, ecap, __VA_ARGS__); return 0; } while (0)
// live do=add at=main+0x… [kind=entry|insn|ret] [cap=…] [filt=…] [max=N] [every=N] [rate=N] [name=…] → hook (its number; id is the control port's own)
// live do=del hook=N|all; live do=list [from=N]; live do=stats; live do=flush
static int cmd_live(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    if (!live_on()) ERR("live hooks need DM_LIVE=1 (Mac development runs)");
    if (!attach_main(err, ecap)) return 0;
    const char *d = dm_ctl_get(a, "do"); if (!d) d = "add";
    if (!strcmp(d, "add")) {
        char spec[1024] = ""; static const char *const keys[] = {"at", "kind", "cap", "filt", "max", "every", "rate", "count", "name", "type"};   // type: a hint for live.py (x0:Unit), carried in the hooks table
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
        for (int i = 0; i < LV_WATCH; i++) if (g_watch[i].addr) o += (size_t)snprintf(out + o, cap - o, ",\"watch%d\":\"%#llx %s %llu hits\"", i, (unsigned long long)g_watch[i].addr, g_watch[i].on ? "on" : "off", (unsigned long long)atomic_load(&g_watch[i].hits));
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
    const char *file = dm_ctl_get(a, "file"); char path[1100] = ""; if (file) beside_out(file, path, sizeof path);
    if (!strcmp(d, "load")) {   // many specs from a file (the call-trace mode: live.py trace-spec)
        if (!file) ERR("missing file");
        char *s = slurp_text(path); if (!s) ERR("cannot read %s", path);
        int armed = 0, refused = 0; load_specs(s, &armed, &refused); free(s);
        snprintf(out, cap, ",\"armed\":%d,\"refused\":%d", armed, refused); return 1;
    }
    if (!strcmp(d, "counts")) { if (!file) ERR("missing file"); int n = counts_dump(path); if (n < 0) ERR("cannot write %s", path); snprintf(out, cap, ",\"hooks\":%d,\"file\":\"%s\"", n, path); return 1; }
    if (!strcmp(d, "cover")) { if (!file) ERR("missing file (block offsets: live.py blocks)"); int sk = 0, n = cov_arm(path, &sk, err, ecap); if (n < 0) return 0;
        LOG("live hooks: coverage on, %d blocks trapped, %d left alone", n, sk); snprintf(out, cap, ",\"blocks\":%d,\"skipped\":%d", n, sk); return 1; }
    if (!strcmp(d, "cover_dump")) { if (!file) ERR("missing file"); if (!g_cov_n) ERR("coverage was never on"); int n = cov_dump(path); if (n < 0) ERR("cannot write %s", path);
        snprintf(out, cap, ",\"ran\":%d,\"blocks\":%d,\"file\":\"%s\"", n, g_cov_n, path); return 1; }
    if (!strcmp(d, "cover_rearm")) { if (!atomic_load(&g_cov_on)) ERR("coverage is not on"); snprintf(out, cap, ",\"rearmed\":%d", cov_rearm()); return 1; }
    if (!strcmp(d, "cover_off")) { cov_off(); LOG("live hooks: coverage off"); return 1; }
    if (!strcmp(d, "watch")) {   // at=main+0x…|0x… [chain=off,off] [len=1|2|4|8] [mode=write|read|rw]: chain as the mem command (read the pointer there, add the offset)
        const char *at = dm_ctl_get(a, "at"), *chain = dm_ctl_get(a, "chain"); if (!at) ERR("missing at");
        uint64_t p = parse_addr(at);
        for (const char *c = chain; c && *c; c = strchr(c, ',') ? strchr(c, ',') + 1 : NULL) { uint64_t q = 0; mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), p, 8, (mach_vm_address_t)&q, &got) != KERN_SUCCESS || got != 8 || !q) ERR("no pointer at %#llx", (unsigned long long)p);
            p = q + strtoull(c, NULL, 16); }
        const char *md = dm_ctl_get(a, "mode"); int mode = !md || !strcmp(md, "write") ? 2 : !strcmp(md, "read") ? 1 : !strcmp(md, "rw") ? 3 : 0;
        if (!mode) ERR("mode is write (default), read or rw");
        int slot = watch_add(p, (int)dm_ctl_num(a, "len", 8), mode, (uint64_t)dm_ctl_num(a, "max", 0), err, ecap); if (slot < 0) return 0;
        LOG("live hooks: watch %d on %#llx", slot, (unsigned long long)p); snprintf(out, cap, ",\"watch\":%d,\"addr\":\"%#llx\"", slot, (unsigned long long)p); return 1;
    }
    if (!strcmp(d, "unwatch")) { const char *w = dm_ctl_get(a, "watch"); watch_del(!w || !strcmp(w, "all") ? -1 : atoi(w)); return 1; }
    ERR("do is add, del, list, stats, flush, load, counts, cover, cover_dump, cover_rearm, cover_off, watch or unwatch");
}
void dm_live_ctl_init(void) { dm_ctl_register("live", cmd_live); }
#endif
