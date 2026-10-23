// dm_events.c 的单测：各字段类型与转义、日志钩子（红标 / FATAL / 长行）、拍号、崩溃事件、多线程不交错、exit 事件、DM_EVENTS=log。
// Unit test for dm_events.c: field types and escaping, the log hook (red marker / FATAL / long lines), tick, crash event, no interleaving across threads, the exit event, DM_EVENTS=log.
#include "dm.h"
#include <fcntl.h>
#include <mach/mach_time.h>
#include <sys/wait.h>
#include <unistd.h>

uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char *slurp(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return strdup("");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); b[fread(b, 1, (size_t)n, f)] = 0; fclose(f); return b;
}
static int count(const char *s, const char *needle) { int n = 0; for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }

static long long tick42(void) { return 42; }
static void *spam(void *a) { for (int i = 0; i < 500; i++) DM_EV("spam", "th:d i:d pad:s", (int)(intptr_t)a, i, "0123456789012345678901234567890123456789"); return NULL; }

int main(void) {
    char dir[] = "/tmp/t_events.XXXXXX"; if (!mkdtemp(dir)) return 2;
    char ev[256], err[256], ex[256], lg[256];
    snprintf(ev, sizeof ev, "%s/ev.ndjson", dir); snprintf(err, sizeof err, "%s/stderr.txt", dir); snprintf(ex, sizeof ex, "%s/exit.ndjson", dir); snprintf(lg, sizeof lg, "%s/log.txt", dir);

    // 子进程：exit() 走 atexit 发 exit 事件；DM_EVENTS=log 时事件行进 stderr | Child processes: exit() emits the exit event via atexit; with DM_EVENTS=log the event lines go to stderr
    pid_t c = fork();
    if (!c) { setenv("DM_EVENTS", ex, 1); dm_events_init(); DM_EV("hello", ""); exit(3); }
    int st; waitpid(c, &st, 0);
    char *e1 = slurp(ex);
    CHECK(count(e1, "\"ev\":\"start\"") == 1 && count(e1, "\"ev\":\"hello\"") == 1 && strstr(e1, "\"ev\":\"exit\",\"why\":\"exit\"}\n"), "exit 事件 | exit event: %s", e1);
    c = fork();
    if (!c) { int fd = open(lg, O_WRONLY | O_CREAT | O_TRUNC, 0644); dup2(fd, 2); setenv("DM_EVENTS", "log", 1); dm_events_init(); LOG("普通一行"); _exit(0); }
    waitpid(c, &st, 0);
    char *l1 = slurp(lg);
    CHECK(strstr(l1, "[dm] EV {\"seq\":0,") && strstr(l1, "[dm] 普通一行\n") && count(l1, "[dm] EV ") == 1, "DM_EVENTS=log: %s", l1);

    // 没设 DM_EVENTS 时指针全空 | Without DM_EVENTS all pointers stay null
    CHECK(!g_dm_event_fn && !g_dm_log_hook && !g_dm_event_crash_fn, "未开时指针应为空 | pointers should be null when off");

    setenv("DM_EVENTS", ev, 1); unsetenv("DM_EVENTS_OSLOG");
    int fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644), saved = dup(2); dup2(fd, 2); close(fd);
    dm_events_init(); dm_events_init();   // 第二次不生效 | the second call has no effect
    CHECK(g_dm_event_fn && g_dm_log_hook && g_dm_event_crash_fn, "开了以后指针应已填 | pointers should be set");
    DM_EV("types", "s:s d:d l:l u:u x:x f:f b:b n:s", "a\"b\\c\n\x01中文", -5, -1234567890123LL, 18446744073709551615ULL, 0xabcULL, 1.5, 1, (const char *)NULL);
    LOG("普通一行 %d", 1);
    int red_line = __LINE__ + 1;
    LOG("【红】测试 %d", 7);
    LOG("%s", "参数里带 🔴 也算");
    g_dm_log_hook(__FILE_NAME__, __LINE__, "[dm] [nvn-mtl] FATAL boom %d", 9);
    char big[3000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    LOG("长行 %s 尾", big);
    char han[1500] = ""; for (int i = 0; i < 160; i++) strcat(han, "汉");   // 480 字节：400 处截到字符边界 = 399 字节 133 个字 | 480 bytes: cut at 400 on a character boundary = 399 bytes, 133 characters
    DM_EV("trunc", "s:s", han);
    DM_EV("badsig", "a:z b:d", 1, 2);
    g_dm_event_tick_fn = tick42; DM_EV("ticked", ""); g_dm_event_tick_fn = 0;
    g_dm_event_crash_fn(11, 0x1234, "main", 0x10, 0x8);
    g_dm_event_crash_fn(6, 0xabc, NULL, 0, 0);
    pthread_t th[4]; for (intptr_t i = 0; i < 4; i++) pthread_create(&th[i], NULL, spam, (void *)i);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    fflush(stderr); dup2(saved, 2);

    char *s = slurp(ev), *o = slurp(err);
    CHECK(strstr(s, "\"ev\":\"types\",\"s\":\"a\\\"b\\\\c\\n\\u0001中文\",\"d\":-5,\"l\":-1234567890123,\"u\":18446744073709551615,\"x\":\"0xabc\",\"f\":1.5,\"b\":true,\"n\":null}\n"), "字段类型 | field types");
    CHECK(!strstr(s, "普通一行"), "普通日志行不是事件 | plain log lines are not events");
    { const char *r = strstr(s, "\"ev\":\"red\",\"code\":\""); unsigned code = 0; int k = 0;   // code = 8 位十六进制 | code = 8 hex digits
      CHECK(r && sscanf(r + 19, "%8x\"%n", &code, &k) == 1 && k == 9, "red 事件的 code | code of the red event"); }
    { char src[64]; snprintf(src, sizeof src, "\"src\":\"t_events.c:%d\",\"msg\":\"【红】测试 7\"}", red_line); CHECK(strstr(s, src), "red 事件的 src / msg | src / msg of the red event: %s", src); }
    CHECK(strstr(s, "\"msg\":\"参数里带 🔴 也算\"}"), "参数里的红标 | red marker inside an argument");
    CHECK(strstr(s, "\"ev\":\"fatal\"") && strstr(s, "\"msg\":\"[nvn-mtl] FATAL boom 9\"}"), "MLOG 的 FATAL | FATAL from MLOG");
    CHECK(count(s, "\"ev\":\"red\"") == 2 && count(s, "\"ev\":\"fatal\"") == 1, "red 2 条、fatal 1 条 | 2 red, 1 fatal");
    { const char *p = strstr(o, "[dm] 长行 "); CHECK(p && strlen(p) >= 3000 && strstr(p, " 尾\n"), "长行整行写出 | long line written whole"); }
    CHECK(strstr(o, "[dm] 普通一行 1\n") && strstr(o, "[dm] 【红】测试 7\n"), "日志照常写 stderr | log still written to stderr");
    { char t[600] = "\"ev\":\"trunc\",\"s\":\""; for (int i = 0; i < 133; i++) strcat(t, "汉"); strcat(t, "\"}"); CHECK(strstr(s, t), "按字符边界截断 | truncated on a character boundary"); }
    CHECK(strstr(s, "\"ev\":\"badsig\",\"a\":null}"), "签名写错停在那一项 | bad signature stops at that field");
    CHECK(strstr(s, "\"ev\":\"ticked\",\"tick\":42}"), "拍号 | tick");
    CHECK(strstr(s, "\"ev\":\"crash\",\"sig\":11,\"pc\":\"0x1234\",\"where\":\"main+0x10\",\"addr\":\"0x8\"}\n"), "崩溃事件 | crash event");
    CHECK(strstr(s, "\"ev\":\"crash\",\"sig\":6,\"pc\":\"0xabc\",\"addr\":\"0x0\"}\n"), "模块外的崩溃 | crash outside the modules");
    // 每行完整、序号在文件里递增（崩溃事件不取锁，序号与下一条相同是允许的） | every line complete, seq increasing through the file (the crash event takes no lock, so sharing a seq with the next line is allowed)
    int lines = 0, last = -1, bad = 0;
    for (char *p = s; *p; ) { char *nl = strchr(p, '\n'); if (!nl) { bad++; break; }
        int seq = -1; if (sscanf(p, "{\"seq\":%d,", &seq) != 1 || nl[-1] != '}' || seq < last) bad++;
        last = seq; lines++; p = nl + 1; }
    CHECK(!bad, "%d 行不完整或乱序 | %d lines incomplete or out of order", bad, bad);
    CHECK(count(s, "\"ev\":\"spam\"") == 2000, "多线程 2000 条 | 2000 events from threads: %d", count(s, "\"ev\":\"spam\""));
    printf("t_events: %d 行事件 | event lines, %s\n", lines, fails ? "FAIL" : "OK");
    return fails != 0;
}
