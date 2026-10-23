// 内存类 SVC（hle_svc.c）的并发与语义单测：直接包含真的 hle_svc.c，读得到映射表 g_map / g_nmap。
// MapMemory / UnmapMemory 在查表与登记之间的那次 memcpy 换成 t_memcpy：能让选定的线程停在那里，排出确定的交错。
//   det17   UnmapMemory 按过期下标删表项（F-CORE-17）：A 的 Unmap 停在 memcpy；B 另起线程 Unmap 别的栈；放开 A。期望活着的栈 C 仍在表里
//   det5    MapMemory 查表与登记不是一步（F-CORE-5）：A 的 Map 停在 memcpy；B 另起线程 Map 同一目标。期望只有一个成功、表里一项
//   disjoint R / overlap R   压力（线程消毒版 make tsan 跑）：8 线程各映射自己的槽 R 轮；2 线程同时映射同一目标 R 轮
// 不带参数 = 跑全部确定用例（make run）
// Concurrency and semantics unit test of the memory SVCs (hle_svc.c): includes the real hle_svc.c, so it can read the map registry g_map / g_nmap.
// The memcpy MapMemory / UnmapMemory do between looking up the registry and recording in it is routed through t_memcpy, which can park a chosen thread there for deterministic interleavings.
//   det17   UnmapMemory deletes its entry by a stale index (F-CORE-17): A's Unmap parks in memcpy; B unmaps another stack on its own thread; A is released. Expected: the live stack C is still registered
//   det5    MapMemory's lookup and record are not one step (F-CORE-5): A's Map parks in memcpy; B maps the same target on its own thread. Expected: exactly one success, one entry
//   disjoint R / overlap R   stress (run by the ThreadSanitizer build, make tsan): 8 threads map their own slot for R rounds; 2 threads map the same target at once for R rounds
// No argument = run all deterministic cases (make run)
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
static void *t_memcpy(void *d, const void *s, size_t n);
#undef memcpy
#define memcpy t_memcpy
#include "../hle_svc.c"
#undef memcpy
#include <stdatomic.h>

