// 控制口 v1：测试脚本 / 代理按行发 JSON 命令，宿主按行回「[dm] RE {json}」；事件（dm_events.h）照「[dm] EV {json}」同一条连接推过去。
//   Mac（命令行宿主与 Mac App）：DM_CTL_PORT=<端口> 宿主往 127.0.0.1:<端口> 上监听着的控制端（dmctl.py）连过去，断了自己重连；宿主不开端口
//   iOS（真机与 Mac 上跑的 iOS 包）：走已有的日志端口 DM_LOG_PORT（main.m 把以「{」开头的行交给 dm_ctl_handle）；事件要 DM_EVENTS=log 才随日志过来
// 命令：{"id":1,"cmd":"名字",…参数…}；回执：{"id":1,"cmd":"名字","ok":true|false[,"err":"…"],…结果…}。命令表与参数见 dm/verify/control_port.md。
// 往客户端写的东西（回执、事件）都进一个环形队列，由写线程发出：发事件的游戏线程不会被慢客户端卡住；队列满了丢事件并计数（state 里报）。
// Control port v1: test scripts / agents send JSON commands one per line, the host answers one "[dm] RE {json}" line each; events (dm_events.h) are pushed over the same connection as "[dm] EV {json}".
//   Mac (command-line host and Mac App): DM_CTL_PORT=<port> the host connects to the controller (dmctl.py) listening on 127.0.0.1:<port> and reconnects after a drop; the host opens no port
//   iOS (device and the iOS package running on a Mac): goes through the existing log port DM_LOG_PORT (main.m hands lines starting with "{" to dm_ctl_handle); events come along with the log only with DM_EVENTS=log
// Command: {"id":1,"cmd":"name",...arguments...}; reply: {"id":1,"cmd":"name","ok":true|false[,"err":"..."],...results...}. The command table and arguments are in dm/verify/control_port.md.
// Everything written to the client (replies, events) goes into a ring queue sent by a writer thread: a game thread emitting an event is never held up by a slow client; when the queue is full, events are dropped and counted (reported by state).
#include "dm.h"
#include "dm_ctl.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <TargetConditionals.h>
#include <unistd.h>

// ---------- 平面 JSON 对象解析（只认一层：串 / 数 / true / false / null） | Flat JSON object parsing (one level only: string / number / true / false / null) ----------
#define CTL_MAXKV 16   // 与 dm_ctl.h 的 ctl_args.kv 同长 | same length as ctl_args.kv in dm_ctl.h
static const char *skip(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const char *jstr(const char *p, char *out, size_t cap) {   // p 指在引号上；返回引号之后，失败 NULL | p is at the quote; returns past the closing quote, NULL on failure
    size_t n = 0; p++;
    for (; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') { p++; c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == 'u' ? '?' : *p; if (*p == 'u') { for (int i = 0; i < 4 && p[1]; i++) p++; } if (!*p) return NULL; }
        if (n + 1 < cap) out[n++] = c;
    }
    if (*p != '"') return NULL;
    out[n] = 0; return p + 1;
}
int dm_ctl_parse(const char *line, ctl_args *a) {
    const char *p = skip(line); a->n = 0;
    if (*p++ != '{') return -1;
    for (p = skip(p); *p != '}';) {
        if (*p != '"' || a->n == CTL_MAXKV) return -1;
        ctl_kv *kv = &a->kv[a->n];
        if (!(p = jstr(p, kv->key, sizeof kv->key))) return -1;
        p = skip(p); if (*p++ != ':') return -1; p = skip(p);
        if (*p == '"') { if (!(p = jstr(p, kv->val, sizeof kv->val))) return -1; kv->str = 1; }
        else if (*p == '[' || *p == '{') return -1;   // 只认一层：数组 / 嵌套对象不收 | one level only: arrays / nested objects are refused
        else { size_t n = 0; while (*p && *p != ',' && *p != '}' && *p != ' ' && n + 1 < sizeof kv->val) kv->val[n++] = *p++; kv->val[n] = 0; kv->str = 0; if (!n) return -1; }
        a->n++; p = skip(p);
        if (*p == ',') p = skip(p + 1); else if (*p != '}') return -1;
    }
    return a->n;
}
const char *dm_ctl_get(const ctl_args *a, const char *k) { for (int i = 0; i < a->n; i++) if (!strcmp(a->kv[i].key, k)) return a->kv[i].val; return NULL; }
double dm_ctl_num(const ctl_args *a, const char *k, double def) { const char *v = dm_ctl_get(a, k); char *e; double d = v ? strtod(v, &e) : 0; return v && e != v ? d : def; }

// ---------- 命令 | Commands ----------
// 每个命令把结果字段写进 out（「,"名":值」形式，dm_json_fields），失败时 err 写原因并返回 0
// Each command writes its result fields into out (",\"name\":value" form, dm_json_fields); on failure it writes the reason into err and returns 0
static struct { const char *name; dm_ctl_fn fn; } g_cmd[48]; static int g_ncmd;
void dm_ctl_register(const char *name, dm_ctl_fn fn) { if (g_ncmd < (int)(sizeof g_cmd / sizeof g_cmd[0])) { g_cmd[g_ncmd].name = name; g_cmd[g_ncmd].fn = fn; g_ncmd++; } }
static _Atomic unsigned g_dropped, g_commands;

static int cmd_ping(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) { (void)a; (void)err; (void)ecap; dm_json_fields(out, cap, "pong:b", 1); return 1; }
static int cmd_help(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    (void)a; (void)err; (void)ecap; size_t n = (size_t)snprintf(out, cap, ",\"cmds\":\"");
    for (int i = 0; i < g_ncmd && n + 24 < cap; i++) n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? " " : "", g_cmd[i].name);
    if (n + 2 < cap) { out[n++] = '"'; out[n] = 0; }
    return 1;
}

