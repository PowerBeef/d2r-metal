// nn::oe（应用运行环境）：真机上经 am/oe 服务查询主机状态；宿主直接给固定状态。
// oe::Initialize 在 hle_sdkinit.c，不在这里。
// 环境开关：DM_OPMODE=0 掌机 / 1 主机（默认 1，对应主机模式的渲染档）；DM_LANG=语言码（默认 zh-Hans）。
// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default 1, corresponding to the console-mode render profile); DM_LANG=language code (default zh-Hans).
#include "dm_cursor.h"
#include "hle.h"
#include "dm_uiscreen.h"

// 运行模式是运行时状态（前端可改：iOS 过热降级切掌机）：唯一来源在此，启动时从 DM_OPMODE 取一次，之后不再读环境变量
// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <math.h>
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : 1; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
    return m;
}
// ---------- 系统通知队列（AppletMessage）：前端状态变化 → 游戏每帧 TryPopNotificationMessage 取 ----------
// 消息号（游戏 main 0xe10d30 实际处理的）：15 焦点变化、30 运行模式变化、31 性能模式变化
// ---------- System notification queue (AppletMessage): front end state changes → fetched by the game every frame with TryPopNotificationMessage ----------
// Message numbers (those actually handled by the game at main 0xe10d30): 15 focus change, 30 operation mode change, 31 performance mode change
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // 游戏用 Set*NotificationEnabled 打开后才推 | only pushed after the game turns it on with Set*NotificationEnabled
static void mq_push(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31 是「查询当前状态」的失效通知，不是携带历史值的事件。
    // 同类型尚未取走时保留首次排位：D2R 消费循环只读最新状态，重复通知的写回相同，尺寸相同不再 resize。
    // 只合并这三种状态通知；未来的逐次事件（如 Resume/Exit）不能套这个规则。
    // 合法生产者只发这三种，因此待取最多三项，任意多次失焦/恢复不再撞 32 槽上限；弹出后新变化照常入队。
    // 15/30/31 are invalidation notifications meaning "query the current state", not events carrying historical values.
    // While one of the same type has not been taken yet, the first queue position is kept: the D2R consumer loop only reads the latest state, repeated notifications write back the same thing, and with the same size there is no further resize.
    // Only these three state notifications are merged; future per-occurrence events (such as Resume/Exit) must not use this rule.
    // Legitimate producers only send these three, so at most three items are pending, and any number of focus losses/restores no longer hits the 32-slot limit; after a pop, new changes are enqueued as usual.
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe 通知队列溢出（游戏没在取？）");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_mutex_unlock(&g_mq_lock);
}
void dm_oe_res_changed(void) { if (getenv("DM_TEST_OE_FOCUS_FIRST")) mq_push(MSG_FOCUS);   /* 测试：让 31 排在一条焦点通知后面（iOS 包启动时的顺序） | Test: queue 31 behind a focus notification (the order at iOS app startup) */
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE); else LOG("【红】分辨率要切，但游戏没打开性能模式变化通知"); }   // 出图分辨率该变了（dm_uiscreen.c）：让游戏重读参数位 | the render resolution should change (dm_uiscreen.c): make the game re-read the parameter bits
// 只对 GetOperationMode 报的运行模式（手柄 / 触屏布局切换让游戏重载布局用，dm_padmode.c）：m ≥ 0 报 m 并推一次运行模式通知，m < 0 报回真值再推一次。
// 手柄样式（hle_hid.c connected_id）与性能模式照真值，所以不弹「重新连接控制器」、出图分辨率不变
// Operation mode reported to GetOperationMode only (used to make the game reload layouts when switching between controller / touch layouts, dm_padmode.c):
// m ≥ 0 reports m and pushes one operation-mode notification; m < 0 reports the true value again and pushes one more.
// The controller style (hle_hid.c connected_id) and performance mode keep their true values, so no "Reconnect controller" prompt appears and the render
// resolution does not change
static _Atomic int g_ui_opmode = -1;
static _Atomic int g_ui_armed; static _Atomic uint32_t g_ui_reads;   // 推过通知后游戏第一次来读运行模式记一次：切换事务拿它回读「游戏收到了」 | Counts once when the game first comes to read the operation mode after a notification has been pushed: the switch transaction uses it to read back "the game received it"
uint32_t dm_oe_ui_opmode_reads(void) { return atomic_load(&g_ui_reads); }
void dm_oe_ui_opmode(int m) { g_ui_opmode = m < 0 ? -1 : m; atomic_store(&g_ui_armed, 1); if (g_ntf_opmode) mq_push(MSG_OPMODE); }
void dm_set_opmode(int m) {   // 前端调（过热降级等）：改状态并按游戏打开的开关推送 30 / 31 | called by the front end (overheat downgrade etc.): change the state and push 30 / 31 according to the switches the game has turned on
    int old = dm_opmode(); if (old == m) return;
    g_opmode = m;
    LOG("oe 运行模式 %d → %d", old, m);
    DM_EV("opmode", "from:d to:d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState：1 InFocus、2 OutOfFocus、3 Background。前端调 dm_focus_set（iOS 生命周期 / Mac 窗口失焦）
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background. The front end calls dm_focus_set (iOS lifecycle / Mac window losing focus)
static _Atomic int g_focus = 1; static _Atomic uint64_t g_focus_in_ns;   // 最近一次进入 InFocus 的时刻 | time of the most recent entry into InFocus
int dm_focus_now(void) { return atomic_load(&g_focus); }
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }   // 图形层显示节拍看门狗：只在激活时算停顿，从激活起算（nvn_mtl.m mtl_present_wait） | graphics layer display link watchdog: stalls are only computed while active, counted from activation (nvn_mtl.m mtl_present_wait)
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set 非法状态 %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    LOG("oe 焦点状态 → %d", st);
    mq_push(MSG_FOCUS);
}
static int opmode(void) { return dm_opmode(); }

