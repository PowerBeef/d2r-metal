// 内存类 SVC（hle_svc.c）的并发与语义单测：直接包含真的 hle_svc.c，读得到映射表 g_map / g_nmap。
// MapMemory / UnmapMemory 在查表与登记之间的那次 memcpy 换成 t_memcpy：能让选定的线程停在那里，排出确定的交错。
//   det17   UnmapMemory 按过期下标删表项（F-CORE-17）：A 的 Unmap 停在 memcpy；B 另起线程 Unmap 别的栈；放开 A。期望活着的栈 C 仍在表里
//   det5    MapMemory 查表与登记不是一步（F-CORE-5）：A 的 Map 停在 memcpy；B 另起线程 Map 同一目标。期望只有一个成功、表里一项
//   phys    MapPhysicalMemory 给的页应是零（F-CORE-16）：映射、写满 0xA5、UnmapPhysicalMemory、再映射，数还剩几个 0xA5
//   svc     ExitProcess（0x07）走宿主优雅退出、未实现的 SVC 回 NotImplemented 0x4201 并报一次红（F-CORE-12）
//   query   QueryMemory 对模块按段回答、对别的宿主地址回一整块 Inaccessible（F-CORE-1）；照 SDK 的 GetExceptionInfo（sdk+0x3642e0）的走法走一遍，应几步内找到模块起点与 CodeData 末尾
//   disjoint R / overlap R   压力（线程消毒版 make tsan 跑）：8 线程各映射自己的槽 R 轮；2 线程同时映射同一目标 R 轮
// 不带参数 = 跑全部确定用例（make run）
// Concurrency and semantics unit test of the memory SVCs (hle_svc.c): includes the real hle_svc.c, so it can read the map registry g_map / g_nmap.
// The memcpy MapMemory / UnmapMemory do between looking up the registry and recording in it is routed through t_memcpy, which can park a chosen thread there for deterministic interleavings.
//   det17   UnmapMemory deletes its entry by a stale index (F-CORE-17): A's Unmap parks in memcpy; B unmaps another stack on its own thread; A is released. Expected: the live stack C is still registered
//   det5    MapMemory's lookup and record are not one step (F-CORE-5): A's Map parks in memcpy; B maps the same target on its own thread. Expected: exactly one success, one entry
//   phys    pages from MapPhysicalMemory must be zero (F-CORE-16): map, fill with 0xA5, UnmapPhysicalMemory, map again, count the 0xA5 bytes left
//   svc     ExitProcess (0x07) goes to the host's graceful exit, and an unimplemented SVC returns NotImplemented 0x4201 and reports red once (F-CORE-12)
//   query   QueryMemory answers a module segment by segment and any other host address with one Inaccessible block (F-CORE-1); the walk of the SDK's GetExceptionInfo (sdk+0x3642e0) must find the module start and the end of CodeData in a few steps
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
module_t *module_of(const void *p) {   // 同 loader.c | as loader.c
    for (int i = 0; i < g_nmod; i++) if ((const uint8_t *)p >= g_mod[i].base && (const uint8_t *)p < g_mod[i].base + g_mod[i].size) return &g_mod[i];
    return NULL;
}
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
uint64_t svc_host_call(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void hlecount_svc(int num) { (void)num; }
uint64_t dm_entropy64(void) { return 4; }
void dm_backtrace(uint64_t fp) { (void)fp; }
uint64_t guest_ns(void) { return 0; }
static _Atomic long g_logs, g_red_logs; static char g_shutdown_why[64];
static void quiet_log(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; atomic_fetch_add(&g_logs, 1); if (strstr(fmt, "【红】")) atomic_fetch_add(&g_red_logs, 1); }
void dm_host_shutdown(const char *why) { snprintf(g_shutdown_why, sizeof g_shutdown_why, "%s", why); }   // 真的会 _exit(0)；这里只记下来 | the real one _exit(0)s; here it is only recorded

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

static void phys(void) {
    uint64_t a = (uint64_t)g_alias; const size_t n = 0x40000;
    uint64_t r1 = svc(0x2C, a, n, 0); memset((void *)a, 0xA5, n);
    uint64_t r2 = svc(0x2D, a, n, 0), r3 = svc(0x2C, a, n, 0);
    size_t stale = 0; for (size_t i = 0; i < n; i++) stale += ((uint8_t *)a)[i] == 0xA5;
    printf("phys: Map %#llx, fill 0xA5, Unmap %#llx, Map %#llx: %zu of %zu bytes still 0xA5 (Horizon: all 0)\n", r1, r2, r3, stale, n);
    CHECK(!r1 && !r2 && !r3 && !stale, "phys: re-mapped pages must be zero");
    svc(0x2D, a, n, 0);
}
static void svcs(void) {
    long red0 = g_red_logs;
    uint64_t r[32] = {0}; svc_dispatch(0x07, r, 0);
    printf("svc: ExitProcess -> dm_host_shutdown(\"%s\")\n", g_shutdown_why);
    CHECK(!strcmp(g_shutdown_why, "ExitProcess"), "svc: ExitProcess must shut the host down");
    uint64_t a = svc(0x50, 1, 2, 3), b = svc(0x50, 1, 2, 3);
    printf("svc: unknown SVC 0x50 -> %#llx, again -> %#llx, red log lines %ld\n", a, b, g_red_logs - red0);
    CHECK(a == 0x4201 && b == 0x4201 && g_red_logs - red0 == 1, "svc: an unknown SVC must return NotImplemented 0x4201 and report red once");
}
// ---- query ----
typedef struct { uint64_t base, size; uint32_t state, perm; } qinfo_t;
static long g_nq;
static qinfo_t q(uint64_t addr) { uint64_t mi[8] = {0}; uint64_t r[32] = {(uint64_t)mi, 0, addr}; svc_dispatch(6, r, 0); g_nq++; return (qinfo_t){mi[0], mi[1], ((uint32_t *)mi)[4], ((uint32_t *)mi)[6]}; }
// 照 sdk+0x3642e0 的走法，只看 QueryMemory 的回答（无符号比较照抄）：1 = 找到模块，0 = pc 不在 Code/RX 里（没有异常信息），−1 = SDK 会中止，−2 = 查询超过上限
// The walk of sdk+0x3642e0, looking only at QueryMemory's answers (unsigned comparisons as in the original): 1 = module found, 0 = pc not in Code/RX (no exception info), -1 = the SDK would abort, -2 = query limit exceeded
static int exc_walk(uint64_t pc, uint64_t *mod, uint64_t *msize) {
    const long cap = 200000; g_nq = 0;
    qinfo_t i = q(pc);
    if (i.perm != 5 || (i.state != 3 && i.state != 8)) return 0;
    uint32_t st = i.state; uint64_t x21 = 0, a = pc; int found = 0;
    for (;;) { if (g_nq > cap) return -2; i = q(a);
        if (i.state != st || i.perm != 5) { if (!found) return -1; break; }
        x21 = i.base; a = i.base - 1; found = 1; if (!(a <= pc)) break; }
    *mod = x21; uint64_t x23 = 0; a = pc; found = 0;
    for (;;) { if (g_nq > cap) return -2; i = q(a);
        if (i.state != st) { if (!found) return -1; break; }
        a = i.base + i.size; x23 = a - 1; found = 1; if (!(a >= pc)) break; }
    uint32_t want = st == 3 ? 4 : 9; uint64_t start = x23 + 1; a = start; found = 0;
    for (;;) { if (g_nq > cap) return -2; i = q(a);
        if (i.state != want) { if (!found) return -1; break; }
        a = i.base + i.size; x23 = a - 1; found = 1; if (!(a >= start)) break; }
    *msize = x23 - x21 + 1; return 1;
}
static void query(void) {
    // 两个首尾相接的模块，布局照产物（main：TEXT_END 0x1b786a0、RO_OFF 0x1b79000、DATA_OFF 0x2056000、键页 0x3214000、补丁区 0x3220000、大小 0x33dc000），地址放在宿主空间里一块没用的地方
    // Two modules mapped end to end with the products' layout (main: TEXT_END 0x1b786a0, RO_OFF 0x1b79000, DATA_OFF 0x2056000, key page 0x3214000, patch area 0x3220000, size 0x33dc000), placed at an unused spot of the host address space
    uint64_t b0 = 0x500000000000ull, sz0 = 0x33dc000, b1 = b0 + sz0, sz1 = 0xe80000;
    g_mod[0] = (module_t){.name = "main", .base = (uint8_t *)b0, .size = sz0, .text_end = 0x1b786a0, .ro_off = 0x1b79000, .data_off = 0x2056000, .key_page_off = 0x3214000, .patch_off = 0x3220000};
    g_mod[1] = (module_t){.name = "sdk", .base = (uint8_t *)b1, .size = sz1, .text_end = 0x58d930, .ro_off = 0x58e000, .data_off = 0xc15000, .key_page_off = 0xd34000, .patch_off = 0xd40000};
    g_nmod = 2;
    struct { const char *what; uint64_t a, base, size; uint32_t state, perm; } want[] = {
        {"main text", b0 + 0x1234, b0, 0x1b79000, 3, 5}, {"main ro", b0 + 0x1b79000, b0 + 0x1b79000, 0x2056000 - 0x1b79000, 3, 1},
        {"main data", b0 + 0x3000000, b0 + 0x2056000, 0x3214000 - 0x2056000, 4, 3}, {"main key page", b0 + 0x3214000, b0 + 0x3214000, 0xc000, 0x14, 1},
        {"main patch area", b0 + 0x33db000, b0 + 0x3220000, 0x1bc000, 0x14, 5}, {"sdk text", b1, b1, 0x58e000, 3, 5},
    };
    for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++) { qinfo_t i = q(want[k].a);
        printf("query: %-16s -> +%#llx size %#llx state %#x perm %u\n", want[k].what, i.base - b0, i.size, i.state, i.perm);
        CHECK(i.base == want[k].base && i.size == want[k].size && i.state == want[k].state && i.perm == want[k].perm, "query: %s", want[k].what); }
    qinfo_t below = q(b0 - 1), above = q(0xffff000000000000ull);
    printf("query: just below main -> state %#x, block ends at main %s; top of the address space -> state %#x, block reaches 2^64 %s\n",
           below.state, below.base + below.size == b0 ? "yes" : "no", above.state, above.base + above.size == 0 ? "yes" : "no");
    CHECK(below.state == 0x10 && below.perm == 0 && below.base + below.size == b0 && above.state == 0x10 && above.base + above.size == 0, "query: host addresses must be one Inaccessible block bounded by the known regions");
    uint64_t mod = 0, msz = 0; int r;
    r = exc_walk(b0 + 0x123450, &mod, &msz);
    printf("query: GetExceptionInfo walk from main text -> %d after %ld queries: module +%#llx size %#llx (expect 1, +0, %#x)\n", r, g_nq, mod - b0, msz, 0x3214000);
    CHECK(r == 1 && mod == b0 && msz == 0x3214000 && g_nq < 20, "query: walk from main text");
    r = exc_walk(b1 + 0x3642e0, &mod, &msz);
    printf("query: GetExceptionInfo walk from sdk text -> %d after %ld queries: module +%#llx (sdk +%#llx) size %#llx (expect 1, sdk +0, %#x)\n", r, g_nq, mod - b0, mod - b1, msz, 0xd34000);
    CHECK(r == 1 && mod == b1 && msz == 0xd34000 && g_nq < 20, "query: walk from sdk text (the main patch area right before it must not count)");
    r = exc_walk(b0 + 0x3221000, &mod, &msz);
    printf("query: GetExceptionInfo walk from the main patch area -> %d after %ld queries (expect 0: no exception info)\n", r, g_nq);
    CHECK(r == 0, "query: a patch-area pc has no exception info");
    r = exc_walk(0x0000000180001000ull, &mod, &msz);
    printf("query: GetExceptionInfo walk from a host address -> %d after %ld queries (expect 0)\n", r, g_nq);
    CHECK(r == 0 && g_nq == 1, "query: a host pc has no exception info");
    g_nmod = 0;
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
    if (all || !strcmp(mode, "phys")) phys();
    if (all || !strcmp(mode, "svc")) svcs();
    if (all || !strcmp(mode, "query")) query();
    if (!strcmp(mode, "disjoint")) run_disjoint();
    if (!strcmp(mode, "overlap")) run_overlap();
    printf("t_svc_mem %s: %s\n", mode, g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