// ---- 宿主其余部分的桩 | stubs for the rest of the host ----
module_t g_mod[MAX_MOD]; int g_nmod;
struct dm_flags g_flag;
module_t *module_of(const void *p) { (void)p; return NULL; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
uint64_t svc_host_call(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void hlecount_svc(int num) { (void)num; }
uint64_t dm_entropy64(void) { return 4; }
void dm_backtrace(uint64_t fp) { (void)fp; }
uint64_t guest_ns(void) { return 0; }
static _Atomic long g_logs;
static void quiet_log(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; (void)fmt; atomic_fetch_add(&g_logs, 1); }

// macOS 没有 pthread_barrier_t：按代计数的小屏障 | macOS has no pthread_barrier_t: a small generation-counting barrier
typedef struct { pthread_mutex_t m; pthread_cond_t c; int n, waiting; unsigned gen; } barrier_t;
static void barrier_init(barrier_t *b, int n) { pthread_mutex_init(&b->m, NULL); pthread_cond_init(&b->c, NULL); b->n = n; b->waiting = 0; b->gen = 0; }
static void barrier_wait(barrier_t *b) {
    pthread_mutex_lock(&b->m); unsigned g = b->gen;
    if (++b->waiting == b->n) { b->waiting = 0; b->gen++; pthread_cond_broadcast(&b->c); }
    else while (g == b->gen) pthread_cond_wait(&b->c, &b->m);
    pthread_mutex_unlock(&b->m);
}

// ---- 停车钩子：选定线程的下一次 memcpy 停住，直到放行 | parking hook: the chosen thread's next memcpy parks until released ----
static pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t pc = PTHREAD_COND_INITIALIZER;
static pthread_t g_park_thr; static int g_park_armed, g_parked, g_go;
static void *t_memcpy(void *d, const void *s, size_t n) {
    pthread_mutex_lock(&pm);
    if (g_park_armed && pthread_equal(pthread_self(), g_park_thr)) {
        g_park_armed = 0; g_parked = 1; pthread_cond_broadcast(&pc);
        while (!g_go) pthread_cond_wait(&pc, &pm);
    }
    pthread_mutex_unlock(&pm);
    return __builtin_memcpy(d, s, n);
}
static void arm_self(void) { pthread_mutex_lock(&pm); g_park_thr = pthread_self(); g_park_armed = 1; g_parked = 0; g_go = 0; pthread_mutex_unlock(&pm); }
static void wait_parked(void) { pthread_mutex_lock(&pm); while (!g_parked) pthread_cond_wait(&pc, &pm); pthread_mutex_unlock(&pm); }
static void release(void) { pthread_mutex_lock(&pm); g_go = 1; pthread_cond_broadcast(&pc); pthread_mutex_unlock(&pm); }

static uint64_t svc(int num, uint64_t a0, uint64_t a1, uint64_t a2) { uint64_t r[32] = {a0, a1, a2}; svc_dispatch(num, r, 0); return r[0]; }
static uint32_t query_state(uint64_t addr) { uint64_t mi[8] = {0}; uint64_t r[32] = {(uint64_t)mi, 0, addr}; svc_dispatch(6, r, 0); return ((uint32_t *)mi)[4]; }
static void reset_map(void) { pthread_mutex_lock(&g_map_lock); g_nmap = 0; pthread_mutex_unlock(&g_map_lock); }

enum { NT = 8, SZ = 0x100000, SLOT = 0x200000 };
static int R = 2000, g_fail;
#define DST(i) ((uint64_t)g_stack + (uint64_t)(i) * SLOT)
static uint8_t *g_src[NT];
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

// 另起线程做一次 SVC（B 在锁上等时主线程不能跟着卡住） | one SVC on its own thread (the main thread must not block while B waits on the lock)
typedef struct { int num, park; uint64_t a0, a1, a2, r; } call_t;
static void *call_thr(void *p) { call_t *c = p; if (c->park) arm_self(); c->r = svc(c->num, c->a0, c->a1, c->a2); return NULL; }

static void det17(void) {
    reset_map();
    for (int i = 0; i < 3; i++) if (svc(4, DST(i), (uint64_t)g_src[i], SZ)) { CHECK(0, "det17 setup map %d", i); return; }
    // 表序：B = 槽0 在 [0]，C = 槽1 在 [1]，A = 槽2 在 [2]（A 在最后） | registry order: B = slot0 at [0], C = slot1 at [1], A = slot2 at [2] (A last)
    call_t a = {5, 1, DST(2), (uint64_t)g_src[2], SZ, ~0ull}, b = {5, 0, DST(0), (uint64_t)g_src[0], SZ, ~0ull};
    pthread_t ta, tb; pthread_create(&ta, NULL, call_thr, &a); wait_parked();
    pthread_create(&tb, NULL, call_thr, &b); usleep(100000);   // 修好后 B 在锁上等；修前 B 此刻已删完 | fixed: B waits on the lock; before the fix B has already deleted by now
    release(); pthread_join(ta, NULL); pthread_join(tb, NULL);
    uint32_t qc = query_state(DST(1)), qa = query_state(DST(2)), qb = query_state(DST(0));
    printf("det17: Unmap A %#llx, Unmap B %#llx, entries %d, Query C (live) %#x, Query A %#x, Query B %#x\n", a.r, b.r, g_nmap, qc, qa, qb);
    CHECK(a.r == 0 && b.r == 0, "det17: both unmaps should succeed");
    CHECK(g_nmap == 1 && qc == 0x0B && qa == 0 && qb == 0, "det17: the live stack C must stay registered and A / B must be gone (expect entries 1, C 0xb, A 0, B 0)");
    svc(5, DST(1), (uint64_t)g_src[1], SZ); reset_map();
}
static void det5(void) {
    reset_map();
    call_t a = {4, 1, DST(5), (uint64_t)g_src[0], SZ, ~0ull}, b = {4, 0, DST(5), (uint64_t)g_src[1], SZ, ~0ull};
    pthread_t ta, tb; pthread_create(&ta, NULL, call_thr, &a); wait_parked();
    pthread_create(&tb, NULL, call_thr, &b); usleep(100000);
    release(); pthread_join(ta, NULL); pthread_join(tb, NULL);
    printf("det5: Map A %#llx, Map B (same target) %#llx, entries %d\n", a.r, b.r, g_nmap);
    CHECK(g_nmap == 1 && a.r == 0 && b.r == 0xD401, "det5: one mapping, the second Map must fail with InvalidCurrentMemory 0xd401 (expect entries 1, A 0, B 0xd401)");
    reset_map();
}

static barrier_t g_bar;
static _Atomic long map_fail, unmap_fail, live_lost, after_not_free, rounds_done;
static void *disjoint(void *p) {
    int i = (int)(intptr_t)p; uint8_t *src = g_src[i]; uint64_t dst = DST(i);
    barrier_wait(&g_bar);
    for (int k = 0; k < R; k++) {
        if (svc(4, dst, (uint64_t)src, SZ)) { map_fail++; continue; }
        if (query_state(dst) != 0x0B) live_lost++;          // 自己活着的映射不在表里 | our own live mapping is missing from the registry
        if (svc(5, dst, (uint64_t)src, SZ)) unmap_fail++;    // 表项丢了或坏了 | registry lost or corrupted our entry
        else if (query_state(dst) != 0) after_not_free++;   // Unmap 成功后表项还在 | registry still has our entry after a successful Unmap
        rounds_done++;
    }
    return NULL;
}
static _Atomic int g_round_ok[2];
static void *overlap(void *p) {
    int i = (int)(intptr_t)p; uint8_t *src = g_src[i]; uint64_t dst = DST(0);
    for (int k = 0; k < R; k++) {
        barrier_wait(&g_bar);
        atomic_store(&g_round_ok[i], svc(4, dst, (uint64_t)src, SZ) == 0);
        barrier_wait(&g_bar); barrier_wait(&g_bar);   // 两道屏障之间主线程查看并清表 | the main thread inspects and resets between these barriers
    }
    return NULL;
}
static void run_disjoint(void) {
    reset_map(); pthread_t t[NT]; barrier_init(&g_bar, NT);
    for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, disjoint, (void *)(intptr_t)i);
    for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
    printf("disjoint: %d threads x %d rounds: full_rounds=%ld map_fail=%ld live_not_0xB=%ld unmap_fail=%ld still_mapped=%ld entries=%d\n",
           NT, R, (long)rounds_done, (long)map_fail, (long)live_lost, (long)unmap_fail, (long)after_not_free, g_nmap);
    CHECK(!map_fail && !live_lost && !unmap_fail && !after_not_free && !g_nmap && rounds_done == NT * R, "disjoint: expect all 0 and full_rounds=%d", NT * R);
}
static void run_overlap(void) {
    reset_map(); pthread_t t[2]; barrier_init(&g_bar, 3); long both = 0;
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, overlap, (void *)(intptr_t)i);
    for (int k = 0; k < R; k++) {
        barrier_wait(&g_bar); barrier_wait(&g_bar);
        if (g_round_ok[0] && g_round_ok[1]) both++;
        reset_map();
        barrier_wait(&g_bar);
    }
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    printf("overlap: %d rounds, 2 threads map the same target: both succeeded in %ld rounds (expect 0)\n", R, both);
    CHECK(!both, "overlap");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all"; if (argc > 2) R = atoi(argv[2]);
    setvbuf(stdout, NULL, _IONBF, 0);
    g_dm_log_hook = quiet_log;
    mem_init();
    for (int i = 0; i < NT; i++) { g_src[i] = malloc(SZ); memset(g_src[i], i, SZ); }
    int all = !strcmp(mode, "all");
    if (all || !strcmp(mode, "det17")) det17();
    if (all || !strcmp(mode, "det5")) det5();
    if (!strcmp(mode, "disjoint")) run_disjoint();
    if (!strcmp(mode, "overlap")) run_overlap();
    printf("t_svc_mem %s: %s\n", mode, g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
