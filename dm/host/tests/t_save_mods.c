// t_save_hooks 的模块桥：把真的 dm_vault.c / dm_pouch.c / dm_pages.c 各包含进一个编译单元（-DMOD_VAULT / -DMOD_POUCH / -DMOD_PAGES，静态量互不冲突），
// 给驱动 t_save_hooks.c 几个入口：照服务端拍那几行的记账做存入 / 取出 / 收进囊 / 放到背包 / 翻页（不调游戏函数），并读出模块的内部状态。提交前 / 提交结果钩子是真的
// Module bridge for t_save_hooks: the real dm_vault.c / dm_pouch.c / dm_pages.c are each included in their own compilation unit (-DMOD_VAULT / -DMOD_POUCH / -DMOD_PAGES, so their statics do not clash),
// giving the driver t_save_hooks.c a few entry points that do the bookkeeping of the server-tick lines for store / withdraw / stow / release / page turn (without calling game functions) and read back the module's internal state. The pre-commit / commit outcome hooks are the real ones
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef MOD_VAULT
#include "../dm_vault.c"
void tv_setup(const char *name) { pthread_mutex_lock(&g_vmu); strlcpy(g_vname, name, sizeof g_vname); g_loaded = 1; pthread_mutex_unlock(&g_vmu); }
void tv_load(void) { pthread_mutex_lock(&g_vmu); g_loaded = 1; vault_load(g_vname); pthread_mutex_unlock(&g_vmu); }   // 进局读侧文件（dm_vault_tick 那一行） | read the side file on entering a run (that line of dm_vault_tick)
void tv_deposit(int kind, int idx, const uint8_t *bits, int len) {   // deposit()：记一条存入、内存仓库里有了 | deposit(): record a store, the in-memory vault has it
    pthread_mutex_lock(&g_vmu); uint8_t *keep = malloc((size_t)len); memcpy(keep, bits, (size_t)len); pend_add(1, kind, idx, keep, len);
    int old = vfind(kind, idx); if (old >= 0) { free(g_v[old].data); g_v[old].data = keep; g_v[old].len = (uint16_t)len; } else g_v[g_nv++] = (ventry){ (uint8_t)kind, (uint16_t)idx, (uint16_t)len, keep }; pthread_mutex_unlock(&g_vmu); }
void tv_withdraw(int kind, int idx) {   // withdraw()：记一条取出、内存仓库里没了 | withdraw(): record a withdraw, the in-memory vault loses it
    pthread_mutex_lock(&g_vmu); int k = vfind(kind, idx); if (k >= 0) { pend_add(0, kind, idx, g_v[k].data, g_v[k].len); free(g_v[k].data); g_v[k] = g_v[--g_nv]; } pthread_mutex_unlock(&g_vmu); }
void tv_age_all(double secs) { pthread_mutex_lock(&g_vmu); for (int i = 0; i < g_npend; i++) g_pend[i].t -= (uint64_t)secs; pthread_mutex_unlock(&g_vmu); }
void tv_state(int *npend, int *ddirty, int *nv, int *nd) { pthread_mutex_lock(&g_vmu); *npend = g_npend; *ddirty = g_ddirty; *nv = g_nv; *nd = g_nd; pthread_mutex_unlock(&g_vmu); }
int tv_mem_has(int kind, int idx) { pthread_mutex_lock(&g_vmu); int r = vfind(kind, idx) >= 0; pthread_mutex_unlock(&g_vmu); return r; }
int tv_enter(uint64_t G, uint32_t tq, const char *who) { return vault_enter(G, tq, who, g_vname[0] ? g_vname : "ModernSharedStashSoftCoreV2"); }   // 服务端拍进局那一行（共享仓库名照游戏给的） | the run-entry line of the server tick (shared stash name as the game gives it)
void tv_tick0(void) { dm_vault_tick(0, 0, NULL); }   // 没有受控玩家的一拍（正在死 / 不在局内） | a tick without a controlled player (dying / not in a run)
void tv_resave(uint32_t tq) { vault_resave(tq); }
int tv_loaded(void) { pthread_mutex_lock(&g_vmu); int r = g_loaded; pthread_mutex_unlock(&g_vmu); return r; }
#endif

