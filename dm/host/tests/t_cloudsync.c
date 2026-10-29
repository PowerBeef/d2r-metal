// iCloud 同步核心单测（秒级）：dm_cloudsync.c。两个本地目录当两台设备、一个本地目录当云端；期望都是按「后写覆盖先写」手推的，不跑被测实现得出。
// iCloud sync core unit test (takes seconds): dm_cloudsync.c. Two local directories act as two devices and one local directory as the cloud; all expectations are derived by hand from "later write overwrites earlier write", not obtained by running the implementation under test.
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
int dm_cloud_char_file(const char *root, const char *guest, char *out, size_t n);
int dm_cloud_inbox_retire(const char *root, const char *name);
int dm_cloud_sync(const char *save, const char *cloud, const char *bak, const char *statef, int allow_down, uint64_t now_ms, int (*ready)(const char *unit));
int dm_cloud_conflict_take(void);
int dm_cloud_char_conflict_take(char *out, size_t n);
int dm_cloud_inbox_add(const char *save, const char *statef, const char *pack);
#if __has_include("../stash_seed.h")   // 公开仓库不带这份预置（它是游戏生成的一份存档的字节）：不在时跳过用到它的 ⑱ ⑲ 两组 | the public repository does not ship this seed (it is the bytes of a save the game generated): when it is absent, the two groups that use it, 18 and 19, are skipped
#include "../stash_seed.h"
#define T_HAVE_STASH_SEED 1
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char R[512];
static void path(char *o, const char *a, const char *b) { snprintf(o, 1024, "%s/%s/%s", R, a, b); }
static void put(const char *dev, const char *rel, const char *txt, long sec) {   // 写一个文件并把修改时间定成 sec 秒 | write a file and set its modification time to sec seconds
    char p[1024]; path(p, dev, rel); char d[1024]; strcpy(d, p); for (char *s = d + strlen(R) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(d, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); struct timeval tv[2] = { { sec, 0 }, { sec, 0 } }; utimes(p, tv);
}
static const char *get(const char *dev, const char *rel) { static char b[256]; char p[1024]; path(p, dev, rel); FILE *f = fopen(p, "r"); if (!f) return NULL; size_t n = fread(b, 1, 255, f); b[n] = 0; fclose(f); return b; }
static long mtime(const char *dev, const char *rel) { char p[1024]; struct stat st; path(p, dev, rel); return stat(p, &st) ? -1 : st.st_mtimespec.tv_sec; }
static int dsync(const char *dev, int down, uint64_t now) { char s[1024], c[1024], b[1024], t[1024]; path(s, dev, "save"); snprintf(c, sizeof c, "%s/cloud", R); path(b, dev, "bak"); path(t, dev, "state"); return dm_cloud_sync(s, c, b, t, down, now, NULL); }
static int count_bak(const char *dev, const char *pre) { char d[1024]; path(d, dev, "bak"); DIR *dp = opendir(d); struct dirent *e; int n = 0; while (dp && (e = readdir(dp))) n += !strncmp(e->d_name, pre, strlen(pre)); if (dp) closedir(dp); return n; }
static int no_skip(const char *u) { return strcmp(u, "Alice"); }
// 共享仓库档：一页物品（items 件，只写页头与件数）+ 一段编年史（chron 字节）；侧文件：n 件。只造结构，同步那边只看结构（dm_cloudsync.c rich_of）
// Shared stash file: one page of items (items pieces, only the page header and item count are written) + one chronicle section (chron bytes); side file: n pieces. Only the structure is built, as the sync side only looks at structure (dm_cloudsync.c rich_of)
static void put_bin(const char *dev, const char *rel, const uint8_t *d, size_t n, long sec) {
    char p[1024]; path(p, dev, rel); char dd[1024]; strcpy(dd, p); for (char *x = dd + strlen(R) + 1; (x = strchr(x, '/')); x++) { *x = 0; mkdir(dd, 0755); *x = '/'; }
    FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); struct timeval tv[2] = { { sec, 0 }, { sec, 0 } }; utimes(p, tv);
}
static void put_d2i(const char *dev, int items, int chron, long sec) {
    static uint8_t b[8192]; memset(b, 0, sizeof b); uint32_t l1 = 68, l2 = (uint32_t)chron;
    memcpy(b, "\x55\xaa\x55\xaa", 4); memcpy(b + 16, &l1, 4); memcpy(b + 64, "JM", 2); b[66] = (uint8_t)items;
    memcpy(b + 68, "\x55\xaa\x55\xaa", 4); memcpy(b + 68 + 16, &l2, 4); b[68 + 64] = 0xc0;
    put_bin(dev, "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", b, 68 + l2, sec);
}
static void put_d2i_salt(const char *dev, int items, int chron, long sec, uint8_t salt) {   // as put_d2i, one byte inside the chronicle segment changed: same structure, other content
    static uint8_t b[8192]; memset(b, 0, sizeof b); uint32_t l1 = 68, l2 = (uint32_t)chron;
    memcpy(b, "\x55\xaa\x55\xaa", 4); memcpy(b + 16, &l1, 4); memcpy(b + 64, "JM", 2); b[66] = (uint8_t)items;
    memcpy(b + 68, "\x55\xaa\x55\xaa", 4); memcpy(b + 68 + 16, &l2, 4); b[68 + 64] = 0xc0; b[68 + 100] = salt;
    put_bin(dev, "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", b, 68 + l2, sec);
}
static int inbox_n(const char *dev) { char d[1024]; path(d, dev, "save/_cloudinbox"); DIR *dp = opendir(d); struct dirent *e; int n = 0; while (dp && (e = readdir(dp))) n += e->d_name[0] != '.'; if (dp) closedir(dp); return n; }
static int inbox_has(const char *dev, const char *f) {   // 收件箱里有没有哪个条目带这个文件 | whether any inbox entry carries this file
    char d[1024]; path(d, dev, "save/_cloudinbox"); DIR *dp = opendir(d); struct dirent *e; int k = 0; struct stat st;
    while (dp && (e = readdir(dp))) { if (e->d_name[0] == '.') continue; char p[1400]; snprintf(p, sizeof p, "%s/%s/%s", d, e->d_name, f); k |= !stat(p, &st); } if (dp) closedir(dp); return k; }
// 手造一个云端包（格式见 dm_cloudsync.c pack_write）：n 个文件，路径 rel[i]、内容 txt[i]，修改时间都取 stamp | hand-build a cloud pack (format in dm_cloudsync.c pack_write): n files, paths rel[i], contents txt[i], all with modification time stamp
static void pack_put(const char *p, uint64_t stamp, uint32_t n, const char *const *rel, const char *const *txt) {
    FILE *f = fopen(p, "wb"); uint32_t fl = 0; fwrite("SWPK1\n\0\0", 8, 1, f); fwrite(&stamp, 8, 1, f); fwrite(&fl, 4, 1, f); fwrite(&n, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) { uint16_t pl = (uint16_t)strlen(rel[i]); uint32_t sz = (uint32_t)strlen(txt[i]); fwrite(&pl, 2, 1, f); fwrite(rel[i], pl, 1, f); fwrite(&stamp, 8, 1, f); fwrite(&sz, 4, 1, f); fwrite(txt[i], sz, 1, f); }
    fclose(f);
}
static int d2i_items(const char *dev, const char *rel) { char p[1024]; path(p, dev, rel); FILE *f = fopen(p, "rb"); if (!f) return -1; uint8_t b[70]; size_t n = fread(b, 1, 70, f); fclose(f); return n >= 67 ? b[66] : -1; }

