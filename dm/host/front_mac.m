// Mac 前端：DM_WINDOW=1 时开窗口（CAMetalLayer，供图形后端出图）+ 键盘 → dm_input；客体改在大栈线程上跑，主线程归 AppKit。
// 不开 DM_WINDOW：客体直接在当前线程跑，行为与无前端时相同（无窗口测试用这个）。
// 键位（任天堂布局）：WASD 左摇杆、方向键十字键、J=A K=B U=X I=Y、Q=L E=R、Z=ZL C=ZR、F=按左摇杆 G=按右摇杆、回车=+ Tab=−。
// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−.
#import <Cocoa/Cocoa.h>
#include <sys/sysctl.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <ImageIO/ImageIO.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_uiscreen.h"
#include "dm_scrub.h"
#include <pwd.h>
#ifndef APP_NAME
#define APP_NAME "D2R Mobile"   // as app_mac.m: name of the log directory
#endif
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h));
static void ui_rect_frac(double *fy, double *fh) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; } }   // 同 front_ios.m：界面矩形的纵向位置与高（占画面高的比例） | Same as front_ios.m: vertical position and height of the UI rectangle (as fractions of the frame height)
__attribute__((constructor)) static void ui_shift_reg(void) { mtl_set_ui_rect_fn(ui_rect_frac); }   // 不开窗口的跑法（截图 / 闸门）也要生效，所以不放在建图层那里 | Must also take effect when running without a window (screenshots / gate checks), so it is not placed where the layer is created
#include "front.h"
#include "hle.h"
#include "dm_ctl.h"
#include "../ios/Sources/dm_vkbd_ui.h"
#include "../ios/Sources/dm_vkbd.h"
#include <signal.h>
#include <stdatomic.h>

