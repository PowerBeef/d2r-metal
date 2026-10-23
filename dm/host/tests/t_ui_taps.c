// 界面点按单测（直接包含 dm_cursor.c，同 t_minimap_lift.c）：讲话框单独开着时点它的坐标（ui_wtap_pump）、一下点击还在途时又来的点按要排队（dm_ui_touch / ut_promote）、
// 落点查询之后、主线程作答之前又来的按下查询不能被当成落点查询（dm_ui_item_query）。取手柄处的发送端在这里按真实的相位规则模拟：相 2 发按下，相 3 发抬起。
// UI tap unit test (includes dm_cursor.c directly, like t_minimap_lift.c): the click position when the speech box is open on its own (ui_wtap_pump), taps arriving while a click is still in flight must queue (dm_ui_touch / ut_promote),
// and a press query arriving after a drop-point query and before the main thread answers must not be taken for a drop query (dm_ui_item_query). The sender at the controller read is simulated here with the real phase rules: phase 2 sends the press, phase 3 the release.
#include "../dm_cursor.c"
#include <time.h>
module_t g_mod[MAX_MOD]; int g_nmod;
uint64_t dm_guest_frame(void) { return 100; }
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
int dm_padmode_busy(void) { return 0; }
int dm_uiscreen_ingame(void) { return 1; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = 0; *y = 0; *w = 2360; *h = 1326; }
void dm_uiscreen_set_ingame(int on) { (void)on; }
void dm_custom_on_uimsg(const char *c, const char *a, const char *g) { (void)c; (void)a; (void)g; }
void dm_elysium_arm(int on) { (void)on; }
static int world_taps;
unsigned dm_world_tap(float x, float y) { (void)x; (void)y; world_taps++; return 0; }

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL "); } else printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

// 假控件：+8 名字、+0x50 可见、+0x58 子控件表、+0x60 子控件数；虚表 +0x48 = 命中函数（一律命中，记下问的点）
// Fake widgets: +8 name, +0x50 visible, +0x58 child table, +0x60 child count; vtable +0x48 = hit function (always hits, records the point asked)
static int hit_calls, hit_x, hit_y;
static int hit_any(uint64_t w, uint64_t xy) { (void)w; hit_calls++; hit_x = (int32_t)(uint32_t)xy; hit_y = (int32_t)(uint32_t)(xy >> 32); return 1; }
static void *vt[16];
static _Alignas(8) unsigned char root[0x100], sp[0x100], border[0x100];
static uint64_t root_ch[1], sp_ch[1];
static void widget(unsigned char *p, const char *name) { memset(p, 0, 0x100); *(const char **)(p + 8) = name; p[0x50] = 1; *(void ***)p = vt; }
static void xy_of(uint64_t r, float *x, float *y) { *x = ((r >> 16) & 0xffff) / 65535.0f; *y = (r & 0xffff) / 65535.0f; }

// 取手柄处那一段的相位规则（dm_ui_mouse_pump）：先接上排着的，相 2 发按下转相 3，相 3 发抬起转相 0。记下每次抬起（= 一次完整点击）的坐标
// The phase rules of the controller-read sender (dm_ui_mouse_pump): first take up whatever is waiting, phase 2 sends the press and moves to 3, phase 3 sends the release and moves to 0. Record the coordinates of each release (= one complete click)
static int downs, ups; static float up_x[16], up_y[16], down_x, down_y;
static void frame(void) {
    ut_promote(); int ph = atomic_load(&g_ut_phase); float x, y; xy_of(atomic_load(&g_ut_xy), &x, &y);
    if (ph == 2) { int e = 2; if (atomic_compare_exchange_strong(&g_ut_phase, &e, 3)) { downs++; down_x = x; down_y = y; } }
    else if (ph == 3) { int e = 3; if (atomic_compare_exchange_strong(&g_ut_phase, &e, 0)) { if (ups < 16) { up_x[ups] = x; up_y[ups] = y; } ups++; } }
}
static void frames(int n) { for (int i = 0; i < n; i++) frame(); }
static void reset(void) { ut_reset(); downs = ups = 0; atomic_store(&g_fe_opt_open, 0); atomic_store(&g_fe_back, 0); }
static int near(float a, float b) { return fabsf(a - b) < 0.001f; }

