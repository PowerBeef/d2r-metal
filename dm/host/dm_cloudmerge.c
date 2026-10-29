// iCloud 共享仓库分叉的局内并回（iCloud 离线冲突第 1 条第二笔，口径：两台设备的数据一件不少、没有永久拦截）。
// 第一笔（dm_cloudsync.c）在启动同步时以云端为现行版，本机那份存进待合并收件箱 <存档>/_cloudinbox/<条目>/mine.pack（+ base.pack 基准）。
// 这里在局内把本机那份里「云端没有的」物品和金币还给玩家：
//   物品：开着储藏箱、人在箱子旁时逐件建进当前角色的背包（读位流建物品 0x5793a0 + 0x6564e0 → 原版找空位 0x55f620
//         → 读档放物品 0x6f1940 → 通知客户端 0x642f70），玩家自己拖进仓库；背包放不下就停，提示一次，下次开箱接着放。
//   金币：加到共享页的金币属性 15（每页上限 250 万），照原版存取金币那条路通知客户端（本文件 dm_stash_gold_notify）；放不下的留到下次。
// 挑哪些（去重）：mine.pack 里当前这一类共享仓库档（dm_stash_name：标准 / 专家 × 毁灭之王 / 资料片）逐页拆成单件位流（游戏自己的读位流函数，
//   孔里镶的跟着母件），与云端（= 现在服务端共享页 + 高级仓库里的物品，原版写位流）按「位流去掉位置字段」比：
//   · 有基准（base.pack）且云端编年史不短于基准（编年史只增不减 ⇒ 云端是基准之后接着玩的）：只还本机新增的 = 本机件数 − 基准件数（同一串按个数算）；
//   · 没有基准 / 分不清来历：同一件取一份的并集 = 本机件数 − 云端件数。
//   金币：有基准还「本机 − 基准」，没有基准还「本机 − 预置」（新设备的共享仓库预置 250 万，见 hle_fs.c stash_seed）；本机花掉的不扣云端。
//   本机取走的（基准有、本机没有）不从云端扣：单机游戏里多出一件比丢一件好。
// 落盘（不变式：同一件只在一处）：算好的清单写进条目目录里的 plan-<仓库名>.bin（工作目录里，随存档提交原子换入）；
//   一件放进背包后，提交前钩子（写盘线程）在这次要提交的角色档里找到放好的那一串才从清单划掉；金币在这次要提交的共享仓库档改写时刻晚于加金币那一刻才划掉。
// 线程：dm_cloudmerge_srv_tick 在服务端拍（diag.c hud_tick）；提交前钩子 cm_hook 在写盘线程（持存档锁），两边都拿 g_mu。
// In-game merge of a forked iCloud shared stash (iCloud offline conflict item 1, part two; principle: not a single item from either device is lost, and nothing is blocked permanently).
// Part one (dm_cloudsync.c) takes the cloud as current at startup sync, and stores the local copy into the pending-merge inbox <save>/_cloudinbox/<entry>/mine.pack (+ base.pack baseline).
// Here, in game, the items and gold in the local copy that "the cloud does not have" are returned to the player:
//   Items: with the stash open and the player next to it, created one by one into the current character's inventory (create item from bitstream 0x5793a0 + 0x6564e0 → original free-slot search 0x55f620
//         → place item on load 0x6f1940 → notify client 0x642f70); the player drags them into the stash. Stops when the inventory is full, shows a notice once, and continues on the next stash open.
//   Gold: added to the shared page gold stat 15 (2.5 million cap per page), notifying the client via the original gold deposit path (dm_stash_gold_notify in this file); what does not fit waits for next time.
// What to pick (dedup): the shared stash file of the current type in mine.pack (dm_stash_name: standard / expert × Lord of Destruction / expansion) is split page by page into single-item bitstreams (the game's own bitstream read function;
//   socketed items follow their parent), and compared against the cloud (= items now in the server shared pages + advanced stash, bitstreams written the original way) by "bitstream with position fields stripped":
//   · With a baseline (base.pack) and the cloud Chronicle not shorter than the baseline (the Chronicle only grows ⇒ the cloud continued play after the baseline): only return local additions = local count − baseline count (identical bitstreams counted by number);
//   · No baseline / origin unclear: union taking one copy of each identical item = local count − cloud count.
//   Gold: with a baseline return "local − baseline"; without one return "local − preset" (the new-device shared stash preset is 2.5 million, see hle_fs.c stash_seed); local spending is not deducted from the cloud.
//   Items taken locally (in the baseline, not local) are not deducted from the cloud: in a single-player game an extra item is better than a lost one.
// Persistence (invariant: an item exists in only one place): the computed list is written to plan-<stash name>.bin in the entry directory (in the working directory, swapped in atomically with the save commit);
//   after an item is placed in the inventory, the pre-commit hook (disk write thread) only removes it from the list once it finds the placed bitstream in the character save of this commit; gold is removed only when the modification time of the shared stash file in this commit is later than the moment gold was added.
//   List empty = this stash type is fully merged; once every stash type in the entry is, the entry is retired (see F-SAVE-11 below). The chronicle
//   union (part three) happens in the launch sync itself (dm_cloudsync.c), so it needs nothing from the entry.
// Threads: dm_cloudmerge_srv_tick runs on the server tick (diag.c hud_tick); the pre-commit hook cm_hook runs on the disk write thread (holding the save lock); both take g_mu.
// Later fixes:
//   - Crossing off only counts once the commit lands (F-SAVE-2): cm_hook records what it settled, and the commit outcome hook cm_post (hle_fs.c
//     g_dm_post_commit_hook[3]) puts it back to "placed, awaiting proof" and marks the list dirty when the commit did not land (refused after a write
//     failure, the work folder recloned from the last committed version; or the clone / swap failed). Before, the list was rewritten only in the lost
//     work folder while the hook's state said "written", so the committed list still held what the player already had and handed it out again.
//   - Returns awaiting proof survive leaving the game and entering it again (F-SAVE-1): the list, the placed returns and the gold sent stay until a
//     commit proves them. Before, every player change reread the list from disk and handed out again whatever was placed but not yet proven.
//   - A placed return is proven only by the character it was placed into, or the shared stash it may have been moved to (F-SAVE-15): a simple item
//     (no unique id) identical to it in another character's file proved nothing but crossed it off.
//   - Each character file is found under its guest name (dm_cloud_char_file: ".dm-case-N" aliases included), never through a path that a
//     case-insensitive file system resolves to another character's folder.
//   - The inbox is read whole and in name order, and an entry whose every stash is finished is retired in the pre-commit hook (F-SAVE-11):
//     dm_cloud_inbox_retire removes it and keeps its content digest in a ledger so the same content is never stored again. Before, only the
//     first 64 entries in readdir order (not name order on APFS) were read and none was ever removed, so later ones could stay unmerged forever.
#include "hle.h"
#include "dm_i18n.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

