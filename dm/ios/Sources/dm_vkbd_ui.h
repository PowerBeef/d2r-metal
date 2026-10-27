// 手柄屏幕键盘的前端部分（dm_vkbd_ui.m，Mac App 与 iOS 共用）：在游戏画面上画键盘、接手柄 / 实体键盘 / 点按，阻塞等结果。模型见 dm_vkbd.h。
// Front-end part of the on-screen controller keyboard (dm_vkbd_ui.m, shared by the Mac App and iOS): draws the keyboard over the game picture, takes controller / physical keyboard / click input, and blocks for the result. The model is in dm_vkbd.h.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#ifdef __cplusplus
extern "C" {
#endif

// 前端告诉键盘画在哪：layer = 加子层的 CALayer；pic = 游戏画面在这个层里的位置；y_up = 层坐标向上增（Mac 视图层）；scale = 像素 / 点
// The front end tells the keyboard where to draw: layer = the CALayer to add the sublayer to; pic = the game picture's rectangle in that layer; y_up = layer coordinates grow upward (Mac view layer); scale = pixels per point
typedef struct { void *layer; CGRect pic; int y_up; double scale; } dm_vkbd_host;
enum { DM_VKBD_KEY_TEXT, DM_VKBD_KEY_BACK, DM_VKBD_KEY_OK, DM_VKBD_KEY_CANCEL, DM_VKBD_KEY_LEFT, DM_VKBD_KEY_RIGHT, DM_VKBD_KEY_UP, DM_VKBD_KEY_DOWN };

void dm_vkbd_ui_set_host(void (*where)(dm_vkbd_host *h));   // 前端开机登记；在主线程调 | registered by the front end at startup; called on the main thread
int dm_vkbd_wanted(void);   // 该用屏幕键盘：手柄是当前在用的设备（DM_VKBD=1 / 0 强制开 / 关） | the on-screen keyboard should be used: the controller is the device in use (DM_VKBD=1 / 0 forces it on / off)
// 软键盘请求（front_text_input 在游戏的键盘线程调，不能是主线程）：弹键盘、等结果。返回 1 确定 / 0 取消 / 2 玩家改用键盘鼠标或触屏、交给前端的输入框（1、2 时 out 填好）
// Software keyboard request (front_text_input calls it on the game's keyboard thread, never the main thread): shows the keyboard and waits. Returns 1 OK / 0 cancel / 2 the player turned to keyboard and mouse or touch, hand over to the front end's input box (out filled for 1 and 2)
int dm_vkbd_input(const char *title, const char *initial, int max_chars, char *out, int out_cap);
// 以下都在主线程 | all of the following run on the main thread
int dm_vkbd_ui_active(void);
int dm_vkbd_ui_cancel(void);       // 有开着的就按取消收起，返回 1 | if one is open, close it as cancel and return 1
void dm_vkbd_ui_relayout(void);    // 画面位置变了（改窗口大小、进出全屏、转屏） | the picture moved (window resized, full screen toggled, rotation)
void dm_vkbd_ui_key(int key, const char *chars);   // 实体键盘（DM_VKBD_KEY_*；TEXT 带打的字） | physical keyboard (DM_VKBD_KEY_*; TEXT carries the typed characters)
// 名字留空确定 = 随机取名（dm_namegen.c 抽一个不重名的，只填进名字框、不自动接受，同 iOS 系统输入框）：写进 out 返回 1；抽不出返回 0（按取消）。任意线程
// Name left empty and confirmed = random name (dm_namegen.c draws one that collides with no character; it only fills the name box and is not auto-accepted, as the iOS system input box): written to out, returns 1; returns 0 if none could be drawn (treated as cancel). Any thread
int dm_name_blank_random(char *out, int cap);
void dm_vkbd_ui_handoff(void);     // 玩家改用键盘鼠标 / 触屏：收起，dm_vkbd_input 返回 2 带已打的字 | the player turned to keyboard and mouse / touch: close; dm_vkbd_input returns 2 with what was typed
CGImageRef dm_vkbd_ui_snapshot(CGRect *in_pic);     // 测试：面板图（调用方释放）+ 它在游戏画面里的位置（点，原点左上） | test: the panel image (caller releases) + its rectangle in the game picture (points, origin at top left)
CTFontRef dm_game_font(double size);                // 原版字体 BlizzardGlobal（运行时从资源包读，整个进程读一次，任意线程；还没读好时会当场读完再返回）；调用方释放，读不到 NULL | the original font BlizzardGlobal (read from the asset pack at run time, once per process, any thread; if not read yet it reads it there and then before returning); the caller releases it, NULL if unreadable
int dm_game_font_ready(void);                       // 原版字体读好了没有（读不到也算读好）：主线程据此决定先用系统字，免得在主线程上等 | whether the original fonts have been read (unreadable counts as read): the main thread uses a system font until then instead of waiting on the main thread
void dm_game_font_preload(void);                    // 在后台线程上读原版字体（只发一次，任意线程） | read the original fonts on a background thread (sent once, any thread)

#ifdef __cplusplus
}
#endif
