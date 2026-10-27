// 屏幕常亮判断的单测（dm/ios/Sources/dm_awake.h）：在玩常亮、菜单 / 暂停页过宽限后交还系统、手柄输入续宽限、测试开关整局常亮。
// Unit test for the keep-awake decision (dm/ios/Sources/dm_awake.h): awake while playing, handed back to the system in menus / the pause page after the grace, controller input extends the grace, test switches keep the whole session awake.
#include <stdio.h>
#include "dm_awake.h"

static int g_fail;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main(void) {
    const double G = DM_AWAKE_GRACE_S;
    // 局内在玩：常亮，不管多久没碰 | in a run and playing: awake, however long nobody touched anything
    CHECK(dm_awake_want(0, 1, 0, 10000, 10000, 0) == 1);
    // 宿主刚起来（开机、主菜单）：宽限内常亮，过了交还 | the host just started (booting, main menu): awake within the grace, handed back after it
    CHECK(dm_awake_want(0, 0, 0, 100 + G - 1, 100, 0) == 1);
    CHECK(dm_awake_want(0, 0, 0, 100 + G + 1, 100, 0) == 0);
    // 开了暂停页：宽限内常亮，过了交还 | pause page opened: awake within the grace, handed back after it
    CHECK(dm_awake_want(0, 1, 1, 500 + 5, 500, 0) == 1);
    CHECK(dm_awake_want(0, 1, 1, 500 + G + 1, 500, 0) == 0);
    // 读取画面 / 回到选角页（不在局内）：同上 | loading screen / back on character select (not in a run): same
    CHECK(dm_awake_want(0, 0, 0, 500 + 30, 500, 0) == 1);
    CHECK(dm_awake_want(0, 0, 0, 500 + G + 30, 500, 0) == 0);
    // 菜单里用手柄翻：最近一次手柄输入续上宽限 | navigating menus with a controller: the latest controller input renews the grace
    CHECK(dm_awake_want(0, 0, 0, 2000, 100, 2000 - 10) == 1);
    CHECK(dm_awake_want(0, 0, 0, 2000, 100, 2000 - G - 1) == 0);
    CHECK(dm_awake_want(0, 1, 1, 2000, 100, 2000 - 10) == 1);
    // 没有过手柄输入（0）不算 | no controller input yet (0) does not count
    CHECK(dm_awake_want(0, 0, 0, 50, 0, 0) == 0);
    // 测试开关：菜单里没人碰也常亮 | test switch: awake in menus with nobody touching
    CHECK(dm_awake_want(1, 0, 0, 100000, 100, 0) == 1);
    CHECK(dm_awake_want(1, 1, 1, 100000, 100, 0) == 1);
    if (g_fail) { printf("t_awake: %d FAIL\n", g_fail); return 1; }
    printf("t_awake: OK\n");
    return 0;
}