const char *dm_save_workdir(void); const char *dm_stash_name(uint8_t *b); int dm_request_save(void); uint64_t dm_fs_commit_count(void);
int dm_item_write(uint8_t *b, uint64_t it, uint8_t *buf); uint64_t dm_item_make_n(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out, int *bytes_out);
void *dm_memfind(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn); void dm_game_notice(uint8_t *main_base, const char *text); int dm_ui_panel_state(void);
// 玩家物品栏对象（换手位时取 +0x98），签名 0x01020304 才算；玩家能用的共享页单位；改仓库金币后通知客户端
// The player's inventory object (+0x98 while the hands are swapped), valid only with signature 0x01020304; the shared page units the player can use; notifying the client after changing stash gold
static uint64_t dm_unit_inv(uint64_t u) { uint64_t a0 = *(uint64_t *)(u + 0xa0), i = a0 && *(int32_t *)(a0 + 8) && *(uint64_t *)(u + 0x98) ? *(uint64_t *)(u + 0x98) : *(uint64_t *)(u + 0x90); return i && *(uint32_t *)i == 0x01020304 ? i : 0; }
static int dm_stash_pages(uint64_t G, uint64_t player, uint64_t pg[8], uint32_t id[8]) {   // 玩家能用的共享页单位，按链表次序 | Shared page units the player can use, in linked-list order
    uint64_t pi = dm_unit_inv(player); int n = 0;
    for (uint64_t e = pi ? *(uint64_t *)(pi + 0x68) : 0; e && n < 8; e = *(uint64_t *)(e + 0x10)) { if (!*(uint8_t *)(e + 9) || *(uint8_t *)(e + 0xa)) continue;
        uint32_t i = *(uint32_t *)e; uint64_t u = *(uint64_t *)(G + 0x2230 + 8 * (i & 0x7f)); while (u && *(uint32_t *)(u + 8) != i) u = *(uint64_t *)(u + 0x158);
        if (u && *(uint32_t *)u == 0 && u != player && dm_unit_inv(u)) { pg[n] = u; id[n++] = i; } }
    return n;
}
// 把某个仓库单位的金币新值告诉客户端：照原版存取金币处理函数（包 0x27，main+0x6d7fb0）改完属性后的那一段（0x6d8390–0x6d8484）——
// 连接对象 = [[玩家+0x10]+0xf0]，发送口 = 全局对象 main+0x2d7f3d8 的虚函数 +0x28(对象, 连接号 [连接], {包头, 包尾})；
// 先按它的条件补一个拍号包 2d [u32 连接+0x6c0]（连接 +0x6c4 记着上次发过的拍号），再发 20 [u32 属性号 15][u32 单位 id][u32 新值]。
// 只改服务端属性不发这个包，当局箱子界面上金币还是旧数（mini 实测显示 0），要等下次进局才对
// Tell the client the new gold value of a stash unit: follows the section (0x6d8390–0x6d8484) after the attribute change in the original game's deposit / withdraw gold handler (packet 0x27, main+0x6d7fb0) --
// connection object = [[player+0x10]+0xf0], send port = virtual function +0x28 of the global object main+0x2d7f3d8 (object, connection number [connection], {packet head, packet tail});
// first, under its condition, add a tick number packet 2d [u32 connection+0x6c0] (connection +0x6c4 records the tick number last sent), then send 20 [u32 stat number 15][u32 unit id][u32 new value].
// If only the server-side stat is changed without sending this packet, the gold in the stash UI still shows the old number for the current run (measured on the second Mac: shows 0) and is only right on the next run entry
static void dm_stash_gold_notify(uint8_t *b, uint64_t G, uint64_t player, uint32_t uid, int value) {
    uint64_t pd = *(uint64_t *)(player + 0x10), cn = pd ? *(uint64_t *)(pd + 0xf0) : 0; if (!cn) return;
    uint64_t obj = (uint64_t)(uintptr_t)(b + 0x2d7f3d8); void (*send)(uint64_t, uint32_t, void *) = (void *)(uintptr_t)*(uint64_t *)(*(uint64_t *)obj + 0x28); struct { uint8_t *a, *z; } rg;
    uint32_t seq = *(uint32_t *)(cn + 0x6c0); *(uint32_t *)(G + 0x19050) = 1;
    if (*(uint32_t *)(cn + 0x6c4) != seq) { uint64_t g2 = *(uint64_t *)(cn + 0x2e0); int busy = g2 && *(uint32_t *)(g2 + 0x19050) != 0;
        if (!(*(int32_t *)(cn + 0x99c) >= 1 && *(uint8_t *)(cn + 0x9a4) && !busy)) { uint8_t p2[5] = { 0x2d }; memcpy(p2 + 1, &seq, 4); *(uint32_t *)(cn + 0x6c4) = seq; rg.a = p2; rg.z = p2 + 5; send(obj, *(uint32_t *)cn, &rg); } }
    uint8_t pk[13] = { 0x20 }; uint32_t st = 15; memcpy(pk + 1, &st, 4); memcpy(pk + 5, &uid, 4); memcpy(pk + 9, &value, 4); rg.a = pk; rg.z = pk + 13; send(obj, *(uint32_t *)cn, &rg);
    *(uint32_t *)(G + 0x19050) = 0;
}
int dm_cloud_pack_each(const char *path, void (*cb)(const char *rel, const uint8_t *d, uint32_t n, void *ctx), void *ctx);
int dm_cloud_char_file(const char *root, const char *guest, char *out, size_t n); int dm_cloud_inbox_retire(const char *root, const char *name);   // dm_cloudsync.c

#if __has_include("stash_seed.h")   // 新设备共享仓库的预置（公开仓库不带：没有就当预置金币 0） | Preset shared stash for new devices (not in the public repository: if absent, preset gold is 0)
#include "stash_seed.h"
#define CM_HAVE_STASH_SEED 1
#endif
#define VB 1024            // 单件位流上限（含孔里的，同 dm_savebits.c VBYTES） | Max bitstream size per item (including socketed items, same as dm_savebits.c VBYTES)
#define PAGE_GOLD 2500000  // 每个共享页金币上限（界面「金币最大数」） | Gold cap per shared page (the UI "max gold")
typedef struct { uint16_t len; uint8_t *bits; } cm_item;
typedef struct { cm_item *v; int n, cap; } cm_list;
static void cl_add(cm_list *l, const uint8_t *b, int len) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, sizeof *l->v * (size_t)l->cap); }
    l->v[l->n].len = (uint16_t)len; l->v[l->n].bits = malloc((size_t)len); memcpy(l->v[l->n].bits, b, (size_t)len); l->n++; }
static void cl_free(cm_list *l) { for (int i = 0; i < l->n; i++) free(l->v[i].bits); free(l->v); memset(l, 0, sizeof *l); }
static void cl_del(cm_list *l, int i) { free(l->v[i].bits); l->v[i] = l->v[--l->n]; }

// 去掉位置字段后的比较键：物品位流（低位在前）前 32 位是标志，接着 3 位版本，再往后 3 位方式、4 位装备位、4 位 x、4 位 y、3 位页（第 35–52 位）。
// 同一件放在不同格 / 页只差这 18 位；另把标志里的「新物品」位（第 13 位，建物品时会清掉）也去掉
// Comparison key with position fields stripped: in the item bitstream (low bit first) the first 32 bits are flags, then 3 bits version, then 3 bits mode, 4 bits equip slot, 4 bits x, 4 bits y, 3 bits page (bits 35–52).
// The same item in a different cell / page differs only in these 18 bits; the "new item" bit in the flags (bit 13, cleared when the item is created) is also stripped
static void key_of(const uint8_t *b, int len, uint8_t *k) { memcpy(k, b, (size_t)len); if (len < 7) return;
    k[1] &= (uint8_t)~0x20; k[4] &= 0x07; k[5] = 0; k[6] &= (uint8_t)~0x1f; }
static int key_eq(const cm_item *a, const cm_item *b) { if (a->len != b->len) return 0; uint8_t x[VB], y[VB]; key_of(a->bits, a->len, x); key_of(b->bits, b->len, y); return !memcmp(x, y, a->len); }
static int cl_count(const cm_list *l, const cm_item *it) { int c = 0; for (int i = 0; i < l->n; i++) c += key_eq(&l->v[i], it); return c; }

