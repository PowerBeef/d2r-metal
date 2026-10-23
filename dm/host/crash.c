#include <TargetConditionals.h>
// 崩溃现场：打印出错地址、pc、lr（换成模块内偏移）与寄存器；缺页钩子分发。
// 只放信号处理本身：诊断功能（探针 / UI 消息 / 采样）在 diag.c，本文件不依赖它们（图形单测只链本文件）。
// Crash state: prints the faulting address, pc, lr (converted to module-relative offsets) and the registers; dispatches page fault hooks.
// Only signal handling itself lives here: diagnostic features (probes / UI messages / sampling) are in diag.c, and this file does not depend on them (the graphics unit tests link only this file).
#include "dm.h"
#include "dm_uctx.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

static _Atomic uint64_t g_watch_last_ns;
static _Atomic uint64_t g_watch_count;
static _Atomic uint32_t g_watch_tick;
static _Atomic int g_watch_pregame = 1;
static _Atomic int g_watch_stage_idx;
static char g_watch_stage[2][256] = {"启动", "启动"};
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);
static void (*g_wait_dump)(void);

static uint64_t watch_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t crash_watch_progress_count(void) { return atomic_load(&g_watch_count); }
void crash_watch_progress(uint32_t tick, const char *stage) {
    int next = 1 - atomic_load(&g_watch_stage_idx);
    if (stage && *stage) snprintf(g_watch_stage[next], sizeof g_watch_stage[next], "%s", stage);
    atomic_store(&g_watch_tick, tick);
    atomic_store(&g_watch_stage_idx, next);
    atomic_fetch_add(&g_watch_count, 1);
    atomic_store(&g_watch_last_ns, watch_now_ns());
}
void crash_watch_frame(void) { if (atomic_load(&g_watch_pregame)) crash_watch_progress(0, "出图帧"); }
void crash_watch_pregame_done(void) { atomic_store(&g_watch_pregame, 0); }
void crash_watch_pregame_again(void) { atomic_store(&g_watch_pregame, 1); }   // 从局里退回前端：重新按出图帧算进展（diag.c） | Back from a run to the front end: count progress by presented frames again (diag.c)
void crash_dump_now(const char *why);
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        usleep(250000);
        // 先读最后进展时刻、再读现在：反过来读时，两次读之间游戏线程刚好记了一次进展，last 会比 now 新，now − last 无符号下溢成「第 18446744073 秒起无进展」，
        // 当场误判卡死退出（accept inv_drag_out，本机负载 11、看门狗线程两次读之间被切走）。这样读 last ≤ now；万一时钟回绕也不判
        // Read the last progress time first, then read now: reading in the opposite order, if the game thread happens to record progress between the two reads, last is newer than now and now − last underflows as unsigned into 「第 18446744073 秒起无进展」(no progress since second 18446744073),
        // which is misjudged on the spot as a hang and exits (accept inv_drag_out, local load 11, the watchdog thread was switched out between the two reads). Reading this way gives last ≤ now; and if the clock ever wraps around it is not judged either
        uint64_t last = atomic_load(&g_watch_last_ns), now = watch_now_ns();
        if (last > now) continue;
        // 秒级卡顿（进展停了 1 秒以上、还没到判死的阈值）：不退出，把各线程在等什么导一份。撞到「每 2 秒才出一帧」的现场，15 秒阈值抓不到、日志里没有它在等什么。
        // 进局之后才导；每个进程最多 3 份、两份之间至少隔 10 秒（读取画面、暂停菜单也会停拍，别让它们刷屏）
        // Second-scale stall (progress has stopped for more than 1 second but has not yet reached the kill threshold): do not exit, dump what each thread is waiting on. When hitting a case of "only one frame every 2 seconds", the 15-second threshold cannot catch it and the log has nothing on what it is waiting for.
        // Dump only after entering a run; at most 3 dumps per process, at least 10 seconds between two dumps (the loading screen and the pause menu also stop ticks, do not let them flood the log)
        { static int dumps; static uint64_t last_dump, dumped_for;
          if (g_watch_stall && !atomic_load(&g_watch_pregame) && atomic_load(&g_watch_tick) > 0 && now - last >= 1000000000ull && now - last < (uint64_t)g_watch_stall * 1000000000ull
              && dumps < 3 && dumped_for != last && (!last_dump || now - last_dump >= 10000000000ull)) { dumps++; last_dump = now; dumped_for = last; crash_dump_now("进展停了 1 秒以上"); } }
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            uint64_t sec = (now - last) / 1000000000ull;
            int idx = atomic_load(&g_watch_stage_idx);
            LOG("【红】看门狗：第 %llu 秒起无进展，卡在 拍 %u / 阶段 %s", (unsigned long long)sec,
                atomic_load(&g_watch_tick), g_watch_stage[idx]);
            DM_EV("watchdog", "stalled_s:u stage:s", (unsigned long long)sec, g_watch_stage[idx]);
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();   // 图形侧所有阻塞等待点的状态（nvn_mtl.m 初始化时注册） | State of all blocking wait points on the graphics side (registered when nvn_mtl.m initializes)
#if TARGET_OS_OSX
            { const char *d = getenv("DM_WATCH_SAMPLE");   // Mac 诊断：卡住时对自己采样 1 秒，线程栈写到该路径 | Mac diagnostics: when stuck, sample itself for 1 second and write the thread stacks to this path
              if (d) { char c[1200]; snprintf(c, sizeof c, "/usr/bin/sample %d 1 -f '%s' >/dev/null 2>&1", (int)getpid(), d); if (system(c)) LOG("看门狗：采样失败"); else LOG("看门狗：线程栈已写 %s", d); } }