static void speech_box(void) {
    printf("讲话框单独开着时点它：点击落在命中检测用的那一点 | tapping the speech box open on its own: the click lands on the point the hit test used\n");
    vt[0x48 / 8] = (void *)hit_any;
    widget(root, "Root"); widget(sp, "ScrollSpeechPanel"); widget(border, "DialogBorder");
    root_ch[0] = (uint64_t)(uintptr_t)sp; *(uint64_t *)(root + 0x58) = (uint64_t)(uintptr_t)root_ch; *(uint64_t *)(root + 0x60) = 1;
    sp_ch[0] = (uint64_t)(uintptr_t)border; *(uint64_t *)(sp + 0x58) = (uint64_t)(uintptr_t)sp_ch; *(uint64_t *)(sp + 0x60) = 1;
    static const float c[][2] = { { 0.50f, 0.80f }, { 0.25f, 0.60f }, { 0.90f, 0.95f }, { 0.50f, -0.10f } };
    int W = 2360, H = 1326;
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        reset(); hit_calls = 0; world_taps = 0;
        dm_ui_world_tap(c[i][0], c[i][1], 0.5f, 0.5f);
        ui_wtap_pump(NULL, (uint64_t)(uintptr_t)root, W, H);
        float ux, uy; xy_of(atomic_load(&g_ut_xy), &ux, &uy); int ph = atomic_load(&g_ut_phase);
        float ex = c[i][0], ey = c[i][1] < 0 ? 0 : c[i][1];   // 界面点击坐标夹在 [0,1] 里 | UI click coordinates are clamped to [0,1]
        CHECK(hit_calls >= 1 && abs(hit_x - (int)lroundf(c[i][0] * W)) <= 1 && abs(hit_y - (int)lroundf(c[i][1] * H)) <= 1, "u=(%.2f,%.2f) 命中检测问的是 (%d,%d) | the hit test asks (%d,%d)", c[i][0], c[i][1], hit_x, hit_y, hit_x, hit_y);
        CHECK(ph == 2 && world_taps == 0 && near(ux, ex) && near(uy, ey), "点击排在 (%.3f,%.3f)，要 (%.3f,%.3f)，没交给世界 | click queued at (%.3f,%.3f), want (%.3f,%.3f), not passed to the world", ux, uy, ex, ey, ux, uy, ex, ey);
        // 发出去的像素坐标 = 命中检测的那个像素 | the pixel sent = the pixel of the hit test
        int px = (int)(ux * W), py = (int)(uy * H);
        if (c[i][1] >= 0) CHECK(px == hit_x && py == hit_y, "发送端换回的像素 (%d,%d) = 命中像素 | the pixel the sender converts back (%d,%d) = the hit pixel", px, py, px, py);
    }
    reset();
}