// ---- 拆一份 .d2i：55 AA 55 AA 分页，+12 金币，+16 页长，+64 'JM' 是物品页（+66 件数，+68 起物品紧挨着）；其余段是编年史
// ---- Split a .d2i: paged by 55 AA 55 AA, +12 gold, +16 page length, +64 'JM' is an item page (+66 item count, items packed from +68); other segments are the Chronicle
typedef struct { long long gold; int chron, items, bad; } d2i_sum;
static uint8_t *B;
static d2i_sum d2i_split(uint64_t G, const uint8_t *d, uint32_t n, cm_list *out) {
    d2i_sum s = {0};
    for (uint32_t o = 0; o + 68 <= n && !memcmp(d + o, "\x55\xaa\x55\xaa", 4); ) { uint32_t ln, g; memcpy(&ln, d + o + 16, 4); if (ln < 68 || ln > n - o) { s.bad++; break; }
        memcpy(&g, d + o + 12, 4);
        if (!memcmp(d + o + 64, "JM", 2)) { uint16_t c; memcpy(&c, d + o + 66, 2); s.gold += g; uint32_t p = o + 68;
            for (int i = 0; i < c; i++) { int used = 0, socks = 0; uint64_t it = out && p < o + ln ? dm_item_make_n(B, G, 0, d + p, (int)(o + ln - p), &socks, &used) : 0;
                if (!it || used <= 0 || used > VB) { if (out) s.bad++; if (it) ((void (*)(uint64_t, uint64_t))(B + 0x658be0))(G, it); break; }
                uint8_t buf[VB]; int len = dm_item_write(B, it, buf); ((void (*)(uint64_t, uint64_t))(B + 0x658be0))(G, it);   // 建出来再按原版写一遍：键与云端那边（同一个写函数）口径一致 | After creating it, write it once more the original way: keys match the cloud side (same write function)
                if (len > 0) { cl_add(out, buf, len); s.items++; } else s.bad++;
                p += (uint32_t)used; } }
        else s.chron += (int)ln;
        o += ln; }
    return s;
}
// 云端（现在服务端里的）：共享页 + 高级仓库里在容器中的物品（方式 0），按原版写位流
// Cloud (what is now on the server): items inside containers (mode 0) in the shared pages + advanced stash, bitstreams written the original way
static long long live_items(uint64_t G, uint64_t player, cm_list *out) {
    uint64_t pg[8]; uint32_t id[8]; int n = dm_stash_pages(G, player, pg, id); long long gold = 0;
    uint64_t pi = dm_unit_inv(player); int m = n;
    for (uint64_t e = pi ? *(uint64_t *)(pi + 0x68) : 0, k = 0; e && k < 16 && m < 8; e = *(uint64_t *)(e + 0x10), k++) if (*(uint8_t *)(e + 0xa) == 1) {   // 高级仓库（类 1） | Advanced stash (class 1)
        uint32_t i = *(uint32_t *)e; uint64_t u = *(uint64_t *)(G + 0x2230 + 8 * (i & 0x7f)); while (u && *(uint32_t *)(u + 8) != i) u = *(uint64_t *)(u + 0x158); if (u) pg[m++] = u; }
    for (int k = 0; k < m; k++) { if (k < n) gold += ((int (*)(uint64_t, int, int))(B + 0x5b9290))(pg[k], 15, 0);
        uint64_t iv = dm_unit_inv(pg[k]); int c = 0;
        for (uint64_t it = iv ? *(uint64_t *)(iv + 0x10) : 0; it && c < 2048; c++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
            if (*(uint32_t *)(it + 0xc) == 0) { uint8_t buf[VB]; int len = dm_item_write(B, it, buf); if (len > 0) cl_add(out, buf, len); }
            it = *(uint64_t *)(d + 0xb0); } }
    return gold;
}

// ---- 清单（plan-<仓库名>.bin，小端）：'SWCM' u8 版本 1 u8 0 u16 件数；i64 待还金币；每件 u16 位流长 + 位流
// ---- List (plan-<stash name>.bin, little endian): 'SWCM' u8 version 1 u8 0 u16 item count; i64 gold to return; each item u16 bitstream length + bitstream
// Placed into the inventory and awaiting proof: index in the list (found by content), new item id, the bitstream after placing, the character file
// fingerprint at placement, the commit at which a different character file was first seen, and the character it was placed into (only that file proves it)
typedef struct { int idx; uint32_t nid; uint16_t plen; uint8_t pb[VB]; uint64_t h0, seen; char ch[64]; } cm_pend;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void plan_prune(const char *wd);
int dm_ser_state(void); uint64_t dm_fnv64(const uint8_t *d, size_t n);   // dm_savebits.c：在途存盘、整份指纹（同个人仓库翻页的「翻页后序列化」证明） | dm_savebits.c: save in flight, whole-file fingerprint (same proof of 'serialized after the change' as personal stash page turns)
static uint64_t char_h(const char *wd, const char *ch);
static char g_entry[600], g_stash[64];   // 当前在并的条目（相对存档根：_cloudinbox/<条目>）与仓库名；空 = 没有要并的 | Entry currently being merged (relative to the save root: _cloudinbox/<entry>) and stash name; empty = nothing to merge
static cm_list g_plan; static long long g_gold; static int g_have, g_dirty;   // g_have：清单已算好 / 读到；g_dirty：要随下次提交写盘 | g_have: list computed / read; g_dirty: must be written with the next commit
// At most PEND_MAX placed returns await proof; g_pend has room for twice that, so a commit that does not land can always put back the ones its hook
// crossed off (cm_post) even if more were placed in between
enum { PEND_MAX = 32 };
static cm_pend g_pend[2 * PEND_MAX]; static int g_npend; static uint64_t g_gold_cno; static struct timespec g_gold_at; static long long g_gold_sent;   // 加了金币的墙钟时刻（证明用）与加了多少 | Wall-clock time when gold was added (for proof) and how much was added
static char g_char[64];   // the player in the game now (returns are placed into their inventory)
static int g_hold;   // returns from an earlier player of another stash kind still await proof: this player's entry is picked once they are settled
static void plan_path_of(char *p, size_t n, const char *wd, const char *entry, const char *stash) { snprintf(p, n, "%s/%s/plan-%s.bin", wd, entry, stash); }
static void plan_path(char *p, size_t n, const char *wd, const char *entry) { plan_path_of(p, n, wd, entry, g_stash); }
static int plan_read(const char *path, cm_list *l, long long *gold) {   // 1 = 读到，0 = 没有，-1 = 坏 | 1 = read, 0 = none, -1 = corrupt
    FILE *f = fopen(path, "rb"); if (!f) return 0; uint8_t h[8]; int ok = fread(h, 1, 8, f) == 8 && !memcmp(h, "SWCM", 4) && h[4] == 1 && fread(gold, 8, 1, f) == 1;
    for (int i = 0, n = ok ? *(uint16_t *)(h + 6) : 0; ok && i < n; i++) { uint16_t len; uint8_t buf[VB]; ok = fread(&len, 2, 1, f) == 1 && len && len <= VB && fread(buf, 1, len, f) == len; if (ok) cl_add(l, buf, len); }
    fclose(f); if (!ok) cl_free(l); return ok ? 1 : -1;
}
static int plan_write(const char *path) {
    char t[1400]; snprintf(t, sizeof t, "%s.tmp", path); FILE *f = fopen(t, "wb"); if (!f) return -1;
    uint8_t h[8] = { 'S', 'W', 'C', 'M', 1, 0, 0, 0 }; *(uint16_t *)(h + 6) = (uint16_t)g_plan.n; int ok = fwrite(h, 1, 8, f) == 8 && fwrite(&g_gold, 8, 1, f) == 1;
    for (int i = 0; ok && i < g_plan.n; i++) ok = fwrite(&g_plan.v[i].len, 2, 1, f) == 1 && fwrite(g_plan.v[i].bits, 1, g_plan.v[i].len, f) == g_plan.v[i].len;
    ok = fclose(f) == 0 && ok; if (!ok || rename(t, path)) { remove(t); return -1; } return 0;
}

