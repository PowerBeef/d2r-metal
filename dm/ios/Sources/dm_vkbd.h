// 手柄屏幕键盘（用手柄玩时代替系统输入框）的纯 C 模型：键位、焦点移动、编辑、角色名规则 / 数量上限、手柄键的连发，以及面板几何（画和点都按它）。
// 画面与输入由 dm_vkbd_ui.m 接（Mac App、iOS 共用）；单测 dm/ios/tests/t_vkbd.c。
// Pure C model of the on-screen controller keyboard (replaces the system input box when the game is played with a controller): key layout, focus movement, editing, the character name rule / quantity limit, button repeat, and the panel geometry (drawing and clicks both use it).
// Drawing and input are handled by dm_vkbd_ui.m (shared by the Mac App and iOS); unit test dm/ios/tests/t_vkbd.c.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DM_VKBD_TEXT, DM_VKBD_NAME, DM_VKBD_NUM };   // 键盘种类：一般文字 / 角色名 / 数量 | keyboard kind: general text / character name / quantity
enum { DM_VK_CHAR, DM_VK_BACK, DM_VK_SHIFT, DM_VK_SPACE, DM_VK_OK, DM_VK_CANCEL, DM_VK_MAX, DM_VK_HALF };   // 键的动作 | key action
enum { DM_VKBD_OPEN = 0, DM_VKBD_OK = 1, DM_VKBD_CANCEL = -1 };   // done 的值 | values of done
// 提示行：上一个动作为什么被拦（到下一个动作为止一直显示） | hint line: why the last action was refused (shown until the next action)
enum { DM_VKBD_HINT_NONE, DM_VKBD_HINT_FULL, DM_VKBD_HINT_SHORT, DM_VKBD_HINT_EDGE, DM_VKBD_HINT_SEP };
enum { DM_VKBD_MAXKEYS = 48, DM_VKBD_TEXTCAP = 128 };

typedef struct { int8_t row, col, span, act; char ch; } dm_vkey;   // ch：DM_VK_CHAR 的字（字母存小写） | ch: the character of a DM_VK_CHAR key (letters stored lower case)
typedef struct {
    int kind, max_bytes, num_max;
    int nkeys, nrows, ncols;
    dm_vkey keys[DM_VKBD_MAXKEYS];
    int focus;               // 焦点键的下标 | index of the focused key
    float vcol;              // 上下移动时要对准的列（横移才改，穿过宽键回来还在原列） | the column vertical moves aim for (changed only by horizontal moves, so passing through a wide key comes back to the same column)
    int shift;               // 0 小写、1 下一个字母大写、2 大写锁定 | 0 lower case, 1 next letter upper case, 2 caps lock
    int fresh;               // 数量框：还是打开时的初始值，第一个数字整个换掉 | quantity box: still the initial value; the first digit replaces it all
    char text[DM_VKBD_TEXTCAP]; int len;
    int hint;
    int done;
    uint64_t prev;           // 上次看到的键（取边沿） | buttons seen last time (for edges)
    int rep_dir; double rep_at;   // 按住的方向、下一次连发的时刻 | the held direction and when it repeats next
} dm_vkbd;

// held = 打开这一刻已按着的键：要先松开才算（弹键盘的那一下确认不会顺手打一个字） | held = buttons already down at this moment: they count only after release (the confirm press that brought up the keyboard does not also type a key)
void dm_vkbd_open(dm_vkbd *k, int kind, const char *initial, int max_chars, int num_max, uint64_t held);
int dm_vkbd_pad(dm_vkbd *k, uint64_t buttons, int32_t lx, int32_t ly, double now);   // 手柄状态（dm_input.h 的键位、左摇杆）；返回 1 = 有变化要重画 | controller state (dm_input.h buttons, left stick); returns 1 = changed, redraw
int dm_vkbd_press(dm_vkbd *k, int idx);     // 按下第 idx 个键（手柄确认、鼠标点、手指点）；返回 1 = 有变化 | press key idx (controller confirm, mouse click, finger tap); returns 1 = changed
int dm_vkbd_move(dm_vkbd *k, int dx, int dy);   // 焦点移一格（方向键）；返回 1 = 有变化 | move the focus by one key (arrow keys); returns 1 = changed
int dm_vkbd_type(dm_vkbd *k, char c);       // 实体键盘打的一个字（按当前种类的规则收或拦） | one character typed on a physical keyboard (taken or refused by the current kind's rules)
int dm_vkbd_backspace(dm_vkbd *k);
int dm_vkbd_ok(dm_vkbd *k);                  // 按规则收下文字并结束；不合规则就只出提示 | accept the text by the rules and finish; if it breaks a rule, only show a hint
void dm_vkbd_cancel(dm_vkbd *k);
int dm_vkbd_name_check(const char *s);       // 角色名规则（同 hle_swkbd.c / 游戏 main+0x7fed78）：0 = 合规（空 = 随机取名），否则 DM_VKBD_HINT_* | character name rule (as hle_swkbd.c / the game's main+0x7fed78): 0 = valid (empty = random name), otherwise DM_VKBD_HINT_*
int dm_vkbd_key_dim(const dm_vkbd *k, int idx);   // 这个键现在按了也没用（画暗） | pressing this key now would do nothing (drawn dimmed)

// 面板几何（点，原点左上）：宽由前端按画面定，高随之算出；键的位置、命中都从这里来
// Panel geometry (points, origin at top left): the front end picks the width from the picture, the height follows; key positions and hit tests all come from here
typedef struct { float w, h, pad, gap, key_w, key_h, title_y, title_h, field_y, field_h, hint_y, hint_h, rows_y, legend_y, legend_h; } dm_vkbd_geo;
void dm_vkbd_geom(const dm_vkbd *k, float w, dm_vkbd_geo *g);
void dm_vkbd_key_rect(const dm_vkbd *k, const dm_vkbd_geo *g, int idx, float r[4]);   // x, y, w, h
int dm_vkbd_hit(const dm_vkbd *k, const dm_vkbd_geo *g, float x, float y);            // 点中的键，没点中 -1 | the key hit, -1 if none

#ifdef __cplusplus
}
#endif
