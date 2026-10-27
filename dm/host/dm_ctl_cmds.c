// 控制口的游戏相关命令（协议与传输见 dm_ctl.c，命令表见 dm/verify/control_port.md）。
// 每个命令只调宿主已有的路径（与 DM_KEYS 脚本、前端退出 / 进后台用的同一批函数），不另造输入或存盘通道。
// Game-facing commands of the control port (protocol and transport in dm_ctl.c, command table in dm/verify/control_port.md).
// Each command only calls existing host paths (the same functions the DM_KEYS script and the front ends' quit / background paths use), without inventing new input or save channels.
#include "dm.h"
#include "dm_ctl.h"
#include "dm_hud.h"
#include "dm_engine.h"
#include <unistd.h>
#include <limits.h>
#include <strings.h>

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

// state：局内与否、拍号、暂停、最近打开的面板、玩家（等级 / 生命 / 法力 / 区域 / 坐标 / 死亡 / 经验进度 / 新任务）、控制口统计；
// 再加引擎快照（dm_engine.c）：屏上面板（最上面的在前）、焦点控件与它的面板、手柄光标模式、存盘忙 / 提交推迟 / 挂着的容器
// state: in a run or not, tick, paused, the most recently opened panel, the player (level / life / mana / area / position / dead / experience progress / new quest), control-port counters;
// plus the engine snapshot (dm_engine.c): the panels on screen (top one first), the focused widget and its panel, the controller cursor mode, save busy / commit deferred / mounted containers
static int cmd_state(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    (void)a; (void)err; (void)ecap;
    dm_hud_info h; dm_hud_get(&h);
    char panel[48]; strlcpy(panel, g_lf_panel, sizeof panel);
    size_t n = dm_json_fields(out, cap, "in_game:b tick:u paused:b panel:s level:d hp:d hp_max:d mp:d mp_max:d area:d x:d y:d dead:b players:d holding:d belt0:d belt1:d belt2:d belt3:d portal:d points:d exp_permille:d quest_unread:b ev_dropped:u",
                   h.valid, (unsigned long long)(h.valid ? dm_cur_tick() : 0), h.valid && dm_game_paused(), panel[0] ? panel : NULL, h.level, h.hp, h.hp_max, h.mp, h.mp_max,
                   h.area, h.x, h.y, h.dead, h.players, h.holding, h.belt[0], h.belt[1], h.belt[2], h.belt[3], h.portal, h.points, h.exp_permille, h.quest_unread, (unsigned long long)dm_ctl_dropped());
    if (n < cap) dm_engine_state_json(out + n, cap - n);
    return 1;
}

// save_now [timeout=秒，缺省 5]：请游戏照自己的定时存盘路径马上存一次并等提交（前端退出 / 进后台用的同一个 dm_save_wait；暂停着会先冻结世界、临时放开）
// save_now wait=0：只请下一拍存一次、不等（同 iOS 失活的 dm_save_soon），让后面的步（退出、进后台）落在存盘进行中
// save_now [timeout=seconds, default 5]: ask the game to save right away along its own timed-save path and wait for the commit (the same dm_save_wait the front ends use on quit / background; when paused, the world is frozen and the pause released for this save)
// save_now wait=0: only ask for a save on the next tick without waiting (as dm_save_soon on iOS resign), so the next steps (quit, background) land while the save is in progress
void dm_save_soon(void);
static int cmd_save_now(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    if (!dm_ctl_num(a, "wait", 1)) { dm_save_soon(); dm_json_fields(out, cap, "queued:b", 1); return 1; }
    double t = dm_ctl_num(a, "timeout", 5), ms = 0; uint64_t n = 0; int froze = 0;
    if (t <= 0 || t > 60) ERR("timeout must be 0-60 s");
    int asked = dm_save_wait(t, &n, &froze, &ms);
    if (!asked) ERR("not in a state where a save can be requested (not in a game, or on the loading screen)");
    if (!n) ERR("no save commit within %.1f s", t);
    dm_json_fields(out, cap, "commits:u ms:f paused:b", (unsigned long long)n, ms, froze);
    return 1;
}

