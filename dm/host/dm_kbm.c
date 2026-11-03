// PC keyboard and mouse in game (the Mac App's PC control mode, docs/MAC.md): keys run the game's own PC key bindings, the mouse clicks the world the
// way D2R on PC does (left = move / left skill at the cursor, right = right skill, Shift = stand still), and clicks on panels keep going through the
// Mac pointer path (touch_mac.c), so the console item panels the PC set keeps (dm_padmode.c, gen_romfs_overlay.py PC_TWINS) still work.
// Only while the PC interface set is on screen and a game is running; the front end keeps the keyboard-as-controller mapping (front_mac.m).
// The front end (any thread) queues events; dm_kbm_step drains them on the game main thread at the controller poll (touch_mac.c dm_touch_host_step).
// Game addresses (offsets into the original 3.3.94012 main image):
//   main+0x2086f64  the live key-binding list, 138 x {u32 binding id, u16 VK | 0x1000 Shift | 0x2000 Ctrl | 0x4000 Alt, u16 kind, u16}; the game
//                   fills it with D2's PC defaults on entering a game and its Key Bindings page edits it
//   main+0x2086388  the binding handler table the two runners below take
//   main+0x3700e0   Input_ExecuteHotkey(entry, table, 1): runs a binding (offers it to the interface first)
//   main+0x36ff90   the binding release runner (entry, table, 0): ends a held binding (Shift, Alt, skill hold)
// World clicks go through dm_world_pc_click (dm_world.c: the click entry main+0x4ace20 with the PC action codes).
#include "dm.h"
#include "dm_kbm.h"
#include "dm_cursor.h"
#include "dm_input.h"
#include "dm_uiscreen.h"
#include "dm_actions.h"
#include "dm_cursor_item.h"
#include "dm_hud.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

enum { BIND_LIST = 0x2086f64, BIND_TABLE = 0x2086388, EXEC_HOTKEY = 0x3700e0, RELEASE_HOTKEY = 0x36ff90, BIND_N = 138, BIND_SZ = 10 };
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };
#define HUD_BAND 0.885f   // below this (interface height: the 16:9 band, which on a taller 16:10 screen is not the whole picture) is the PC HUD: orbs, skill buttons, belt, mini panel

// ---- pure helpers (t_kbm) ----
int dm_kbm_vk_from_mac(unsigned short kc) {   // macOS virtual key code -> Windows virtual key (the codes the game's bindings use); 0 = not used
    static const unsigned char letters[] = { 'A','S','D','F','H','G','Z','X','C','V',0,'B','Q','W','E','R','Y','T' };   // kVK_ANSI_A (0) .. kVK_ANSI_T (17)
    if (kc < sizeof letters) return letters[kc];
    switch (kc) {
        case 18: return '1'; case 19: return '2'; case 20: return '3'; case 21: return '4'; case 22: return '6'; case 23: return '5'; case 25: return '9';
        case 26: return '7'; case 28: return '8'; case 29: return '0'; case 31: return 'O'; case 32: return 'U'; case 34: return 'I'; case 35: return 'P';
        case 37: return 'L'; case 38: return 'J'; case 40: return 'K'; case 45: return 'N'; case 46: return 'M';
        case 24: return 0xBB; case 27: return 0xBD; case 30: return 0xDD; case 33: return 0xDB; case 39: return 0xDE; case 41: return 0xBA; case 42: return 0xDC;
        case 43: return 0xBC; case 44: return 0xBF; case 47: return 0xBE; case 50: return 0xC0;
        case 36: case 76: return 0x0D; case 48: return 0x09; case 49: return 0x20; case 51: return 0x08; case 53: return 0x1B; case 117: return 0x2E;
        case 115: return 0x24; case 116: return 0x21; case 119: return 0x23; case 121: return 0x22;
        case 123: return 0x25; case 126: return 0x26; case 124: return 0x27; case 125: return 0x28;
        case 122: return 0x70; case 120: return 0x71; case 99: return 0x72; case 118: return 0x73; case 96: return 0x74; case 97: return 0x75;
        case 98: return 0x76; case 100: return 0x77; case 101: return 0x78; case 109: return 0x79; case 103: return 0x7A; case 111: return 0x7B;
        case 82: return 0x60; case 83: return 0x61; case 84: return 0x62; case 85: return 0x63; case 86: return 0x64; case 87: return 0x65;
        case 88: return 0x66; case 89: return 0x67; case 91: return 0x68; case 92: return 0x69;
        case 56: case 60: return 0x10; case 59: case 62: return 0x11; case 58: case 61: return 0x12;
    }
    return 0;
}
// Bindings a key fires: entries whose VK matches and whose modifier is the one held (an unmodified binding also fires with Shift held, which is
// stand still, so Shift + F1 still picks a skill); a modifier key itself matches on its VK alone. Returns the count, indices in out
int dm_kbm_match(const uint8_t *list, int n, int vk, int mods, int *out, int max) {
    int c = 0, want = (mods & MOD_SHIFT ? 0x1000 : 0) | (mods & MOD_CTRL ? 0x2000 : 0) | (mods & MOD_ALT ? 0x4000 : 0), modkey = vk >= 0x10 && vk <= 0x12;
    for (int i = 0; i < n && c < max; i++) {
        uint16_t key; memcpy(&key, list + i * BIND_SZ + 4, 2); if (key == 0xffff || (key & 0xfff) != vk) continue;
        int m = key & 0xf000; if (!modkey && m != want && !(m == 0 && want == 0x1000)) continue;
        out[c++] = i;
    }
    return c;
}

