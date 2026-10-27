// 引擎快照：游戏主线程每帧读一遍游戏自己的界面与存盘状态，发布给控制口（state / engine）、事件流（action、focus、save_stuck）与红时现场。
// 只读游戏内存（native_write_scan 的规矩）；跨帧不留控件指针：焦点 / 悬停控件指针一变（或界面代号一变）就先核它还挂在界面树上，再读名字。
// 依赖的游戏地址（3.3.94012 main）：
//   main+0x2f05458  界面根（面板管理器）：子控件 +0x58 / +0x60 = 各面板（按优先级排好、最后一个在最上），+0xd0 = 焦点管理器
//   控件：+8 名字、+0x50 可见、+0x51 激活、+0x58 / +0x60 子控件；面板 +0x84 优先级
//   焦点管理器：+0x178 悬停控件、+0x188 焦点所在面板、+0x190 焦点控件
//   main+0x2d128f0  手柄光标模式字节（1 = 开着自由光标的面板）；main+0x2d12928 光标状态
//   main+0x31162c9  存盘忙（刷新在跑）；main+0x315e9ac 提交被推迟（另一个容器还挂着）
//   main+0x3115d60  存盘管理器：+0xa0 / +0xa8 = 存档容器表；容器 +0x378 挂载状态（> 1 = 挂着）、+0x388 脏
//   main+0x2e3ce48  手柄动作名表（游戏开机建一次的「名字 → 动作号」散列表，建表守卫 main+0x2e3ce68）：+0 桶数、+8 桶、+0x10 条数；
//                   节点 +0 下一个、+8 名字、+0x10 名字长度，动作号在节点里（偏移开机时核出）
//   界面消息 InputMessage / ControllerActionBegin、ControllerActionEnd：载荷 +0x10 = 动作号
// Engine snapshot: once per frame the game main thread reads the game's own UI and save state and publishes it to the control port (state / engine), the event stream (action, focus, save_stuck) and the snapshot taken on red.
// Reads game memory only (native_write_scan's rule); keeps no widget pointer across frames: when the focus / hover widget pointer changes (or the UI generation does), it first checks that the widget is still attached to the UI tree, then reads its name.
// Game addresses relied on (3.3.94012 main):
//   main+0x2f05458  UI root (the panel manager): children +0x58 / +0x60 = the panels (sorted by priority, the last one on top), +0xd0 = the focus manager
//   widget: +8 name, +0x50 visible, +0x51 active, +0x58 / +0x60 children; panel +0x84 priority
//   focus manager: +0x178 hovered widget, +0x188 the focus's panel, +0x190 focused widget
//   main+0x2d128f0  controller cursor mode byte (1 = a free-cursor panel is open); main+0x2d12928 cursor state
//   main+0x31162c9  save busy (a flush is running); main+0x315e9ac commit deferred (another container is still mounted)
//   main+0x3115d60  save manager: +0xa0 / +0xa8 = the save container table; container +0x378 mount state (> 1 = mounted), +0x388 dirty
//   main+0x2e3ce48  controller action name table (a "name → action number" hash map the game builds once at boot, build guard main+0x2e3ce68): +0 bucket count, +8 buckets, +0x10 entry count;
//                   node +0 next, +8 name, +0x10 name length, the action number inside the node (its offset is worked out at boot)
//   UI messages InputMessage / ControllerActionBegin, ControllerActionEnd: payload +0x10 = action number
#include "dm.h"
#include "dm_engine.h"
#include "dm_cursor.h"
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t dm_guest_frame(void);
int dm_game_paused(void);
void uimsg_subscribe(void (*fn)(const char *cat, const char *act, const char *arg));

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a && !vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) && got == n; }
static uint64_t rd64(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static uint64_t mono_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t fnv(const char *s) { uint64_t h = 0xcbf29ce484222325ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull; return h; }
static void cpy_name(char *out, const char *s) { size_t k = 0; if (s) for (; s[k] && k < DM_ENGINE_NAME - 1; k++) out[k] = (s[k] >= 0x20 && s[k] < 0x7f) ? s[k] : '?'; out[k] = 0; }

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static dm_engine_snap g_snap;

// ---------- 最近的动作与界面消息（红时现场用） | the latest actions and UI messages (for the snapshot on red) ----------
enum { RING = 48 };
static struct { uint64_t f; int id; uint8_t begin; } g_act[RING]; static unsigned g_nact;
static struct { uint64_t f; unsigned n; char m[96]; } g_msg[RING]; static unsigned g_nmsg;   // n = 连着同一条的次数（血条每帧清一次之类的折成一条） | n = consecutive repeats of the same message (the health bar clearing every frame and the like fold into one)
static void on_uimsg(const char *cat, const char *act, const char *arg) {   // 游戏主线程（界面总线同步派发） | game main thread (the UI bus dispatches synchronously)
    if (!strcmp(cat, "InputMessage")) return;   // 输入消息每帧都有，只记动作（见下） | input messages come every frame; only actions are kept (below)
    char m[96]; snprintf(m, sizeof m, "%s:%s:%s", cat, act, arg ? arg : "");
    pthread_mutex_lock(&g_mu); unsigned last = (g_nmsg - 1) % RING;
    if (g_nmsg && !strcmp(g_msg[last].m, m)) { g_msg[last].n++; g_msg[last].f = dm_guest_frame(); }
    else { unsigned k = g_nmsg++ % RING; g_msg[k].f = dm_guest_frame(); g_msg[k].n = 1; memcpy(g_msg[k].m, m, sizeof m); }
    pthread_mutex_unlock(&g_mu);
}

// ---------- 动作号 → 名字：读游戏自己的动作名表 | action number → name: read the game's own action name table ----------
enum { NACT = 40 };
static char g_act_name[NACT][24]; static int g_act_ready;   // 0 = 还没读、1 = 读好、-1 = 读不了（只给号） | 0 = not read yet, 1 = read, -1 = unreadable (numbers only)
static void act_names(uint64_t b) {
    if (g_act_ready) return;
    uint8_t guard = 0; if (!rd(b + 0x2e3ce68, &guard, 1) || !(guard & 1)) return;   // 游戏还没建表 | the game has not built the table yet
    uint64_t nb = rd64(b + 0x2e3ce48), bk = rd64(b + 0x2e3ce50), sz = rd64(b + 0x2e3ce58);
    if (!nb || nb > 1024 || (nb & (nb - 1)) || !bk || !sz || sz > NACT) { g_act_ready = -1; LOG("引擎快照：动作名表形状不对（桶 %llu、条 %llu），动作只给号", nb, sz); return; }
    static const int offs[] = { 0x30, 0x28, 0x20, 0x38 };   // 动作号在节点里的偏移：取让全部条目落在 0..NACT-1、互不相同的那个 | offset of the action number in the node: the one that puts every entry in 0..NACT-1 with no repeats
    for (size_t o = 0; o < sizeof offs / sizeof *offs; o++) {
        char names[NACT][24]; memset(names, 0, sizeof names); unsigned got = 0, bad = 0;
        for (uint64_t i = 0; i < nb && !bad; i++)
            for (uint64_t nd = rd64(bk + 8 * i), k = 0; nd && k < 64 && !bad; nd = rd64(nd), k++) {
                uint64_t kp = rd64(nd + 8); uint32_t kl = (uint32_t)rd64(nd + 0x10); int32_t v = -1; rd(nd + (uint64_t)offs[o], &v, 4);
                if (v < 0 || v >= NACT || !kl || kl >= 24 || names[v][0] || !rd(kp, names[v], kl)) { bad = 1; break; }
                for (uint32_t c = 0; c < kl; c++) if (names[v][c] < 0x20 || names[v][c] >= 0x7f) bad = 1;
                got++; }
        if (!bad && got == sz) { memcpy(g_act_name, names, sizeof names); g_act_ready = 1; LOG("引擎快照：动作名表 %u 个（节点偏移 %#x）", got, offs[o]); return; }
    }
    g_act_ready = -1; LOG("引擎快照：动作名表读不出来，动作只给号");
}
static const char *act_name(int id) { return g_act_ready > 0 && id >= 0 && id < NACT && g_act_name[id][0] ? g_act_name[id] : NULL; }

void dm_engine_input_msg(uint64_t act_hash, uint64_t payload) {   // 游戏主线程（diag.c 界面消息观测门回调） | game main thread (the diag.c UI-message observation gate callback)
    static uint64_t hb, he; if (!hb) { hb = fnv("ControllerActionBegin"); he = fnv("ControllerActionEnd"); }
    if (act_hash != hb && act_hash != he) return;
    int32_t id = -1; if (!rd(payload + 0x10, &id, 4)) return;
    int begin = act_hash == hb; uint64_t fr = dm_guest_frame();
    pthread_mutex_lock(&g_mu); unsigned k = g_nact++ % RING; g_act[k].f = fr; g_act[k].id = id; g_act[k].begin = (uint8_t)begin; pthread_mutex_unlock(&g_mu);
    DM_EV("action", "id:d name:s phase:s", (int)id, act_name(id), begin ? "begin" : "end");
}

// ---------- 存盘卡住 | stuck saves ----------
// 宿主请了存盘（dm_request_save）、之后游戏走了 DM_SAVE_STUCK_S 秒（缺省 20；暂停着、不出图不算）还没有任何提交或拒绝 → 卡住；
// 或者某个存档容器连着挂了 DM_SAVE_MOUNT_STUCK_S 秒（缺省 60；游戏的存盘任务挂载、写完、卸载，正常一次不到一秒；打开失败不卸载就是 715ed80 修的那种卡死）。
// 注意：存盘任务跑在游戏的异步任务池上，一个任务卡住约 8 秒，每帧排进去的任务就把任务分配器用光，任务池的入队函数（main+0x14f0590）拿到空指针照写、崩溃——卡住的存盘活不到 60 秒
// 30 秒节流里排着的写不算（容器没挂）。各报一次，之后有提交 / 卸载了再重新计
// The host asked for a save (dm_request_save) and the game then ran DM_SAVE_STUCK_S seconds (default 20; paused or not presenting does not count) with no commit or rejection → stuck;
// or a save container stayed mounted DM_SAVE_MOUNT_STUCK_S seconds in a row (default 60; the game's save job mounts, writes, unmounts, normally in under a second; skipping the unmount after a failed open is the wedge fixed by 715ed80).
// Note: save jobs run on the game's async job pool; with one job stuck for about 8 s, the tasks queued every frame use up the task allocator and the pool's enqueue function (main+0x14f0590) writes through the null pointer it gets and crashes -- a stuck save does not live 60 s
// Writes queued behind the 30 s throttle do not count (the container is not mounted). Each is reported once, re-armed after a commit / unmount
static _Atomic uint64_t g_answers, g_req_answers, g_req_on;   // 提交钩子的回数（成功 / 失败都算）；请求时的回数；有没有在等 | post-commit hook calls (success or failure); the count at request time; whether a request is waiting
static void on_commit(int ok) { (void)ok; atomic_fetch_add(&g_answers, 1); }
__attribute__((constructor)) static void engine_hook_install(void) { extern void (*g_dm_post_commit_hook[5])(int); g_dm_post_commit_hook[4] = on_commit; }
void dm_engine_save_requested(void) {
    if (atomic_load(&g_req_on)) return;   // 已经在等：按最早那次算 | already waiting: count from the earliest
    atomic_store(&g_req_answers, atomic_load(&g_answers)); atomic_store(&g_req_on, 1);
    DM_EV("save_request", "");
}
static void stuck(const char *why, double s, const dm_engine_snap *e) {   // why = no_commit（请了存盘、一直没有提交）/ mounted（存档容器一直挂着） | why = no_commit (a save was requested and never committed) / mounted (a save container stayed mounted)
    if (!strcmp(why, "no_commit")) LOG("【红】存盘卡住：请了存盘、一直没有提交（%.0f 秒；存盘忙 %d、提交推迟 %d、挂着的容器 %d / %d）", s, e->save_busy, e->save_pending, e->mounted, e->containers);
    else LOG("【红】存盘卡住：存档容器一直挂着（%.0f 秒；存盘忙 %d、提交推迟 %d、挂着的容器 %d / %d）", s, e->save_busy, e->save_pending, e->mounted, e->containers);
    DM_EV("save_stuck", "why:s s:f busy:b pending:b mounted:d containers:d", why, s, e->save_busy, e->save_pending, e->mounted, e->containers);
}
static void stuck_check(const dm_engine_snap *e) {
    static uint64_t last, waited, mount_t; static int said_req, said_mount; static double lim = -1, mlim = -1;
    if (lim < 0) { const char *v = getenv("DM_SAVE_STUCK_S"); lim = v ? atof(v) : 20; v = getenv("DM_SAVE_MOUNT_STUCK_S"); mlim = v ? atof(v) : 60; }
    uint64_t now = mono_ns(), dt = last ? now - last : 0; last = now;
    if (dt > 500000000ull) dt = 0;   // 隔了半秒以上才来（窗口藏着、调试器停着）：这段不算 | more than half a second since the last frame (window hidden, stopped in a debugger): this gap does not count
    if (atomic_load(&g_req_on)) {
        if (atomic_load(&g_answers) != atomic_load(&g_req_answers)) { atomic_store(&g_req_on, 0); waited = 0; said_req = 0; }
        else if (!dm_game_paused() && (waited += dt) > (uint64_t)(lim * 1e9) && !said_req) { said_req = 1; stuck("no_commit", waited / 1e9, e); }
    } else waited = 0;
    if (e->mounted > 0) { if (!mount_t) mount_t = now; else if (now - mount_t > (uint64_t)(mlim * 1e9) && !said_mount) { said_mount = 1; stuck("mounted", (now - mount_t) / 1e9, e); } }
    else { mount_t = 0; said_mount = 0; }
}

// ---------- 每帧 | every frame ----------
static int in_tree(uint64_t w, uint64_t want, int dep) {   // 按指针找（不解引用 want） | searched by pointer (want is never dereferenced)
    if (!w || dep > 14) return 0; if (w == want) return 1;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (in_tree(*(uint64_t *)(ch + 8 * i), want, dep + 1)) return 1;
    return 0;
}
typedef struct { uint64_t w; uint32_t gen; char name[DM_ENGINE_NAME]; } named;
static void resolve(uint64_t root, uint64_t w, named *c) {   // 指针或界面代号变了才重核、重读名字 | re-check and re-read the name only when the pointer or the UI generation changed
    uint32_t gen = dm_ui_gen(); if (w == c->w && gen == c->gen) return;
    c->w = w; c->gen = gen; c->name[0] = 0;
    if (w && in_tree(root, w, 0)) cpy_name(c->name, (const char *)*(uint64_t *)(w + 8));
}
void dm_engine_pump(uint64_t root) {
    static uint64_t b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)(uintptr_t)g_mod[i].base;
    if (!b || !root) return;
    static int sub; if (!sub) { sub = 1; uimsg_subscribe(on_uimsg); }
    act_names(b);
    dm_engine_snap e; memset(&e, 0, sizeof e); e.frame = dm_guest_frame();
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60);
    if (ch && n <= 256) for (uint64_t j = 0; j < n && e.npanels < DM_ENGINE_PANELS; j++) {
        uint64_t p = *(uint64_t *)(ch + 8 * j); if (!p || !*(uint8_t *)(p + 0x50)) continue;
        cpy_name(e.panels[e.npanels].name, (const char *)*(uint64_t *)(p + 8));
        e.panels[e.npanels].active = *(uint8_t *)(p + 0x51); e.panels[e.npanels].priority = *(int32_t *)(p + 0x84); e.npanels++; }
    static named fp, fw, hw; uint64_t fm = *(uint64_t *)(root + 0xd0);
    resolve(root, fm ? *(uint64_t *)(fm + 0x188) : 0, &fp); resolve(root, fm ? *(uint64_t *)(fm + 0x190) : 0, &fw); resolve(root, fm ? *(uint64_t *)(fm + 0x178) : 0, &hw);
    memcpy(e.focus_panel, fp.name, sizeof e.focus_panel); memcpy(e.focus, fw.name, sizeof e.focus); memcpy(e.hover, hw.name, sizeof e.hover);
    e.cursor_mode = *(volatile uint8_t *)(b + 0x2d128f0); e.cursor_state = *(volatile int32_t *)(b + 0x2d12928);
    e.save_busy = *(volatile uint8_t *)(b + 0x31162c9); e.save_pending = *(volatile uint8_t *)(b + 0x315e9ac);
    uint64_t arr = rd64(b + 0x3115d60 + 0xa0), cn = rd64(b + 0x3115d60 + 0xa8);   // 存盘任务在别的线程改：用 vm_read 读、读不到就当没有 | save jobs change these on other threads: read with vm_read, treat unreadable as absent
    if (arr && cn <= 16) for (uint64_t i = 0; i < cn; i++) { uint64_t c = rd64(arr + 8 * i); uint32_t st = 0; uint8_t d = 0;
        if (!c || !rd(c + 0x378, &st, 4)) continue; rd(c + 0x388, &d, 1); e.containers++; e.mounted += st > 1; e.dirty += d != 0; }
    static char last_f[2 * DM_ENGINE_NAME] = "\x01";
    char fnow[2 * DM_ENGINE_NAME]; snprintf(fnow, sizeof fnow, "%s/%s", e.focus_panel, e.focus);
    if (strcmp(fnow, last_f)) { strlcpy(last_f, fnow, sizeof last_f); DM_EV("focus", "panel:s widget:s", e.focus_panel[0] ? e.focus_panel : NULL, e.focus[0] ? e.focus : NULL); }
    pthread_mutex_lock(&g_mu); g_snap = e; pthread_mutex_unlock(&g_mu);
    stuck_check(&e);
}
void dm_engine_get(dm_engine_snap *out) { pthread_mutex_lock(&g_mu); *out = g_snap; pthread_mutex_unlock(&g_mu); }