// ---- 进局：找第一个这一类仓库还没并完的条目（目录名是时刻-序号，按名字次序 = 先后次序）
// ---- On entering the game: find the first entry whose stash of this type is not fully merged yet (directory names are time-index, so name order = chronological order)
typedef struct { const char *want; const uint8_t *d; uint32_t n; uint8_t *copy; } pick_ctx;
static void pick_cb(const char *rel, const uint8_t *d, uint32_t n, void *ctx) { pick_ctx *c = ctx; char a[200], b2[200];
    snprintf(a, sizeof a, "%s.d2i", c->want); snprintf(b2, sizeof b2, "%s/%s.d2i", c->want, c->want);
    if (!c->copy && (!strcmp(rel, a) || !strcmp(rel, b2))) { c->copy = malloc(n ? n : 1); memcpy(c->copy, d, n); c->n = n; } }
static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
// ---- Finished entries (F-SAVE-11): every shared stash file in mine.pack either holds nothing (no item on any page, no gold: nothing to return) or has
// a plan with nothing left. entry_pick queues them; the pre-commit hook retires them (it holds the save lock, so no commit clones the folder meanwhile)
typedef struct { char nm[8][96]; int n, over; } fin_ctx;   // stash files with something in them: each must have a finished plan
static void fin_cb(const char *rel, const uint8_t *d, uint32_t n, void *ctx) {
    fin_ctx *c = ctx; size_t L = strlen(rel); if (L < 5 || strcmp(rel + L - 4, ".d2i")) return;
    const char *sl = strrchr(rel, '/'), *base = sl ? sl + 1 : rel; size_t bl = strlen(base) - 4; char b[96]; if (bl >= sizeof b) { c->over = 1; return; }
    memcpy(b, base, bl); b[bl] = 0; if (sl && ((size_t)(sl - rel) != bl || strncmp(rel, b, bl))) return;   // only <name>.d2i or <name>/<name>.d2i, as pick_cb
    int any = n < 68 || memcmp(d, "\x55\xaa\x55\xaa", 4);   // not a stash file we can read: never counted as empty
    for (uint32_t o = 0; !any && o + 68 <= n && !memcmp(d + o, "\x55\xaa\x55\xaa", 4); ) { uint32_t ln, g; uint16_t k; memcpy(&ln, d + o + 16, 4); if (ln < 68 || ln > n - o) { any = 1; break; }
        if (!memcmp(d + o + 64, "JM", 2)) { memcpy(&k, d + o + 66, 2); memcpy(&g, d + o + 12, 4); any = k || g; } o += ln; }
    if (any) { if (c->n < 8) strlcpy(c->nm[c->n++], b, sizeof c->nm[0]); else c->over = 1; } }
static int entry_finished(const char *wd, const char *ent) {
    char mp[1500]; snprintf(mp, sizeof mp, "%s/%s/mine.pack", wd, ent); fin_ctx c; memset(&c, 0, sizeof c);
    if (dm_cloud_pack_each(mp, fin_cb, &c) < 0 || c.over) return 0;
    for (int i = 0; i < c.n; i++) { char pp[1600]; plan_path_of(pp, sizeof pp, wd, ent, c.nm[i]); cm_list l = {0}; long long gold = 0; int r = plan_read(pp, &l, &gold), left = l.n || gold > 0; cl_free(&l);
        if (r != 1 || left) return 0; }
    return 1; }
static char g_drop[16][128]; static int g_ndrop;   // finished entries (folder names) the next pre-commit hook retires
static void drop_queue(const char *nm) { for (int i = 0; i < g_ndrop; i++) if (!strcmp(g_drop[i], nm)) return; if (g_ndrop < 16) strlcpy(g_drop[g_ndrop++], nm, sizeof g_drop[0]); }
// 1 = found (g_entry set, g_have indicates whether the list already exists), 0 = nothing to merge. Every entry is read, in name order (time-index =
// the order they were stored); before, only the first 64 readdir entries were, and APFS lists them in hash order, not by name
static int entry_pick(const char *wd) {
    char ib[1200]; snprintf(ib, sizeof ib, "%s/_cloudinbox", wd); DIR *dp = opendir(ib); if (!dp) return 0;
    char (*nm)[128] = NULL; int n = 0, cap = 0, found = 0; struct dirent *e;
    while ((e = readdir(dp))) { if (e->d_name[0] == '.' || strlen(e->d_name) >= 128) continue;
        if (n == cap) { int c2 = cap ? cap * 2 : 64; void *q = realloc(nm, (size_t)c2 * 128); if (!q) break; nm = q; cap = c2; }
        strlcpy(nm[n++], e->d_name, 128); }
    closedir(dp); if (n) qsort(nm, (size_t)n, 128, cmp_name);
    for (int i = 0; i < n && !found; i++) { char ent[300], pp[1500]; snprintf(ent, sizeof ent, "_cloudinbox/%s", nm[i]);
        if (entry_finished(wd, ent)) { drop_queue(nm[i]); continue; }
        plan_path(pp, sizeof pp, wd, ent);
        cm_list l = {0}; long long gold = 0; int r = plan_read(pp, &l, &gold);
        if (r < 0) { LOG("【红】iCloud 并回：清单 %s 坏了，跳过这个条目（文件不动）", pp); continue; }
        if (r == 1) { if (!l.n && gold <= 0) { cl_free(&l); continue; }   // 这一类并完了 | This type is fully merged
            strlcpy(g_entry, ent, sizeof g_entry); cl_free(&g_plan); g_plan = l; g_gold = gold; g_have = 1; plan_prune(wd); found = 1; continue; }
        char mp[1500]; snprintf(mp, sizeof mp, "%s/%s/mine.pack", wd, ent); pick_ctx c = { g_stash, 0, 0, 0 };
        if (dm_cloud_pack_each(mp, pick_cb, &c) < 0) { LOG("【红】iCloud 并回：%s 读不了，跳过", mp); continue; }
        if (!c.copy) continue;   // 这个条目里没有这一类仓库的共享仓库档 | This entry has no shared stash file of this type
        free(c.copy); strlcpy(g_entry, ent, sizeof g_entry); g_have = 0; found = 1; }
    if (g_ndrop) LOG("iCloud merge: %d finished inbox entr%s retired with the next commit", g_ndrop, g_ndrop == 1 ? "y is" : "ies are");
    free(nm); return found;
}
// ---- 开箱时算清单（服务端拍；要用游戏函数拆位流）
// ---- Compute the list on stash open (server tick; game functions are needed to split bitstreams)
static int plan_compute(uint64_t G, uint64_t player, const char *wd) {
    char mp[1500], bp[1500]; snprintf(mp, sizeof mp, "%s/%s/mine.pack", wd, g_entry); snprintf(bp, sizeof bp, "%s/%s/base.pack", wd, g_entry);
    pick_ctx cm = { g_stash, 0, 0, 0 }, cb = { g_stash, 0, 0, 0 }; dm_cloud_pack_each(mp, pick_cb, &cm); int hbp = dm_cloud_pack_each(bp, pick_cb, &cb) >= 0, hb = hbp && cb.copy;
    if (!cm.copy) { free(cb.copy); return -1; }
    cm_list mine = {0}, base = {0}, cloud = {0}; d2i_sum sm = cm.copy ? d2i_split(G, cm.copy, cm.n, &mine) : (d2i_sum){0}, sb = {0};
    if (hb) sb = d2i_split(G, cb.copy, cb.n, &base);
    long long cg = live_items(G, player, &cloud);
    // 云端的编年史段长：读工作目录里现在这一份共享仓库档（编年史不在服务端单位里）
    // Chronicle segment length in the cloud: reads the current shared stash file in the working directory (the Chronicle is not in the server units)
    d2i_sum sc = {0}; { char cp[1400], cp2[1400]; snprintf(cp, sizeof cp, "%s/%s/%s.d2i", wd, g_stash, g_stash); snprintf(cp2, sizeof cp2, "%s/%s.d2i", wd, g_stash);
        FILE *f = fopen(cp, "rb"); if (!f) f = fopen(cp2, "rb");
        if (f) { fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *m = L > 0 && L < (16 << 20) ? malloc((size_t)L) : NULL; if (m && fread(m, 1, (size_t)L, f) == (size_t)L) sc = d2i_split(G, m, (uint32_t)L, NULL); free(m); fclose(f); } }
    int use_base = hb && !sb.bad && sc.chron >= sb.chron;
    long long seed = 0;
#ifdef CM_HAVE_STASH_SEED
    { d2i_sum ss = d2i_split(G, g_stash_seed, sizeof g_stash_seed, NULL); seed = ss.gold; }
#endif
    cl_free(&g_plan);
    for (int i = 0; i < mine.n; i++) { cm_item *it = &mine.v[i]; int dup = 0; for (int j = 0; j < i; j++) dup |= key_eq(&mine.v[j], it); if (dup) continue;   // 同一串只算一次个数 | Count each identical bitstream only once
        int have = cl_count(&mine, it), ref = cl_count(use_base ? &base : &cloud, it);
        for (int k = 0; k < have - ref; k++) cl_add(&g_plan, it->bits, it->len); }
    long long g = sm.gold - (use_base ? sb.gold : seed); g_gold = g > 0 ? g : 0;
    LOG("iCloud 并回：%s 的 %s —— 本机 %d 件、金币 %lld；云端 %d 件、金币 %lld、编年史 %d 字节 → 要还 %d 件、金币 %lld", g_entry, g_stash, mine.n, sm.gold, cloud.n, cg, sc.chron, g_plan.n, g_gold);
    if (use_base) LOG("iCloud 并回：按基准（%d 件、金币 %lld）只还本机新增", base.n, sb.gold);
    else if (hb) LOG("iCloud 并回：基准不能用（云端编年史比基准短 / 基准坏）→ 按并集，金币比预置 %lld 多出的才还", seed);
    else LOG("iCloud 并回：没有基准 → 按并集，金币比预置 %lld 多出的才还", seed);
    if (sm.bad) LOG("【红】iCloud 并回：本机那份有 %d 处拆不开的物品（拆开的照还）", sm.bad);
    cl_free(&mine); cl_free(&base); cl_free(&cloud); free(cm.copy); free(cb.copy);
    g_have = 1; g_dirty = 1; plan_prune(wd); return 0;
}