#if TARGET_OS_OSX && !defined(DM_KBM_PURE_ONLY)   // DM_KBM_PURE_ONLY: the unit test (t_kbm) builds the pure helpers alone
// ---- events from the front end ----
typedef struct { uint8_t op, btn, mods; int16_t vk; float nx, ny; } kev;   // op: 0 move, 1 down, 2 up, 3 key down, 4 key up, 5 wheel (vk = notches)
static struct { kev q[128]; _Atomic unsigned head, tail; } g_q;
static void push(kev e) { unsigned t = atomic_load(&g_q.tail); if (t - atomic_load(&g_q.head) >= 128) return; g_q.q[t % 128] = e; atomic_store(&g_q.tail, t + 1); }
void dm_kbm_mouse(int op, float nx, float ny, int btn, int mods) { push((kev){ (uint8_t)op, (uint8_t)btn, (uint8_t)mods, 0, nx, ny }); }
void dm_kbm_key(int vk, int down, int mods) { if (vk) push((kev){ (uint8_t)(down ? 3 : 4), 0, (uint8_t)mods, (int16_t)vk, 0, 0 }); }
void dm_kbm_wheel(float nx, float ny, int notches) { if (notches) push((kev){ 5, 0, 0, (int16_t)notches, nx, ny }); }

// ---- game main thread ----
static module_t *mainmod(void) { for (int i = 0; i < g_nmod; ++i) if (!strcmp(g_mod[i].name, "main")) return &g_mod[i]; return NULL; }
static int hold_binding(uint32_t id) { return id == 34 || id == 36 || id == 37; }   // Ctrl, Shift (stand still), Alt (show items): held, so their key up ends them; the
// release runner re-fires every other binding (a panel key would toggle its panel shut again)
static void run_binding(uint8_t *b, int idx, int down) {
    uint8_t *e = b + BIND_LIST + idx * BIND_SZ; uint32_t bid; memcpy(&bid, e, 4); if (!down && !hold_binding(bid)) return; static int lg; if (lg++ < 100) { uint32_t id; memcpy(&id, e, 4); LOG("PC input: binding %u %s", id, down ? "down" : "up"); }
    if (down) ((int (*)(void *, uint64_t, uint64_t))(b + EXEC_HOTKEY))(e, (uint64_t)(uintptr_t)(b + BIND_TABLE), 1);
    else ((int (*)(void *, uint64_t, uint64_t))(b + RELEASE_HOTKEY))(e, (uint64_t)(uintptr_t)(b + BIND_TABLE), 0);
}
// The PC bindings for the console pages (C, I, Q, T, O) only open them: pressed while their page shows, they close it, as the PC toggles do. The
// console's Back closes the group's frame but not a page a PC binding opened (the skill tree stays on screen without its frame), so closing runs
// the game's own Clear Screen binding (Space), and the empty frame then goes (dm_kbm_step)
enum { BIND_CLEAR = 38 };
static int bind_index(uint8_t *b, uint32_t id) { for (int i = 0; i < BIND_N; i++) { uint32_t v; memcpy(&v, b + BIND_LIST + i * BIND_SZ, 4); if (v == id) return i; } return -1; }
static const struct { uint32_t id; const char *panel; } k_toggle[] = {
    { 0, "CharacterStatsPanel" }, { 1, "PlayerInventoryExpansionLayout" }, { 1, "PlayerInventoryOriginalLayout" }, { 4, "QuestLogPanelExpansion" },
    { 4, "QuestLogPanelOriginal" }, { 12, "SkillsTreePanel" }, { 54, "HirelingInventoryPanel" } };
