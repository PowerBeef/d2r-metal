// 现场导出（hle_thread.c svc_thread_dump，经 crash.c 调）不许卡死、不许读已释放的线程对象（F-CORE-2 / F-CORE-6）：直接包含真的 hle_thread.c，链真的 crash.c。
//   dump N    活着的进程里连做 N 次导出（控制口 dump、DM_WATCH_STALL 卡顿导出、iOS DM_LOG_PORT dump 走的同一条），同时 3 条「客体线程」不停用 LOG 打日志。
//             导出暂停一条正持着 stderr 锁（或 libc 区域设置锁）的线程后再打印就永远等下去；监视线程 3 秒没见到导出返回即判卡死
//   crash T   崩溃路径：T 次分叉，子进程装 crash.c 的信号处理、起打日志的线程、在主线程 die()（FATAL + abort → SIGABRT → on_crash → 导出）；
//             导出钩子做完就报完成并 _exit(0)。父进程把 5 秒后还活着的子进程算卡死并杀掉
//   uaf N     一条线程不停起短命「客体线程」（关自己的句柄再 ExitThread → thr_free），主线程同时做 N 次导出；地址消毒版（t_dump_asan）抓已释放内存的读
// 不带参数 = dump 200 + crash 5（make run）
// The state dump (hle_thread.c svc_thread_dump, called through crash.c) must not hang and must not read freed thread objects (F-CORE-2 / F-CORE-6): includes the real hle_thread.c and links the real crash.c.
//   dump N    N dumps in a live process (the same path as the control-port dump, the DM_WATCH_STALL stall dump and the iOS DM_LOG_PORT dump) while 3 "guest threads" keep logging with LOG.
//             A dump that suspends a thread holding the stderr lock (or libc's locale lock) and then prints waits forever; a monitor thread calls it a hang when no dump returns for 3 s
//   crash T   the crash path: T forks; each child installs crash.c's signal handlers, starts the logging threads and calls die() on the main thread (FATAL + abort → SIGABRT → on_crash → dump);
//             the dump hook reports completion and _exit(0)s. The parent counts children still alive after 5 s as hung and kills them
//   uaf N     one thread keeps starting short-lived "guest threads" (they close their own handle, then ExitThread → thr_free) while the main thread dumps N times; the AddressSanitizer build (t_dump_asan) catches reads of freed memory
// No argument = dump 200 + crash 5 (make run)
#include "../hle_thread.c"
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>

// ---- 桩（同 t_condvar_stub.c，只是 crash_thread_init 由 crash.c 给） | stubs (as t_condvar_stub.c, except crash_thread_init, which crash.c provides) ----
struct dm_flags g_flag;
uint64_t g_t0_ns;
void guest_thread_init(void) {}
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
void *guest_sym(const char *name) { (void)name; return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
#include <mach/mach_time.h>
uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void crash_install(void); void crash_set_dump_hook(void (*fn)(void)); void crash_dump_now(const char *why);

// 照 svc_thread_main_init 把调用线程登记成客体线程（不重建 g_keythr） | register the calling pthread as a guest thread the way svc_thread_main_init does (without re-creating g_keythr)
static thr_t *reg_self(void) {
    thr_t *t = calloc(1, sizeof *t); pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt); t->born_ns = now_ns();
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    return t;
}
static void say(const char *s) { (void)!write(1, s, strlen(s)); }
static void say_num(unsigned long v) { char t[24]; int n = 0; do t[n++] = (char)('0' + v % 10); while (v /= 10); char o[24]; for (int i = 0; i < n; i++) o[i] = t[n - 1 - i]; o[n] = 0; say(o); }

enum { NL = 3 };
static _Atomic int g_nreg; static _Atomic unsigned long g_lines;
static void *logger(void *p) {
    int i = (int)(intptr_t)p; reg_self(); atomic_fetch_add(&g_nreg, 1);
    for (;;) LOG("guest thread %d logging line %lu with some payload to make the write a little longer", i, (unsigned long)atomic_fetch_add(&g_lines, 1));
    return NULL;
}
static void start_loggers(void) {
    for (int i = 0; i < NL; i++) { pthread_t t; pthread_create(&t, NULL, logger, (void *)(intptr_t)i); pthread_detach(t); }
    while (atomic_load(&g_nreg) < NL) usleep(1000);
}
// 监视线程：3 秒没有导出返回即判卡死。这里不用 printf 一族：被导出暂停的线程可能正持着区域设置锁
// Monitor thread: a hang when no dump returns for 3 s. No printf family here: a thread suspended by the dump may hold the locale lock
static _Atomic unsigned long g_dumps;
static void *monitor(void *p) {
    (void)p; unsigned long last = (unsigned long)-1; int still = 0;
    for (;;) {
        usleep(100000);
        unsigned long d = atomic_load(&g_dumps);
        if (d != last) { last = d; still = 0; continue; }
        if (++still < 30) continue;
        say("t_dump dump: FAIL (dump #"); say_num(d + 1); say(" has not returned for 3 s: hung)\n"); _exit(3);
    }
    return NULL;
}
static int g_done_fd = -1;
static void dump_then_exit(void) { svc_thread_dump(); if (g_done_fd >= 0) (void)!write(g_done_fd, "D", 1); _exit(0); }

