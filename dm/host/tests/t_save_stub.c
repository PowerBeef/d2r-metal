// t_save_hooks 的桩：存档钩子单测里用不到的游戏侧函数（测试走的路径一个都不调它们），加上前端存档失败通知的强实现（数次数）。
// Stubs for t_save_hooks: the game-side functions the save hook unit test does not reach (none of them is called on the paths the test drives), plus a strong implementation of the front end's save-failure notification (counts the calls).
#include <stdint.h>
#include <stddef.h>
int g_nfail, g_lasterr, g_nsave_req;
void front_save_failed(int err) { g_nfail++; g_lasterr = err; }   // 替掉 hle_fs.c 的弱实现（同 t_fs_stub.c） | replaces the weak implementation in hle_fs.c (as in t_fs_stub.c)
int dm_custom_lang_explicit(void) { return -1; }
int dm_custom_voice_explicit(void) { return -1; }
int dm_sys_lang(void) { return 0; }
int dm_request_save(void) { return ++g_nsave_req; }
uint64_t dm_diag_srv_item(uint32_t id) { (void)id; return 0; }
int dm_custom_get(const char *k) { (void)k; return 1; }
uint64_t dm_guest_frame(void) { return 0; }
int dm_ui_panel_state(void) { return 0; }
void dm_game_notice(uint8_t *b, const char *t) { (void)b; (void)t; }
int dm_lootfilter_hidden(uint8_t *b, uint64_t it) { (void)b; (void)it; return 0; }
const char *dm_str(uint8_t *b, const char *k) { (void)b; return k; }
const char *dm_untag(const char *s) { return s; }
int dm_ui_top_chk_on(void) { return 0; }
void dm_ui_top_chk(uint64_t r, const char *n, uint64_t g) { (void)r; (void)n; (void)g; }
int64_t dm_user_clock(void) { return 0; }
int dm_user_clock_fixed(void) { return 1; }
void dm_test_clock_shift(int64_t d) { (void)d; }