// engine：整份引擎快照（面板栈带激活与优先级、焦点 / 悬停控件、光标、存盘状态）加最近 48 个手柄动作与 48 条界面消息；场景运行器在第一个红事件时存成 engine.json
// engine: the whole engine snapshot (panel stack with active and priority, focused / hovered widget, cursor, save state) plus the latest 48 controller actions and 48 UI messages; the scenario runner stores it as engine.json at the first red event
static int cmd_engine(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) { (void)a; (void)err; (void)ecap; dm_engine_full_json(out, cap); return 1; }

// fault arm point=<点> [skip=0] [count=1]：跑着时挂一个存档故障（同 DM_TEST_FS_FAILAT 的点：mountclone / reclone / guestname / renameback / guestopen）；
// fault delay ms=<毫秒>：下一次存档事务第一次写前等这么久（同 DM_TEST_SAVE_DELAY，只管一次）；fault list：各点挂着 / 经过 / 已打的次数
// fault arm point=<point> [skip=0] [count=1]: arm a save fault while running (the DM_TEST_FS_FAILAT points: mountclone / reclone / guestname / renameback / guestopen);
// fault delay ms=<ms>: wait that long before the next save transaction's first write (as DM_TEST_SAVE_DELAY, once only); fault list: each point's armed / seen / fired counts
int dm_fs_fault_arm(const char *pt, int skip, int count);
size_t dm_fs_fault_list(char *out, size_t cap);
void dm_fs_save_delay_once(int ms);
static int cmd_fault(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *op = dm_ctl_get(a, "op"); if (!op) op = "list";
    if (!strcmp(op, "arm")) { const char *pt = dm_ctl_get(a, "point");
        if (dm_fs_fault_arm(pt, (int)dm_ctl_num(a, "skip", 0), (int)dm_ctl_num(a, "count", 1))) ERR("unknown point '%s' (mountclone reclone guestname renameback guestopen), or skip < 0 / count < 1", pt ? pt : ""); }
    else if (!strcmp(op, "delay")) { int ms = (int)dm_ctl_num(a, "ms", 0); if (ms < 1 || ms > 120000) ERR("ms must be 1-120000"); dm_fs_save_delay_once(ms); }
    else if (strcmp(op, "list")) ERR("op must be arm, delay or list");
    size_t n = snprintf(out, cap, ",\"faults\":"); if (n < cap) dm_fs_fault_list(out + n, cap - n);
    return 1;
}

// fe line="秒 动作 参数"：一行前端测试输入（同 DM_TEST_NSMOUSE：虚拟手柄 pad / pads / navpad、键 nav / skey / key / type、输入框表单 sheet、真鼠标 down / move / up / hover / dragto / dbl、点单位 unit），
// 走与真输入同一条前端路（虚拟手柄与屏幕键盘、事件监视器、表单），不像 press 只到游戏取手柄处。秒自此刻起算，排好就回执（不等它做完）；只 Mac 前端
// fe line="seconds action arguments": one line of front-end test input (as DM_TEST_NSMOUSE: virtual controller pad / pads / navpad, keys nav / skey / key / type, the input sheet sheet, real mouse down / move / up / hover / dragto / dbl, unit taps),
// going through the same front-end path as real input (virtual controller and on-screen keyboard, event monitors, the sheet), unlike press, which only reaches the game's controller read. Seconds count from now; replies once scheduled (not when done); Mac front end only
static int cmd_fe(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *l = dm_ctl_get(a, "line"); (void)out; (void)cap;
    if (!l || !*l) ERR("missing line (\"<seconds> <action> <arguments>\")");
    if (!g_dm_ctl_fe_fn) ERR("no front end to drive (headless host, or not the Mac front end)");
    int r = g_dm_ctl_fe_fn(l);
    if (r < 0) ERR("the window is not built yet");
    if (r == 0) ERR("line not understood: %s", l);
    return 1;
}