#ifdef MOD_POUCH
#include "../dm_pouch.c"
void tp_setup(const char *ch) { pthread_mutex_lock(&g_umu); strlcpy(g_uchar, ch, sizeof g_uchar); pouch_load(); g_uload = 1; pthread_mutex_unlock(&g_umu); }
void tp_stow(uint32_t code, const uint8_t *bits, int len) {   // pouch_put() 的记账：囊里多一件、记一条「收」（证据 = 背包里那一串，要「没有」） | pouch_put() bookkeeping: one more in the pouch, one "stow" entry (evidence = that inventory bitstream, must be absent)
    pthread_mutex_lock(&g_umu); uitem u = { code, (uint16_t)len, { 0 } }; memcpy(u.bits, bits, (size_t)len);
    g_u[g_nu++] = u; upend *p = &g_up[g_nup++]; memset(p, 0, sizeof *p); p->put = 1; p->it = u; p->plen = (uint16_t)len; memcpy(p->pb, bits, (size_t)len); p->h0 = pend_fp(); pouch_pub();
    pthread_mutex_unlock(&g_umu); }
void tp_release(const uint8_t *placed, int plen, uint32_t nid) {   // pouch_take() 的记账：囊里最后一件放到背包，证据 = 放好后那一串，要「有」 | pouch_take() bookkeeping: the last pouch item goes to the inventory, evidence = the bitstream after placing, must be present
    pthread_mutex_lock(&g_umu); uitem u = g_u[g_nu - 1]; upend *p = &g_up[g_nup++]; memset(p, 0, sizeof *p); p->put = 0; p->it = u; p->nid = nid;
    p->plen = (uint16_t)plen; memcpy(p->pb, placed, (size_t)plen); p->h0 = pend_fp(); g_nu--; pouch_pub(); pthread_mutex_unlock(&g_umu); }
int tp_take_ready(uint32_t *ids, int max) {   // 服务端拍取「已证明、等发 0x63」的新物品 id | the server tick takes the new item ids "proven, waiting for 0x63"
    pthread_mutex_lock(&g_umu); int n = g_nready < max ? g_nready : max; memcpy(ids, g_ready, sizeof(uint32_t) * (size_t)n); g_nready = 0; pthread_mutex_unlock(&g_umu); return n; }
void tp_state(int *nu, int *nud, int *nup, int *udirty) { pthread_mutex_lock(&g_umu); *nu = g_nu; *nud = g_nud; *nup = g_nup; *udirty = g_udirty; pthread_mutex_unlock(&g_umu); }
int tp_enter(uint64_t G, uint32_t tq, const char *name) { pthread_mutex_lock(&g_umu); int r = pouch_enter(G, tq, name); pthread_mutex_unlock(&g_umu); return r; }   // 服务端拍进局那一行 | the run-entry line of the server tick
void tp_tick0(void) { dm_pouch_srv_tick(NULL, 0, 0); }   // 没有受控玩家的一拍 | a tick without a controlled player
int tp_hook_on(void) { pthread_mutex_lock(&g_umu); int r = g_uload; pthread_mutex_unlock(&g_umu); return r; }
#endif

