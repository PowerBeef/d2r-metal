// 控制口的游戏相关命令（协议与传输见 dm_ctl.c，命令表见 dm/verify/control_port.md）。
// 每个命令只调宿主已有的路径（与 DM_KEYS 脚本、前端退出 / 进后台用的同一批函数），不另造输入或存盘通道。
// Game-facing commands of the control port (protocol and transport in dm_ctl.c, command table in dm/verify/control_port.md).
// Each command only calls existing host paths (the same functions the DM_KEYS script and the front ends' quit / background paths use), without inventing new input or save channels.
#include "dm.h"
#include "dm_ctl.h"
#include "dm_hud.h"
#include <unistd.h>

#include "dm_cursor.h"
#include "dm_world.h"
#include "dm_uiscreen.h"
#include <stdatomic.h>

int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms);
int dm_game_paused(void);
uint32_t dm_cur_tick(void);
uint64_t dm_guest_frame(void);
void crash_dump_now(const char *why);
void dm_host_shutdown(const char *why);
extern char g_lf_panel[48];
void dm_ctl_input(uint64_t buttons, int lx, int ly, int frames);   // hle_hid.c
int dm_ctl_input_busy(void);
uint64_t dm_key_mask(const char *names, const char **bad);
int dm_ui_send_pending(void);                                      // dm_cursor.c
int dm_restart_busy(void);
void dm_ui_dump_request(const char *want);
int dm_ui_dump_pending(void);
void dm_world_walk_cell(int x, int y, int timeout_s);              // dm_world.c
int dm_world_walk_cell_result(void);
int dm_ctl_tick_hold(int on);                                      // diag.c
int dm_ctl_tick_held(void);
uint32_t dm_ctl_tick_now(void);
extern void (*g_dm_ctl_shot_hook)(const char *path, int ok);
void dm_gfx_shot_at(long k, const char *path);                     // nvn_mtl.m

#define ERR(...) do { snprintf(err, ecap, __VA_ARGS__); return 0; } while (0)
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// 等到条件成立或超时（每 5 ms 看一次）；cond 返回非零即成立 | wait until the condition holds or the timeout passes (checked every 5 ms); cond returning non-zero means it holds
static int wait_until(int (*cond)(void *), void *arg, double timeout_s) {
    uint64_t end = mono_ns() + (uint64_t)(timeout_s * 1e9);
    while (!cond(arg)) { if (mono_ns() > end) return 0; usleep(5000); }
    return 1;
}
static int c_frame(void *a) { return dm_guest_frame() >= *(uint64_t *)a; }
static int wait_frames(uint64_t n, double timeout_s) { uint64_t f = dm_guest_frame() + n; return wait_until(c_frame, &f, timeout_s); }
static int c_input_idle(void *a) { (void)a; return !dm_ctl_input_busy(); }

// state：局内与否、拍号、暂停、最近打开的面板、玩家（等级 / 生命 / 法力 / 区域 / 坐标 / 死亡）、控制口统计
// state: in a run or not, tick, paused, the most recently opened panel, the player (level / life / mana / area / position / dead), control-port counters
static int cmd_state(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    (void)a; (void)err; (void)ecap;
    dm_hud_info h; dm_hud_get(&h);
    char panel[48]; strlcpy(panel, g_lf_panel, sizeof panel);
    dm_json_fields(out, cap, "in_game:b tick:u paused:b panel:s level:d hp:d hp_max:d mp:d mp_max:d area:d x:d y:d dead:b players:d holding:d belt0:d belt1:d belt2:d belt3:d portal:d points:d ev_dropped:u",
                   h.valid, (unsigned long long)(h.valid ? dm_cur_tick() : 0), h.valid && dm_game_paused(), panel[0] ? panel : NULL, h.level, h.hp, h.hp_max, h.mp, h.mp_max,
                   h.area, h.x, h.y, h.dead, h.players, h.holding, h.belt[0], h.belt[1], h.belt[2], h.belt[3], h.portal, h.points, (unsigned long long)dm_ctl_dropped());
    return 1;
}

