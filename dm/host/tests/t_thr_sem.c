// 线程类 SVC 的边角语义（hle_thread.c，直接包含真源码）。用未定义行为消毒器编（make run 里的 t_thr_sem），有符号溢出当场中止。
//   core      CreateThread 理想核 −2（用进程缺省）/ −3 存成真实核号；SetThreadCoreMask −2 = 进程缺省核、−3 = 不改（F-CORE-10）
//   timeout   WaitSynchronization 超时 INT64_MAX 不溢出、照常被唤醒（F-CORE-7 前半）；墙钟比单调钟快 3 秒时 100 ms 的超时仍是 100 ms（后半：原先按 CLOCK_REALTIME 算绝对截止，墙钟一跳等待就被拉长 / 缩短）
//   exit      ExitThread 先回到宿主栈、再公布「已退出」唤醒等它的线程：等的一方一醒就收回客体栈（模拟 UnmapMemory 后同一段又被映射给别的线程），
//             退出线程之后的收尾（pthread_exit、线程键析构）不许再碰那段栈（F-CORE-4）。在子进程里跑：修前子进程在被收回的栈上 SIGBUS
//   signal    SignalToAddress 类型 2 的改值与当前 Mesosphere 一致（Atmosphère 5f2d713「kern: mea culpa」之后是 value−1，F-CORE-9 已驳回）：防止有人照旧资料改成 value−2
// Edge semantics of the thread SVCs (hle_thread.c, the real source included). Built with the UndefinedBehaviorSanitizer (t_thr_sem in make run): a signed overflow aborts on the spot.
//   core      CreateThread ideal core -2 (use the process default) / -3 is stored as a real core number; SetThreadCoreMask -2 = the process default core, -3 = leave unchanged (F-CORE-10)
//   timeout   WaitSynchronization with timeout INT64_MAX does not overflow and is woken as usual (first half of F-CORE-7); with the wall clock 3 s ahead of the monotonic clock a 100 ms timeout is still 100 ms (second half: the old absolute deadline on CLOCK_REALTIME stretched / shortened a wait whenever the wall clock jumped)
//   exit      ExitThread first returns to the host stack, then publishes "exited" and wakes the threads waiting for it: the waiter takes the guest stack back as soon as it wakes (as when UnmapMemory is followed by the same range being mapped for another thread),
//             and the exiting thread's remaining teardown (pthread_exit, thread-key destructors) must not touch that stack again (F-CORE-4). Runs in a child process: before the fix the child dies with SIGBUS on the revoked stack
//   signal    SignalToAddress type 2 changes the value as current Mesosphere does (value-1 since Atmosphère 5f2d713 "kern: mea culpa"; F-CORE-9 was rejected): guards against someone changing it to value-2 from old references
#include "../hle_thread.c"
#include <limits.h>
#include <stdatomic.h>
#include <sys/wait.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void *guest_sym(const char *name) { (void)name; return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
#include <mach/mach_time.h>
uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
// 同 entry.S 的 dm_thread_enter：切到给的栈、x0 = 参数调入口 | as dm_thread_enter in entry.S: switch to the given stack and call entry with x0 = argument
__asm__(".globl _dm_thread_enter\n.p2align 2\n_dm_thread_enter:\n mov sp, x2\n mov x29, #0\n mov x16, x0\n mov x0, x1\n adr x30, 1f\n br x16\n1: brk #0\n");
// 墙钟可以拨快：hle_thread.c 的 clock_gettime 调用落到这里（同一映像内先绑本地定义）；单调钟照真
// The wall clock can be set ahead: hle_thread.c's clock_gettime calls land here (references inside the same image bind to the local definition first); the monotonic clocks are real
static int64_t g_realtime_skew_s;
int clock_gettime(clockid_t c, struct timespec *ts) {
    uint64_t ns = c == CLOCK_REALTIME ? clock_gettime_nsec_np(CLOCK_REALTIME) + (uint64_t)(g_realtime_skew_s * 1000000000ll) : clock_gettime_nsec_np(c);
    ts->tv_sec = (time_t)(ns / 1000000000ull); ts->tv_nsec = (long)(ns % 1000000000ull); return 0;
}

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)
static uint64_t svc(int num, uint64_t *r) { svc_thread(num, r); return r[0]; }
static void entry_park(uint64_t arg) { (void)arg; for (;;) pause(); }