#ifdef MOD_PAGES
#include "../dm_savebits.c"   // 位流 / 存档工具与在途存盘记录（原先在 dm_pages.c 里）：同一个编译单元，tg_set_inflight 才拨得到 | bitstream / save helpers and the in-flight save record (formerly in dm_pages.c): same unit, so tg_set_inflight can set it
#include "../dm_pages.c"
static void pg_set(ppage *p, const uint8_t *bits, int len, int x, int y) { ppage_free(p); if (!bits) return; pitem t = { (uint8_t)x, (uint8_t)y, (uint16_t)len, (uint8_t *)bits }; pitem_dup(p, &t); }
void tg_setup(const char *ch) { pthread_mutex_lock(&g_pmu); strlcpy(g_pchar, ch, sizeof g_pchar); g_pload = 1; pthread_mutex_unlock(&g_pmu); }
// 翻页前的盘上状态：页槽 [to] 放 B、[from] 空，当前页 = from；标脏让下一次提交写下去 | the on-disk state before the turn: page slot [to] holds B, [from] is empty, current page = from; marked dirty so the next commit writes it
void tg_preflip(int from, int to, const uint8_t *B, int bl) { pthread_mutex_lock(&g_pmu); for (int i = 0; i < PGN; i++) ppage_free(&g_pp[i]); pg_set(&g_pp[to], B, bl, 3, 3); g_pcur = from; g_pdirty = 1; g_ppend = 0; pthread_mutex_unlock(&g_pmu); }
// pages_flip() 的记账：格里的 A 进页槽 [from]、页槽 [to] 的 B 进格；g_pH0 = 工作目录里翻页前那份角色档的指纹 | pages_flip() bookkeeping: grid A goes into slot [from], slot [to]'s B goes into the grid; g_pH0 = fingerprint of the pre-turn character file in the work directory
void tg_flip(int from, int to, const uint8_t *A, int al, const uint8_t *B, int bl) {
    pthread_mutex_lock(&g_pmu); pend_clear(); pg_set(&g_pA, A, al, 2, 2); pg_set(&g_pB, B, bl, 3, 3);
    ppage_free(&g_pp[to]); pg_set(&g_pp[from], A, al, 2, 2); g_pfrom = from; g_pto = to;
    { const char *wd = dm_save_workdir(); size_t dn = 0; uint8_t *d = wd ? d2s_read(wd, &dn) : NULL; g_pH0 = d ? dm_fnv64(d, dn) : 0; free(d); }
    g_pcur = to; g_ppend = 1; pthread_mutex_unlock(&g_pmu); }
void tg_state(int *ppend, int *pdirty, int *pmore, int *pcur) { pthread_mutex_lock(&g_pmu); *ppend = g_ppend; *pdirty = g_pdirty; *pmore = g_pmore; *pcur = g_pcur; pthread_mutex_unlock(&g_pmu); }
// 服务端拍进局那一行（侧文件里不能有待定记录：落定要读游戏的物品栏，这里没有游戏） | the run-entry line of the server tick (the side file must not hold a pending record: settling it reads the game's inventory, and there is no game here)
int tg_enter(uint64_t G, uint32_t tq, const char *name) { pthread_mutex_lock(&g_pmu); int r = pages_enter(G, tq, name, "ModernSharedStashSoftCoreV2", NULL, 0); pthread_mutex_unlock(&g_pmu); return r; }
void tg_tick0(void) { dm_pages_tick(0, 0, NULL); }
int tg_hook_on(void) { pthread_mutex_lock(&g_pmu); int r = g_pload; pthread_mutex_unlock(&g_pmu); return r; }
void tg_set_inflight(int on) { g_ser_infl = on; g_ser_tq = 0; g_tq_now = 1; }   // 造「有在途的存盘」（dm_ser_state() = 1） | stage "a save is in flight" (dm_ser_state() = 1)
void tg_dirty(void) { pthread_mutex_lock(&g_pmu); g_pdirty = 1; pthread_mutex_unlock(&g_pmu); }
// 读一份 .swpage：返回这串位流在不在某个页槽里（slot_out = 第几个槽，0 起；-1 = 不在），cur_out = 头里的当前页、pend_out = 头里的待定标志 | read a .swpage: returns whether this bitstream is in some page slot (slot_out = which slot, 0-based; -1 = none), cur_out = current page in the header, pend_out = pending flag in the header
int tg_file_has(const char *path, const uint8_t *bits, int len, int *slot_out, int *cur_out, int *pend_out) {
    FILE *f = fopen(path, "rb"); if (!f) return -1; uint8_t h[8]; if (fread(h, 1, 8, f) != 8) { fclose(f); return -2; } *cur_out = h[5]; *pend_out = h[6]; int hit = 0; *slot_out = -1;
    for (int pg = 0; pg < PGN; pg++) { ppage p = { 0, NULL }; if (ppage_read(f, &p)) { ppage_free(&p); break; } for (int i = 0; i < p.n; i++) if (p.it[i].len == len && !memcmp(p.it[i].bits, bits, (size_t)len)) { hit = 1; *slot_out = pg; } ppage_free(&p); }
    fclose(f); return hit; }
#endif