static int toggle_closes(uint8_t *b, int idx) {
    uint32_t id; memcpy(&id, b + BIND_LIST + idx * BIND_SZ, 4);
    for (unsigned i = 0; i < sizeof k_toggle / sizeof *k_toggle; i++) if (k_toggle[i].id == id && dm_ui_panel_shown(k_toggle[i].panel)) return 1;
    return 0;
}
static int switches_page(uint8_t *b, int idx) {   // a page key while another page shows: the console draws one page at a time, so that one closes first
    uint32_t id; int page = 0, other = 0; memcpy(&id, b + BIND_LIST + idx * BIND_SZ, 4);
    for (unsigned i = 0; i < sizeof k_toggle / sizeof *k_toggle; i++) { if (k_toggle[i].id == id) page = 1; else if (dm_ui_panel_shown(k_toggle[i].panel)) other = 1; }
    return page && other;
}
static float ui_to_screen_y(float uy) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); return dm_screen_real(&rw, &rh) && rh > 0 && h > 0 ? (uy * h + y) / rh : uy; }

static struct { int down, ui; } g_btn[2];                    // left, right: held, where the press went (0 world, 1 panel, 2 PC HUD, 3 an item area: done at the press)
static int g_back, g_a, g_y_hold;                             // polls left to hold the console's Back (B), A, Y
// An NPC talking (the speech scroll) or its menu (Talk / Trade / Cancel): console panels the PC bindings don't close. The game's Clear Screen
// (Esc, Space) leaves both up and Return opens the chat over the speech, which then stops scrolling for good, so while one shows Esc and Clear
// Screen press the console's Back as the controller and touch paths do (touch_mac.c), and Return does nothing
static int npc_dialog_shown(void) { return dm_ui_panel_shown("ScrollSpeechPanel") || dm_ui_panel_shown("NpcDialogPanel"); }
static float g_x = .5f, g_y = .5f;                            // cursor, interface-normalized
static int g_mods;
static struct { int16_t vk; int n; int idx[4]; } g_keys[16];   // keys held and the bindings they fired