// 带合法编年史段的共享仓库档：一页物品（items 件）+ 编年史段（A 组 n 条：序号 id[i]、时刻 t[i]；B、C 组空），格式见 dm_cloudsync.c chron_union
// Shared stash file with a valid Chronicle segment: one item page (items items) + Chronicle segment (group A n records: index id[i], time t[i]; groups B and C empty); format see dm_cloudsync.c chron_union
static void put_chron(const char *dev, int items, int n, const uint32_t *id, const uint32_t *t, long sec) {
    static uint8_t b[8192]; memset(b, 0, sizeof b); uint32_t l1 = 68, l2 = 84 + 10 * (uint32_t)n + 64; uint16_t one = 1, a = (uint16_t)n;
    memcpy(b, "\x55\xaa\x55\xaa", 4); memcpy(b + 16, &l1, 4); memcpy(b + 64, "JM", 2); b[66] = (uint8_t)items;
    uint8_t *c = b + 68; memcpy(c, "\x55\xaa\x55\xaa", 4); memcpy(c + 16, &l2, 4); memcpy(c + 64, "\xc0\xed\xea\xc0", 4); memcpy(c + 68, &one, 2); memcpy(c + 70, &a, 2);
    for (int i = 0; i < n; i++) { memcpy(c + 84 + 10 * i, &id[i], 4); uint16_t u = 0x6a5d; memcpy(c + 88 + 10 * i, &u, 2); memcpy(c + 90 + 10 * i, &t[i], 4); }
    put_bin(dev, "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", b, 68 + l2, sec);
}
// 读回编年史 A 组：「序号@时刻」按序号升序拼成串（比对用）；格式不对返回 "坏"
// Read back Chronicle group A: "index@time" joined in ascending index order (for comparison); returns "坏" on bad format
static const char *chron_str(const char *dev) {
    static char o[512]; char p[1024]; path(p, dev, "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"); FILE *f = fopen(p, "rb"); uint8_t b[8192]; size_t n = f ? fread(b, 1, sizeof b, f) : 0; if (f) fclose(f);
    if (n < 68 + 148 || memcmp(b + 68 + 64, "\xc0\xed\xea\xc0", 4)) return "坏";
    uint16_t a; uint32_t ln; memcpy(&a, b + 68 + 70, 2); memcpy(&ln, b + 68 + 16, 4); if (ln != 84u + 10u * a + 64u || 68 + ln != n) return "坏";
    uint32_t id[64], t[64]; for (int i = 0; i < a && i < 64; i++) { memcpy(&id[i], b + 68 + 84 + 10 * i, 4); memcpy(&t[i], b + 68 + 90 + 10 * i, 4); }
    for (int i = 0; i < a; i++) for (int j = i + 1; j < a; j++) if (id[j] < id[i]) { uint32_t x = id[i]; id[i] = id[j]; id[j] = x; x = t[i]; t[i] = t[j]; t[j] = x; }
    o[0] = 0; for (int i = 0; i < a; i++) snprintf(o + strlen(o), sizeof o - strlen(o), "%s%u@%u", i ? " " : "", id[i], t[i]); return o;
}

// Log capture (dm.h LOG goes through this hook when set): the cases below that must say something in red check the captured lines
extern void (*g_dm_log_hook)(const char *file, int line, const char *fmt, ...);
static char g_log[65536];
static void log_cap(const char *file, int line, const char *fmt, ...) {
    (void)file; (void)line; char b[4096]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", b); if (strlen(g_log) + strlen(b) + 2 < sizeof g_log) { strcat(g_log, b); strcat(g_log, "\n"); } }
static int log_has(const char *a, const char *b) { for (const char *l = g_log; *l; ) { const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l); char x[4096]; if (n >= sizeof x) n = sizeof x - 1; memcpy(x, l, n); x[n] = 0;
        if (strstr(x, a) && (!b || strstr(x, b))) return 1; l += n + (e != NULL); } return 0; }