// 截图 / 录像开关：宿主无系统截图服务，空操作
// Screenshot / video recording switches: the host has no system screenshot service, no-op
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "录像开关；宿主无系统录像", void, h_oe_EnableRec, void) {}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "录像开关；宿主无系统录像", void, h_oe_DisableRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "截图开关；宿主无系统截图", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "截图开关；宿主无系统截图", void, h_oe_DisableShot, void) {}

// OperationMode：0 Handheld、1 Console；PerformanceMode：0 Normal、1 Boost（与运行模式一一对应）
// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "运行模式查询；按 DM_OPMODE 给固定值", int, h_oe_GetOpMode, void) { int u = g_ui_opmode; if (atomic_exchange(&g_ui_armed, 0)) atomic_fetch_add(&g_ui_reads, 1); return u >= 0 ? u : opmode(); }
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "性能模式查询；与运行模式一致", int, h_oe_GetPerfMode, void) { return opmode(); }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "设 CPU/GPU 频率档；宿主不调频", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x（忽略）", mode, cfg);
}

// 脚本化状态变化（无窗口验证生命周期 / 过热降级）：DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30"（第 N 秒）
// 时刻从游戏第一次取通知算起；「@ingame+N」= 读取画面第 K 次撤下（K = 前面 @ingame 项数 + 1，首个即进局）后 N 秒（按事件，不受负载影响）。与前端调用并存。
// Scripted state changes (verify lifecycle / overheat downgrade without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (second N)
// Time counts from the game's first notification fetch; "@ingame+N" = N seconds after the loading screen is taken down for the K-th time (K = number of preceding @ingame items + 1, the first one is run entry) (event-based, not affected by load). Coexists with front end calls.
#include <unistd.h>
static _Atomic int g_ls_unloads;   // 读取画面撤下次数（diag.c 的 UIMSG 处理调 dm_note_loadscreen_unload） | number of times the loading screen was taken down (the UIMSG handler in diag.c calls dm_note_loadscreen_unload)
void dm_note_loadscreen_unload(void) { atomic_fetch_add(&g_ls_unloads, 1); }
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@ingame+%lf", key, &v, &at) == 3) {
            static int need;
            need++; while (atomic_load(&g_ls_unloads) < need) usleep(20000);
            usleep((useconds_t)(at * 1e6));
        } else {
            if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT 项「%s」格式应为 键=值@秒 或 键=值@ingame+秒", tok);
            uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
            if (due > now) usleep((useconds_t)((due - now) / 1000));
        }
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else die("DM_OE_SCRIPT 未知键「%s」（focus / opmode）", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT 线程");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "前后台状态；由前端 dm_focus_set 驱动", int, h_oe_GetFocus, void) { return g_focus; }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "前后台切换策略；游戏设 1（收焦点通知），宿主按通知语义实现", void, h_oe_SetFocusMode, int m) { LOG("oe::SetFocusHandlingMode %d", m); }

// Notification: returns bool (w0); the host has no system notifications
// ---------- 3D internal render scale ----------
// The game has a built-in static 3D render scale: renderer object (global pointer [main+0x31e0848]) member +0x66a4 (float, always 1.0 in the original game).
// The resolution-change function main+0x8582f0(output width, output height) -> main+0x1627ef0 rebuilds the 3D targets at "scale x output size"
// (+0xee78 / +0xee7c), while the UI layer and Final Result are still built at the output size (+0xee88 / +0xee8c), and at the end the game's own upscale
// shader ps_upsample_p (or MetalFX, nvn_mtl.m mfx3d_draw) scales back to the output size.
// The startup flow does not call 0x8582f0, so it must be called once proactively; the call site must be where the game main thread fetches system
// notifications, the same context in which the game itself handles mode switches (calling it inside the present HLE self-deadlocks: the render lock is
// already held by the present path).
// The configured value: DM_3D_SCALE=0.5-1.0, else the 3D Resolution setting (dm_video.c), else 0.75 on phones and 1.0 on the Mac host. During the
// visual ruler's reference phase (A / B of DM_NVN_RULER=1) it is forced to 1.0 and takes effect only after switching to the mode under test.
// Frame Drops (the dynamic scale, on with Frame Drops = Lower; Keep pins the configured value): the controller judges the actual on-screen present
// intervals hle_nvn.c reports for every present (diag.c dm_perf_present, the same source as perf.txt and the perf event), on the Mac as on iOS. Its
// levels are up to three steps of x0.93 below the configured value, never below 50% (dm_s3lv.h): 0.70 / 0.65 / 0.60 at 75%, 0.93 / 0.86 / 0.80 at
// 100%, none at 50%. Its rules: scale3d_ctl_step; frames not shown, loading screens and gaps in the presents: dm_3d_scale_feed.
// Switches: DM_3D_SCALE_DYN=0 disables it; DM_THERMAL_NOFALLBACK=1 (power / frame-time measurement) also disables it, since the scale must be pinned
// while measuring. DM_3D_SCALE_FLOOR=<scale> allows deeper levels for device sweeps (up to 12, never below 50%).
// DM_3D_SCALE_DYN_INJECT=<a>-<b> (frames counted by dm_nvn_frame_count) tests the whole step-down / step-up chain: in that range every interval fed to
// the controller counts as two periods (a drop), outside it as one; +<a>-<b> counts from the first frame the controller judges in a game
// (s3_inject_parse). It acts only in a game, and the first step comes after a full 10 s (5 windows). While it is set, Keep does not stop the controller
// unless DM_TEST_Q3D=1 says so (s3_qmode), so an injection run steps on the Mac too.
#include <TargetConditionals.h>
#include "dm_s3lv.h"
// The scale of level lv and the deepest level for a configured value (dm_s3lv.h); floor_env = DM_3D_SCALE_FLOOR, 0 when unset
static float s3_level_scale(float cfg, int lv) { return dm_s3_level_scale(cfg, lv); }
static int s3_max_level(float cfg, float floor_env) { return dm_s3_max_level(cfg, floor_env); }
// 配置值：DM_3D_SCALE（0.5–1.0，读一次），否则 iOS 选项页的「3D 清晰度」，再否则缺省手机 0.75、Mac 宿主 1.0
// Configured value: DM_3D_SCALE (0.5–1.0, read once), else the iOS options page's "3D Resolution", else the default: 0.75 on phones, 1.0 on the Mac host
__attribute__((weak)) float dm_video_scale3d(void) { return 0; }   // 宿主 dm_video.c 给强定义（手机选项页「3D 清晰度」）；单测不链它时恒 0 | the host's dm_video.c gives the strong definition (phone options page "3D Resolution"); 0 when a unit test does not link it
static float g_s3_env;   // DM_3D_SCALE, read once by s3_env_read (0 = not set)
static void s3_env_read(void) {
    const char *e = getenv("DM_3D_SCALE");
    if (e) { char *end; float v = strtof(e, &end); if (end == e || *end || !(v >= 0.5f && v <= 1.0f)) die("DM_3D_SCALE 非法：%s（要求 0.5–1.0）", e); g_s3_env = v; }
}
static float s3_cfg(void) {   // 配置的 3D 比例：DM_3D_SCALE 定死；没给就按选项页（手机，点「应用」后 scale3d_tick 下一拍就换），再没有就平台缺省 | configured 3D scale: DM_3D_SCALE fixes it; otherwise the options page (phones; scale3d_tick switches on the next tick after "Apply"), otherwise the platform default
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, s3_env_read);   // the game, present and replay threads all call it
    if (g_s3_env > 0) return g_s3_env;
    float v = dm_video_scale3d(); return v >= 0.5f && v <= 1.0f ? v : DM_PHONE ? 0.75f : 1.0f;
}
// Pure-function controller (the unit test t_oe.c feeds it sequences directly): level 0 = the configured value, higher = lower. It judges only what
// reached the screen:
// - A dropped frame = a present interval over 1.5 x the pacer's frame period + 0.25 ms (25.25 ms at 60 fps), or a frame not shown (-1); one window
//   every 2 s records its frames and dropped frames, and the last 30 windows (60 s) are kept. The margin matters on screens that are not
//   multiples of 60: at 72 fps one refresh late is exactly 1.5 periods (20.83 ms), so without it timestamp jitter decided whether it counted;
//   two refreshes late (27.8 ms) always count.
// - Lower: dropped-frame rate over the last 5 windows (10 s) > 5% (fast: lower as soon as a heavy scene arrives), or over the last 30 windows (60 s)
//   > 1.5% (slow: also lower on long-term minor drops) -> down one level, pause judging for 3 s, clear the window records.
// - Raise: 15 consecutive windows (30 s) without a dropped frame -> up one level; if knocked back within 15 s of being raised, the next raise needs
//   45 consecutive windows (90 s).
// - Thresholds set from real-play recordings: real-play segment 2 (59+ fps, occasional stutter) worst 10 s 3.7%, per minute 0.97% / 0.47% / 0.39%,
//   triggering neither rule; a warm v1.0.15 device in a heavy scene dropped 12.6% and triggers.
// - A run of consecutive dropped frames counts only its first 3, unless the run lasts 1 s and holds a frame that reached the screen late (from then
//   on every frame counts): one-off stalls such as a screenshot, a save or a system notification are not helped by a lower resolution (on 2026-10-20
//   the Redmi test harness's screenshot every 10 s dropped about 20 frames in a row each time, and the old count stepped down to 0.60); sustained
//   load (long runs with late frames, or frequent short runs) still counts.
// - R1: a run of only frames not shown (-1) counts its first 3 and never escalates: a window covered or out of view shows nothing whatever the GPU
//   does, so it is no evidence of load.
// - R2: a 2 s window with no frame on screen at all is neither judged nor counted as clean (the clean run stays as it was).
// The feed (dm_3d_scale_feed) also keeps frames out of the controller while the picture is not shown (presents discarded or paused for an inactive
// shell), outside a game and under a loading screen.
// The old rule (lower only when the window frame rate < target x 0.92, only two levels 0.72 / 0.70) bottomed out at 0.70 in heavy scenes, and warm
// steady state still dropped 12.6% (Hc run).
typedef struct { int level, max_level; double period_ms; uint64_t hold_until, last_up; double win_ms; unsigned win_n, win_d, win_s;
                 uint16_t wn[30], wd[30]; int nw; int good_run, need_good; double last_r10, last_r60; unsigned run_n; double run_ms; int run_late; } scale3d_ctl;