// save_now [timeout=秒，缺省 5]：请游戏照自己的定时存盘路径马上存一次并等提交（前端退出 / 进后台用的同一个 dm_save_wait；暂停着会先冻结世界、临时放开）
// save_now [timeout=seconds, default 5]: ask the game to save right away along its own timed-save path and wait for the commit (the same dm_save_wait the front ends use on quit / background; when paused, the world is frozen and the pause released for this save)
static int cmd_save_now(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    double t = dm_ctl_num(a, "timeout", 5), ms = 0; uint64_t n = 0; int froze = 0;
    if (t <= 0 || t > 60) ERR("timeout must be 0-60 s");
    int asked = dm_save_wait(t, &n, &froze, &ms);
    if (!asked) ERR("not in a state where a save can be requested (not in a game, or on the loading screen)");
    if (!n) ERR("no save commit within %.1f s", t);
    dm_json_fields(out, cap, "commits:u ms:f paused:b", (unsigned long long)n, ms, froze);
    return 1;
}

static int cmd_dump(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {   // 把各线程在等什么导进日志（同日志端口的 dump） | dump what each thread is waiting on into the log (same as the log port's dump)
    (void)a; (void)out; (void)cap; (void)err; (void)ecap; crash_dump_now("控制口 dump"); return 1;
}

// quit：回执先发出去，再走宿主的优雅退出（与 SIGTERM / iOS 终止同一条 dm_host_shutdown；Mac App 的「存盘再退」用 save_now 后再 quit）
// quit: the reply goes out first, then the host's graceful exit (the same dm_host_shutdown as SIGTERM / iOS terminate; for the Mac App's save-then-quit, send save_now before quit)
static void *quit_later(void *u) { (void)u; usleep(300 * 1000); dm_host_shutdown("控制口 quit"); return NULL; }
static int cmd_quit(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    if (dm_ctl_num(a, "save", 0)) { double ms = 0; uint64_t n = 0; int froze = 0; dm_save_wait(3, &n, &froze, &ms); dm_json_fields(out, cap, "saved:b", n > 0); }   // save=1：先像 Mac 关窗那样存一次（最多 3 秒） | save=1: save once first, like closing the Mac window (at most 3 s)
    pthread_t t; if (pthread_create(&t, NULL, quit_later, NULL)) ERR("cannot start the quit thread");
    pthread_detach(t); return 1;
}

// press keys=A+B [frames=6] [gap=3]：按住这些键 frames 个出图帧（经 hle_hid.c 取手柄处并进去，与 DM_KEYS 同一套裁决），再松开 gap 帧才回执
// press keys=A+B [frames=6] [gap=3]: hold these buttons for `frames` presented frames (merged at the controller read in hle_hid.c, same arbitration as DM_KEYS), then release for `gap` frames before replying
// stick x=-1..1 y=-1..1 [frames=30] [keys=…]：左摇杆推到 (x, y)（上为正），可同时按键 | stick x=-1..1 y=-1..1 [frames=30] [keys=...]: push the left stick to (x, y) (up is positive), optionally with buttons
static int input_common(const ctl_args *a, double fx, double fy, int frames_def, char *out, size_t cap, char *err, size_t ecap) {
    const char *keys = dm_ctl_get(a, "keys"), *bad = NULL; uint64_t m = 0;
    if (keys && *keys && !(m = dm_key_mask(keys, &bad))) ERR("unknown key near '%s' (names: A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN)", bad ? bad : keys);
    int frames = (int)dm_ctl_num(a, "frames", frames_def), gap = (int)dm_ctl_num(a, "gap", 3);
    if (frames < 1 || frames > 3600 || gap < 0 || gap > 600) ERR("frames must be 1-3600, gap 0-600");
    if (fx < -1 || fx > 1 || fy < -1 || fy > 1) ERR("x / y must be in [-1, 1]");
    if (!wait_until(c_input_idle, NULL, 10)) ERR("previous input still held");
    uint64_t f0 = dm_guest_frame();
    dm_ctl_input(m, (int)(fx * 32767), (int)(fy * 32767), frames);
    if (!wait_until(c_input_idle, NULL, frames / 10.0 + 5)) { dm_ctl_input(0, 0, 0, 0); ERR("frames are not advancing (window hidden / game stalled?)"); }
    if (gap && !wait_frames((uint64_t)gap, gap / 10.0 + 5)) ERR("frames are not advancing after release");
    dm_json_fields(out, cap, "frame:u held_frames:u", (unsigned long long)f0, (unsigned long long)frames);
    return 1;
}
static int cmd_press(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    if (!dm_ctl_get(a, "keys")) ERR("missing keys");
    return input_common(a, 0, 0, 6, out, cap, err, ecap);
}
static int cmd_stick(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) { return input_common(a, dm_ctl_num(a, "x", 0), dm_ctl_num(a, "y", 0), 30, out, cap, err, ecap); }

// tap x=0..1 y=0..1 [hold=3]：界面点一下（出图画面归一化坐标，与 DM_KEYS 的 C<x>:<y> 同一条 dm_ui_touch 注入）| tap x=0..1 y=0..1 [hold=3]: one UI tap (coordinates normalized to the rendered picture, the same dm_ui_touch injection as DM_KEYS's C<x>:<y>)
static int cmd_tap(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    double x = dm_ctl_num(a, "x", -1), y = dm_ctl_num(a, "y", -1); int hold = (int)dm_ctl_num(a, "hold", 3);
    if (x < 0 || x > 1 || y < 0 || y > 1) ERR("x / y must be in [0, 1]");
    if (hold < 1 || hold > 600) ERR("hold must be 1-600 frames");
    if (dm_restart_busy()) ERR("one-key restart in progress: taps are dropped");
    y = dm_ui_ny((float)y);   // 真实画面纵坐标 → 界面矩形（同 DM_UI_TAPS；界面占满屏时不变） | real-picture y -> the UI rectangle (as DM_UI_TAPS; unchanged when the UI fills the screen)
    dm_ui_touch(0, (float)x, (float)y);
    if (!wait_frames((uint64_t)hold, hold / 10.0 + 5)) { dm_ui_touch(3, (float)x, (float)y); ERR("frames are not advancing"); }
    dm_ui_touch(2, (float)x, (float)y); wait_frames(2, 5);
    (void)out; (void)cap; return 1;
}

// msg spec="类别:动作:参数"：往游戏发一条界面消息（同 DM_KEYS 的 MSG:… 步，取手柄处发出），发出去才回执 | msg spec="category:action:argument": send one UI message into the game (same as DM_KEYS's MSG:... step, sent at the controller read), replying once it has gone out
static int c_msg_sent(void *a) { (void)a; return !dm_ui_send_pending(); }
static int cmd_msg(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *spec = dm_ctl_get(a, "spec"); (void)out; (void)cap;
    if (!spec || !strchr(spec, ':') || strlen(spec) >= 96) ERR("spec must be \"category:action[:argument]\" (under 96 characters)");
    if (!wait_until(c_msg_sent, NULL, 3)) ERR("previous message not sent yet");
    dm_ui_send_queue(spec);
    if (!wait_until(c_msg_sent, NULL, 3)) ERR("message not sent within 3 s (game thread not reading the controller?)");
    return 1;
}

// world_tap x=0..1 y=0..1：点世界里那一处（同触屏点人 / 物 / 门：走过去交互，没有目标什么都不发）；回执带点到的是什么 | world_tap x=0..1 y=0..1: tap that spot in the world (as a touch tap on a person / item / door: walk over and interact; nothing is sent without a target); the reply says what was hit
static int c_tap_done(void *a) { return dm_world_tap_result(*(unsigned *)a) >= 0; }
static int cmd_world_tap(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    double x = dm_ctl_num(a, "x", -1), y = dm_ctl_num(a, "y", -1);
    if (x < 0 || x > 1 || y < 0 || y > 1) ERR("x / y must be in [0, 1]");
    unsigned seq = dm_world_tap((float)x, (float)y);
    if (!wait_until(c_tap_done, &seq, 3)) ERR("tap not handled within 3 s (not in a game?)");
    static const char *kind[] = {"none", "item", "npc", "object", "exit"}; int r = dm_world_tap_result(seq);
    dm_json_fields(out, cap, "target:s", r >= 0 && r <= 4 ? kind[r] : "?");
    return 1;
}

// walk_to x=格 y=格 [timeout=12]：按格坐标走过去（同 DM_TEST_WORLD_XY 的路线规划）；到 2 格内算到 | walk_to x=cell y=cell [timeout=12]: walk to the cell coordinates (same route planning as DM_TEST_WORLD_XY); within 2 cells counts as arrived
static int c_walk_done(void *a) { (void)a; return dm_world_walk_cell_result() != 0; }
static int cmd_walk_to(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *xs = dm_ctl_get(a, "x"), *ys = dm_ctl_get(a, "y"); int t = (int)dm_ctl_num(a, "timeout", 12);
    if (!xs || !ys) ERR("missing x / y (cells)");
    if (t < 1 || t > 300) ERR("timeout must be 1-300 s");
    dm_hud_info h; dm_hud_get(&h); if (!h.valid) ERR("not in a game");
    dm_world_walk_cell(atoi(xs), atoi(ys), t);
    if (!wait_until(c_walk_done, NULL, t + 5)) ERR("walk request not picked up");
    int r = dm_world_walk_cell_result(); dm_hud_get(&h);
    dm_json_fields(out, cap, "arrived:b x:d y:d", r == 1, h.x, h.y);
    if (r != 1) ERR("did not arrive within %d s (stopped at %d,%d)", t, h.x, h.y);
    return 1;
}

// screenshot path=… [frames=1]：第 frames 个出图帧之后截游戏输出（PNG，dm_gfx_shot_at），写完才回执；iOS 上相对路径落在 Documents 下
// screenshot path=... [frames=1]: capture the game output after `frames` presented frames (PNG, dm_gfx_shot_at) and reply once written; on iOS a relative path lands under Documents
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER; static char g_shot_path[1024]; static _Atomic int g_shot_done, g_shot_ok;
static void shot_hook(const char *path, int ok) { pthread_mutex_lock(&g_shot_mu); if (!strcmp(path, g_shot_path)) { atomic_store(&g_shot_ok, ok); atomic_store(&g_shot_done, 1); } pthread_mutex_unlock(&g_shot_mu); }
static int c_shot(void *a) { (void)a; return atomic_load(&g_shot_done); }
static int cmd_screenshot(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *p = dm_ctl_get(a, "path"); long k = (long)dm_ctl_num(a, "frames", 1); char path[1024];
    if (!p || !*p) ERR("missing path");
    if (k < 1 || k > 600) ERR("frames must be 1-600");
#if TARGET_OS_IPHONE
    if (p[0] != '/' && getenv("HOME")) snprintf(path, sizeof path, "%s/Documents/%s", getenv("HOME"), p); else
#endif
    strlcpy(path, p, sizeof path);
    pthread_mutex_lock(&g_shot_mu); strlcpy(g_shot_path, path, sizeof g_shot_path); atomic_store(&g_shot_done, 0); pthread_mutex_unlock(&g_shot_mu);
    g_dm_ctl_shot_hook = shot_hook;
    dm_gfx_shot_at(k, path);
    if (!wait_until(c_shot, NULL, k / 10.0 + 10)) ERR("screenshot not written within %.0f s (frames not presenting?)", k / 10.0 + 10);
    if (!atomic_load(&g_shot_ok)) ERR("screenshot failed: %s", path);
    dm_json_fields(out, cap, "path:s", path);
    return 1;
}

// ui_dump [want=@top]：下一帧把界面控件树打进日志（UIDUMP 行，同 DM_UI_DUMP；@top = 界面根下可见的前两层，或写控件名） | ui_dump [want=@top]: on the next frame print the UI widget tree into the log (UIDUMP lines, same as DM_UI_DUMP; @top = the visible top two levels under the UI root, or a widget name)
static int c_dump_done(void *a) { (void)a; return !dm_ui_dump_pending(); }
static int cmd_ui_dump(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    (void)out; (void)cap;
    dm_ui_dump_request(dm_ctl_get(a, "want"));
    if (!wait_until(c_dump_done, NULL, 3)) ERR("UI root not available within 3 s");
    return 1;
}

// pause / resume / step [n=1]：冻结游戏拍 / 解冻 / 解冻走 n 拍再冻（同 DM_TICK_HOLD 的冻结；冻着超过 DM_WATCH_STALL 秒看门狗会判死）
// pause / resume / step [n=1]: freeze game ticks / unfreeze / unfreeze for n ticks and freeze again (same freeze as DM_TICK_HOLD; frozen longer than DM_WATCH_STALL seconds, the watchdog gives up)
static int hold_err(int r, char *err, size_t ecap) { snprintf(err, ecap, r == -1 ? "no tick origin yet (needs a run that has ticked with a control channel on)" : "another freeze is active (DM_TICK_HOLD / loading screen)"); return 0; }
static int cmd_pause(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) { (void)a; int r = dm_ctl_tick_hold(1); if (r) return hold_err(r, err, ecap); dm_json_fields(out, cap, "tick:u", (unsigned long long)dm_ctl_tick_now()); return 1; }
static int cmd_resume(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) { (void)a; (void)err; (void)ecap; dm_ctl_tick_hold(0); dm_json_fields(out, cap, "tick:u", (unsigned long long)dm_ctl_tick_now()); return 1; }
static int c_tick(void *a) { return dm_ctl_tick_now() >= *(uint32_t *)a; }
static int cmd_step(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    int n = (int)dm_ctl_num(a, "n", 1);
    if (n < 1 || n > 1000) ERR("n must be 1-1000");
    if (!dm_ctl_tick_held()) ERR("not paused (send pause first)");
    uint32_t want = dm_ctl_tick_now() + (uint32_t)n;
    dm_ctl_tick_hold(0);
    int ok = wait_until(c_tick, &want, n * 0.2 + 5);
    int r = dm_ctl_tick_hold(1);
    if (!ok) ERR("ticks did not advance");
    if (r) return hold_err(r, err, ecap);
    dm_json_fields(out, cap, "tick:u", (unsigned long long)dm_ctl_tick_now());
    return 1;
}

// wait frames=N | ticks=N | ms=N：在宿主这头等（等出图帧 / 游戏拍 / 毫秒），给脚本对齐用 | wait frames=N | ticks=N | ms=N: wait on the host side (presented frames / game ticks / milliseconds), for scripts to line up
static int cmd_wait(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    double fr = dm_ctl_num(a, "frames", 0), tk = dm_ctl_num(a, "ticks", 0), ms = dm_ctl_num(a, "ms", 0), to = dm_ctl_num(a, "timeout", 60);
    if (fr < 0 || tk < 0 || ms < 0 || fr + tk + ms <= 0) ERR("give frames, ticks or ms");
    if (ms > 0) usleep((useconds_t)(ms > 60000 ? 60000 : ms) * 1000);
    if (fr > 0 && !wait_frames((uint64_t)fr, to)) ERR("frames did not advance within %.0f s", to);
    if (tk > 0) { uint32_t want = dm_ctl_tick_now() + (uint32_t)tk; if (!wait_until(c_tick, &want, to)) ERR("ticks did not advance within %.0f s", to); }
    dm_json_fields(out, cap, "frame:u tick:u", (unsigned long long)dm_guest_frame(), (unsigned long long)dm_ctl_tick_now());
    return 1;
}

// tap_ui name=控件名[@面板] [timeout=10]：等控件可见，点它的中心（同触屏脚本 tap_ui；名字里「+」= 空格） | tap_ui name=widget[@panel] [timeout=10]: wait until the widget is visible and tap its center (same as the touch script's tap_ui; "+" in the name = a space)
// await_ui name=… [timeout=30]：只等它可见，回执带位置 | await_ui name=... [timeout=30]: only wait until it is visible; the reply carries the position
static void ui_name(const ctl_args *a, char *out, size_t n) { const char *v = dm_ctl_get(a, "name"); strlcpy(out, v ? v : "", n); for (char *p = out; *p; p++) if (*p == '+') *p = ' '; }
static int c_ui_seen(void *a) { float *xy = a; return dm_ui_watch_pos(&xy[0], &xy[1]); }
static int watch_ui(const ctl_args *a, double def_timeout, float xy[2], char *err, size_t ecap) {
    char name[64]; ui_name(a, name, sizeof name); if (!name[0]) ERR("missing name");
    dm_ui_watch(name); int ok = wait_until(c_ui_seen, xy, dm_ctl_num(a, "timeout", def_timeout)); dm_ui_watch(NULL);
    if (!ok) ERR("widget '%s' not visible", name);
    return 1;
}
static int cmd_await_ui(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    float xy[2]; if (!watch_ui(a, 30, xy, err, ecap)) return 0;
    dm_json_fields(out, cap, "x:f y:f", (double)xy[0], (double)xy[1]); return 1;
}
static int cmd_tap_ui(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    float xy[2]; if (!watch_ui(a, 10, xy, err, ecap)) return 0;
    if (dm_restart_busy()) ERR("one-key restart in progress: taps are dropped");
    dm_ui_touch(0, xy[0], xy[1]); wait_frames(3, 5); dm_ui_touch(2, xy[0], xy[1]); wait_frames(2, 5);   // 控件位置已在界面矩形里，直接给 dm_ui_touch | the widget position is already in the UI rectangle, given to dm_ui_touch as is
    dm_json_fields(out, cap, "x:f y:f", (double)xy[0], (double)xy[1]); return 1;
}
// tap_unit type=1 cls=119 [timeout=10]：等这类单位出现在画面里，点它（同触屏脚本 tap_unit；type 1 怪 / NPC、2 物件、4 物品、5 出口） | tap_unit type=1 cls=119 [timeout=10]: wait until a unit of this kind is on screen and tap it (same as the touch script's tap_unit; type 1 monster / NPC, 2 object, 4 item, 5 exit)
static int c_unit_seen(void *a) { float *xy = a; return dm_world_watch_pos(&xy[0], &xy[1]); }
static int cmd_tap_unit(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *ts = dm_ctl_get(a, "type"), *cs = dm_ctl_get(a, "cls"); float xy[2];
    if (!ts || !cs) ERR("missing type / cls");
    dm_world_watch(atoi(ts), (uint32_t)(strcmp(cs, "any") ? strtoul(cs, NULL, 0) : 0xffffff));
    int ok = wait_until(c_unit_seen, xy, dm_ctl_num(a, "timeout", 10)); dm_world_watch(-1, 0);
    if (!ok) ERR("no unit type %s class %s on screen", ts, cs);
    unsigned seq = dm_world_tap(xy[0], xy[1]);
    if (!wait_until(c_tap_done, &seq, 3)) ERR("tap not handled within 3 s");
    static const char *kind[] = {"none", "item", "npc", "object", "exit"}; int r = dm_world_tap_result(seq);
    dm_json_fields(out, cap, "x:f y:f target:s", (double)xy[0], (double)xy[1], r >= 0 && r <= 4 ? kind[r] : "?");
    return 1;
}
// app do=background|foreground|resign|active|minimize|restore|terminate：前端生命周期（见 dm_ctl.h g_dm_ctl_app_fn） | app do=...: front-end lifecycle (see g_dm_ctl_app_fn in dm_ctl.h)
static int cmd_app(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *w = dm_ctl_get(a, "do"); (void)out; (void)cap;
    if (!w) ERR("missing do");
    if (!g_dm_ctl_app_fn) ERR("no front end to drive (headless host?)");
    if (g_dm_ctl_app_fn(w)) ERR("'%s' is not supported by this front end", w);
    return 1;
}

void dm_ctl_commands_init(void) {
    dm_ctl_register("state", cmd_state);
    dm_ctl_register("save_now", cmd_save_now);
    dm_ctl_register("dump", cmd_dump);
    dm_ctl_register("quit", cmd_quit);
    dm_ctl_register("press", cmd_press);
    dm_ctl_register("stick", cmd_stick);
    dm_ctl_register("tap", cmd_tap);
    dm_ctl_register("msg", cmd_msg);
    dm_ctl_register("world_tap", cmd_world_tap);
    dm_ctl_register("walk_to", cmd_walk_to);
    dm_ctl_register("screenshot", cmd_screenshot);
    dm_ctl_register("ui_dump", cmd_ui_dump);
    dm_ctl_register("pause", cmd_pause);
    dm_ctl_register("resume", cmd_resume);
    dm_ctl_register("step", cmd_step);
    dm_ctl_register("wait", cmd_wait);
    dm_ctl_register("tap_ui", cmd_tap_ui);
    dm_ctl_register("await_ui", cmd_await_ui);
    dm_ctl_register("tap_unit", cmd_tap_unit);
    dm_ctl_register("app", cmd_app);
}
