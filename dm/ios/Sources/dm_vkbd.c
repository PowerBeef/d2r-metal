// 手柄屏幕键盘的纯 C 模型（见 dm_vkbd.h）。不依赖游戏地址，不碰游戏内存。
// 键位：角色名 = 三排 QWERTY 字母（含 ⇧ ⌫ - _）+ 确定 / 取消；一般文字 = 多一排数字、空格；数量 = 3×3 数字盘 + 一半 / 0 / 最大 + ⌫ 确定 取消。
// 手柄：方向键 / 左摇杆移焦点（按住 0.4 秒后每 0.08 秒连发，左右循环、上下循环），A 按焦点键，B 删一个字（空了再按 = 取消），
// X 大小写（小写 → 下一个大写 → 大写锁定），Y 空格（数量框是「最大」），＋ 确定，− 取消。
// Pure C model of the on-screen controller keyboard (see dm_vkbd.h). It relies on no game address and does not touch game memory.
// Layouts: character name = three QWERTY letter rows (with ⇧ ⌫ - _) + OK / Cancel; general text = one more row of digits, and Space; quantity = a 3×3 number pad + Half / 0 / Max + ⌫ OK Cancel.
// Controller: D-pad / left stick move the focus (held 0.4 s, then repeating every 0.08 s; left / right and up / down wrap around), A presses the focused key, B deletes one character (pressed again when empty = cancel),
// X switches case (lower → next one upper → caps lock), Y is Space (Max on the quantity box), + is OK, − is Cancel.
#include "dm_vkbd.h"
#include "../../host/dm_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NAME_BYTES = 15 };   // 游戏的角色名上限（字节） | the game's character name limit (bytes)
static const double REP_FIRST = 0.4, REP_NEXT = 0.08;

static void add(dm_vkbd *k, int row, int col, int span, int act, char ch) {
    if (k->nkeys >= DM_VKBD_MAXKEYS) return;
    k->keys[k->nkeys++] = (dm_vkey){(int8_t)row, (int8_t)col, (int8_t)span, (int8_t)act, ch};
    if (row + 1 > k->nrows) k->nrows = row + 1;
    if (col + span > k->ncols) k->ncols = col + span;
}
static void add_row(dm_vkbd *k, int row, int col, const char *chars) { for (int i = 0; chars[i]; i++) add(k, row, col + i, 1, DM_VK_CHAR, chars[i]); }

static void layout(dm_vkbd *k) {
    k->nkeys = k->nrows = k->ncols = 0;
    if (k->kind == DM_VKBD_NUM) {
        add_row(k, 0, 0, "123"); add_row(k, 1, 0, "456"); add_row(k, 2, 0, "789");
        add(k, 3, 0, 1, DM_VK_HALF, 0); add(k, 3, 1, 1, DM_VK_CHAR, '0'); add(k, 3, 2, 1, DM_VK_MAX, 0);
        add(k, 4, 0, 1, DM_VK_BACK, 0); add(k, 4, 1, 1, DM_VK_OK, 0); add(k, 4, 2, 1, DM_VK_CANCEL, 0);
        return;
    }
    int r = 0;
    if (k->kind == DM_VKBD_TEXT) add_row(k, r++, 0, "1234567890");
    add_row(k, r++, 0, "qwertyuiop");
    add_row(k, r, 0, "asdfghjkl"); add(k, r++, 9, 1, DM_VK_BACK, 0);
    add(k, r, 0, 1, DM_VK_SHIFT, 0); add_row(k, r++, 1, k->kind == DM_VKBD_TEXT ? "zxcvbnm-." : "zxcvbnm-_");
    if (k->kind == DM_VKBD_TEXT) { add(k, r, 0, 4, DM_VK_SPACE, 0); add(k, r, 4, 3, DM_VK_OK, 0); add(k, r, 7, 3, DM_VK_CANCEL, 0); }
    else { add(k, r, 0, 5, DM_VK_OK, 0); add(k, r, 5, 5, DM_VK_CANCEL, 0); }
}

static float center(const dm_vkey *v) { return v->col + v->span / 2.0f; }
static void focus_to(dm_vkbd *k, int i, int set_col) { k->focus = i; if (set_col) k->vcol = center(&k->keys[i]); }