static void taps(void) {
    printf("一下点击在途时又来的点按 | taps arriving while a click is in flight\n");
    reset(); dm_ui_touch(0, .10f, .10f); dm_ui_touch(1, .12f, .12f); dm_ui_touch(2, .12f, .12f); frames(4);
    CHECK(downs == 1 && ups == 1 && near(up_x[0], .12f) && near(up_y[0], .12f) && !dm_ui_touch_active(), "单独一下：按下、移动、抬起 → 抬起处一次点击，之后空闲 | a single tap: down, move, up -> one click at the release point, then idle");

    reset(); dm_ui_touch(0, .10f, .10f); dm_ui_touch(2, .10f, .10f); dm_ui_touch(0, .60f, .60f); dm_ui_touch(2, .60f, .60f);
    { float x, y; xy_of(atomic_load(&g_ut_xy), &x, &y); CHECK(atomic_load(&g_ut_phase) == 2 && near(x, .10f), "第二下按下没有改写待发的第一下（相 %d，坐标 %.2f） | the second press did not overwrite the pending first tap (phase %d, x %.2f)", atomic_load(&g_ut_phase), x, atomic_load(&g_ut_phase), x); }
    frames(8);
    CHECK(ups == 2 && near(up_x[0], .10f) && near(up_x[1], .60f), "同一帧里的两下点按 → 两次点击，按先后（%d 次：%.2f, %.2f） | two taps within one frame -> two clicks, in order (%d: %.2f, %.2f)", ups, up_x[0], up_x[1], ups, up_x[0], up_x[1]);
    CHECK(downs == ups && !dm_ui_touch_active(), "每次按下都配了抬起，最后空闲 | every press got its release, idle at the end");

    reset(); dm_ui_touch(0, .10f, .10f); dm_ui_touch(2, .10f, .10f); frame();   // 第一下的按下已发，抬起待发（相 3） | the first tap's press has been sent, its release is pending (phase 3)
    dm_ui_touch(0, .30f, .30f);
    CHECK(atomic_load(&g_ut_phase) == 3, "相 3 时来的按下不丢掉待发的抬起 | a press arriving in phase 3 does not drop the pending release");
    frame(); CHECK(ups == 1 && near(up_x[0], .10f), "抬起仍落在第一下的点上（%.2f） | the release still lands on the first tap's point (%.2f)", up_x[0], up_x[0]);
    frame(); CHECK(atomic_load(&g_ut_phase) == 1, "第二下还按着：接上后是「按着」（相 1），不发点击 | the second tap is still held: taken up as held (phase 1), no click");
    dm_ui_touch(1, .35f, .35f); dm_ui_touch(2, .35f, .35f); frames(3);
    CHECK(ups == 2 && near(up_x[1], .35f), "第二下抬起后照常点在抬起处（%.2f） | after its release the second tap clicks at the release point as usual (%.2f)", up_x[1], up_x[1]);

    reset(); dm_ui_touch(0, .10f, .10f); dm_ui_touch(2, .10f, .10f); dm_ui_touch(0, .50f, .50f); dm_ui_touch(1, .55f, .55f); dm_ui_touch(3, 0, 0); frames(6);
    CHECK(ups == 1 && near(up_x[0], .10f) && !dm_ui_touch_active(), "作废（改成滚动）只作废还按着的那一下，在途的照发 | cancel (turned into a scroll) voids only the gesture still held; the one in flight is still sent");

    reset(); dm_ui_touch(0, .05f, .05f); dm_ui_touch(2, .05f, .05f);
    for (int i = 0; i < 12; i++) { dm_ui_touch(0, .1f + i * .05f, .5f); dm_ui_touch(2, .1f + i * .05f, .5f); }
    CHECK(dm_ui_touch_active(), "排着的点按算「界面触摸进行中」 | queued taps count as UI touch in progress");
    frames(40);
    CHECK(ups == 1 + UT_QN && downs == ups && !dm_ui_touch_active(), "连点 13 下：在途 1 下 + 排满 %d 下，其余丢掉，按下与抬起成对（%d 次） | 13 taps in a row: 1 in flight + a full queue of %d, the rest dropped, presses and releases paired (%d)", UT_QN, ups, UT_QN, ups);

    reset(); atomic_store(&g_fe_opt_open, 1);   // 模式页：牌子以外 = 返回，按接上那一刻判 | mode page: outside the plaques = back, judged when taken up
    dm_ui_touch(0, .3f, 1000 / 2160.0f); dm_ui_touch(2, .3f, 1000 / 2160.0f); dm_ui_touch(0, .3f, .1f); dm_ui_touch(2, .3f, .1f); frames(8);
    CHECK(ups == 1 && atomic_load(&g_fe_back) == 1, "模式页：先点牌子、再点牌子以外 → 牌子点一次，随后返回（点击 %d，返回 %d） | mode page: a plaque, then outside -> the plaque clicked once, then back (clicks %d, back %d)", ups, atomic_load(&g_fe_back), ups, atomic_load(&g_fe_back));
    reset();
    dm_ui_touch(0, .2f, .2f); dm_ui_touch(2, .2f, .2f); dm_ui_touch(0, .4f, .4f); ut_reset();
    CHECK(!dm_ui_touch_active() && atomic_load(&g_ut_phase) == 0, "换模式清理把排着的一并丢掉 | the mode-switch cleanup drops the queued ones too");
}

static void drop_query(void) {
    printf("落点查询之后、主线程作答之前的按下查询 | a press query after a drop-point query, before the main thread answers\n");
    atomic_store(&g_drop_last, 5); atomic_store(&g_sn_new, 0);
    unsigned s1 = dm_ui_drop_query(0.4f, 0.35f, 0.40f);
    uint64_t w = atomic_load(&g_iq_xy);
    CHECK((w & IQ_DROP) && (w >> 32 & 0xffff) == (uint64_t)(0.40f * 65535 + .5f) && atomic_load(&g_drop_last) == 5, "落点查询：字里带落点标志和手指 y，不清上一次的落点 | drop query: the word carries the drop flag and the finger y, the previous drop result is kept");
    unsigned s2 = dm_ui_item_query(0.8f, 0.8f);
    w = atomic_load(&g_iq_xy);
    CHECK(s2 == s1 + 1 && !(w & IQ_DROP) && (w >> 32) == 0, "随后的按下查询按按下答（不带落点标志） | the following press query is answered as a press (no drop flag)");
    CHECK(atomic_load(&g_drop_last) == -1 && atomic_load(&g_sn_new) == 1, "按下查询照旧作废上一次的落点与吸附 | the press query invalidates the previous drop result and snap as before");
}

int main(void) {
    speech_box(); taps(); drop_query();
    printf("== t_ui_taps %d 项，失败 %d | %d checks, %d failed ==\n", checks, fails, checks, fails);
    return fails != 0;
}