// ---------- 控制口 JSON | control-port JSON ----------
static size_t put(char *out, size_t cap, size_t n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t put(char *out, size_t cap, size_t n, const char *fmt, ...) {
    if (n >= cap) return n; va_list ap; va_start(ap, fmt); int k = vsnprintf(out + n, cap - n, fmt, ap); va_end(ap);
    return k < 0 ? n : n + (size_t)k;
}
static size_t put_str(char *out, size_t cap, size_t n, const char *s) {   // JSON 串；空串写 null | a JSON string; an empty string writes null
    if (!s || !*s) return put(out, cap, n, "null");
    char b[256]; dm_json_str(b, sizeof b, s); return put(out, cap, n, "%s", b);
}
size_t dm_engine_state_json(char *out, size_t cap) {
    dm_engine_snap e; dm_engine_get(&e); size_t n = 0;
    n = put(out, cap, n, ",\"panels\":[");
    for (int i = e.npanels - 1; i >= 0; i--) { n = put_str(out, cap, n, e.panels[i].name); if (i) n = put(out, cap, n, ","); }   // 最上面的在前 | the top one first
    n = put(out, cap, n, "],\"focus\":"); n = put_str(out, cap, n, e.focus);
    n = put(out, cap, n, ",\"focus_panel\":"); n = put_str(out, cap, n, e.focus_panel);
    n = put(out, cap, n, ",\"cursor_mode\":%d,\"save_busy\":%s,\"save_pending\":%s,\"save_mounted\":%d,\"save_containers\":%d",
            e.cursor_mode, e.save_busy ? "true" : "false", e.save_pending ? "true" : "false", e.mounted, e.containers);
    if (n >= cap && cap) out[cap - 1] = 0;
    return n < cap ? n : cap ? cap - 1 : 0;
}
size_t dm_engine_full_json(char *out, size_t cap) {
    dm_engine_snap e; dm_engine_get(&e); size_t n = 0;
    n = put(out, cap, n, ",\"frame\":%llu,\"panels\":[", (unsigned long long)e.frame);
    for (int i = e.npanels - 1; i >= 0; i--) { n = put(out, cap, n, "%s{\"name\":", i == e.npanels - 1 ? "" : ","); n = put_str(out, cap, n, e.panels[i].name); n = put(out, cap, n, ",\"active\":%s,\"priority\":%d}", e.panels[i].active ? "true" : "false", e.panels[i].priority); }
    n = put(out, cap, n, "],\"focus_panel\":"); n = put_str(out, cap, n, e.focus_panel);
    n = put(out, cap, n, ",\"focus\":"); n = put_str(out, cap, n, e.focus);
    n = put(out, cap, n, ",\"hover\":"); n = put_str(out, cap, n, e.hover);
    n = put(out, cap, n, ",\"cursor_mode\":%d,\"cursor_state\":%d,\"save\":{\"busy\":%s,\"pending\":%s,\"containers\":%d,\"mounted\":%d,\"dirty\":%d},\"actions\":[",
            e.cursor_mode, e.cursor_state, e.save_busy ? "true" : "false", e.save_pending ? "true" : "false", e.containers, e.mounted, e.dirty);
    pthread_mutex_lock(&g_mu);
    unsigned a0 = g_nact > RING ? g_nact - RING : 0, m0 = g_nmsg > RING ? g_nmsg - RING : 0;
    for (unsigned i = a0; i < g_nact; i++) { unsigned k = i % RING; n = put(out, cap, n, "%s{\"f\":%llu,\"id\":%d,\"name\":", i == a0 ? "" : ",", (unsigned long long)g_act[k].f, g_act[k].id); n = put_str(out, cap, n, act_name(g_act[k].id)); n = put(out, cap, n, ",\"phase\":\"%s\"}", g_act[k].begin ? "begin" : "end"); }
    n = put(out, cap, n, "],\"messages\":[");
    for (unsigned i = m0; i < g_nmsg; i++) { unsigned k = i % RING; n = put(out, cap, n, "%s{\"f\":%llu,\"m\":", i == m0 ? "" : ",", (unsigned long long)g_msg[k].f); n = put_str(out, cap, n, g_msg[k].m); n = put(out, cap, n, ",\"n\":%u}", g_msg[k].n); }
    pthread_mutex_unlock(&g_mu);
    n = put(out, cap, n, "]");
    if (n >= cap && cap) out[cap - 1] = 0;
    return n < cap ? n : cap ? cap - 1 : 0;
}