#endif
            DM_EV("exit", "why:s code:d", "watchdog", 128 + SIGALRM);
            _exit(128 + SIGALRM);
        }
    }
    return NULL;
}
// 往定长缓冲里追加（满了就截断，不越界）| Append to a fixed buffer (truncated when full, never past the end)
typedef struct { char *b; size_t n, cap; } crash_buf;
__attribute__((format(printf, 2, 3))) static void cb_add(crash_buf *c, const char *fmt, ...) {
    if (c->n + 1 >= c->cap) return;
    va_list ap; va_start(ap, fmt); int k = vsnprintf(c->b + c->n, c->cap - c->n, fmt, ap); va_end(ap);
    if (k > 0) c->n = c->n + (size_t)k < c->cap ? c->n + (size_t)k : c->cap - 1;
}
// 一个地址写成「 标签=模块+偏移」（不在客体模块里就用 dladdr 的符号，再不行打原值） | One address as " tag=module+offset" (outside the guest modules dladdr's symbol, else the raw value)
static void cb_where(crash_buf *c, const char *tag, uint64_t a) {
    module_t *m = module_of((void *)a);
    Dl_info di;
    if (m) cb_add(c, " %s%s%s+%#llx", tag, *tag ? "=" : "", m->name, a - (uint64_t)m->base);
    else if (dladdr((void *)a, &di) && di.dli_sname) cb_add(c, " %s%s%s+%#lx", tag, *tag ? "=" : "", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else cb_add(c, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
void crash_off(const char *tag, uint64_t a) {   // diag.c 也用 | Also used by diag.c
#define off crash_off

    module_t *m = module_of((void *)a);
    Dl_info di;
    if (m) fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", m->name, a - (uint64_t)m->base);
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s%s%s+%#lx", tag, *tag ? "=" : "", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
// 帧指针回溯：x29 链 [fp]=上一帧 fp、[fp+8]=返回地址；只走 8 字节对齐、单调上升的帧。dm_backtrace 也在客体小栈上调（Break），逐段打、不占大缓冲；cb_backtrace 是崩溃路径用的缓冲版
// Frame pointer backtrace: x29 chain [fp]=previous frame's fp, [fp+8]=return address; only 8-byte aligned, monotonically increasing frames are walked. dm_backtrace is also called on small guest stacks (Break), so it prints piece by piece without a big buffer; cb_backtrace is the buffered one for the crash path
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t nfp = ((uint64_t *)fp)[0], ra = ((uint64_t *)fp)[1];
        off("", ra);
        if (nfp <= fp) break;
        fp = nfp;
    }
    fputc('\n', stderr);
}
static void cb_backtrace(crash_buf *c, uint64_t fp) {
    cb_add(c, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t nfp = ((uint64_t *)fp)[0], ra = ((uint64_t *)fp)[1];
        cb_where(c, "", ra);
        if (nfp <= fp) break;
        fp = nfp;
    }
    cb_add(c, "\n");
}
// 崩溃路径（on_crash）与现场导出标题行的输出：整段 write(2)，不经 stdio（出错的那一刻 stderr 的锁可能在别的线程手里） | Output of the crash path (on_crash) and of the state dump's header line: written whole with write(2), not through stdio (at the moment of the fault another thread may hold stderr's lock)
static void cb_flush(crash_buf *c) {
    const char *p = c->b; size_t n = c->n;
    while (n) { ssize_t k = write(2, p, n); if (k < 0 && errno == EINTR) continue; if (k <= 0) break; p += k; n -= (size_t)k; }
    c->n = 0; c->b[0] = 0;
}
// ---------- 缺页钩子（写保护脏页跟踪） ----------
// on_crash 打印现场前按注册顺序调用；返回 1 = 已处理（直接返回、重执行出错指令），全部返回 0 才走崩溃路径。
// 钩子运行在信号处理上下文：只许查表 / 原子操作 / mprotect，不许分配内存、加锁。
// ---------- Page fault hooks (write-protect dirty page tracking) ----------
// Called in registration order before on_crash prints the crash state; returning 1 = handled (return directly and re-execute the faulting instruction); the crash path is taken only when all return 0.
// Hooks run in signal handler context: only table lookups / atomic operations / mprotect are allowed, no memory allocation and no locking.
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault_hook >= MAX_FAULT_HOOK) die("缺页钩子过多");
    g_fault_hook[g_nfault_hook++] = fn;
}
static void (*g_res_report)(uint32_t tick, const char *why);   // 资源计数回调（图形层注册，diag.c 按拍 / 事件调用） | Resource count callback (registered by the graphics layer, called by diag.c per tick / event)
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why)) { g_res_report = fn; }
void (*dm_res_report_cb(void))(uint32_t, const char *) { return g_res_report; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (g_nwrite_prep >= MAX_FAULT_HOOK) die("宿主写前钩子过多");
    g_write_prep[g_nwrite_prep++] = fn;
}
// 宿主自己写客体内存（read() 直读进客体缓冲等）之前调用：写保护页不会对系统调用触发缺页、而是返回 EFAULT
// Called before the host itself writes guest memory (read() straight into a guest buffer, etc.): for a system call a write-protected page does not trigger a page fault but returns EFAULT
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nwrite_prep; i++) g_write_prep[i](addr, len); }