void dm_vkbd_open(dm_vkbd *k, int kind, const char *initial, int max_chars, int num_max, uint64_t held) {
    memset(k, 0, sizeof *k);
    k->kind = kind == DM_VKBD_NAME || kind == DM_VKBD_NUM ? kind : DM_VKBD_TEXT;
    k->num_max = num_max < 0 ? 0 : num_max;
    int cap = DM_VKBD_TEXTCAP - 1;
    if (k->kind == DM_VKBD_NAME) cap = NAME_BYTES;
    if (k->kind == DM_VKBD_NUM) cap = 10;
    if (max_chars > 0 && max_chars < cap) cap = max_chars;
    k->max_bytes = cap;
    layout(k);
    if (initial) {   // 初始文本：数量框只留数字并夹到上限；别的原样（可能有汉字，删的时候整字删） | initial text: the quantity box keeps only digits, clamped to the limit; others as is (may contain Chinese; deleting removes whole characters)
        for (const char *p = initial; *p && k->len < k->max_bytes; p++) if (k->kind != DM_VKBD_NUM || (*p >= '0' && *p <= '9')) k->text[k->len++] = *p;
        k->text[k->len] = 0;
        if (k->kind == DM_VKBD_NUM && k->len) { long long v = atoll(k->text); if (v > k->num_max) k->len = snprintf(k->text, sizeof k->text, "%d", k->num_max); k->fresh = 1; }
    }
    k->shift = k->kind == DM_VKBD_NAME && !k->len;   // 起名：第一个字母自动大写 | naming: the first letter is upper case automatically
    k->prev = held;
    focus_to(k, 0, 1);
}

int dm_vkbd_name_check(const char *s) {
    size_t n = strlen(s); int sep = 0;
    if (!n) return DM_VKBD_HINT_NONE;   // 留空 = 随机取名 | left empty = random name
    if (n > NAME_BYTES) return DM_VKBD_HINT_FULL;
    if (n < 2) return DM_VKBD_HINT_SHORT;
    if (s[0] == '-' || s[0] == '_' || s[n - 1] == '-' || s[n - 1] == '_') return DM_VKBD_HINT_EDGE;
    for (size_t i = 0; i < n; i++) if ((s[i] == '-' || s[i] == '_') && ++sep > 1) return DM_VKBD_HINT_SEP;
    return DM_VKBD_HINT_NONE;
}

static int has_sep(const dm_vkbd *k) { return memchr(k->text, '-', (size_t)k->len) || memchr(k->text, '_', (size_t)k->len); }
static int is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int put_char(dm_vkbd *k, char c) {   // 收一个字（已按大小写换好）；拦下时置提示 | take one character (case already applied); set the hint when refused
    if (k->kind == DM_VKBD_NUM) {
        if (c < '0' || c > '9') return 0;
        if (k->fresh || (k->len == 1 && k->text[0] == '0')) k->len = 0;   // 第一个数字整个换掉初始值；不留前导 0 | the first digit replaces the initial value; no leading zero
        k->fresh = 0;
        if (k->len >= k->max_bytes) { k->hint = DM_VKBD_HINT_FULL; return 1; }
        k->text[k->len++] = c; k->text[k->len] = 0;
        if (atoll(k->text) > k->num_max) k->len = snprintf(k->text, sizeof k->text, "%d", k->num_max);   // 超过上限当场改成上限（同 iOS 系统输入框） | over the limit becomes the limit on the spot (as the iOS system input box)
        return 1;
    }
    if (k->kind == DM_VKBD_NAME) {
        if (!is_letter(c) && c != '-' && c != '_') return 0;
        if ((c == '-' || c == '_') && !k->len) { k->hint = DM_VKBD_HINT_EDGE; return 1; }
        if ((c == '-' || c == '_') && has_sep(k)) { k->hint = DM_VKBD_HINT_SEP; return 1; }
    } else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return 0;
    if (k->len >= k->max_bytes) { k->hint = DM_VKBD_HINT_FULL; return 1; }
    k->text[k->len++] = c; k->text[k->len] = 0;
    return 1;
}

int dm_vkbd_type(dm_vkbd *k, char c) {
    if (k->done) return 0;
    k->hint = DM_VKBD_HINT_NONE;
    int r = put_char(k, c);
    if (r && is_letter(c) && k->shift == 1) k->shift = 0;
    return r;
}

int dm_vkbd_backspace(dm_vkbd *k) {
    if (k->done) return 0;
    k->hint = DM_VKBD_HINT_NONE; k->fresh = 0;
    if (!k->len) { dm_vkbd_cancel(k); return 1; }   // 空了再删 = 取消（同多数主机键盘的 B） | deleting when empty = cancel (like B on most console keyboards)
    do k->len--; while (k->len > 0 && ((unsigned char)k->text[k->len] & 0xC0) == 0x80);   // 整个 UTF-8 字去掉 | remove a whole UTF-8 character
    k->text[k->len] = 0;
    if (k->kind == DM_VKBD_NAME && !k->len && !k->shift) k->shift = 1;
    return 1;
}