// inputmode mode=touch|controller、glyph=switch|keyboard|xbox|playstation（可只给一个）：请求切手柄 / 触屏布局与键帽样式，同前端看到输入换了时调的那两个（dm_padmode.c）；
// 切换是事务、要几帧，做完发 padmode 事件。宿主得带界面覆盖层（场景 env DM_ROMFS_OVERLAY=auto），没有覆盖层时请求不起作用
// inputmode mode=touch|controller, glyph=switch|keyboard|xbox|playstation (either may be given alone): request the controller / touch layout and the key cap style, through the same two calls the front ends make when the input changes (dm_padmode.c);
// the switch is a transaction taking a few frames and emits a padmode event when done. The host needs the UI overlay (scenario env DM_ROMFS_OVERLAY=auto); without it requests have no effect
void dm_padmode_request(int pad);
void dm_padmode_request_glyph(int g);
static int cmd_inputmode(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *m = dm_ctl_get(a, "mode"), *g = dm_ctl_get(a, "glyph"); (void)out; (void)cap;
    if (!m && !g) ERR("give mode=touch|controller and/or glyph=switch|keyboard|xbox|playstation");
    static const char *const gl[] = { "switch", "keyboard", "xbox", "playstation" }; int k = -1;
    if (g) { for (int i = 0; i < 4; i++) if (!strcmp(g, gl[i])) k = i; if (k < 0) ERR("glyph must be switch, keyboard, xbox or playstation"); }
    if (m && strcmp(m, "touch") && strcmp(m, "controller")) ERR("mode must be touch or controller");
    if (k >= 0) dm_padmode_request_glyph(k);
    if (m) dm_padmode_request(!strcmp(m, "controller"));
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

// wheel x y dy [precise=1] [view=720] [n=1]：滚轮 / 触控板滚一下（出图画面归一化坐标；走 Mac 前端同一个 dm_wheel_ui：precise = 触控板点数，0 = 鼠标滚轮行数；view = 视图高几点）。
// n > 1：一口气连发 n 个同样的事件、中间不等帧（同触控板甩一下，两帧之间到好几个滚动事件；验滚轮并条，dm_cursor.c dm_ui_ptr）
// wheel x y dy [precise=1] [view=720] [n=1]: one wheel / trackpad scroll (coordinates normalized to the rendered picture; goes through dm_wheel_ui like the Mac front end: precise = trackpad points, 0 = mouse-wheel lines; view = view height in points).
// n > 1: send n identical events back to back without waiting for a frame (as a trackpad flick delivers several scroll events between two frames; checks wheel coalescing, dm_cursor.c dm_ui_ptr)
__attribute__((weak)) int dm_wheel_ui(float nx, float ny, double dy, int precise, double view_h) { (void)nx; (void)ny; (void)dy; (void)precise; (void)view_h; return -1; }   // Mac 前端（touch_mac.c）给强定义；别的平台 -1 = 没有滚轮 | the Mac front end (touch_mac.c) gives the strong definition; elsewhere -1 = no wheel
static int cmd_wheel(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    double x = dm_ctl_num(a, "x", -1), y = dm_ctl_num(a, "y", -1), dy = dm_ctl_num(a, "dy", 0);
    if (x < 0 || x > 1 || y < 0 || y > 1) ERR("x / y must be in [0, 1]");
    long n = (long)dm_ctl_num(a, "n", 1); if (n < 1 || n > 60) ERR("n must be 1-60");
    int r = 0; for (long i = 0; i < n && r >= 0; i++) r = dm_wheel_ui((float)x, dm_ui_ny((float)y), dy, (int)dm_ctl_num(a, "precise", 1), dm_ctl_num(a, "view", 720));
    if (r < 0) ERR("no wheel on this platform");
    wait_frames(3, 5); dm_json_fields(out, cap, "ui:u", (unsigned long long)r); return 1;
}
// drag x0 y0 x1 y1 [frames=20]：按下、在这么多帧里直线拖到终点、抬起（同 tap 的 dm_ui_touch 注入：竖拖 = 滚列表、滚动条上拖 = 拖滑块、滑块上横拖 = 调值）
// drag x0 y0 x1 y1 [frames=20]: press, drag in a straight line to the end point over that many frames, release (the same dm_ui_touch injection as tap: a vertical drag scrolls a list, a drag on a scroll bar drags its thumb, a sideways drag on a slider sets the value)
__attribute__((weak)) void dm_touch_host_live(int op, float nx, float ny) { (void)op; (void)nx; (void)ny; }   // 同上：Mac 真鼠标的手势路（touch_mac.c）；别的平台没有 | as above: the Mac real mouse's gesture path (touch_mac.c); absent elsewhere
__attribute__((weak)) int dm_touch_host_is_live(void) { return 0; }
static int cmd_drag(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    double x0 = dm_ctl_num(a, "x0", -1), y0 = dm_ctl_num(a, "y0", -1), x1 = dm_ctl_num(a, "x1", -1), y1 = dm_ctl_num(a, "y1", -1); int n = (int)dm_ctl_num(a, "frames", 20);
    if (x0 < 0 || x0 > 1 || y0 < 0 || y0 > 1 || x1 < 0 || x1 > 1 || y1 < 0 || y1 > 1) ERR("x0 / y0 / x1 / y1 must be in [0, 1]");
    if (n < 2 || n > 600) ERR("frames must be 2-600");
    if (dm_restart_busy()) ERR("one-key restart in progress: touches are dropped");
    float ay = dm_ui_ny((float)y0), by = dm_ui_ny((float)y1);
    void (*t)(int, float, float) = dm_touch_host_is_live() ? dm_touch_host_live : dm_ui_touch;   // Mac App 的真鼠标走手势状态机（touch_mac.c），同一条路测 | the Mac App's real mouse goes through the gesture state machine (touch_mac.c): test that same path
    t(0, (float)x0, ay); wait_frames(2, 5);
    for (int i = 1; i <= n; i++) { t(1, (float)(x0 + (x1 - x0) * i / n), ay + (by - ay) * i / n); if (!wait_frames(1, 5)) { t(2, (float)x1, by); ERR("frames are not advancing"); } }
    wait_frames(3, 5); t(2, (float)x1, by); wait_frames(3, 5);
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
// 截图路径的范围：控制口只供测试，但它收的路径原先照写，App 能写的文件都能被一个 PNG 盖掉。现在只写 .png、路径里不许 ..，并且——
//   iOS：只收相对路径，落在 Documents 下；
//   Mac：所在目录（已存在，按 realpath 比）要在这一局的运行目录里：DM_EVENTS 所在目录（scenario.py 的结果目录）、DM_APP_SUPPORT 的上一级（mac-app 测试副本）、
//        当前目录、$DM_ROOT/run（缺省 ~/dm-work/run），或临时目录（TMPDIR、/tmp）。返回 NULL = 可以，否则是原因
// Range of screenshot paths: the control port is for tests only, but it used to write whatever path it got, so any file the App can write could be overwritten by a PNG. Now it writes only .png, no .. in the path, and --
//   iOS: relative paths only, landing under Documents;
//   Mac: the directory (existing, compared by realpath) must be inside this run's folders: the directory of DM_EVENTS (scenario.py's result directory), the parent of DM_APP_SUPPORT (mac-app test copies),
//        the current directory, $DM_ROOT/run (default ~/dm-work/run), or a temporary directory (TMPDIR, /tmp). Returns NULL = allowed, otherwise the reason
#if !TARGET_OS_IPHONE
static int under(const char *dir, const char *root) {   // dir（已 realpath）在 root 之下（含相等） | dir (already realpath'd) is under root (or equal)
    char r[PATH_MAX]; if (!root || !*root || !realpath(root, r)) return 0; size_t l = strlen(r);
    return !strncmp(dir, r, l) && (dir[l] == 0 || dir[l] == '/' || (l == 1 && r[0] == '/'));
}
#endif
static const char *shot_path_bad(const char *p) {
    size_t n = strlen(p); if (n < 5 || strcasecmp(p + n - 4, ".png")) return "path must end in .png";
    for (const char *s = p; (s = strstr(s, "..")); s += 2) if ((s == p || s[-1] == '/') && (s[2] == 0 || s[2] == '/')) return "path must not contain ..";
#if TARGET_OS_IPHONE
    if (p[0] == '/') return "path must be relative (it lands under Documents)";
    return NULL;
#else
    char full[PATH_MAX], dir[PATH_MAX], cwd[PATH_MAX] = "";
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    if (p[0] == '/') strlcpy(full, p, sizeof full); else snprintf(full, sizeof full, "%s/%s", cwd, p);
    char *sl = strrchr(full, '/'); if (sl == full) sl++; *sl = 0;
    if (!realpath(full, dir)) return "the directory does not exist";
    char roots[4][PATH_MAX]; int nr = 0; const char *e;
    if ((e = getenv("DM_EVENTS")) && strchr(e, '/')) { strlcpy(roots[nr], e, PATH_MAX); *strrchr(roots[nr], '/') = 0; nr++; }
    if ((e = getenv("DM_APP_SUPPORT")) && strchr(e, '/')) { strlcpy(roots[nr], e, PATH_MAX); *strrchr(roots[nr], '/') = 0; nr++; }
    if ((e = getenv("DM_ROOT")) && *e) snprintf(roots[nr++], PATH_MAX, "%s/run", e); else if ((e = getenv("HOME"))) snprintf(roots[nr++], PATH_MAX, "%s/dm-work/run", e);
    for (int i = 0; i < nr; i++) if (under(dir, roots[i])) return NULL;
    if (under(dir, cwd) || under(dir, getenv("TMPDIR")) || under(dir, "/tmp")) return NULL;
    return "the directory is outside the run folder ($DM_ROOT/run, the scenario result directory, the current directory or a temporary directory)";
#endif
}
static int cmd_screenshot(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *p = dm_ctl_get(a, "path"); long k = (long)dm_ctl_num(a, "frames", 1); char path[1024];
    if (!p || !*p) ERR("missing path");
    if (k < 1 || k > 600) ERR("frames must be 1-600");
    { const char *bad = shot_path_bad(p); if (bad) ERR("%s: %s", bad, p); }
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

// mark name=…：在事件流里打一个标记（基准路线用它切段） | mark name=...: put a marker into the event stream (benchmark routes use it to cut segments)
static int cmd_mark(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *n = dm_ctl_get(a, "name"); if (!n || !*n) ERR("missing name");
    uint64_t dm_nvn_frame_count(void); uint64_t f = dm_nvn_frame_count();   // 帧号与 DM_NVN_TSV 逐帧计时行同一计数 | the frame number uses the same count as the DM_NVN_TSV per-frame timing rows
    DM_EV("mark", "name:s frame:u", n, (unsigned long long)f); dm_json_fields(out, cap, "t:f frame:u", dm_events_now_s(), (unsigned long long)f); return 1;
}

// crash kind=segv|abort|trap：故意让进程崩在控制口线程上（测崩溃这一路：crash 事件、crash.json、系统 .ips、场景判红），只在测试里用
// crash kind=segv|abort|trap: deliberately crash the process on the control-port thread (to test the crash path: crash event, crash.json, the system .ips, the scenario going red); for tests only
static int cmd_crash(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *k = dm_ctl_get(a, "kind"); (void)out; (void)cap;
    if (!k || (strcmp(k, "segv") && strcmp(k, "abort") && strcmp(k, "trap"))) ERR("kind must be segv, abort or trap");
    LOG("控制口：按要求崩溃（%s）", k);
    if (!strcmp(k, "abort")) abort();
    if (!strcmp(k, "trap")) __builtin_trap();
    *(volatile int *)(uintptr_t)8 = 1;   // segv：写低地址 | segv: write to a low address
    return 1;
}

void dm_ctl_commands_init(void) {
    dm_ctl_register("state", cmd_state);
    dm_ctl_register("save_now", cmd_save_now);
    dm_ctl_register("engine", cmd_engine);
    dm_ctl_register("fault", cmd_fault);
    dm_ctl_register("fe", cmd_fe);
    dm_ctl_register("inputmode", cmd_inputmode);
    dm_ctl_register("dump", cmd_dump);
    dm_ctl_register("quit", cmd_quit);
    dm_ctl_register("press", cmd_press);
    dm_ctl_register("stick", cmd_stick);
    dm_ctl_register("tap", cmd_tap);
    dm_ctl_register("wheel", cmd_wheel);
    dm_ctl_register("drag", cmd_drag);
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
    dm_ctl_register("mark", cmd_mark);
    dm_ctl_register("crash", cmd_crash);
}