// Esc with nothing open: the console interface's game menu is the pause page of the panel group, reached as scenario.py open_pause does: + opens
// the group on its default tab, three tabs to the left is the pause page. g_esc: 1-2 + held, 3 waiting for the group (at most a second), 4.. L
// pressed three times (held 3 polls, released 3); 0 = idle
static int g_esc, g_esc_n;
static void esc_pump(uint64_t *buttons) {
    if (g_esc <= 2) { *buttons |= DM_BTN_PLUS; g_esc++; g_esc_n = 0; return; }
    if (g_esc == 3) { if (dm_ui_panel_shown("UISwitcher")) { g_esc = 4; g_esc_n = 0; } else if (++g_esc_n > 60) { LOG("PC input: Esc found no panel group to open"); g_esc = 0; } return; }
    if (g_esc_n % 6 < 3) *buttons |= DM_BTN_L;
    if (++g_esc_n >= 18) g_esc = 0;
}
// A click on an item area (dm_ui_item_at_now 1, or 7 on a vendor's shelf), as the game's own PC item tooltips state it ("Right Click to Use / Buy /
// Open / Read", "Shift + Left Click to Equip / Unequip", "Ctrl + Left Click to Move / Sell / Drop / Unequip"). The game's cursor goes to the point
// first (dm_ui_hover_now: the item under it is the one acted on, and the console's buttons act there), then:
//   left click          the console's A: picks the item up onto the cursor, or puts down / swaps the one held (in between it follows the pointer,
//                       whose hover moves the game's cursor); any click while an item is held puts it down
//   Shift + left click  equip / unequip; a potion goes into the belt
//   Ctrl + left click   unequip a worn item; with the stash, the cube or a vendor open the console's held Y (move it to the other side; sell); else
//                       the console's Y (drop)
//   right click         buy from a vendor, open the cube, use / read; nothing on an item to wear (Shift's on PC)
static void item_click(int bt, int hit) {
    int W = *(int32_t *)(mainmod()->base + 0x211da38), H = *(int32_t *)(mainmod()->base + 0x211da3c); dm_ui_hover_now((int)(g_x * W), (int)(g_y * H));
    dm_hud_info hi; dm_hud_get(&hi); int held = hi.valid && hi.holding;
    dm_cursor_item_info ci; int on = dm_cursor_item(&ci); const char *what;
    if (bt == 0 && (held || !(g_mods & (MOD_SHIFT | MOD_CTRL)))) { g_a = 3; what = held ? "put_down" : on ? "pick_up" : "nothing"; }
    else if (bt == 0 && !on) what = "no_item";
    else if (bt == 0 && (g_mods & MOD_CTRL)) {
        if (ci.container == DM_ITEM_AT_EQUIP) { dm_item_unequip(ci.id); what = "unequip"; }
        else if ((dm_ui_panel_state() & (DM_PS_BANK | DM_PS_CUBE | DM_PS_VENDOR)) || ci.container == DM_ITEM_AT_STASH || ci.container == DM_ITEM_AT_CUBE) { g_y_hold = 40; what = "move"; }
        else if (ci.container == DM_ITEM_AT_INVENTORY) { g_y_hold = 3; what = "drop"; }
        else what = "none_here";
    } else if (bt == 0) {
        if (ci.container == DM_ITEM_AT_EQUIP) { dm_item_unequip(ci.id); what = "unequip"; }
        else if (ci.equippable) { dm_item_equip(ci.id); what = "equip"; }
        else if (ci.container == DM_ITEM_AT_INVENTORY && ci.belt_room) { dm_belt_put(ci.id); what = "belt"; }
        else what = "none_here";
    } else if (!on || held) what = "nothing";
    else if (ci.container == DM_ITEM_AT_EQUIP || (ci.equippable && ci.container != DM_ITEM_AT_SHOP)) what = "wearable";
    else { dm_ui_use_item(); what = ci.container == DM_ITEM_AT_SHOP ? "buy" : "use"; }
    static int lg; if (lg++ < 100) LOG("PC item: %s click%s%s at %.3f,%.3f (area %d) on %s -> %s", bt ? "right" : "left", g_mods & MOD_SHIFT ? " +Shift" : "", g_mods & MOD_CTRL ? " +Ctrl" : "", g_x, g_y, hit, on ? ci.code : "-", what);
    DM_EV("pc_item", "btn:s mods:d item:s at:d did:s", bt ? "right" : "left", g_mods & (MOD_SHIFT | MOD_CTRL), on ? ci.code : "", on ? ci.container : 0, what);
}
static void world_click(int btn, int phase) {   // phase 0 press, 1 held (every poll, as the game's own PC held logic sends it every frame), 2 release
    static const int act[2][3] = { { 0, 1, 2 }, { 3, 4, 5 } };   // the click entry's PC actions (dm_world.c dm_world_pc_click)
    dm_world_pc_click(act[btn][phase], g_mods & MOD_SHIFT ? 1 : 0, g_x, ui_to_screen_y(g_y));
}
int dm_kbm_step(uint64_t *buttons) {   // returns 1 when PC input is live (the caller skips its own keyboard mapping)
    int dm_uiscreen_ingame(void), dm_ui_panel_state(void), dm_game_paused(void);
    uint8_t *b = NULL; module_t *m = mainmod(); if (m) b = m->base;
    int live = b && dm_padmode_pc() && dm_uiscreen_ingame();
    // Anything non-resident open: a press the hit test can't place (0: a button, a list row, the inside of a panel, the NPC speech) belongs to it, not to the world
    int open = live && atomic_load(&g_q.head) != atomic_load(&g_q.tail)
        && (dm_ui_panel_state() || dm_ui_open_panels() || dm_game_paused() || dm_ui_panel_shown("UISwitcher") || npc_dialog_shown());
    unsigned h;
    while ((h = atomic_load(&g_q.head)) != atomic_load(&g_q.tail)) {
        kev e = g_q.q[h % 128]; atomic_store(&g_q.head, h + 1);
        if (!live) { static int nl; if (nl++ < 5) LOG("PC input: event %d dropped (PC set %d, in game %d)", e.op, dm_padmode_pc(), dm_uiscreen_ingame()); continue; }
        g_mods = e.mods;
        if (e.op <= 2) {
            if (e.op != 2 || e.btn > 1) { g_x = e.nx; g_y = e.ny; }
            // The game's cursor: W x H is the interface (on a 16:10 screen the 16:9 band, dm_uiscreen.c), so over the interface it takes the
            // interface position, as the panels' own pointer pump writes it (dm_cursor.c); over the world it keeps the whole-picture height
            int W = *(int32_t *)(b + 0x211da38), H = *(int32_t *)(b + 0x211da3c); if (W > 0 && H > 0) dm_cursor_set((int)(g_x * W), (int)((open || g_y > HUD_BAND ? g_y : ui_to_screen_y(g_y)) * H));
            if (e.op == 0) { if (!e.btn) { dm_touch_host_live(3, g_x, g_y); dm_ui_ptr(DM_UP_MOVE, g_x, g_y, 0); } else if (g_btn[0].down && g_btn[0].ui == 1) dm_touch_host_live(1, g_x, g_y); continue; }   // hover: the panels' tooltips and the PC HUD's   // hover, or a drag on a panel (a held world click just follows the cursor)
            if (e.btn > 1) { int k[4], n = dm_kbm_match(b + BIND_LIST, BIND_N, 0x100 + e.btn - 2, g_mods, k, 4); for (int i = 0; i < n; i++) run_binding(b, k[i], e.op == 1); continue; }   // mouse 3-5: their bindings
            int bt = e.btn;
            if (e.op == 1) {
                // Where the press lands: an open panel (the Mac pointer path, as for the console panels), the PC HUD band at the bottom (the game's own
                // mouse messages, which its buttons take), else the world. dm_ui_item_at_now: 3 = outside every panel; 0 = nothing it could classify,
                // which is also a clickable widget or the inside of a panel, so with a panel open it goes to the panel. The HUD band is compared in
                // interface height (on 16:10 the band's bottom part, not the picture's)
                int hit = dm_ui_item_at_now(g_x, g_y), ps = dm_ui_panel_state(), hud = g_y > HUD_BAND, where = (hit != 0 && hit != 3) || ps ? 1 : hud ? 2 : hit == 0 && open ? 1 : 0;
                static int lh; if (lh++ < 50) LOG("PC input: button %d at %.3f,%.3f -> hit %d panels %d open %d -> %s", bt, g_x, g_y, hit, ps, open, where == 1 ? "panel" : where == 2 ? "HUD" : "world");
                g_btn[bt].down = 1; g_btn[bt].ui = where;
                if (where == 1 && (hit == 1 || hit == 7)) { g_btn[bt].ui = 3; item_click(bt, hit); }
                else if (where == 1) { if (bt == 0) dm_touch_host_live(0, g_x, g_y); }
                else if (where == 2) dm_ui_ptr(bt ? DM_UP_RDOWN : DM_UP_LDOWN, g_x, g_y, 0);
                else world_click(bt, 0);
            } else if (g_btn[bt].down) {
                if (g_btn[bt].ui == 1) { if (bt == 0) dm_touch_host_live(2, g_x, g_y); } else if (g_btn[bt].ui == 2) dm_ui_ptr(bt ? DM_UP_RUP : DM_UP_LUP, g_x, g_y, 0); else if (!g_btn[bt].ui) world_click(bt, 2);
                g_btn[bt].down = 0;
            }
            continue;
        }
        if (e.op == 5) { int k[4], n = dm_kbm_match(b + BIND_LIST, BIND_N, e.vk > 0 ? 0x103 : 0x104, g_mods, k, 4); for (int i = 0; i < n; i++) run_binding(b, k[i], 1); continue; }   // wheel over the world: previous / next skill
        if (e.vk == 0x1B) {   // Esc, as on PC: after death the death screen's own Continue (its button's message); in the game menu and Options, back (B);
                              // an NPC's speech or menu, back (held a few polls); else closes what is open (Clear Screen, as the toggles above), or opens the game menu
            if (e.op == 3) { int ps = dm_ui_panel_state(), k;
                if (ps & DM_PS_DEAD) dm_ui_send_queue("YouDiedModalMessage:Continue");
                else if (dm_game_paused()) *buttons |= DM_BTN_B;
                else if (npc_dialog_shown()) { g_back = 3; LOG("PC input: Esc on an NPC dialog -> back"); }
                else if (ps || dm_ui_open_panels() || dm_ui_panel_shown("UISwitcher")) { if ((k = bind_index(b, BIND_CLEAR)) >= 0) run_binding(b, k, 1); else *buttons |= DM_BTN_B; }
                else if (!g_esc) g_esc = 1; }
            continue;
        }
        if (e.op == 3) {
            int s = -1; for (int i = 0; i < 16; i++) if (g_keys[i].vk == e.vk) s = -2; if (s == -2) continue;   // auto-repeat: the binding is already held
            for (int i = 0; i < 16 && s < 0; i++) if (!g_keys[i].vk) s = i; if (s < 0) continue;
            int dlg = npc_dialog_shown();
            if (dlg && e.vk == 0x0D) { static int lr; if (lr++ < 20) LOG("PC input: Return on an NPC dialog: ignored (the chat would freeze the speech)"); continue; }
            g_keys[s].vk = e.vk; g_keys[s].n = dm_kbm_match(b + BIND_LIST, BIND_N, e.vk, g_mods, g_keys[s].idx, 4);
            for (int i = 0; i < g_keys[s].n; i++) { int k = g_keys[s].idx[i], c = bind_index(b, BIND_CLEAR);
                if (dlg && k == c) { g_back = 3; continue; }   // Clear Screen (Space) on an NPC dialog: back, as Esc
                if (toggle_closes(b, k)) { if (c >= 0) run_binding(b, c, 1); continue; }
                if (c >= 0 && switches_page(b, k)) run_binding(b, c, 1);
                run_binding(b, k, 1); }
        } else for (int i = 0; i < 16; i++) if (g_keys[i].vk == e.vk) { for (int j = 0; j < g_keys[i].n; j++) run_binding(b, g_keys[i].idx[j], 0); g_keys[i].vk = 0; }
    }
    if (!live) { memset(g_btn, 0, sizeof g_btn); g_esc = 0; g_back = g_a = g_y_hold = 0; return 0; }
    if (g_esc) esc_pump(buttons);
    if (g_back > 0) { *buttons |= DM_BTN_B; g_back--; }
    if (g_a > 0) { *buttons |= DM_BTN_A; g_a--; }
    if (g_y_hold > 0) { *buttons |= DM_BTN_Y; g_y_hold--; }
    // The PC bindings open and close the console pages of the panel group (I = inventory, C, T, Q; Space closes them all), but closing a page
    // leaves the group's frame (UISwitcher, its tab bar and border) on screen, empty, and taking the keys. With no page in it for a few polls,
    // close the frame as the game does
    { static int empty; if (!g_esc && dm_ui_panel_shown("UISwitcher") && !dm_ui_open_panels() && !dm_game_paused()) { if (++empty == 4) { dm_ui_send_queue("PanelManager:ClosePanel:UISwitcher"); LOG("PC input: the panel group was left empty: closed"); } } else empty = 0; }
    // The PC hover: the unit under the pointer is the game's hover target every poll (none over the interface), and a button held from a world press
    // sends the held action every poll; both are what D2R's own PC mouse code does every frame (dm_world.c)
    { int ui = g_y > HUD_BAND || dm_ui_panel_state() || dm_ui_open_panels() || dm_game_paused() || dm_ui_panel_shown("UISwitcher") || npc_dialog_shown();
      dm_world_pc_hover(ui ? -1.f : g_x, ui_to_screen_y(g_y)); }
    for (int bt = 0; bt < 2; bt++) if (g_btn[bt].down && !g_btn[bt].ui) world_click(bt, 1);
    return 1;
}
int dm_kbm_live(void) { int dm_uiscreen_ingame(void); return dm_padmode_pc() && dm_uiscreen_ingame(); }   // any thread: in game with the PC interface
int dm_kbm_shift(void) { return dm_kbm_live() && (g_mods & MOD_SHIFT); }   // game main thread: Shift held in PC mode (dm_cursor.c: items compare, as on PC)
#else   // iOS: no PC mode (touch and controller only); the unit test
void dm_kbm_mouse(int op, float nx, float ny, int btn, int mods) { (void)op; (void)nx; (void)ny; (void)btn; (void)mods; }
void dm_kbm_key(int vk, int down, int mods) { (void)vk; (void)down; (void)mods; }
void dm_kbm_wheel(float nx, float ny, int notches) { (void)nx; (void)ny; (void)notches; }
int dm_kbm_step(uint64_t *buttons) { (void)buttons; return 0; }
int dm_kbm_live(void) { return 0; }
int dm_kbm_shift(void) { return 0; }
#endif