static int mode_dump(long n) {
    crash_set_dump_hook(svc_thread_dump);
    start_loggers();
    pthread_t m; pthread_create(&m, NULL, monitor, NULL);
    for (long k = 0; k < n; k++) { crash_dump_now("t_dump"); atomic_fetch_add(&g_dumps, 1); }
    char b[160]; snprintf(b, sizeof b, "t_dump dump: OK (%ld dumps, none hung, %lu log lines written meanwhile)\n", n, (unsigned long)atomic_load(&g_lines)); say(b);
    return 0;
}
static int mode_crash(long n) {
    long hung = 0, done = 0, other = 0;
    for (long k = 0; k < n; k++) {
        int pfd[2]; if (pipe(pfd)) return 2;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]); g_done_fd = pfd[1];
            int nul = open("/dev/null", O_WRONLY); dup2(nul, 1);
            crash_install(); crash_set_dump_hook(dump_then_exit);
            start_loggers(); usleep(20000);
            die("t_dump: deliberate die() on the main thread");
        }
        close(pfd[1]);
        int st = 0, alive = 1;
        for (int w = 0; w < 50 && alive; w++) { usleep(100000); if (waitpid(pid, &st, WNOHANG) == pid) alive = 0; }
        if (alive) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
        char c; ssize_t got = read(pfd[0], &c, 1); close(pfd[0]);
        if (alive) hung++; else if (got == 1) done++; else other++;
    }
    printf("t_dump crash: %s (%ld trials: dump finished %ld, hung > 5 s %ld, other exit %ld)\n", hung || other ? "FAIL" : "OK", n, done, hung, other);
    return hung || other ? 1 : 0;
}
static _Atomic long g_churned;
static void *short_lived(void *p) {
    (void)p; thr_t *t = reg_self();
    usleep(200);
    uint64_t r[32] = {t->handle}; svc_thread(0x16, r);   // CloseHandle（自己的句柄）：closed = 1 | CloseHandle(own handle): closed = 1
    atomic_fetch_add(&g_churned, 1);
    uint64_t x[32] = {0}; svc_thread(0x0A, x);           // ExitThread：exited = 1，thr_free，pthread_exit | ExitThread: exited = 1, thr_free, pthread_exit
    return NULL;
}
static void *churn(void *p) { (void)p; for (;;) { pthread_t t; pthread_create(&t, NULL, short_lived, NULL); pthread_detach(t); usleep(300); } return NULL; }
static int mode_uaf(long n) {
    crash_set_dump_hook(svc_thread_dump);
    pthread_t c; pthread_create(&c, NULL, churn, NULL);
    for (long k = 0; k < n; k++) crash_dump_now("t_dump uaf");
    printf("t_dump uaf: OK (no fault in %ld dumps while %ld short-lived threads came and went)\n", n, (long)atomic_load(&g_churned));
    return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    svc_thread_main_init();   // 建 g_keythr 并登记主线程 | creates g_keythr and registers the main thread
    const char *mode = argc > 1 ? argv[1] : "all"; long n = argc > 2 ? atol(argv[2]) : 0;
    if (!strcmp(mode, "dump")) _exit(mode_dump(n ? n : 200));   // 打日志的线程还在跑：不走 exit 的收尾 | the logging threads are still running: skip exit's teardown
    if (!strcmp(mode, "crash")) return mode_crash(n ? n : 5);
    if (!strcmp(mode, "uaf")) { int r = mode_uaf(n ? n : 1000); _exit(r); }
    int r = mode_crash(5);   // 先分叉（子进程只带主线程），再起打日志的线程 | fork first (children carry only the main thread), then start the logging threads
    if (r) return r;
    _exit(mode_dump(200));
}