static int pack_tomb(const char *p) { FILE *f = fopen(p, "rb"); uint8_t h[24]; size_t n = f ? fread(h, 1, 24, f) : 0; if (f) fclose(f); return n == 24 ? h[16] & 1 : -1; }   // -1 = no pack
static int not_zed(const char *u) { return strcmp(u, "Zed"); }
static int not_stash(const char *u) { return strcmp(u, "_stash"); }
static long file_bytes(const char *dev, const char *rel, uint8_t *o, size_t cap) { char p[1024]; path(p, dev, rel); FILE *f = fopen(p, "rb"); if (!f) return -1; long n = (long)fread(o, 1, cap, f); fclose(f); return n; }
static void cp_file(const char *a, const char *b) { FILE *fi = fopen(a, "rb"), *fo = fopen(b, "wb"); char buf[8192]; size_t k; while (fi && fo && (k = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, k, fo); if (fi) fclose(fi); if (fo) fclose(fo); }

int main(void) {
    snprintf(R, sizeof R, "%s/t_cloudsync.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(R, 0755);
    g_dm_log_hook = log_cap;
    char c[1024]; snprintf(c, sizeof c, "%s/cloud", R); mkdir(c, 0755);
    // ① A 有角色 Alice（时刻 1000 秒）、共享仓库、Settings；B 空 → A 上传，B 启动拿到 Alice 与共享仓库，Settings 不同步，修改时间照 A 的
    // (1) A has character Alice (timestamp 1000 seconds), the shared stash and Settings; B is empty -> A uploads, B gets Alice and the shared stash at startup, Settings is not synced, and modification times follow A's
    put("A", "save/Alice/Alice.d2s", "a1", 1000); put("A", "save/Alice/Alice.key", "k", 900);
    put("A", "save/SharedStashSoftCoreV2.d2i", "s1", 1100); put("A", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", "m1", 1100); put("A", "save/Settings/Settings.json", "{}", 1100);
    put("B", "save/Settings/Settings.json", "{b}", 1200);
    CHECK(dsync("A", 0, 0) == 2, "① A 上传应为 2 个单位（Alice、_stash）");
    CHECK(dsync("B", 1, 0) == 2, "① B 下载应为 2 个单位");
    CHECK(get("B", "save/Alice/Alice.d2s") && !strcmp(get("B", "save/Alice/Alice.d2s"), "a1"), "① B 的 Alice 应是 a1");
    CHECK(mtime("B", "save/Alice/Alice.d2s") == 1000, "① 修改时间应还原成 1000，实为 %ld", mtime("B", "save/Alice/Alice.d2s"));
    CHECK(get("B", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i") && !strcmp(get("B", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"), "m1"), "① B 的共享仓库应是 m1");
    CHECK(!strcmp(get("B", "save/Settings/Settings.json"), "{b}"), "① B 的 Settings 不该被动");
    CHECK(dsync("A", 1, 0) == 0 && dsync("B", 1, 0) == 0, "① 再同步一次两边都应无动作");
    // ② 只有 B 改（2000）→ B 运行中上传；A 启动拿到，A 原来那份进备份
    // (2) Only B changed (2000) -> B uploads while running; A gets it at startup, and A's former copy goes to backup
    put("B", "save/Alice/Alice.d2s", "b2", 2000);
    CHECK(dsync("B", 0, 0) == 1, "② B 应上传 1 个");
    CHECK(dsync("A", 0, 0) == 0 && !strcmp(get("A", "save/Alice/Alice.d2s"), "a1"), "② A 运行中不该改本地");
    CHECK(dsync("A", 1, 0) == 1 && !strcmp(get("A", "save/Alice/Alice.d2s"), "b2"), "② A 启动后应是 b2");
    CHECK(get("A", "bak/Alice-1000000/Alice/Alice.d2s") && !strcmp(get("A", "bak/Alice-1000000/Alice/Alice.d2s"), "a1"), "② A 被覆盖的 a1 应在备份 Alice-1000000");
    // ③ 两边都改，B 后写（A 3000 先传，B 3500）→ B 运行中照样上传（后写赢），云端原来的 a3 进 B 的备份；A 启动拿到 b3
    // (3) Both sides changed, B wrote later (A's 3000 uploaded first, B 3500) -> B still uploads while running (last write wins), the cloud's former a3 goes to B's backup; A gets b3 at startup
    put("A", "save/Alice/Alice.d2s", "a3", 3000); put("B", "save/Alice/Alice.d2s", "b3", 3500);
    CHECK(dsync("A", 0, 0) == 1, "③ A 先上传");
    CHECK(dsync("B", 0, 0) == 1, "③ B 后写也上传");
    CHECK(get("B", "bak/Alice-3000000/Alice/Alice.d2s") && !strcmp(get("B", "bak/Alice-3000000/Alice/Alice.d2s"), "a3"), "③ 被覆盖的云端 a3 应在 B 的备份");
    CHECK(dsync("A", 1, 0) == 1 && !strcmp(get("A", "save/Alice/Alice.d2s"), "b3"), "③ A 启动后应是 b3");
    // ④ 两边都改，A 后写（A 5000 已传；B 4000 没传就重启）→ B 启动拿 A 的 a4，b4 进备份 Alice-4000000
    // (4) Both sides changed, A wrote later (A's 5000 uploaded; B's 4000 not uploaded before restarting) -> B gets A's a4 at startup, b4 goes to backup Alice-4000000
    put("A", "save/Alice/Alice.d2s", "a4", 5000); CHECK(dsync("A", 0, 0) == 1, "④ A 上传");
    put("B", "save/Alice/Alice.d2s", "b4", 4000);
    CHECK(dsync("B", 1, 0) == 1 && !strcmp(get("B", "save/Alice/Alice.d2s"), "a4"), "④ B 启动后应是 a4（云端 5000 > 本机 4000）");
    CHECK(get("B", "bak/Alice-4000000/Alice/Alice.d2s") && !strcmp(get("B", "bak/Alice-4000000/Alice/Alice.d2s"), "b4"), "④ B 被覆盖的 b4 应在备份");
    // ⑤ 云端的包还没下载好（ready 返回 0）→ 跳过，本地不动
    // (5) The cloud package has not finished downloading (ready returns 0) -> skip, local files untouched
    put("A", "save/Alice/Alice.d2s", "a5", 6000); CHECK(dsync("A", 0, 0) == 1, "⑤ A 上传");
    { char s[1024], b[1024], t[1024]; path(s, "B", "save"); path(b, "B", "bak"); path(t, "B", "state"); CHECK(dm_cloud_sync(s, c, b, t, 1, 0, no_skip) == 0 && !strcmp(get("B", "save/Alice/Alice.d2s"), "a4"), "⑤ 没下载好时 B 应不动"); }
    CHECK(dsync("B", 1, 0) == 1 && !strcmp(get("B", "save/Alice/Alice.d2s"), "a5"), "⑤ 下载好后 B 应是 a5");
    // ⑥ A 删 Alice → 云端删除标记；B 启动也删，B 那份进备份；B 的备份只留 3 份（此前有 1000000 不在 B、3000000、4000000，再加 6000000 = 3 份）
    // (6) A deletes Alice -> deletion marker in the cloud; B also deletes at startup and B's copy goes to backup; B's backup keeps only 3 copies (before this there were 1000000 not on B, 3000000, 4000000, plus 6000000 = 3 copies)
    { char p[1024]; path(p, "A", "save/Alice/Alice.d2s"); unlink(p); path(p, "A", "save/Alice/Alice.key"); unlink(p); path(p, "A", "save/Alice"); rmdir(p); }
    CHECK(dsync("A", 0, 7000000) == 1, "⑥ A 应写删除标记");
    CHECK(dsync("B", 1, 0) == 1 && !get("B", "save/Alice/Alice.d2s"), "⑥ B 启动后 Alice 应被删");
    CHECK(get("B", "bak/Alice-6000000/Alice/Alice.d2s") && !strcmp(get("B", "bak/Alice-6000000/Alice/Alice.d2s"), "a5"), "⑥ B 被删的 a5 应在备份");
    CHECK(count_bak("B", "Alice-") == 3, "⑥ B 的 Alice 备份应留 3 份，实为 %d", count_bak("B", "Alice-"));
    for (long t = 8; t <= 9; t++) { char v[8]; snprintf(v, sizeof v, "x%ld", t); put("B", "save/Bob/Bob.d2s", v, t * 1000); }   // 无关角色不受影响 | unrelated characters are not affected
    CHECK(dsync("B", 1, 0) == 1, "⑥ B 的新角色 Bob 上传");
    // ⑦ 删除之后 A 又建了同名角色（9500）→ 上传（复活）；B 启动拿到
    // (7) After the deletion A created a character with the same name again (9500) -> upload (revive); B gets it at startup
    put("A", "save/Alice/Alice.d2s", "a7", 9500);
    CHECK(dsync("A", 1, 0) == 2 && !strcmp(get("A", "save/Bob/Bob.d2s"), "x9"), "⑦ A：Alice 上传 + Bob 下载");
    CHECK(dsync("B", 1, 0) == 1 && get("B", "save/Alice/Alice.d2s") && !strcmp(get("B", "save/Alice/Alice.d2s"), "a7"), "⑦ B 启动后应有新建的 Alice a7");
    // ⑧ 共享仓库：B 改（10000）→ A 启动拿到
    // (8) Shared stash: B changes it (10000) -> A gets it at startup
    put("B", "save/SharedStashSoftCoreV2.d2i", "s8", 10000);
    CHECK(dsync("B", 0, 0) == 1 && dsync("A", 1, 0) == 1 && !strcmp(get("A", "save/SharedStashSoftCoreV2.d2i"), "s8"), "⑧ A 的共享仓库应是 s8");
    CHECK(!strcmp(get("A", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"), "m1"), "⑧ 共享仓库的另一个文件应保留 m1");
    // ⑨ 存档目录被游戏以外的原因清空（状态文件还在）→ 启动时不算删除，从云端全拿回来，云端不出现删除标记
    // (9) The save directory was emptied by something other than the game (the state file is still there) -> at startup it does not count as a deletion, everything is fetched back from the cloud, and no deletion marker appears in the cloud
    { char p[1024]; path(p, "A", "save"); char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", p); system(cmd); mkdir(p, 0755); }
    CHECK(dsync("A", 1, 0) == 3, "⑨ A 启动应下载 3 个（Alice、Bob、_stash）");
    CHECK(get("A", "save/Alice/Alice.d2s") && !strcmp(get("A", "save/Alice/Alice.d2s"), "a7") && get("A", "save/Bob/Bob.d2s") && !strcmp(get("A", "save/SharedStashSoftCoreV2.d2i"), "s8"), "⑨ A 应拿回 Alice a7、Bob、共享仓库 s8");
    CHECK(dsync("B", 1, 0) == 0 && get("B", "save/Alice/Alice.d2s"), "⑨ B 不受影响（没有删除标记传过来）");
    // ⑩ 备份目录写不进（这里用同名普通文件把它堵住）→ 覆盖和删除都不动本地；解开后照常进行（原先备份写失败仍照样覆盖 / 删）
    // (10) The backup directory cannot be written (here it is blocked with a regular file of the same name) -> neither overwrite nor delete touches local files; after unblocking it proceeds as usual (previously it still overwrote / deleted when the backup write failed)
    { char s[1024], c2[1024], b[1024], t2[1024], bf[1024]; snprintf(c2, sizeof c2, "%s/cloud2", R); mkdir(c2, 0755);
      #define SY(dev, down, now) (path(s, dev, "save"), path(b, dev, "bak"), path(t2, dev, "state"), dm_cloud_sync(s, c2, b, t2, down, now, NULL))
      put("F", "save/Settings/Settings.json", "{}", 1);   // F 是一台已有存档目录的设备 | F is a device that already has a save directory
      put("E", "save/Eve/Eve.d2s", "e1", 1000); CHECK(SY("E", 0, 0) == 1 && SY("F", 1, 0) == 1 && get("F", "save/Eve/Eve.d2s") && !strcmp(get("F", "save/Eve/Eve.d2s"), "e1"), "⑩ 准备：F 拿到 Eve e1");
      put("E", "save/Eve/Eve.d2s", "e2", 2000); CHECK(SY("E", 0, 0) == 1, "⑩ E 上传 e2");
      path(bf, "F", "bak"); FILE *f = fopen(bf, "w"); fclose(f);          // F 的备份目录位置是个普通文件：建不了目录 | F's backup directory location is a regular file: the directory cannot be created
      CHECK(SY("F", 1, 0) == 0 && get("F", "save/Eve/Eve.d2s") && !strcmp(get("F", "save/Eve/Eve.d2s"), "e1"), "⑩ 备份写不进时 F 不应覆盖，仍是 e1");
      unlink(bf);
      CHECK(SY("F", 1, 0) == 1 && get("F", "save/Eve/Eve.d2s") && !strcmp(get("F", "save/Eve/Eve.d2s"), "e2") && get("F", "bak/Eve-1000000/Eve/Eve.d2s") && !strcmp(get("F", "bak/Eve-1000000/Eve/Eve.d2s"), "e1"), "⑩ 解开后 F 换成 e2，e1 整份在备份里");
      { char p2[1024]; path(p2, "E", "save/Eve/Eve.d2s"); unlink(p2); path(p2, "E", "save/Eve"); rmdir(p2); }
      CHECK(SY("E", 0, 3000000) == 1, "⑩ E 运行中删了 Eve，写删除标记");
      char bk[1100]; snprintf(bk, sizeof bk, "%s.keep", bf); rename(bf, bk); f = fopen(bf, "w"); fclose(f);   // 已有的备份目录先挪开，再用普通文件堵住 | move the existing backup directory aside first, then block it with a regular file
      CHECK(SY("F", 1, 0) == 0 && get("F", "save/Eve/Eve.d2s"), "⑩ 备份写不进时 F 不应删 Eve");
      unlink(bf); rename(bk, bf);
      CHECK(SY("F", 1, 0) == 1 && !get("F", "save/Eve/Eve.d2s") && get("F", "bak/Eve-2000000/Eve/Eve.d2s"), "⑩ 解开后 F 删 Eve，e2 整份在备份里");
      #undef SY
    }
    // ⑪–⑮ 编年史被整份换空：新设备首次同步以云端为准；共享仓库不许被「明显更少」的一份覆盖（另一个云端目录 cloud3，设备 G / H）
    // (11)-(15) Chronicle replaced wholesale by an empty one: a new device's first sync takes the cloud as authoritative; the shared stash must not be overwritten by a copy with "clearly less" (another cloud directory cloud3, devices G / H)
    { char s3[1024], c3[1024], b3[1024], t3[1024]; snprintf(c3, sizeof c3, "%s/cloud3", R); mkdir(c3, 0755);
      #define S3(dev, down) (path(s3, dev, "save"), path(b3, dev, "bak"), path(t3, dev, "state"), dm_cloud_sync(s3, c3, b3, t3, down, 0, NULL))
      #define D2I "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"
      put_d2i("G", 5, 300, 1000);
      CHECK(S3("G", 0) == 1, "⑫ 云端为空：G 首次同步上传共享仓库");
      dm_cloud_conflict_take();
      put_d2i("H", 0, 148, 5000);   // H 新装：游戏刚建的空共享仓库，时刻比云端新 | H freshly installed: the empty shared stash the game just created, with a timestamp newer than the cloud's
      CHECK(S3("H", 0) == 0 && d2i_items("H", D2I) == 0, "⑪ 新设备运行中首次同步：不传不改（不能拿空仓库盖云端）");
      CHECK(S3("H", 1) == 1 && d2i_items("H", D2I) == 5, "⑪ 新设备启动首次同步：以云端为准，拿到 5 件");
      CHECK(count_bak("H", "_stash-") == 1, "⑪ 新设备本来那份空仓库进备份");
      CHECK(!dm_cloud_conflict_take() && inbox_n("H") == 0, "⑪ 本机那份是新建空档：不进收件箱、不提示");
      put_d2i("G", 6, 400, 6000); CHECK(S3("G", 0) == 1, "⑭ G 编年史变长（400）照常上传");
      CHECK(S3("H", 1) == 1 && d2i_items("H", D2I) == 6, "⑭ H 启动拿到 G 的新版");
      put_d2i("H", 0, 148, 7000);   // H 上的共享仓库不知为何变空（编年史 148、0 件），时刻更新 | the shared stash on H somehow became empty (chronicle 148, 0 items), with a newer timestamp
      CHECK(S3("H", 0) == 0, "⑬ H 运行中要上传变空的那份：分叉，运行中不动");
      CHECK(!dm_cloud_conflict_take(), "⑬ 运行中不提示（不再永久拦）");
      CHECK(S3("G", 1) == 0 && d2i_items("G", D2I) == 6, "⑬ 云端没被盖：G 仍是 6 件");
      CHECK(S3("H", 1) == 1 && d2i_items("H", D2I) == 6, "⑬ H 启动：云端为现行版，拿回 6 件");
      CHECK(inbox_n("H") == 0 && !dm_cloud_conflict_take(), "⑬ 本机那份是空的新建大小（0 件、编年史 148）：不进收件箱、不提示");
      put_d2i("H", 7, 500, 7500); CHECK(S3("H", 0) == 1, "⑮ H 正常玩：编年史 500 上传");
      put_d2i("G", 6, 450, 7600);   // G 没同步到 H 的 500 就自己写了 450（两边都改，G 后写） | G wrote 450 itself without having synced H's 500 (both sides changed, G wrote later)
      CHECK(S3("G", 0) == 0, "⑮ G 运行中要拿编年史 450 盖云端的 500：分叉，运行中不动");
      CHECK(S3("G", 1) == 1 && d2i_items("G", D2I) == 7, "⑮ G 启动：云端（7 件、500）为现行版");
      CHECK(inbox_n("G") == 1 && inbox_has("G", "mine.pack") && inbox_has("G", "base.pack"), "⑮ G 本机那份（6 件、450）进收件箱，带上次同步的基准（6 件、400）");
      CHECK(dm_cloud_conflict_take() == 1, "⑮ 收进收件箱：进局提示一次");
      CHECK(S3("G", 1) == 0 && inbox_n("G") == 1, "⑮ 再启动：一致、无动作、不再收（不是永久状态）");
      put_d2i("H", 0, 500, 8100);
      CHECK(S3("H", 0) == 1 && !dm_cloud_conflict_take(), "⑯ 正常玩法：共享仓库物品取空，编年史没变短——照常上传");
      // (17) F-SAVE-4: a stash whose chronicle never grew (148) and holds 2 items is emptied by the player (the items went to a character). With a sync
      //      state and the cloud unchanged since, that is the newer state of the same stash and is uploaded like any other change. Before, it counted as
      //      "an empty new save": never uploaded, and the next launch put the 2 items back while the character still held them (duplicated on one device).
      //      Another device whose copy is unchanged since its last sync follows without a fork; the rule still holds when both sides changed
      { char cd[1024]; snprintf(cd, sizeof cd, "%s/cloud4", R); mkdir(cd, 0755);
        #define S4(dev, down) (path(s3, dev, "save"), path(b3, dev, "bak"), path(t3, dev, "state"), dm_cloud_sync(s3, cd, b3, t3, down, 0, NULL))
        put_d2i("J", 2, 148, 1000); CHECK(S4("J", 1) == 1, "⑰ 准备：J 上传 2 件、编年史 148 的仓库");
        put("J2", "save/Settings/Settings.json", "{}", 1); CHECK(S4("J2", 1) == 1 && d2i_items("J2", D2I) == 2, "(17) prep: J2 gets the 2-item stash");
        put_d2i("J", 0, 148, 2000); dm_cloud_conflict_take();
        CHECK(S4("J", 0) == 1 && !dm_cloud_conflict_take(), "(17) running: the stash J emptied (0 items, 148; only J changed) is uploaded");
        CHECK(S4("J", 1) == 0 && d2i_items("J", D2I) == 0 && inbox_n("J") == 0, "(17) J launch: the 2 items do not come back (no duplicate of what the character holds)");
        CHECK(S4("J2", 1) == 1 && d2i_items("J2", D2I) == 0 && inbox_n("J2") == 0 && !dm_cloud_conflict_take() && count_bak("J2", "_stash-") == 1,
              "(17) J2 launch (only the cloud changed): takes the emptied stash without a fork or inbox entry; its 2-item copy is in its backup");
        put_d2i("J2", 3, 148, 3000); CHECK(S4("J2", 0) == 1, "(17) prep: J2 puts 3 items in and uploads");
        put_d2i_salt("J", 0, 148, 4000, 7);   // J's stash replaced by another empty new save (different bytes): both sides changed, J's is later
        CHECK(S4("J", 0) == 0, "(17) both sides changed and J's later copy is an empty new save: still a fork, nothing uploaded while running");
        CHECK(S4("J", 1) == 1 && d2i_items("J", D2I) == 3 && inbox_n("J") == 0, "(17) J launch: the cloud's 3 items are kept; the empty copy is not inboxed");
        put_d2i("J", 0, 300, 5000); CHECK(S4("J", 0) == 1, "⑰ 编年史长过新建大小的仓库取空：照常上传");
        // ⑱ rf3 真形状：云端专家仓库是另一台的预置（stash_seed.h），本机还没有专家仓库档（启动同步跑在写预置之前）→ 不拦、不弹；本机写了预置后上传也不拦
        // (18) The real rf3 shape: the cloud hardcore stash is another device's preset (stash_seed.h), and the local device has no hardcore stash file yet (startup sync runs before the preset is written) -> not blocked, no prompt; uploading after the local device writes the preset is not blocked either
#ifdef T_HAVE_STASH_SEED
        { char cd5[1024]; snprintf(cd5, sizeof cd5, "%s/cloud5", R); mkdir(cd5, 0755);
          #define S5(dev, down) (path(s3, dev, "save"), path(b3, dev, "bak"), path(t3, dev, "state"), dm_cloud_sync(s3, cd5, b3, t3, down, 0, NULL))
          #define HC "save/ModernSharedStashHardCoreV2/ModernSharedStashHardCoreV2.d2i"
          put_bin("K", HC, g_stash_seed, sizeof g_stash_seed, 1000); put_d2i("K", 3, 400, 1000); CHECK(S5("K", 1) == 1, "⑱ 准备：K 上传（专家仓库是预置）");
          { char p5[1024]; path(p5, "K", HC); unlink(p5); } put_d2i("K", 3, 400, 2000); dm_cloud_conflict_take();
          S5("K", 0); CHECK(!dm_cloud_conflict_take(), "⑱ 本机还没有专家仓库档、云端只是预置：不算丢东西，不弹");
          put_bin("K", HC, g_stash_seed, sizeof g_stash_seed, 3000); S5("K", 0); CHECK(!dm_cloud_conflict_take(), "⑱ 本机写了预置再上传：不拦");
          // ⑲ 反过来：云端有真东西，本机是预置 → 照拦
          // (19) The reverse: the cloud has real content and the local copy is the preset -> still blocked
          put_d2i("K", 5, 600, 4000); S5("K", 0); dm_cloud_conflict_take();
          { uint8_t big[700]; memset(big, 0, sizeof big); uint32_t l1 = 68, l2 = 300; memcpy(big, "\x55\xaa\x55\xaa", 4); memcpy(big + 16, &l1, 4); memcpy(big + 64, "JM", 2); big[66] = 9; memcpy(big + 68, "\x55\xaa\x55\xaa", 4); memcpy(big + 84, &l2, 4); big[132] = 0xc0;
            put_bin("K", HC, big, 68 + 300, 5000); CHECK(S5("K", 0) == 1, "⑲ 准备：专家仓库有 9 件、编年史 300 上传"); }
          put_bin("K", HC, g_stash_seed, sizeof g_stash_seed, 6000);
          CHECK(S5("K", 0) == 0 && !dm_cloud_conflict_take(), "⑲ 本机变回预置要盖 9 件的那份：分叉，运行中不动");
          #undef HC
          #undef S5
        }
#else
        printf("跳过 ⑱ ⑲：没有共享仓库预置 stash_seed.h（公开仓库不带）\n");
#endif
        #undef S4
      }
      #undef D2I
      #undef S3
    }
    // ⑳ 同名角色（iCloud 离线冲突第 3 条）：云端有 P 的 Foo（1000 秒）；新设备 Q 从没同步过、离线建了同名 Foo（5000 秒）和 Bar → 首次同步：Foo 用云端那份、
    //    本机 Foo 整份进备份 Foo-5000000、记下名字待进局提示（取一次后清空）；Bar 上传、不进提示
    // (20) Same-name character (iCloud offline conflict item 3): the cloud has P's Foo (1000 s); new device Q has never synced and created a Foo with the same name (5000 s) and a Bar offline -> first sync: Foo uses the cloud copy,
    //    the local Foo goes into the backup whole as Foo-5000000, the name is recorded for a notice after entering a run (cleared once taken); Bar is uploaded and not part of the notice
    { char c6[1024], s6[1024], b6[1024], t6[1024]; snprintf(c6, sizeof c6, "%s/cloud6", R); mkdir(c6, 0755); char nm[256];
      #define S6(dev, down) (path(s6, dev, "save"), path(b6, dev, "bak"), path(t6, dev, "state"), dm_cloud_sync(s6, c6, b6, t6, down, 0, NULL))
      put("P", "save/Foo/Foo.d2s", "fooP", 1000); CHECK(S6("P", 1) == 1, "⑳ 准备：P 上传 Foo");
      dm_cloud_char_conflict_take(nm, sizeof nm);
      put("Q", "save/Foo/Foo.d2s", "fooQ", 5000); put("Q", "save/Bar/Bar.d2s", "barQ", 5000);
      CHECK(S6("Q", 1) == 2, "⑳ Q 首次同步：Foo 下载、Bar 上传，共 2 个动作");
      CHECK(get("Q", "save/Foo/Foo.d2s") && !strcmp(get("Q", "save/Foo/Foo.d2s"), "fooP"), "⑳ Q 的 Foo 应换成云端那份 fooP");
      CHECK(count_bak("Q", "Foo-5000000") == 1, "⑳ Q 本机那份 Foo 应进备份 Foo-5000000");
      CHECK(dm_cloud_char_conflict_take(nm, sizeof nm) == 1 && !strcmp(nm, "Foo"), "⑳ 记下同名角色 Foo 待提示，实为「%s」", nm);
      CHECK(dm_cloud_char_conflict_take(nm, sizeof nm) == 0, "⑳ 取过一次后清空");
      CHECK(S6("Q", 1) == 0 && dm_cloud_char_conflict_take(nm, sizeof nm) == 0, "⑳ 再同步无动作、不再提示");
      #undef S6
    }
    // ㉑ iCloud 离线冲突三个场景（第一笔口径：两台设备的全部数据都在——仓库或收件箱，且没有永久拦截）。老设备 A：10 件、编年史 600，已上云
    // (21) Three iCloud offline conflict scenarios (first-step criterion: all data of both devices is kept - in the stash or the inbox - and nothing is blocked forever). Old device A: 10 items, chronicle 600, already in the cloud
    { char c7[1024], s7[1024], b7[1024], t7[1024], cq[1024]; snprintf(c7, sizeof c7, "%s/cloud7", R); mkdir(c7, 0755); snprintf(cq, sizeof cq, "%s/cloud7q", R); mkdir(cq, 0755);
      #define S7(dev, cl, down) (path(s7, dev, "save"), path(b7, dev, "bak"), path(t7, dev, "state"), dm_cloud_sync(s7, cl, b7, t7, down, 0, NULL))
      #define D2I7 "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"
      put_d2i("A7", 10, 600, 1000); CHECK(S7("A7", c7, 1) == 1, "㉑ 准备：A 上传 10 件");
      dm_cloud_conflict_take();
      // ① 新设备 N 没登录时离线玩（3 件、编年史 300），之后登录、启动同步
      // (1) new device N plays offline while not signed in (3 items, chronicle 300), then signs in and syncs at launch
      put_d2i("N7", 3, 300, 5000);
      CHECK(S7("N7", c7, 1) == 1 && d2i_items("N7", D2I7) == 10, "㉑① N 首次同步：仓库是云端的 10 件");
      CHECK(inbox_n("N7") == 1 && inbox_has("N7", "mine.pack") && !inbox_has("N7", "base.pack") && dm_cloud_conflict_take() == 1, "㉑① N 本机 3 件进收件箱（没有基准）、提示一次");
      CHECK(S7("N7", c7, 1) == 0 && inbox_n("N7") == 1 && d2i_items("A7", D2I7) == 10, "㉑① 再启动无动作（没有永久状态）；A 的 10 件不动");
      // ② 新设备 M 离线期间编年史比云端长（900 > 600）：原先永久拦住，现在同样云端为现行版 + 收件箱
      // (2) new device M's chronicle grew longer than the cloud's while offline (900 > 600): previously blocked forever, now likewise the cloud is the current version + inbox
      put_d2i("M7", 3, 900, 5000);
      CHECK(S7("M7", c7, 0) == 0 && inbox_n("M7") == 0, "㉑② M 运行中：不动、不收（等启动）");
      CHECK(S7("M7", c7, 1) == 1 && d2i_items("M7", D2I7) == 10 && inbox_n("M7") == 1 && dm_cloud_conflict_take() == 1, "㉑② M 启动：仓库 10 件，本机 3 件进收件箱");
      CHECK(S7("M7", c7, 1) == 0 && S7("M7", c7, 0) == 0 && inbox_n("M7") == 1, "㉑② 再启动、再运行中都无动作：不是永久拦截");
      // ③ 新设备 Q 登录但离线、看到的云端是空的（cloud7q），上传了 3 件；联网后 iCloud 拿 Q 的包当现行版（盖到 cloud7 上）
      // (3) new device Q signs in but is offline and sees an empty cloud (cloud7q), uploads 3 items; once online, iCloud takes Q's pack as the current version (overwriting cloud7)
      put_d2i("Q7", 3, 300, 5000); CHECK(S7("Q7", cq, 1) == 1, "㉑③ 准备：Q 往看着是空的云端传了 3 件");
      char sp[1300], dp[1300], keep[1300]; snprintf(sp, sizeof sp, "%s/_stash.pack", cq); snprintf(dp, sizeof dp, "%s/_stash.pack", c7); snprintf(keep, sizeof keep, "%s/a_old.pack", R);
      rename(dp, keep); { FILE *fi = fopen(sp, "rb"), *fo = fopen(dp, "wb"); char buf[8192]; size_t k; while ((k = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, k, fo); fclose(fi); fclose(fo); }
      CHECK(S7("A7", c7, 1) == 1 && d2i_items("A7", D2I7) == 3, "㉑③ A 启动：云端（Q 的 3 件）为现行版");
      CHECK(inbox_n("A7") == 1 && inbox_has("A7", "base.pack") && dm_cloud_conflict_take() == 1, "㉑③ A 本机 10 件进收件箱，带上次同步的基准");
      CHECK(S7("A7", c7, 1) == 0 && inbox_n("A7") == 1, "㉑③ 再启动无动作");
      // iCloud 冲突版本：A 那台读出被换下的旧版本（a_old.pack，10 件）收进收件箱；只有预置 / 空档的版本不收
      // iCloud conflict version: device A reads out the replaced old version (a_old.pack, 10 items) and stores it in the inbox; a version with only the preset / an empty save is not stored
      { char ts7[1100]; path(s7, "A7", "save"); path(ts7, "A7", "state");
        // a_old.pack is A's own last upload: exactly the local copy A just put in the inbox. Storing it again would return the same 10 items twice (F-SAVE-3)
        CHECK(dm_cloud_inbox_add(s7, ts7, keep) == 0 && inbox_n("A7") == 1 && !dm_cloud_conflict_take(), "(21-3) the conflict version (10 items) is the local copy already in the inbox: not stored twice");
        put_d2i("C7", 7, 650, 1500); { char c7s[1100], c7b[1100], c7t[1100], c7c[1300], pk[1400]; path(c7s, "C7", "save"); path(c7b, "C7", "bak"); path(c7t, "C7", "state"); snprintf(c7c, sizeof c7c, "%s/cloud7c", R); mkdir(c7c, 0755);
          dm_cloud_sync(c7s, c7c, c7b, c7t, 1, 0, NULL); snprintf(pk, sizeof pk, "%s/_stash.pack", c7c);
          CHECK(dm_cloud_inbox_add(s7, ts7, pk) == 0 && inbox_n("A7") == 2 && dm_cloud_conflict_take() == 1, "(21-3) a conflict version with other content (7 items) is stored in the inbox");
          // the same conflict version offered again (a launch that could not mark it resolved): no second entry, no second notice
          CHECK(dm_cloud_inbox_add(s7, ts7, pk) == 0 && inbox_n("A7") == 2 && !dm_cloud_conflict_take(), "(21-3) the same conflict version again: not stored a second time"); }
        put_d2i("E7", 0, 148, 100); { char e7[1100], ee[1100], et[1100], pk[1300]; path(e7, "E7", "save"); path(ee, "E7", "bak"); path(et, "E7", "state"); char ce[1300]; snprintf(ce, sizeof ce, "%s/cloud7e", R); mkdir(ce, 0755);
          dm_cloud_sync(e7, ce, ee, et, 1, 0, NULL); snprintf(pk, sizeof pk, "%s/_stash.pack", ce);
          CHECK(dm_cloud_inbox_add(s7, ts7, pk) == 0 && inbox_n("A7") == 2, "㉑③ 空档的冲突版本：没东西，不收"); }
        CHECK(dm_cloud_inbox_add(s7, ts7, "/nonexistent.pack") == -1, "㉑③ 读不了的冲突版本：返回 -1"); }
      #undef S7
      #undef D2I7
    }
    // ㉒ 编年史段并集（第三笔）：两台设备各自找到过的条目都留下，同一条取更早的时刻
    // ㉒ Chronicle segment union (part three): entries found on each of the two devices are all kept; for the same entry the earlier time is taken
    { char c8[1024], s8[1024], b8[1024], t8[1024]; snprintf(c8, sizeof c8, "%s/cloud8", R); mkdir(c8, 0755);
      #define S8(dev, down) (path(s8, dev, "save"), path(b8, dev, "bak"), path(t8, dev, "state"), dm_cloud_sync(s8, c8, b8, t8, down, 0, NULL))
      uint32_t ia[] = { 1, 2 }, ta[] = { 100, 200 }, in_[] = { 2, 3 }, tn[] = { 150, 300 };
      put_chron("A8", 2, 2, ia, ta, 1000); CHECK(S8("A8", 1) == 1, "㉒ 准备：A 上传（1@100 2@200）");
      put_chron("N8", 0, 2, in_, tn, 5000);
      CHECK(S8("N8", 1) == 1 && !strcmp(chron_str("N8"), "1@100 2@150 3@300"), "㉒① N 首次同步：云端为现行版、编年史并进本机的 → 1@100 2@150 3@300，实为 %s", chron_str("N8"));
      CHECK(d2i_items("N8", "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i") == 2 && inbox_n("N8") == 1, "㉒① 物品页是云端的 2 件，本机那份照常进收件箱");
      CHECK(S8("N8", 0) == 1, "㉒② N 运行中：并过的本地当改过、传上去");
      CHECK(S8("A8", 1) == 1 && !strcmp(chron_str("A8"), "1@100 2@150 3@300"), "㉒② A 启动拿到并集，实为 %s", chron_str("A8"));
      CHECK(S8("A8", 1) == 0 && S8("N8", 1) == 0 && S8("N8", 0) == 0, "㉒② 之后两边都无动作（不来回传）");
      // ③ 本机后写赢（上传）时：启动时把云端多出的并进本机再传；运行中不动本地
      // ③ When local last-write wins (upload): at startup merge the extra cloud entries into local and then upload; at runtime leave local untouched
      uint32_t i4[] = { 1, 2, 3, 4 }, t4[] = { 100, 150, 300, 400 }, i5[] = { 1, 2, 3, 5 }, t5[] = { 100, 150, 300, 500 };
      put_chron("N8", 2, 4, i5, t5, 6000); CHECK(S8("N8", 0) == 1, "㉒③ 准备：N 加了 5、上传");
      put_chron("A8", 2, 4, i4, t4, 7000);
      CHECK(S8("A8", 1) == 1 && !strcmp(chron_str("A8"), "1@100 2@150 3@300 4@400 5@500"), "㉒③ A 后写赢、启动时上传：本机先并进云端的 5 → 实为 %s", chron_str("A8"));
      CHECK(S8("N8", 1) == 1 && !strcmp(chron_str("N8"), "1@100 2@150 3@300 4@400 5@500"), "㉒③ N 启动拿到五条，实为 %s", chron_str("N8"));
      #undef S8
    }
    // ㉓ 损坏的共享仓库档：第二段的段长 0xFFFFFFBC 让 32 位的「起点 68 + 段长」绕回 0。旧判法（o + ln > 大小）照放，o 加上段长又回到 0，
    //    逐段扫描（rich_of / chron_seg）死循环、同步卡死在 100% CPU。在子进程里跑、5 秒闹钟：同步得照常结束
    // ㉓ Corrupted shared stash: the second segment's length 0xFFFFFFBC makes the 32-bit "start 68 + length" wrap to 0. The old check (o + ln > size) let it through, o plus the length went back to 0,
    //    and the segment walk (rich_of / chron_seg) looped forever with sync stuck at 100% CPU. Run in a child process with a 5-second alarm: sync must finish normally
    { static uint8_t b[136]; uint32_t l1 = 68, l2 = 0xFFFFFFBCu;
      memcpy(b, "\x55\xaa\x55\xaa", 4); memcpy(b + 16, &l1, 4); memcpy(b + 64, "JM", 2); b[66] = 1;
      memcpy(b + 68, "\x55\xaa\x55\xaa", 4); memcpy(b + 68 + 16, &l2, 4);
      const char *devs[2] = { "W9", "X9" };
      put_d2i("X9", 2, 300, 9100); CHECK(dsync("X9", 0, 9100000) >= 0, "㉓ 准备：X 先把一份正常的共享仓库传上云");   // 第二台带着坏档来时走并编年史那条路（chron_seg） | the second device then arrives with the corrupted file and takes the chronicle-merge path (chron_seg)
      for (int k = 0; k < 2; k++) {
          put_bin(devs[k], "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i", b, sizeof b, 9200 + k);
          pid_t pid = fork();
          if (!pid) { alarm(5); dsync(devs[k], 1, 9200000 + (uint64_t)k * 1000); _exit(0); }
          int st = 0; waitpid(pid, &st, 0);
          CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "㉓ %s 带着段长绕回的坏仓库档同步：5 秒内照常结束（实为%s）", devs[k],
                WIFSIGNALED(st) ? (WTERMSIG(st) == SIGALRM ? "卡死被闹钟杀掉" : "被信号杀掉") : "异常退出");
      }
    }
    // ㉔ 包与单位绑定：云端放进来的包里的路径必须落在它自己的单位里。Mallory.pack 带着 Victim/Victim.d2s 与 Ghost/Ghost.d2s、Evil.pack 带着 Settings/… 与根目录的共享仓库侧文件、
    //    _stash.pack 带着角色档、_cloudinbox.pack（不是单位名）：一律不换进本地；正常的 Good.pack 照常下载（对照）
    // ㉔ Binding packs to their unit: paths in a pack placed in the cloud must stay inside its own unit. Mallory.pack carrying Victim/Victim.d2s and Ghost/Ghost.d2s, Evil.pack carrying Settings/... and the shared stash side file at the root,
    //    _stash.pack carrying a character file, _cloudinbox.pack (not a unit name): none is swapped into the local save; a proper Good.pack downloads as usual (control)
    { char c9[1024], s9[1024], b9[1024], t9[1024], p[1200]; snprintf(c9, sizeof c9, "%s/cloud9", R); mkdir(c9, 0755);
      #define S9(dev, down) (path(s9, dev, "save"), path(b9, dev, "bak"), path(t9, dev, "state"), dm_cloud_sync(s9, c9, b9, t9, down, 0, NULL))
      put("V9", "save/Victim/Victim.d2s", "mine", 1000); put("V9", "save/Settings/Settings.json", "{\"mine\":1}", 1000); put("V9", "save/ModernSharedStashSoftCoreV2.swvault", "vault", 1000);
      const char *r1[] = { "Mallory/Mallory.d2s", "Ghost/Ghost.d2s", "Victim/Victim.d2s" }, *x1[] = { "m", "boo", "pwned" };
      const char *r2[] = { "Evil/Evil.d2s", "ModernSharedStashSoftCoreV2.swvault", "Settings/Settings.json" }, *x2[] = { "e", "pwned", "{\"pwned\":1}" };
      const char *r3[] = { "Victim/Victim.d2s" }, *x3[] = { "pwned" };
      const char *r4[] = { "_cloudinbox/1-0/mine.pack" }, *x4[] = { "x" };
      const char *r5[] = { "Good/Good.d2s", "Good/Good.key" }, *x5[] = { "g", "k" };
      snprintf(p, sizeof p, "%s/Mallory.pack", c9); pack_put(p, 5000000, 3, r1, x1);
      snprintf(p, sizeof p, "%s/Evil.pack", c9); pack_put(p, 5000000, 3, r2, x2);
      snprintf(p, sizeof p, "%s/_stash.pack", c9); pack_put(p, 5000000, 1, r3, x3);
      snprintf(p, sizeof p, "%s/_cloudinbox.pack", c9); pack_put(p, 5000000, 1, r4, x4);
      snprintf(p, sizeof p, "%s/Good.pack", c9); pack_put(p, 5000000, 2, r5, x5);
      int n9 = S9("V9", 1);
      CHECK(get("V9", "save/Victim/Victim.d2s") && !strcmp(get("V9", "save/Victim/Victim.d2s"), "mine"), "㉔ Mallory.pack 里的 Victim/Victim.d2s 没盖掉本地的 Victim");
      CHECK(get("V9", "save/Settings/Settings.json") && !strcmp(get("V9", "save/Settings/Settings.json"), "{\"mine\":1}"), "㉔ Evil.pack 里的 Settings/ 没换进本地");
      CHECK(get("V9", "save/ModernSharedStashSoftCoreV2.swvault") && !strcmp(get("V9", "save/ModernSharedStashSoftCoreV2.swvault"), "vault"), "㉔ Evil.pack 里根目录的共享仓库侧文件没盖掉本地那份");
      CHECK(!get("V9", "save/Mallory/Mallory.d2s") && !get("V9", "save/Ghost/Ghost.d2s") && !get("V9", "save/Evil/Evil.d2s") && !get("V9", "save/_cloudinbox/1-0/mine.pack"), "㉔ 越界的包整份不下载（Mallory / Ghost / Evil / _cloudinbox 都没出现）");
      CHECK(get("V9", "save/Good/Good.d2s") && !strcmp(get("V9", "save/Good/Good.d2s"), "g") && n9 == 2, "㉔ 对照：Good.pack 照常下载、Victim 照常上传（动作 %d 个，应为 2）", n9);
      // 冲突版本收件箱也只收真共享仓库的包 | the conflict-version inbox also takes only real shared stash packs
      char sv[1024], st9[1024]; path(sv, "V9", "save"); path(st9, "V9", "state"); snprintf(p, sizeof p, "%s/_stash.pack", c9);
      CHECK(dm_cloud_inbox_add(sv, st9, p) == -1 && inbox_n("V9") == 0, "㉔ 带着角色档的「共享仓库」冲突版本不收进收件箱");
      #undef S9
    }
    #define D2IX "save/ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"
    // (25) F-SAVE-3: inbox entries are idempotent (each merge returns what its copy holds, so a second entry with the same content returns it twice).
    //      Cloud: P's 10 items (chronicle 600); new device N (3 items, 300) forks at launch.
    //      a) the swap fails (save root not writable): the entry made for it is removed, the local copy stays the live stash. Before, it stayed, and
    //         each later launch forked again and added another entry;
    //      b) a kill between storing the entry and the swap leaves it behind: the next launch finds the same content and does not store it again;
    //      c) a kill, then a launch where the stash pack is not ready: the leftover entry holding the live stash is removed;
    //      d) an entry the merge retired goes into the ledger: the same content is never stored again
    { char c25[1024], s25[1024], b25[1024], t25[1024], ib[1100], k25[1024], kp[1100], dst[1300]; snprintf(c25, sizeof c25, "%s/cloud25", R); mkdir(c25, 0755); snprintf(k25, sizeof k25, "%s/cloud25k", R); mkdir(k25, 0755);
      #define S25(dev, cl, down, rdy) (path(s25, dev, "save"), path(b25, dev, "bak"), path(t25, dev, "state"), dm_cloud_sync(s25, cl, b25, t25, down, 0, rdy))
      put_d2i("P25", 10, 600, 1000); CHECK(S25("P25", c25, 1, NULL) == 1, "(25) prep: P uploads 10 items");
      put_d2i("N25", 3, 300, 5000); dm_cloud_conflict_take();
      path(ib, "N25", "save/_cloudinbox"); mkdir(ib, 0755); path(s25, "N25", "save"); chmod(s25, 0555);   // the inbox folder stays writable, the save root does not: the swap's .cloudin cannot be made
      int r25 = S25("N25", c25, 1, NULL); path(s25, "N25", "save"); chmod(s25, 0755);
      CHECK(r25 == 0 && d2i_items("N25", D2IX) == 3 && inbox_n("N25") == 0, "(25a) the swap failed: local still 3 items, no inbox entry left (actions %d, entries %d)", r25, inbox_n("N25"));
      CHECK(S25("N25", c25, 1, NULL) == 1 && d2i_items("N25", D2IX) == 10 && inbox_n("N25") == 1 && dm_cloud_conflict_take() == 1, "(25a) next launch: swapped, exactly one entry (%d)", inbox_n("N25"));
      CHECK(S25("N25", c25, 1, NULL) == 0 && inbox_n("N25") == 1, "(25a) and no more after that");
      // the pack of a 4-item / 350 stash, as a launch killed after storing it would have left it in the inbox
      put_d2i("K25", 4, 350, 5000); S25("K25", k25, 1, NULL); snprintf(kp, sizeof kp, "%s/_stash.pack", k25);
      put_d2i("N25b", 4, 350, 5000); path(dst, "N25b", "save/_cloudinbox/5000000-0"); { char d0[1100]; path(d0, "N25b", "save/_cloudinbox"); mkdir(d0, 0755); } mkdir(dst, 0755); strcat(dst, "/mine.pack"); cp_file(kp, dst);
      CHECK(S25("N25b", c25, 1, NULL) == 1 && d2i_items("N25b", D2IX) == 10 && inbox_n("N25b") == 1, "(25b) relaunch after a kill: swapped, the leftover entry is the only one (%d)", inbox_n("N25b"));
      put_d2i("N25c", 4, 350, 5000); path(dst, "N25c", "save/_cloudinbox/5000000-0"); { char d0[1100]; path(d0, "N25c", "save/_cloudinbox"); mkdir(d0, 0755); } mkdir(dst, 0755); strcat(dst, "/mine.pack"); cp_file(kp, dst);
      CHECK(S25("N25c", c25, 1, not_stash) == 0 && d2i_items("N25c", D2IX) == 4 && inbox_n("N25c") == 0, "(25c) relaunch after a kill, stash pack not ready: the entry holding the live stash is removed (%d)", inbox_n("N25c"));
      { DIR *dp = opendir(ib); struct dirent *e; char nm[256] = ""; while (dp && (e = readdir(dp))) if (e->d_name[0] != '.') strlcpy(nm, e->d_name, sizeof nm); if (dp) closedir(dp);
        path(s25, "N25", "save"); path(t25, "N25", "state");
        CHECK(nm[0] && dm_cloud_inbox_retire(s25, nm) == 0 && inbox_n("N25") == 0 && get("N25", "save/_cloudinbox/.done"), "(25d) the merge retires the finished entry %s: removed, its content in the ledger", nm);
        put_d2i("K25d", 3, 300, 5000); char k25d[1024]; snprintf(k25d, sizeof k25d, "%s/cloud25d", R); mkdir(k25d, 0755); S25("K25d", k25d, 1, NULL); snprintf(kp, sizeof kp, "%s/_stash.pack", k25d);
        path(s25, "N25", "save"); path(t25, "N25", "state");
        CHECK(dm_cloud_inbox_add(s25, t25, kp) == 0 && inbox_n("N25") == 0, "(25d) the same content offered again (a conflict version): not stored, it was merged already"); }
      #undef S25
    }
    // (26) F-SAVE-5: the launch-time chronicle union (both sides changed, local later: upload) writes the merged stash to a temp folder and renames it
    //      over the live file, only for the files that changed. A file-size limit makes that write fail half way, as a full disk does: the live file
    //      must stay whole (before: rewritten in place with "wb", left truncated) and nothing is uploaded
    { char c26[1024], s26[1024], b26[1024], t26[1024], pv[1024]; snprintf(c26, sizeof c26, "%s/cloud26", R); mkdir(c26, 0755);
      #define S26(dev, down) (path(s26, dev, "save"), path(b26, dev, "bak"), path(t26, dev, "state"), dm_cloud_sync(s26, c26, b26, t26, down, 0, NULL))
      uint32_t i2[] = { 1, 2 }, t2[] = { 100, 200 }, i5[] = { 1, 2, 5 }, t5[] = { 100, 200, 500 }, i4[] = { 1, 2, 4 }, t4[] = { 100, 200, 400 };
      put_chron("A26", 2, 2, i2, t2, 1000); put("A26", "save/ModernSharedStashSoftCoreV2.swvault", "side", 1000);
      put("N26", "save/Settings/Settings.json", "{}", 1); CHECK(S26("A26", 1) == 1 && S26("N26", 1) == 1, "(26) prep: A uploads, N gets it");
      put_chron("N26", 2, 3, i5, t5, 6000); CHECK(S26("N26", 0) == 1, "(26) prep: N adds 5 and uploads");
      put_chron("A26", 2, 3, i4, t4, 7000);
      uint8_t before[8192], after[8192]; long nb = file_bytes("A26", D2IX, before, sizeof before); struct stat sv0, sv1; path(pv, "A26", "save/ModernSharedStashSoftCoreV2.swvault"); stat(pv, &sv0);
      pid_t pid = fork();   // the cloud copy's backup (246 bytes) fits under the limit, the merged stash (256 bytes) does not
      if (!pid) { signal(SIGXFSZ, SIG_IGN); struct rlimit rl = { 250, 250 }; setrlimit(RLIMIT_FSIZE, &rl); _exit(S26("A26", 1) == 0 ? 0 : 3); }
      int st = 0; waitpid(pid, &st, 0); long na = file_bytes("A26", D2IX, after, sizeof after);
      CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0 && nb > 0 && na == nb && !memcmp(before, after, (size_t)nb), "(26) the merged stash could not be written: nothing uploaded, the live file whole (%ld of %ld bytes, exit %d)", na, nb, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
      CHECK(S26("A26", 1) == 1 && !strcmp(chron_str("A26"), "1@100 2@200 4@400 5@500"), "(26) without the limit: merged and uploaded, %s", chron_str("A26"));
      stat(pv, &sv1); CHECK(sv1.st_ino == sv0.st_ino && sv1.st_ctimespec.tv_sec == sv0.st_ctimespec.tv_sec && sv1.st_ctimespec.tv_nsec == sv0.st_ctimespec.tv_nsec, "(26) the side file the union did not change was not rewritten");
      #undef S26
    }
    // (27) F-SAVE-7: Zed is synced; its folder is gone before a launch whose Zed pack is not ready (skipped). The first sync after a commit must not write
    //      a deletion marker: this run never saw Zed present, so it was lost locally, not deleted (before: marker written, the deletion spread to every
    //      device). The next launch restores it. Control: deleted after this run saw it present -> marker as before
    { char c27[1024], s27[1024], b27[1024], t27[1024], zp[1100]; snprintf(c27, sizeof c27, "%s/cloud27", R); mkdir(c27, 0755); snprintf(zp, sizeof zp, "%s/Zed.pack", c27);
      #define S27(dev, down, now, rdy) (path(s27, dev, "save"), path(b27, dev, "bak"), path(t27, dev, "state"), dm_cloud_sync(s27, c27, b27, t27, down, now, rdy))
      put("Z27", "save/Zed/Zed.d2s", "z1", 1000); CHECK(S27("Z27", 1, 0, NULL) == 1, "(27) prep: Zed uploaded");
      { char p[1024]; path(p, "Z27", "save/Zed/Zed.d2s"); unlink(p); path(p, "Z27", "save/Zed"); rmdir(p); }
      CHECK(S27("Z27", 1, 0, not_zed) == 0, "(27) launch: Zed missing here and its pack not ready: skipped");
      int r27 = S27("Z27", 0, 5000000, NULL);
      CHECK(r27 == 0 && pack_tomb(zp) == 0, "(27) first sync after a commit: no deletion marker (actions %d, marker %d)", r27, pack_tomb(zp));
      CHECK(S27("Z27", 1, 0, NULL) == 1 && get("Z27", "save/Zed/Zed.d2s") && !strcmp(get("Z27", "save/Zed/Zed.d2s"), "z1"), "(27) next launch: Zed restored");
      { char p[1024]; path(p, "Z27", "save/Zed/Zed.d2s"); unlink(p); path(p, "Z27", "save/Zed"); rmdir(p); }
      CHECK(S27("Z27", 0, 6000000, NULL) == 1 && pack_tomb(zp) == 1, "(27) control: deleted after this run saw it present: deletion marker written");
      #undef S27
    }
    // (28) F-SAVE-10: P deletes Foo (marker at 9000 s). New device Q (never synced) has its own Foo from 5000 s: the first sync uploads it as a different
    //      character (before: "later write wins", the newer marker moved it to the backup without a word). No same-name notice: nothing was replaced
    { char c28[1024], s28[1024], b28[1024], t28[1024], fp[1100], nm[256]; snprintf(c28, sizeof c28, "%s/cloud28", R); mkdir(c28, 0755); snprintf(fp, sizeof fp, "%s/Foo.pack", c28);
      #define S28(dev, down, now) (path(s28, dev, "save"), path(b28, dev, "bak"), path(t28, dev, "state"), dm_cloud_sync(s28, c28, b28, t28, down, now, NULL))
      put("P28", "save/Foo/Foo.d2s", "fooP", 1000); CHECK(S28("P28", 1, 0) == 1, "(28) prep: P uploads Foo");
      { char p[1024]; path(p, "P28", "save/Foo/Foo.d2s"); unlink(p); path(p, "P28", "save/Foo"); rmdir(p); }
      CHECK(S28("P28", 0, 9000000) == 1 && pack_tomb(fp) == 1, "(28) prep: P deletes Foo, marker at 9000 s");
      dm_cloud_char_conflict_take(nm, sizeof nm);
      put("Q28", "save/Foo/Foo.d2s", "fooQ", 5000);
      CHECK(S28("Q28", 1, 0) == 1 && get("Q28", "save/Foo/Foo.d2s") && !strcmp(get("Q28", "save/Foo/Foo.d2s"), "fooQ") && count_bak("Q28", "Foo-") == 0 && pack_tomb(fp) == 0,
            "(28) Q's first sync: its Foo kept and uploaded, nothing moved to its backup");
      CHECK(!dm_cloud_char_conflict_take(nm, sizeof nm), "(28) no same-name notice");
      CHECK(S28("P28", 1, 0) == 1 && get("P28", "save/Foo/Foo.d2s") && !strcmp(get("P28", "save/Foo/Foo.d2s"), "fooQ"), "(28) P gets Q's Foo at its next launch");
      #undef S28
    }
    // (29) F-SAVE-16: a ".dm-case-N" alias (hle_fs.c: a character whose name differs from another's only in case) is not synced and every sync says so
    //      in red (before: left out without a word); dm_cloud_char_file finds each character by its exact guest name. On a case-insensitive file system
    //      a cloud unit "Alice" must not be taken for the local folder "alice" of another character (before: that folder went to the backup, replaced)
    { char c29[1024], c29b[1024], s29[1024], b29[1024], t29[1024], cf[1100]; snprintf(c29, sizeof c29, "%s/cloud29", R); mkdir(c29, 0755); snprintf(c29b, sizeof c29b, "%s/cloud29b", R); mkdir(c29b, 0755);
      #define S29(dev, cl) (path(s29, dev, "save"), path(b29, dev, "bak"), path(t29, dev, "state"), dm_cloud_sync(s29, cl, b29, t29, 1, 0, NULL))
      put("K29", "save/alice/alice.d2s", "lower", 1000); put("K29", "save/.dm-case-1/Alice.d2s", "upper", 1000);
      { char p[1024]; path(p, "K29", "save/.dm-case-1"); setxattr(p, "org.d2rmobile.guest-name", "Alice", 5, 0, XATTR_NOFOLLOW); }
      g_log[0] = 0; int r29 = S29("K29", c29);
      CHECK(r29 == 1 && log_has("【红】", "Alice"), "(29) the aliased Alice: not synced, said in red (actions %d)", r29);
      path(s29, "K29", "save");
      CHECK(dm_cloud_char_file(s29, "Alice", cf, sizeof cf) && strstr(cf, "/.dm-case-1/Alice.d2s") && dm_cloud_char_file(s29, "alice", cf, sizeof cf) && strstr(cf, "/alice/alice.d2s")
            && !dm_cloud_char_file(s29, "ALICE", cf, sizeof cf), "(29) dm_cloud_char_file finds each character by its exact guest name");
      { char up[1024]; struct stat x; path(up, "K29", "save/ALICE/ALICE.D2S");
        if (!stat(up, &x)) { put("L29", "save/alice/alice.d2s", "mine", 1000); const char *r[] = { "Alice/Alice.d2s" }, *t[] = { "other" }; char p[1100]; snprintf(p, sizeof p, "%s/Alice.pack", c29b); pack_put(p, 5000000, 1, r, t);
            S29("L29", c29b);
            CHECK(get("L29", "save/alice/alice.d2s") && !strcmp(get("L29", "save/alice/alice.d2s"), "mine") && count_bak("L29", "alice-") == 0 && count_bak("L29", "Alice-") == 0,
                  "(29) the cloud's Alice did not replace the local alice (%s)", get("L29", "save/alice/alice.d2s") ? get("L29", "save/alice/alice.d2s") : "gone"); }
        else printf("skipping the case-insensitive half of (29): this file system is case-sensitive\n"); }
      #undef S29
    }
    printf("t_cloudsync: %d 项，%d 失败\n", checks, fails);
    return fails != 0;
}
