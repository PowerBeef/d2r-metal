// iCloud inbox merge unit test (takes seconds): includes dm_cloudmerge.c directly (its state is static) and links dm_cloudsync.c and dm_savebits.c; the
// game-facing calls are stubbed. The game functions themselves (building items, placing them on the server tick) are not run: each case sets up the
// state a placement leaves behind (the list, a placed return with its bitstream and character, gold sent) and drives the pre-commit hook, the commit
// outcome hook and entering / leaving the game against files in a temp work folder. Expectations are written by hand from the semantics:
//   F-SAVE-2: a commit that does not land puts what its hook crossed off back to "awaiting proof" and the list is written again with the next one;
//   F-SAVE-15: a placed return is proven only by the character it was placed into (found under its guest name, a ".dm-case-N" alias included) or by
//              the shared stash, never by an identical simple item in another character's file;
//   F-SAVE-1: leaving and entering the game keeps returns awaiting proof (none handed out again); another stash kind waits until they are settled;
//   F-SAVE-11: the whole inbox is read (more than 64 entries) in name order, and finished entries are retired with the next commit into the ledger.
#include "../dm_cloudmerge.c"
#include <sys/xattr.h>

// ---- stubs for what the merge calls outside the files linked in
static char g_wd[700]; static uint64_t g_commits; static int g_poison;
const char *dm_save_workdir(void) { return g_wd; }
int dm_request_save(void) { return 0; }
uint64_t dm_fs_commit_count(void) { return g_commits; }
void dm_game_notice(uint8_t *main_base, const char *text) { (void)main_base; (void)text; }
int dm_ui_panel_state(void) { return 0; }
const char *dm_str(uint8_t *main_base, const char *key) { (void)main_base; return key; }
void dm_fs_save_poison(int e) { g_poison = e; }
void (*g_dm_pre_commit_hook[5])(const char *);
void (*g_dm_post_commit_hook[5])(int);
int64_t dm_user_clock(void) { return 0; }
uint64_t dm_seq_next(void) { return 0; }
uint64_t dm_fs_d2s_wseq(void) { return 0; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define STASH "ModernSharedStashSoftCoreV2"
#define E "_cloudinbox/100-0"   // the entry most cases use
static char R[512];
static void mkdirs(const char *p) { char d[1400]; strlcpy(d, p, sizeof d); for (char *s = d + strlen(R) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(d, 0755); *s = '/'; } }
static void wfile(const char *rel, const void *d, size_t n) { char p[1400]; snprintf(p, sizeof p, "%s/%s", g_wd, rel); mkdirs(p); FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }
static long rfile(const char *rel, uint8_t *o, size_t cap) { char p[1400]; snprintf(p, sizeof p, "%s/%s", g_wd, rel); FILE *f = fopen(p, "rb"); if (!f) return -1; long n = (long)fread(o, 1, cap, f); fclose(f); return n; }
// A save commit as hle_fs.c runs it: the pre-commit hook, then (if it lands) the commit count moves on, then the outcome hook. ok = 0: refused / failed
// Every hook slot, as hle_fs.c calls them (a module that took another module's slot would otherwise go unnoticed here)
static int commit(int ok) { g_poison = 0; for (int k = 0; k < 5; k++) if (g_dm_pre_commit_hook[k]) g_dm_pre_commit_hook[k](g_wd);
    int land = ok && !g_poison; if (land) g_commits++; for (int k = 0; k < 5; k++) if (g_dm_post_commit_hook[k]) g_dm_post_commit_hook[k](land); return land; }
// The list as it is on disk: items (-1 = no list file) and gold
static int disk_plan(const char *ent, long long *gold) { char p[1500]; plan_path_of(p, sizeof p, g_wd, ent, STASH); cm_list l = {0}; long long g = 0; int r = plan_read(p, &l, &g), n = r == 1 ? l.n : -1; cl_free(&l); if (gold) *gold = g; return n; }
static void st_reset(const char *wd) { snprintf(g_wd, sizeof g_wd, "%s/%s", R, wd); mkdir(g_wd, 0755); pthread_mutex_lock(&g_mu); cm_reset(); g_ndrop = 0; g_gold = 0; g_char[0] = 0; strlcpy(g_stash, STASH, sizeof g_stash); pthread_mutex_unlock(&g_mu); }
// An entry with a list of n items (bitstreams it[i], 24 bytes each) and gold, written to disk as the last commit left it
static void st_list(const char *ent, uint8_t it[][24], int n, long long gold) { strlcpy(g_entry, ent, sizeof g_entry); g_have = 1; cl_free(&g_plan); for (int i = 0; i < n; i++) cl_add(&g_plan, it[i], 24); g_gold = gold;
    char p[1500]; plan_path(p, sizeof p, g_wd, ent); mkdirs(p); plan_write(p); }
static void st_placed(int idx, uint32_t nid, const uint8_t *pb, const char *ch) { cm_pend *p = &g_pend[g_npend++]; memset(p, 0, sizeof *p); p->idx = idx; p->nid = nid; p->plen = 24; memcpy(p->pb, pb, 24); strlcpy(p->ch, ch, sizeof p->ch); }
static void item(uint8_t *b, uint8_t tag, int simple) { for (int i = 0; i < 24; i++) b[i] = (uint8_t)(0x40 + tag * 7 + i * 13); b[2] = simple ? (b[2] | 0x20) : (b[2] & ~0x20); }
static void charfile(const char *dir, const char *name, const uint8_t *with) {   // a character file <dir>/<name>.d2s, holding the bitstream `with` (or not)
    uint8_t b[200]; memset(b, 0x11, sizeof b); if (with) memcpy(b + 60, with, 24); char rel[300]; snprintf(rel, sizeof rel, "%s/%s.d2s", dir, name); wfile(rel, b, sizeof b); }
// A pack (dm_cloudsync.c pack_write format) holding one shared stash file: a page of `items` items with `gold`, then a 148-byte chronicle; salt makes contents differ
static void d2i_bytes(uint8_t *b, int items, uint32_t gold, uint8_t salt) { memset(b, 0, 216); uint32_t l1 = 68, l2 = 148; memcpy(b, "\x55\xaa\x55\xaa", 4); memcpy(b + 12, &gold, 4); memcpy(b + 16, &l1, 4); memcpy(b + 64, "JM", 2); b[66] = (uint8_t)items;
    memcpy(b + 68, "\x55\xaa\x55\xaa", 4); memcpy(b + 84, &l2, 4); b[68 + 64] = 0xc0; b[68 + 100] = salt; }
static void mine_pack(const char *ent, const char *stash, int items, uint32_t gold, uint8_t salt) {
    uint8_t d[216]; d2i_bytes(d, items, gold, salt); char rel[300], p[1500]; snprintf(rel, sizeof rel, "%s/%s.d2i", stash, stash); snprintf(p, sizeof p, "%s/%s/mine.pack", g_wd, ent); mkdirs(p);
    FILE *f = fopen(p, "wb"); uint64_t st = 1000; uint32_t fl = 0, n = 1, sz = sizeof d; uint16_t pl = (uint16_t)strlen(rel);
    fwrite("SWPK1\n\0\0", 8, 1, f); fwrite(&st, 8, 1, f); fwrite(&fl, 4, 1, f); fwrite(&n, 4, 1, f); fwrite(&pl, 2, 1, f); fwrite(rel, pl, 1, f); fwrite(&st, 8, 1, f); fwrite(&sz, 4, 1, f); fwrite(d, sz, 1, f); fclose(f); }
static int entries(void) { char d[1400]; snprintf(d, sizeof d, "%s/_cloudinbox", g_wd); DIR *dp = opendir(d); struct dirent *e; int n = 0; while (dp && (e = readdir(dp))) n += e->d_name[0] != '.'; if (dp) closedir(dp); return n; }
static int lines(const char *rel) { uint8_t b[8192]; long n = rfile(rel, b, sizeof b); int k = 0; for (long i = 0; i < n; i++) k += b[i] == '\n'; return n < 0 ? -1 : k; }

int main(void) {
    snprintf(R, sizeof R, "%s/t_cloudmerge.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(R, 0755);
    uint8_t it[4][24]; item(it[0], 1, 0); item(it[1], 2, 0); item(it[2], 3, 1); item(it[3], 4, 1);

    // (1) F-SAVE-2: a return placed into Alice and found in her file is crossed off by the hook, but the commit is refused (hle_fs.c reclones the work
    //     folder from the last committed version, so the list file it wrote is gone). It must go back to awaiting proof with the list marked dirty, and
    //     the next commit that lands must write the list without it. Before, the hook's "written" stood and the committed list kept it for good
    st_reset("w1"); st_list(E, it, 1, 0); st_placed(0, 501, it[0], "Alice"); charfile("Alice", "Alice", it[0]);
    uint8_t keep[4096]; long kn = rfile(E "/plan-" STASH ".bin", keep, sizeof keep);
    CHECK(!commit(0) && g_npend == 1 && g_plan.n == 1 && g_dirty == 1, "(1) refused commit: the return is awaiting proof again, list dirty (pending %d, list %d, dirty %d)", g_npend, g_plan.n, g_dirty);
    wfile(E "/plan-" STASH ".bin", keep, (size_t)kn);   // the reclone: the work folder's list is the committed one again
    CHECK(commit(1) && disk_plan(E, NULL) == 0 && g_npend == 0 && !g_dirty, "(1) next commit lands: the list on disk no longer holds it (%d)", disk_plan(E, NULL));
    // the same with gold sent: the stash file was rewritten after the gold went in, the commit fails, the gold is pending again
    st_reset("w1g"); st_list(E, it, 0, 7000); g_gold_sent = 7000; clock_gettime(CLOCK_REALTIME, &g_gold_at); g_gold_at.tv_sec -= 100;
    { uint8_t d[216]; d2i_bytes(d, 0, 7000, 0); wfile(STASH "/" STASH ".d2i", d, sizeof d); }
    kn = rfile(E "/plan-" STASH ".bin", keep, sizeof keep);
    CHECK(!commit(0) && g_gold_sent == 7000 && g_gold == 7000 && g_dirty == 1, "(1) refused commit: the gold is awaiting proof again (sent %lld, left %lld)", g_gold_sent, g_gold);
    wfile(E "/plan-" STASH ".bin", keep, (size_t)kn);
    { long long g = -1; CHECK(commit(1) && disk_plan(E, &g) == 0 && g == 0 && !g_gold_sent, "(1) next commit lands: no gold left in the list on disk (%lld)", g); }
    // a commit that lands settles at once (control)
    st_reset("w1c"); st_list(E, it, 2, 0); st_placed(1, 502, it[1], "Alice"); charfile("Alice", "Alice", it[1]);
    CHECK(commit(1) && g_npend == 0 && g_plan.n == 1 && disk_plan(E, NULL) == 1, "(1) control: a landed commit crosses it off for good");

    // (2) F-SAVE-15: a simple item (no unique id) placed into Alice. Bob's file holds identical bytes, Alice's does not, nor does the stash: not proven.
    //     Alice is stored as a ".dm-case-1" alias and another character "alice" holds the bytes: not proven either (her file is found by guest name)
    st_reset("w2"); st_list(E, it, 3, 0); st_placed(2, 601, it[2], "Alice");
    charfile(".dm-case-1", "Alice", NULL); { char p[1400]; snprintf(p, sizeof p, "%s/.dm-case-1", g_wd); setxattr(p, DM_GUEST_NAME_XATTR, "Alice", 5, 0, XATTR_NOFOLLOW); }
    charfile("Bob", "Bob", it[2]); charfile("alice", "alice", it[2]);
    CHECK(commit(1) && g_npend == 1 && g_plan.n == 3 && disk_plan(E, NULL) == 3, "(2) identical simple item in other characters' files: not proven (pending %d, list %d)", g_npend, g_plan.n);
    charfile(".dm-case-1", "Alice", it[2]);
    CHECK(commit(1) && g_npend == 0 && disk_plan(E, NULL) == 2, "(2) control: in Alice's own file -> proven, crossed off");
    st_placed(1, 602, it[1], "Alice"); { uint8_t d[216]; uint8_t big[400]; d2i_bytes(d, 1, 0, 0); memcpy(big, d, 216); memcpy(big + 216, it[1], 24); wfile(STASH "/" STASH ".d2i", big, 240); }
    CHECK(commit(1) && g_npend == 0 && disk_plan(E, NULL) == 1, "(2) control: moved into the shared stash before the save -> proven");

    // (3) F-SAVE-1: Alice has one return placed, not yet proven; she leaves the game and enters again. The list and the placed return stay: the
    //     placement loop skips it (pend_has), nothing is handed out again. Before, leaving dropped it and entering reread the list, where it still was
    st_reset("w3"); mine_pack(E, STASH, 2, 0, 1); st_list(E, it, 2, 0); st_placed(0, 701, it[0], "Alice"); charfile("Alice", "Alice", NULL);
    pthread_mutex_lock(&g_mu); cm_leave(); pthread_mutex_unlock(&g_mu);
    CHECK(g_npend == 1 && g_entry[0] && g_plan.n == 2, "(3) left the game: the placed return is kept (pending %d, list %d)", g_npend, g_plan.n);
    pthread_mutex_lock(&g_mu); int e3 = cm_enter(g_wd, STASH, "Alice"); pthread_mutex_unlock(&g_mu);
    CHECK(e3 == 1 && g_npend == 1 && pend_has(0) && !pend_has(1) && g_plan.n == 2, "(3) entered again: same list, the placed one is not handed out again (pending %d)", g_npend);
    // another stash kind (Bob, hardcore): waits; once the commit proves Alice's return, Bob's entry is picked
    mine_pack("_cloudinbox/200-0", "ModernSharedStashHardCoreV2", 3, 0, 2);
    pthread_mutex_lock(&g_mu); int e3b = cm_enter(g_wd, "ModernSharedStashHardCoreV2", "Bob"); pthread_mutex_unlock(&g_mu);
    CHECK(e3b == 0 && g_hold && g_npend == 1 && !strcmp(g_stash, STASH), "(3) another stash kind: Alice's returns stay pending, Bob waits");
    charfile("Alice", "Alice", it[0]);
    CHECK(commit(1) && g_npend == 0 && !cm_busy(), "(3) the exit save proves Alice's return");
    pthread_mutex_lock(&g_mu); int e3c = g_hold && !cm_busy() ? cm_enter(g_wd, "ModernSharedStashHardCoreV2", "Bob") : -1; pthread_mutex_unlock(&g_mu);
    CHECK(e3c == 1 && !g_hold && !strcmp(g_entry, "_cloudinbox/200-0"), "(3) then Bob's entry is picked (%s)", g_entry);
    // nothing pending: leaving forgets the entry as before
    pthread_mutex_lock(&g_mu); cm_leave(); pthread_mutex_unlock(&g_mu); CHECK(!g_entry[0], "(3) control: leaving with nothing pending forgets the entry");

    // (4) F-SAVE-11: 70 entries; 69 finished (half: nothing in the stash file; half: a list with nothing left), one not (sorts last). The one left is
    //     found whatever the readdir order, and the finished ones are retired with the following commits, their content digests in the ledger
    st_reset("w4");
    for (int i = 0; i < 70; i++) { char ent[64]; snprintf(ent, sizeof ent, "_cloudinbox/%d-0", 1000 + i);
        if (i == 69) { mine_pack(ent, STASH, 4, 0, (uint8_t)i); continue; }
        if (i % 2) mine_pack(ent, STASH, 0, 0, (uint8_t)i);
        else { mine_pack(ent, STASH, 2, 500, (uint8_t)i); strlcpy(g_entry, ent, sizeof g_entry); cl_free(&g_plan); g_gold = 0; char p[1500]; plan_path(p, sizeof p, g_wd, ent); plan_write(p); } }
    g_entry[0] = 0; int found = 0;
    for (int k = 0; k < 10; k++) { pthread_mutex_lock(&g_mu); found = entry_pick(g_wd); pthread_mutex_unlock(&g_mu); if (!g_ndrop) break; commit(1); }
    CHECK(found && !strcmp(g_entry, "_cloudinbox/1069-0") && !g_have, "(4) the unfinished entry is found among 70 (%s)", g_entry);
    CHECK(entries() == 1 && lines("_cloudinbox/.done") == 64, "(4) the 69 finished entries are retired (left %d), the ledger keeps the newest 64 (%d)", entries(), lines("_cloudinbox/.done"));
    // a retirement in a commit that does not land is queued again (the reclone brings the folder back)
    mine_pack("_cloudinbox/0999-0", STASH, 0, 0, 200); pthread_mutex_lock(&g_mu); entry_pick(g_wd); pthread_mutex_unlock(&g_mu);
    CHECK(g_ndrop == 1 && !commit(0) && g_ndrop == 1, "(4) retirement in a refused commit: queued again");
    CHECK(commit(1) && entries() == 1, "(4) and retired with the next one");

    printf("t_cloudmerge: %d 项，%d 失败\n", checks, fails);
    return fails != 0;
}