// 测试开关 DM_TEST_KILL_AT=cm:<时刻>：在并回的关键时刻对自己发 SIGKILL（同 kill -9），核「已还」标记与角色档要么一起生效、要么都不生效。
//   1 = 第一件放进背包之后（还没存盘）；2 = 提交前钩子找到证明、清单还没写；3 = 清单已写进工作目录、这次提交还没换入；
//   4 = 加了金币之后已有存档提交、金币还没证明（物品那次提交已落盘，金币那次还没有）
// Test switch DM_TEST_KILL_AT=cm:<point>: sends SIGKILL to itself (same as kill -9) at a key point of the merge, checking that the "returned" marker and the character save take effect together or not at all.
//   1 = after the first item is put into the inventory (not yet saved); 2 = pre-commit hook found the proof, list not yet written; 3 = list written into the working directory, this commit not yet swapped in;
//   4 = after gold was added a save commit already happened, gold not yet proven (the item commit is on disk, the gold one is not)
static void cm_kill(int at) { static int k = -1; if (k < 0) { const char *e = getenv("DM_TEST_KILL_AT"); k = e && !strncmp(e, "cm:", 3) ? atoi(e + 3) : 0; }
    if (k == at) { LOG("DM_TEST_KILL_AT：iCloud 并回时刻 %d，对自己发 SIGKILL", at); kill(getpid(), SIGKILL); for (;;) pause(); } }   // 信号送达前不再往下跑 | Do not continue until the signal is delivered
// ---- 放一件进背包：1 = 放了，0 = 放不下 / 建不出
// ---- Put one item into the inventory: 1 = placed, 0 = no room / could not be created
static int take_one(uint64_t G, uint64_t player, int idx) {
    cm_item *u = &g_plan.v[idx]; uint64_t pp = *(uint64_t *)(player + 0x38), room = pp ? *(uint64_t *)(pp + 0x20) : 0; if (!room) return 0;
    int used = 0; uint64_t nu = dm_item_make_n(B, G, room, u->bits, u->len, NULL, &used);
    if (!nu) { static int said; if (!said++) LOG("【红】iCloud 并回：清单第 %d 件建不出物品，留在清单里不放", idx + 1); return -1; }
    uint64_t inv = *(uint64_t *)(player + 0x90); uint32_t cls = *(uint32_t *)(player + 4), fx = 0, fy = 0;
    if (!inv || cls > 7 || !((int (*)(uint64_t, uint64_t, int, uint32_t *, uint32_t *, int))(B + 0x55f620))(inv, nu, *(int32_t *)(B + 0x1dae684 + cls * 8 + 4), &fx, &fy, 0)) {
        ((void (*)(uint64_t, uint64_t))(B + 0x658be0))(G, nu); return 0; }
    uint64_t nd = *(uint64_t *)(nu + 0x10), ip = *(uint64_t *)(nu + 0x38); if (!nd || !ip) { ((void (*)(uint64_t, uint64_t))(B + 0x658be0))(G, nu); return 0; }
    *(uint8_t *)(nd + 0x55) = 0; *(int32_t *)(ip + 0x10) = (int32_t)fx; *(int32_t *)(ip + 0x14) = (int32_t)fy;
    uint64_t one = nu, ox = 0; ((int (*)(uint64_t, uint64_t, uint64_t *, uint64_t *))(B + 0x6f1940))(G, player, &one, &ox);
    if (!*(uint64_t *)(nd + 0xa0)) { ((void (*)(uint64_t, uint64_t))(B + 0x658be0))(G, nu); LOG("【红】iCloud 并回：清单第 %d 件放进背包没成（格 %u,%u）", idx + 1, fx, fy); return 0; }
    ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, uint64_t))(B + 0x642f70))(G, player, nu, 1, 2, 0, 0, 0, (uint64_t)(fx & 0xffff) | (uint64_t)fy << 16);
    cm_pend *p = &g_pend[g_npend++]; memset(p, 0, sizeof *p); p->idx = idx; p->nid = *(uint32_t *)(nu + 8); strlcpy(p->ch, g_char, sizeof p->ch);
    { const char *wd = dm_save_workdir(); p->h0 = wd && dm_ser_state() == 0 ? char_h(wd, p->ch) : 0; }
    int len = dm_item_write(B, nu, p->pb); if (len > 0) p->plen = (uint16_t)len; else { p->plen = u->len; memcpy(p->pb, u->bits, u->len); LOG("【红】iCloud 并回：放好后的位流写不出，用清单那串作证据"); }
    LOG("iCloud 并回：清单第 %d 件（%d 字节）放到背包格 %u,%u（新 id %u），等落盘证明", idx + 1, u->len, fx, fy, p->nid);
    cm_kill(1);
    { cm_item a = { p->plen, p->pb }; if (!key_eq(&a, u)) LOG("【红】iCloud 并回：清单第 %d 件放好后的那一串与清单去位置键不同（落盘证明照用放好后的那一串）", idx + 1); }
    return 1;
}
static int pend_has(int idx) { for (int i = 0; i < g_npend; i++) if (g_pend[i].idx == idx) return 1; return 0; }
static void add_gold(uint64_t G, uint64_t player) {   // 加到有空的共享页，放不下的留在清单里 | Added to shared pages with room; what does not fit stays in the list
    uint64_t pg[8]; uint32_t id[8]; int n = dm_stash_pages(G, player, pg, id); long long left = g_gold, put = 0;
    for (int k = 0; k < n && left > 0; k++) { int g = ((int (*)(uint64_t, int, int))(B + 0x5b9290))(pg[k], 15, 0), room = PAGE_GOLD - g; if (room <= 0) continue; int add = left < room ? (int)left : room;
        ((void (*)(uint64_t, int, int, int))(B + 0x5bc1f0))(pg[k], 15, g + add, 0); dm_stash_gold_notify(B, G, player, id[k], g + add); left -= add; put += add;
        LOG("iCloud 并回：共享页 %d（单位 id %u）金币 %d → %d", k + 1, id[k], g, g + add); }
    if (put) { g_gold_sent = put; clock_gettime(CLOCK_REALTIME, &g_gold_at); g_gold_cno = dm_fs_commit_count(); }
    if (left) LOG("iCloud 并回：共享页金币都满了，还有 %lld 金币留到下次", left);
}

