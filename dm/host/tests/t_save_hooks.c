// 存档事务 + 提交前 / 提交结果钩子单测（秒级）：直接包含 hle_fs.c，链真的 dm_vault.c / dm_pouch.c / dm_pages.c（t_save_mods.c），在临时目录里走真实的挂载 / 写 / Commit。
// 每个用例一个子进程（模块的静态量、测试开关只读一次）。期望值全部按语义手写，不调被测实现算：
//   被拒的提交（游戏写失败 / 后面的钩子写失败）之后，前面钩子核对通过的存入 / 收进囊 / 翻页回到待证明，下一次提交照样落盘（F-SAVE-2）；
//   放到背包的那件等提交成了才交出去发 0x63；本事务已有写失败就不跑提交前钩子、照样报一次「没成」；
//   个人仓库侧文件写不成 → 这次提交被拒（F-SAVE-6）；
//   残留的 .new 删不掉 → 提交失败、一段连续失败只通知前端一次；重挂时先处理 .new、工作目录不删；能清掉以后，有没提交改动的工作目录整个挪到 .work.orphan（F-SAVE-8）；
//   正常退出后重挂不留 .work.orphan（「没改过」记号 / 逐个文件相同）；提交后留一份 .prev，克隆前有改过的文件才发一次 F_BARRIERFSYNC（F-SAVE-12）；
//   事务目录里建文件 / 写方式打开失败、空句柄写都让本事务被拒，「已存在 / 不存在」不算；列目录时别的项刚好没了不影响查找（F-SAVE-9）；
//   fd ≥ 4096 打开不越界（F-SAVE-13，本测试带 ASan）；设置文件关文件失败不换掉原文件（F-SAVE-14）；
//   死亡那几拍（没有受控玩家）不关钩子、之后同一局同一角色不重读，没证明完的收进囊 / 存入 / 翻页留着；换了一局或换了角色才重读；角色删了就丢掉它的待证明、不拒提交（F-SAVE-1）；
//   编年史各格各证、同一格守先后，有待落盘的每 2 秒再请存盘（F-SAVE-17）；收进囊按「之后序列化的角色档」指纹证明，同格同种符文挡不住；提交没成指纹作废（F-SAVE-15）。
// Save transaction + pre-commit / commit outcome hook unit test (takes seconds): includes hle_fs.c directly and links the real dm_vault.c / dm_pouch.c / dm_pages.c (t_save_mods.c), running real mounts / writes / Commits in a temp directory.
// One child process per case (the modules' statics and test switches are read once). All expected values are hand-written from the semantics, not computed by calling the implementation under test:
//   after a refused commit (a game write failed / a later hook failed to write), the stores / stows / page turns that earlier hooks had proven go back to pending, and the next commit still persists them (F-SAVE-2);
//   an item released to the inventory is handed out for 0x63 only once its commit went through; a commit whose transaction already had a write failure does not run the pre-commit hooks and still reports "not committed" once;
//   the personal stash side file failing to write refuses this commit (F-SAVE-6);
//   a leftover .new that cannot be deleted fails the commit and the front end is notified once per run of failures; a remount deals with .new first and keeps the work directory; once it can be cleared, a work directory with uncommitted changes is moved whole to .work.orphan (F-SAVE-8);
//   a remount after a normal exit leaves no .work.orphan ("unchanged" mark / identical files); a commit keeps one .prev, and before the clone one F_BARRIERFSYNC is issued only when some file changed (F-SAVE-12);
//   a failed create / open for writing in the transaction directory and a null-handle write refuse the transaction, "already exists / does not exist" do not; another entry vanishing during a listing does not break a lookup (F-SAVE-9);
//   opening with fd >= 4096 stays in bounds (F-SAVE-13, this test is built with ASan); a settings file whose close fails does not replace the original (F-SAVE-14);
//   the dying ticks (no controlled player) keep the hooks enabled, and the same character in the same session is not reread, so unproven stows / stores / page turns survive; only a new session or a different character rereads; a deleted character drops its pending entries without refusing the commit (F-SAVE-1);
//   Chronicle cells are proven independently with the order kept within a cell, and a save is requested every 2 seconds while anything is pending (F-SAVE-17); a stow is proven by the fingerprint of a character file serialized afterwards, so an identical rune in the same cell cannot block it; a commit that does not go through drops the fingerprints (F-SAVE-15).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/xattr.h>
#include <fcntl.h>
static ssize_t fake_getxattr(const char *path, const char *name, void *value, size_t size, u_int32_t position, int options);
static int fake_fcntl(int fd, int cmd, ...);
#define getxattr fake_getxattr
#define fcntl fake_fcntl
#include "../hle_fs.c"
#undef getxattr
#undef fcntl
ssize_t getxattr(const char *path, const char *name, void *value, size_t size, u_int32_t position, int options);
static int g_race, g_nbarrier;   // g_race = 1：名字以 .tmp 结尾的项在列出之后、读客体名之前没了 | g_race = 1: entries named *.tmp vanish after being listed and before their guest name is read
static ssize_t fake_getxattr(const char *path, const char *name, void *value, size_t size, u_int32_t position, int options) {
    size_t L = strlen(path); if (g_race && L > 4 && !strcmp(path + L - 4, ".tmp")) { errno = ENOENT; return -1; }
    return getxattr(path, name, value, size, position, options); }
