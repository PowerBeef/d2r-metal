// The button legend's append gate (svc_legend_append, at main+0xaa9f88) and the legend pump on the production code, with the game's legend functions
// replaced by stand-ins that model the legend's eight inline slots: an append past eight is counted as an overrun, the crash the gate prevents.
// Covers: the game's own entries stop at eight; our hints make room for the game's entry, newest first; another legend is never touched; a text update
// at eight entries still goes through; our pump never adds a ninth. Ported from D2R Mobile's test (upstream fb3baaf).
#include "../dm_padmode.c"
module_t g_mod[MAX_MOD]; int g_nmod;
static uint64_t frame = 100;
uint64_t dm_guest_frame(void) { return frame; }
static int checks, fails, appends, removals, overruns;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { uint64_t widget[0x1818 / 8]; uint64_t entries[16][8]; char text[16][64]; } test_legend;
static test_legend a, other;
static uint64_t root[16], overlay[0x448 / 8], children[1];
static uint64_t addr(const void *p) { return (uint64_t)(uintptr_t)p; }
static uint64_t count(test_legend *lg) { return lg->widget[0x90 / 8]; }
static void write_entry(test_legend *lg, unsigned i, uint32_t glyph, const char *text) {
    memmove(lg->text[i], text, strlen(text) + 1); memset(lg->entries[i], 0, sizeof lg->entries[i]);
    lg->entries[i][0] = glyph; lg->entries[i][1] = 1; lg->entries[i][2] = addr(lg->text[i]); lg->entries[i][3] = strlen(text);
}
static void make(void *out, int act, int unused, void *view) {   // the keycap glyph is the action id here
    (void)unused; struct { const char *p; uint64_t n; } *s = view; uint64_t *e = out; memset(e, 0, 64);
    e[0] = (uint32_t)act; e[1] = 1; e[2] = addr(s->p); e[3] = s->n; e[4] = 1ull << 63;   // the text is the test's own: nothing for the game's allocator to free
}
static void append(uint64_t vector, uint64_t entry) {
    test_legend *lg = (test_legend *)(uintptr_t)(vector - 0x88); uint64_t *e = (uint64_t *)(uintptr_t)entry; unsigned n = (unsigned)count(lg);
    if (n >= LG_CAP) overruns++;
    if (n >= 16) abort();
    write_entry(lg, n, (uint32_t)e[0], (const char *)(uintptr_t)e[2]); lg->widget[0x90 / 8]++; appends++;
}
static void add(uint64_t widget, void *entry) {   // the game's add: a same-keycap entry only gets its text, otherwise the append through the gate
    test_legend *lg = (test_legend *)(uintptr_t)widget; uint64_t *e = entry;
    for (unsigned i = 0; i < count(lg); i++) if (lg->entries[i][0] == e[0]) { write_entry(lg, i, (uint32_t)e[0], (const char *)(uintptr_t)e[2]); return; }
    uint64_t regs[31] = { 0 }; regs[0] = widget + 0x88; regs[1] = addr(entry); regs[19] = widget; svc_legend_append(regs);
}
static void remove_entry(uint64_t widget, int act) {
    test_legend *lg = (test_legend *)(uintptr_t)widget;
    for (unsigned i = 0; i < count(lg); i++) if (lg->entries[i][0] == (uint32_t)act) {
        for (unsigned j = i; j + 1 < count(lg); j++) write_entry(lg, j, (uint32_t)lg->entries[j + 1][0], lg->text[j + 1]);
        lg->widget[0x90 / 8]--; removals++; return; }
}
static void native_add(test_legend *lg, int act, const char *text) { uint64_t e[8]; struct { const char *p; uint64_t n; } sv = { text, strlen(text) }; make(e, act, 0, &sv); add(addr(lg), e); }
static int has(test_legend *lg, int act, const char *text) {
    for (unsigned i = 0; i < count(lg); i++) if (lg->entries[i][0] == (uint32_t)act) return !text || !strcmp(lg->text[i], text);
    return 0;
}
static void reset(void) {
    memset(&a, 0, sizeof a); memset(&other, 0, sizeof other); a.widget[0x88 / 8] = addr(a.entries); other.widget[0x88 / 8] = addr(other.entries);
    memset(root, 0, sizeof root); memset(overlay, 0, sizeof overlay);
    root[0x58 / 8] = addr(children); root[0x60 / 8] = 1; children[0] = addr(overlay); overlay[1] = addr("ControllerOverlay");
    ((uint8_t *)overlay)[0x50] = 1; overlay[0x440 / 8] = addr(&a);
    *(uint64_t *)(g_b + 0x2f05458) = addr(root);
    g_lg_n = g_lg_on_n = g_lh_n = 0; g_lg_frame = 0; frame = 100; g_cur = 1; overruns = appends = removals = 0;
}
static void pump_with(int n, const int *acts) { frame++; for (int i = 0; i < n; i++) dm_padmode_legend(acts[i], acts[i] == 30 ? "ours A" : acts[i] == 31 ? "ours B" : "ours C"); legend_pump(g_b, frame); }
int main(void) {
    g_b = calloc(1, 0x2f05460 + 16); if (!g_b) return 2;
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = g_b;
    g_lg_api.base = g_b; g_lg_api.make = make; g_lg_api.add = add; g_lg_api.remove = remove_entry; g_lg_api.append = append;

    // The game's own entries stop at eight
    reset(); for (int i = 0; i < 9; i++) native_add(&a, i, "native");
    CHECK(count(&a) == 8); CHECK(overruns == 0); CHECK(!has(&a, 8, NULL));
    // A text update at eight entries still goes through (it never reaches the gate)
    native_add(&a, 3, "updated"); CHECK(count(&a) == 8); CHECK(has(&a, 3, "updated"));

    // Our two hints fill the legend to eight; the game's next entry takes the place of our newest hint
    reset(); for (int i = 0; i < 6; i++) native_add(&a, i, "native");
    const int two[] = { 30, 31 }; pump_with(2, two); CHECK(count(&a) == 8); CHECK(has(&a, 30, "ours A") && has(&a, 31, "ours B"));
    native_add(&a, 7, "Compare"); CHECK(count(&a) == 8); CHECK(overruns == 0); CHECK(has(&a, 7, "Compare")); CHECK(has(&a, 30, NULL)); CHECK(!has(&a, 31, NULL));
    // The pump never adds a ninth: our removed hint stays out while the legend is full
    for (int k = 0; k < 5; k++) pump_with(2, two);
    CHECK(count(&a) == 8); CHECK(overruns == 0); CHECK(!has(&a, 31, NULL)); CHECK(has(&a, 7, NULL));
    // The game removes its entry: our hint comes back on the next pump
    remove_entry(addr(&a), 7); pump_with(2, two); CHECK(count(&a) == 8); CHECK(has(&a, 31, "ours B"));

    // Two of the game's entries in a row: both our hints make room, newest first
    reset(); for (int i = 0; i < 6; i++) native_add(&a, i, "native"); pump_with(2, two);
    native_add(&a, 7, "Compare"); native_add(&a, 9, "Pick up"); CHECK(count(&a) == 8); CHECK(overruns == 0);
    CHECK(has(&a, 7, NULL) && has(&a, 9, NULL)); CHECK(!has(&a, 30, NULL) && !has(&a, 31, NULL));
    // A third with nothing of ours left: skipped
    native_add(&a, 10, "Advanced"); CHECK(count(&a) == 8); CHECK(overruns == 0); CHECK(!has(&a, 10, NULL));

    // Another legend (a page's own) full at eight: skipped, and our hints on the bottom legend are left alone
    reset(); for (int i = 0; i < 6; i++) native_add(&a, i, "native"); pump_with(2, two);
    for (int i = 0; i < 9; i++) native_add(&other, i, "page");
    CHECK(count(&other) == 8); CHECK(overruns == 0); CHECK(has(&a, 30, NULL) && has(&a, 31, NULL)); CHECK(count(&a) == 8);
    // A game entry that already sits on another keycap of ours: our text is never mistaken for it (only entries with our exact text are removed)
    reset(); for (int i = 0; i < 6; i++) native_add(&a, i, "native"); pump_with(2, two);
    native_add(&a, 31, "game text"); CHECK(has(&a, 31, "game text")); CHECK(count(&a) == 8);
    native_add(&a, 11, "New"); CHECK(count(&a) == 8); CHECK(overruns == 0); CHECK(has(&a, 11, NULL)); CHECK(has(&a, 31, "game text"));

    free(g_b); printf("t_legend: %d checks, %d failures (appends %d, removals %d)\n", checks, fails, appends, removals); return fails != 0;
}
