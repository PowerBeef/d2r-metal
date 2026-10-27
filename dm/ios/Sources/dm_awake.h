// 屏幕常亮的判断（纯函数，iOS 前端 front_ios.m 每 2 秒调一次，结果写 UIApplication.idleTimerDisabled；单测 tests/t_awake.c）。
// 只在真正在玩（局内、原版暂停页没开）时不让系统自动变暗 / 锁屏；主菜单、选角、暂停页、读取画面交还给系统的自动锁屏。
// 两段宽限（DM_AWAKE_GRACE_S 秒）：刚离开在玩（开暂停页、过读取画面、宿主刚起来还在开机）时不立刻交还；手柄刚有明确输入时也不交还——
// 系统的自动锁屏只认触屏，用手柄翻菜单不会重置它，没有这段宽限手柄玩家在选角页会被锁屏。
// 测试开关（浸泡 DM_SOAK、自动化真机跑 DM_SAVE_FRESH）照旧整局常亮：脚本在菜单里停着没人碰屏，锁屏 = 进后台，测试会断。
// Keep-awake decision (pure function; the iOS front end front_ios.m calls it every 2 seconds and writes the result to UIApplication.idleTimerDisabled; unit test tests/t_awake.c).
// The system is kept from dimming / locking only while actually playing (in a run, the original pause page not open); the main menu, character select, the pause page and loading screens hand back to the system's auto-lock.
// Two grace periods (DM_AWAKE_GRACE_S seconds): right after leaving play (opening the pause page, a loading screen, the host just started and still booting) it is not handed back at once; nor right after a clear controller input --
// the system's auto-lock only counts touches, so navigating menus with a controller does not reset it, and without this grace a controller player would be locked out on character select.
// Test switches (soak DM_SOAK, automated device runs DM_SAVE_FRESH) keep the whole session awake as before: scripts sit in menus with nobody touching the screen, locking = going to the background, and the test would break.
#pragma once

#define DM_AWAKE_GRACE_S 120.0

// test_keep：测试开关开着；in_run：局内（读到受控玩家）；paused：原版暂停页开着；now / last_play / last_pad：单调钟秒数，
// last_play = 最近一次「在玩」的时刻（宿主刚起来时记成起来那一刻），last_pad = 最近一次手柄明确输入的时刻（0 = 没有过）。返回 1 = 保持常亮
// test_keep: a test switch is on; in_run: in a run (the controlled player was read); paused: the original pause page is open; now / last_play / last_pad: monotonic seconds,
// last_play = the latest moment of "playing" (set to the moment the host started, right after it starts), last_pad = the latest clear controller input (0 = none yet). Returns 1 = keep the screen awake
static inline int dm_awake_want(int test_keep, int in_run, int paused, double now, double last_play, double last_pad) {
    if (test_keep) return 1;
    if (in_run && !paused) return 1;
    if (last_play > 0 && now - last_play < DM_AWAKE_GRACE_S) return 1;
    if (last_pad > 0 && now - last_pad < DM_AWAKE_GRACE_S) return 1;
    return 0;
}