void dm_touch_host_live_enable(void); void dm_touch_host_live(int op, float nx, float ny);   // touch_mac.c
static int g_live;   // App 形态（或 DM_TOUCH_LIVE=1）：鼠标进我们的触屏状态机；否则照旧当原版触摸屏的一个触点 | App form (or DM_TOUCH_LIVE=1): the mouse goes into our touch state machine; otherwise it stays a single touch point of the original touch screen as before
static CAMetalLayer *g_layer;
static NSWindow *g_win;
static NSView *g_view;
// 真全屏（App 形态，屏幕比 16:9 窄——16:10 的 MacBook，「不能全屏」：全屏后上下各 56 点黑边）：照 iPad 那套（front_ios.m、dm_uiscreen.c）——
// 启动时按屏幕比例（全屏时的内容区：屏幕扣掉刘海安全区）定游戏分辨率，局内 3D 铺满、界面按 16:9 贴底（开大面板居中），前端各页与读取画面游戏自己按 16:9 出、上下留边。
// 窗口锁成同一比例，窗口态与全屏态都铺满。屏幕不比 16:9 窄（外接 16:9 / 更宽的屏）：照旧全程 16:9。比例在启动时按主屏定，之后挪到别的屏不重算（游戏分辨率只在启动时定）。
// 测试开关 DM_TEST_SCREEN_ASPECT=<宽>x<高>：当作主屏是这个比例（16x9 = 外接 16:9 屏的口径）
// True full screen (App form, screen narrower than 16:9 - a 16:10 MacBook, "cannot go full screen": 56 points of black bar above and below in full screen): the iPad approach (front_ios.m, dm_uiscreen.c) -
// at launch the game resolution is set from the screen's aspect ratio (the full-screen content area: the screen minus the notch safe area); in game the 3D fills the screen, the interface is 16:9 along the bottom (centred with a large panel open), and the front-end pages and the loading screen are drawn by the game at 16:9 with bars above and below.
// The window is locked to the same ratio, so both windowed and full screen are filled. Screens not narrower than 16:9 (external 16:9 / wider screens): 16:9 throughout as before. The ratio is set from the main screen at launch and not recomputed when moved to another screen (the game resolution is only set at launch).
// Test switch DM_TEST_SCREEN_ASPECT=<w>x<h>: treat the main screen as this ratio (16x9 = the external 16:9 screen case)
static double g_aspect = 16.0 / 9;   // 游戏画面比例（宽 ÷ 高） | aspect ratio of the game picture (width / height)
static int g_full;                    // 1 = 真全屏已开 | 1 = true full screen is on
static void full_setup(void) {
    if (!getenv("DM_APP") || getenv("DM_GAME_RES")) return;
    NSScreen *sc = NSScreen.mainScreen; double w = sc.frame.size.width, h = sc.frame.size.height - sc.safeAreaInsets.top; const char *e = getenv("DM_TEST_SCREEN_ASPECT"); unsigned tw, th;
    if (e && sscanf(e, "%ux%u", &tw, &th) == 2 && tw && th) { w = tw; h = th; }
    if (h <= 0 || w / h >= 16.0 / 9 - 0.01) { LOG("画面：主屏 %.0fx%.0f 不比 16:9 窄 → 全程 16:9", w, h); return; }
    g_aspect = w / h; g_full = 1; dm_uiscreen_enable(DM_UISCREEN_BOTTOM);
    LOG("画面：主屏全屏内容区 %.0fx%.0f（比例 %.3f）比 16:9 窄 → 真全屏：窗口锁这个比例，局内铺满、界面贴底，前端 16:9 留边", w, h, g_aspect);
}
// 出图分辨率（主机模式的游戏出图宽，高按画面比例）：原版 1600。App 里按选项页「显示 → 分辨率」那一档（dm_video.c 的 res：1600 / 1920 / 2560），
// 只在启动时定（游戏分辨率启动后不变）；换屏、拉窗口后由出图（MetalFX / 双线性）跟着视口缩放。DM_GAME_RES（直接给分辨率，对拍用）在时不理它
// Output resolution (the docked game output width; height from the picture's aspect ratio): 1600 originally. In the App it follows the options page "Display → Resolution" tier (res in dm_video.c: 1600 / 1920 / 2560),
// set only at launch (the game resolution does not change after launch); after a screen change or window resize the present (MetalFX / bilinear) scales to the viewport. Ignored with DM_GAME_RES (which gives the resolution directly, for comparisons)
// 第四档「原生」（res=3）= 主屏的像素宽（16 对齐，1600–3840）：界面与文字按屏幕原生分辨率画，3D 由「3D 清晰度」单独降（dm_video.c s3d）
// The fourth tier "Native" (res=3) = the main screen's pixel width (16-aligned, 1600–3840): the interface and text are drawn at the screen's own resolution, and "3D Resolution" lowers the 3D on its own (s3d in dm_video.c)
static const int k_render_w[3] = { 1600, 1920, 2560 };
static int g_render_w = 1600, g_render_tier = -1;   // 这次启动用的那一档（-1 = 没按设置定） | the tier used for this launch (-1 = not set from the settings)
int dm_video_get(const char *key);
void dm_video_fullscreen_note(int on);
static double main_px(void) { NSScreen *sc = NSScreen.mainScreen; return sc ? sc.frame.size.width * sc.backingScaleFactor : 1600; }   // 主屏的像素宽 | the main screen's width in pixels
static int native_w(void) { int w = (int)main_px() / 16 * 16; return w < 1600 ? 1600 : w > 3840 ? 3840 : w; }
static void render_setup(void) {
    if (!getenv("DM_APP") || getenv("DM_GAME_RES")) return;
    int t = dm_video_get("res"); if (t < 0 || t > 3) t = 0;
    g_render_tier = t; g_render_w = t == 3 ? native_w() : k_render_w[t];
    LOG("画面：出图宽 %d（选项页「分辨率」第 %d 档） | render width %d (Resolution tier %d)", g_render_w, t, g_render_w, t);
}
int front_video_running_res(void) { return g_render_tier; }
int front_video_mfx_ok(void) { static int ok = -1; if (ok < 0) ok = [MTLFXSpatialScalerDescriptor supportsDevice:MTLCreateSystemDefaultDevice()] ? 1 : 0; return ok; }
// 显卡档（按名字）：0 基础款、1 Pro、2 Max / Ultra | GPU class (by name): 0 base, 1 Pro, 2 Max / Ultra
static int gpu_class(void) { NSString *g = MTLCreateSystemDefaultDevice().name ?: @""; return [g containsString:@" Max"] || [g containsString:@" Ultra"] ? 2 : [g containsString:@" Pro"] ? 1 : 0; }
// 推荐的原生出图与 3D 清晰度：原生宽不超过这档显卡的上限（2560 / 3456 / 3840）才推荐原生；3D 取「原生宽 × 比例」不超过 1920 / 2560 / 3840 的最高一档。
// M6 实测（town_loop，2560 屏）见 docs/MAC.md。原生不合适时 *res = -1（照旧按 1600 / 1920 / 2560 三档挑）
// Recommended native output and 3D resolution: Native only when the native width is within this GPU class's limit (2560 / 3456 / 3840); 3D takes the highest tier whose "native width × scale" is within
// 1920 / 2560 / 3840. Measurements on an M6 (town_loop, 2560 screen) are in docs/MAC.md. When Native does not fit, *res = -1 (pick from the 1600 / 1920 / 2560 tiers as before)
static void rec_native(int *res, int *s3d) {
    static const int lim_out[3] = { 2560, 3456, 3840 }, lim_3d[3] = { 1920, 2560, 3840 }; static const float sc[5] = { 1.0f, 0.85f, 0.75f, 0.67f, 0.5f };
    int c = gpu_class(), w = native_w(); *res = -1; *s3d = 0;
    if (!front_video_mfx_ok() || w > lim_out[c]) return;
    *res = 3; *s3d = 4; for (int i = 0; i < 5; i++) if (w * sc[i] <= lim_3d[c] + 0.5f) { *s3d = i; break; }
}
// 首次启动的推荐值（dm_video.c 读到的 sw_video.txt 缺键或越界时调，只取缺的那几个，启动时在主线程）：全屏；分辨率取「不超过主屏像素宽的 3/4」的最高一档，2560 那档只给 Pro / Max / Ultra
// 显卡（名字里带；M6 实测 1920 出图 + MetalFX 每帧约 13.7 ms，2560 约 14.5 ms，60 帧只有 16.7 ms）；出图比画布小又支持 MetalFX 就开 MetalFX。
// 旧版偏好（StartFullScreen / RenderScale / MetalFXUpscale）设过的当作当前值接着用。测试副本（DM_APP_SUPPORT）缺省全按原版（窗口、1600、标准），
// 自动跑的局才稳定；DM_TEST_VIDEO_FIRSTBOOT=1 让测试也走真推荐
// Recommended values on first boot (called by dm_video.c when sw_video.txt lacks a key or has one out of range, only the missing ones are used, on the main thread at launch): full screen; resolution = the highest tier not wider than 3/4 of the main screen's pixel width, the 2560 tier only for
// Pro / Max / Ultra GPUs (going by the name; on an M6 a 1920 render + MetalFX took about 13.7 ms per frame and 2560 about 14.5 ms, against 16.7 ms at 60 fps); MetalFX on when the render is smaller than the canvas and MetalFX is supported.
// Values from the older preferences (StartFullScreen / RenderScale / MetalFXUpscale) carry over as the current values when set. The test copy (DM_APP_SUPPORT) defaults to the original throughout (windowed, 1600, standard)
// so automated runs stay stable; DM_TEST_VIDEO_FIRSTBOOT=1 makes a test take the real recommendation
int front_video_firstboot(int rec[3], int cur[3]) {
    if (getenv("DM_APP_SUPPORT") && !getenv("DM_TEST_VIDEO_FIRSTBOOT")) { for (int i = 0; i < 3; i++) rec[i] = cur[i] = 0; LOG("画面设置：测试副本，推荐值按原版（窗口 / 1600 / 标准）"); return 1; }
    double px = main_px(); NSString *gpu = MTLCreateSystemDefaultDevice().name ?: @"";
    int big = gpu_class() > 0, t = 0, nres, ns3d; rec_native(&nres, &ns3d);
    for (int i = 0; i < 3; i++) if (k_render_w[i] <= fmax(px * 0.75, 1600) && (i < 2 || big)) t = i;
    rec[0] = 1; rec[1] = nres >= 0 ? nres : t; rec[2] = front_video_mfx_ok() && (nres >= 0 || k_render_w[t] < px) ? 1 : 0;   // 原生时 MetalFX 管 3D 放大趟 | with Native, MetalFX does the 3D upscale pass
    for (int i = 0; i < 3; i++) cur[i] = rec[i];
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults; int legacy = 0;
    if ([ud objectForKey:@"StartFullScreen"]) { cur[0] = [ud boolForKey:@"StartFullScreen"]; legacy = 1; }
    id rs = [ud objectForKey:@"RenderScale"]; double k = [rs respondsToSelector:@selector(doubleValue)] ? [rs doubleValue] : 0;   // 手写的设置可能是任何类型 | a hand-written default can be any type
    if (k > 0 && isfinite(k)) { double w = fmin(fmax(px * fmin(k, 1.0), 1600), 2560); cur[1] = 0; for (int i = 0; i < 3; i++) if (k_render_w[i] <= w) cur[1] = i; legacy = 1; }
    if ([ud objectForKey:@"MetalFXUpscale"]) { cur[2] = [ud boolForKey:@"MetalFXUpscale"] && front_video_mfx_ok(); legacy = 1; }
    LOG("画面设置：首次启动 主屏 %.0f 像素宽、显卡 %s → 推荐 全屏 / %d%s / %s%s", px, gpu.UTF8String, nres >= 0 ? native_w() : k_render_w[t], nres >= 0 ? "（原生）" : "", rec[2] ? "MetalFX" : "标准", legacy ? "；旧版偏好设过的接着用" : "");
    return 1;
}
// Recommendations for the picture keys added later (asked by dm_video.c when the file lacks the key or holds an older recommendation version): SMAA, 16x
// anisotropy, accurate colour and environment shadows (+0.5 ms GPU in town at Recommended on an M6, still a steady 60 fps) in the Mac App. The CLI host
// (no DM_APP) and test copies keep the original (0), so automated runs, golden baselines and benchmarks stay as they were
int front_video_rec(const char *key) {
    if (!getenv("DM_APP") || (getenv("DM_APP_SUPPORT") && !getenv("DM_TEST_VIDEO_FIRSTBOOT"))) return 0;
    if (!strcmp(key, "aa")) return 1;
    if (!strcmp(key, "aniso")) return 3;
    if (!strcmp(key, "color")) return 1;
    if (!strcmp(key, "envsh")) return 1;
    if (!strcmp(key, "s3d")) { int r, s3; rec_native(&r, &s3); return s3; }
    return -1;
}
// 「色彩」：准确 = 层标 sRGB（游戏写的就是 sRGB 编码的字节），系统把它配到屏幕的色域；鲜艳 = 不标（原先的样子：宽色域屏把 sRGB 原色当屏幕原色，偏艳）。
// 画面字节不变（截图、比对照旧），只改合成器怎么解释。DM_COLORSPACE=srgb|none 定死（测试、对照）
// "Colour": Accurate = the layer is tagged sRGB (the game writes sRGB-encoded bytes) and the system maps it to the panel's gamut; Vivid = untagged (the old look: wide-gamut panels take the sRGB primaries as their own,
// more saturated). The picture bytes are unchanged (captures and comparisons as before); only how the compositor reads them changes. DM_COLORSPACE=srgb|none fixes it (tests, comparisons)
static void layer_color(int accurate) {   // 主线程 | main thread
    const char *e = getenv("DM_COLORSPACE"); if (e && *e) accurate = !strcmp(e, "srgb");
    if (!g_layer) return;
    CGColorSpaceRef cs = accurate ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB) : NULL;
    g_layer.colorspace = cs; if (cs) CGColorSpaceRelease(cs);
    LOG("画面：色彩%s（层的色彩空间 %s） | picture: colour %s (layer colour space %s)", accurate ? "准确" : "鲜艳", accurate ? "sRGB" : "不标", accurate ? "Accurate" : "Vivid", accurate ? "sRGB" : "untagged");
}
void front_video_color(int accurate) { dispatch_async(dispatch_get_main_queue(), ^{ layer_color(accurate); }); }   // 选项页点了应用（游戏线程） | Apply on the options page (game thread)
void front_video_fullscreen(int on) {   // 选项页「显示模式」点了应用：当场切（游戏线程调，切到主线程做） | "Display Mode" applied on the options page: switch at once (called on the game thread, done on the main thread)
    dispatch_async(dispatch_get_main_queue(), ^{ NSWindow *w = g_win; if (w && ((w.styleMask & NSWindowStyleMaskFullScreen) != 0) != (on != 0)) [w toggleFullScreen:nil]; });
}
// 游戏画面在视图里的位置：按游戏画面比例放进视图（比例对得上就是整个视图；对不上的四周留黑边）
// Where the game picture sits in the view: fitted into the view by the picture's aspect ratio (if the ratios match, the whole view; otherwise black bars around)
// 真全屏下画面归一化坐标 → 界面矩形归一化坐标（鼠标点击与滚轮共用，两步见鼠标那段的说明） | true full screen: picture-normalised → interface-rectangle-normalised coordinates (shared by mouse clicks and the wheel; the two steps are described at the mouse handler)
static void full_ui_point(NSRect gr, float nx, float ny, float *vx, float *uy) {
    int rw = 0, rh = 0; double cx = 0, cy = 0, cw = 1, ch = 1;
    if (dm_screen_real(&rw, &rh) && rw > 0 && rh > 0) { double ca = (double)rw / rh, ga = gr.size.width / gr.size.height;
        if (fabs(ga / ca - 1) >= 0.01) { if (ga > ca) { cw = ca / ga; cx = (1 - cw) / 2; } else { ch = ga / ca; cy = (1 - ch) / 2; } } }
    *vx = (float)((nx - cx) / cw); float vy = (float)((ny - cy) / ch);
    double fy = 0, fh = 1; ui_rect_frac(&fy, &fh); *uy = fh > 0 ? (float)((vy - fy) / fh) : vy;
}
static NSRect game_rect(void) {
    NSSize b = g_view.bounds.size; CGFloat w = b.width, h = b.height;
    if (fabs(w / h / g_aspect - 1) < 0.003) { }   // 比例差 0.3% 以内（窗口高取整的零头、全屏内容区比按刘海安全区算的少 1 点）当对得上，铺满 | within 0.3% (the remainder from rounding the window height, and the full-screen content area being 1 point less than computed from the notch safe area) counts as matching, and it fills
    else if (w > h * g_aspect) w = round(h * g_aspect); else h = round(w / g_aspect);
    return NSMakeRect(floor((b.width - w) / 2), floor((b.height - h) / 2), w, h);
}
static void vkbd_where(dm_vkbd_host *h) {   // 手柄屏幕键盘画在视图层上、游戏画面里（视图层坐标向上增） | the on-screen controller keyboard is drawn on the view layer, inside the game picture (view layer coordinates grow upward)
    h->layer = (__bridge void *)g_view.layer; h->pic = NSRectToCGRect(game_rect()); h->y_up = 1; h->scale = g_win.backingScaleFactor > 0 ? g_win.backingScaleFactor : 2;
}
static void layout_game(void) {
    NSRect r = game_rect(); if (NSEqualRects(r, g_layer.frame)) return;
    CGFloat sc = g_view.window.backingScaleFactor ?: 2;   // 画布跟着改：作为子层的 CAMetalLayer 改 frame 不会自己改 drawableSize，原先进全屏后一直用窗口态的画布放大显示（发糊） | the canvas follows: a CAMetalLayer used as a sublayer does not change its drawableSize when its frame changes, so after going full screen it used to keep the windowed canvas and scale it up (blurry)
    [CATransaction begin]; [CATransaction setDisableActions:YES]; g_layer.frame = r; g_layer.contentsScale = sc; g_layer.drawableSize = CGSizeMake(round(r.size.width * sc), round(r.size.height * sc)); [CATransaction commit];
    dm_vkbd_ui_relayout();   // 屏幕键盘跟着画面走 | the on-screen keyboard follows the picture
    LOG("窗口：内容区 %.0f×%.0f，游戏画面 %.0f×%.0f 放在 (%.0f,%.0f)", g_view.bounds.size.width, g_view.bounds.size.height, r.size.width, r.size.height, r.origin.x, r.origin.y);
}
// 退出（关窗口 / ⌘Q / 菜单）：有局在就先请游戏走一次它自己的存盘并等存档提交（做法同 iOS 壳进后台），最多 3 秒，再有序收尾。
// 暂停菜单这类开着时游戏停拍：冻住世界、临时放开暂停存这一次（diag.c dm_save_wait，与 iOS 进后台同一个函数）；读取画面里等它进到局内再请。3 秒到了还没有提交就照实记「未存盘退出」
// Quit (close window / Cmd-Q / menu): if a game is running, first ask the game to do one of its own saves and wait for the save commit (same as the iOS shell going to the background), at most 3 s, then shut down in order.
// When the pause menu or the like is open the game does not tick: freeze the world and temporarily release the pause for this one save (diag.c dm_save_wait, the same function as iOS going to the background); on the loading screen wait until in game before asking. If there is still no commit after 3 s, log "quit without saving" as is
// 等的那几步（iCloud 启动同步最多 20 秒、存盘最多 3.5 秒、iCloud 上传最多 10 秒、收尾最多 1.5 秒）不在主线程上：原先整段堵着主线程，
// 慢的时候窗口卡住、出彩球，系统也当它没响应。现在主线程只做非它不可的几步（关输入框、读窗口状态、最小化着就先恢复），其余交给后台线程，结束时 _exit。
// 存盘的保证不变：游戏自己的存盘本来就在游戏线程上做、出图走显示节拍线程，都不靠主线程；退出一开始就记下 g_quitting，之后失焦不再告诉游戏（原先主线程堵着，失焦也到不了游戏），
// 系统来的退出答「稍后」（applicationShouldTerminate），注销 / 关机等这个进程自己结束，不会被取消
// The waiting steps (the iCloud boot sync up to 20 s, the save up to 3.5 s, the iCloud upload up to 10 s, the wrap-up up to 1.5 s) are off the main thread: they used to block it throughout,
// so a slow quit froze the window with a spinning cursor and the system took the app as not responding. Now the main thread does only what needs it (close an input box, read the window state, resume first when minimized), the rest goes to a background thread that ends with _exit.
// The save guarantee is unchanged: the game's own save runs on the game thread and frames go through the display-link thread, neither needs the main thread; g_quitting is set when the quit starts, after which a focus loss is no longer passed to the game (with the main thread blocked it never reached the game either),
// and a quit from the system is answered "later" (applicationShouldTerminate), so log out / shut down wait for this process to end by itself instead of being cancelled
static _Atomic int g_quitting;
static void app_quit_finish(const char *why, int ingame) {   // 后台线程 | background thread
    // iCloud 启动同步还在做（窗口先出来了，游戏还在等它，见 main.c cloud_boot_start）：它在改存档目录，做完再退，最多 20 秒
    // The iCloud boot sync is still running (the window came up first and the game is waiting for it, see main.c cloud_boot_start): it is changing the save directory, so let it finish before quitting, at most 20 s
    { int dm_cloud_boot_wait(double max_s); dm_cloud_boot_wait(20); }
    int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms), dm_cloud_flush_wait(double sec);
    if (!ingame) { LOG("窗口：退出（不在局内，没有待存的进度）"); dm_cloud_flush_wait(10); dm_host_shutdown(why); return; }   // 刚「保存并退出」就退：那一局的 iCloud 上传可能还在排队，等它传完 | Quitting right after "Save and Exit": that run's iCloud upload may still be queued, wait for it
    uint64_t n = 0; int froze = 0; double ms = 0; int asked = dm_save_wait(3, &n, &froze, &ms);
    if (n && asked) LOG("窗口：退出前等游戏存盘 %.0f ms，期间存档提交 %llu 次%s", ms, (unsigned long long)n, froze ? "（原先暂停着：冻结世界存盘、已恢复暂停）" : "");
    else LOG("【红】窗口：未存盘退出——3 秒内没等到存档提交（%s%s）", asked ? "已请游戏存盘" : "一直没到能请存盘的状态（读取画面？）", froze ? "，原先暂停着、冻结放开过" : "");
    DM_EV("quit_save", "saved:b commits:u ms:f paused:b", n && asked, (unsigned long long)n, ms, froze);
    dm_cloud_flush_wait(10);   // 存盘之后把这一局传上 iCloud 再退（没开同步时立即返回） | after saving, upload this session to iCloud before quitting (returns immediately when sync is off)
    dm_host_shutdown(why);
}
static int text_input_cancel(void), text_input_up(void);
// 控制口 fe 命令（dm_ctl_cmds.c）：把一行测试输入交给上面 DM_TEST_NSMOUSE 的行解释（窗口建好后才有）；任意线程调 | the control port's fe command (dm_ctl_cmds.c): hands one test-input line to the DM_TEST_NSMOUSE line interpreter (exists once the window is built); callable from any thread
static int (^g_fe_line)(NSString *);
static int fe_line_c(const char *line) { int (^f)(NSString *) = g_fe_line; return f ? f(@(line)) : -1; }
static void text_input_handoff_pad(void), text_input_test(NSString *text, int ok);
static void app_quit(const char *why) {   // 主线程 | main thread
    static int once; if (once++) return;
    atomic_store(&g_quitting, 1);
    { uint64_t dm_audio_render_calls(void); LOG("窗口：退出（%s）；声卡回调累计 %llu（0 = 这次没出声）", why, (unsigned long long)dm_audio_render_calls()); }
    DM_EV("quit", "why:s", why);
    // 数量 / 起名输入框（或手柄屏幕键盘）开着：先按「取消」关掉，放走等它的键盘线程、解冻客体时钟，下面才存得了盘（见 front_text_input）
    // A quantity / name input box (or the on-screen controller keyboard) is open: answer "Cancel" first to release the keyboard thread waiting on it and unfreeze the guest clock, so the save below can happen (see front_text_input)
    if (text_input_cancel() || dm_vkbd_ui_cancel()) LOG("窗口：退出时输入框开着 → 先按取消关掉，再存盘");
    int dm_ingame_now(void), ingame = dm_ingame_now() || dm_uiscreen_ingame();
    // 最小化着退出：游戏线程挡在图形挂起处（dm_gfx_suspend）、客体时钟冻着，拍不走就存不了盘。先恢复（出图照丢，不画到看不见的窗口），再请存盘
    // Quitting while minimized: the game thread is blocked at the graphics suspend (dm_gfx_suspend) and the guest clock is frozen, so no tick runs and nothing can be saved. Resume first (frames are still dropped, nothing is drawn to the hidden window), then ask for the save
    if (ingame && g_win.miniaturized) { void mtl_present_discard(int on); void guest_clock_freeze_for(int reason, int on); mtl_present_discard(1); dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); LOG("窗口：最小化着退出，先恢复图形与客体时钟再存盘"); }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ app_quit_finish(why, ingame); });
}
// 控制口 app 命令（dm_ctl_cmds.c）：在主线程上做与 DM_TEST_MINI_AT / DM_TEST_TERMINATE_AT 相同的动作。最小化与 DM_TEST_MINI_AT 一样临时给窗口加上可最小化（游戏窗口本身没有）；
// background / foreground 在 Mac 上就是最小化 / 还原；resign / active 不支持（Mac 的失焦靠别的窗口抢走焦点）
// The control port's app command (dm_ctl_cmds.c): on the main thread, the same actions as DM_TEST_MINI_AT / DM_TEST_TERMINATE_AT. Like DM_TEST_MINI_AT, minimizing temporarily makes the window minimizable (the game window is not by itself);
// background / foreground on the Mac are minimize / restore; resign / active are not supported (on the Mac, focus is lost by another window taking it)
// 测试用：给自己发一次系统的「退出」Apple 事件（同程序坞「退出」走的那条，见 quitEvent:reply:） | for tests: send ourselves the system "quit" Apple event (the same path as Quit in the Dock, see quitEvent:reply:)
static void send_quit_event(void) {
    NSAppleEventDescriptor *ev = [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass eventID:kAEQuitApplication targetDescriptor:[NSAppleEventDescriptor currentProcessDescriptor]
                                                                         returnID:kAutoGenerateReturnID transactionID:kAnyTransactionID];
    NSError *err = nil; if (![ev sendEventWithOptions:NSAppleEventSendNoReply timeout:5 error:&err]) LOG("【红】测试退出：退出 Apple 事件发不出去：%s", err.localizedDescription.UTF8String);
}
static int ctl_app(const char *w) {
    if (!g_win) return -1;
    int mini = !strcmp(w, "minimize") || !strcmp(w, "background"), rest = !strcmp(w, "restore") || !strcmp(w, "foreground");
    if (!strcmp(w, "terminate")) { dispatch_async(dispatch_get_main_queue(), ^{ send_quit_event(); }); return 0; }   // 同程序坞「退出」：退出 Apple 事件 → app_quit（存盘再退），不返回 | as Quit in the Dock: the quit Apple event -> app_quit (save, then quit), does not return
    if (!mini && !rest) return -1;
    dispatch_sync(dispatch_get_main_queue(), ^{ if (mini) { g_win.styleMask |= NSWindowStyleMaskMiniaturizable; [g_win miniaturize:nil]; } else [g_win deminiaturize:nil]; });
    return 0;
}
static const char *ui(const char *k) { const char *dm_ui(const char *key); return dm_ui(k); }
static void show_keys(void) { NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmAppKeysTitle")); a.informativeText = @(ui("dmAppKeysText")); [a addButtonWithTitle:@(ui("dmAppOK"))]; [a runModal]; }
// 分享日志的脱敏规则（dm_scrub.c，同 iOS 的 dm_sharelog.m）：家目录 → ~、显示器型号 → <display>、挂着的盘名、登录名 / 全名 / 主机名、
// 不是本项目自己的包名（org.d2rmobile.*）时的包名，以及存档里的角色名 → <char1> …；路径里的 UUID、/var/folders 与 .d2s 文件名按形状换（见 dm_scrub.c）
// Scrubbing rules of the shared log (dm_scrub.c, as iOS's dm_sharelog.m): home directory -> ~, display models -> <display>, mounted disk names, login / full / host name,
// the bundle ID when it is not the project's own (org.d2rmobile.*), and the character names in the save -> <char1> ...; UUIDs in paths, /var/folders and .d2s file names go by shape (see dm_scrub.c)
static dm_scrub *share_scrubber(NSString *home) {
    dm_scrub *s = dm_scrub_new(); if (!s) return NULL;
    struct passwd *pw = getpwuid(getuid());
    dm_scrub_add(s, home.fileSystemRepresentation, "~", 1); if (pw && pw->pw_dir) dm_scrub_add(s, pw->pw_dir, "~", 1);
    { const char *sv = getenv("DM_SAVE"); if (sv) dm_scrub_add_chars(s, sv); }
    for (NSScreen *sc in NSScreen.screens) { CGDirectDisplayID id_ = [sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue]; NSString *nm = sc.localizedName;
        if (nm.length >= 2) dm_scrub_add(s, nm.UTF8String, CGDisplayIsBuiltin(id_) ? "<built-in display>" : "<display>", 0); }
    for (NSURL *u in [NSFileManager.defaultManager mountedVolumeURLsIncludingResourceValuesForKeys:nil options:0] ?: @[]) if ([u.path hasPrefix:@"/Volumes/"]) dm_scrub_add(s, u.path.fileSystemRepresentation, "/Volumes/<volume>", 0);
    NSString *bid = NSBundle.mainBundle.bundleIdentifier;
    if (bid.length && ![bid hasPrefix:@"org.d2rmobile."]) { dm_scrub_add(s, bid.UTF8String, "<bundle-id>", 1); dm_scrub_add(s, [bid stringByReplacingOccurrencesOfString:@"." withString:@"~"].UTF8String, "<bundle-id>", 1); }
    if (pw && pw->pw_name && strlen(pw->pw_name) >= 3) dm_scrub_add(s, pw->pw_name, "<user>", 1);
    NSString *full = NSFullUserName(); if (full.length >= 3) dm_scrub_add(s, full.UTF8String, "<user>", 1);
    char hn[256] = ""; if (!gethostname(hn, sizeof hn - 1) && strlen(hn) >= 3) {   // 主机名（Mac-mini-de-Patrice.local）与它去掉 .local、连字符换空格的样子（多半就是电脑名） | host name (Mac-mini-de-Patrice.local) and its form without .local and with spaces for hyphens (usually the computer name)
        dm_scrub_add(s, hn, "<computer>", 1); char *dot = strstr(hn, ".local"); if (dot) *dot = 0; dm_scrub_add(s, hn, "<computer>", 1);
        for (char *p = hn; *p; p++) if (*p == '-') *p = ' '; dm_scrub_add(s, hn, "<computer>", 1); }
    return s;
}
// 帮助 → 分享日志…：把本次与上一次的日志（~/Library/Logs/D2R Mobile/host.log、host.prev.log）和一份版本信息打成一个压缩包，弹系统的存储面板让玩家选位置（缺省桌面），存好后在访达里选中它。
// 不上传、不发邮件；包里只有这三份文件——不放存档、不放 iCloud 目录里的任何东西。三份都先脱敏（share_scrubber：家目录、角色名、显示器、盘名……），洗不了的那份不放。
// to 非空 = 测试（DM_TEST_SHARELOG）：不弹面板，直接写到这个路径。返回 nil 成功，否则是失败原因
// Help -> Share Log...: pack the logs of this run and the previous one (~/Library/Logs/D2R Mobile/host.log, host.prev.log) and a version-information file into one archive, show the system save panel for the player to choose a location (Desktop by default), and select it in Finder once saved.
// Nothing is uploaded or mailed; the archive contains only these three files - no saves, nothing from the iCloud directory. All three are scrubbed first (share_scrubber: home directory, character names, displays, disk names...); a file that cannot be scrubbed is left out.
// to non-empty = test (DM_TEST_SHARELOG): no panel, write directly to this path. Returns nil on success, otherwise the reason for failure
static NSString *share_logs(NSString *to) {
    // The log directory is where app_mac.m writes: <support>/Logs for a test copy (DM_APP_SUPPORT), Logs/APP_NAME otherwise. It used to be hard-coded to Logs/D2R Mobile, so a test copy packed the player's logs
    NSFileManager *fm = NSFileManager.defaultManager; NSString *home = NSHomeDirectory(), *logd = getenv("DM_APP_SUPPORT") ? [@(getenv("DM_APP_SUPPORT")) stringByAppendingPathComponent:@"Logs"] : [home stringByAppendingPathComponent:@"Library/Logs/" APP_NAME];
    NSDictionary *inf = NSBundle.mainBundle.infoDictionary; NSString *build = [inf[@"CFBundleVersion"] description] ?: @"0", *ver = [inf[@"CFBundleShortVersionString"] description] ?: @"?";
    NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyyMMdd-HHmm"; NSString *name = [NSString stringWithFormat:@"D2RMobile-logs-%@-%@", build, [df stringFromDate:NSDate.date]];
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"dm_share_%d", getpid()]], *dir = [tmp stringByAppendingPathComponent:name];
    [fm removeItemAtPath:tmp error:nil]; if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return @"mkdir";
    fflush(stderr);
    dm_scrub *sc = share_scrubber(home); if (!sc) { [fm removeItemAtPath:tmp error:nil]; return @"scrub"; }
    NSData *(^scrub)(NSData *) = ^NSData *(NSData *d) { size_t on = 0; char *o = d ? dm_scrub_run(sc, d.bytes, d.length, &on) : NULL; return o ? [NSData dataWithBytesNoCopy:o length:on freeWhenDone:YES] : nil; };   // 洗不了（内存不够）= nil：那份不放 | cannot scrub (out of memory) = nil: that file is left out
    int n = 0; for (NSString *f in @[@"host.log", @"host.prev.log"]) { NSData *d = [NSData dataWithContentsOfFile:[logd stringByAppendingPathComponent:f] options:NSDataReadingMappedIfSafe error:nil]; if (d && [scrub(d) writeToFile:[dir stringByAppendingPathComponent:f] atomically:YES]) n++; }
    NSString *bt = [NSString stringWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"build.txt"] encoding:NSUTF8StringEncoding error:nil] ?: @"";
    char hw[64] = "?"; size_t hn = sizeof hw; sysctlbyname("hw.model", hw, &hn, NULL, 0); NSProcessInfo *pi = NSProcessInfo.processInfo;
    NSString *info = [NSString stringWithFormat:@"%@\nD2R Metal (macOS) %@ (%@)\n%@\n%@\nmacOS %@\n%@  %.0f GB\n%@\n", @(ui("dmAppShareInfo")), ver, build, inf[@"CFBundleIdentifier"] ?: @"", [bt stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet],
        pi.operatingSystemVersionString, @(hw), pi.physicalMemory / 1073741824.0, [NSLocale.preferredLanguages componentsJoinedByString:@","]];
    [scrub([info dataUsingEncoding:NSUTF8StringEncoding]) writeToFile:[dir stringByAppendingPathComponent:@"info.txt"] atomically:YES]; dm_scrub_free(sc);
    NSString *zip = [tmp stringByAppendingPathComponent:[name stringByAppendingString:@".zip"]];
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/ditto"]; t.arguments = @[@"-c", @"-k", @"--keepParent", dir, zip]; NSError *err = nil;
    if (![t launchAndReturnError:&err]) { [fm removeItemAtPath:tmp error:nil]; return err.localizedDescription ?: @"ditto"; } [t waitUntilExit];
    if (t.terminationStatus != 0 || ![fm fileExistsAtPath:zip]) { [fm removeItemAtPath:tmp error:nil]; return [NSString stringWithFormat:@"ditto %d", t.terminationStatus]; }
    NSURL *dst = to ? [NSURL fileURLWithPath:to] : nil;
    if (!dst) { NSSavePanel *sp = [NSSavePanel savePanel]; sp.nameFieldStringValue = zip.lastPathComponent; sp.directoryURL = [fm URLsForDirectory:NSDesktopDirectory inDomains:NSUserDomainMask].firstObject; sp.canCreateDirectories = YES;
        if ([sp runModal] != NSModalResponseOK) { [fm removeItemAtPath:tmp error:nil]; LOG("分享日志：玩家取消"); return nil; } dst = sp.URL; }
    [fm removeItemAtURL:dst error:nil]; BOOL ok = [fm moveItemAtPath:zip toPath:dst.path error:&err]; [fm removeItemAtPath:tmp error:nil];
    if (!ok) return err.localizedDescription ?: @"move";
    LOG("分享日志：已存（日志 %d 份 + 版本信息，%llu 字节）", n, [[fm attributesOfItemAtPath:dst.path error:nil] fileSize]);
    if (!to) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[dst]];
    return nil;
}
static void share_logs_ui(void) { NSString *e = share_logs(nil); if (e) { LOG("【红】分享日志：打包失败 %s", e.UTF8String); NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmAppShareFail")); a.informativeText = e; [a addButtonWithTitle:@(ui("dmAppOK"))]; [a runModal]; } }
@interface DmWinCtl : NSObject <NSWindowDelegate, NSApplicationDelegate> @end
// 窗口所在那块屏的名字与刷新率（主线程读 AppKit，存一份给出图线程）：出图节拍按它选整除的帧率（nvn_mtl.m mtl_pace_hz）。
// 窗口换屏、屏幕参数变（改刷新率、接拔显示器）时重读。刷新率取显示模式的值，内建屏读不到（0）时取屏幕的最高帧率
// Name and refresh rate of the screen the window is on (read from AppKit on the main thread, a copy kept for the present thread): present pacing picks an evenly dividing frame rate from it (nvn_mtl.m mtl_pace_hz).
// Re-read when the window moves to another screen or screen parameters change (refresh rate change, display plugged / unplugged). The refresh rate comes from the display mode; when the built-in screen gives none (0), the screen's maximum frame rate is used
static pthread_mutex_t g_scr_mu = PTHREAD_MUTEX_INITIALIZER; static double g_scr_hz; static char g_scr_name[128];
static void screen_note(void) {
    NSScreen *sc = g_win.screen ?: NSScreen.mainScreen; if (!sc) return;
    CGDirectDisplayID id_ = [sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(id_); double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    if (hz <= 0) hz = (double)sc.maximumFramesPerSecond;
    pthread_mutex_lock(&g_scr_mu); int same = hz == g_scr_hz && !strcmp(g_scr_name, sc.localizedName.UTF8String ?: "");
    g_scr_hz = hz; snprintf(g_scr_name, sizeof g_scr_name, "%s", sc.localizedName.UTF8String ?: "?"); pthread_mutex_unlock(&g_scr_mu);
    if (!same) LOG("屏幕：%s，刷新率 %.2f Hz（最高 %ld 帧）", g_scr_name, hz, (long)sc.maximumFramesPerSecond);
}
double front_display_refresh(char *name, size_t n) {
    pthread_mutex_lock(&g_scr_mu); double hz = g_scr_hz; if (name && n) snprintf(name, n, "%s", g_scr_name); pthread_mutex_unlock(&g_scr_mu); return hz;
}
@implementation DmWinCtl
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; app_quit("关窗口"); return NO; }
- (void)windowDidResize:(NSNotification *)n { (void)n; layout_game(); }
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; if (getenv("DM_APP")) dm_video_fullscreen_note(1); }   // 设置跟着窗口走（菜单 / ⌃⌘F 切的也算） | the setting follows the window (switching from the menu / ⌃⌘F counts too)
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n; if (getenv("DM_APP")) dm_video_fullscreen_note(0); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { (void)n; layout_game(); }
- (void)windowDidChangeScreen:(NSNotification *)n { (void)n; screen_note(); }
- (void)quit:(id)s { (void)s; app_quit("菜单退出"); }
// 系统来的退出（程序坞「退出」、⌘-Tab 时按 Q、注销 / 重启 / 关机、AppleScript quit）都走 terminate:。没有这个委托方法时 AppKit 直接结束进程，不存盘。
// app_quit 在后台线程上存完盘自己结束进程（dm_host_shutdown 里 _exit），这里一律答「稍后」：注销 / 关机等着这个进程结束，不会像答「取消」那样把注销取消掉；
// 已有一次退出在进行时也一样，等那次结束进程
// Quits coming from the system (Quit in the Dock, Q while Cmd-Tabbing, log out / restart / shut down, AppleScript quit) all go through terminate:. Without this delegate method AppKit ends the process directly, without saving.
// app_quit saves on a background thread and ends the process itself (_exit in dm_host_shutdown), so the answer here is always "later": log out / shut down wait for this process to end, instead of being cancelled as answering "cancel" would;
// the same when a quit is already under way, which will end the process
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a { (void)a; app_quit("系统退出"); return NSTerminateLater; }
// 系统的「退出」Apple 事件（程序坞「退出」、⌘-Tab 时按 Q、AppleScript quit、注销 / 关机）自己接：窗口上挂着表单（起名 / 数量输入框）时，AppKit 自带的处理把它丢掉，
// applicationShouldTerminate 也不来，输入框开着就退不掉。接过来直接走 app_quit（先按「取消」关掉表单、存盘、自己结束进程）。
// 要在 finishLaunching 之后装（[NSApp run] 里的 finishLaunching 会装上 AppKit 自己的处理）
// The system's "quit" Apple event (Quit in the Dock, Q while Cmd-Tabbing, AppleScript quit, log out / shut down) is handled here: while a sheet (the name / quantity input box) is attached to the window,
// AppKit's own handler drops it and applicationShouldTerminate never comes, so the App could not be quit with the box open. It now goes straight to app_quit (answer "Cancel" to the sheet, save, end the process itself).
// Installed after finishLaunching (the finishLaunching inside [NSApp run] installs AppKit's own handler)
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n; [[NSAppleEventManager sharedAppleEventManager] setEventHandler:self andSelector:@selector(quitEvent:reply:) forEventClass:kCoreEventClass andEventID:kAEQuitApplication];
}
- (void)quitEvent:(NSAppleEventDescriptor *)e reply:(NSAppleEventDescriptor *)r { (void)e; (void)r; app_quit("系统退出"); }
- (void)keys:(id)s { (void)s; show_keys(); }
- (void)share:(id)s { (void)s; share_logs_ui(); }
@end
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// 原版 16:9 常数，行为不变；DM_GAME_RES=宽x高（主机模式，掌机按 720/900 同比）只供与 iOS 宽屏对拍
// The original game's 16:9 constants, behavior unchanged; DM_GAME_RES=<width>x<height> (docked mode; handheld scaled by 720/900) is only for side-by-side comparison with iOS widescreen
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    if (g_full) return docked ? (uint64_t)(llround(g_render_w / g_aspect / 2) * 2) << 32 | (uint64_t)g_render_w : 720ull << 32 | (uint64_t)(llround(720 * g_aspect / 8) * 8);   // 真全屏：宽 = 出图宽（缺省 1600）、高按屏幕比例（16:10 → 1600x1004） | true full screen: width = render width (1600 by default), height by the screen's aspect ratio (16:10 -> 1600x1004)
    return docked ? (uint64_t)(llround(g_render_w * 9.0 / 16 / 2) * 2) << 32 | (uint64_t)g_render_w : 720ull << 32 | 1280;   // 缺省 1600x900 | 1600x900 by default
}