void dm_ctl_handle(const char *line, dm_ctl_reply reply, void *ctx) {
    dm_ctl_init();   // iOS 日志端口可能在宿主起来之前就收到命令 | the iOS log port may receive commands before the host has started
    ctl_args a; char out[3072] = "", err[256] = "", head[640];
    atomic_fetch_add(&g_commands, 1);
    const char *id = "null", *cmd = "";
    int ok = 0;
    if (dm_ctl_parse(line, &a) < 0) snprintf(err, sizeof err, "bad json");
    else {
        const char *iv = dm_ctl_get(&a, "id"); cmd = dm_ctl_get(&a, "cmd") ? dm_ctl_get(&a, "cmd") : "";
        if (iv) id = iv;
        int k = 0; for (; k < g_ncmd && strcmp(g_cmd[k].name, cmd); k++) {}
        if (k == g_ncmd) snprintf(err, sizeof err, "unknown cmd");
        else ok = g_cmd[k].fn(&a, out, sizeof out, err, sizeof err);
    }
    char idj[96], cmdj[96], errj[300];
    { int idstr = 0; for (int i = 0; i < a.n; i++) if (!strcmp(a.kv[i].key, "id")) idstr = a.kv[i].str; if (idstr) dm_json_str(idj, sizeof idj, id); else snprintf(idj, sizeof idj, "%s", id); }
    dm_json_str(cmdj, sizeof cmdj, cmd); dm_json_str(errj, sizeof errj, err);
    int hn = snprintf(head, sizeof head, "[dm] RE {\"id\":%s,\"cmd\":%s,\"ok\":%s%s%s", idj, cmdj, ok ? "true" : "false", ok ? "" : ",\"err\":", ok ? "" : errj);
    size_t on = strlen(out);
    char *msg = malloc((size_t)hn + on + 3); if (!msg) return;
    memcpy(msg, head, (size_t)hn); memcpy(msg + hn, out, on); memcpy(msg + hn + on, "}\n", 3);
    reply(ctx, msg, (size_t)hn + on + 2);
    free(msg);
}

