// 画面设置：Mac 选项页顶上「显示」「图形」两节（布局 touch_layout.py _VIDEO），存在自定义设置旁的 sw_video.txt。
// 三个值：mode（0 窗口 / 1 全屏）、res（出图宽档 0 = 1600 / 1 = 1920 / 2 = 2560）、up（0 标准拉伸 / 1 MetalFX），另存一份「推荐」rec_*。
// 首次启动（文件里一个键都没有；只缺 / 越界个别键时只补那几个，见 load_once）按这台 Mac 定推荐值并当作当前值（前端 front_video_firstboot：屏幕像素宽、显卡核数、MetalFX 支持；旧版偏好 StartFullScreen /
// RenderScale / MetalFXUpscale 设过的接着用）。「图形预设」是推导出来的：三项都等于推荐 = 推荐；分辨率 + 放大方式等于性能档 / 品质档 = 那一档；都不是 = 自定义。
// 页上照原版选项页的语义：切换框改的是待定值，「应用」才生效并存盘（显示模式当场切、放大方式下一帧起、分辨率下次启动——行下提示「重启游戏后生效」），
// 「放弃」丢掉待定值，「默认」待定值回推荐值。「有改动」位与自定义页合在一起（dm_custom.c 按 dm_video_dirty 一并点亮「应用」）。
// 切换框（CarouselWidget，宿主实读 3.3.94012）：选项个数 +0x7e8、当前项 +0x7ec、每项可选与否 +0x7d8 起每项一字节（0 = 跳过）；
// 设当前项走原版 main+0xab0c84(控件, 序号)（按个数钳、跳过不可选的项、按当前语言换上这一项的字）。读写前核虚表与选项个数。
// Picture settings: the "Display" and "Graphics" sections at the top of the Mac options page (layout touch_layout.py _VIDEO), stored in sw_video.txt next to the custom settings.
// Three values: mode (0 windowed / 1 full screen), res (render width tier 0 = 1600 / 1 = 1920 / 2 = 2560), up (0 standard stretch / 1 MetalFX), plus a stored "recommended" copy rec_*.
// On first boot (no key at all in the file; when only some keys are missing / out of range only those are filled, see load_once) the recommendation is worked out for this Mac and becomes the current value (front end front_video_firstboot: screen pixel width, GPU cores, MetalFX support; values from the
// older preferences StartFullScreen / RenderScale / MetalFXUpscale carry over when set). "Graphic Presets" is derived: all three equal to the recommendation = Recommended; resolution + upscaling equal to the
// Performance / Quality tier = that tier; otherwise = Custom.
// On the page it follows the original options page: a selector changes a pending value, only "Apply" makes it take effect and saves (display mode switches at once, upscaling from the next frame, resolution at the next
// launch -- with "Takes effect after restarting the game" under the row), "Discard" drops pending values, "Default" sets pending values back to the recommendation. The "has changes" bit is shared with the custom page
// (dm_custom.c lights "Apply" from dm_video_dirty too).
// Selector (CarouselWidget, read on the host, 3.3.94012): option count +0x7e8, current option +0x7ec, one byte per option from +0x7d8 saying whether it can be chosen (0 = skipped);
// the current option is set through the game's main+0xab0c84(widget, index) (clamps to the count, skips options that cannot be chosen, puts that option's text in the current language). The vtable and option count are checked before reading or writing.
// 手机（DM_PHONE，iOS 覆盖层 DM_OVERLAY_IOS）：页上只有「图形」一节——res 是 3D 清晰度（0.75 / 0.85 / 1.0，交给 hle_oe.c 的 3D 比例，点应用当场换），
// up 是游戏 3D 放大趟用不用 MetalFX（nvn_mtl.m mfx3d_draw），没有显示模式那一行；预设：推荐 = 首次启动按机型定，性能 = 75% 标准（最省电），品质 = 100% + MetalFX。
// 页上有哪几行就管哪几行（dm_video_rows 按实际找到的切换框排）
// 两个平台的「图形」节末尾还有「帧率」（60 / 30 省电）与「掉帧时」（降低 / 保持 3D 清晰度）：值存在自定义设置里（eco_fps / q3d，dm_custom.c 读写、存 sw_custom.txt），
// 这里只管页上的切换框——待定值、应用、放弃都走自定义设置那一套；图形预设不管它们，「默认」把它们回到缺省（60 帧、降低）
// Phones (DM_PHONE, iOS overlay DM_OVERLAY_IOS): the page only has the "Graphics" section -- res is the 3D resolution (0.75 / 0.85 / 1.0, handed to the 3D scale in hle_oe.c, switching as soon as Apply is pressed),
// up is whether the game's 3D upscale pass uses MetalFX (nvn_mtl.m mfx3d_draw), and there is no display mode row; presets: Recommended = chosen per device on first boot, Performance = 75% Standard (lightest on the battery), Quality = 100% + MetalFX.
// Only the rows the page actually has are handled (dm_video_rows lists the selectors it finds)
// On both platforms the Graphics section ends with Frame Rate (60 / 30 power saver) and Frame Drops (lower / keep the 3D resolution): their values live in the custom settings (eco_fps / q3d, read and written by
// dm_custom.c, saved in sw_custom.txt), and this file only drives the selectors -- pending values, Apply and Discard go through the custom settings; the graphic presets leave them alone, and "Default" puts them back to their defaults (60 fps, lower)
// Mac 另有三行画质（画质计划第 1 阶段）：「抗锯齿」aa（0 FXAA = 原版 / 1 SMAA，游戏自己电脑版的那一档：宿主 hle_oe.c aa_tick 写渲染器的档位字节）、
// 「各向异性过滤」aniso（0 原版 / 1 4× / 2 8× / 3 16×，nvn_mtl.m 采样器覆盖）、「色彩」color（0 鲜艳 = 原先不标色彩空间，宽色域屏把 sRGB 当屏幕原色撑开 / 1 准确 = 层标 sRGB 交给系统配色）。
// 三项都点应用即生效、都算进「推荐」；值 0 都是原先的样子，所以测试副本、命令行宿主、手机（页上没有这几行）一概不变。旧文件没有这几个键：读盘时按前端 front_video_rec 补
// The Mac has three more picture rows (picture plan phase 1): "Anti-Aliasing" aa (0 FXAA = the original / 1 SMAA, the game's own PC setting: the host's hle_oe.c aa_tick writes the renderer's mode byte),
// "Anisotropic Filtering" aniso (0 original / 1 4x / 2 8x / 3 16x, the sampler override in nvn_mtl.m) and "Colour" color (0 Vivid = no colour space on the layer as before, so wide-gamut panels stretch sRGB to
// the panel's primaries / 1 Accurate = the layer is tagged sRGB and the system colour-matches it).
// All three take effect on Apply and count towards "Recommended"; value 0 is always the old look, so test copies, the CLI host and phones (whose page lacks these rows) are unchanged. Older files lack these keys: loading fills
// them from the front end's front_video_rec
// Mac 的原生出图与 3D 清晰度（画质计划第 3 阶段）：「分辨率」多一档「原生」（res=3：主屏的像素宽，16 对齐，1600–3840），界面与文字按屏幕原生分辨率画；
// 「3D 清晰度」s3d（0 = 100% 原先的样子 / 1 = 85% / 2 = 75% / 3 = 67% / 4 = 50%）只降 3D，游戏自己的 3D 放大趟把它放回出图尺寸（开 MetalFX 时由 MetalFX 放大，nvn_mtl.m mfx3d_draw）。
// 推荐值有版本（rec_v）：版本旧的文件读盘时按这一版重算推荐值；玩家还停在旧推荐上（各键都等于旧推荐）就一起换成新推荐，改过的照留
// Native output and 3D resolution on the Mac (picture plan phase 3): "Resolution" gains a "Native" tier (res=3: the main screen's pixel width, 16-aligned, 1600–3840), so the interface and text are drawn at
// the screen's own resolution; "3D Resolution" s3d (0 = 100%, the old look / 1 = 85% / 2 = 75% / 3 = 67% / 4 = 50%) lowers only the 3D, which the game's own 3D upscale pass brings back to the output size
// (with MetalFX when Upscaling is MetalFX, nvn_mtl.m mfx3d_draw).
// The recommendation has a version (rec_v): a file with an older version gets this version's recommendation at load; a player still on the old recommendation (every key equal to it) moves to the new one
// with it, while changed settings stay
#include "hle.h"
#include "dm.h"
#include "dm_cursor.h"
#include "dm_widget.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t dm_custom_find_w(uint64_t w, const char *name);
int dm_custom_shown(void);
int dm_custom_path(const char *name, char *p, size_t n);
enum { K_MODE, K_RES, K_UP, K_AA, K_ANISO, K_COLOR, K_S3D, NV };   // 新键只往后加（存盘按名字，旧文件缺的键读盘时补） | new keys go at the end (saved by name; keys an older file lacks are filled at load)
static const char *const k_keys[NV] = { "mode", "res", "up", "aa", "aniso", "color", "s3d" };
static const int k_n[NV] = { 2, 4, 2, 2, 4, 2, 5 };
#define REC_V 2   // 推荐值的版本：2 = 原生出图 + 3D 清晰度（Mac） | version of the recommendation: 2 = native output + 3D resolution (Mac)
static int kn(int i) { return i == K_RES && DM_PHONE ? 3 : k_n[i]; }   // 手机的 res 是三档 3D 清晰度；Mac 的 res 多一档「原生」 | on phones res is the three 3D-resolution tiers; on the Mac res has the extra "Native" tier
#define NR 10
static const char *const k_rows[NR] = { "mode", "res", "color", "preset", "s3d", "upscale", "aa", "aniso", "fps", "drops" };   // 页上的行序（页上没有的行跳过） | row order on the page (rows the page lacks are skipped)
static const int k_row_key[NR] = { K_MODE, K_RES, K_COLOR, -1, K_S3D, K_UP, K_AA, K_ANISO, -2, -2 };   // -1 = 图形预设（推导）；-2 = 自定义设置里的键（k_row_ckey） | -1 = graphic presets (derived); -2 = a custom settings key (k_row_ckey)
static const char *const k_row_ckey[NR] = { 0, 0, 0, 0, 0, 0, 0, 0, "eco_fps", "q3d" };
static int row_n(int i) { return k_row_key[i] >= 0 ? kn(k_row_key[i]) : k_row_key[i] == -1 ? 4 : 2; }   // 切换框的选项个数：键的档数；预设 4；自定义设置的键 2 | options in the selector: the key's tiers; presets 4; custom settings keys 2   // 切换框第 0 / 1 项 = 键值 0 / 1（60 帧 / 30 帧；降低 / 保持） | selector options 0 / 1 = key values 0 / 1 (60 / 30 fps; lower / keep)
int dm_custom_shown_value(const char *key); void dm_custom_set_pending(const char *key, int v); int dm_custom_default(const char *key);
static _Atomic int g_crows;   // 页上有的自定义键行（按行号的位）：「默认」只回这几行 | the custom-key rows the page has (one bit per row): "Default" resets only those
enum { P_REC, P_PERF, P_QUAL, P_CUSTOM };
static _Atomic int g_val[NV], g_pend[NV] = { -1, -1, -1, -1, -1, -1, -1 }, g_rec[NV], g_loaded, g_repaint;
_Static_assert(NV == 7, "g_pend 的初值个数 | g_pend initialiser count");
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // 读盘 / 存盘：前端主线程（启动、全屏切换）与游戏线程（应用）都会写 | load / save: written by both the front end's main thread (launch, full-screen changes) and the game thread (Apply)