static int fake_fcntl(int fd, int cmd, ...) { if (cmd == F_BARRIERFSYNC) g_nbarrier++; va_list a; va_start(a, cmd); long arg = va_arg(a, long); va_end(a); return fcntl(fd, cmd, arg); }

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }   // 日志时间戳与读路径的桩 | stubs for log timestamps and the read path
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; } void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;
void tv_setup(const char *name); void tv_load(void); void tv_deposit(int kind, int idx, const uint8_t *bits, int len); void tv_withdraw(int kind, int idx);
void tv_age_all(double secs); void tv_state(int *npend, int *ddirty, int *nv, int *nd); int tv_mem_has(int kind, int idx);
void tp_setup(const char *ch); void tp_stow(uint32_t code, const uint8_t *bits, int len); void tp_release(const uint8_t *placed, int plen, uint32_t nid);
int tp_take_ready(uint32_t *ids, int max); void tp_state(int *nu, int *nud, int *nup, int *udirty);
void tg_setup(const char *ch); void tg_preflip(int from, int to, const uint8_t *B, int bl); void tg_flip(int from, int to, const uint8_t *A, int al, const uint8_t *B, int bl);
void tg_state(int *ppend, int *pdirty, int *pmore, int *pcur); int tg_file_has(const char *path, const uint8_t *bits, int len, int *slot_out, int *cur_out, int *pend_out);
int tv_enter(uint64_t G, uint32_t tq, const char *who); void tv_tick0(void); void tv_resave(uint32_t tq); int tv_loaded(void);
int tp_enter(uint64_t G, uint32_t tq, const char *name); void tp_tick0(void); int tp_hook_on(void);
int tg_enter(uint64_t G, uint32_t tq, const char *name); void tg_tick0(void); int tg_hook_on(void); void tg_set_inflight(int on); void tg_dirty(void);
extern int g_nsave_req;
void *dm_memfind(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)
static char root[1024], save[1100], work[1200];
#define STASH "ModernSharedStashSoftCoreV2"
static void mkp(const char *p) { char d[1400]; strlcpy(d, p, sizeof d); for (char *s = d + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(d, 0755); *s = '/'; } mkdir(d, 0755); }
static void putb(const char *p, const void *b, size_t n) { char d[1400]; strlcpy(d, p, sizeof d); char *sl = strrchr(d, '/'); *sl = 0; mkp(d); FILE *f = fopen(p, "wb"); fwrite(b, 1, n, f); fclose(f); }
static uint8_t *readb(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); *n = 0; if (!f) return NULL; fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *d = malloc((size_t)L + 1); *n = fread(d, 1, (size_t)L, f); fclose(f); return d; }
static int file_has(const char *dir, const char *rel, const uint8_t *bits, size_t len) { char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, rel); size_t n; uint8_t *d = readb(p, &n); if (!d) return -1; int r = dm_memfind(d, n, bits, len) != NULL; free(d); return r; }
static int exists(const char *dir, const char *rel) { char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, rel); return access(p, F_OK) == 0; }
static Result gwrite(const char *gpath, const uint8_t *b, size_t n) {   // 游戏写存档：打开（须已存在）→ 定长 → 写 → 关；打开失败照样拿空句柄写（同游戏） | the game's save write: open (must exist) -> set size -> write -> close; on a failed open it writes with the null handle anyway (as the game does)
    uint64_t h = 0; Result r = h_fs_OpenFile(&h, gpath, MODE_WRITE); if (r) h = 0;
    h_fs_SetFileSize(h, (int64_t)n); Result w = h_fs_WriteFile(h, 0, b, n, NULL); if (h) h_fs_CloseFile(h); return r ? r : w; }
static uint8_t X[40], Y[40], R[14], R2[14], A[30], Bb[30];   // X / Y 暗金件，R 符文（背包格 1），R2 同一颗放到背包格 2，A / Bb 个人仓库两页各一件 | X / Y unique items, R a rune (inventory cell 1), R2 the same rune placed in inventory cell 2, A / Bb one item on each of two personal stash pages
static int g_stamp = 1;   // 角色档头里的存盘时刻（真档每次存盘都变）：测试里要「同样内容、下一次存盘」时加一 | the save time in the character file header (a real file changes it at every save): tests bump it for "same content, a later save"
static size_t d2s(uint8_t *o, const uint8_t *i1, size_t n1, const uint8_t *i2, size_t n2) { char hdr[17]; snprintf(hdr, sizeof hdr, "D2S-HEADER-%04d", g_stamp); size_t k = 16; memcpy(o, hdr, 16);
    if (i1) { memcpy(o + k, i1, n1); k += n1; } if (i2) { memcpy(o + k, i2, n2); k += n2; } memcpy(o + k, "TRAILER", 7); return k + 7; }
static void init_items(void) { for (int i = 0; i < 40; i++) { X[i] = (uint8_t)(0x10 + i); Y[i] = (uint8_t)(0x80 + i); } for (int i = 0; i < 14; i++) R[i] = R2[i] = (uint8_t)(0xc0 + i); R[2] = R2[2] = 0x20; R2[5] = 0x55;
    for (int i = 0; i < 30; i++) { A[i] = (uint8_t)(0x40 + i); Bb[i] = (uint8_t)(0x60 ^ i); } }
static void base_save(const uint8_t *items, size_t n) {   // 挂载前盘上的已提交存档：Joe/Joe.d2s + 共享仓库 | the committed save on disk before mounting: Joe/Joe.d2s + the shared stash
    uint8_t b[256]; size_t k = d2s(b, items, n, NULL, 0); char p[1400]; snprintf(p, sizeof p, "%s/Joe/Joe.d2s", save); putb(p, b, k);
    snprintf(p, sizeof p, "%s/" STASH "/" STASH ".d2i", save); putb(p, "STASH-v1", 8); }
static void tmount(void) { setenv("DM_SAVE", save, 1); CHECK(h_fs_MountSave("save", NULL) == RES_OK, "挂载事务式存档 | mount the transactional save"); }
static void restart(void) { g_nmnt = 0; CHECK(h_fs_MountSave("save", NULL) == RES_OK, "重挂（进程重启） | remount (process restart)"); }
static void put_d2s(const uint8_t *i1, size_t n1, const uint8_t *i2, size_t n2) { uint8_t b[256]; size_t k = d2s(b, i1, n1, i2, n2); CHECK(gwrite("save:/Joe/Joe.d2s", b, k) == RES_OK, "游戏写 Joe.d2s | the game writes Joe.d2s"); }
// 测试用的第 5 个钩子（真机上是 iCloud 并回 [4]，本测试没链）：数调用次数、按需在提交前把本事务记成写失败，记下提交结果 | a fifth hook for the test (the iCloud merge [4] in the product, not linked here): counts calls, can mark this transaction as failed before the commit, and records the outcome
static int g_t4_pre, g_t4_poison, g_t4_post[8], g_t4_npost;
static void t4_pre(const char *wd) { (void)wd; g_t4_pre++; if (g_t4_poison) dm_fs_save_poison(EIO); }
static void t4_post(int ok) { if (g_t4_npost < 8) g_t4_post[g_t4_npost++] = ok; }
static void t4_install(void) { g_dm_pre_commit_hook[4] = t4_pre; g_dm_post_commit_hook[4] = t4_post; }