// ---------- Mac 传输：DM_CTL_PORT（宿主往外连控制端，不监听端口） | Mac transport: DM_CTL_PORT (the host connects out to the controller and listens on no port) ----------
// 方向反过来是有意的：iOS 出包核对（ios/run.sh）只许日志端口一处 listen(，宿主这边因此一个端口都不开；控制端（dmctl.py）先监听，宿主起来后每 0.5 秒连一次直到连上，断了照样重连，起跑先后不要紧
// The reversed direction is deliberate: the iOS package check (ios/run.sh) allows listen( only at the log port, so the host opens no port at all; the controller (dmctl.py) listens, and the host connects every 0.5 s until it gets through and reconnects after a drop, so the launch order does not matter
#if TARGET_OS_OSX
#define QCAP (1u << 20)
static char g_q[QCAP]; static size_t g_qh, g_qt; static _Atomic int g_client = -1;   // 环形队列 [qh, qt) | ring queue [qh, qt)
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_qcv = PTHREAD_COND_INITIALIZER;
static int q_put(const char *p, size_t n, int must) {   // must = 回执：队列满也等着放 | must = reply: wait for room even when full
    pthread_mutex_lock(&g_qmu);
    while (g_client >= 0 && QCAP - 1 - (g_qt - g_qh) < n) { if (!must) { pthread_mutex_unlock(&g_qmu); atomic_fetch_add(&g_dropped, 1); return 0; } pthread_cond_wait(&g_qcv, &g_qmu); }
    if (g_client < 0) { pthread_mutex_unlock(&g_qmu); return 0; }
    for (size_t i = 0; i < n; i++) g_q[(g_qt + i) % QCAP] = p[i];
    g_qt += n; pthread_cond_broadcast(&g_qcv); pthread_mutex_unlock(&g_qmu); return 1;
}
// 事件历史：最近 256 KiB 的事件行，连上时先补发（同 iOS 日志端口一连上先补发整份日志）：控制端晚连也看得到主菜单、进局这些已经发生的事
// Event history: the most recent 256 KiB of event lines, replayed first on connect (as the iOS log port replays the whole log on connect): a controller that connects late still sees what already happened, such as main menu and game enter
#define HCAP (1u << 18)
static char g_h[HCAP]; static size_t g_hh, g_ht;   // 环形 [hh, ht)，满了从头丢整行 | ring [hh, ht); when full, whole lines are dropped from the front
static void sink(const char *line, size_t n) {
    char b[2200]; if (n + 8 > sizeof b) return;
    memcpy(b, "[dm] EV ", 8); memcpy(b + 8, line, n); n += 8;
    pthread_mutex_lock(&g_qmu);
    while (HCAP - 1 - (g_ht - g_hh) < n) { while (g_hh != g_ht && g_h[g_hh++ % HCAP] != '\n') {} }
    for (size_t i = 0; i < n; i++) g_h[(g_ht + i) % HCAP] = b[i];
    g_ht += n;
    pthread_mutex_unlock(&g_qmu);
    if (g_client >= 0) q_put(b, n, 0);
}
static void replay(void) {   // 持 g_qmu 调：历史整份进发送队列 | called with g_qmu held: the whole history goes into the send queue
    for (size_t i = g_hh; i != g_ht && g_qt - g_qh < QCAP - 1; i++) g_q[g_qt++ % QCAP] = g_h[i % HCAP];
}
static void tcp_reply(void *ctx, const char *line, size_t n) { (void)ctx; q_put(line, n, 1); }
static void *writer(void *u) {
    (void)u; char b[16384];
    for (;;) {
        pthread_mutex_lock(&g_qmu);
        while (g_qh == g_qt) pthread_cond_wait(&g_qcv, &g_qmu);
        size_t n = 0; while (g_qh != g_qt && n < sizeof b) b[n++] = g_q[g_qh++ % QCAP];
        int c = g_client; pthread_cond_broadcast(&g_qcv); pthread_mutex_unlock(&g_qmu);
        for (size_t o = 0; c >= 0 && o < n;) { ssize_t w = send(c, b + o, n - o, 0); if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; } o += (size_t)w; }
    }
    return NULL;
}
static void serve(int c) {   // 读这条连接的命令行，直到断开 | read this connection's command lines until it closes
    char buf[8192]; size_t used = 0; ssize_t r;
    while ((r = recv(c, buf + used, sizeof buf - 1 - used, 0)) > 0) {
        used += (size_t)r; buf[used] = 0;
        char *s = buf, *nl;
        while ((nl = strchr(s, '\n'))) {
            *nl = 0; if (nl > s && nl[-1] == '\r') nl[-1] = 0;
            const char *t = s; while (*t == ' ') t++;
            if (*t == '{') dm_ctl_handle(t, tcp_reply, NULL);
            else if (!strcmp(t, "quit")) dm_ctl_handle("{\"cmd\":\"quit\"}", tcp_reply, NULL);   // 与日志端口同一个纯文本 quit | the same plain-text quit as the log port
            s = nl + 1;
        }
        used = strlen(s); memmove(buf, s, used + 1);
        if (used == sizeof buf - 1) used = 0;   // 一行超过 8 KiB：丢掉 | a line over 8 KiB: dropped
    }
}
static void *connector(void *arg) {
    int port = (int)(intptr_t)arg, one = 1, said = 0;
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    LOG("DM_CTL_PORT：连控制端 127.0.0.1:%d（控制口，dm/verify/control_port.md）", port);
    for (;;) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        if (c < 0 || connect(c, (struct sockaddr *)&a, sizeof a)) { if (c >= 0) close(c); if (!said++) LOG("DM_CTL_PORT：控制端还没在听，每 0.5 秒再连"); usleep(500 * 1000); continue; }
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        pthread_mutex_lock(&g_qmu); g_client = c; g_qh = g_qt = 0; replay(); pthread_cond_broadcast(&g_qcv); pthread_mutex_unlock(&g_qmu);
        DM_EV("ctl", "state:s", "connected");
        serve(c);
        pthread_mutex_lock(&g_qmu); g_client = -1; g_qh = g_qt = 0; pthread_cond_broadcast(&g_qcv); pthread_mutex_unlock(&g_qmu);
        close(c); said = 0;
        usleep(500 * 1000);
    }
    return NULL;
}
static void transport_start(int port) {
    dm_events_set_sink(sink);
    pthread_t t; if (!pthread_create(&t, NULL, writer, NULL)) pthread_detach(t);
    if (!pthread_create(&t, NULL, connector, (void *)(intptr_t)port)) pthread_detach(t);
}
#else
static void transport_start(int port) { (void)port; LOG("DM_CTL_PORT 只在 Mac 上用；iOS 的控制口走日志端口 DM_LOG_PORT"); }
#endif

unsigned dm_ctl_dropped(void) { return atomic_load(&g_dropped); }
unsigned dm_ctl_commands(void) { return atomic_load(&g_commands); }
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init_once(void) {
    dm_ctl_register("ping", cmd_ping); dm_ctl_register("help", cmd_help);
    dm_ctl_commands_init();   // 游戏相关的命令（dm_ctl_cmds.c） | the game-facing commands (dm_ctl_cmds.c)
    const char *e = getenv("DM_CTL_PORT"); int port = e ? atoi(e) : 0;
    if (port > 0 && port <= 65535) transport_start(port);
}
void dm_ctl_init(void) { pthread_once(&g_once, init_once); }