static int g_mouse_down;      // 这一笔左键的按下收了（落在画面里）：随后的拖动 / 抬起才交下去 | the press of this left-button gesture was taken (inside the picture): only then are its drags / release passed on
static NSPoint g_test_last;   // 合成鼠标脚本：上一条带坐标的指令落在视图里的点（dragto 从这里起拖） | synthetic mouse script: the point in the view where the last instruction with coordinates landed (dragto starts dragging from here)
static int g_wheel;   // 滚轮：+1 / −1 = 右摇杆向上 / 向下推一小会儿（原版手柄版用右摇杆滚列表） | Wheel: +1 / -1 = push the right stick up / down for a short while (the original gamepad UI scrolls lists with the right stick)
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, g_wheel * 32767);
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → 按摇杆 | F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return g_live ? 40 : 10; case 48: return 11;                            // 回车 Tab → + −（App 形态：回车交给 touch_mac.c 按当前页决定是 ＋ 还是确认，见那里） | Return Tab -> + - (App form: Return is handed to touch_mac.c, which decides by the current page whether it is + or confirm, see there)
        case 24: case 69: return 10;                                                     // = 键（＋ 那个键）、小键盘 + → ＋ | the = key (the one with +) and keypad + -> +
        case 49: return 0;                                                               // 空格 → A（确认） | Space -> A (confirm)
        case 53: return g_live ? 41 : -1;                                                // Esc → 返回（App 形态；局内没有面板时是开功能页，touch_mac.c） | Esc -> back (App form; with no panel open in game it opens the function page, touch_mac.c)
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static void on_key(NSEvent *e, int down) {
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int b = key_bit(c);
    if (down && (b >= 0 || c == 13 || c <= 2)) dm_padmode_request_glyph(DM_GLYPH_KBD);   // 按了游戏用的键：键位图标换成键盘键帽（鼠标移动 / 点击不算） | a key used by the game was pressed: switch the key icons to keyboard key caps (mouse moves / clicks do not count)
    if (b >= 0) g_btn = down ? (g_btn | 1ull << b) : (g_btn & ~(1ull << b));
    push();
}