static void t_core(void) {
    uint64_t r[32] = {0, (uint64_t)entry_park, 0, 0x10000, 44, (uint64_t)(uint32_t)-2}; svc(0x08, r);   // 不启动：thread_main 停在 started 上 | never started: thread_main parks on started
    uint32_t h = (uint32_t)r[1];
    uint64_t q[32] = {0, 0, h}; svc(0x0E, q);
    printf("core: CreateThread(core -2) -> GetThreadCoreMask core %#llx mask %#llx\n", q[1], q[2]);
    CHECK(q[0] == 0 && q[1] == 0, "core: ideal core -2 must be stored as the process default core 0, not 0xfffffffe");
    uint64_t r3[32] = {0, (uint64_t)entry_park, 0, 0x10000, 44, (uint64_t)(uint32_t)-3}; svc(0x08, r3);
    uint64_t q3[32] = {0, 0, r3[1]}; svc(0x0E, q3);
    CHECK(q3[1] == 0, "core: ideal core -3 at CreateThread must be stored as 0 (got %#llx)", q3[1]);
    uint64_t s1[32] = {h, 1, 6}; svc(0x0F, s1);
    uint64_t s2[32] = {h, (uint64_t)(uint32_t)-3, 6}; svc(0x0F, s2);
    uint64_t q2[32] = {0, 0, h}; svc(0x0E, q2);
    printf("core: SetThreadCoreMask(1, 6) then (-3, 6) -> core %#llx mask %#llx\n", q2[1], q2[2]);
    CHECK(q2[1] == 1 && q2[2] == 6, "core: -3 must leave the ideal core unchanged");
    uint64_t s3[32] = {h, (uint64_t)(uint32_t)-2, 7}; svc(0x0F, s3);
    uint64_t q4[32] = {0, 0, h}; svc(0x0E, q4);
    printf("core: SetThreadCoreMask(-2, 7) -> core %#llx mask %#llx\n", q4[1], q4[2]);
    CHECK(q4[1] == 0, "core: -2 must set the process default core 0");
    uint64_t m1[32] = {H_CUR_THREAD, (uint64_t)(uint32_t)-1, 7}; svc(0x0F, m1);
    uint64_t pn[32] = {0}; svc(0x10, pn);
    printf("core: GetCurrentProcessorNumber after SetThreadCoreMask(-1) -> %#llx\n", pn[0]);
    CHECK(pn[0] <= 2, "core: GetCurrentProcessorNumber must be a real core");
}

static uint32_t g_hw;
static void *signal_later(void *p) { usleep((useconds_t)(uintptr_t)p); uint64_t r[32] = {g_hw}; svc(0x11, r); return NULL; }
static uint64_t wait_ms(uint32_t hr, int64_t timeout_ns, uint64_t *res) {
    uint32_t hs[1] = {hr}; uint64_t t0 = now_ns();
    uint64_t r[32] = {0, (uint64_t)hs, 1, (uint64_t)timeout_ns}; svc(0x18, r); *res = r[0];
    return (now_ns() - t0) / 1000000;
}
static void t_timeout(void) {
    uint64_t e[32] = {0}; svc(0x45, e); g_hw = (uint32_t)e[1]; uint32_t hr = (uint32_t)e[2];
    pthread_t sg; pthread_create(&sg, NULL, signal_later, (void *)(uintptr_t)300000);
    uint64_t res, ms = wait_ms(hr, INT64_MAX, &res); pthread_join(sg, NULL);
    printf("timeout: WaitSynchronization(timeout INT64_MAX), signalled at 300 ms -> %#llx after %llu ms\n", res, ms);
    CHECK(res == 0 && ms >= 250 && ms < 2000, "timeout: an INT64_MAX timeout must behave as infinite");
    uint64_t c[32] = {g_hw}; svc(0x12, c);   // ClearEvent
    g_realtime_skew_s = 3;   // 墙钟快 3 秒 | the wall clock is 3 s ahead
    ms = wait_ms(hr, 100000000, &res);
    printf("timeout: wall clock 3 s ahead, WaitSynchronization(100 ms) -> %#llx after %llu ms\n", res, ms);
    CHECK(res == R_TIMEOUT && ms >= 90 && ms < 1000, "timeout: a 100 ms timeout must not follow the wall clock");
    pthread_create(&sg, NULL, signal_later, (void *)(uintptr_t)100000);
    ms = wait_ms(hr, 2000000000, &res); pthread_join(sg, NULL);
    printf("timeout: wall clock 3 s ahead, WaitSynchronization(2 s), signalled at 100 ms -> %#llx after %llu ms\n", res, ms);
    CHECK(res == 0 && ms < 1000, "timeout: a signal must still wake a timed wait");
    g_realtime_skew_s = 0;
}

