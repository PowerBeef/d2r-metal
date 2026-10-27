// 物品位流与存档文件的工具（iCloud 并回 dm_cloudmerge.c 用；原先在个人仓库多页 dm_pages.c 里）：原版写位流 / 读位流建物品、
// 工作目录里原版存档整份读进内存与按位流找物品、这一类共享仓库档的名字，以及「有没有已序列化、还没写完的存盘」的记录。
// 用到的游戏地址：写位流 main+0x581b60（写入器上下文 = GOT 虚表 [main+0x211feb8] + 0x10）；读位流 0x5793a0 + 建物品 0x6564e0；
// 孔里的件：0x5ca8a0 改方式、0x64d080 镶回；共享仓库名：存档类型 main+0x2127c48、状态字 main+0x21278d5；序列化时刻 [G+0x5800]
// Item bitstream and save-file helpers (used by the iCloud merge, dm_cloudmerge.c; formerly in the multi-page personal stash dm_pages.c): the game's own bitstream write / read-and-build,
// reading the original saves in the working directory whole and finding an item in them by its bitstream, the name of this kind of shared stash save, and the record of "is a save serialized but not yet written".
// Game addresses used: bitstream write main+0x581b60 (writer context = GOT vtable [main+0x211feb8] + 0x10); bitstream read 0x5793a0 + item build 0x6564e0;
// socketed items: 0x5ca8a0 sets the mode, 0x64d080 sockets them back; shared stash name: save type main+0x2127c48, status word main+0x21278d5; serialization time [G+0x5800]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "dm.h"