typedef struct { void (*fn)(void); } guest_arg;
// 游戏自己退出（nnMain 返回）：走与关窗口相同的收尾——等这一局还排着的 iCloud 上传，再 dm_host_shutdown（停预取、暂停声音、挂起图形，_exit）。
// 原先在客体线程上直接 exit(0)：跳过 dm_host_shutdown，排着的上传被掐掉，atexit / C++ 析构跑的时候主线程与出图线程还在动。
// 这时游戏已经不拍了，没有能存的盘（不调 dm_save_wait）。dm_host_shutdown 返回 = 已有一次退出在进行（关窗口等），由那次结束进程
// The game exiting by itself (nnMain returned): the same wrap-up as closing the window -- wait for this run's still-queued iCloud upload, then dm_host_shutdown (stop prefetch, pause audio, suspend graphics, _exit).
// It used to call exit(0) right on the guest thread: dm_host_shutdown was skipped, the queued upload was cut off, and atexit / C++ destructors ran while the main and present threads were still busy.
// The game no longer ticks at this point, so there is nothing to save (no dm_save_wait). dm_host_shutdown returning = a quit is already under way (window closed etc.), and that one ends the process
static void *guest_tramp(void *p) {
    static char alt[65536];                       // 信号备用栈是每线程的；崩溃打印要在客体线程上也生效 | The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("客体线程结束：游戏自己退出 → 有序收尾");
    { int dm_cloud_flush_wait(double sec); dm_cloud_flush_wait(10); }
    dm_host_shutdown("客体线程结束");
    return NULL;
}

