// 手柄屏幕键盘模型（Sources/dm_vkbd.c）单测：角色名规则、数量框上限、焦点移动与连发、按着的键要先松开、几何与命中。
// Unit test of the on-screen controller keyboard model (Sources/dm_vkbd.c): character name rule, quantity limit, focus movement and repeat, keys held at open must be released first, geometry and hit tests.
#include "dm_vkbd.h"
#include "../../host/dm_input.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double t;
static void tap(dm_vkbd *k, uint64_t b) { dm_vkbd_pad(k, b, 0, 0, t += 0.05); dm_vkbd_pad(k, 0, 0, 0, t += 0.05); }   // 按一下再松开 | press once and release
static int find(const dm_vkbd *k, int act, char ch) { for (int i = 0; i < k->nkeys; i++) if (k->keys[i].act == act && (act != DM_VK_CHAR || k->keys[i].ch == ch)) return i; return -1; }
static void type_by_pad(dm_vkbd *k, const char *s) {   // 用方向键走到每个字再按 A（不直接跳焦点：验移动本身） | walk to each character with the D-pad and press A (no direct focus jumps: this checks the movement itself)
    for (; *s; s++) {
        char lc = *s >= 'A' && *s <= 'Z' ? (char)(*s - 'A' + 'a') : *s; int want = find(k, DM_VK_CHAR, lc), guard = 0;
        if (want < 0) { CHECK(0, "no key for '%c'", *s); return; }
        while (k->keys[k->focus].row != k->keys[want].row && guard++ < 20) tap(k, k->keys[k->focus].row < k->keys[want].row ? DM_BTN_DOWN : DM_BTN_UP);
        while (k->focus != want && guard++ < 40) tap(k, k->keys[k->focus].col < k->keys[want].col ? DM_BTN_RIGHT : DM_BTN_LEFT);
        int up = *s >= 'A' && *s <= 'Z';
        if (up != (k->shift != 0)) { tap(k, DM_BTN_X); if (!up && k->shift) { tap(k, DM_BTN_X); } }
        tap(k, DM_BTN_A);
    }
}