// ---- 提交前钩子（写盘线程）
// ---- Pre-commit hook (disk write thread)
// 读整个文件：1 = 读全了（*d 归调用方 free）；0 = 没有 / 空 / 太大 / 读错，*d 已放掉、置 NULL。原先读错（fread 不足）时缓冲留给了调用方，
// 有的调用方只在返回 1 时才 free，漏掉；读了一半的也当读到了用
// Read a whole file: 1 = read completely (*d is the caller's to free); 0 = missing / empty / too large / read error, with *d already released and set to NULL. Before, on a read error (short fread) the buffer was left to the caller,
// and a caller that frees only on 1 leaked it; a half-read file also counted as read
static int file_blob(const char *p, uint8_t **d, size_t *n) { FILE *f = fopen(p, "rb"); *d = NULL; *n = 0; if (!f) return 0;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); *d = L > 0 && L < (16 << 20) ? malloc((size_t)L) : NULL; *n = *d ? fread(*d, 1, (size_t)L, f) : 0; fclose(f);
    if (!*d || *n != (size_t)L) { free(*d); *d = NULL; *n = 0; return 0; }
    return 1; }
int dm_save_blobs(const char *wd, const char *stash, dm_blob *out, int max); void dm_blobs_free(dm_blob *bl, int nb);
int dm_blobs_have(const dm_blob *bl, int nb, const uint8_t *bits, int len); int dm_bits_simple(const uint8_t *bits, int len);
// A character's file in the work folder, found under its guest name (dm_cloud_char_file: a ".dm-case-N" alias too; "<wd>/<name>/<name>.d2s" opened
// directly would reach another character's folder on a case-insensitive file system). 1 = read whole (*d is the caller's to free)
static int char_blob(const char *wd, const char *ch, uint8_t **d, size_t *n) { char p[1400]; *d = NULL; *n = 0; return ch[0] && dm_cloud_char_file(wd, ch, p, sizeof p) && file_blob(p, d, n); }
static uint64_t char_h(const char *wd, const char *ch) { uint8_t *d; size_t n; uint64_t h = char_blob(wd, ch, &d, &n) ? dm_fnv64(d, n) : 0; free(d); return h; }   // fingerprint of that character's whole file; 0 if it cannot be read
// Where a return placed into a character's inventory may have gone by the next save: that character's file, or the shared stash file (dragged there
// before the save; before that was checked, such a return was never crossed off and came back on every entry). Only those: a simple item (no unique
// id) identical to it in another character's file proves nothing about this one, yet used to cross it off (F-SAVE-15)
static void stash_blobs(const char *wd, uint8_t *d[2], size_t n[2]) { char a[1400]; snprintf(a, sizeof a, "%s/%s/%s.d2i", wd, g_stash, g_stash); file_blob(a, &d[0], &n[0]); snprintf(a, sizeof a, "%s/%s.d2i", wd, g_stash); file_blob(a, &d[1], &n[1]); }
// 进局核清单：有自己身份的件（简单物品字节相同认不出是哪一件，不判）已经在上面这些文件里 = 以前还过、证明没划掉，划掉不再还
// Check the list on entering the game: items with their own identity (simple items with identical bytes cannot be told apart, so they are not judged) already in any character file or the shared stash file
// were returned earlier but never crossed off; cross them off and do not return them again
static void plan_prune(const char *wd) {
    dm_blob bl[70]; int nb = dm_save_blobs(wd, g_stash, bl, 70), drop = 0;
    for (int i = 0; i < g_plan.n; ) { cm_item *it = &g_plan.v[i]; if (!dm_bits_simple(it->bits, it->len) && dm_blobs_have(bl, nb, it->bits, it->len)) { cl_del(&g_plan, i); drop++; } else i++; }
    dm_blobs_free(bl, nb);
    if (drop) { g_dirty = 1; LOG("iCloud 并回：清单里 %d 件已经在存档里（以前还过、没划掉）→ 划掉不再还，还剩 %d 件", drop, g_plan.n); } }
