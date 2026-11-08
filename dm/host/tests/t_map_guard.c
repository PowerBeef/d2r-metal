// The map pump's two guards, on the production code with authored objects:
// - outside a run the game's map switch (main+0x40f780) is never called and a pending request is dropped (upstream 64156a3: Quick Restart with the
//   full-screen map crashed at main+0x4117cc). The base here is plain heap, so a call into it would crash this test;
// - a cutscene that closed the minimap gets the map key pressed for the player in touch mode, at most three times, two seconds apart, only with the
//   game's "AutoMap Open On Load" on, never with a panel open, during the cutscene, in controller mode, or after leaving the run (upstream d1275c7).
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
static int ingame = 1, padmode; static uint64_t frame = 1000;
uint64_t dm_guest_frame(void) { return frame; }
int dm_uiscreen_ingame(void) { return ingame; }
int dm_padmode_current(void) { return padmode; }
int dm_padmode_busy(void) { return 0; }
void dm_hud_get(dm_hud_info *h) { memset(h, 0, sizeof *h); h->valid = 1; h->area = 1; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = 0; *y = 0; *w = 2420; *h = 1360; }
void dm_uiscreen_set_ingame(int on) { (void)on; }
uint64_t now_ns(void) { return frame * 16666667; }
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg) { (void)cat; (void)act; (void)arg; }
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static uint64_t ptr(void *p) { return (uint64_t)(uintptr_t)p; }
static uint64_t pressed(void) { return atomic_exchange(&g_alone_mask, 0); }
int main(void) {
    unsigned char *base = calloc(1, 0x2f05460); if (!base) return 2;
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base; module_t *m = &g_mod[0];
    static _Alignas(8) unsigned char root[128], am[160]; static uint64_t child;
    child = ptr(am); *(uint64_t *)am = ptr(base) + 0x20c58a0; *(const char **)(am + 8) = "AutoMap";
    *(uint64_t *)(root + 0x58) = ptr(&child); *(uint64_t *)(root + 0x60) = 1;
    int32_t *cfg = (int32_t *)(base + 0x2d479f4), *mini = (int32_t *)(base + 0x2d5c748), *open_on_load = (int32_t *)(base + 0x2d485b4);

    // In a run with Automap Size at Full Screen, already shown full screen: nothing to switch
    *cfg = 0; *mini = 0; am[0x50] = 1; ui_map_pump(m, ptr(root));
    CHECK(g_map_want == 1); CHECK(dm_ui_map_is_full() == 1);
    // Leaving the run with the map out of step (the minimap shown, the player wants full screen) and a request pending: no call into the game
    *mini = 1; dm_ui_map_full(1); ingame = 0;
    for (int i = 0; i < 5; i++) { frame++; ui_map_pump(m, ptr(root)); }
    CHECK(atomic_load(&g_map_req) == -1); CHECK(dm_ui_map_is_full() == 0);
    ingame = 1; *mini = 0;   // back in step for the rest

    // No cutscene: a closed map is left alone
    am[0x50] = 0; *open_on_load = 1; frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);
    // A cutscene: nothing while it plays, the map key after it ends
    dm_ui_on_uimsg("ProgressionMessage", "PlayVideo", ""); frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);
    dm_ui_on_uimsg("Cinematics", "MovieComplete", ""); frame++; ui_map_pump(m, ptr(root)); CHECK(pressed() == DM_BTN_MINUS);
    frame += 60; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);                  // two seconds between presses
    frame += 61; ui_map_pump(m, ptr(root)); CHECK(pressed() == DM_BTN_MINUS);
    frame += 121; ui_map_pump(m, ptr(root)); CHECK(pressed() == DM_BTN_MINUS);
    frame += 121; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0); CHECK(atomic_load(&g_map_reopen) == 0);   // three presses at most
    frame += 500; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);

    // A panel open holds it back; it presses once the panel closes, and stands down when the map stays open for two seconds
    dm_ui_on_uimsg("ProgressionMessage", "PlayVideo", ""); dm_ui_on_uimsg("Cinematics", "MovieComplete", "");
    ui_panel_note("OpenPanel", "PlayerInventoryExpansionLayout"); frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);
    ui_panel_note("ClosePanel", "PlayerInventoryExpansionLayout"); frame++; ui_map_pump(m, ptr(root)); CHECK(pressed() == DM_BTN_MINUS);
    am[0x50] = 1; frame++; ui_map_pump(m, ptr(root)); frame += 119; ui_map_pump(m, ptr(root)); CHECK(atomic_load(&g_map_reopen) != 0);
    frame++; ui_map_pump(m, ptr(root)); CHECK(atomic_load(&g_map_reopen) == 0); CHECK(pressed() == 0);
    am[0x50] = 0; frame += 500; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);   // closed again later, no cutscene: left alone

    // AutoMap Open On Load off: the game's own behaviour, no press
    *open_on_load = 0; dm_ui_on_uimsg("ProgressionMessage", "PlayVideo", ""); dm_ui_on_uimsg("Cinematics", "MovieComplete", "");
    frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0); CHECK(atomic_load(&g_map_reopen) == 0);
    *open_on_load = 1;
    // Controller mode: the pump's controller branch returns first; leaving touch mode disarms
    dm_ui_on_uimsg("ProgressionMessage", "PlayVideo", ""); dm_ui_on_uimsg("Cinematics", "MovieComplete", "");
    padmode = 1; frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0);
    dm_ui_auto_mode_leave(); padmode = 0; frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0); CHECK(atomic_load(&g_map_reopen) == 0);
    // Leaving the run disarms too
    dm_ui_on_uimsg("ProgressionMessage", "PlayVideo", ""); dm_ui_on_uimsg("FrontEndNavigation", "ToFrontEnd", "");
    frame += 200; ui_map_pump(m, ptr(root)); CHECK(pressed() == 0); CHECK(atomic_load(&g_map_reopen) == 0); CHECK(atomic_load(&g_map_movie) == 0);

    free(base); printf("t_map_guard: %d checks, %d failures\n", checks, fails); return fails != 0;
}
