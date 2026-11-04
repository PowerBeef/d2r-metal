// Unit test of the PC input layer's pure parts (dm/host/dm_kbm.c): the macOS key code -> Windows virtual key table, and which key bindings a key
// fires (modifier rules) against a binding list laid out like the game's (main+0x2086f64: {u32 id, u16 VK | 0x1000 Shift | 0x2000 Ctrl | 0x4000 Alt, u16 kind, u16})
#define DM_KBM_PURE_ONLY 1
#include "../dm_kbm.c"
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void put(uint8_t *l, int i, uint32_t id, uint16_t key) { memcpy(l + i * 10, &id, 4); memcpy(l + i * 10 + 4, &key, 2); uint16_t z = 0; memcpy(l + i * 10 + 6, &z, 2); memcpy(l + i * 10 + 8, &z, 2); }

int main(void) {
    CHECK(dm_kbm_vk_from_mac(0) == 'A' && dm_kbm_vk_from_mac(34) == 'I' && dm_kbm_vk_from_mac(17) == 'T', "letters");
    CHECK(dm_kbm_vk_from_mac(18) == '1' && dm_kbm_vk_from_mac(23) == '5' && dm_kbm_vk_from_mac(29) == '0', "digits");
    CHECK(dm_kbm_vk_from_mac(122) == 0x70 && dm_kbm_vk_from_mac(100) == 0x77 && dm_kbm_vk_from_mac(111) == 0x7B, "F1 F8 F12");
    CHECK(dm_kbm_vk_from_mac(53) == 0x1B && dm_kbm_vk_from_mac(48) == 0x09 && dm_kbm_vk_from_mac(49) == 0x20 && dm_kbm_vk_from_mac(36) == 0x0D, "Esc Tab Space Return");
    CHECK(dm_kbm_vk_from_mac(56) == 0x10 && dm_kbm_vk_from_mac(59) == 0x11 && dm_kbm_vk_from_mac(58) == 0x12 && dm_kbm_vk_from_mac(50) == 0xC0, "Shift Ctrl Alt `");
    CHECK(dm_kbm_vk_from_mac(10) == 0 && dm_kbm_vk_from_mac(55) == 0, "unused: section sign, Command");

    uint8_t l[8 * 10]; int o[4], n;
    put(l, 0, 1, 'I'); put(l, 1, 1, 'B'); put(l, 2, 14, 0x70); put(l, 3, 36, 0x10); put(l, 4, 37, 0x12); put(l, 5, 50, 0x1000 | 'Q'); put(l, 6, 51, 0xffff); put(l, 7, 7, 0x100);
    n = dm_kbm_match(l, 8, 'I', 0, o, 4); CHECK(n == 1 && o[0] == 0, "I -> inventory (%d)", n);
    n = dm_kbm_match(l, 8, 'B', 0, o, 4); CHECK(n == 1 && o[0] == 1, "B -> inventory, second key");
    n = dm_kbm_match(l, 8, 0x70, 1, o, 4); CHECK(n == 1 && o[0] == 2, "Shift + F1 still picks the skill");
    n = dm_kbm_match(l, 8, 0x70, 2, o, 4); CHECK(n == 0, "Ctrl + F1 does not");
    n = dm_kbm_match(l, 8, 0x10, 1, o, 4); CHECK(n == 1 && o[0] == 3, "Shift itself (stand still)");
    n = dm_kbm_match(l, 8, 0x12, 4, o, 4); CHECK(n == 1 && o[0] == 4, "Alt itself (show items)");
    n = dm_kbm_match(l, 8, 'Q', 1, o, 4); CHECK(n == 1 && o[0] == 5, "Shift + Q binding");
    n = dm_kbm_match(l, 8, 'Q', 0, o, 4); CHECK(n == 0, "Q alone does not fire Shift + Q");
    n = dm_kbm_match(l, 8, 0x100, 0, o, 4); CHECK(n == 1 && o[0] == 7, "mouse 3");
    n = dm_kbm_match(l, 8, 'Z', 0, o, 4); CHECK(n == 0, "unbound key");
    // where a press lands (dm_kbm_where): the HUD band is judged in interface height (on 16:10, interface y 0.878 is picture y 0.890: world)
    CHECK(dm_kbm_where(0, 0, 0, 0.878f) == 0, "interface y 0.878: world, not the HUD");
    CHECK(dm_kbm_where(0, 0, 0, 0.885f) == 0, "the band's edge itself: world");
    CHECK(dm_kbm_where(0, 0, 0, 0.90f) == 2 && dm_kbm_where(0, 0, 0, 1.0f) == 2, "the PC HUD band");
    CHECK(dm_kbm_where(0, 0, 0, 1.05f) == 0 && dm_kbm_where(0, 0, 0, -0.2f) == 0, "below / above the band: world");
    CHECK(dm_kbm_where(5, 0, 0, 0.5f) == 1, "an item area: the panel");
    CHECK(dm_kbm_where(3, 0, 0, 0.95f) == 2 && dm_kbm_where(3, 1, 0, 0.95f) == 1, "outside every panel in the HUD band: HUD; with panels up: the panel");
    CHECK(dm_kbm_where(0, 0, 1, 0.5f) == 1 && dm_kbm_where(3, 0, 1, 0.5f) == 0, "nothing classified while something is open: the panel; outside every panel: world");
    printf("KBM %s fails=%d\n", fails ? "RED" : "PASS", fails);
    return fails != 0;
}
