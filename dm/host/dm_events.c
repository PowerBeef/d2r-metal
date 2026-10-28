// 事件流 v1：开关与格式见 dm_events.h，事件类型表见 dm/verify/event_stream.md。
// 一个事件 = 一次 write()（O_APPEND），多线程同时发也不会半行交错；序号在锁里取，文件里按序号递增。
// Event stream v1: switches and format in dm_events.h, the table of event types in dm/verify/event_stream.md.
// One event = one write() (O_APPEND), so threads emitting at the same time never interleave half lines; the sequence number is taken under the lock, so it increases through the file.
#include "dm_events.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <os/log.h>
#include <sys/sysctl.h>
#include <TargetConditionals.h>
#endif

uint64_t now_ns(void);

static int g_fd = -1, g_to_log;
static uint64_t g_t0;
static unsigned g_seq;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#ifdef __APPLE__
static os_log_t g_oslog;
#endif

typedef struct { char *p; size_t n, cap; } buf_t;   // 写满即截断（末尾留给「}\n」） | truncates when full (room is kept for "}\n" at the end)
static void put(buf_t *b, const char *s, size_t k) { if (b->n + k + 3 > b->cap) k = b->cap > b->n + 3 ? b->cap - b->n - 3 : 0; memcpy(b->p + b->n, s, k); b->n += k; }
static void puts_(buf_t *b, const char *s) { put(b, s, strlen(s)); }
static void putf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(buf_t *b, const char *fmt, ...) {
    char t[64]; va_list ap; va_start(ap, fmt); int k = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (k > 0) put(b, t, (size_t)k < sizeof t ? (size_t)k : sizeof t - 1);
}
static void put_str(buf_t *b, const char *s, size_t max) {   // JSON 串；max 字节处截断时退到 UTF-8 字符边界 | JSON string; when cut at max bytes, back off to a UTF-8 character boundary
    if (!s) { puts_(b, "null"); return; }
    size_t n = strlen(s); if (n > max) { n = max; while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--; }
    puts_(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; put(b, e, 2); }
        else if (c == '\n') puts_(b, "\\n");
        else if (c == '\t') puts_(b, "\\t");
        else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); puts_(b, e); }
        else put(b, (const char *)&c, 1);
    }
    puts_(b, "\"");
}