// What the pre-commit hook settled in the commit under way, kept until hle_fs.c reports whether that commit landed (cm_post; F-SAVE-2):
// returns crossed off (their pending record and their list item), pending records whose "first changed file" mark was set, gold crossed off,
// whether the list file was written, and the finished entries retired
typedef struct { cm_pend p; uint16_t len; uint8_t bits[VB]; } cm_undo;
static cm_undo g_undo[2 * PEND_MAX]; static int g_nundo; static uint32_t g_unseen[2 * PEND_MAX]; static int g_nunseen;
static long long g_ugold, g_ugold_sent; static struct timespec g_ugold_at; static uint64_t g_ugold_cno; static int g_stg, g_uwrote;
static char g_udrop[16][128]; static int g_nudrop;
static void cm_hook(const char *wd) {
    pthread_mutex_lock(&g_mu);
    g_stg = g_nundo = g_nunseen = g_uwrote = g_nudrop = 0; g_ugold = g_ugold_sent = 0;
    for (int i = 0; i < g_ndrop; i++) if (!dm_cloud_inbox_retire(wd, g_drop[i])) { strlcpy(g_udrop[g_nudrop++], g_drop[i], sizeof g_udrop[0]); g_stg = 1; }   // one that cannot be retired is found again at the next entry
    g_ndrop = 0;
    if (g_entry[0] && g_have && (g_npend || g_gold_sent || g_dirty)) {
        uint64_t cno = dm_fs_commit_count() + 1; char p[1400]; g_stg = 1;
        if (g_gold_sent && cno - 1 > g_gold_cno) cm_kill(4);   // 测试：加金币之后已有一次提交落盘、这一次还没证明金币 | Test: after gold was added, a commit has already been written to disk, and this one has not yet proven the gold
        if (g_npend) { uint8_t *sd[2] = { 0 }, *cd = NULL; size_t sn[2] = { 0 }, cn = 0; char cch[64] = ""; uint64_t h = 0; stash_blobs(wd, sd, sn);
            for (int i = 0; i < g_npend; ) { cm_pend *q = &g_pend[i];
                if (!cch[0] || strcmp(cch, q->ch)) { free(cd); strlcpy(cch, q->ch, sizeof cch); h = char_blob(wd, q->ch, &cd, &cn) ? dm_fnv64(cd, cn) : 0; }   // most returns go to the same character: read its file once
                int at = (cd && dm_memfind(cd, cn, q->pb, q->plen)) || (sd[0] && dm_memfind(sd[0], sn[0], q->pb, q->plen)) || (sd[1] && dm_memfind(sd[1], sn[1], q->pb, q->plen));
                // 证据二（事实）：某次提交的本角色档已不是放件时那份（放件时没有在途的存盘，之后的只能是放好之后序列化的）；再过一次提交
                // （同一次存盘的共享仓库档也已落盘），这次提交的存档已是放好之后的状态，这件无论挪到哪、卖掉还是扔了都已落盘，再还就是复制
                // Evidence two (a fact): some commit's character file is no longer the copy present at placement (no save was in flight at placement, so any later one was serialized after placing); after one more commit
                // (the shared stash file from the same save is on disk too), the committed save reflects the state after placing. Wherever the item went, sold or dropped, that is on disk; returning it again would duplicate it
                if (!at && !q->seen && q->h0 && h && h != q->h0) { q->seen = cno; g_unseen[g_nunseen++] = q->nid; }
                int fresh = !at && q->seen && cno > q->seen;
                if (!at && !fresh) { i++; continue; }
                cm_kill(2);
                int k = q->idx >= 0 && q->idx < g_plan.n ? q->idx : -1; cm_undo *u = &g_undo[g_nundo++]; u->p = *q; u->len = 0;
                if (k >= 0) { u->len = g_plan.v[k].len; memcpy(u->bits, g_plan.v[k].bits, u->len);
                    for (int j = 0; j < g_npend; j++) if (g_pend[j].idx == g_plan.n - 1) g_pend[j].idx = k; cl_del(&g_plan, k); }   // 清单删件是「拿最后一件补位」：补位那件的待证明下标跟着改 | Removing an item from the list "moves the last item into the gap": the pending-proof index of the moved item is updated accordingly
                LOG("iCloud 并回：提交前核对 #%llu —— 新 id %u %s → 从清单划掉，还剩 %d 件", (unsigned long long)cno, q->nid, at ? "已在存档里" : "放好之后游戏已序列化并落盘", g_plan.n);
                g_pend[i] = g_pend[--g_npend]; g_dirty = 1; }
            free(cd); free(sd[0]); free(sd[1]); }
        if (g_gold_sent) { char a[1400], b2[1400]; struct stat st; snprintf(a, sizeof a, "%s/%s/%s.d2i", wd, g_stash, g_stash); snprintf(b2, sizeof b2, "%s/%s.d2i", wd, g_stash);
            if ((!stat(a, &st) || !stat(b2, &st)) && (st.st_mtimespec.tv_sec > g_gold_at.tv_sec || (st.st_mtimespec.tv_sec == g_gold_at.tv_sec && st.st_mtimespec.tv_nsec > g_gold_at.tv_nsec))) {
                long long g0 = g_gold; g_gold -= g_gold_sent; if (g_gold < 0) g_gold = 0; g_ugold = g0 - g_gold; g_ugold_sent = g_gold_sent; g_ugold_at = g_gold_at; g_ugold_cno = g_gold_cno; g_gold_sent = 0; g_dirty = 1;
                LOG("iCloud 并回：提交前核对 #%llu —— 共享仓库档在加金币之后重写过 → 金币划掉，还剩 %lld", (unsigned long long)cno, g_gold); } }
        if (g_dirty) { plan_path(p, sizeof p, wd, g_entry);
            if (plan_write(p) == 0) { g_dirty = 0; g_uwrote = 1; cm_kill(3); LOG("iCloud 并回：清单随这次提交落盘（还剩 %d 件、金币 %lld）%s", g_plan.n, g_gold, !g_plan.n && g_gold <= 0 ? "—— 这一类仓库并完" : ""); }
            else { void dm_fs_save_poison(int e); dm_fs_save_poison(5); LOG("【红】iCloud 并回：清单写不进 %s → 这次存档提交拒绝，之后重试", p); } }
    }
    pthread_mutex_unlock(&g_mu);
}
// Commit outcome (hle_fs.c post_commit, same thread and save lock as the hook): ok = 1 settles what the hook staged. ok = 0 means the list it wrote is
// not on disk (the commit was refused and the work folder recloned from the last committed version, or the clone / swap failed): the returns it
// crossed off go back to "placed, awaiting proof" (still in the player's inventory, so not handed out again; proven again against the next commit),
// the gold likewise, and the list is marked dirty so the next commit writes it from memory. Before, the hook's "written" stood, the committed list kept
// what the player already held, and the next session handed it out again (F-SAVE-2)
static void cm_post(int ok) {
    pthread_mutex_lock(&g_mu);
    if (g_stg && !ok) { int back = 0;
        for (int i = 0; i < g_nundo && g_npend < 2 * PEND_MAX; i++) { cm_undo *u = &g_undo[i]; u->p.idx = -1; if (u->len) { cl_add(&g_plan, u->bits, u->len); u->p.idx = g_plan.n - 1; } g_pend[g_npend++] = u->p; back++; }
        for (int i = 0; i < g_nunseen; i++) for (int j = 0; j < g_npend; j++) if (g_pend[j].nid == g_unseen[i]) g_pend[j].seen = 0;   // that changed file was not committed: it proves nothing yet
        if (g_ugold_sent) { g_gold += g_ugold; if (!g_gold_sent) { g_gold_at = g_ugold_at; g_gold_cno = g_ugold_cno; } g_gold_sent += g_ugold_sent; }   // gold sent since keeps its later time: the proof must follow both
        for (int i = 0; i < g_nudrop; i++) drop_queue(g_udrop[i]);
        if (g_uwrote) g_dirty = 1;
        if (back || g_ugold_sent || g_uwrote) LOG("iCloud merge: the commit did not land: %d return(s) and %lld gold back to awaiting proof%s", back, g_ugold_sent, g_uwrote ? "; the list is written again with the next commit" : ""); }
    g_stg = g_nundo = g_nunseen = g_uwrote = g_nudrop = 0; g_ugold = g_ugold_sent = 0;
    pthread_mutex_unlock(&g_mu);
}
__attribute__((constructor)) static void cm_hook_install(void) { extern void (*g_dm_pre_commit_hook[5])(const char *); extern void (*g_dm_post_commit_hook[5])(int);
    g_dm_pre_commit_hook[4] = cm_hook; g_dm_post_commit_hook[3] = cm_post; }   // post slot 4 is dm_engine.c's (save events): one slot per module