void front_run(void (*guest)(void)) {
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        full_setup(); render_setup();
        NSRect r = NSMakeRect(0, 0, 1280, floor(1280 / g_aspect));
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"D2R Metal"; win.restorable = NO; g_win = win; g_dm_ctl_app_fn = ctl_app;
        NSView *v = win.contentView; g_view = v;
        v.wantsLayer = YES; v.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary; win.contentMinSize = NSMakeSize(640, floor(640 / g_aspect)); if (g_full) win.contentAspectRatio = NSMakeSize(1280, 1280 / g_aspect);
        static DmWinCtl *ctl; ctl = [DmWinCtl new]; win.delegate = ctl; NSApp.delegate = ctl;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        layer_color(dm_video_get("color") == 1);
        [v.layer addSublayer:g_layer]; layout_game();
        dm_vkbd_ui_set_host(vkbd_where);
        [win center];
        g_live = getenv("DM_APP") || getenv("DM_TOUCH_LIVE"); if (g_live) dm_touch_host_live_enable();
        if (g_live) {
            win.acceptsMouseMovedEvents = YES;
            [v addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:v userInfo:nil]];
        }
        { // 菜单栏：应用（退出 ⌘Q）/ 显示（全屏 ⌃⌘F）/ 帮助（键位对照） | Menu bar: application (Quit ⌘Q) / View (Full Screen ⌃⌘F) / Help (key mapping)
          NSMenu *bar = [NSMenu new];
          NSMenu *(^add)(const char *) = ^NSMenu *(const char *t) { NSMenuItem *it = [NSMenuItem new]; [bar addItem:it]; NSMenu *m = [[NSMenu alloc] initWithTitle:@(t)]; it.submenu = m; return m; };
          [[add("") addItemWithTitle:@(ui("dmAppQuit")) action:@selector(quit:) keyEquivalent:@"q"] setTarget:ctl];
          NSMenuItem *fs = [add(ui("dmAppMenuView")) addItemWithTitle:@(ui("dmAppFullscreen")) action:@selector(toggleFullScreen:) keyEquivalent:@"f"]; fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
          { NSMenu *hm = add(ui("dmAppMenuHelp")); [[hm addItemWithTitle:@(ui("dmAppKeysTitle")) action:@selector(keys:) keyEquivalent:@""] setTarget:ctl]; [[hm addItemWithTitle:@(ui("dmAppShareLog")) action:@selector(share:) keyEquivalent:@""] setTarget:ctl]; }
          NSApp.mainMenu = bar; }
        { void dm_gamepad_start(void (*on_mode)(int pad)), dm_gamepad_on_press(void (*fn)(void)); dm_gamepad_start(NULL); dm_gamepad_on_press(text_input_handoff_pad); }   // 手柄（与 iOS 壳共用 ios/Sources/dm_gamepad.m）：连上即用，与键盘并存 | Gamepad (ios/Sources/dm_gamepad.m, shared with the iOS shell): works as soon as it is connected, alongside the keyboard
        [win makeKeyAndOrderFront:nil];
        screen_note();
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; screen_note(); }];
        [NSApp activateIgnoringOtherApps:YES];
        // 启动即全屏：选项页「显示模式」是全屏（dm_video.c 的 mode；旧版偏好 StartFullScreen 首次启动时接过来）。全屏测试开关（DM_TEST_FULLSCREEN_AT）在时不理它，
        // 来回切的时刻表才不被打乱。等运行循环转起来再切，窗口那时已上屏
        // Start in full screen: the options page "Display Mode" is Fullscreen (mode in dm_video.c; the older StartFullScreen preference is taken over on first boot). Ignored with the full-screen test switch
        // (DM_TEST_FULLSCREEN_AT) so its toggle schedule stays intact. The switch waits for the run loop, when the window is on screen.
        if (getenv("DM_APP") && !getenv("DM_TEST_FULLSCREEN_AT") && dm_video_get("mode") == 1) {
            LOG("窗口：启动即全屏（显示模式 = 全屏） | window: starts in full screen (Display Mode = Fullscreen)");
            dispatch_async(dispatch_get_main_queue(), ^{ if (!(win.styleMask & NSWindowStyleMaskFullScreen)) [win toggleFullScreen:nil]; });
        }
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // 只吃游戏窗口的键；软键盘输入框等其他窗口照常收字 | Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            if (dm_vkbd_ui_active() && !(e.modifierFlags & NSEventModifierFlagCommand)) {   // 手柄屏幕键盘开着时键盘有输入：回车 = 确定、Esc = 取消，别的键把这一下记进去（打的字、退格）再收起屏幕键盘、换成表单；松开照交给游戏，之前按着的键不会卡住 | keyboard input while the on-screen keyboard is open: Return = OK, Esc = cancel, any other key is applied (typed character, backspace), then the on-screen keyboard closes and the sheet takes over; releases still go to the game so keys held before are not left stuck
                if (e.type == NSEventTypeKeyUp) { on_key(e, 0); return nil; }
                unsigned short kc = e.keyCode;
                if (kc == 36 || kc == 76 || kc == 53) { dm_vkbd_ui_key(kc == 53 ? DM_VKBD_KEY_CANCEL : DM_VKBD_KEY_OK, NULL); return nil; }
                if (kc == 51) dm_vkbd_ui_key(DM_VKBD_KEY_BACK, NULL); else if (kc < 123 || kc > 126) dm_vkbd_ui_key(DM_VKBD_KEY_TEXT, e.characters.UTF8String);
                dm_padmode_request_glyph(DM_GLYPH_KBD); dm_vkbd_ui_handoff();
                return nil;
            }
            if (e.modifierFlags & NSEventModifierFlagCommand) { if (e.type == NSEventTypeKeyUp) on_key(e, 0); return e; }   // 带 ⌘ 的按下留给菜单（⌘Q、⌃⌘F）；松开照样记上（按着 W 时按住 ⌘ 再松开 W，原先松开收不到、角色一直走） | Key presses with ⌘ are left to the menu (⌘Q, ⌃⌘F); releases are still recorded (releasing W while ⌘ was held used to be missed, and the character kept walking)
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // 吃掉，免得系统提示音 | Swallow it to avoid the system beep
        }];
        // 左键沿用单点触屏；Mac App 的无按键悬停只交坐标，复用同一套窗口 / 全屏换算，不合成点击。
        // Left clicks reuse single-point touch; Mac App button-free hover only submits coordinates using the same window / full-screen conversion, without synthesizing clicks.
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp | NSEventMaskMouseMoved | NSEventMaskMouseExited handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;
            if (dm_vkbd_ui_active() && e.type != NSEventTypeLeftMouseUp) {   // 手柄屏幕键盘开着时鼠标点了：收起屏幕键盘、换成表单；悬停 / 拖动不交给游戏 | a mouse click while the on-screen keyboard is open: it closes and the sheet takes over; hover / drags do not go to the game
                if (e.type == NSEventTypeLeftMouseDown) dm_vkbd_ui_handoff();
                return e.type == NSEventTypeLeftMouseDown || e.type == NSEventTypeLeftMouseDragged ? nil : e; }
            if (text_input_up() && e.type != NSEventTypeLeftMouseUp) return e;   // 输入框表单开着：按下 / 拖动 / 悬停交还系统（点不进游戏窗口），抬起照收，免得之前按下的那一下收不回 | input box sheet open: presses / drags / hover go back to the system (the game window takes no clicks); a release is still taken so an earlier press is not left held
            BOOL hover = e.type == NSEventTypeMouseMoved || e.type == NSEventTypeMouseExited;
            if (hover && getenv("DM_TEST_HOVER_LOG")) LOG("鼠标悬停输入：类型 %ld", (long)e.type);
            if (hover && !g_live) return e;
            if (e.type == NSEventTypeMouseExited) { dm_touch_host_live(3, -1, -1); return e; }
            if (e.type == NSEventTypeLeftMouseUp) { if (!g_mouse_down) return nil; g_mouse_down = 0; if (g_live) dm_touch_host_live(2, 0, 0); else dm_touch_set(NULL, 0); return nil; }
            if (e.type == NSEventTypeLeftMouseDragged && !g_mouse_down) return nil;   // 按在黑边上没收的那一笔：拖动夹到画面边上会被当成按下，抬起就成了一下点击 | a gesture whose press on the black bars was not taken: a drag clamped to the picture edge would count as a press and its release as a click
            NSPoint p = [v convertPoint:e.locationInWindow fromView:nil]; NSRect gr = game_rect();
            if (hover && getenv("DM_TEST_HOVER_LOG")) LOG("鼠标悬停坐标：窗口 %.1f,%.1f 视图 %.1f,%.1f 画面 %.1f,%.1f %.1fx%.1f", e.locationInWindow.x, e.locationInWindow.y, p.x, p.y, gr.origin.x, gr.origin.y, gr.size.width, gr.size.height);
            if (gr.size.width <= 0 || gr.size.height <= 0) return e;
            float nx = (float)((p.x - gr.origin.x) / gr.size.width), ny = (float)(1.0 - (p.y - gr.origin.y) / gr.size.height);   // AppKit 视图原点在左下，触屏原点在左上 | The AppKit view origin is at bottom left, the touchscreen origin is at top left
            if (e.type == NSEventTypeLeftMouseDown) { g_mouse_down = nx >= 0 && nx <= 1 && ny >= 0 && ny <= 1; if (!g_mouse_down) return nil; }   // 按在黑边上：不算点 | Pressed on the black bars: not a click
            if (g_full && g_live) {   // 真全屏：状态机吃界面矩形里的归一化坐标（同 iOS 的游戏视图）。两步： | true full screen: the state machine takes normalised coordinates inside the interface rectangle (like the iOS game view). Two steps:
                // ① 先换到「实际显示的那块」：游戏当前分辨率（前端 16:9、局内整屏）按比例居中放进游戏画面，与上屏同一口径（nvn_mtl.m present_encode：比例差 1% 内铺满）；
                // ② 再换到界面矩形（局内贴底）。局内界面矩形外是 3D 世界，带符号交下去（点世界换回整屏，touch_mac.c）；不在局内时显示块外是黑边，不算点
                // (1) First map to "the block actually shown": the game's current resolution (16:9 in the front end, the whole screen in game) placed centred in the game picture by aspect ratio, the same measure as presentation (nvn_mtl.m present_encode: fill when the aspect ratios differ by less than 1%);
                // (2) then map to the interface rectangle (along the bottom in game). In game, outside the interface rectangle is the 3D world, passed down with a sign (tapping the world maps back to the whole screen, touch_mac.c); outside the game the area beyond the shown block is the black bars, not counted as a tap
                float vx, uy; full_ui_point(gr, nx, ny, &vx, &uy);
                if (e.type == NSEventTypeLeftMouseDown && !dm_uiscreen_ingame() && (vx < 0 || vx > 1 || uy < 0 || uy > 1)) { g_mouse_down = 0; return nil; }
                dm_touch_host_live(hover ? 3 : e.type == NSEventTypeLeftMouseDown ? 0 : 1, vx, uy); return hover ? e : nil; }
            if (hover) { dm_touch_host_live(3, nx, ny); return e; }
            dm_touch t = {0, nx < 0 ? 0 : nx > 1 ? 1 : nx, ny < 0 ? 0 : ny > 1 ? 1 : ny};
            if (g_live) { dm_touch_host_live(e.type == NSEventTypeLeftMouseDown ? 0 : 1, t.x, t.y); return nil; }
            dm_touch_set(&t, 1); return nil;
        }];
        // 右键 = 手柄 B（返回 / 关面板）；滚轮 = 界面上在指针处发原版滚轮（touch_mac.c dm_wheel_ui），局内没开面板时右摇杆上下推 0.12 秒。只在 App 形态接（命令行宿主的测试不受影响）
        // Right click = gamepad B (back / close panel); wheel = over the UI, the original wheel at the pointer (touch_mac.c dm_wheel_ui); in game with no panel open, push the right stick up or down for 0.12 s. Only wired in App form (tests of the command-line host are unaffected)
        if (g_live) {
            [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskRightMouseDown | NSEventMaskRightMouseUp handler:^NSEvent *(NSEvent *e) {
                if (e.type == NSEventTypeRightMouseDown && e.window == win && dm_vkbd_ui_active()) { dm_vkbd_ui_handoff(); return nil; }   // 同左键：换成表单 | as the left button: hand over to the sheet
                if (e.window != win || (text_input_up() && e.type == NSEventTypeRightMouseDown)) return e;   // 输入框开着不按 B，松开照收 | no B press while the input box is open; the release is still taken
                if (e.type == NSEventTypeRightMouseDown) g_btn |= DM_BTN_B; else g_btn &= ~(uint64_t)DM_BTN_B;
                push(); return nil;
            }];
            [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel handler:^NSEvent *(NSEvent *e) {
                if (e.window != win || e.scrollingDeltaY == 0 || text_input_up() || dm_vkbd_ui_active()) return e;
                NSPoint p = [v convertPoint:e.locationInWindow fromView:nil]; NSRect gr = game_rect();
                if (gr.size.width > 0 && gr.size.height > 0) {
                    float nx = (float)((p.x - gr.origin.x) / gr.size.width), ny = (float)(1.0 - (p.y - gr.origin.y) / gr.size.height), ux = nx, uy = ny;
                    if (g_full) full_ui_point(gr, nx, ny, &ux, &uy);
                    int dm_wheel_ui(float, float, double, int, double);
                    if (dm_wheel_ui(ux, uy, e.scrollingDeltaY, e.hasPreciseScrollingDeltas, gr.size.height)) return nil;
                }
                static int gen; int my = ++gen; g_wheel = e.scrollingDeltaY > 0 ? 1 : -1; push();
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.12 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ if (my == gen) { g_wheel = 0; push(); } });
                return nil;
            }];
            // 指针自动隐藏：鼠标指针停在游戏窗口里不动一段时间就藏起来（用键盘 / 手柄玩时不挡画面）。用 setHiddenUntilMouseMoves，鼠标一动系统自己显示回来，没有隐藏 / 显示的计数要配对。
            // 计时器只挂在缺省运行循环模式上：菜单、拖动改窗口大小、模态对话框（键位对照）期间都不触发。只在 App 在前台、游戏窗口是键窗口（输入框表单开着时键窗口是表单）、指针在窗口内容区里、没按着鼠标键时隐藏。
            // 时长：defaults write org.d2rmobile.mac CursorHideSeconds -float <秒>（0 = 关），缺省 3 秒
            // Pointer auto-hide: the mouse pointer hides after it rests over the game window for a while (so it doesn't cover the picture while you play with keyboard / controller). It uses setHiddenUntilMouseMoves: the system shows it again on the next mouse movement, so there is no hide / unhide count to balance.
            // The timer is scheduled only in the default run loop mode: it does not fire during menus, window resizing or modal dialogs (key map). It hides only while the App is frontmost, the game window is key (while the input box sheet is up the sheet is key), the pointer is inside the window's content area and no mouse button is held.
            // Duration: defaults write org.d2rmobile.mac CursorHideSeconds -float <seconds> (0 = off), default 3 seconds
            { id pref = [NSUserDefaults.standardUserDefaults objectForKey:@"CursorHideSeconds"];   // 手写的设置可能是任何类型：只认数字或数字字符串，数组、日期等当没设 | a hand-written default can be any type: only a number or numeric string counts, an array, date etc. is treated as unset
              double idle = [pref respondsToSelector:@selector(doubleValue)] ? [pref doubleValue] : 3.0;
              if (idle > 0 && isfinite(idle)) { __block NSPoint last = NSMakePoint(NAN, NAN); __block double since = 0; __block BOOL hid = NO;
                  LOG("鼠标指针：静止 %.1f 秒自动隐藏 | pointer auto-hides after %.1f s at rest", idle, idle);
                  [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) { (void)t;
                      NSPoint m = NSEvent.mouseLocation; double now = NSProcessInfo.processInfo.systemUptime;
                      if (!NSEqualPoints(m, last) || NSEvent.pressedMouseButtons || !NSApp.isActive || !win.isKeyWindow) { last = m; since = now; hid = NO; return; }   // 动过、按着键或不在前台：重新计时 | moved, a button held or not frontmost: restart the clock
                      if (hid || now - since < idle || !NSPointInRect(m, [win convertRectToScreen:v.frame])) return;   // 已藏过、没到时长、指针不在内容区：不动 | already hidden, not long enough, or pointer outside the content area: leave it
                      [NSCursor setHiddenUntilMouseMoves:YES]; hid = YES; }]; } }
        }
        if (getenv("DM_APP") && !getenv("DM_APP_ROMFS") && ![NSUserDefaults.standardUserDefaults boolForKey:@"KeysShown"]) { show_keys(); [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"KeysShown"]; }   // 第一次启动给一次键位对照（界面上的按键提示是手柄字母）；以后在「帮助」菜单里 | Show the key mapping once on first launch (button prompts on screen are gamepad letters); afterwards it is in the Help menu
        { const char *e;   // 测试（没人点窗口时用）：DM_TEST_WINSIZE=宽x高 设内容区大小；DM_TEST_FULLSCREEN_AT=<秒> 进全屏；DM_TEST_QUIT_AT=<秒> 走一次退出 | Tests (for when nobody is there to click the window): DM_TEST_WINSIZE=WxH sets the content size; DM_TEST_FULLSCREEN_AT=<seconds> enters full screen; DM_TEST_QUIT_AT=<seconds> goes through one quit
          int w, h; if ((e = getenv("DM_TEST_WINSIZE")) && sscanf(e, "%dx%d", &w, &h) == 2) { [win setContentSize:NSMakeSize(w, h)]; layout_game(); }
          if ((e = getenv("DM_TEST_FULLSCREEN_AT"))) for (const char *q = e; q && *q; q = strchr(q, ';') ? strchr(q, ';') + 1 : NULL)   // 「秒[;秒…]」：每个时刻切一次全屏（来回切） | "seconds[;seconds...]": toggle full screen at each time (back and forth)
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(q) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; });
          // DM_TEST_PRESENT_SHOT=<秒>:<路径>[;<秒>:<路径>…]：到点取下一次上屏的画面（图形层与上屏同一套编码，见 nvn_mtl.m mtl_grab_next_present）写成 PNG——
          // 看的是玩家真看到的（含留边位置），不是游戏出图贴图（DM_NVN_SHOT_UI 取的那张，裁切区不居中）
          // DM_TEST_PRESENT_SHOT=<seconds>:<path>[;<seconds>:<path>...]: at the given time, grab the next presented picture (the graphics layer uses the same encoding as presentation, see nvn_mtl.m mtl_grab_next_present) and write it as PNG -
          // this is what the player actually sees (including where the bars are), not the game's output texture (the one DM_NVN_SHOT_UI takes, whose crop is not centred)
          if ((e = getenv("DM_TEST_PRESENT_SHOT"))) { void mtl_grab_next_present(void (^cb)(CGImageRef img));
              for (NSString *it in [@(e) componentsSeparatedByString:@";"]) { NSRange c = [it rangeOfString:@":"]; if (c.location == NSNotFound) continue;
                  double at = [it substringToIndex:c.location].doubleValue; NSString *path = [it substringFromIndex:c.location + 1];
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                      NSSize vb = v.bounds.size; NSRect gr = game_rect(); double fy = 0, fh = 1; ui_rect_frac(&fy, &fh);
                      CGRect kr; CGImageRef kimg = dm_vkbd_ui_snapshot(&kr);   // 屏幕键盘是画面上面的层、不在上屏图里：叠上去，截图才是玩家看到的 | the on-screen keyboard is a layer above the picture, not in the presented image: composite it so the shot is what the player sees
                      mtl_grab_next_present(^(CGImageRef img0) { if (!img0) { LOG("【红】DM_TEST_PRESENT_SHOT：%s 没取到", path.UTF8String); if (kimg) CGImageRelease(kimg); return; }
                          CGImageRef img = CGImageRetain(img0);
                          if (kimg) { size_t W = CGImageGetWidth(img0), H = CGImageGetHeight(img0); double s = W / gr.size.width;
                              CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB); CGContextRef c = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little); CGColorSpaceRelease(cs);
                              if (c) { CGContextDrawImage(c, CGRectMake(0, 0, W, H), img0); CGContextDrawImage(c, CGRectMake(kr.origin.x * s, H - (kr.origin.y + kr.size.height) * s, kr.size.width * s, kr.size.height * s), kimg);
                                  CGImageRelease(img); img = CGBitmapContextCreateImage(c); CGContextRelease(c); }
                              CGImageRelease(kimg); }
                          CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], (__bridge CFStringRef)@"public.png", 1, NULL);
                          if (d) { CGImageDestinationAddImage(d, img, NULL); CGImageDestinationFinalize(d); CFRelease(d); }
                          LOG("DM_TEST_PRESENT_SHOT：%s %zux%zu（内容区 %.0fx%.0f，游戏画面 %.0fx%.0f 在 (%.0f,%.0f)，界面矩形纵向 %.3f 高 %.3f，%s%s）", path.UTF8String, CGImageGetWidth(img), CGImageGetHeight(img),
                              vb.width, vb.height, gr.size.width, gr.size.height, gr.origin.x, gr.origin.y, fy, fh, dm_uiscreen_ingame() ? "局内" : "局外", kimg ? "，叠了屏幕键盘" : "");
                          CGImageRelease(img); }); }); } }
          // DM_TEST_NSMOUSE=<文件>：合成真正的系统鼠标事件（NSEvent）投进 App 的事件队列，走与真人点击同一条分发路（事件监视器 → 换算 → 状态机），不经测试入口。
          // 每行「秒 down|move|up|hover x,y」，x,y 是内容区里的点（左上角为原点；窗口缺省 1280×720），时刻自窗口建好起算。
          // 坐标后可带锚点「@c」「@b」：按 1280×720 写的 y 换算到实际内容区高度——@c 上下居中的界面（前端页、大面板）加 (高 − 720) / 2，@b 贴底的（局内 HUD）加 (高 − 720)；
          // 不带 = 原样（贴顶）。真全屏下窗口比例跟着屏幕（16:10 屏 1280×803），同一份脚本两种比例都能用
          // DM_TEST_NSMOUSE=<file>: synthesizes real system mouse events (NSEvent) and posts them into the App's event queue, taking the same dispatch path as a real click (event monitor -> conversion -> state machine), not a test entry.
          // Each line is "seconds down|move|up|hover x,y", where x,y is a point in the content area (origin at top-left; window default 1280x720), timed from when the window is created.
          // An anchor "@c" / "@b" may follow the coordinates: a y written for 1280x720 is converted to the actual content height - @c for vertically centered UI (front-end pages, large panels) adds (height - 720) / 2, @b for bottom-anchored UI (in-game HUD) adds (height - 720);
          // none = as is (top-anchored). In true full screen the window aspect follows the screen (1280x803 on a 16:10 screen), so the same script works at both aspects
          { NSTimeInterval t0 = NSProcessInfo.processInfo.systemUptime;   // 测试输入的行解释（同一套给 DM_TEST_NSMOUSE 文件与控制口 fe 命令）：一行 =「秒 动作 参数」，秒自此刻起算；返回 1 = 排好、0 = 不认识 | the test-input line interpreter (shared by the DM_TEST_NSMOUSE file and the control port's fe command): one line = "seconds action arguments", seconds counted from now; returns 1 = scheduled, 0 = not understood
              dispatch_queue_t tq = dispatch_queue_create("dm.test.nsevent", DISPATCH_QUEUE_SERIAL);   // 不排在主队列上：模态弹框（如键位对照）在主队列的一个块里跑模态循环，那期间主队列上别的块不执行，事件就投不进弹框 | not queued on the main queue: a modal dialog (such as the key map) runs its modal loop inside a block on the main queue, during which other blocks on the main queue do not execute, so events could not be delivered into the dialog
              int (^fe_line)(NSString *) = ^int(NSString *ln) { double sec, x = 0, y = 0; char op[8];
                  if (sscanf(ln.UTF8String, "%lf %7s %lf,%lf", &sec, op, &x, &y) < 2 || ln.UTF8String[0] == '#') return 0;
                  const char *at = strchr(ln.UTF8String, '@'); double anc = at && at[1] == 'c' ? 0.5 : at && at[1] == 'b' ? 1 : 0;   // 锚点（见上）：在投事件那一刻按当时的内容区高度换算 | Anchor (see above): converted using the content area height at the moment the event is posted
                  if (!strcmp(op, "sheet")) {   // 「秒 sheet ok|cancel [字]」：往开着的输入框表单里填这些字（不写 = 空）再按确定 / 取消（text_input_test） | "<seconds> sheet ok|cancel [text]": fill the open input sheet with this text (omitted = empty), then press OK / Cancel (text_input_test)
                      char how[16] = "", txt[64] = ""; sscanf(ln.UTF8String, "%*lf %*7s %15s %63s", how, txt); NSString *t = @(txt); int ok = !strcmp(how, "ok");
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ text_input_test(t, ok); }); CFRunLoopWakeUp(CFRunLoopGetMain()); });
                      return 1; }
                  if (!strcmp(op, "skey")) {   // 「秒 skey <键码> [字]」：合成一次键盘按下 / 松开，直接交给 App 的事件分发（[NSApp sendEvent:]，走事件监视器，同真键盘按在游戏窗口上），不靠 App 在前台 | "<seconds> skey <key code> [chars]": synthesize one key press / release and hand it straight to the App's event dispatch ([NSApp sendEvent:], through the event monitors, like a real key on the game window), without needing the App in front
                      char kcs[16] = "", chs[32] = ""; sscanf(ln.UTF8String, "%*lf %*7s %15s %31s", kcs, chs); unsigned short kc = (unsigned short)atoi(kcs); NSString *chars = @(chs);
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 skey %d「%s」", NSProcessInfo.processInfo.systemUptime - t0, kc, chars.UTF8String);
                          for (int dn = 1; dn >= 0; dn--) [NSApp sendEvent:[NSEvent keyEventWithType:dn ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:kc]]; });
                          CFRunLoopWakeUp(CFRunLoopGetMain()); });
                      return 1; }
                  if (!strcmp(op, "pads")) {   // 「秒 pads 键,键,…」：虚拟手柄依次按这些键，每下 0.1 秒、间隔 0.15 秒，自己掐时（一条一条排定时会被系统合并成同一刻） | "<seconds> pads key,key,...": the virtual controller presses these in turn, 0.1 s each with 0.15 s gaps, timed by itself (one scheduled line per press gets merged into the same instant by the system)
                      char arg[512] = ""; sscanf(ln.UTF8String, "%*lf %*7s %511s", arg); NSString *all = @(arg); NSArray<NSString *> *list = [all componentsSeparatedByString:@","];
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{
                          void dm_gamepad_inject(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry); uint64_t dm_key_mask(const char *names, const char **bad);
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 pads %s（%lu 下）", NSProcessInfo.processInfo.systemUptime - t0, all.UTF8String, (unsigned long)list.count);
                          for (NSString *k in list) { const char *bad = NULL; uint64_t m = dm_key_mask(k.UTF8String, &bad); if (!m) { LOG("DM_TEST_NSMOUSE：不认识的手柄键 %s", k.UTF8String); continue; }
                              CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ dm_gamepad_inject(m, 0, 0, 0, 0); }); CFRunLoopWakeUp(CFRunLoopGetMain()); usleep(100000);
                              CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ dm_gamepad_inject(0, 0, 0, 0, 0); }); CFRunLoopWakeUp(CFRunLoopGetMain()); usleep(150000); } });
                      return 1; }
                  if (!strcmp(op, "pad") || !strcmp(op, "navpad") || !strcmp(op, "nav")) {
                      // 「秒 pad <键>」：虚拟手柄按 0.1 秒（键名同控制口 press：A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN，可用 + 连），走 dm_gamepad 同一条路（屏幕键盘开着时归它），不靠 App 在前台；
                      // 「秒 navpad <键>」同 pad，但输入框 / 屏幕键盘开着时跳过；「秒 nav <键码>」把这个键直接交给游戏的键盘处理（同真键盘按在游戏窗口上，但不靠 App 在前台），输入框开着时也跳过。
                      // 带游戏到起名页用 nav / navpad：读取快慢不一，多排几下也不怕，框一弹出来后面的就不按了
                      // "<seconds> pad <buttons>": a virtual controller press for 0.1 s (names as the control port's press: A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN, joined with +), through the same dm_gamepad path (owned by the on-screen keyboard while it is open), without needing the App in front;
                      // "<seconds> navpad <buttons>" is like pad but skipped while an input box / the on-screen keyboard is open; "<seconds> nav <key code>" hands the key straight to the game's keyboard handling (like a real key on the game window, without needing the App in front), also skipped while an input box is open.
                      // nav / navpad bring the game to the naming page: loading speed varies, so extra presses are harmless; once the box is up the rest are not pressed
                      char arg[32] = ""; sscanf(ln.UTF8String, "%*lf %*7s %31s", arg);
                      int nav = op[0] == 'n', pad = strcmp(op, "nav") != 0; unsigned short kc = (unsigned short)atoi(arg); uint64_t m = 0;
                      if (pad) { uint64_t dm_key_mask(const char *names, const char **bad); const char *bad = NULL; if (!(m = dm_key_mask(arg, &bad))) { LOG("DM_TEST_NSMOUSE：不认识的手柄键 %s", arg); return 0; } }
                      NSString *what = [NSString stringWithFormat:@"%s %s", op, arg];
                      void (^perform)(void (^)(void)) = ^(void (^b)(void)) { CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, b); CFRunLoopWakeUp(CFRunLoopGetMain()); };
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ perform(^{
                          if (nav && (text_input_up() || dm_vkbd_ui_active())) { LOG("DM_TEST_NSMOUSE：%.2f 秒 %s 跳过（输入框开着）", NSProcessInfo.processInfo.systemUptime - t0, what.UTF8String); return; }
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 %s", NSProcessInfo.processInfo.systemUptime - t0, what.UTF8String);
                          void dm_gamepad_inject(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
                          NSEvent *(^kev)(NSEventType) = ^(NSEventType t) { return [NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:kc]; };
                          if (pad) dm_gamepad_inject(m, 0, 0, 0, 0); else on_key(kev(NSEventTypeKeyDown), 1);
                          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)), tq, ^{ perform(^{ if (pad) dm_gamepad_inject(0, 0, 0, 0, 0); else on_key(kev(NSEventTypeKeyUp), 0); }); }); }); });
                      return 1; }
                  if (!strcmp(op, "key") || !strcmp(op, "type")) {   // 「秒 key <键码>」：合成一次真正的键盘按下 / 0.08 秒后松开（键码 = 苹果的虚拟键码，如回车 36、Tab 48、J 38）；「秒 type <字>」：逐字打字。 | "<seconds> key <key code>": synthesise a real key press / release 0.08 s later (key code = Apple virtual key code, e.g. Return 36, Tab 48, J 38); "<seconds> type <text>": type character by character.
                      // 都投给当时的键窗口（真键盘就是这样：弹着输入框时进输入框），并记一行键窗口是不是游戏窗口
                      // All are posted to the key window at that moment (which is what a real keyboard does: into the input field while it is shown), and one line records whether the key window is the game window
                      int ty = !strcmp(op, "type"); char word[64] = ""; if (ty) sscanf(ln.UTF8String, "%*lf %*7s %63s", word); NSString *chars = ty ? @(word) : (x == 36 ? @"\r" : @"");
                      if (ty) {   // 打字：在主线程的运行循环里执行（模态框开着时也走——主队列那时不走），记下第一响应者是谁，再把字经响应链送进去（等于输入法提交了这些字） | typing: executed in the main thread's run loop (which also runs while a modal dialog is open - the main queue does not), noting who the first responder is, then sending the text through the responder chain (as if an input method committed it)
                          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                              NSWindow *kw = NSApp.keyWindow; id fr = kw.firstResponder; BOOL ok = [NSApp sendAction:@selector(insertText:) to:nil from:chars];
                              LOG("DM_TEST_NSMOUSE：%.2f 秒打字「%s」（键窗口%s，第一响应者 %s，%s）", NSProcessInfo.processInfo.systemUptime - t0, chars.UTF8String, kw == win ? "是游戏窗口" : kw ? "是弹框" : "没有", fr ? NSStringFromClass([fr class]).UTF8String : "无", ok ? "有人收" : "没人收"); });
                              CFRunLoopWakeUp(CFRunLoopGetMain()); });
                          return 1; }
                      for (NSUInteger ci = 0; ci < (ty ? chars.length : 1); ci++) { NSString *one = ty ? [chars substringWithRange:NSMakeRange(ci, 1)] : chars; double at = sec + ci * 0.15;
                          for (int dn = 1; dn >= 0; dn--) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((at + (dn ? 0 : 0.08)) * NSEC_PER_SEC)), tq, ^{ NSWindow *kw = NSApp.keyWindow;
                              if (dn) LOG("DM_TEST_NSMOUSE：%.2f 秒投 键盘 %s%s（键窗口%s）", NSProcessInfo.processInfo.systemUptime - t0, ty ? "字 " : "键码 ", ty ? one.UTF8String : [NSString stringWithFormat:@"%d", (int)x].UTF8String, kw == win ? "是游戏窗口" : kw ? "是别的窗口（弹框）" : "没有：App 里没有键窗口");
                              [NSApp postEvent:[NSEvent keyEventWithType:dn ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:(kw ?: win).windowNumber context:nil characters:one charactersIgnoringModifiers:one isARepeat:NO keyCode:ty ? 0 : (unsigned short)x] atStart:NO]; }); }
                      return 1; }
                  if (!strcmp(op, "unit") || !strcmp(op, "unith")) {   // 「秒 unit <类型>,<类号>」：盯住这类单位（类号 16777214 = 敌对且可选中的怪、16777215 = 任意），它一在画面里就在它身体框中心投一次真正的鼠标点击；unith = 按住 2.5 秒才抬起 | "<seconds> unit <type>,<class>": watch for units of this kind (class 16777214 = hostile and selectable monster, 16777215 = any) and post one real mouse click at the centre of its body box as soon as it is on screen; unith = hold for 2.5 seconds before releasing
                      double hold = !strcmp(op, "unith") ? 2.5 : 0.1;
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ void dm_world_watch(int, uint32_t); int dm_world_watch_pos(float *, float *); float nx = 0, ny = 0; int ok = 0;
                          dm_world_watch((int)x, (uint32_t)y); for (int i = 0; i < 60 && !(ok = dm_world_watch_pos(&nx, &ny)); i++) usleep(50000); dm_world_watch(-1, 0);
                          if (!ok) { LOG("DM_TEST_NSMOUSE：unit %d,%u 3 秒内没进画面，不点", (int)x, (unsigned)y); return; }
                          __block NSPoint pt; dispatch_sync(dispatch_get_main_queue(), ^{ pt = [v convertPoint:NSMakePoint(nx * v.bounds.size.width, (1 - ny) * v.bounds.size.height) toView:nil]; });
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 unit %d,%u 在画面 (%.3f,%.3f) → 投按下，%.1f 秒后抬起", NSProcessInfo.processInfo.systemUptime - t0, (int)x, (unsigned)y, nx, ny, hold);
                          for (int dn = 1; dn >= 0; dn--) { if (!dn) usleep((useconds_t)(hold * 1e6));
                              [NSApp postEvent:[NSEvent mouseEventWithType:dn ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp location:pt modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:dn ? 1 : 0] atStart:NO]; } });
                      return 1; }
                  if (!strcmp(op, "dbl") || !strcmp(op, "dragto")) {   // 「秒 dbl x,y」：同一点连点两下（间隔 0.15 秒，时刻自己掐，不靠两条定时——定时有零点几秒的误差，凑不出双击）； | "<seconds> dbl x,y": click the same point twice (0.15 s apart, timed by itself rather than by two scheduled lines - scheduling is off by a few tenths of a second and cannot produce a double click);
                      // 「秒 dragto x,y」：从上一条 down 的位置拖到这里（0.6 秒里匀速 12 步）再抬起，前面要先有一条 down
                      // "<seconds> dragto x,y": drag from the position of the previous down to here (12 even steps within 0.6 s) and release; a down must come before it
                      int dbl = !strcmp(op, "dbl");
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ __block NSPoint from, to; dispatch_sync(dispatch_get_main_queue(), ^{ from = g_test_last; to = NSMakePoint(x, v.bounds.size.height - (y + anc * (v.bounds.size.height - 720))); if (dbl) from = to; g_test_last = to; });
                          NSEvent *(^mk)(NSEventType, NSPoint, int) = ^(NSEventType t, NSPoint p, int cc) { __block NSPoint wp; dispatch_sync(dispatch_get_main_queue(), ^{ wp = [v convertPoint:p toView:nil]; });
                              return [NSEvent mouseEventWithType:t location:wp modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:cc pressure:t == NSEventTypeLeftMouseUp ? 0 : 1]; };
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 %s 内容区 (%.0f,%.0f)", NSProcessInfo.processInfo.systemUptime - t0, dbl ? "双击" : "拖到", to.x, v.bounds.size.height - to.y);
                          if (dbl) { for (int i = 0; i < 2; i++) { [NSApp postEvent:mk(NSEventTypeLeftMouseDown, to, i + 1) atStart:NO]; usleep(60000); [NSApp postEvent:mk(NSEventTypeLeftMouseUp, to, i + 1) atStart:NO]; usleep(150000); } }
                          else { for (int i = 1; i <= 12; i++) { [NSApp postEvent:mk(NSEventTypeLeftMouseDragged, NSMakePoint(from.x + (to.x - from.x) * i / 12, from.y + (to.y - from.y) * i / 12), 1) atStart:NO]; usleep(50000); } usleep(200000); [NSApp postEvent:mk(NSEventTypeLeftMouseUp, to, 1) atStart:NO]; } });
                      return 1; }
                  NSEventType ty = !strcmp(op, "hover") ? NSEventTypeMouseMoved : !strcmp(op, "down") ? NSEventTypeLeftMouseDown : !strcmp(op, "up") ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDragged; int has = !strcmp(op, "up") ? 0 : 1;
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ static NSPoint last;
                      if (has) { last = NSMakePoint(x, v.bounds.size.height - (y + anc * (v.bounds.size.height - 720))); g_test_last = last; }   // 内容区的点 → 窗口坐标（原点左下） | point in the content area -> window coordinates (origin at bottom-left)
                      NSEvent *ev = [NSEvent mouseEventWithType:ty location:[v convertPoint:last toView:nil] modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:ty == NSEventTypeLeftMouseUp ? 0 : 1];
                      LOG("DM_TEST_NSMOUSE：%.2f 秒投 %s 内容区 (%.0f,%.0f)（窗口%s是主窗口，App %s在前台，背板缩放 %.1f）", NSProcessInfo.processInfo.systemUptime - t0, ty == NSEventTypeMouseMoved ? "悬停" : ty == NSEventTypeLeftMouseDown ? "按下" : ty == NSEventTypeLeftMouseUp ? "抬起" : "拖动", last.x, v.bounds.size.height - last.y, win.isKeyWindow ? "" : "不", NSApp.isActive ? "" : "不", win.backingScaleFactor);
                      [NSApp postEvent:ev atStart:NO]; }); return 1; };
              g_fe_line = fe_line; g_dm_ctl_fe_fn = fe_line_c;
              if ((e = getenv("DM_TEST_NSMOUSE"))) { NSString *txt = [NSString stringWithContentsOfFile:@(e) encoding:NSUTF8StringEncoding error:nil]; int n = 0;
                  for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) n += fe_line(ln);
                  LOG("DM_TEST_NSMOUSE：%d 条排好", n); } }
          if ((e = getenv("DM_TEST_SHARELOG"))) { NSString *to = @(e); dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ NSString *er = share_logs(to); LOG("DM_TEST_SHARELOG：第 8 秒打包到 %s → %s", to.lastPathComponent.UTF8String, er ? er.UTF8String : "OK"); }); }   // 测试：不弹存储面板，第 8 秒直接把压缩包写到这个路径 | test: no save panel, write the archive directly to this path at second 8
          if ((e = getenv("DM_TEST_QUIT_AT"))) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(e) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ app_quit("DM_TEST_QUIT_AT"); });
          // DM_TEST_TERMINATE_AT=<秒>：走一次系统退出（退出 Apple 事件，同程序坞「退出」）；DM_TEST_MINI_AT=<秒>[;<秒>…]：每个时刻切一次最小化（来回切）。
          // 游戏窗口本来不带「可最小化」（没有黄色按钮、没有 ⌘M）：这个开关只在测试时给窗口加上，验上面的最小化处理
          // DM_TEST_TERMINATE_AT=<seconds>: go through one system quit (the quit Apple event, same as Quit in the Dock); DM_TEST_MINI_AT=<seconds>[;<seconds>...]: toggle minimize at each time (back and forth).
          // The game window is not minimizable by itself (no yellow button, no Cmd-M): this switch adds that only for the test, to check the minimize handling above
          // 退出走主线程运行循环的公共模式（不排主队列）：模态弹框（如键位对照）的模态循环跑在主队列的一个块里，那时主队列不走，真的「程序坞退出」却照样进来
          // The quit goes through the main run loop's common modes (not the main queue): a modal dialog's (such as the key map's) modal loop runs inside a main-queue block, during which the main queue does not run, yet a real "Quit in the Dock" still comes in
          if ((e = getenv("DM_TEST_TERMINATE_AT"))) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(e) * NSEC_PER_SEC)), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
              CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ send_quit_event(); }); CFRunLoopWakeUp(CFRunLoopGetMain()); });
          if ((e = getenv("DM_TEST_MINI_AT"))) win.styleMask |= NSWindowStyleMaskMiniaturizable;
          if ((e = getenv("DM_TEST_MINI_AT"))) for (const char *q = e; q && *q; q = strchr(q, ';') ? strchr(q, ';') + 1 : NULL)
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(q) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ if (win.miniaturized) [win deminiaturize:nil]; else [win miniaturize:nil]; }); }
        // 生命周期（hle.h / dm.h）：失焦 → 2、拿回焦点 → 1；最小化 → 3 + 音频暂停 + GPU 挂起，还原 → 恢复（随后拿回焦点 → 1）
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        // 窗口被别的窗口整个盖住（遮挡状态不含 Visible）：系统不给看不见的窗口显示节拍回调。按「看不见」处理——出图直接丢（mtl_present_discard，
        // 同 iOS 进后台等存盘那段），游戏逻辑照走、声音照放、焦点不变；露出来就照常出图。不处理的话节拍停着、出图全靠兜底丢帧，日志报假红
        // （mini 整表被 Mac App 窗口盖住坐实）。DM_OCCL=0 关掉这项（对照用）；
        // DM_TEST_COVER=<秒>,<时长>：到点用一个不透明窗口把游戏窗口整个盖住，时长到了拿开（验收用，产品不设）
        // Window fully covered by other windows (occlusion state lacks Visible): the system does not deliver display-link callbacks to invisible windows. Treat it as "not visible": discard frames directly (mtl_present_discard,
        // same as the iOS background save-wait period); game logic keeps running, audio keeps playing, focus unchanged; once uncovered, present normally. Without this the display link stalls, presentation relies entirely on fallback frame drops, and the log reports false reds
        // (confirmed when a full regression table on the test Mac was covered by the Mac App window). DM_OCCL=0 disables this (for comparison);
        // DM_TEST_COVER=<seconds>,<duration>: at that time, cover the game window entirely with an opaque window and remove it after the duration (for acceptance testing, not set in the product)
        { const char *e = getenv("DM_OCCL"); int on = !(e && !strcmp(e, "0"));
          [nc addObserverForName:NSWindowDidChangeOcclusionStateNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n;
              int hidden = !(win.occlusionState & NSWindowOcclusionStateVisible); void mtl_present_discard(int on);
              LOG("前端：窗口%s（遮挡处理%s）", hidden ? "整个被盖住 / 不可见" : "露出来了", on ? "开" : "关 DM_OCCL=0");
              DM_EV("app", "state:s", hidden ? "occluded" : "visible");
              if (on) mtl_present_discard(hidden); }];
          double at = 0, dur = 0;
          if ((e = getenv("DM_TEST_COVER")) && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0)
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                  NSWindow *c = [[NSWindow alloc] initWithContentRect:NSInsetRect(win.frame, -40, -40) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                  c.opaque = YES; c.backgroundColor = NSColor.grayColor; c.releasedWhenClosed = NO; [c orderFrontRegardless];
                  LOG("DM_TEST_COVER：盖上 %.1f 秒，游戏窗口%s是主窗口", dur, win.isKeyWindow ? "" : "不");
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(dur * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [c orderOut:nil]; LOG("DM_TEST_COVER：拿开"); });
              }); }
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; DM_EV("app", "state:s", "resign"); if (!win.miniaturized && !atomic_load(&g_quitting)) dm_focus_set(2); g_btn = 0; g_w = g_a = g_s = g_d = 0; push(); { void dm_gamepad_reset(void); dm_gamepad_reset(); } }];   // 失焦：键盘和手柄按着的键清掉（松开事件收不到） | Focus lost: clear the keys held on keyboard and gamepad (their release events will not arrive)
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; DM_EV("app", "state:s", "active"); dm_focus_set(1); }];
        // 最小化：客体时钟一并冻结（同 iOS 进后台）。否则游戏线程挡在图形挂起处、墙钟照走，恢复时主循环把整段最小化时长的拍一口气补跑完——角色站着不动、怪照打（专家模式死了就没了）
        // Minimize: the guest clock is frozen as well (as when iOS goes to the background). Otherwise the game thread is blocked at the graphics suspend while the wall clock keeps running, and on restore the main loop catches up on every tick of the minimized period in one burst -- the character stands still while monsters keep attacking (a death in Hardcore is permanent)
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; void guest_clock_freeze_for(int reason, int on); uint64_t guest_ns(void);
            dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); LOG("窗口：最小化（客体时钟 %.3f s，冻结）", guest_ns() / 1e9); DM_EV("app", "state:s", "minimized"); void dm_game_state_background(int); dm_game_state_background(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; void guest_clock_freeze_for(int reason, int on); uint64_t guest_ns(void);
            dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); LOG("窗口：恢复（客体时钟 %.3f s）", guest_ns() / 1e9); DM_EV("app", "state:s", "restored"); void dm_game_state_background(int); dm_game_state_background(0); }];
        // 客体线程：栈 64 MiB（Switch 主线程栈由 npdm 指定，宿主给足）
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// 软键盘输入框（front.h）：用手柄玩时走手柄屏幕键盘（dm_vkbd_ui.m）；否则主线程在游戏窗口上弹 NSAlert 表单 + 文本框（标题缺省、按钮走字符串表），客体线程（游戏的键盘线程）等信号量拿结果。无窗口模式不弹（返回 -1，调用方按取消）。
// 表单不开模态循环：主队列照走，表单开着时游戏窗口的鼠标 / 滚轮不交给游戏（键盘本来就进键窗口 = 表单）。
// 两种可来回换，打了的字带过去：屏幕键盘开着时键盘按键 / 鼠标点击 → 收起换成表单；表单开着时手柄按键 → 关掉换成屏幕键盘（text_input_handoff_pad）。
// 框开着时退出（app_quit）先按「取消」放走键盘线程：开着的框冻着客体时钟（hle_swkbd.c），不放走一拍也走不了、存不了盘（同 iOS 壳 background_wait_save 关掉输入框）。
// Software keyboard input box (front.h): when played with a controller, the on-screen controller keyboard (dm_vkbd_ui.m); otherwise the main thread shows an NSAlert sheet + text field on the game window (default title and buttons from the string table); the guest thread (the game's keyboard thread) waits on a semaphore for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel).
// A sheet runs no modal loop: the main queue keeps running, and while the sheet is up the game window's mouse / wheel input is not passed to the game (typing already goes to the key window = the sheet).
// The two hand over to each other, keeping what was typed: a key press / mouse click while the on-screen keyboard is open → it closes and the sheet takes over; a controller press while the sheet is open → it closes and the on-screen keyboard takes over (text_input_handoff_pad).
// A quit while the box is up (app_quit) first answers "Cancel" to release the keyboard thread: the open box freezes the guest clock (hle_swkbd.c), so otherwise no tick runs and nothing can be saved (as the iOS shell's background_wait_save closes the input box).
@interface DmTextReq : NSObject <NSTextFieldDelegate> { @public dispatch_semaphore_t sem; int done, ok, kind, num_max; NSString *text; NSAlert *alert; } @end
@implementation DmTextReq
// 同 iOS 系统输入框，打字时就按游戏规则拦：起名只留英文字母 / 汉字、- 与 _ 合计一个、UTF-8 合计 15 字节，不合规则（太短、首尾是 - _）时「确定」置灰；
// 数量框只留数字、超过上限当场改成上限。输入法还在组字时不动，上屏后再量
// As the iOS system input box, the game's rules are enforced while typing: naming keeps only English letters / Chinese characters, one - or _ in total, 15 UTF-8 bytes, and greys out "OK" while the name breaks a rule (too short, starts or ends with - _);
// the quantity box keeps only digits and turns a value over the limit into the limit on the spot. Nothing changes while an input method is still composing; it is measured after commit
- (void)controlTextDidChange:(NSNotification *)n {
    NSTextField *f = n.object; if ([(NSTextView *)f.currentEditor hasMarkedText]) return;
    NSString *x = f.stringValue; NSMutableString *o = [NSMutableString string];
    if (kind == DM_VKBD_NUM) { long long v = 0;
        for (NSUInteger i = 0; i < x.length && o.length < 10; i++) { unichar c = [x characterAtIndex:i]; if (c >= '0' && c <= '9') { [o appendFormat:@"%C", c]; v = v * 10 + (c - '0'); } }
        if (v > num_max) [o setString:[NSString stringWithFormat:@"%d", num_max]]; }
    else if (kind == DM_VKBD_NAME) { int dash = 0; NSUInteger bytes = 0;
        for (NSUInteger i = 0; i < x.length; i++) { unichar c = [x characterAtIndex:i]; NSUInteger nb = 0;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) nb = 1;
            else if ((c == '-' || c == '_') && !dash) { nb = 1; dash = 1; }
            else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) nb = 3;   // 中日韩统一表意文字（基本区 + 扩展 A） | CJK Unified Ideographs (basic block + Extension A)
            if (!nb || bytes + nb > 15) continue;
            [o appendFormat:@"%C", c]; bytes += nb; } }
    else return;
    if (![o isEqualToString:x]) f.stringValue = o;
    if (kind == DM_VKBD_NAME) alert.buttons.firstObject.enabled = dm_vkbd_name_check(o.UTF8String) == DM_VKBD_HINT_NONE;   // 留空也能确定 = 随机取名 | OK also works when empty = random name
}
@end
static DmTextReq *g_text_req;   // 只在主线程读写：屏上那个输入框的请求 | read and written on the main thread only: the request of the box on screen
enum { SHEET_TO_PAD = 1100 };   // 表单收起的返回码：换成屏幕键盘 | sheet return code: hand over to the on-screen keyboard
static void text_req_finish(DmTextReq *q, int ok, NSString *text) {   // 主线程；只交一次结果（ok：1 确定 / 0 取消 / 2 换成屏幕键盘） | main thread; the result is handed over once (ok: 1 OK / 0 cancel / 2 hand over to the on-screen keyboard)
    if (q->done) return;
    q->done = 1; q->ok = ok; q->text = text; dispatch_semaphore_signal(q->sem);
}
static int text_input_up(void) { return g_text_req != nil; }   // 主线程：输入框在屏上 | main thread: the input box is on screen
static void text_input_test(NSString *text, int ok) {   // 测试（DM_TEST_NSMOUSE 的 sheet）：往开着的表单里填字（照打字时的规则过一遍）、按确定 / 取消，不靠 App 在前台 | test (DM_TEST_NSMOUSE sheet): fill the open sheet (passed through the same rules as typing) and press OK / Cancel, without needing the App in front
    DmTextReq *q = g_text_req; if (!q || q->done) { LOG("DM_TEST_NSMOUSE：sheet——没有开着的表单"); return; }
    NSTextField *f = (NSTextField *)q->alert.accessoryView; f.stringValue = text;
    [q controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification object:f]];
    NSButton *b = q->alert.buttons[ok ? 0 : 1];
    LOG("DM_TEST_NSMOUSE：sheet 填「%s」→「%s」，确定%s，按%s", text.UTF8String, f.stringValue.UTF8String, q->alert.buttons[0].enabled ? "可按" : "置灰", ok ? "确定" : "取消");
    if (b.enabled) [b performClick:nil];
}
static void text_input_handoff_pad(void) {   // dm_gamepad.m 的按键回调（主线程）：表单开着时手柄有按键 → 换成屏幕键盘 | dm_gamepad.m's press callback (main thread): a controller press while the sheet is open → hand over to the on-screen keyboard
    DmTextReq *q = g_text_req; if (q && !q->done) [g_win endSheet:q->alert.window returnCode:SHEET_TO_PAD];
}
static int text_input_cancel(void) {   // 主线程：有开着的输入框就按「取消」交结果并收起表单；返回 1 = 有 | main thread: if an input box is open, hand over "Cancel" and close the sheet; returns 1 if there was one
    DmTextReq *q = g_text_req; if (!q || q->done) return 0;
    text_req_finish(q, 0, nil); [g_win endSheet:q->alert.window returnCode:NSAlertSecondButtonReturn]; return 1;
}
static int text_sheet(const char *title, const char *sub, const char *guide, const char *initial, char *out, int out_cap) {   // 表单：1 确定 / 0 取消 / 2 换成屏幕键盘（out 是已打的字） | the sheet: 1 OK / 0 cancel / 2 hand over to the on-screen keyboard (out holds what was typed)
    DmTextReq *q = [DmTextReq new]; q->sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        extern int g_swkbd_name_panel, g_swkbd_numeric_max; q->num_max = g_swkbd_numeric_max; q->kind = q->num_max >= 0 ? DM_VKBD_NUM : g_swkbd_name_panel ? DM_VKBD_NAME : DM_VKBD_TEXT;
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @(ui(q->kind == DM_VKBD_NAME ? "dmNameTitle" : q->kind == DM_VKBD_NUM ? "dmQtyTitle" : "dmInputTitle"));
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        if (q->kind == DM_VKBD_NAME) info = info.length ? [info stringByAppendingFormat:@"\n%s", ui("dmNameRule")] : @(ui("dmNameRule"));
        if (q->kind == DM_VKBD_NUM) info = [NSString stringWithFormat:@"0 – %d", q->num_max];
        a.informativeText = info;
        [a addButtonWithTitle:@(ui("dmConfirm"))]; [a addButtonWithTitle:@(ui("dmCancel"))];
        a.buttons[1].keyEquivalent = @"\033";   // Esc = 「取消」（系统只给英文标题「Cancel」自动配 Esc） | Esc = "Cancel" (the system assigns Esc automatically only to the English title "Cancel")
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        f.stringValue = initial ? @(initial) : @"";
        if (q->kind == DM_VKBD_NAME) f.placeholderString = @(ui("dmNameBlankRandom")); else if (guide && *guide) f.placeholderString = @(guide);
        f.delegate = q;
        a.accessoryView = f; [a layout]; a.window.initialFirstResponder = f;
        f.target = a.buttons.firstObject; f.action = @selector(performClick:);   // 在输入框里按回车 = 点「确定」（明确接上，不靠系统把回车转给缺省按钮） | pressing Return in the input field = clicking "OK" (wired explicitly, not relying on the system forwarding Return to the default button)
        g_text_req = q; q->alert = a;
        [q controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification object:f]];   // 带过来的字（屏幕键盘交过来的）也按规则量一次 | text carried over (handed over by the on-screen keyboard) is checked by the rules once too
        [a beginSheetModalForWindow:g_win completionHandler:^(NSModalResponse m) {
            if (g_text_req == q) g_text_req = nil;
            text_req_finish(q, m == NSAlertFirstButtonReturn ? 1 : m == SHEET_TO_PAD ? 2 : 0, f.stringValue);   // 退出时已按取消交过结果的，这里不再交 | if the quit already handed over "Cancel", nothing more is handed over here
        }];
        // 表单出来后光标要在输入框里：只指定「初始第一响应者」不保险（模态框时实测第一响应者是弹框自己，要先点一下输入框才能打字），显式指定一次
        // The cursor must be in the input field once the sheet appears: specifying only the "initial first responder" is not reliable (measured with the modal dialog: the first responder was the dialog itself, and the field had to be clicked before typing), so it is specified explicitly once
        [a.window makeFirstResponder:f];
        LOG("前端：输入框已弹出（游戏窗口上的表单，%s）", a.window.visible ? "在屏上" : "还没上屏：窗口最小化着或另一个表单还开着");
    });
    dispatch_semaphore_wait(q->sem, DISPATCH_TIME_FOREVER);
    if (!q->ok) return 0;
    snprintf(out, out_cap, "%s", q->text.UTF8String);
    if (q->ok == 1 && q->kind == DM_VKBD_NAME && !*out && !dm_name_blank_random(out, out_cap)) return 0;   // 名字留空确定 = 随机取名（同屏幕键盘、iOS） | name left empty and confirmed = random name (as the on-screen keyboard and iOS)
    return q->ok;
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer || !g_win) return -1;
    // 用手柄玩：游戏画面上的屏幕键盘（dm_vkbd_ui.m）；用键盘鼠标：表单。中途换了设备就换另一种，已打的字带过去。截断按 UTF-16 单元在 hle_swkbd.c 统一做
    // Played with a controller: the on-screen keyboard over the game picture (dm_vkbd_ui.m); with keyboard and mouse: the sheet. Switching devices midway switches to the other, keeping what was typed. Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    char cur[1024]; snprintf(cur, sizeof cur, "%s", initial ? initial : "");
    for (int pad = dm_vkbd_wanted();; pad = !pad) {
        int r = pad ? dm_vkbd_input(title, cur, max_chars, out, out_cap) : text_sheet(title, sub, guide, cur, out, out_cap);
        if (r != 2) return r;
        snprintf(cur, sizeof cur, "%s", out);
        LOG("前端：输入框——%s（带过去「%s」）", pad ? "键盘 / 鼠标有输入，屏幕键盘换成表单" : "手柄有按键，表单换成屏幕键盘", cur);
    }
}