// 前端提供（Mac：front_mac.m）；别的平台没有这一节，用这里的弱定义 | Provided by the front end (Mac: front_mac.m); other platforms have no such section and use the weak definitions here
__attribute__((weak)) int front_video_firstboot(int rec[3], int cur[3]) { (void)rec; (void)cur; return 0; }
__attribute__((weak)) void front_video_fullscreen(int on) { (void)on; }
__attribute__((weak)) int front_video_running_res(void) { return -1; }
__attribute__((weak)) int front_video_mfx_ok(void) { return 0; }
__attribute__((weak)) int front_video_rec(const char *key) { (void)key; return -1; }   // front_video_firstboot 之后的键的推荐值；-1 = 没有（取 0，原先的样子） | recommendation for keys after front_video_firstboot's three; -1 = none (0, the old look)
__attribute__((weak)) void front_video_color(int accurate) { (void)accurate; }   // 「色彩」点了应用 | "Colour" applied

static const char *vals_str(const int *v) {   // 「mode=1 res=1 …」：日志用（非线程安全的静态缓冲，只在 g_mu 下或启动时用） | "mode=1 res=1 ...": for logs (static buffer, not thread-safe; only used under g_mu or at launch)
    static char b[160]; size_t L = 0; b[0] = 0;
    for (int i = 0; i < NV && L < sizeof b; i++) L += (size_t)snprintf(b + L, sizeof b - L, "%s%s=%d", i ? " " : "", k_keys[i], v[i]);
    return b;
}
static void save_locked(void) {
    char p[1100], t[1120]; if (!dm_custom_path("sw_video.txt", p, sizeof p)) { LOG("【红】画面设置：设置目录还不知道，没存盘"); return; }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); int ok = f != NULL;
    for (int i = 0; ok && i < NV; i++) ok = fprintf(f, "%s=%d\nrec_%s=%d\n", k_keys[i], atomic_load(&g_val[i]), k_keys[i], atomic_load(&g_rec[i])) > 0;
    if (ok) ok = fprintf(f, "rec_v=%d\n", REC_V) > 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(t, p) == 0;   // 先写临时文件再换名（同 sw_custom.txt） | write a temporary file, then rename (as sw_custom.txt)
    int v[NV], r[NV]; for (int i = 0; i < NV; i++) { v[i] = atomic_load(&g_val[i]); r[i] = atomic_load(&g_rec[i]); }
    char rs[160]; strlcpy(rs, vals_str(r), sizeof rs);
    LOG("画面设置：存盘%s %s（推荐 %s）", ok ? "" : "【红】失败", vals_str(v), rs);
}
static void load_once(void) {
#ifndef __APPLE__
    return;   // 安卓 / 鸿蒙的覆盖层没有这几行：不读不写 sw_video.txt，取值全为 0，3D 比例照旧按各自的缺省 | the Android / HarmonyOS overlays have none of these rows: sw_video.txt is never read or written, every value reads 0, and the 3D scale keeps each platform's own default
#endif
    if (atomic_load(&g_loaded)) return;
    pthread_mutex_lock(&g_mu);
    char p[1100];
    if (!atomic_load(&g_loaded) && dm_custom_path("sw_video.txt", p, sizeof p)) {
        // 逐键读：缺的、越界的那一个键才按首次启动的值补，其余照读到的用。原先任一 rec_* 缺 / 越界就整份当首次启动，玩家设好的三项全被推荐值盖掉
        // Read key by key: only a missing or out-of-range key is filled from the first-boot values, the rest keep what was read. Before, one missing / out-of-range rec_* treated the whole file as a first boot and the recommendation overwrote all three of the player's settings
        int v[NV], r[NV], rv_file = 1; for (int i = 0; i < NV; i++) v[i] = r[i] = -1;   // rv_file：文件里的推荐版本（没写 = 1） | rv_file: the recommendation version in the file (absent = 1)
        FILE *f = fopen(p, "r"); char ln[64];
        while (f && fgets(ln, sizeof ln, f)) { char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0; int x = atoi(eq + 1), rec = !strncmp(ln, "rec_", 4);
            for (int i = 0; i < NV; i++) if (!strcmp(rec ? ln + 4 : ln, k_keys[i]) && x >= 0 && x < kn(i)) { if (rec) r[i] = x; else v[i] = x; }
            if (!strcmp(ln, "rec_v")) rv_file = x; }
        if (f) fclose(f);
        int miss = 0, first = 1; for (int i = 0; i < NV; i++) { miss |= (v[i] < 0) << i | (r[i] < 0) << (i + NV); first &= v[i] < 0 && r[i] < 0; }   // first = 一个键都没读到（真首次启动） | first = not a single key read (a real first boot)
        int bump = !first && rv_file < REC_V, s3d_missing = v[K_S3D] < 0;   // 推荐值版本旧了：按这一版重算 | the recommendation is from an older version: work it out again
        if (miss || bump) {   // 定推荐值（前端没有 = 原版：窗口、1600、标准），只补缺的键 | work out the recommendation (no front end = the original: windowed, 1600, standard), filling only the missing keys
            int rec[NV] = { 0 }, cur[NV] = { 0 };   // 前三个键前端一起定（front_video_firstboot），之后的键一个个问（front_video_rec，当前值 = 推荐） | the front end sets the first three together (front_video_firstboot), later keys one by one (front_video_rec, current = recommended)
            if (!front_video_firstboot(rec, cur)) memcpy(cur, rec, 3 * sizeof cur[0]);
            for (int i = 3; i < NV; i++) { int x = front_video_rec(k_keys[i]); rec[i] = cur[i] = x >= 0 && x < kn(i) ? x : 0; }
            if (bump) {   // 还停在旧推荐上（读到的键都等于旧推荐）：一起换成新推荐；改过的照留，只换推荐 | still on the old recommendation (every key read equals it): move to the new one; changed settings stay, only the recommendation changes
                int on_rec = 1; for (int i = 0; i < NV; i++) if (v[i] >= 0 && r[i] >= 0 && v[i] != r[i]) on_rec = 0;
                for (int i = 0; i < NV; i++) { int nr = rec[i] >= 0 && rec[i] < kn(i) ? rec[i] : 0; if (on_rec && v[i] >= 0) v[i] = nr; r[i] = nr; }
                LOG("画面设置：推荐值第 %d 版 → 第 %d 版，%s | picture settings: recommendation version %d → %d, %s", rv_file, REC_V, on_rec ? "原先停在推荐上，一起换成新推荐" : "改过的设置照留，只换推荐",
                    rv_file, REC_V, on_rec ? "was on the recommendation, moved to the new one" : "changed settings kept, only the recommendation updated");
            }
            for (int i = 0; i < NV; i++) { int rv = rec[i] >= 0 && rec[i] < kn(i) ? rec[i] : 0, had_r = r[i] >= 0;
                if (r[i] < 0) r[i] = rv;
                if (v[i] < 0) v[i] = had_r ? r[i] : cur[i] >= 0 && cur[i] < kn(i) ? cur[i] : r[i]; }   // 当前值缺：有存着的推荐就回推荐，都缺才按首次启动的当前值（含旧版偏好） | current value missing: back to the stored recommendation; only when both are missing, the first-boot current value (incl. older preferences)
            if (!first && s3d_missing && v[K_RES] != 3) v[K_S3D] = 0;   // 3D 清晰度只配原生出图：没用原生的老设置补 100%（原先的样子） | 3D resolution only goes with native output: older settings not on Native get 100% (the old look)
        }
        for (int i = 0; i < NV; i++) { atomic_store(&g_val[i], v[i]); atomic_store(&g_rec[i], r[i]); }
        atomic_store(&g_loaded, 1);
        if (miss && !first) { char mk[128] = ""; for (int i = 0; i < 2 * NV; i++) if (miss >> i & 1) snprintf(mk + strlen(mk), sizeof mk - strlen(mk), "%s%s%s", mk[0] ? " " : "", i >= NV ? "rec_" : "", k_keys[i % NV]);
            LOG("画面设置：%s 缺 / 越界的键 %s → 只补这几个（其余照读）", p, mk); }
        { char rs[160]; strlcpy(rs, vals_str(r), sizeof rs); LOG("画面设置：%s %s %s，推荐 %s", first ? "首次启动定推荐值" : miss ? "读盘（补过缺的键）" : "读盘", p, vals_str(v), rs); }
        if (miss || bump) save_locked();   // 补齐 / 换了推荐的整份写回 | write the completed / updated file back
    }
    pthread_mutex_unlock(&g_mu);
}
int dm_video_get(const char *key) {   // 生效值；设置目录还不知道时答原版值 | the value in effect; the original value while the settings directory is unknown
    load_once(); for (int i = 0; i < NV; i++) if (key && !strcmp(key, k_keys[i])) return atomic_load(&g_loaded) ? atomic_load(&g_val[i]) : 0;
    return 0;
}
int dm_video_upscale(void) { return dm_video_get("up"); }
int dm_video_aa_mode(void) { return dm_video_get("aa") == 1 ? 2 : 1; }   // 游戏渲染器的抗锯齿档位字节（hle_oe.c aa_tick 每拍读）：1 FXAA（原版）/ 2 SMAA 1x | the game renderer's AA mode byte (read every tick by aa_tick in hle_oe.c): 1 FXAA (original) / 2 SMAA 1x
int dm_video_aniso(void) { static const int k[4] = { 0, 4, 8, 16 }; int a = dm_video_get("aniso"); return a >= 0 && a < 4 ? k[a] : 0; }   // 提交开头读（nvn_mtl.m aniso_latch）；0 = 原版 | read at the start of each submission (nvn_mtl.m aniso_latch); 0 = the original
float dm_video_scale3d(void) {   // 3D 清晰度（hle_oe.c s3_cfg 每拍读；> 0 才算数）：手机按 res（75 / 85 / 100%），Mac 按 s3d（0 = 100% 答 0，照原先） | the 3D resolution (read every tick by s3_cfg in hle_oe.c; only > 0 counts): phones from res (75 / 85 / 100%), the Mac from s3d (0 = 100% answers 0, as before)
    static const float k[3] = { 0.75f, 0.85f, 1.0f }, m[5] = { 0, 0.85f, 0.75f, 0.67f, 0.5f };
    load_once(); if (!atomic_load(&g_loaded)) return 0;
    if (DM_PHONE) { int t = atomic_load(&g_val[K_RES]); return t >= 0 && t < 3 ? k[t] : 0; }
    int t = atomic_load(&g_val[K_S3D]); return t > 0 && t < 5 ? m[t] : 0;
}   // 出图每帧读（nvn_mtl.m mfx_on） | read by the present every frame (nvn_mtl.m mfx_on)
void dm_video_fullscreen_note(int on) {   // 玩家用菜单 / ⌃⌘F 切了全屏：设置跟着窗口走（同电脑版「显示模式」） | the player switched full screen from the menu / ⌃⌘F: the setting follows the window (as the PC "Display Mode")
    load_once(); if (!atomic_load(&g_loaded) || atomic_load(&g_val[K_MODE]) == (on != 0)) return;
    pthread_mutex_lock(&g_mu); atomic_store(&g_val[K_MODE], on != 0); save_locked(); pthread_mutex_unlock(&g_mu);
    atomic_store(&g_repaint, 1);
}
static int shown(int k) { int p = atomic_load(&g_pend[k]); return p >= 0 ? p : atomic_load(&g_val[k]); }
static int perf_up(void) { return DM_PHONE ? 0 : front_video_mfx_ok() ? 1 : 0; }   // 性能档的放大方式：手机 = 标准（省电），Mac = MetalFX（它很便宜） | Performance's upscaling: phone = Standard (saves power), Mac = MetalFX (cheap there)
// 性能 / 品质档各键的值（t 0 = 性能，1 = 品质）；-1 = 这一档不管这个键。抗锯齿、各向异性只在 Mac 上归档管（手机页上没有这两行，档位不该暗改它们）
// Each key's value in the Performance / Quality presets (t 0 = Performance, 1 = Quality); -1 = the preset leaves this key alone. Anti-aliasing and anisotropy belong to the presets on the Mac only (the phone page has no such rows,
// so a preset must not change them behind the player's back)
static int tier_of(int k, int t) {
    switch (k) {
    case K_RES: return t ? (DM_PHONE ? 2 : 3) : 0;   // 品质：手机 100% 3D，Mac 原生出图 | Quality: 100% 3D on phones, native output on the Mac
    case K_UP: return t ? (front_video_mfx_ok() ? 1 : 0) : perf_up();
    case K_AA: return DM_PHONE ? -1 : t;
    case K_ANISO: return DM_PHONE ? -1 : t ? 3 : -1;
    case K_S3D: return DM_PHONE ? -1 : 0;   // Mac：性能（出图 1600）与品质（原生）的 3D 都是 100% | Mac: both Performance (1600 output) and Quality (native) draw the 3D at 100%
    default: return -1;
    }
}
static int preset_of(void) {
    int rec = 1; for (int k = 0; k < NV; k++) rec &= shown(k) == atomic_load(&g_rec[k]);
    if (rec) return P_REC;
    for (int t = 0; t < 2; t++) { int m = 1; for (int k = 0; k < NV; k++) { int x = tier_of(k, t); if (x >= 0) m &= shown(k) == x; } if (m) return t ? P_QUAL : P_PERF; }
    return P_CUSTOM;
}
static void apply_preset(int p) {   // 选了某档：改待定值（推荐 = 全部回推荐；性能 / 品质只管 tier_of 列的键） | a preset was chosen: change pending values (Recommended = everything back to the recommendation; Performance / Quality only the keys tier_of lists)Quality set only resolution and upscaling)
    if (p == P_REC) for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], atomic_load(&g_rec[i]));
    else if (p == P_PERF || p == P_QUAL) for (int k = 0; k < NV; k++) { int x = tier_of(k, p == P_QUAL); if (x >= 0) atomic_store(&g_pend[k], x); }
}
int dm_video_dirty(void) { int d = 0; for (int i = 0; i < NV; i++) { int p = atomic_load(&g_pend[i]); d |= p >= 0 && p != atomic_load(&g_val[i]); } return d; }
void dm_video_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (!cat || !act || strcmp(cat, "OptionsPanelMessage")) return;
    load_once(); if (!atomic_load(&g_loaded)) return;
    if (!strcmp(act, "AcceptChanges")) { int ch = 0, mode = -1, color = -1;
        for (int i = 0; i < NV; i++) { int p = atomic_exchange(&g_pend[i], -1); if (p >= 0 && p != atomic_load(&g_val[i])) { atomic_store(&g_val[i], p); ch = 1; if (i == K_MODE) mode = p; if (i == K_COLOR) color = p; } }
        if (ch) { pthread_mutex_lock(&g_mu); save_locked(); pthread_mutex_unlock(&g_mu); }
        if (mode >= 0) front_video_fullscreen(mode);   // 显示模式当场切 | display mode switches at once
        if (color >= 0) front_video_color(color);   // 色彩当场换（抗锯齿、各向异性由各自读设置的地方下一帧接上） | colour changes at once (anti-aliasing and anisotropy follow from where they read the setting, next frame)
        atomic_store(&g_repaint, 1); }
    else if (!strcmp(act, "CancelChanges")) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1); atomic_store(&g_repaint, 1); }
    // 「默认」只管当前页签那一节（「附加」页签上不动画面设置，同原版各页签各管各的） | Default covers only the current tab's section (on the "Extras" tab it leaves the picture settings alone, as each original tab covers its own)
    else if (!strcmp(act, "ResetToDefaults") && (dm_custom_shown() & 1)) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], atomic_load(&g_rec[i]));
        for (int i = 0; i < NR; i++) if (k_row_ckey[i] && (atomic_load(&g_crows) >> i & 1)) dm_custom_set_pending(k_row_ckey[i], dm_custom_default(k_row_ckey[i]));
        atomic_store(&g_repaint, 1); LOG("画面设置：默认 → 推荐值、帧率与掉帧时回缺省（点「应用」生效）"); }
}