// ---- F-SAVE-2：游戏写失败让提交被拒，编年史存入回到待落盘、下一次提交落盘 | F-SAVE-2: a failed game write refuses the commit; the Chronicle store goes back to pending and the next commit persists it
static void c_refused_vault(void) {
    setenv("DM_TEST_FS_FAIL", "write:2:28", 1);
    base_save(X, 40); tmount(); tv_setup(STASH); tv_load(); tv_deposit(1, 100, X, 40);
    put_d2s(NULL, 0, NULL, 0); gwrite("save:/" STASH "/" STASH ".d2i", (const uint8_t *)"STASH-v2", 8);   // 第 2 次写注入失败 | the 2nd write fails by injection
    int nf0 = g_nfail, np, dd, nv, nd; CHECK(h_fs_Commit("save") != RES_OK && g_nfail == nf0 + 1, "第 1 次提交被拒、前端通知 1 次 | commit 1 refused, front end notified once");
    tv_state(&np, &dd, &nv, &nd); CHECK(np == 1 && nd == 0, "存入仍待落盘（%d 条）、盘上状态没并进（%d 件） | the store is still pending (%d) and not merged into the on-disk state (%d)", np, nd, np, nd);
    put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "第 2 次提交成功 | commit 2 succeeds");
    CHECK(file_has(save, STASH ".swvault", X, 40) == 1 && file_has(save, "Joe/Joe.d2s", X, 40) == 0, "已提交：X 在 .swvault、不在角色档 | committed: X is in .swvault, not in the character file");
    restart(); tv_load(); CHECK(tv_mem_has(1, 100), "重启后编年史里有 X | after a restart the Chronicle holds X");
}
// ---- F-SAVE-2：后面的钩子（百宝囊）写失败，前面钩子（编年史）写好的存入也不算数、下一次照样落盘 | F-SAVE-2: a later hook (pouch) fails to write; the store the earlier hook (Chronicle) wrote does not count either and is persisted next time
static void c_refused_crosshook(void) {
    setenv("DM_TEST_POUCH_SAVE_FAIL", "1", 1);
    uint8_t ci[60]; memcpy(ci, X, 40); memcpy(ci + 40, R, 14); base_save(ci, 54); tmount(); tv_setup(STASH); tv_load(); tp_setup("Joe");
    tv_deposit(1, 100, X, 40); tp_stow(0x31307220, R, 14); put_d2s(NULL, 0, NULL, 0);
    CHECK(h_fs_Commit("save") != RES_OK, "第 1 次提交被拒（百宝囊侧文件写失败） | commit 1 refused (the pouch side file failed to write)");
    int np, dd, nv, nd, nu, nud, nup, ud; tv_state(&np, &dd, &nv, &nd); tp_state(&nu, &nud, &nup, &ud);
    CHECK(np == 1 && nd == 0 && nup == 1 && nud == 0, "两边都回到待证明（编年史 %d 条、百宝囊 %d 条） | both are back to pending (Chronicle %d, pouch %d)", np, nup, np, nup);
    put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "第 2 次提交成功 | commit 2 succeeds");
    CHECK(file_has(save, STASH ".swvault", X, 40) == 1 && file_has(save, "Joe/Joe.swpouch", R, 14) == 1, "已提交：X 在 .swvault、R 在 .swpouch | committed: X in .swvault, R in .swpouch");
}
// ---- F-SAVE-2：百宝囊收进囊在被拒的提交里证明过，回到待证明，下一次落盘 | F-SAVE-2: a stow proven in a refused commit goes back to pending and is persisted next time
static void c_refused_pouch(void) {
    setenv("DM_TEST_FS_FAIL", "write:2:28", 1);
    base_save(R, 14); tmount(); tp_setup("Joe"); tp_stow(0x31307220, R, 14);
    put_d2s(NULL, 0, NULL, 0); gwrite("save:/" STASH "/" STASH ".d2i", (const uint8_t *)"S2", 2);
    CHECK(h_fs_Commit("save") != RES_OK, "第 1 次提交被拒 | commit 1 refused");
    int nu, nud, nup, ud; tp_state(&nu, &nud, &nup, &ud); CHECK(nup == 1 && nud == 0, "收进囊仍待证明 | the stow is still pending");
    put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "第 2 次提交成功 | commit 2 succeeds");
    CHECK(file_has(save, "Joe/Joe.swpouch", R, 14) == 1 && file_has(save, "Joe/Joe.d2s", R, 14) == 0, "已提交：R 在 .swpouch、不在角色档 | committed: R in .swpouch, not in the character file");
    restart(); tp_setup("Joe"); tp_state(&nu, &nud, &nup, &ud); CHECK(nu == 1, "重启后囊里 1 件 | after a restart the pouch holds 1 item");
}
// ---- F-SAVE-2：个人仓库翻页在被拒的提交里证明过，回到待定，下一次落定 | F-SAVE-2: a page turn proven in a refused commit stays pending and is settled next time
static void pages_base(void) { base_save(A, 30); tmount(); tg_setup("Joe"); tg_preflip(0, 1, Bb, 30); CHECK(h_fs_Commit("save") == RES_OK, "第 0 次提交：翻页前（第 2 页 = B，当前第 1 页） | commit 0: before the turn (page 2 = B, current page 1)"); }
static void pages_after(const char *tag) {   // 期望：翻页后的状态已提交——格里是 B，A 在某个页槽，B 不在页槽，当前第 2 页，没有待定记录 | expected: the post-turn state is committed -- B in the grid, A in a page slot, B in no slot, current page 2, no pending record
    char p[1400]; snprintf(p, sizeof p, "%s/Joe/Joe.swpage", save); int sa, sb, cur, pend; int ha = tg_file_has(p, A, 30, &sa, &cur, &pend), hb = tg_file_has(p, Bb, 30, &sb, &cur, &pend);
    CHECK(file_has(save, "Joe/Joe.d2s", Bb, 30) == 1 && ha == 1 && hb == 0 && cur == 1 && pend == 0, "%s：格里 B、A 在页槽 %d、B 不在页槽、当前第 %d 页、待定 %d | %s: B in the grid, A in slot %d, B in no slot, current page %d, pending %d", tag, sa + 1, cur + 1, pend, tag, sa + 1, cur + 1, pend); }
