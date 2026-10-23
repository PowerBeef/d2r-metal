// 技能瞄准选目标的「指定目标槽」寿命单测（直接包含 hle_private_aim.c）：施放时写进槽里的怪指针，到下一次取手柄（dm_aim_cone_pump）就清掉，
// 右摇杆还推着也一样——那只怪随后可能被释放，游戏读槽不核死活；锥内没怪时技能记录的标志照旧留到右摇杆归零才恢复。槽与记录用假内存，不跑游戏。
// Lifetime unit test of the skill-aim "designated target slot" (includes hle_private_aim.c directly): the monster pointer written into the slot at the cast is cleared at the next controller read (dm_aim_cone_pump),
// even with the right stick still pushed -- the monster may be freed afterwards and the game reads the slot without checking it is alive; with no monster in the cone, the skill record's flag still stays until the right stick returns to zero. The slot and the record are fake memory; the game does not run.
#include "../hle_private_aim.c"
#include <stdio.h>
static int32_t g_rx, g_ry;
void dm_input_sampled_right(int32_t *rx, int32_t *ry) { *rx = g_rx; *ry = g_ry; }
static int checks, fails;
#define CHECK(c, s) do { checks++; if (!(c)) { fails++; printf("  FAIL %s\n", s); } else printf("  ok   %s\n", s); } while (0)
int main(void) {
    unsetenv("DM_TEST_AIM"); unsetenv("DM_TEST_AIM_AT");
    static uint64_t mgr[0x1000 / 8]; static uint8_t rec[0x40]; static uint64_t unit[16];
    uint64_t *slot = &mgr[0xd10 / 8];
    g_rx = 20000; g_ry = 0;                                   // 手指还拖着 | the finger is still dragging
    cone_lock((uint64_t)(uintptr_t)mgr, (uint64_t)(uintptr_t)unit); atomic_store(&g_cone_set, 1);   // 施放：锥内有怪，锁它（同 svc_private_skill_aim） | the cast: a monster in the cone is locked (as svc_private_skill_aim)
    CHECK(*slot == (uint64_t)(uintptr_t)unit, "施放时指定目标槽是锁定的怪 | at the cast the designated target slot holds the locked monster");
    dm_aim_cone_pump();
    CHECK(*slot == 0, "下一次取手柄：右摇杆还推着，槽也清掉了 | next controller read: the slot is cleared even with the right stick still pushed");
    CHECK(!atomic_load(&g_cone_set), "没有别的要恢复：不再每帧检查 | nothing else to restore: no per-frame check any more");

    g_cone_rec = (uint64_t)(uintptr_t)rec; g_cone_rec_was = 0; rec[0x18] = 1; atomic_store(&g_cone_set, 1);   // 锥内没怪：对位置放（技能记录的标志临时置 1） | no monster in the cone: cast at the position (the skill record's flag temporarily set to 1)
    dm_aim_cone_pump();
    CHECK(rec[0x18] == 1 && atomic_load(&g_cone_set), "右摇杆还推着：技能记录的标志留着 | right stick still pushed: the skill record's flag stays");
    g_rx = 0; dm_aim_cone_pump();
    CHECK(rec[0x18] == 0 && !atomic_load(&g_cone_set) && *slot == 0, "右摇杆归零：标志恢复原值 | right stick back to zero: the flag is restored");
    printf("== t_aim_cone %d 项，失败 %d | %d checks, %d failed ==\n", checks, fails, checks, fails);
    return fails != 0;
}