// 存档没写进去（front.h；宿主 hle_fs.c 在游戏线程调，盖过那里只记日志的弱实现）：同 iOS 壳，告诉玩家「存储空间不足，进度未保存」，只通知、不改存档流程。
// 在主线程弹成游戏窗口上的表单：不开模态循环，游戏线程不等它，主队列上别的事照走；已有一个在屏上就不再叠，只记一行。没有窗口（命令行没开 DM_WINDOW）只记日志
// Save not written (front.h; called by the host's hle_fs.c on the game thread, overriding the weak implementation there that only logs): like the iOS shell, tell the player 「存储空间不足，进度未保存」(Not enough storage. Progress was not saved) -- notify only, the save flow is unchanged.
// Shown on the main thread as a sheet on the game window: no modal loop, so the game thread does not wait for it and other main-queue work keeps going; if one is already on screen no second one is stacked, only a log line. Without a window (command line without DM_WINDOW) only a log line
static _Atomic int g_save_alert_on;
void front_save_failed(int err) {
    if (!g_win) { LOG("前端：存档写失败 errno=%d（没有窗口，只记日志）", err); return; }
    if (atomic_exchange(&g_save_alert_on, 1)) { LOG("前端：存档写失败 errno=%d，提示已在屏上，不再叠一个", err); return; }
    LOG("前端：存档写失败 errno=%d，异步提示玩家", err);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmSaveNoSpace")); [a addButtonWithTitle:@(ui("dmConfirm"))];
        [a beginSheetModalForWindow:g_win completionHandler:^(NSModalResponse r) { (void)r; atomic_store(&g_save_alert_on, 0); LOG("前端：存档失败提示已关"); }];
        LOG("前端：存档失败提示已显示 errno=%d（%s）", err, a.window.visible ? "在屏上" : "窗口最小化着，还原后出现");
    });
}