static void c_refused_pages(void) {
    setenv("DM_TEST_FS_FAIL", "write:2:28", 1);
    pages_base(); tg_flip(0, 1, A, 30, Bb, 30); put_d2s(Bb, 30, NULL, 0); gwrite("save:/" STASH "/" STASH ".d2i", (const uint8_t *)"S2", 2);
    CHECK(h_fs_Commit("save") != RES_OK, "第 1 次提交被拒 | commit 1 refused");
    int pp, pd, pm, pc; tg_state(&pp, &pd, &pm, &pc); CHECK(pp == 1 && pm == 1, "翻页仍待定、要再请存盘 | the turn is still pending and asks for another save");
    put_d2s(Bb, 30, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "第 2 次提交成功 | commit 2 succeeds");
    tg_state(&pp, &pd, &pm, &pc); CHECK(pp == 0, "翻页已落定 | the turn is settled"); pages_after("被拒之后的那次提交 | the commit after the refusal");
}
// ---- 放到背包：钩子证明了也要等提交成了才交出去发 0x63；被拒的那次不交 | release to the inventory: even when proven, it is handed out for 0x63 only after the commit went through; not on a refused one
static void c_release_waits(void) {
    base_save(R, 14); tmount(); tp_setup("Joe"); t4_install(); tp_stow(0x31307220, R, 14); put_d2s(NULL, 0, NULL, 0);
    CHECK(h_fs_Commit("save") == RES_OK && file_has(save, "Joe/Joe.swpouch", R, 14) == 1, "收进囊落盘 | the stow is persisted");
    tp_release(R2, 14, 77); put_d2s(R2, 14, NULL, 0); g_t4_poison = 1;
    uint32_t ids[4]; CHECK(h_fs_Commit("save") != RES_OK && tp_take_ready(ids, 4) == 0, "后面的钩子让提交被拒：放到背包的不交出去 | a later hook refuses the commit: the release is not handed out");
    int nu, nud, nup, ud; tp_state(&nu, &nud, &nup, &ud); CHECK(nup == 1 && nud == 1 && file_has(save, "Joe/Joe.swpouch", R, 14) == 1, "仍待证明、盘上状态和已提交的侧文件都还有 R | still pending; the on-disk state and the committed side file still hold R");
    g_t4_poison = 0; put_d2s(R2, 14, NULL, 0);
    int n = (h_fs_Commit("save") == RES_OK) ? tp_take_ready(ids, 4) : -1; CHECK(n == 1 && ids[0] == 77, "提交成了才交出新物品 id 77（交出 %d 个） | only after the commit succeeds is new item id 77 handed out (%d handed out)", n, n);
    CHECK(file_has(save, "Joe/Joe.swpouch", R, 14) == 0 && file_has(save, "Joe/Joe.d2s", R2, 14) == 1, "已提交：侧文件里没有 R、角色档背包里是放好的那串 | committed: R is gone from the side file and the placed bitstream is in the character file");
}
// ---- 本事务已有写失败：不跑提交前钩子，照样报一次「没成」；之后正常提交报「成了」 | the transaction already had a write failure: the pre-commit hooks do not run and "not committed" is still reported once; a normal commit afterwards reports success
static void c_poison_skips(void) {
    setenv("DM_TEST_FS_FAIL", "write:1:28", 1);
    base_save(NULL, 0); tmount(); t4_install(); { uint8_t b[64]; size_t k = d2s(b, NULL, 0, NULL, 0); CHECK(gwrite("save:/Joe/Joe.d2s", b, k) != RES_OK, "第 1 次写注入失败 | the 1st write fails by injection"); }
    CHECK(h_fs_Commit("save") != RES_OK && g_t4_pre == 0 && g_t4_npost == 1 && g_t4_post[0] == 0, "被拒：钩子没跑（%d 次）、报了一次没成 | refused: the hook did not run (%d calls), one failure reported", g_t4_pre, g_t4_pre);
    put_d2s(X, 40, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK && g_t4_pre == 1 && g_t4_npost == 2 && g_t4_post[1] == 1, "下一次：钩子跑一次、报成了 | next: the hook runs once and success is reported");
}
// ---- F-SAVE-6：个人仓库侧文件写不成 → 提交被拒、前端有通知、翻页仍待定；障碍去掉后下一次落定 | F-SAVE-6: the personal stash side file cannot be written -> the commit is refused, the front end is notified, the turn stays pending; once the obstacle is gone the next commit settles it
static void c_pages_write_fail(void) {
    pages_base(); tg_flip(0, 1, A, 30, Bb, 30); put_d2s(Bb, 30, NULL, 0);
    char t[1400]; snprintf(t, sizeof t, "%s/Joe/Joe.swpage.tmp", work); mkdir(t, 0755);   // 临时文件位置上放个目录：fopen "wb" 失败（代替盘满） | a directory where the temp file goes: fopen "wb" fails (standing in for a full disk)
    int nf0 = g_nfail, pp, pd, pm, pc; Result c = h_fs_Commit("save"); tg_state(&pp, &pd, &pm, &pc);
    CHECK(c != RES_OK && g_nfail == nf0 + 1 && pp == 1 && pm == 1, "提交被拒、前端通知 1 次、翻页仍待定、再请存盘 | commit refused, front end notified once, turn still pending, another save requested");
    char p[1400]; snprintf(p, sizeof p, "%s/Joe/Joe.swpage", save); int sa, cur, pend; tg_file_has(p, A, 30, &sa, &cur, &pend);
    CHECK(file_has(save, "Joe/Joe.d2s", A, 30) == 1 && cur == 0, "已提交的仍是翻页前那一对（格里 A、第 1 页） | the committed pair is still the pre-turn one (A in the grid, page 1)");
    rmdir(t); put_d2s(Bb, 30, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "障碍去掉后提交成功 | with the obstacle gone the commit succeeds"); pages_after("障碍去掉后 | after the obstacle is gone");
}
// ---- F-SAVE-8：删不掉的 .new | F-SAVE-8: a .new that cannot be deleted
static void stuck_new(int on) { char neu[1300], sub[1400], f[1500]; snprintf(neu, sizeof neu, "%s.new", save); snprintf(sub, sizeof sub, "%s/locked", neu); snprintf(f, sizeof f, "%s/x", sub);
    if (on) { mkp(sub); putb(f, "x", 1); chmod(sub, 0555); } else chmod(sub, 0755); }