// ---- exit：子进程里跑 | exit: runs in a child ----
enum { STK = 0x10000 };
static uint8_t *g_stk; static pthread_key_t g_dkey; static _Atomic int g_dtor_done; static _Atomic uintptr_t g_dtor_sp;
static void dtor(void *v) { (void)v; volatile char here; atomic_store(&g_dtor_sp, (uintptr_t)&here); usleep(200000); atomic_store(&g_dtor_done, 1); }
static void entry_exit(uint64_t arg) {
    (void)arg; pthread_setspecific(g_dkey, (void *)1);   // 线程退出时析构在跑：它的帧压在哪块栈上 | the destructor runs at thread exit: which stack its frame lands on
    uint64_t r[32] = {0}; svc(0x0A, r);                   // ExitThread
    abort();
}
static int exit_child(void) {
    g_stk = mmap(NULL, STK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    pthread_key_create(&g_dkey, dtor);
    uint64_t r[32] = {0, (uint64_t)entry_exit, 0, (uint64_t)(g_stk + STK), 44, 0}; svc(0x08, r); uint32_t h = (uint32_t)r[1];
    uint64_t s[32] = {h}; svc(0x09, s);
    uint32_t hs[1] = {h}; uint64_t w[32] = {0, (uint64_t)hs, 1, (uint64_t)-1}; svc(0x18, w);   // 等它退出 | wait for it to exit
    mprotect(g_stk, STK, PROT_NONE);                     // 一醒就收回它的客体栈 | take its guest stack back right away
    for (int i = 0; i < 300 && !atomic_load(&g_dtor_done); i++) usleep(10000);
    uintptr_t sp = atomic_load(&g_dtor_sp); int on_guest = sp >= (uintptr_t)g_stk && sp < (uintptr_t)g_stk + STK;
    printf("exit: WaitSynchronization -> %#llx; thread-key destructor ran on the %s stack and %s\n", w[0], on_guest ? "guest" : "host", atomic_load(&g_dtor_done) ? "finished" : "did not finish");
    return w[0] == 0 && !on_guest && atomic_load(&g_dtor_done) ? 0 : 1;
}
static void t_exit(void) {
    fflush(stdout);
    pid_t p = fork();
    if (!p) _exit(exit_child());
    int st = 0; waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) printf("exit: the exiting thread crashed with %s after its stack was taken back\n", strsignal(WTERMSIG(st)));
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "exit: ExitThread must finish on the host stack before it wakes the waiter");
}

// ---- signal：对照当前 libmesosphere kern_k_address_arbiter.cpp | signal: against current libmesosphere kern_k_address_arbiter.cpp ----
static _Atomic int32_t g_word;
static int32_t g_val;
static void *waiter(void *p) { (void)p; uint64_t r[32] = {(uint64_t)&g_word, 2, (uint64_t)(uint32_t)g_val, (uint64_t)-1};
    thr_t *t = calloc(1, sizeof *t); pthread_cond_init(&t->cv, NULL); t->started = 1; t->prio = 44; t->pt = pthread_self();
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t); svc(0x34, r); return NULL; }
static int nwait(void) { int n = 0; pthread_mutex_lock(&G); for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == (uint64_t)&g_word) n++; pthread_mutex_unlock(&G); return n; }
static int32_t meso(int32_t v, int nw, int cnt) {
    if (cnt <= 0) return nw > 0 ? v - 1 : v + 1;
    if (nw == 0) return v + 1;
    int tmp = 0; for (int k = 1; k < nw; k++) { if (++tmp >= cnt) break; }
    return tmp < cnt ? v - 1 : v;
}
static void t_signal(void) {
    int diff = 0, cases = 0;
    for (int nw = 0; nw <= 3; nw++) for (int cnt = -1; cnt <= 3; cnt++) {
        g_val = 5; atomic_store(&g_word, g_val);
        pthread_t th[3]; for (int i = 0; i < nw; i++) pthread_create(&th[i], NULL, waiter, NULL);
        while (nwait() < nw) usleep(1000);
        uint64_t r[32] = {(uint64_t)&g_word, 2, (uint64_t)(uint32_t)g_val, (uint64_t)(uint32_t)cnt}; svc(0x35, r);
        int32_t got = atomic_load(&g_word); cases++;
        if (got != meso(g_val, nw, cnt)) { diff++; printf("  signal waiters=%d count=%d: host writes %+d, Mesosphere %+d\n", nw, cnt, got - g_val, meso(g_val, nw, cnt) - g_val); }
        uint64_t w[32] = {(uint64_t)&g_word, 0, 0, (uint64_t)-1}; svc(0x35, w);
        for (int i = 0; i < nw; i++) pthread_join(th[i], NULL);
    }
    printf("signal: SignalToAddress type 2, %d cases, %d differ from current Mesosphere\n", cases, diff);
    CHECK(!diff, "signal");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    svc_thread_main_init();
    const char *m = argc > 1 ? argv[1] : "all"; int all = !strcmp(m, "all");
    if (all || !strcmp(m, "exit")) t_exit();   // 先分叉：子进程里只有主线程 | fork first: the child has only the main thread
    if (all || !strcmp(m, "core")) t_core();
    if (all || !strcmp(m, "timeout")) t_timeout();
    if (all || !strcmp(m, "signal")) t_signal();
    printf("t_thr_sem %s: %s\n", m, g_fail ? "FAIL" : "OK");
    _exit(g_fail ? 1 : 0);   // 停着的线程不收：直接退 | parked threads are left alone: exit directly
}