#define VBYTES 1024   // 单件位流上限（含孔里的） | Upper limit of a single item's bitstream (including socketed ones)
int dm_item_write(uint8_t *b, uint64_t it, uint8_t *buf) {   // 原版写位流（模板 main+0x65a840）：写入器 {缓冲, 容量位, 字节, 余位, 错误}，上下文 = GOT 虚表 + 0x10 | The original game's bitstream write (template main+0x65a840): writer {buffer, capacity in bits, byte, remaining bits, error}, context = GOT vtable + 0x10
    struct { uint64_t buf, cap, n, bits; uint32_t err, pad; uint8_t rest[0x60]; } w; uint64_t ctx[2] = { *(uint64_t *)(b + 0x211feb8) + 0x10, 0 };
    memset(&w, 0, sizeof w); memset(buf, 0, VBYTES); w.buf = (uint64_t)buf; w.cap = VBYTES * 8;
    ((void (*)(uint64_t, void *, int, int, int, void *))(b + 0x581b60))(it, &w, 1, 1, 0, ctx);
    int len = w.err ? 0 : (int)w.n + (w.bits != 0); return len > 0 && len <= VBYTES ? len : 0;
}
// 读位流建物品（含孔里镶的，照原版复制物品 0x65a840：母件后面紧跟孔里的件数 = 信息块 +0x14），清「新物品」位。room = 落在哪个房间（0 = 不进房间，放进物品栏用）
// Read a bitstream and build the item (including socketed ones, following the original game's item copy 0x65a840: the parent item is immediately followed by the socketed items, count = info block +0x14), clearing the "new item" bit. room = which room it lands in (0 = not placed in a room, used when putting it into an item container)
uint64_t dm_item_make_n(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out, int *bytes_out);
uint64_t dm_item_make(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out) { return dm_item_make_n(b, G, room, buf, len, socks_out, NULL); }
// 同上，另给出这件（连同孔里的）在位流里占了多少字节（bytes_out）：从一页里连续排着的物品逐件拆开用（dm_cloudmerge.c）
// Same as above, and also reports how many bytes this item (with its socketed items) takes in the bitstream (bytes_out): used to split items packed consecutively in a page one by one (dm_cloudmerge.c)
uint64_t dm_item_make_n(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out, int *bytes_out) {
    uint8_t info[0x100]; int used = 0; memset(info, 0, sizeof info);
    ((void (*)(int, const void *, int64_t, int, void *, int))(b + 0x5793a0))(*(uint8_t *)(G + 0x106), buf, len, 1, info, 0x69);
    uint64_t nu = ((uint64_t (*)(uint64_t, uint64_t, const void *, int, int, void *, int *, int))(b + 0x6564e0))(G, room, buf, len, 1, info, &used, 0x69);
    if (!nu) return 0;
    { uint64_t nd = *(uint64_t *)(nu + 0x10); if (nd) *(uint32_t *)(nd + 0x18) &= ~0x2000u; }
    int socks = *(int32_t *)(info + 0x14), off = used, rem = len - used, okn = 0;
    for (int s = 0; s < socks && rem > 0; s++) { uint8_t ci[0x100]; int cu = 0; memset(ci, 0, sizeof ci);
        ((void (*)(int, const void *, int64_t, int, void *, int))(b + 0x5793a0))(*(uint8_t *)(G + 0x106), buf + off, rem, 1, ci, 0x69);
        uint64_t ch = ((uint64_t (*)(uint64_t, uint64_t, const void *, int, int, void *, int *, int))(b + 0x6564e0))(G, 0, buf + off, rem, 1, ci, &cu, 0x69);
        if (!ch || cu <= 0) { LOG("【红】建物品：孔里第 %d 件建不出来", s + 1); break; }
        ((void (*)(uint64_t, int))(b + 0x5ca8a0))(ch, 4);
        if (!((int (*)(uint64_t, uint64_t, uint32_t, uint32_t, int))(b + 0x64d080))(G, nu, *(uint32_t *)(ch + 8), *(uint32_t *)(nu + 8), 2)) LOG("【红】建物品：孔里第 %d 件镶不回去", s + 1); else okn++;
        { uint64_t cd = *(uint64_t *)(ch + 0x10); if (cd) *(uint32_t *)(cd + 0x18) &= ~0x2000u; }
        off += cu; rem -= cu; }
    if (socks_out) *socks_out = okn; if (bytes_out) *bytes_out = off; return nu;
}
void *dm_memfind(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn) { if (!nn || hn < nn) return NULL; for (size_t i = 0; i + nn <= hn; i++) if (h[i] == n[0] && !memcmp(h + i, n, nn)) return (void *)(h + i); return NULL; }
// 工作目录里的原版存档：各角色档 <角色>/<角色>.d2s 与这一类共享仓库档（<名>/<名>.d2i 或 <名>.d2i），整份读进内存，供提交前核对「这件物品在不在原版存档里」
// （编年史仓库 dm_vault.c、个人仓库多页共用）。stash 为空就只读角色档
// The original saves in the working directory: each character save <character>/<character>.d2s and the shared stash saves of this kind (<name>/<name>.d2i or <name>.d2i), read whole into memory for the pre-commit check "is this item in the original save"
// (shared by the chronicle vault dm_vault.c and the multi-page personal stash). If stash is empty, only character saves are read
int dm_save_blobs(const char *wd, const char *stash, dm_blob *out, int max) {
    int k = 0; DIR *dd = opendir(wd); struct dirent *e; char p[1400];
    #define BLOB(path) do { FILE *f_; if (k < max && (f_ = fopen(path, "rb"))) { fseek(f_, 0, SEEK_END); long L_ = ftell(f_); fseek(f_, 0, SEEK_SET); \
        uint8_t *m_ = L_ > 0 && L_ < (16 << 20) ? malloc((size_t)L_) : NULL; size_t n_ = m_ ? fread(m_, 1, (size_t)L_, f_) : 0; fclose(f_); if (m_ && n_) { out[k].d = m_; out[k].n = n_; k++; } else free(m_); } } while (0)
    while (dd && (e = readdir(dd))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s/%s.d2s", wd, e->d_name, e->d_name); BLOB(p); }
    if (dd) closedir(dd);
    if (stash && stash[0]) { snprintf(p, sizeof p, "%s/%s/%s.d2i", wd, stash, stash); BLOB(p); snprintf(p, sizeof p, "%s/%s.d2i", wd, stash); BLOB(p); }
    #undef BLOB
    return k;
}
// 这件（位流）在不在这些存档里：比位流第 8 字节起最多 48 字节（跳过标志位与位置字段，物品挪了地方也认得）。只对有自己身份的物品可靠：
// 简单物品（位流标志位 0x200000，如同种符文、药水）没有唯一编号，两颗同种符文字节一样——调用方别拿它判「同一件」
// Whether this item (bitstream) is in these saves: compare up to 48 bytes of the bitstream starting from byte 8 (skipping the flag bits and position fields, so an item is still recognized after being moved). Reliable only for items with their own identity:
// simple items (bitstream flag bit 0x200000, e.g. runes or potions of the same kind) have no unique id, and two runes of the same kind have identical bytes -- callers must not use it to decide "the same item"
int dm_blobs_have(const dm_blob *bl, int nb, const uint8_t *bits, int len) { int n = len - 8 > 48 ? 48 : len - 8; if (n <= 8) return 0;
    for (int i = 0; i < nb; i++) if (dm_memfind(bl[i].d, bl[i].n, bits + 8, (size_t)n)) return 1; return 0; }