// ---- 服务端拍（diag.c hud_tick；不在局内 player 为 0）
// ---- Server tick (diag.c hud_tick; player is 0 when not in game)
static char g_notice[256]; static _Atomic int g_notice_req;
// Entering and leaving keep returns awaiting proof (F-SAVE-1). Before, any player change (leaving, entering again, another character) dropped the
// placed returns and reread the list from disk, where they were still listed: whatever was placed but not yet proven was handed out a second time.
// Busy = the entry has returns placed or gold sent awaiting proof, a list not yet written, or a commit under way that crossed some off (all under g_mu)
static int cm_busy(void) { return g_entry[0] && g_have && (g_npend || g_gold_sent || g_dirty || g_stg); }
static void cm_reset(void) { g_entry[0] = 0; g_have = 0; g_npend = 0; g_gold_sent = 0; g_dirty = 0; g_hold = 0; cl_free(&g_plan); }
static void cm_leave(void) {   // left the game: the save made on leaving commits and proves what is pending, so keep it until then
    if (cm_busy()) LOG("iCloud merge: left the game with %d return(s) and %lld gold of %s awaiting proof: kept until a commit settles them", g_npend, g_gold_sent, g_entry);
    else cm_reset(); }
// A player entered (or another one): ch = their name, sn = their stash kind (NULL = none). 1 = there is something to merge while they play
static int cm_enter(const char *wd, const char *sn, const char *ch) {
    strlcpy(g_char, ch ? ch : "", sizeof g_char);
    if (cm_busy()) {
        if (sn && !strcmp(sn, g_stash)) { g_hold = 0; LOG("iCloud merge: entered with %d return(s) of %s still awaiting proof: the same list carries on, nothing handed out again", g_npend, g_entry); return 1; }
        g_hold = 1; LOG("iCloud merge: returns of %s (%s) still await proof; this player's stash (%s) is merged once they are settled", g_entry, g_stash, sn ? sn : "none"); return 0; }
    cm_reset(); strlcpy(g_stash, sn ? sn : "", sizeof g_stash);
    if (!sn || !entry_pick(wd)) return 0;
    LOG("iCloud 并回：进局 —— 收件箱条目 %s 有 %s 要并（清单%s）", g_entry, g_stash, g_have ? "已有" : "开箱时算"); return 1; }
void dm_cloudmerge_srv_tick(uint8_t *b, uint64_t G, uint64_t player) {
    static uint64_t lp; static int none; static char cur_sn[64], cur_ch[64];
    if (!player) { if (lp) { pthread_mutex_lock(&g_mu); cm_leave(); pthread_mutex_unlock(&g_mu); } lp = 0; return; }
    B = b; uint32_t tq = *(uint32_t *)(G + 0x170); const char *wd = dm_save_workdir(); if (!wd) return;
    if (player != lp) { lp = player; const char *sn = dm_stash_name(b); uint64_t pd = *(uint64_t *)(player + 0x10);
        strlcpy(cur_sn, sn ? sn : "", sizeof cur_sn); strlcpy(cur_ch, pd ? (const char *)pd : "", sizeof cur_ch);
        pthread_mutex_lock(&g_mu); none = !cm_enter(wd, sn, cur_ch); pthread_mutex_unlock(&g_mu); }
    else if (none && g_hold) { pthread_mutex_lock(&g_mu); if (g_hold && !cm_busy()) none = !cm_enter(wd, cur_sn[0] ? cur_sn : NULL, cur_ch); pthread_mutex_unlock(&g_mu); }   // the earlier player's returns are settled: pick this one's
    int open = (dm_ui_panel_state() & 1) != 0, near = ((int (*)(uint64_t, uint64_t, int, int))(b + 0x650d10))(G, player, 0x10b, 0x32) & 1;
    // 测试开关 DM_TEST_CM_KEYS=1：第一次开箱时把工作目录里现在这一份共享仓库档也按同一套拆开，与服务端共享页 + 高级仓库逐件对键（核拆位流与去位置的键）
    // Test switch DM_TEST_CM_KEYS=1: on the first stash open, also splits the current shared stash file in the working directory the same way and matches keys item by item against the server shared pages + advanced stash (checks bitstream splitting and the position-stripped keys)
    { static int t = -1; if (t < 0) t = getenv("DM_TEST_CM_KEYS") != NULL; const char *sn = dm_stash_name(b);
      if (t == 1 && open && near && sn) { t = 0; char cp[1400]; snprintf(cp, sizeof cp, "%s/%s/%s.d2i", wd, sn, sn); uint8_t *d; size_t n; cm_list disk = {0}, live = {0};
          if (!file_blob(cp, &d, &n)) { snprintf(cp, sizeof cp, "%s/%s.d2i", wd, sn); file_blob(cp, &d, &n); }
          d2i_sum s = d ? d2i_split(G, d, (uint32_t)n, &disk) : (d2i_sum){0}; long long lg = live_items(G, player, &live); int hit = 0, miss = 0, raw = 0;
          for (int i = 0; i < disk.n; i++) { int c1 = cl_count(&disk, &disk.v[i]), c2 = cl_count(&live, &disk.v[i]); if (c1 == c2) hit++; else miss++;
              for (int j = 0; j < live.n; j++) raw += live.v[j].len == disk.v[i].len && !memcmp(live.v[j].bits, disk.v[i].bits, disk.v[i].len); }
          LOG("测试 DM_TEST_CM_KEYS：%s 盘上 %d 件（拆不开 %d、金币 %lld、编年史 %d 字节），服务端 %d 件（金币 %lld）；去位置键个数一致 %d 件、不一致 %d 件；原串逐字节相同 %d 件",
              sn, disk.n, s.bad, s.gold, s.chron, live.n, lg, hit, miss, raw);
          free(d); cl_free(&disk); cl_free(&live); } }
    if (none || !open || !near || dm_ser_state() == 1) return;   // 有在途的存盘：等它写完再放（放件时记下的角色档指纹才是唯一的「放件前那份」），等过 50 拍就照放、不记指纹 | A save is in flight: wait for it to finish writing before placing (only then is the character file fingerprint taken at placement the unique pre-placement copy); after 50 ticks, place anyway without a fingerprint
    static uint32_t last; if (tq - last < 25) return; last = tq;
    pthread_mutex_lock(&g_mu); int saved = 0, full = 0;
    if (!g_have) plan_compute(G, player, wd), saved = 1;
    if (g_gold > 0 && !g_gold_sent) { add_gold(G, player); saved |= g_gold_sent != 0; }
    for (int i = 0, miss = 0; i < g_plan.n && g_npend < PEND_MAX && miss < 8; i++) { if (pend_has(i)) continue; int r = take_one(G, player, i); if (r > 0) saved = 1; else if (r == 0) { full = 1; miss++; } }   // 大件放不下时接着试小件，一拍最多试 8 件放不下的 | When a large item does not fit, keep trying smaller ones; at most 8 non-fitting items are tried per tick
    int left = g_plan.n - g_npend, done = !g_plan.n && g_gold <= 0 && !g_npend && !g_dirty && !g_stg, pend = g_npend || g_gold_sent || g_dirty;
    static int said; if (saved && !said) { said = 1; snprintf(g_notice, sizeof g_notice, "%s", dm_str(b, "dmCloudMergeBack")); atomic_store(&g_notice_req, 1); }
    if (full && left) { static uint32_t fl; if (!fl || tq - fl > 750) { fl = tq; snprintf(g_notice, sizeof g_notice, "%s", dm_str(b, "dmCloudMergeFull")); atomic_store(&g_notice_req, 1); } }
    if (done) { LOG("iCloud 并回：%s 的 %s 并完", g_entry, g_stash); none = !entry_pick(wd); said = 0; }
    pthread_mutex_unlock(&g_mu);
    { static uint32_t lastreq; if (saved || (pend && tq - lastreq >= 50)) { lastreq = tq; dm_request_save(); } }
    if (atomic_exchange(&g_notice_req, 0)) { dm_game_notice(b, g_notice); LOG("iCloud 并回：系统提示「%s」", g_notice); }
}