int dm_vkbd_ok(dm_vkbd *k) {
    if (k->done) return 0;
    if (k->kind == DM_VKBD_NAME && (k->hint = dm_vkbd_name_check(k->text))) return 1;
    if (k->kind == DM_VKBD_NUM && !k->len) { k->text[0] = '0'; k->text[1] = 0; k->len = 1; }
    k->done = DM_VKBD_OK; return 1;
}
void dm_vkbd_cancel(dm_vkbd *k) { if (!k->done) k->done = DM_VKBD_CANCEL; }

static void set_num(dm_vkbd *k, int v) { k->len = snprintf(k->text, sizeof k->text, "%d", v < 0 ? 0 : v); k->fresh = 0; k->hint = DM_VKBD_HINT_NONE; }

int dm_vkbd_press(dm_vkbd *k, int idx) {
    if (k->done || idx < 0 || idx >= k->nkeys) return 0;
    const dm_vkey *v = &k->keys[idx];
    if (k->focus != idx) focus_to(k, idx, 1);   // 点的键同时成为焦点 | a clicked key also becomes the focus
    switch (v->act) {
        case DM_VK_CHAR: { char c = v->ch; if (is_letter(c) && k->shift) c = (char)(c - 'a' + 'A'); dm_vkbd_type(k, c); return 1; }
        case DM_VK_BACK: if (k->len) dm_vkbd_backspace(k); return 1;   // 屏上的 ⌫ 不当取消用 | the on-screen ⌫ never cancels
        case DM_VK_SHIFT: k->shift = (k->shift + 1) % 3; k->hint = DM_VKBD_HINT_NONE; return 1;
        case DM_VK_SPACE: dm_vkbd_type(k, ' '); return 1;
        case DM_VK_OK: return dm_vkbd_ok(k);
        case DM_VK_CANCEL: dm_vkbd_cancel(k); return 1;
        case DM_VK_MAX: set_num(k, k->num_max); return 1;
        case DM_VK_HALF: set_num(k, k->num_max / 2); return 1;
    }
    return 0;
}

int dm_vkbd_key_dim(const dm_vkbd *k, int idx) {
    if (idx < 0 || idx >= k->nkeys) return 1;
    const dm_vkey *v = &k->keys[idx];
    if (v->act == DM_VK_OK) return k->kind == DM_VKBD_NAME && dm_vkbd_name_check(k->text) != DM_VKBD_HINT_NONE;
    if (v->act == DM_VK_BACK) return !k->len;
    if (v->act == DM_VK_CHAR && k->kind == DM_VKBD_NAME && (v->ch == '-' || v->ch == '_')) return !k->len || has_sep(k);
    return 0;
}

int dm_vkbd_move(dm_vkbd *k, int dx, int dy) {
    if (k->done || !k->nkeys) return 0;
    const dm_vkey *cur = &k->keys[k->focus];
    if (dx) {   // 同一排里左右，循环 | left / right within the row, wrapping
        int best = -1, bestc = 0;
        for (int i = 0; i < k->nkeys; i++) { if (k->keys[i].row != cur->row || i == k->focus) continue;
            int d = (k->keys[i].col - cur->col) * dx; if (d < 0) d += k->ncols * 2;   // 反方向的排到最后 = 绕回来 | keys in the opposite direction sort last = wrap around
            if (best < 0 || d < bestc) { best = i; bestc = d; } }
        if (best < 0) return 0;
        focus_to(k, best, 1); return 1;
    }
    if (dy) {   // 上下：换到相邻一排里盖住 vcol 的键，循环 | up / down: to the key covering vcol in the adjacent row, wrapping
        int row = ((cur->row + dy) % k->nrows + k->nrows) % k->nrows, best = -1; float bd = 1e9f;
        for (int i = 0; i < k->nkeys; i++) { if (k->keys[i].row != row) continue;
            float d = k->vcol < k->keys[i].col ? k->keys[i].col - k->vcol : k->vcol > k->keys[i].col + k->keys[i].span ? k->vcol - (k->keys[i].col + k->keys[i].span) : 0;
            if (d < bd) { bd = d; best = i; } }
        if (best < 0) return 0;
        focus_to(k, best, 0); return 1;
    }
    return 0;
}