void dm_blobs_free(dm_blob *bl, int nb) { for (int i = 0; i < nb; i++) free(bl[i].d); }
int dm_bits_simple(const uint8_t *bits, int len) { return len >= 4 && (bits[2] & 0x20); }   // 位流标志位 0x200000 = 简单物品（没有唯一编号） | Bitstream flag bit 0x200000 = simple item (no unique id)
// 这一类共享仓库档的名字（同游戏自己选名）：DLC 类型（3）= ModernSharedStash*、毁灭之王类型（2）= SharedStash*；状态位 2 = 专家。经典类型没有共享仓库，返回 NULL
// The name of this kind of shared stash save (the same choice the game makes): DLC type (3) = ModernSharedStash*, Lord of Destruction type (2) = SharedStash*; status bit 2 = hardcore. The classic type has no shared stash: NULL
const char *dm_stash_name(uint8_t *b) {
    uint8_t mode = *(uint8_t *)(b + 0x2127c48); uint16_t st; memcpy(&st, b + 0x21278d5, 2); int hc = (st >> 2) & 1;   // 状态字在奇数地址：memcpy 读 | the status word sits at an odd address: read with memcpy
    if (mode == 3) return hc ? "ModernSharedStashHardCoreV2" : "ModernSharedStashSoftCoreV2";
    if (mode == 2) return hc ? "SharedStashHardCoreV2" : "SharedStashSoftCoreV2";
    return NULL;
}
// g_ser_infl：服务端拍看到序列化（[G+0x5800] 变成真时刻；与翻页同在服务端拍上，先后确定），且这一拍窗口里角色档还没写完 → 在途；之后看到角色档写完（hle_fs 的序号更大）→ 不在途。
//   假「在途」只多等，安全；假「不在途」要「上一次存盘的角色档恰好在这一拍才写完、这一次的还没写」——存盘间隔秒级、写盘毫秒级，不会同拍。
// g_ser_sec：最近一次看到序列化那一拍的用户时钟秒；同一秒里翻页前刚存过就顺延到下一秒（只防「翻页后同秒再存、内容逐字节相同」，用相等判、改时钟不会长卡）
// g_ser_infl: the server tick sees a serialization ([G+0x5800] became a real time; on the same server tick as page turns, so the order is fixed) and the character file has not finished writing within this tick's window -> in flight; once a finished write is seen (a larger hle_fs number) -> not in flight.
//   A false 'in flight' only waits longer, which is safe; a false 'not in flight' would need the previous save's character file to finish exactly in this tick while this one is not yet written - saves are seconds apart and writes take milliseconds, so not in the same tick.
// g_ser_sec: user clock second of the tick where the last serialization was seen; if a save happened earlier in the same second, defer the turn to the next second (only guards against a byte-identical save later in the same second; compared for equality, so clock changes cannot stall it for long)
static int g_ser_infl; static uint64_t g_ser_seq; static int64_t g_ser_sec = -1; static uint32_t g_ser_tq, g_tq_now;
int64_t dm_user_clock(void); int dm_user_clock_fixed(void); uint64_t dm_seq_next(void); uint64_t dm_fs_d2s_wseq(void);   // hle_time.c / hle_fs.c
// 给 iCloud 并回：0 = 没有在途的存盘（放件时可记指纹）；1 = 在途、50 拍内（等）；2 = 在途超过 50 拍还没见写完（不等了，这次拿不到「放好后序列化」的事实，只认在存档里找到）
// For the iCloud merge: 0 = no save in flight (a fingerprint can be taken at placement); 1 = in flight, within 50 ticks (wait); 2 = in flight for more than 50 ticks with no finished write seen (stop waiting; the 'serialized after placing' fact is not available this time, only 'found in the saves' counts)
int dm_ser_state(void) { return !g_ser_infl ? 0 : g_tq_now - g_ser_tq < 50 ? 1 : 2; }
uint64_t dm_fnv64(const uint8_t *d, size_t n);
uint64_t dm_fnv64(const uint8_t *d, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; } return h ? h : 1; }
// 服务端拍（diag.c hud_tick，在各存档模块之前）调：[G+0x5800] 变成真时刻 = 游戏序列化了（请存盘时我们拨成 1）；从 0 变过来是进局开始计时，不是序列化
// Called from the server tick (hud_tick in diag.c, before the save modules): [G+0x5800] becoming a real time = the game serialized (we set it to 1 when requesting a save); a change from 0 is the run starting its clock, not a serialization
int64_t dm_ser_last_sec(void) { return g_ser_sec; }
void dm_ser_track(uint64_t G, uint64_t player) {
    if (!player) return;
    uint32_t tq = *(uint32_t *)(G + 0x170); static uint64_t pv, tick_seq; uint64_t v = *(uint64_t *)(G + 0x5800), w = dm_fs_d2s_wseq();
    if (v != pv) { if (v > 1 && pv) { g_ser_sec = dm_user_clock(); g_ser_infl = !(w > tick_seq); g_ser_seq = dm_seq_next(); g_ser_tq = tq; } pv = v; }
    if (g_ser_infl && w > g_ser_seq) g_ser_infl = 0;   // 看到序列化之后角色档写完了 | The character file finished writing after the serialization was seen
    tick_seq = dm_seq_next(); g_tq_now = tq;
}