static void c_stuck_new(void) {
    base_save(NULL, 0); tmount(); stuck_new(1); int nf0 = g_nfail;
    put_d2s(X, 40, NULL, 0); CHECK(h_fs_Commit("save") != RES_OK && g_nfail == nf0 + 1, "提交失败、前端通知 1 次 | commit fails, front end notified once");
    put_d2s(X, 40, NULL, 0); CHECK(h_fs_Commit("save") != RES_OK && g_nfail == nf0 + 1, "同一段失败里再失败不再通知 | another failure in the same run is not reported again");
    CHECK(file_has(save, "Joe/Joe.d2s", X, 40) == 0 && file_has(work, "Joe/Joe.d2s", X, 40) == 1, "进度只在工作目录里 | the progress is only in the work directory");
    restart(); CHECK(g_mnt[0].ro && file_has(work, "Joe/Joe.d2s", X, 40) == 1 && g_nfail == nf0 + 2, "重挂时 .new 还卡着：只读挂载，工作目录原样留着、进度还在 | remount with .new still stuck: read-only mount, the work directory is left as is with the progress");
    stuck_new(0); restart(); char orph[1300]; snprintf(orph, sizeof orph, "%s.work.orphan", save);
    CHECK(!g_mnt[0].ro && file_has(orph, "Joe/Joe.d2s", X, 40) == 1, "能清掉 .new 后：工作目录挪到 .work.orphan，进度在里面 | once .new can be cleared: the work directory moved to .work.orphan with the progress inside");
    CHECK(file_has(save, "Joe/Joe.d2s", X, 40) == 0 && file_has(work, "Joe/Joe.d2s", X, 40) == 0, "不自动放回：已提交版与新工作目录都没有那份进度 | not put back automatically: neither the committed version nor the new work directory has that progress");
    put_d2s(Y, 40, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "之后照常提交 | commits work afterwards"); stuck_new(1);
    put_d2s(Y, 40, NULL, 0); CHECK(h_fs_Commit("save") != RES_OK && g_nfail == nf0 + 3, "成功之后的新一段失败再通知一次 | a new run of failures after a success is reported again"); stuck_new(0);
}
// ---- F-SAVE-8：正常退出后重挂不留 .work.orphan；没提交就没了才留，只留最近一份 | F-SAVE-8: a remount after a normal exit leaves no .work.orphan; only a run that ended without committing leaves one, and only the latest is kept
static void c_relaunch(void) {
    base_save(NULL, 0); tmount(); char orph[1300]; snprintf(orph, sizeof orph, "%s.work.orphan", save);
    put_d2s(X, 40, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "提交 X | commit X"); restart(); CHECK(access(orph, F_OK) != 0, "正常退出后重挂：没有 .work.orphan | remount after a normal exit: no .work.orphan");
    { uint8_t b[256]; size_t k = d2s(b, X, 40, Y, 40); char p[1400]; snprintf(p, sizeof p, "%s/Joe/Joe.d2s", save); restart(); putb(p, b, k); }   // 启动时有人改了已提交版（如 iCloud 下载），工作目录还是旧的 | something changed the committed version at launch (e.g. an iCloud download) while the work directory is the old one
    restart(); CHECK(access(orph, F_OK) != 0, "已提交版在启动时被改过、工作目录没改：不当成没提交的进度 | the committed version changed at launch and the work directory did not: not taken for uncommitted progress");
    removexattr(work, FS_CLEAN_MARK, XATTR_NOFOLLOW); restart(); CHECK(access(orph, F_OK) != 0, "没有记号（旧版本留下的）但逐个文件相同：照旧删掉 | no mark (left by an older version) but identical files: deleted as before");
    put_d2s(Y, 40, NULL, 0); restart(); CHECK(file_has(orph, "Joe/Joe.d2s", Y, 40) == 1, "写了没提交就重启：留在 .work.orphan | written but not committed, then restarted: kept in .work.orphan");
    put_d2s(X, 40, NULL, 0); restart(); CHECK(file_has(orph, "Joe/Joe.d2s", X, 40) == 1 && file_has(orph, "Joe/Joe.d2s", Y, 40) == 0, "再来一次：只留最近的一份 | once more: only the latest is kept");
}
// ---- F-SAVE-12：提交后留一份 .prev；克隆前改过的文件逐个 fsync、再发一次 F_BARRIERFSYNC | F-SAVE-12: a commit keeps one .prev; before the clone the changed files are fsynced and one F_BARRIERFSYNC is issued
static void c_prev_sync(void) {
    base_save(NULL, 0); tmount(); char prev[1300]; snprintf(prev, sizeof prev, "%s.prev", save);
    put_d2s(X, 40, NULL, 0); g_nbarrier = 0; CHECK(h_fs_Commit("save") == RES_OK && g_nbarrier == 1, "改了 1 个文件：刷 1 次（实际 %d） | 1 file changed: 1 barrier (got %d)", g_nbarrier, g_nbarrier);
    CHECK(file_has(prev, "Joe/Joe.d2s", X, 40) == 0 && exists(prev, "Joe/Joe.d2s"), ".prev 是上一个已提交版（没有 X） | .prev is the previous committed version (without X)");
    g_nbarrier = 0; CHECK(h_fs_Commit("save") == RES_OK && g_nbarrier == 0, "没改文件：一次也不刷（实际 %d） | nothing changed: no barrier (got %d)", g_nbarrier, g_nbarrier);
    put_d2s(Y, 40, NULL, 0); gwrite("save:/" STASH "/" STASH ".d2i", (const uint8_t *)"STASH-v2", 8); g_nbarrier = 0;
    CHECK(h_fs_Commit("save") == RES_OK && g_nbarrier == 1, "改了 2 个文件：屏障还是 1 次（实际 %d） | 2 files changed: still one barrier (got %d)", g_nbarrier, g_nbarrier);
    CHECK(file_has(prev, "Joe/Joe.d2s", X, 40) == 1, ".prev 跟着换成上一个版本（有 X） | .prev moves on to the previous version (with X)");
    char neu[1300]; snprintf(neu, sizeof neu, "%s.new", save); CHECK(access(neu, F_OK) != 0, "没有 .new 残留 | no leftover .new");
}
// ---- F-SAVE-9：事务目录里的增删改失败 | F-SAVE-9: failed mutations in the transaction directory
static void c_mutations(void) {
    base_save(NULL, 0); tmount(); int nf0 = g_nfail;
    CHECK(h_fs_CreateFile("save:/Joe/Joe.d2s", 4) == RES_PATH_EXISTS && h_fs_DeleteFile("save:/Joe/none") == RES_PATH_NOT_FOUND, "已存在 / 不存在照常回答 | already exists / does not exist answered as usual");
    put_d2s(X, 40, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK && g_nfail == nf0, "这些不算写失败：照常提交 | these are not write failures: the commit goes through");
    char jd[1300]; snprintf(jd, sizeof jd, "%s/Joe", work); chmod(jd, 0555);
    Result rc = h_fs_CreateFile("save:/Joe/Joe.ma0", 8); chmod(jd, 0755);
    CHECK(rc != RES_OK && h_fs_Commit("save") != RES_OK && g_nfail == nf0 + 1 && !exists(save, "Joe/Joe.ma0"), "建文件被拒（权限）→ 本事务被拒 | create refused (permissions) -> the transaction is refused");
    uint8_t b[64]; size_t k = d2s(b, NULL, 0, NULL, 0); Result w = gwrite("save:/Joe/Joe.key", b, k);   // 不存在的文件写方式打开失败 → 空句柄写 | a write-mode open of a missing file fails -> null-handle write
    CHECK(w != RES_OK && h_fs_Commit("save") != RES_OK, "打开失败后的空句柄写 → 本事务被拒 | a null-handle write after a failed open -> the transaction is refused");
    uint64_t h = 0; CHECK(h_fs_OpenFile(&h, "save:/Joe/Joe.d2s", MODE_WRITE) == RES_OK, "写方式打开成功（清掉上次失败的记号） | an open for writing succeeds (clearing the earlier failure)"); h_fs_CloseFile(h);
    { setenv("DM_TEMP", root, 1); h_fs_MountTemp("temp"); gwrite("temp:/none", b, k); CHECK(h_fs_Commit("save") == RES_OK, "临时区上的空句柄写不碰存档事务 | a null-handle write on the temp mount leaves the save transaction alone"); }
    char t[1400]; const char *tn[] = { "Joe.sw_kills.tmp", "Joe.dm_pets.tmp", "Joe.swpouch.tmp", "Joe.swpage.tmp", "Joe.sw_rift.tmp", "a.tmp", "b.tmp", "c.tmp" };   // 服务端拍写手的临时文件名 | the server-tick writers' temp file names
    for (int i = 0; i < 8; i++) { snprintf(t, sizeof t, "%s/Joe/%s", work, tn[i]); putb(t, "k", 1); }
    g_race = 1; k = d2s(b, Y, 40, NULL, 0); w = gwrite("save:/Joe/Joe.d2s", b, k); g_race = 0;
    for (int i = 0; i < 8; i++) { snprintf(t, sizeof t, "%s/Joe/%s", work, tn[i]); unlink(t); }
    CHECK(w == RES_OK && h_fs_Commit("save") == RES_OK && file_has(save, "Joe/Joe.d2s", Y, 40) == 1, "旁边的临时文件刚好没了：照样找到 Joe.d2s、新进度提交进去 | sibling temp files vanished meanwhile: Joe.d2s is still found and the new progress is committed");
}
// ---- F-SAVE-13：fd ≥ 4096 打开不越界（ASan） | F-SAVE-13: opening with fd >= 4096 stays in bounds (ASan)
static void c_high_fd(void) {
    base_save(NULL, 0); tmount(); struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl); rl.rlim_cur = rl.rlim_max < 8192 ? rl.rlim_max : 8192;
    if (setrlimit(RLIMIT_NOFILE, &rl) || rl.rlim_cur < 4200) { CHECK(1, "（fd 上限不够 4200，跳过） | (fd limit below 4200, skipped)"); return; }
    int last = -1; while ((last = open("/dev/null", O_RDONLY)) >= 0 && last < 4100) ;
    uint64_t h = 0; CHECK(last >= 4100 && h_fs_OpenFile(&h, "save:/Joe/Joe.d2s", MODE_READ) == RES_OK && h >= 4096, "fd %llu 打开成功（没越界） | fd %llu opened (in bounds)", (unsigned long long)h, (unsigned long long)h);
}
// ---- F-SAVE-14：设置缺省关文件失败（文件大小上限代替盘满）不换掉原文件 | F-SAVE-14: a failed close of the settings seed (a file size cap standing in for a full disk) does not replace the original
static void c_settings(void) {
    char sd[1300], sp[1400]; snprintf(sd, sizeof sd, "%s/s14", root); snprintf(sp, sizeof sp, "%s/Settings/Settings.json", sd);
    const char *good = "{\n    \"Foo\": 1\n}\n"; putb(sp, good, strlen(good)); setenv("DM_TOUCH_DEFAULTS", "1", 1);
    signal(SIGXFSZ, SIG_IGN); struct rlimit rl; getrlimit(RLIMIT_FSIZE, &rl); struct rlimit lim = { 40, rl.rlim_max };
    setrlimit(RLIMIT_FSIZE, &lim); settings_seed(sd); setrlimit(RLIMIT_FSIZE, &rl);
    size_t n; uint8_t *d = readb(sp, &n); char tmp[1500]; snprintf(tmp, sizeof tmp, "%s.seed", sp);
    CHECK(d && n == strlen(good) && !memcmp(d, good, n) && access(tmp, F_OK) != 0, "原 Settings.json 不动（%zu 字节）、没留临时文件 | the original Settings.json is untouched (%zu bytes) and no temp file is left", n, n); free(d);
}
// ---- F-SAVE-1：死亡那几拍不关钩子、同一局同一角色不重读，没证明完的留着 | F-SAVE-1: the dying ticks keep the hooks enabled and the same character in the same session is not reread, so unproven entries survive
#define G1 0x10000ull
static void enter_all(uint64_t G, uint32_t tq, const char *who, int *rp, int *rv, int *rg) { *rp = tp_enter(G, tq, who); *rv = tv_enter(G, tq, who); *rg = tg_enter(G, tq, who); }
static void c_death_keeps(void) {
    uint8_t ci[60]; memcpy(ci, R, 14); memcpy(ci + 14, Y, 40); base_save(ci, 54); tmount(); tv_setup(STASH); int rp, rv, rg; enter_all(G1, 10, "Joe", &rp, &rv, &rg);
    CHECK(rp && rv && rg, "进局：三个模块都读了侧文件 | entering the run: all three modules read their side files");
    tp_stow(0x31307220, R, 14); tv_deposit(1, 7, Y, 40); tg_dirty();
    tp_tick0(); tv_tick0(); tg_tick0(); CHECK(tp_hook_on() && tv_loaded() && tg_hook_on(), "死亡那几拍（没有受控玩家）：钩子都还开着 | the dying ticks (no controlled player): every hook stays enabled");
    enter_all(G1, 260, "Joe", &rp, &rv, &rg); int nu, nud, nup, ud, np, dd, nv, nd; tp_state(&nu, &nud, &nup, &ud); tv_state(&np, &dd, &nv, &nd);
    CHECK(!rp && !rv && !rg && nup == 1 && np == 1 && nu == 1 && tv_mem_has(1, 7), "复活（同一局、同一角色）：不重读，收进囊 / 存入仍待证明 | respawn (same session, same character): no reread, the stow and the store are still pending");
    tp_tick0(); put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "死亡那几拍里的一次提交 | a commit during the dying ticks");
    CHECK(file_has(save, "Joe/Joe.swpouch", R, 14) == 1 && file_has(save, STASH ".swvault", Y, 40) == 1 && file_has(save, "Joe/Joe.d2s", R, 14) == 0, "钩子照样核对：R 进 .swpouch、Y 进 .swvault，角色档里都没有 | the hooks still check: R in .swpouch, Y in .swvault, neither in the character file");
    restart(); tp_setup("Joe"); tp_state(&nu, &nud, &nup, &ud); CHECK(nu == 1, "重启后囊里恰 1 件 | after a restart the pouch holds exactly 1 item");
}
// ---- F-SAVE-1：换了一局（拍数从头数）或换了角色才重读；保存并退出后进另一个角色同样重读 | F-SAVE-1: only a new session (ticks counted from zero) or a different character rereads; Save and Exit into another character rereads too
static void c_new_session(void) {
    base_save(R, 14); char p[1400]; snprintf(p, sizeof p, "%s/Ann/Ann.d2s", save); { uint8_t b[64]; size_t k = d2s(b, NULL, 0, NULL, 0); putb(p, b, k); }
    tmount(); tv_setup(STASH); int rp, rv, rg, nu, nud, nup, ud; enter_all(G1, 10, "Joe", &rp, &rv, &rg); tp_stow(0x31307220, R, 14);
    enter_all(G1, 5, "Joe", &rp, &rv, &rg); tp_state(&nu, &nud, &nup, &ud);
    CHECK(rp && rv && rg && nup == 0 && nu == 0, "保存并退出、再进同一角色（拍数从头数）：重读，按工作目录里那份（游戏也从盘上重读了角色） | Save and Exit, then the same character (ticks from zero): reread from the work directory (the game reread the character from disk too)");
    enter_all(G1, 90, "Joe", &rp, &rv, &rg); CHECK(!rp && !rv && !rg, "同一局往后：不重读 | later in the same session: no reread");
    enter_all(G1, 95, "Ann", &rp, &rv, &rg); CHECK(rp && rv && rg, "换了角色：重读 | a different character: reread");
    enter_all(G1, 3, "Joe", &rp, &rv, &rg); CHECK(rp && rv && rg, "保存并退出后进另一个角色：重读 | Save and Exit into another character: reread");
    enter_all(G1 + 0x40, 400, "Joe", &rp, &rv, &rg); CHECK(rp && rv && rg, "服务端游戏结构换了：重读 | the server game struct changed: reread");
}
// ---- F-SAVE-1：钩子在主菜单上也开着，删掉的角色丢掉它的待证明、不拒删角色那次提交 | F-SAVE-1: the hooks stay enabled on the main menu; a deleted character drops its pending entries and does not refuse the commit that deletes it
static void c_deleted_char(void) {
    base_save(R, 14); tmount(); int rp, rv, rg; tv_setup(STASH); enter_all(G1, 10, "Joe", &rp, &rv, &rg); tp_stow(0x31307220, R, 14); tg_dirty(); tp_tick0(); tg_tick0();
    int nf0 = g_nfail; CHECK(h_fs_DeleteDirRec("save:/Joe") == RES_OK && h_fs_Commit("save") == RES_OK && g_nfail == nf0, "主菜单上删掉角色：提交照常 | deleting the character on the main menu: the commit goes through");
    int nu, nud, nup, ud; tp_state(&nu, &nud, &nup, &ud); CHECK(nup == 0 && !tp_hook_on() && !tg_hook_on() && !exists(save, "Joe"), "它的待证明丢掉、钩子关了、已提交版里没有这个角色 | its pending entries dropped, its hooks off, the character gone from the committed version");
}
// ---- F-SAVE-17：编年史各格各证、同一格守先后；有待落盘的每 2 秒再请存盘 | F-SAVE-17: Chronicle cells are proven independently with the order kept within a cell; a save is requested every 2 seconds while anything is pending
static void c_vault_cells(void) {
    base_save(NULL, 0); tmount(); tv_setup(STASH); tv_load(); tv_deposit(1, 100, X, 40); put_d2s(NULL, 0, NULL, 0);
    CHECK(h_fs_Commit("save") == RES_OK && file_has(save, STASH ".swvault", X, 40) == 1, "X 存进编年史、已落盘 | X stored in the Chronicle and persisted");
    tv_withdraw(1, 100); put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "取出 X 后卖掉（哪个存档里都没有）：这次提交证明不了 | X withdrawn and sold (in no save): this commit cannot prove it");
    tv_deposit(1, 7, Y, 40); put_d2s(NULL, 0, NULL, 0); int np, dd, nv, nd;
    CHECK(h_fs_Commit("save") == RES_OK && file_has(save, STASH ".swvault", Y, 40) == 1, "另一格的 Y 不等 X：这次提交就进 .swvault | Y in another cell does not wait for X: it reaches .swvault at this commit");
    tv_state(&np, &dd, &nv, &nd); CHECK(np == 1 && file_has(save, STASH ".swvault", X, 40) == 1, "X 仍待落盘（等「第二次提交、过 10 秒」） | X is still pending (waiting for \"the second commit and 10 seconds\")");
    tv_age_all(11); put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK && file_has(save, STASH ".swvault", X, 40) == 0, "过了 10 秒：X 从 .swvault 去掉 | after 10 seconds: X leaves .swvault");
    // 同一格守先后：存入 Z（它还在背包 = 存档里还有）、紧接着又取出，取出等存入那条 | order within a cell: store Z (still in the save) and withdraw it again right away; the withdraw waits behind the store
    uint8_t Z[40]; for (int i = 0; i < 40; i++) Z[i] = (uint8_t)(0x30 + 3 * i); tv_deposit(1, 9, Z, 40); tv_withdraw(1, 9); put_d2s(Z, 40, NULL, 0);
    CHECK(h_fs_Commit("save") == RES_OK, "提交 | commit"); tv_state(&np, &dd, &nv, &nd); CHECK(np == 2, "同一格的两条都等（%d 条）：取出排在没证明的存入后面 | both entries of the cell wait (%d): the withdraw is behind the unproven store", np, np);
    int r0 = g_nsave_req; tv_resave(100); int r1 = g_nsave_req; tv_resave(120); int r2 = g_nsave_req; tv_resave(151); int r3 = g_nsave_req;
    CHECK(r1 == r0 + 1 && r2 == r1 && r3 == r2 + 1, "有待落盘：第 100 拍请存盘、120 拍不请（不到 2 秒）、151 拍再请 | pending: a save is requested at tick 100, not at 120 (under 2 seconds), again at 151");
}
// ---- F-SAVE-15：收进囊按「之后序列化的角色档」指纹证明 | F-SAVE-15: a stow is proven by the fingerprint of a character file serialized afterwards
static void c_pouch_fp(void) {
    base_save(R, 14); tmount(); tp_setup("Joe"); tp_stow(0x31307220, R, 14);   // 收的这一刻工作目录里的角色档 = 背包里有 R 的那份（指纹 h0） | the character file in the work directory at the stow = the one with R in the inventory (fingerprint h0)
    g_stamp++; put_d2s(R, 14, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "收之后一颗同种符文落到同一格，游戏存盘（字节里还有 R） | after the stow an identical rune lands in the same cell and the game saves (R's bytes are still there)");
    CHECK(file_has(save, "Joe/Joe.swpouch", R, 14) == 1 && file_has(save, "Joe/Joe.d2s", R, 14) == 1, "照样证明：囊里一颗、背包一颗，共 2 颗 | proven anyway: one in the pouch, one in the inventory, 2 in all");
    // 放到背包：放好的那一串之前就在（同格同种），指纹没变不算，下一次存盘才算 | release: the placed bitstream was there before (same rune in the same cell); with an unchanged fingerprint it does not count, the next save does
    uint8_t b[64]; size_t k = d2s(b, R2, 14, NULL, 0); { char p[1400]; snprintf(p, sizeof p, "%s/Joe/Joe.d2s", work); putb(p, b, k); }
    tp_release(R2, 14, 9); uint32_t ids[4]; CHECK(h_fs_Commit("save") == RES_OK && tp_take_ready(ids, 4) == 0, "放之后还没序列化（指纹没变）：不算证明 | not yet serialized after the release (same fingerprint): not proven");
    g_stamp++; put_d2s(R2, 14, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK && tp_take_ready(ids, 4) == 1 && ids[0] == 9, "放之后的存盘：证明 | a save after the release: proven");
    // 有在途的存盘：不记指纹，只按字节 | a save in flight: no fingerprint, bytes only
    tg_set_inflight(1); tp_stow(0x31307220, R, 14); tg_set_inflight(0); g_stamp++; put_d2s(R, 14, NULL, 0); int nu, nud, nup, ud;
    CHECK(h_fs_Commit("save") == RES_OK && (tp_state(&nu, &nud, &nup, &ud), nup == 1), "在途时收的：没有指纹、字节还在 → 不证明 | stowed with a save in flight: no fingerprint and the bytes are still there -> not proven");
}
// ---- F-SAVE-15 / F-SAVE-2：被拒的提交把工作目录克隆回更早的已提交版，记下的指纹作废 | F-SAVE-15 / F-SAVE-2: a refused commit reclones the work directory back to an older committed version, so recorded fingerprints are dropped
static void c_fp_dropped(void) {
    setenv("DM_TEST_FS_FAIL", "write:2:28", 1);
    base_save(R, 14); tmount(); tp_setup("Joe"); g_stamp++; put_d2s(R, 14, NULL, 0);   // 第 1 次写：一份还没提交的角色档（背包里还有 R） | write 1: a character file not yet committed (R still in the inventory)
    tp_stow(0x31307220, R, 14); gwrite("save:/" STASH "/" STASH ".d2i", (const uint8_t *)"S2", 2);   // 第 2 次写注入失败 | write 2 fails by injection
    CHECK(h_fs_Commit("save") != RES_OK, "提交被拒，工作目录回到已提交版（另一份收之前的角色档） | refused, the work directory goes back to the committed version (another pre-stow character file)");
    CHECK(h_fs_Commit("save") == RES_OK && file_has(save, "Joe/Joe.swpouch", R, 14) != 1, "下一次提交（游戏没重写角色档）：指纹不同也不算证明，R 还在背包里就不进囊 | the next commit (no rewrite by the game): a different fingerprint no longer proves it; R is still in the inventory, so it is not put in the pouch");
    g_stamp++; put_d2s(NULL, 0, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK && file_has(save, "Joe/Joe.swpouch", R, 14) == 1, "游戏存了收之后的角色档：证明 | the game saved a post-stow character file: proven");
}
static void c_pages_fp_dropped(void) {
    setenv("DM_TEST_FS_FAIL", "write:2:28", 1);
    pages_base(); g_stamp++; put_d2s(A, 30, NULL, 0);   // 第 1 次写：一份还没提交的翻页前角色档 | write 1: a pre-turn character file not yet committed
    tg_flip(0, 1, A, 30, Bb, 30); gwrite("save:/" STASH "/" STASH ".d2i", (const uint8_t *)"S2", 2);
    CHECK(h_fs_Commit("save") != RES_OK, "提交被拒，工作目录回到已提交版（另一份翻页前的角色档） | refused, the work directory goes back to the committed version (another pre-turn character file)");
    CHECK(h_fs_Commit("save") == RES_OK, "下一次提交（游戏没重写角色档） | the next commit (no rewrite by the game)");
    char p[1400]; snprintf(p, sizeof p, "%s/Joe/Joe.swpage", save); int sa, cur, pend; tg_file_has(p, A, 30, &sa, &cur, &pend);
    CHECK(pend == 1 && file_has(save, "Joe/Joe.d2s", A, 30) == 1, "格里还是 A：带待定记录落盘，不当成翻页后 | the grid still holds A: persisted with the pending record, not taken for after the turn");
    put_d2s(Bb, 30, NULL, 0); CHECK(h_fs_Commit("save") == RES_OK, "翻页后的存盘 | a post-turn save"); pages_after("翻页后的存盘 | a post-turn save");
}
static const struct { const char *name; void (*fn)(void); } k_cases[] = {
    { "refused_vault", c_refused_vault }, { "refused_crosshook", c_refused_crosshook }, { "refused_pouch", c_refused_pouch }, { "refused_pages", c_refused_pages },
    { "release_waits", c_release_waits }, { "poison_skips", c_poison_skips }, { "pages_write_fail", c_pages_write_fail }, { "stuck_new", c_stuck_new },
    { "relaunch", c_relaunch }, { "prev_sync", c_prev_sync }, { "mutations", c_mutations }, { "high_fd", c_high_fd }, { "settings", c_settings },
    { "death_keeps", c_death_keeps }, { "new_session", c_new_session }, { "deleted_char", c_deleted_char }, { "vault_cells", c_vault_cells },
    { "pouch_fp", c_pouch_fp }, { "fp_dropped", c_fp_dropped }, { "pages_fp_dropped", c_pages_fp_dropped },
};
int main(int argc, char **argv) {
    init_items(); const char *tmp = getenv("TMPDIR"); int bad = 0, total = 0;
    for (size_t c = 0; c < sizeof k_cases / sizeof *k_cases; c++) {
        if (argc > 1 && strcmp(argv[1], k_cases[c].name)) continue;
        fflush(stdout); pid_t pid = fork();
        if (!pid) {   // 子进程：一个用例一个临时目录 | child: one temp directory per case
            snprintf(root, sizeof root, "%s/t_save.%s.XXXXXX", tmp ? tmp : "/private/tmp", k_cases[c].name); if (!mkdtemp(root)) { perror("mkdtemp"); _exit(2); }
            snprintf(save, sizeof save, "%s/save", root); snprintf(work, sizeof work, "%s.work", save); mkdir(save, 0755);
            printf("== %s\n", k_cases[c].name); k_cases[c].fn();
            char cmd[1200]; snprintf(cmd, sizeof cmd, "chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", root, root); system(cmd);
            printf("   %d 项，失败 %d | %d checks, %d failed\n", checks, fails, checks, fails); fflush(stdout); _exit(fails ? 1 : 0); }
        int st = 0; waitpid(pid, &st, 0); total++; if (!WIFEXITED(st) || WEXITSTATUS(st)) { bad++; printf("  ✗ 用例 %s 失败（状态 %#x） | case %s failed (status %#x)\n", k_cases[c].name, st, k_cases[c].name, st); }
    }
    printf("== 存档钩子 %d 个用例，失败 %d | save hooks: %d cases, %d failed ==\n", total, bad, total, bad);
    return bad != 0;
}