static int dir_of(uint64_t b, int32_t lx, int32_t ly) {   // 1 左 2 右 3 上 4 下，0 无；方向键优先，摇杆取推得多的那一轴（过半才算） | 1 left 2 right 3 up 4 down, 0 none; the D-pad wins, the stick takes its larger axis (counts past half)
    if (b & DM_BTN_LEFT) return 1; if (b & DM_BTN_RIGHT) return 2; if (b & DM_BTN_UP) return 3; if (b & DM_BTN_DOWN) return 4;
    if (abs(lx) < DM_STICK_MAX / 2 && abs(ly) < DM_STICK_MAX / 2) return 0;
    return abs(lx) >= abs(ly) ? (lx < 0 ? 1 : 2) : (ly > 0 ? 3 : 4);   // 摇杆上为正 | stick up is positive
}
static int step(dm_vkbd *k, int d) { return dm_vkbd_move(k, d == 1 ? -1 : d == 2 ? 1 : 0, d == 3 ? -1 : d == 4 ? 1 : 0); }

int dm_vkbd_pad(dm_vkbd *k, uint64_t b, int32_t lx, int32_t ly, double now) {
    if (k->done) return 0;
    uint64_t down = b & ~k->prev; int ch = 0;
    k->prev = b;   // 打开时按着的键（open 的 held）松开后才清掉，再按下才算 | buttons held at open (open's held) clear only when released, and count once pressed again
    int d = dir_of(b, lx, ly);
    if (d != k->rep_dir) { k->rep_dir = d; k->rep_at = now + REP_FIRST; if (d) ch |= step(k, d); }
    else if (d && now >= k->rep_at) { k->rep_at = now + REP_NEXT; ch |= step(k, d); }
    if (down & DM_BTN_A) ch |= dm_vkbd_press(k, k->focus);
    if (down & DM_BTN_B) ch |= dm_vkbd_backspace(k);
    if ((down & DM_BTN_X) && k->kind != DM_VKBD_NUM) { k->shift = (k->shift + 1) % 3; k->hint = DM_VKBD_HINT_NONE; ch = 1; }
    if (down & DM_BTN_Y) { if (k->kind == DM_VKBD_TEXT) ch |= dm_vkbd_type(k, ' '); else if (k->kind == DM_VKBD_NUM) { set_num(k, k->num_max); ch = 1; } }
    if (down & DM_BTN_PLUS) ch |= dm_vkbd_ok(k);
    if (down & DM_BTN_MINUS) { dm_vkbd_cancel(k); ch = 1; }
    return ch;
}

void dm_vkbd_geom(const dm_vkbd *k, float w, dm_vkbd_geo *g) {
    memset(g, 0, sizeof *g);
    int cols = k->ncols > 0 ? k->ncols : 1, rows = k->nrows > 0 ? k->nrows : 1;
    g->w = w; g->pad = w * (k->kind == DM_VKBD_NUM ? 0.06f : 0.03f); g->gap = w * (k->kind == DM_VKBD_NUM ? 0.025f : 0.008f);
    g->key_w = (w - 2 * g->pad - (cols - 1) * g->gap) / cols;
    g->key_h = k->kind == DM_VKBD_NUM ? g->key_w * 0.5f : g->key_w * 0.8f;
    float u = k->kind == DM_VKBD_NUM ? g->key_h * 1.1f : g->key_h;   // 页眉各行以键高为单位（数量盘的键扁，单位放大一点） | header rows are measured in key heights (the number pad's keys are flat, so the unit is a bit larger)
    float y = g->pad;
    g->title_y = y; g->title_h = u * 0.6f; y += g->title_h + g->gap;
    g->field_y = y; g->field_h = u * 0.85f; y += g->field_h + g->gap * 0.5f;
    g->hint_y = y; g->hint_h = u * 0.5f; y += g->hint_h + g->gap;
    g->rows_y = y; y += rows * g->key_h + (rows - 1) * g->gap + g->gap * 1.5f;
    g->legend_y = y; g->legend_h = u * 0.5f; y += g->legend_h + g->pad;
    g->h = y;
}

void dm_vkbd_key_rect(const dm_vkbd *k, const dm_vkbd_geo *g, int idx, float r[4]) {
    const dm_vkey *v = &k->keys[idx];
    r[0] = g->pad + v->col * (g->key_w + g->gap); r[1] = g->rows_y + v->row * (g->key_h + g->gap);
    r[2] = v->span * g->key_w + (v->span - 1) * g->gap; r[3] = g->key_h;
}

int dm_vkbd_hit(const dm_vkbd *k, const dm_vkbd_geo *g, float x, float y) {
    for (int i = 0; i < k->nkeys; i++) { float r[4]; dm_vkbd_key_rect(k, g, i, r);
        if (x >= r[0] - g->gap / 2 && x < r[0] + r[2] + g->gap / 2 && y >= r[1] - g->gap / 2 && y < r[1] + r[3] + g->gap / 2) return i; }   // 键缝也算到最近的键 | the gaps between keys count for the nearest key
    return -1;
}