// ---- 页上的切换框
// ---- Selectors on the page
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
DM_WIDGET_RED_DEF
static uint64_t row_w(uint64_t sp, int i) { char nm[48]; snprintf(nm, sizeof nm, "TouchVideo_%s", k_rows[i]); return dm_custom_find_w(sp, nm); }
static int car_ok(uint64_t w, int n) {
    if (!w || !*(uint8_t *)(w + 0x50) || !DM_WIDGET_OK(main_base(), w, DM_VT_CAROUSEL, "画面设置")) return 0;
    if (*(int32_t *)(w + 0x7e8) != n) { static int k; if (k++ < 3) LOG("【红】画面设置：切换框「%s」选项个数 %d，应为 %d，不读写", *(const char **)(w + 8), *(int32_t *)(w + 0x7e8), n); return 0; }
    return 1;
}
static int car_get(uint64_t w) { return *(int32_t *)(w + 0x7ec); }
static void car_set(uint64_t w, int i) { if (car_get(w) != i) ((void (*)(uint64_t, int))(main_base() + 0xab0c84))(w, i); }
static void preset_enable(uint64_t w, int custom) { *(uint8_t *)(w + 0x7d8 + P_CUSTOM) = custom != 0; }   // 「自定义」只在不对应任何一档时可选（箭头跳过它） | "Custom" can only be chosen when no preset matches (the arrows skip it)
static int present_rows(uint64_t sp, int idx[NR]) {   // 页上实际有的行（按页上的顺序）：Mac 六行，手机没有显示模式 | the rows the page really has (in page order): six on the Mac, no display mode on phones
    int n = 0; for (int i = 0; i < NR && sp; i++) { uint64_t w = row_w(sp, i); if (w && *(uint8_t *)(w + 0x50)) idx[n++] = i; } return n;
}
int dm_video_rows(uint64_t sp) { int idx[NR]; return present_rows(sp, idx); }
void dm_video_row(uint64_t sp, int i, uint64_t *tw, uint64_t *cw) {   // 手柄选中框：第 i 个画面设置行的标签与切换框 | controller selection frame: label and selector of the i-th picture row
    int idx[NR], n = present_rows(sp, idx); *tw = *cw = 0; if (i < 0 || i >= n) return;
    char nm[48]; snprintf(nm, sizeof nm, "TouchVideo_%s_Text", k_rows[idx[i]]); *tw = dm_custom_find_w(sp, nm); *cw = row_w(sp, idx[i]);
}
void dm_video_pad(uint64_t sp, int r, int dir) {   // 手柄：左右 = 上一项 / 下一项，A = 下一项（到头回第一项）；之后 dm_video_pump 当玩家改了处理 | controller: left / right = previous / next option, A = next (wrapping to the first); dm_video_pump then handles it as a player change
    int idx[NR], nr = present_rows(sp, idx); if (r < 0 || r >= nr) return; int i = idx[r];
    uint64_t w = row_w(sp, i); if (!car_ok(w, row_n(i))) return;
    int n = row_n(i), cur = car_get(w), to = cur;
    for (int k = 0; k < n; k++) { to = dir == 2 ? (to + 1) % n : to + dir; if (to < 0 || to >= n) { to = cur; break; } if (*(uint8_t *)(w + 0x7d8 + to)) break; }
    car_set(w, to);
}
static void hint(uint64_t sp) {   // 分辨率改了还没重启：「重启游戏后生效」挪到标签下面（横坐标取标签的；同语言行的提示） | resolution changed but not restarted yet: move "Takes effect after restarting the game" under the label (x from the label; as the language row's hint)
    uint64_t hw = dm_custom_find_w(sp, "TouchVideoResHint"), lw = dm_custom_find_w(sp, "TouchVideo_res_Text"); if (!hw || !lw) return;
    int run = front_video_running_res(), x = run >= 0 && shown(K_RES) != run ? ((int32_t *)(lw + 0x70))[0] : 99999; int32_t *r = (int32_t *)(hw + 0x70);
    if (r[0] != x) { r[0] = x; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)hw + 0x10))(hw); }
}
static int row_want(int i) { return k_row_ckey[i] ? dm_custom_shown_value(k_row_ckey[i]) : k_row_key[i] < 0 ? preset_of() : shown(k_row_key[i]); }   // 这一行该显示哪一项 | the option this row should show
void dm_video_pump(uint64_t sp) {   // 自定义页在屏上时每帧调（dm_custom.c）；sp = 0：页不在屏上 | called every frame while the custom page is on screen (dm_custom.c); sp = 0: the page is not on screen
    static dm_wref seen[NR];
    uint64_t w[NR] = { 0 }; int ok = sp != 0 && dm_video_rows(sp) > 0;
    for (int i = 0; ok && i < NR; i++) { w[i] = row_w(sp, i); if (w[i] && !*(uint8_t *)(w[i] + 0x50)) w[i] = 0; }   // 页上没有的行 = 0，下面跳过 | rows the page lacks = 0, skipped below
    if (!ok) { int was = 0; for (int i = 0; i < NR; i++) was |= seen[i].w != 0;   // 页没了：没点「应用」的改动作废（同自定义页） | the page is gone: changes without "Apply" are dropped (as the custom page)
        if (was) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1); for (int i = 0; i < NR; i++) if (k_row_ckey[i]) dm_custom_set_pending(k_row_ckey[i], -1); }
        memset(seen, 0, sizeof seen); atomic_store(&g_crows, 0); return; }
    for (int i = 0; i < NR; i++) if (w[i] && !car_ok(w[i], row_n(i))) return;
    { int cr = 0; for (int i = 0; i < NR; i++) if (w[i] && k_row_ckey[i]) cr |= 1 << i; atomic_store(&g_crows, cr); }
    load_once(); if (!atomic_load(&g_loaded)) return;
    if (atomic_exchange(&g_repaint, 0)) memset(seen, 0, sizeof seen);
    int want[NR], ch = -1;   // 先按改之前的值算各行该显示什么，再找玩家动了哪一行（一帧只认一行）：原先边改边比，预设刚改的待定值被后面几行当成玩家又改回去了
                            // first work out what each row should show from the values before any change, then find the row the player moved (one per frame): comparing while changing used to let the later rows read a preset's fresh pending values as the player changing them back
    for (int i = 0; i < NR; i++) want[i] = row_want(i);
    for (int i = 0; i < NR; i++) {
        if (!w[i]) continue;
        if (dm_wref_get(&seen[i]) != w[i]) { dm_wref_set(&seen[i], w[i]); if (k_row_key[i] == -1) preset_enable(w[i], want[i] == P_CUSTOM); car_set(w[i], want[i]); continue; }   // 页刚出现：摆成该显示的值，这一帧不当玩家改了 | the page just appeared: show the value it should, and do not treat this frame as a player change
        if (ch < 0 && car_get(w[i]) != want[i]) ch = i;
    }
    if (ch >= 0) {
        int k = k_row_key[ch], cur = car_get(w[ch]);
        if (k_row_ckey[ch]) { dm_custom_set_pending(k_row_ckey[ch], cur); LOG("画面设置：%s → %d（自定义设置里的键；点「应用」生效）", k_row_ckey[ch], cur); }
        else if (k < 0) { if (cur != P_CUSTOM) apply_preset(cur); LOG("画面设置：图形预设 → %d（点「应用」生效）", cur); }
        else { atomic_store(&g_pend[k], cur); LOG("画面设置：%s → %d（点「应用」生效）", k_keys[k], cur); }
        for (int i = 0; i < NR; i++) { if (!w[i]) continue; int v = row_want(i); if (k_row_key[i] == -1) preset_enable(w[i], v == P_CUSTOM); car_set(w[i], v); }   // 预设带动其余几项、其余几项带动预设 | presets move the other rows, and the other rows move the preset
    }
    hint(sp);
}