#define S3_SEC 1000000000ull
#define S3_GAP_NS 500000000ull   // presents stopped for longer than this (no explicit signal): judging pauses, the history stays
#define S3_LATE_MARGIN_MS 0.25   // above 1.5 periods: a frame one refresh late plus timestamp jitter is not a drop
// Runs of dropped frames: only the first S3_BURST_FRAMES count until the run has lasted S3_BURST_MS with a late frame on screen in it
#define S3_BURST_FRAMES 3u
#define S3_BURST_MS 1000.0
// Pause judging for a while and clear the windows and history (entering a game, returning to the foreground, the picture visible again, a level change)
static void scale3d_ctl_hold(scale3d_ctl *c, uint64_t now, uint64_t ns) {
    if (now + ns > c->hold_until) c->hold_until = now + ns;
    c->win_ms = 0; c->win_n = c->win_d = c->win_s = 0; c->nw = 0; c->good_run = 0; c->run_n = 0; c->run_ms = 0; c->run_late = 0;
}
// Pause judging only (presents stopped for over S3_GAP_NS with no explicit signal, e.g. a long stall): the windows, the partial window and the clean
// run are kept, so one stall does not wipe the 60 s history the 1.5% rule needs; the run of drops in progress ends
static void scale3d_ctl_pause(scale3d_ctl *c, uint64_t now, uint64_t ns) {
    if (now + ns > c->hold_until) c->hold_until = now + ns;
    c->run_n = 0; c->run_ms = 0; c->run_late = 0;
}
static double s3_rate(const scale3d_ctl *c, int k) {   // 最近 k 窗的掉帧率（窗不足 k 个按已有的算；没有窗返回 0） | Dropped-frame rate over the last k windows (fewer than k windows: compute over those present; no windows returns 0)
    unsigned n = 0, d = 0; for (int i = 0; i < k && i < c->nw; i++) { n += c->wn[i]; d += c->wd[i]; } return n ? (double)d / n : 0;
}
static int scale3d_ctl_step(scale3d_ctl *c, double iv_ms, uint64_t now) {   // returns the current level
    if (now < c->hold_until || iv_ms == 0) return c->level;   // judging paused / no reading (0)
    double per = c->period_ms > 0 ? c->period_ms : 1000.0 / 60;
    // A frame not shown (-1) counts as a dropped frame too; how runs of dropped frames count (R1 included): see above
    c->win_n++; if (iv_ms > 0) c->win_s++;
    if (iv_ms < 0 || iv_ms > per * 1.5 + S3_LATE_MARGIN_MS) { c->run_n++; c->run_ms += iv_ms > 0 ? iv_ms : per; if (iv_ms > 0) c->run_late = 1;
        if (c->run_n <= S3_BURST_FRAMES || (c->run_ms >= S3_BURST_MS && c->run_late)) c->win_d++; }
    else { c->run_n = 0; c->run_ms = 0; c->run_late = 0; }
    c->win_ms += iv_ms > 0 ? iv_ms : per;
    if (c->win_ms < 2000) return c->level;
    if (!c->win_s) { c->win_ms = 0; c->win_n = c->win_d = 0; return c->level; }   // R2: nothing on screen in the window: not judged, not clean
    memmove(c->wn + 1, c->wn, sizeof c->wn - sizeof c->wn[0]); memmove(c->wd + 1, c->wd, sizeof c->wd - sizeof c->wd[0]);
    c->wn[0] = (uint16_t)c->win_n; c->wd[0] = (uint16_t)c->win_d; if (c->nw < 30) c->nw++;
    int clean = c->win_d == 0; c->win_ms = 0; c->win_n = c->win_d = c->win_s = 0;
    c->last_r10 = s3_rate(c, 5); c->last_r60 = c->nw >= 30 ? s3_rate(c, 30) : 0;
    if (c->level < c->max_level && ((c->nw >= 5 && c->last_r10 > 0.05) || (c->nw >= 30 && c->last_r60 > 0.015))) {
        if (c->last_up && now - c->last_up < 15 * S3_SEC) c->need_good = 45;   // 刚升上去 15 秒内又掉：下次升要 90 秒 | Dropped again within 15 s of being raised: the next raise needs 90 s
        c->level++; scale3d_ctl_hold(c, now, 3 * S3_SEC); return c->level;
    }
    if (clean) { if (++c->good_run >= (c->need_good ? c->need_good : 15) && c->level > 0) { c->level--; c->last_up = now; c->need_good = 0; scale3d_ctl_hold(c, now, 3 * S3_SEC); } }
    else c->good_run = 0;
    return c->level;
}
// The configured value changed (the 3D Resolution setting was applied), or Frame Drops went from Keep back to Lower: recompute the deepest level for
// the value, go back to level 0 (the value itself) and pause judging for 3 s with the history cleared. Levels are relative to the configured value,
// so an old level kept across a change would be a different scale.
static void scale3d_ctl_retarget(scale3d_ctl *c, float cfg, float floor, uint64_t now) {
    c->max_level = s3_max_level(cfg, floor); c->level = 0; c->last_up = 0; c->need_good = 0; scale3d_ctl_hold(c, now, 3 * S3_SEC);
}
// The Frame Drops setting: 0 = Lower (the 3D scale steps down on dropped frames), 1 = Keep (the configured value stays). The Video tab's Frame Drops
// row sets it (dm_video.c row "drops", custom key q3d in sw_custom.txt, dm_custom.c); the default is Lower on phones and comes from the Mac's
// recommendation (front_q3d_default)
static _Atomic int g_3d_quality_mode;
void dm_3d_quality_mode_set(int mode) { atomic_store(&g_3d_quality_mode, mode == 1 ? 1 : 0); }
int dm_3d_quality_mode_get(void) { return atomic_load(&g_3d_quality_mode); }
// The Frame Drops mode the controller goes by (1 = Keep, 0 = Lower), with two test overrides read once:
// - DM_TEST_Q3D=0|1 forces it, so a test runs the controller (or keeps it off) whatever the settings file says (the Mac's default is Keep);
// - with DM_3D_SCALE_DYN_INJECT set and no DM_TEST_Q3D, Keep does not stop the controller, so an injection run steps down and back up
//   (the 3D target rebuilds it exists to test) on the Mac too.
static int g_s3_qforce = -1;   // -1 = no override
static void s3_qforce_read(void) {
    const char *e = getenv("DM_TEST_Q3D");
    if (e && (*e == '0' || *e == '1') && !e[1]) { g_s3_qforce = *e - '0'; LOG("DM_TEST_Q3D=%d: Frame Drops forced to %s", g_s3_qforce, g_s3_qforce ? "Keep" : "Lower"); return; }
    if (e) LOG("【红】DM_TEST_Q3D=%s is not 0 or 1: ignored", e);   // ignored as if unset, so the injection bypass below still applies
    if (getenv("DM_3D_SCALE_DYN_INJECT")) { g_s3_qforce = 0; LOG("DM_3D_SCALE_DYN_INJECT: Frame Drops runs as Lower for the injection test, whatever the setting"); }
}
static int s3_qmode(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, s3_qforce_read);
    return g_s3_qforce >= 0 ? g_s3_qforce : atomic_load(&g_3d_quality_mode);
}
// DM_3D_SCALE_DYN_INJECT=<a>-<b>: frames a..b counted by dm_nvn_frame_count; +<a>-<b>: counted from the first frame the controller judges in a game
// (latched once), so a scenario does not depend on how many frames the boot took; a single number <a> means <a>-<a>. Returns 1 for the + form
static int s3_inject_parse(const char *j, long *a, long *b) {
    int rel = *j == '+'; if (rel) j++;
    *a = atol(j); const char *d = strchr(j, '-'); *b = d ? atol(d + 1) : *a;
    return rel;
}
static _Atomic float g_s3_r10, g_s3_r60;   // the drop rates (percent) behind the controller's last level change, for the scale3d event
// The drop rates for a scale3d event: those of the controller's last step, taken by the first rebuild after it, so a later rebuild with no step
// behind it reads 0. Keep and a new 3D Resolution put the level back to 0 without a step and clear them too, in case their rebuild comes first
static void s3_ev_rates(double *r10, double *r60) { *r10 = atomic_exchange(&g_s3_r10, 0.0f); *r60 = atomic_exchange(&g_s3_r60, 0.0f); }
static void s3_rates_clear(void) { atomic_store(&g_s3_r10, 0.0f); atomic_store(&g_s3_r60, 0.0f); }
static _Atomic int g_scale3d_level;
static _Atomic int g_scale3d_resumed;
void dm_3d_scale_resumed(void) { atomic_store(&g_scale3d_resumed, 1); }   // 前端：刚回前台。回来头几秒的慢帧是恢复（表面重建、缓存变冷），不是 GPU 吃紧：停判 3 秒 | front end: just returned to foreground. Slow frames in the first few seconds back are recovery (surface rebuild, caches gone cold), not GPU pressure: suspend judging for 3 seconds
// 按 GPU 耗时的动态 3D 比例（DM_3D_SCALE_GPU=1）：上面的控制器按「60 帧、超 25 ms 算掉帧」判，帧率在 30 上下时一进局就降到底，且档位是绝对值。
// 这里每 2 秒看一次：帧时间 > 35 ms 且 GPU 占帧时间 ≥ 85%（卡在 GPU，发热后常见）→ 降一档；GPU 只占 < 75%（有富余）连续 10 秒 → 升一档。档位 = 配置值 × 系数，
// 下限 DM_3D_SCALE_GPU_FLOOR（缺省 0.40）。卡在 CPU 时不降（降分辨率不省 CPU）。
// GPU-time-driven dynamic 3D scale (DM_3D_SCALE_GPU=1): the controller above judges by "60 fps, > 25 ms is a drop", so around 30 fps it hits the bottom as soon as
// a run starts, and its levels are absolute. Here, every 2 s: frame time > 35 ms with the GPU at >= 85% of it (GPU-bound, common once hot) -> one level down; GPU below 75% (headroom)
// for 10 s in a row -> one level up. Level = configured value x factor, floor DM_3D_SCALE_GPU_FLOOR (default 0.40). CPU-bound frames do not lower it (resolution does not save CPU).
static const float g_s3g_mult[] = {0.92f, 0.85f, 0.78f, 0.72f, 0.66f, 0.60f};
static _Atomic float g_s3g_scale;   // > 0：覆盖动态档（scale3d_tick）| > 0: overrides the dynamic level (scale3d_tick)
__attribute__((weak)) double xbg_recent_gpu_ms(void) { return -1; }   // 空的弱定义（不是弱声明）：苹果链接器要求弱引用链接时有定义；Vulkan 后端 / 鸿蒙平台层给强定义 | empty weak definitions (not declarations): Apple's linker needs a weak reference to resolve; the Vulkan backend / HarmonyOS platform give the strong ones
static int s3g_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_3D_SCALE_GPU"); on = e && *e == '1'; } return on; }
static void s3g_feed(double iv_ms) {
    static double acc; static unsigned n; static uint64_t hold; static int lv, good, maxlv = -1, was_in; static float cfg;
    if (maxlv < 0) { const char *e = getenv("DM_3D_SCALE"), *f = getenv("DM_3D_SCALE_GPU_FLOOR"); cfg = e ? strtof(e, NULL) : 0.75f; float fl = f ? strtof(f, NULL) : 0.40f;
        maxlv = 0; for (int k = 0; k < (int)(sizeof g_s3g_mult / sizeof g_s3g_mult[0]); k++) if (cfg * g_s3g_mult[k] >= fl - 1e-4f) maxlv = k + 1;
        LOG("3D 比例控制（按 GPU 耗时）：配置 %.2f，最多降 %d 档（最低 %.2f）", cfg, maxlv, maxlv ? cfg * g_s3g_mult[maxlv - 1] : cfg); }
    uint64_t now = now_ns();
    if (!dm_ingame_now()) { was_in = 0; acc = 0; n = 0; return; }
    if (!was_in) { was_in = 1; hold = now + 20 * S3_SEC; }   // 刚进局的 20 秒是装载 | the first 20 s of a run are loading
    if (iv_ms > 0) { acc += iv_ms; n++; }
    if (acc < 2000 || !n) return;
    double fr = acc / n, g = xbg_recent_gpu_ms ? xbg_recent_gpu_ms() : -1; acc = 0; n = 0;
    if (now < hold || g <= 0) return;
    int old = lv;
    if (fr > 35.0 && g >= 0.85 * fr && lv < maxlv) { lv++; good = 0; }
    else if (lv > 0 && g < 0.75 * fr) { if (++good >= 5) { lv--; good = 0; } }
    else good = 0;
    if (lv != old) { hold = now + 4 * S3_SEC; atomic_store(&g_s3g_scale, lv ? cfg * g_s3g_mult[lv - 1] : 0.0f);
        LOG("3D 比例控制（按 GPU 耗时）：帧 %.1f ms、GPU %.1f ms → %s到第 %d 档（3D 比例 %.2f）", fr, g, lv > old ? "降" : "升", lv, lv ? cfg * g_s3g_mult[lv - 1] : cfg); }
}
// The feed's state, at file scope so the unit test (t_oe.c) can reset it: the controller, whether it runs (-1 = not read yet), the Frame Drops mode
// of the last frame, whether the last frame was in a game, DM_3D_SCALE_DYN_INJECT's range (+ form: rel, latched at base), DM_3D_SCALE_FLOOR, the
// configured value c.max_level was computed for, and the time of the last frame fed
typedef struct { scale3d_ctl c; int on, qm_was, was_in, was_hidden, inj_rel; long inj0, inj1, inj_base; float fl, cfg_at; uint64_t last_feed; } s3_feed_state;
#define S3_FEED_INIT { .on = -1, .qm_was = -1, .inj0 = -1, .inj1 = -1, .inj_base = -1 }
static s3_feed_state g_s3f = S3_FEED_INIT;
void dm_3d_scale_feed(double iv_ms) {   // 出图线程每帧调（diag.c dm_perf_present 转来）：实际上屏间隔 ms，−1 = 被丢弃，0 = 无读数 | called by the present thread every frame (forwarded from diag.c dm_perf_present): actual present interval in ms, −1 = dropped, 0 = no reading
    s3_feed_state *f = &g_s3f; scale3d_ctl *c = &f->c;
    if (f->on < 0) { const char *e = getenv("DM_3D_SCALE_DYN"), *j = getenv("DM_3D_SCALE_DYN_INJECT"); f->on = !(e && !strcmp(e, "0")) && !getenv("DM_THERMAL_NOFALLBACK");
        if (j) f->inj_rel = s3_inject_parse(j, &f->inj0, &f->inj1);
        const char *fe = getenv("DM_3D_SCALE_FLOOR"); f->fl = fe ? strtof(fe, NULL) : 0;   // a deeper floor for device sweeps (dm_s3_max_level); 0 = the levels of dm_s3lv.h
        f->cfg_at = s3_cfg(); c->max_level = s3_max_level(f->cfg_at, f->fl);
        { double nvn_frame_period_ms(void); c->period_ms = nvn_frame_period_ms(); }   // the period at the first frame is no change (no 2 s hold)
        LOG("3D 比例控制：%s，配置 %.2f，下限 %.2f（第 %d 档）", f->on ? "开" : "关", f->cfg_at, s3_level_scale(f->cfg_at, c->max_level), c->max_level); }
    if (!f->on) return;
    // Presents stopped for over S3_GAP_NS (graphics suspended for the background or a minimize, or a long stall): pause judging 3 s and keep the
    // history (scale3d_ctl_pause). The reading that spans the gap arrives about 8 presents later, inside the pause
    uint64_t now = now_ns();
    if (f->last_feed && now - f->last_feed > S3_GAP_NS) scale3d_ctl_pause(c, now, 3 * S3_SEC);
    f->last_feed = now;
    if (s3g_on() && s3_qmode() != 1) { s3g_feed(iv_ms); return; }
    if (s3_qmode() == 1) { if (c->level) { c->level = 0; s3_rates_clear(); LOG("3D 比例控制：画质优先，回到配置值"); } atomic_store(&g_scale3d_level, 0); f->qm_was = 1; return; }
    // Keep -> Lower: judge from fresh history (the windows, the clean run and the raise debounce from before Keep are stale) after 3 s. qm_was starts
    // at -1, so the first frame in Lower does not retarget
    if (f->qm_was == 1) { float cfg = s3_cfg(); scale3d_ctl_retarget(c, cfg, f->fl, now); f->cfg_at = cfg; LOG("Frame Drops set to Lower: history cleared, judging resumes in 3 s"); }
    f->qm_was = 0;
    { float cfg = s3_cfg(); if (cfg != f->cfg_at) { scale3d_ctl_retarget(c, cfg, f->fl, now); s3_rates_clear(); atomic_store(&g_scale3d_level, 0);   // 选项页换了 3D 清晰度：档位按新值重算 | the options page changed the 3D resolution: levels recomputed for the new value
        LOG("3D 比例控制：配置 %.2f → %.2f，回到配置值，下限 %.2f（第 %d 档）", f->cfg_at, cfg, s3_level_scale(cfg, c->max_level), c->max_level); f->cfg_at = cfg; } }
    if (atomic_exchange(&g_scale3d_resumed, 0)) scale3d_ctl_hold(c, now, 3 * S3_SEC);
    // Frames not shown are not judged: presents discarded (a covered Mac window, the iOS background save wait) or paused for an inactive shell
    // (iOS resign active, mtl_present_inactive). A Mac window that is visible but not key keeps being judged: its frames are on screen. When the
    // picture is visible again, judging waits 3 s with the history cleared (an explicit signal, as returning to the foreground).
    // The world freeze (diag.c freeze_tick; DM_PANEL_FREEZE / DM_TEST_FREEZE) is dormant for players. If it is ever enabled, its frozen frames present
    // at full rate and would count as clean windows: gate them here like hidden frames and hold 3 s after unfreezing
    { int mtl_present_hidden(void);
      if (mtl_present_hidden()) { f->was_hidden = 1; scale3d_ctl_hold(c, now, S3_SEC); return; }
      if (f->was_hidden) { f->was_hidden = 0; scale3d_ctl_hold(c, now, 3 * S3_SEC); LOG("Frame Drops: picture visible again, judging resumes in 3 s"); } }
    // Not judged outside a game (menus) or under a loading screen, nor in the first 20 s of a game or after a loading screen (slow frames then are
    // loading, not GPU pressure)
    { int dm_loadscreen_up(void), in = dm_ingame_now() && !dm_loadscreen_up();
      if (!in) { f->was_in = 0; scale3d_ctl_hold(c, now, S3_SEC); return; }
      if (!f->was_in) { f->was_in = 1; if (f->inj0 < 0) scale3d_ctl_hold(c, now, 20 * S3_SEC); } }
    // The target period is the pacer's real frame period (hle_nvn.c nvn_frame_period_ms: the power saver, DM_PRESENT_HZ, the adaptive tier, 72 fps
    // on a 144 Hz screen); 0 (uncapped) judges against 60 fps. Judging pauses 2 s after it changes
    { double nvn_frame_period_ms(void), p = nvn_frame_period_ms(); if (fabs(p - c->period_ms) > 0.01) { c->period_ms = p; scale3d_ctl_hold(c, now, 2 * S3_SEC); } }
    // Injection: every frame within the segment counts as a dropped frame (two periods). The + form is latched at the first frame scale3d_ctl_step
    // judges (no hold left: the holds above are all set by now), and frames before it count as clean
    if (f->inj0 >= 0) { double per = c->period_ms > 0 ? c->period_ms : 1000.0 / 60;
        if (f->inj_rel && f->inj_base < 0 && now >= c->hold_until) { f->inj_base = (long)dm_nvn_frame_count(); LOG("DM_3D_SCALE_DYN_INJECT=+%ld-%ld: frames counted from frame %ld, the first judged one", f->inj0, f->inj1, f->inj_base); }
        long fr = (long)dm_nvn_frame_count() - (f->inj_rel ? f->inj_base : 0); iv_ms = f->inj_rel && f->inj_base < 0 ? per : fr >= f->inj0 && fr <= f->inj1 ? 2 * per : per; }
    int was = c->level, lv = scale3d_ctl_step(c, iv_ms, now);
    if (lv != was) { atomic_store(&g_s3_r10, (float)(c->last_r10 * 100)); atomic_store(&g_s3_r60, (float)(c->last_r60 * 100)); }   // before the level, which scale3d_tick reads first
    if (lv != was) LOG("3D 比例控制：最近 10 秒掉帧 %.1f%%、60 秒 %.1f%% → %s到第 %d 档（3D 比例 %.2f）%s", c->last_r10 * 100, c->last_r60 * 100, lv > was ? "降" : "升", lv,
                       s3_level_scale(s3_cfg(), lv), c->need_good ? "（刚升又掉：下次升档要 90 秒）" : "");
    atomic_store(&g_scale3d_level, lv);
}
void dm_3d_scale_hot(int hot) { (void)hot; }   // 过热不再直接降档（改按实际掉帧）；保留符号给前端 | overheating no longer lowers the level directly (now by actual dropped frames); the symbol is kept for the front end
static _Atomic float g_scale3d_cur = 1.0f;
float dm_3d_scale_cur(void) { return atomic_load(&g_scale3d_cur); }
// The configured 3D scale (DM_3D_SCALE, else the 3D Resolution setting, else the platform default), without the Frame Drops level; any thread
float dm_3d_scale_cfg(void) { return s3_cfg(); }
static void scale3d_tick(void) {
    static float cur = 1.0f; static module_t *main; static int cur_lv;   // cur_lv: the Frame Drops level of the last rebuild
    if (!main) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) main = &g_mod[i];
    float want_cfg = s3_cfg();   // 每拍读：手机选项页改了 3D 清晰度，下一拍就按新值重建 3D 目标 | read every tick: when the phone options page changes the 3D resolution, the 3D targets are rebuilt at the new value on the next tick
    // 动态档：0 = 配置值，k = 表里第 k 个低于配置值的比例
    // Dynamic level: 0 = configured value, k = the k-th table scale below the configured value
    int lv = atomic_load(&g_scale3d_level); float dyn = s3_level_scale(want_cfg, lv);
    if (s3g_on()) { float g = atomic_load(&g_s3g_scale); dyn = g > 0 && g < want_cfg ? g : want_cfg; }   // GPU 时间控制器（DM_3D_SCALE_GPU=1）覆盖帧间隔档 | GPU-time controller (DM_3D_SCALE_GPU=1) overrides the frame-interval level
    float want = dm_nvn_ruler_truth() ? 1.0f : dyn;
    if (want == cur || !main || dm_nvn_frame_count() < 30) return;
    uintptr_t renderer = *(uintptr_t *)(main->base + 0x31e0848); if (!renderer) return;
    uint32_t ow = *(uint32_t *)(renderer + 0xee88), oh = *(uint32_t *)(renderer + 0xee8c); if (!ow || !oh) return;
    { extern void mtl_rt_force_full(int) __attribute__((weak)); if (mtl_rt_force_full) mtl_rt_force_full(4); }   // 运行时外推：换比例前后几帧一律全画（主通道换尺寸那帧不能外推） | runtime extrapolation: the few frames before and after a scale change are all drawn in full (the frame where the main pass changes size cannot be extrapolated)
    float *scale = (float *)(renderer + 0x66a4); dm_guest_write_prepare(scale, sizeof *scale); *scale = want;
    ((void (*)(uint32_t, uint32_t))(main->base + 0x8582f0))(ow, oh);
    cur = want; atomic_store(&g_scale3d_cur, want);
    LOG("3D 渲染比例 %.2f：3D 目标 %ux%u，输出 %ux%u（帧 %llu）", want, *(uint32_t *)(renderer + 0xee78), *(uint32_t *)(renderer + 0xee7c),
        *(uint32_t *)(renderer + 0xee88), *(uint32_t *)(renderer + 0xee8c), (unsigned long long)dm_nvn_frame_count());
    // One scale3d event per rebuild of the 3D targets (dm/verify/event_stream.md): from / to = the Frame Drops level of the last rebuild and of this one
    // (to = lv), r10 / r60 = the drop rates of the controller step this rebuild applies (s3_ev_rates; 0 with no step behind it: the boot rebuild,
    // a new 3D Resolution, Keep)
    { double r10, r60; s3_ev_rates(&r10, &r60);
      DM_EV("scale3d", "from:d to:d lv:d scale:f pct:u tw:u th:u ow:u oh:u r10:f r60:f", cur_lv, lv, lv, round(want * 1000) / 1000.0, (unsigned long long)lroundf(want * 100),
            (unsigned long long)*(uint32_t *)(renderer + 0xee78), (unsigned long long)*(uint32_t *)(renderer + 0xee7c),
            (unsigned long long)*(uint32_t *)(renderer + 0xee88), (unsigned long long)*(uint32_t *)(renderer + 0xee8c),
            round(r10 * 10) / 10.0, round(r60 * 10) / 10.0);
      cur_lv = lv; }
}
// Anti-aliasing mode: the game's own byte renderer+0x6614 (0 none / 1 FXAA / 2 SMAA / 3 SMAA T2x; the console config is 1). The scene setup (main+0x162f738)
// copies it into each frame's settings block, and the post-processing node (main+0x166e710) picks FXAA (1) or SMAA (>= 2) from it. The targets SMAA needs
// (edges, blending weights, plus previous colour / depth / velocity and luma history for T2x) are created by the SMAA pass (main+0x16aa23c) on first use, so no
// rebuild call is needed. T2x needs a velocity target the console build never creates, so 3 (and the unused 4-5) crash inside the game: only 0-2 are ever written.
// The picture setting (dm_video.c dm_video_aa_mode) always answers 1 or 2 on the CLI host, the Mac App and iOS, so the host always writes the byte; only
// DM_AA_MODE=off leaves it to the game (or to a test's control-port mem write). DM_AA_MODE=0, 1 or 2 forces the mode; anything else is refused with a red line
// and the picture setting decides. The weak default below (-1: leave it) is for builds without dm_video.c (the t_oe unit test).
__attribute__((weak)) int dm_video_aa_mode(void) { return -1; }
enum { AA_ENV_UNSET = -1, AA_ENV_OFF = -3, AA_ENV_CRASH = -4, AA_ENV_BAD = -5 };
// DM_AA_MODE: unset or empty, exactly "off", or a single digit 0-2 (forced); 3-5 crash the game; anything else is malformed (no trimming, case-sensitive)
static int aa_env_parse(const char *e) {
    if (!e || !*e) return AA_ENV_UNSET;
    if (!strcmp(e, "off")) return AA_ENV_OFF;
    if (!e[1] && *e >= '0' && *e <= '2') return *e - '0';
    if (!e[1] && *e >= '3' && *e <= '5') return AA_ENV_CRASH;
    return AA_ENV_BAD;
}
static void aa_tick(void) {
    static int force = -2; static module_t *main;
    if (force == -2) { const char *e = getenv("DM_AA_MODE"); force = aa_env_parse(e);
        if (force == AA_ENV_CRASH) LOG("【红】DM_AA_MODE=%s refused: modes 3-5 (SMAA T2x) crash the game in its SMAA pass (main+0x16aa23c), which copies from a velocity target the console build never creates; the picture setting decides", e);
        else if (force == AA_ENV_BAD) LOG("【红】DM_AA_MODE=%s is not 0, 1, 2 or off: ignored, the picture setting decides", e);
        if (force == AA_ENV_CRASH || force == AA_ENV_BAD) force = AA_ENV_UNSET; }
    if (force == AA_ENV_OFF) return;   // DM_AA_MODE=off: leave the byte to the game (or to a test's control-port mem write)
    int want = force >= 0 ? force : dm_video_aa_mode(); if (want < 0 || want > 2) return;
    if (!main) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) main = &g_mod[i];
    if (!main || dm_nvn_frame_count() < 30) return;
    uintptr_t renderer = *(uintptr_t *)(main->base + 0x31e0848); if (!renderer) return;
    uint8_t *aa = (uint8_t *)(renderer + 0x6614); if (*aa == want) return;
    uint8_t was = *aa; dm_guest_write_prepare(aa, 1); *aa = (uint8_t)want;
    LOG("抗锯齿：游戏的档位 %u → %d（0 无 / 1 FXAA / 2 SMAA / 3 SMAA T2x，帧 %llu）", was, want, (unsigned long long)dm_nvn_frame_count());
}
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "系统通知队列；前端状态变化入队", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    scale3d_tick();
    aa_tick();
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    // 分辨率参数位在线程槽里，游戏在取通知的线程上读它们来改出图尺寸（收到 31「性能模式变化」时，main+0xe10ddc 起；30 只更新一个标志）。
    // 读的时机：处理函数每次进来、第一条通知取到之后读一次（0xe10d5c / 0xe10d60），后面循环里再取到的通知用的还是这次读的值。所以每取走一条都把此刻该给的值写进当前线程的槽——
    // 只在取走 31 时写不够：31 排在别的通知（如焦点 15）后面时，参数位在取 15 之后就读过了（iOS 包：前端没切成 16:9，Mac 宿主那局 31 正好排第一）。
    // The resolution parameter bits live in the thread slot, and the game reads them on the thread that fetches notifications to change the render size (on receiving 31 "performance mode changed", from main+0xe10ddc; 30 only updates a flag).
    // When they are read: each time the handler is entered, once after the first notification is fetched (0xe10d5c / 0xe10d60), and notifications fetched later in the loop still use the values from that read. So every time one is taken, the value due at this moment is written into the current thread's slot --
    // writing only when 31 is taken is not enough: when 31 is queued behind another notification (e.g. focus 15), the parameter bits have already been read after fetching 15 (iOS app: the front end did not switch to 16:9, in the Mac host run 31 happened to be queued first).
    if (ok) for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));
    if (ok && *msg == MSG_PERFMODE) dm_uiscreen_res_reread();
    if (ok) LOG("oe 通知被游戏取走：%u", *msg);   // 状态变化很少，逐条打不刷屏 | state changes are rare, printing each one does not flood the log
    if (ok && (*msg == MSG_OPMODE || *msg == MSG_PERFMODE)) dm_ui_gen_bump(*msg == MSG_OPMODE ? DM_UIGEN_OPMODE : DM_UIGEN_PERFMODE);   // 界面代号（dm_cursor.h） | UI generation (dm_cursor.h)
    return ok;
}
// bool 参数：Switch 调用方不扩展，声明成 unsigned 只看低 8 位
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "恢复通知开关；宿主的恢复以焦点通知 15 表达", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "运行模式变化通知开关", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "性能模式变化通知开关", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }，8 字节按值返回在 x0
// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "系统语言查询；按 DM_LANG 给值", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
    strncpy(code, e ? e : "zh-Hans", sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// nn::util::Uuid 16 字节：本机固定值（只用于本地区分设备，联网已剪）
// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "伪设备号；宿主固定值", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}