int main(void) {
    dm_vkbd k;

    // 角色名：方向键 + A 打「Zork」，第一个字母自动大写，＋ 确定 | character name: type "Zork" with the D-pad + A, the first letter is upper case automatically, + confirms
    dm_vkbd_open(&k, DM_VKBD_NAME, "", 15, -1, DM_BTN_A);
    CHECK(k.shift == 1, "name starts with shift on (%d)", k.shift);
    dm_vkbd_pad(&k, DM_BTN_A, 0, 0, t += 0.05);   // 弹键盘那一下还按着：不打字 | the press that opened the keyboard is still held: types nothing
    CHECK(k.len == 0, "held A typed '%s'", k.text);
    dm_vkbd_pad(&k, 0, 0, 0, t += 0.05);
    type_by_pad(&k, "Zork");
    CHECK(!strcmp(k.text, "Zork"), "typed '%s'", k.text);
    tap(&k, DM_BTN_PLUS);
    CHECK(k.done == DM_VKBD_OK, "+ confirms (%d)", k.done);

    // 名字规则：开头不能是 - _、- _ 只能一个、至少 2 个字母、最多 15 | name rule: cannot start with - _, only one - or _, at least 2 letters, at most 15
    dm_vkbd_open(&k, DM_VKBD_NAME, "", 15, -1, 0);
    dm_vkbd_type(&k, '-'); CHECK(k.len == 0 && k.hint == DM_VKBD_HINT_EDGE, "leading '-' refused (len %d hint %d)", k.len, k.hint);
    dm_vkbd_type(&k, 'A'); dm_vkbd_ok(&k);
    CHECK(!k.done && k.hint == DM_VKBD_HINT_SHORT, "one letter refused (done %d hint %d)", k.done, k.hint);
    dm_vkbd_type(&k, '-'); dm_vkbd_type(&k, '_');
    CHECK(!strcmp(k.text, "A-") && k.hint == DM_VKBD_HINT_SEP, "second separator refused ('%s' hint %d)", k.text, k.hint);
    dm_vkbd_ok(&k); CHECK(!k.done && k.hint == DM_VKBD_HINT_EDGE, "trailing '-' refused at OK (hint %d)", k.hint);
    CHECK(dm_vkbd_key_dim(&k, find(&k, DM_VK_OK, 0)), "OK dimmed while the name breaks the rule");
    for (int i = 0; i < 20; i++) dm_vkbd_type(&k, 'b');
    CHECK(k.len == 15 && k.hint == DM_VKBD_HINT_FULL, "name capped at 15 bytes (%d, hint %d)", k.len, k.hint);
    CHECK(dm_vkbd_name_check("Ab") == 0 && dm_vkbd_name_check("") == 0 && dm_vkbd_name_check("A_b") == 0, "valid names pass");
    CHECK(dm_vkbd_name_check("A-b-c") == DM_VKBD_HINT_SEP && dm_vkbd_name_check("_ab") == DM_VKBD_HINT_EDGE, "invalid names fail");
    dm_vkbd_open(&k, DM_VKBD_NAME, "", 15, -1, 0); dm_vkbd_ok(&k);
    CHECK(k.done == DM_VKBD_OK && !k.len, "empty name is accepted (= random name)");

    // B 删字，空了再按 B = 取消；屏上 ⌫ 空着按不取消 | B deletes, B again when empty = cancel; the on-screen ⌫ does not cancel when empty
    dm_vkbd_open(&k, DM_VKBD_NAME, "Ab", 15, -1, 0);
    tap(&k, DM_BTN_B); CHECK(!strcmp(k.text, "A"), "B deletes ('%s')", k.text);
    tap(&k, DM_BTN_B); CHECK(k.len == 0 && k.shift == 1 && !k.done, "B to empty turns shift back on");
    dm_vkbd_press(&k, find(&k, DM_VK_BACK, 0)); CHECK(!k.done, "on-screen backspace on empty does not cancel");
    tap(&k, DM_BTN_B); CHECK(k.done == DM_VKBD_CANCEL, "B on empty cancels (%d)", k.done);
    dm_vkbd_open(&k, DM_VKBD_TEXT, "", 0, -1, 0); tap(&k, DM_BTN_MINUS); CHECK(k.done == DM_VKBD_CANCEL, "minus cancels");

    // 大小写：X 小写 → 下一个大写 → 锁定 | case: X goes lower → next one upper → caps lock
    dm_vkbd_open(&k, DM_VKBD_TEXT, "", 0, -1, 0);
    CHECK(k.shift == 0, "text starts lower case");
    tap(&k, DM_BTN_X); dm_vkbd_press(&k, find(&k, DM_VK_CHAR, 'q')); dm_vkbd_press(&k, find(&k, DM_VK_CHAR, 'q'));
    CHECK(!strcmp(k.text, "Qq"), "one-shot shift ('%s')", k.text);
    tap(&k, DM_BTN_X); tap(&k, DM_BTN_X); dm_vkbd_press(&k, find(&k, DM_VK_CHAR, 'w')); dm_vkbd_press(&k, find(&k, DM_VK_CHAR, 'w'));
    CHECK(!strcmp(k.text, "QqWW") && k.shift == 2, "caps lock ('%s', shift %d)", k.text, k.shift);
    tap(&k, DM_BTN_Y); CHECK(!strcmp(k.text, "QqWW "), "Y = space in text ('%s')", k.text);
    dm_vkbd_open(&k, DM_VKBD_TEXT, "\xe4\xb8\xad", 0, -1, 0); dm_vkbd_backspace(&k);
    CHECK(k.len == 0, "backspace removes a whole UTF-8 character (%d left)", k.len);

    // 数量：第一个数字换掉初始值、超上限改成上限、一半 / 最大、空着确定 = 0 | quantity: the first digit replaces the initial value, over the limit becomes the limit, Half / Max, OK when empty = 0
    dm_vkbd_open(&k, DM_VKBD_NUM, "5000", 0, 5000, 0);
    dm_vkbd_type(&k, '7'); CHECK(!strcmp(k.text, "7"), "first digit replaces ('%s')", k.text);
    dm_vkbd_type(&k, '0'); dm_vkbd_type(&k, '0'); dm_vkbd_type(&k, '0'); CHECK(!strcmp(k.text, "5000"), "clamped to the limit ('%s')", k.text);
    dm_vkbd_press(&k, find(&k, DM_VK_HALF, 0)); CHECK(!strcmp(k.text, "2500"), "half ('%s')", k.text);
    tap(&k, DM_BTN_Y); CHECK(!strcmp(k.text, "5000"), "Y = max on the quantity box ('%s')", k.text);
    dm_vkbd_type(&k, 'x'); CHECK(!strcmp(k.text, "5000"), "letters refused in the quantity box");
    while (k.len) dm_vkbd_backspace(&k);
    dm_vkbd_type(&k, '0'); dm_vkbd_type(&k, '4'); CHECK(!strcmp(k.text, "4"), "no leading zero ('%s')", k.text);
    while (k.len) dm_vkbd_backspace(&k);
    dm_vkbd_ok(&k); CHECK(k.done == DM_VKBD_OK && !strcmp(k.text, "0"), "empty quantity confirms as 0 ('%s')", k.text);
    dm_vkbd_open(&k, DM_VKBD_NUM, "99", 0, 50, 0); CHECK(!strcmp(k.text, "50"), "initial clamped ('%s')", k.text);

    // 移动：左右循环、上下保持列（穿过宽键回来还在原列）、按住连发 | movement: left / right wrap, up / down keep the column (passing through a wide key comes back to it), holding repeats
    dm_vkbd_open(&k, DM_VKBD_NAME, "", 15, -1, 0);
    CHECK(k.keys[k.focus].ch == 'q', "starts on q");
    tap(&k, DM_BTN_LEFT); CHECK(k.keys[k.focus].ch == 'p', "left wraps to p (got '%c')", k.keys[k.focus].ch);
    tap(&k, DM_BTN_DOWN); tap(&k, DM_BTN_DOWN); tap(&k, DM_BTN_DOWN);
    CHECK(k.keys[k.focus].act == DM_VK_CANCEL, "down from p reaches Cancel (act %d)", k.keys[k.focus].act);
    tap(&k, DM_BTN_DOWN); CHECK(k.keys[k.focus].ch == 'p', "down wraps back to p (got '%c')", k.keys[k.focus].ch);
    dm_vkbd_open(&k, DM_VKBD_NAME, "", 15, -1, 0); t += 1;
    dm_vkbd_pad(&k, DM_BTN_RIGHT, 0, 0, t);          // 按下：走一格 | pressed: one step
    dm_vkbd_pad(&k, DM_BTN_RIGHT, 0, 0, t + 0.2);    // 0.4 秒内不连发 | no repeat within 0.4 s
    CHECK(k.keys[k.focus].ch == 'w', "one step on press (got '%c')", k.keys[k.focus].ch);
    dm_vkbd_pad(&k, DM_BTN_RIGHT, 0, 0, t + 0.45); dm_vkbd_pad(&k, DM_BTN_RIGHT, 0, 0, t + 0.54);
    CHECK(k.keys[k.focus].ch == 'r', "repeats after the delay (got '%c')", k.keys[k.focus].ch);
    dm_vkbd_pad(&k, 0, 0, 0, t + 0.6);
    dm_vkbd_pad(&k, 0, 0, -DM_STICK_MAX, t + 0.7);   // 左摇杆往下推 | left stick pushed down
    CHECK(k.keys[k.focus].row == 1, "stick down moves a row (row %d)", k.keys[k.focus].row);

    // 几何：键在面板里、互不重叠，点键的中心命中它自己 | geometry: keys lie inside the panel, do not overlap, and a click at a key's center hits that key
    for (int kind = 0; kind < 3; kind++) {
        dm_vkbd_open(&k, kind, "", 0, 100, 0); dm_vkbd_geo g; dm_vkbd_geom(&k, kind == DM_VKBD_NUM ? 300 : 800, &g);
        int bad = 0;
        for (int i = 0; i < k.nkeys; i++) { float r[4]; dm_vkbd_key_rect(&k, &g, i, r);
            if (r[0] < 0 || r[1] < g.rows_y || r[0] + r[2] > g.w + 0.01f || r[1] + r[3] > g.legend_y) bad++;
            if (dm_vkbd_hit(&k, &g, r[0] + r[2] / 2, r[1] + r[3] / 2) != i) bad++; }
        CHECK(!bad && g.h > g.legend_y, "kind %d geometry (%d bad)", kind, bad);
        CHECK(dm_vkbd_hit(&k, &g, 1, 1) == -1, "kind %d: a click in the header hits no key", kind);
    }

    printf("t_vkbd：%d 项检查，%d 项失败 | %d checks, %d failed\n", checks, fails, checks, fails);
    printf(fails ? "VKBD RED\n" : "VKBD OK\n");
    return fails != 0;
}