// 诊断模块（diag.c 的探针）与线程现场（hle_thread.c）由各自注册进来：单测只链本文件时不依赖它们
// The diagnostics module (the probes in diag.c) and the thread state dump (hle_thread.c) each register themselves here: a unit test that links only this file does not depend on them
static int (*g_trap_hook)(ucontext_t *uc);
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)) { g_trap_hook = fn; }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
// 按需导出现场（不退出）：iOS 包没有无进展看门狗，冒烟判红时经 DM_LOG_PORT 发 dump 触发——各客体线程在等什么 + 图形侧等待点，和看门狗打的是同一份
// Dump the current state on demand (without exiting): the iOS app has no no-progress watchdog, so when the smoke test fails it sends dump through DM_LOG_PORT to trigger this -- what each guest thread is waiting on + the graphics-side wait points, the same thing the watchdog prints
void crash_dump_now(const char *why) {
    char b[512]; crash_buf c = {b, 0, sizeof b}; b[0] = 0;
    cb_add(&c, "[dm] 现场导出（%s）：拍 %u / 阶段 %s\n", why, atomic_load(&g_watch_tick), g_watch_stage[atomic_load(&g_watch_stage_idx)]); cb_flush(&c);
    if (g_dump_hook) g_dump_hook();
    if (g_wait_dump) g_wait_dump();
}
// ---------- crash.json（DM_CRASH_JSON=<路径>）：结构化现场，给脚本 / 代理读 ----------
// 信号处理里只用异步信号安全的调用（open / write / close）、静态缓冲与手写的数字格式化，不用 printf / malloc / dladdr，所以在打日志现场之前先写（日志那段可能卡住）。
// 内容：信号、si_code、出错地址、pc / lr / sp / fp 及其模块内偏移、x0–x28、帧指针回溯（每帧模块 + 偏移）、各客体模块的装载范围、宿主映像基址（atos -l 用）、看门狗记的最后拍号与阶段
// ---------- crash.json (DM_CRASH_JSON=<path>): structured crash state for scripts / agents ----------
// The signal handler only uses async-signal-safe calls (open / write / close), a static buffer and hand-written number formatting, no printf / malloc / dladdr, so it is written before the log dump (which may hang).
// Contents: signal, si_code, fault address, pc / lr / sp / fp with their module offsets, x0-x28, a frame-pointer backtrace (module + offset per frame), each guest module's load range, the host image base (for atos -l), and the watchdog's last tick and stage
#include <fcntl.h>
#include <mach-o/dyld.h>
static const char *g_crash_json;
static uint64_t g_host_base;
static const module_t *g_cmods; static int g_cnmod;   // 装载器装完模块后交过来（main.c）：本文件不链装载器（图形单测只链本文件） | handed over by the loader after the modules are loaded (main.c): this file does not link the loader (the graphics unit tests link only this file)
void crash_set_modules(const module_t *m, int n) { g_cmods = m; g_cnmod = n; }
static char g_cj[32768]; static size_t g_cjn;
static void cj(const char *s) { while (*s && g_cjn < sizeof g_cj - 1) g_cj[g_cjn++] = *s++; }
static void cj_hex(uint64_t v) { char t[24]; int n = 0; do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); cj("\"0x"); while (n) { char c[2] = {t[--n], 0}; cj(c); } cj("\""); }
static void cj_dec(int64_t v) { char t[24]; int n = 0; uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v; do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u); if (v < 0) cj("-"); while (n) { char c[2] = {t[--n], 0}; cj(c); } }
static void cj_str(const char *s) { cj("\""); for (; s && *s && g_cjn < sizeof g_cj - 8; s++) { unsigned char c = (unsigned char)*s; if (c == '"' || c == '\\') { char e[3] = {'\\', (char)c, 0}; cj(e); } else if (c >= 0x20) { char e[2] = {(char)c, 0}; cj(e); } } cj("\""); }
static void cj_where(uint64_t a) { module_t *m = module_of((void *)a); if (!m) { cj("null"); return; } cj("\""); cj(m->name); cj("+0x"); { uint64_t v = a - (uint64_t)m->base; char t[24]; int n = 0; do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); while (n) { char c[2] = {t[--n], 0}; cj(c); } } cj("\""); }
static void crash_json(int sig, siginfo_t *si, ucontext_t *uc) {
    if (!g_crash_json) return;
    g_cjn = 0;
    cj("{\"v\":1,\"sig\":"); cj_dec(sig); cj(",\"code\":"); cj_dec(si->si_code); cj(",\"addr\":"); cj_hex((uint64_t)si->si_addr);
    cj(",\"pc\":"); cj_hex(UC_PC(uc)); cj(",\"pc_where\":"); cj_where(UC_PC(uc));
    cj(",\"lr\":"); cj_hex(UC_LR(uc)); cj(",\"lr_where\":"); cj_where(UC_LR(uc));
    cj(",\"sp\":"); cj_hex(UC_SP(uc)); cj(",\"fp\":"); cj_hex(UC_FP(uc));
    cj(",\"x\":["); for (int i = 0; i < 29; i++) { if (i) cj(","); cj_hex(UC_X(uc, i)); } cj("]");
    cj(",\"bt\":[");
    { uint64_t fp = UC_FP(uc); int first = 1;   // 同 dm_backtrace：只走 8 字节对齐、单调上升的帧 | as dm_backtrace: only 8-byte aligned, monotonically increasing frames
      for (int i = 0; i < 48 && fp && !(fp & 7); i++) { uint64_t nfp = ((uint64_t *)fp)[0], ra = ((uint64_t *)fp)[1];
          if (!first) cj(","); first = 0; cj("{\"pc\":"); cj_hex(ra); cj(",\"where\":"); cj_where(ra); cj("}");
          if (nfp <= fp) break; fp = nfp; } }
    cj("],\"modules\":[");
    for (int i = 0; i < g_cnmod; i++) { if (i) cj(","); cj("{\"name\":"); cj_str(g_cmods[i].name); cj(",\"base\":"); cj_hex((uint64_t)g_cmods[i].base); cj(",\"size\":"); cj_dec((int64_t)g_cmods[i].size); cj("}"); }
    cj("],\"host_base\":"); cj_hex(g_host_base);
    cj(",\"tick\":"); cj_dec(atomic_load(&g_watch_tick)); cj(",\"stage\":"); cj_str(g_watch_stage[atomic_load(&g_watch_stage_idx)]);
    cj("}\n");
    int fd = open(g_crash_json, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { (void)!write(fd, g_cj, g_cjn); close(fd); }
}
static void on_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    if (sig == SIGTRAP && g_trap_hook && g_trap_hook(uc)) return;
    if (sig == SIGSEGV || sig == SIGBUS) {
        int is_write = (int)((UC_ESR(uc) >> 6) & 1);   // ESR 的 WnR 位 | The WnR bit of ESR
        for (int i = 0; i < g_nfault_hook; i++) if (g_fault_hook[i](si->si_addr, is_write, uc)) return;
    }
    if (g_dm_event_crash_fn) { module_t *m = module_of((void *)UC_PC(uc)); g_dm_event_crash_fn(sig, UC_PC(uc), m ? m->name : NULL, m ? UC_PC(uc) - (uint64_t)m->base : 0, (uint64_t)si->si_addr); }   // 事件先出（现场打印可能卡住） | the event goes out first (printing the state may hang)
    crash_json(sig, si, uc);   // 结构化现场先落盘（异步信号安全） | the structured state is written first (async-signal-safe)
    { char b[4096]; crash_buf c = {b, 0, sizeof b}; b[0] = 0;   // 信号备用栈 64 KiB | the signal alternate stack is 64 KiB
      cb_add(&c, "[dm] CRASH sig=%d", sig);
      cb_where(&c, "addr", (uint64_t)si->si_addr); cb_where(&c, "pc", UC_PC(uc)); cb_where(&c, "lr", UC_LR(uc));
      cb_add(&c, " sp=%#llx\n", UC_SP(uc));
      for (int i = 0; i < 29; i++) cb_add(&c, "x%d=%#llx%s", i, UC_X(uc, i), i % 6 == 5 ? "\n" : " ");
      cb_add(&c, "\n"); cb_flush(&c);
      cb_backtrace(&c, UC_FP(uc)); cb_flush(&c); }
    if (g_dump_hook) g_dump_hook();
    // 所有致命信号都恢复缺省处理再发一次：进程照旧按这个信号结束，系统的崩溃报告（.ips）照写（原先只 SIGABRT 这样，别的 _exit 掉、系统那边没有报告）；shell 看到的退出码仍是 128 + 信号
    // Every fatal signal restores the default action and is raised again: the process still ends by this signal and the system still writes its crash report (.ips) (previously only SIGABRT did this, the others _exit-ed with no system report); the exit code a shell sees is still 128 + signal
    signal(sig, SIG_DFL); raise(sig);
    _exit(128 + sig);
}
// 信号备用栈是每线程的：每个客体线程起跑时都要调（栈溢出时才能在备用栈上打出现场）
// 备用栈随线程退出释放：pthread 键的析构在线程退出时调用（先停用备用栈再 free）。游戏会频繁起短命线程，不释放每个漏 64 KiB。
// The signal alternate stack is per thread: every guest thread must call this when it starts running (only then can the crash state be printed on the alternate stack on a stack overflow)
// The alternate stack is released when the thread exits: the pthread key destructor is called at thread exit (disable the alternate stack first, then free). The game frequently starts short-lived threads, and without releasing each one leaks 64 KiB.
#if !TARGET_OS_TV   // tvOS 的 SDK 没有 sigaltstack：不设备用栈，崩溃处理在原栈上跑（栈溢出时打不出现场） | The tvOS SDK has no sigaltstack: no alternate stack is set, so the crash handler runs on the original stack (no crash state can be printed on stack overflow)
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, 0); free(p); }
static void altstack_key_init(void) { if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key"); }
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key)) return;                 // 同一线程重复调用不重复分配 | Repeated calls on the same thread do not allocate again
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}
#define CRASH_SA_ONSTACK SA_ONSTACK
#else
void crash_thread_init(void) {}
#define CRASH_SA_ONSTACK 0
#endif
void crash_install(void) {
    crash_thread_init();
    { const char *e = getenv("DM_CRASH_JSON"); if (e && *e) g_crash_json = strdup(e); g_host_base = (uint64_t)(uintptr_t)_dyld_get_image_header(0); }
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | CRASH_SA_ONSTACK};
    sigaction(SIGALRM, &sa, 0);
    // 两种看门狗都只在测试显式设了 DM_WATCH / DM_WATCH_STALL 时开：产品（玩家从桌面启动，无环境变量）一个不开。
    // 读代码查到：iOS main.m 缺省 setenv DM_WATCH=30 → 玩家启动 30 s 被 SIGALRM 杀；无进展看门狗缺省 15 s → 暂停菜单（拍停）15 s 被杀
    // Both watchdogs are turned on only when a test explicitly sets DM_WATCH / DM_WATCH_STALL: the product (the player launches from the desktop, no environment variables) turns on neither.
    // Found by reading the code: iOS main.m did setenv DM_WATCH=30 by default → the player's launch was killed by SIGALRM at 30 s; the no-progress watchdog defaulted to 15 s → the pause menu (ticks stopped) was killed at 15 s
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch) alarm(atoi(watch));
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, watch_now_ns());
        pthread_t th; if (pthread_create(&th, NULL, watchdog_main, NULL)) die("看门狗线程");
        pthread_detach(th);
    }
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGABRT, &sa, 0);   // die() 走 abort()：原先不接，FATAL 那行之后没有寄存器、回溯和线程现场 | die() goes through abort(): previously not caught, so a FATAL line came with no registers, backtrace or thread state
}