static char g_early[16384]; static size_t g_early_n;
void dm_events_set_sink(void (*fn)(const char *line, size_t n)) {
    pthread_mutex_lock(&g_mu);
    g_dm_event_sink = fn;
    for (size_t i = 0, j = 0; fn && j < g_early_n; j++) if (g_early[j] == '\n') { fn(g_early + i, j + 1 - i); i = j + 1; }
    g_early_n = 0;
    pthread_mutex_unlock(&g_mu);
}
static void head(buf_t *b, const char *ev) {
    uint64_t ms = (now_ns() - g_t0) / 1000000;   // 截到毫秒（与崩溃事件同一算法，序号与时刻一起单调） | truncated to milliseconds (same arithmetic as the crash event, so time is monotonic along with seq)
    putf(b, "{\"seq\":%u,\"t\":%llu.%03llu,\"ev\":", g_seq++, (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    put_str(b, ev, 64);
}
static void flush(buf_t *b) {   // 持 g_mu 调 | called with g_mu held
    if (g_dm_event_tick_fn) { long long t = g_dm_event_tick_fn(); if (t >= 0) putf(b, ",\"tick\":%lld", t); }
    memcpy(b->p + b->n, "}\n", 2); b->n += 2;
    if (g_fd >= 0) { ssize_t w; do w = write(g_fd, b->p, b->n); while (w < 0 && errno == EINTR); }
    if (g_to_log) { flockfile(stderr); fputs("[dm] EV ", stderr); fwrite(b->p, 1, b->n, stderr); funlockfile(stderr); }
    if (g_dm_event_sink) g_dm_event_sink(b->p, b->n);   // 控制口（dm_ctl.c） | the control port (dm_ctl.c)
    else if (g_early_n + b->n <= sizeof g_early) { memcpy(g_early + g_early_n, b->p, b->n); g_early_n += b->n; }   // 出口装上之前的事件先留着（start 等），装上时交过去 | events before the outlet is installed (start etc.) are kept and handed over when it is
#ifdef __APPLE__
    if (g_oslog) { b->p[b->n - 1] = 0; os_log(g_oslog, "%{public}s", b->p); }
#endif
}

static void fields(buf_t *b_, const char *sig, va_list ap) {   // 按签名写「,"名":值」 | writes ,"name":value per the signature
#define b (*b_)
    for (const char *s = sig ? sig : ""; *s;) {
        while (*s == ' ') s++;
        const char *c = strchr(s, ':'); if (!c || !c[1]) break;
        puts_(&b, ","); { char k[48]; size_t kn = (size_t)(c - s) < sizeof k - 1 ? (size_t)(c - s) : sizeof k - 1; memcpy(k, s, kn); k[kn] = 0; put_str(&b, k, sizeof k); } puts_(&b, ":");
        switch (c[1]) {
            case 's': put_str(&b, va_arg(ap, const char *), 400); break;
            case 'd': putf(&b, "%d", va_arg(ap, int)); break;
            case 'b': puts_(&b, va_arg(ap, int) ? "true" : "false"); break;
            case 'l': putf(&b, "%lld", va_arg(ap, long long)); break;
            case 'u': putf(&b, "%llu", va_arg(ap, unsigned long long)); break;
            case 'x': putf(&b, "\"%#llx\"", va_arg(ap, unsigned long long)); break;
            case 'f': putf(&b, "%.15g", va_arg(ap, double)); break;
            default: puts_(&b, "null"); goto out;   // 签名写错：后面的值类型不明，不再往下读 | bad signature: the types of the remaining values are unknown, stop reading
        }
        s = c + 2;
    }
out:;
#undef b
}
static void ev_fn(const char *ev, const char *sig, ...) {
    char mem[2048]; buf_t b = {mem, 0, sizeof mem};
    va_list ap; va_start(ap, sig);
    pthread_mutex_lock(&g_mu);
    head(&b, ev);
    fields(&b, sig, ap);
    va_end(ap);
    flush(&b);
    pthread_mutex_unlock(&g_mu);
}
size_t dm_json_fields(char *out, size_t cap, const char *sig, ...) {
    buf_t b = {out, 0, cap}; if (cap < 4) { if (cap) out[0] = 0; return 0; }
    va_list ap; va_start(ap, sig); fields(&b, sig, ap); va_end(ap);
    out[b.n] = 0; return b.n;
}
void dm_json_str(char *out, size_t cap, const char *s) { buf_t b = {out, 0, cap}; if (cap < 4) { if (cap) out[0] = 0; return; } put_str(&b, s, cap); out[b.n] = 0; }

// 日志钩子：整行先格式化进缓冲（长行另分配），一次写 stderr；带红标 / FATAL 的行另发 red / fatal 事件
// Log hook: the whole line is first formatted into a buffer (long lines are allocated separately) and written to stderr at once; lines with a red marker / FATAL also emit a red / fatal event
static uint32_t fnv1a(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
static void log_hook(const char *file, int line, const char *fmt, ...) {
    char mem[1024], *p = mem; va_list ap, aq; va_start(ap, fmt); va_copy(aq, ap);
    int n = vsnprintf(mem, sizeof mem, fmt, ap);
    if (n >= (int)sizeof mem && (p = malloc((size_t)n + 1))) vsnprintf(p, (size_t)n + 1, fmt, aq);
    else if (n >= (int)sizeof mem) p = mem;   // 分配失败：写截断的那一行 | allocation failed: write the truncated line
    va_end(aq); va_end(ap);
    flockfile(stderr); fputs(p, stderr); fputc('\n', stderr); funlockfile(stderr);
    const char *msg = strncmp(p, "[dm] ", 5) ? p : p + 5;
    int fatal = !strncmp(msg, "FATAL ", 6) || strstr(msg, "] FATAL ") != NULL;
    if (fatal || strstr(p, "【红】") || strstr(p, "🔴")) {
        char src[96], code[12]; snprintf(src, sizeof src, "%s:%d", file, line); snprintf(code, sizeof code, "%08x", fnv1a(fmt));
        ev_fn(fatal ? "fatal" : "red", "code:s src:s msg:s", code, src, msg);
    }
    if (p != mem) free(p);
}

// 崩溃事件：只用栈上缓冲与 write()，不取 g_mu（被打断的线程可能正持着它）；序号读一下不加锁
// Crash event: only a stack buffer and write(), without taking g_mu (the interrupted thread may be holding it); the sequence number is read without the lock
static char *u2s(char *e, unsigned long long v, int base) { do { *--e = "0123456789abcdef"[v % (unsigned)base]; v /= (unsigned)base; } while (v); return e; }
static void crash_fn(int sig, uint64_t pc, const char *mod, uint64_t off, uint64_t addr) {
    char out[256], t[24]; size_t n = 0; t[sizeof t - 1] = 0;   // u2s 从 t 末尾往前写 | u2s writes backwards from the end of t
#define CAT(s) do { const char *s_ = (s); size_t k_ = strlen(s_); if (n + k_ < sizeof out - 2) { memcpy(out + n, s_, k_); n += k_; } } while (0)
    CAT("{\"seq\":"); CAT(u2s(t + sizeof t - 1, g_seq, 10));
    CAT(",\"t\":"); { uint64_t ms = (now_ns() - g_t0) / 1000000; CAT(u2s(t + sizeof t - 1, ms / 1000, 10)); CAT("."); char *f = u2s(t + sizeof t - 1, ms % 1000 + 1000, 10); CAT(f + 1); }
    CAT(",\"ev\":\"crash\",\"sig\":"); CAT(u2s(t + sizeof t - 1, (unsigned)sig, 10));
    CAT(",\"pc\":\"0x"); CAT(u2s(t + sizeof t - 1, pc, 16)); CAT("\"");
    if (mod) { CAT(",\"where\":\""); CAT(mod); CAT("+0x"); CAT(u2s(t + sizeof t - 1, off, 16)); CAT("\""); }
    CAT(",\"addr\":\"0x"); CAT(u2s(t + sizeof t - 1, addr, 16)); CAT("\"}\n");
#undef CAT
    if (g_fd >= 0) (void)!write(g_fd, out, n);
    if (g_to_log) { (void)!write(2, "[dm] EV ", 8); (void)!write(2, out, n); }
}

static uint64_t start_t0(void) {   // 零点 = 进程创建时刻（同 main.c 启动时间线与 UIMSG 的 [秒]）；取不到用现在 | zero point = process creation (same as main.c's boot timeline and the [seconds] of UIMSG); now when unavailable
    uint64_t mono = now_ns();
#ifdef __APPLE__
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()}; struct timeval tv; gettimeofday(&tv, NULL);
    if (!sysctl(mib, 4, &kp, &n, NULL, 0) && n) {
        int64_t age = (int64_t)(tv.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000000000ll + (tv.tv_usec - kp.kp_proc.p_starttime.tv_usec) * 1000ll;
        if (age > 0 && (uint64_t)age < mono) return mono - (uint64_t)age;
    }
#endif
    return mono;
}

static void ev_atexit(void) { ev_fn("exit", "why:s", "exit"); }
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init_once(void) {
    const char *e = getenv("DM_EVENTS"), *ol = getenv("DM_EVENTS_OSLOG"), *cp = getenv("DM_CTL_PORT");
    int want_os = ol && *ol && strcmp(ol, "0");
    if ((!e || !*e) && !want_os && !(cp && *cp)) return;   // 控制口要事件推给客户端：设了它也开（不写文件） | the control port pushes events to its client: setting it turns events on too (no file)
    g_t0 = start_t0();
    if (e && (!strcmp(e, "log") || !strcmp(e, "-"))) g_to_log = 1;
    else if (e && *e) {
        char path[1024]; const char *home = getenv("HOME");
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        if (e[0] != '/' && home) snprintf(path, sizeof path, "%s/Documents/%s", home, e); else
#endif
        snprintf(path, sizeof path, "%s", e);
        (void)home;
        g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
        if (g_fd < 0) fprintf(stderr, "[dm] 【红】DM_EVENTS 打不开 %s（errno=%d）：事件不写文件 | cannot open, events are not written to a file\n", path, errno);
    }
#ifdef __APPLE__
    if (want_os) g_oslog = os_log_create("org.d2rmobile", "events");
#endif
    g_dm_event_fn = ev_fn; g_dm_log_hook = log_hook; g_dm_event_crash_fn = crash_fn;
    char pid[16]; snprintf(pid, sizeof pid, "%d", (int)getpid());
    struct timeval tv; gettimeofday(&tv, NULL);
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    const char *plat = "ios";
#else
    const char *plat = "mac";
#endif
    ev_fn("start", "v:d pid:s platform:s wall:f", 1, pid, plat, (double)tv.tv_sec + tv.tv_usec / 1e6);
    atexit(ev_atexit);   // exit() 走这里；_exit 的路径（看门狗、拍数记满、优雅退出）自己先发 exit | exit() comes here; the _exit paths (watchdog, tick count reached, graceful exit) emit exit themselves first
}
void dm_events_init(void) { pthread_once(&g_once, init_once); }
double dm_events_now_s(void) {   // 与事件的 t 同一时钟（进程创建以来的秒；事件流没开时也算得出） | the same clock as the events' t (seconds since process creation; works without the event stream too)
    static uint64_t t0; if (!t0) t0 = g_t0 ? g_t0 : start_t0();
    return (double)(now_ns() - t0) / 1e9;
}
