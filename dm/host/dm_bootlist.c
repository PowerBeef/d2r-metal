// The boot list this machine learns: which blocks of the game's data archives the game decodes before its main menu.
// The next launch reads and decodes them ahead of the game on background threads (the boot prefetch in hle_tact.c, which also feeds the
// animation pre-decode in hle_granny.c). The list is the player's own: it follows their characters and their game files, and never leaves
// the machine. Game addresses: none (it watches the host's own file reads and the block decoder's inputs).
//
// While learning (from start to the main menu) two things are noted, both cheap:
//   - every read of a data archive (nn::fs::ReadFile, hle_fs.c): which archive, where, how much. The game reads 256 KiB cache pages;
//   - every block the decoder is handed (hle_tact.c): its first 16 bytes, its length, a fingerprint of all of it, the asking thread.
// The game copies a block out of its page cache before decoding it, so the decoder's input says nothing about where the block came from.
// At the main menu a background thread therefore looks for each block in the pages that were read: it scans them for the blocks' first
// bytes and confirms a place by the fingerprint of the whole block. A block it cannot place is left out: the list is a hint, and the
// prefetch compares the whole compressed block before it hands out a result, so a wrong or stale line costs time, never correctness.
//
// Path: DM_BOOT_LIST=<file> (off = none), else bootlist_user.tsv beside the save folder (DM_SAVE), as the pipeline list pso_user.tsv is.
// DM_BOOT_LEARN=0/1 turns learning off / on; the default is on for the Mac and off for the phone product, where the prefetch's memory at
// launch (up to 256 MiB of blocks and 192 MiB of animation data in flight) has not been measured.
// The list is written when none was in use, or when the one in use no longer fits: more than 5% of this launch's blocks were not on it,
// or more than 10% of it went unused.
#include "hle.h"
#include "dm_events.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <unistd.h>

#define BL_NAMES 64        // data archives a list may name (the prefetch's own limit)
#define BL_READS 16384     // archive reads noted in one launch (about 2,500 reach the menu)
#define BL_MAX 32768       // blocks noted in one launch (about 5,500 reach the menu)
typedef struct { uint64_t off; uint32_t n; uint8_t name; } bl_read;
typedef struct { uint64_t off, fp; uint32_t n, dl, th; uint8_t key[16], name, placed; } bl_ent;
static _Atomic int g_bl_on;
static pthread_mutex_t g_bl_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_bl_names[BL_NAMES][32], g_bl_paths[BL_NAMES][1024]; static int g_bl_nn;
static int8_t g_bl_fd[4096];   // fd -> archive index + 1; 0 = not an archive
static bl_read *g_bl_rd; static uint32_t g_bl_nrd;
static bl_ent *g_bl; static uint32_t g_bl_n, g_bl_over;
static char g_bl_path[1100], g_bl_id[40];
static pthread_t g_bl_thr; static _Atomic int g_bl_thr_on;

int dm_bootlist_learning(void) { return atomic_load_explicit(&g_bl_on, memory_order_relaxed); }

// A fingerprint of a whole compressed block, eight bytes at a time (about 250 MiB are decoded before the menu: a few tens of ms in all)
static uint64_t bl_fp(const uint8_t *p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull ^ n, w; uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = ((h << 27 | h >> 37) ^ w) * 0x9E3779B97F4A7C15ull; }
    if (i < n) { w = 0; memcpy(&w, p + i, n - i); h = ((h << 27 | h >> 37) ^ w) * 0x9E3779B97F4A7C15ull; }
    return h ^ h >> 32;
}
static int bl_archive_name(const char *path, char out[32]) {   // "NN-NNNNNNNN.data", the archives' only name shape
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    if (strlen(b) != 16 || b[2] != '-' || strcmp(b + 11, ".data")) return 0;
    for (int i = 0; i < 11; i++) if (i != 2 && (b[i] < '0' || b[i] > '9')) return 0;
    memcpy(out, b, 17); return 1;
}
static void bl_on_open(int fd, const char *hostpath) {
    char nm[32]; if (fd < 0 || fd >= 4096) return;
    pthread_mutex_lock(&g_bl_mu);
    g_bl_fd[fd] = 0;
    if (bl_archive_name(hostpath, nm) && strlen(hostpath) < sizeof g_bl_paths[0]) {
        int i = 0; while (i < g_bl_nn && strcmp(g_bl_names[i], nm)) i++;
        if (i == g_bl_nn && g_bl_nn < BL_NAMES) { strcpy(g_bl_names[g_bl_nn], nm); strcpy(g_bl_paths[g_bl_nn++], hostpath); }
        if (i < g_bl_nn) g_bl_fd[fd] = (int8_t)(i + 1);
    }
    pthread_mutex_unlock(&g_bl_mu);
}
static void bl_on_read(int fd, int64_t off, const void *buf, size_t n) {
    (void)buf; if (fd < 0 || fd >= 4096 || !n || off < 0) return;
    pthread_mutex_lock(&g_bl_mu);
    if (g_bl_fd[fd] && g_bl_rd) { if (g_bl_nrd < BL_READS) g_bl_rd[g_bl_nrd++] = (bl_read){ (uint64_t)off, (uint32_t)(n > UINT32_MAX ? UINT32_MAX : n), (uint8_t)(g_bl_fd[fd] - 1) }; else g_bl_over++; }
    pthread_mutex_unlock(&g_bl_mu);
}
// A block the game has just decoded: in / n = the compressed block (after its 'Z'), dl = what it decoded to
void dm_bootlist_decode(const uint8_t *in, uint32_t n, uint32_t dl, uint32_t thread) {
    if (!dm_bootlist_learning() || n < 16) return;
    bl_ent e = { .n = n, .dl = dl, .th = thread, .fp = bl_fp(in, n) }; memcpy(e.key, in, 16);
    pthread_mutex_lock(&g_bl_mu);
    if (g_bl) { if (g_bl_n < BL_MAX) g_bl[g_bl_n++] = e; else g_bl_over++; }
    pthread_mutex_unlock(&g_bl_mu);
}

// Where the learned list lives; NULL = none. Fixed at the first call
const char *dm_bootlist_path(void) {
    static int done; if (done) return g_bl_path[0] ? g_bl_path : NULL; done = 1;
    const char *p = getenv("DM_BOOT_LIST"), *save = getenv("DM_SAVE");
    if (p && !strcmp(p, "off")) return NULL;
    if (p && *p) snprintf(g_bl_path, sizeof g_bl_path, "%s", p);
    else if (save && *save) { char d[1024]; snprintf(d, sizeof d, "%s", save); size_t l = strlen(d); while (l > 1 && d[l - 1] == '/') d[--l] = 0;
        char *s = strrchr(d, '/'); if (!s || s == d) return NULL; *s = 0; snprintf(g_bl_path, sizeof g_bl_path, "%s/bootlist_user.tsv", d); }
    return g_bl_path[0] ? g_bl_path : NULL;
}
// Called once at start, before the game reads anything. build_id = the game program's (may be empty when the host cannot read it).
// Returns the learned list's path when learning is on (the file may not exist yet), else NULL
const char *dm_bootlist_start(const char *build_id) {
    const char *e = getenv("DM_BOOT_LEARN"); int on = e && *e ? atoi(e) != 0 : !DM_PHONE;
    const char *p = on ? dm_bootlist_path() : NULL;
    if (!p) return NULL;
    snprintf(g_bl_id, sizeof g_bl_id, "%s", build_id && *build_id ? build_id : "-");
    g_bl = malloc(BL_MAX * sizeof *g_bl); g_bl_rd = malloc(BL_READS * sizeof *g_bl_rd);
    if (!g_bl || !g_bl_rd) { free(g_bl); free(g_bl_rd); g_bl = NULL; g_bl_rd = NULL; return NULL; }
    g_fs_open_hook = bl_on_open; g_fs_read_hook = bl_on_read; atomic_store(&g_bl_on, 1);
    return p;
}

// Drops repeats of one block (same first bytes, length and fingerprint), keeping the first: the order is the order the game asked in. Returns the new count
uint32_t dm_bootlist_dedup(void *ents, uint32_t n) {
    bl_ent *e = ents; uint32_t cap = 1; while (cap < n * 2 + 16) cap <<= 1;
    uint32_t *tab = calloc(cap, sizeof *tab), m = 0; if (!tab) return n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = (uint32_t)(e[i].fp >> 20) & (cap - 1); int dup = 0;
        for (; tab[s]; s = (s + 1) & (cap - 1)) { const bl_ent *o = &e[tab[s] - 1]; if (o->fp == e[i].fp && o->n == e[i].n && !memcmp(o->key, e[i].key, 16)) { dup = 1; break; } }
        if (dup) continue;
        if (m != i) e[m] = e[i];
        tab[s] = ++m;
    }
    free(tab); return m;
}
// Whether a list in use should be replaced: learned = this launch's distinct blocks, unlisted of them were not on the list; listed = the
// list's blocks, unused of them the game never asked for
int dm_bootlist_stale(uint32_t learned, uint32_t unlisted, uint32_t listed, uint32_t unused) {
    return (uint64_t)unlisted * 20 > learned || (uint64_t)unused * 10 > listed;
}

typedef struct { bl_ent *e; uint32_t n; bl_read *rd; uint32_t nrd; char names[BL_NAMES][32], paths[BL_NAMES][1024]; int nn; char path[1100], id[40]; } bl_job;
static int bl_rd_cmp(const void *a, const void *b) { const bl_read *x = a, *y = b; return x->name != y->name ? (int)x->name - (int)y->name : x->off < y->off ? -1 : x->off > y->off; }
// Finds where each block sits in the archives: scans the stretches the game read for the blocks' first 16 bytes, confirms by the fingerprint.
// Returns how many were placed
static uint32_t bl_place(bl_job *j) {
    uint32_t cap = 1; while (cap < j->n * 2 + 16) cap <<= 1;
    uint32_t *tab = calloc(cap, sizeof *tab), *next = calloc(j->n + 1, sizeof *next), placed = 0;   // key -> chain of blocks with that key (index + 1)
    uint8_t *two = calloc(65536 / 8, 1), *buf = NULL, *blk = NULL; size_t bufcap = 0, blkcap = 0;
    if (!tab || !next || !two) { free(tab); free(next); free(two); return 0; }
    for (uint32_t i = j->n; i-- > 0;) { uint64_t k8; memcpy(&k8, j->e[i].key, 8); uint32_t s = (uint32_t)((k8 * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
        while (tab[s] && memcmp(j->e[tab[s] - 1].key, j->e[i].key, 16)) s = (s + 1) & (cap - 1);
        next[i + 1] = tab[s]; tab[s] = i + 1;
        uint16_t t2 = (uint16_t)(j->e[i].key[0] | j->e[i].key[1] << 8); two[t2 >> 3] |= (uint8_t)(1 << (t2 & 7)); }
    qsort(j->rd, j->nrd, sizeof *j->rd, bl_rd_cmp);
    int fd = -1, cur = -1;
    for (uint32_t r = 0; r < j->nrd && placed < j->n;) {
        const bl_read *x = &j->rd[r]; uint64_t lo = x->off, hi = x->off + x->n; uint32_t r2 = r + 1;
        while (r2 < j->nrd && j->rd[r2].name == x->name && j->rd[r2].off <= hi) { uint64_t h2 = j->rd[r2].off + j->rd[r2].n; if (h2 > hi) hi = h2; r2++; }   // reads that touch or overlap are one stretch
        r = r2;
        if (cur != x->name) { if (fd >= 0) { dm_data_forget_fd(fd); close(fd); } cur = x->name; fd = open(j->paths[cur], O_RDONLY); if (fd >= 0) dm_data_note_fd(fd, j->paths[cur]); }
        if (fd < 0) continue;
        for (uint64_t at = lo; at < hi;) {   // in pieces of at most 4 MiB, each read 15 bytes past its end so that a key across the cut is seen
            size_t want = hi - at > (4u << 20) ? (4u << 20) : (size_t)(hi - at), need = want + 15;
            if (need > bufcap) { free(buf); buf = malloc(bufcap = need); if (!buf) { bufcap = 0; break; } }
            ssize_t got = dm_data_pread(fd, buf, need, (off_t)at); if (got < 16) break;
            size_t lim = (size_t)got - 15 < want ? (size_t)got - 15 : want;
            for (size_t i = 0; i < lim; i++) {
                uint16_t t2 = (uint16_t)(buf[i] | buf[i + 1] << 8); if (!(two[t2 >> 3] >> (t2 & 7) & 1)) continue;
                uint64_t k8; memcpy(&k8, buf + i, 8); uint32_t s = (uint32_t)((k8 * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
                while (tab[s] && memcmp(j->e[tab[s] - 1].key, buf + i, 16)) s = (s + 1) & (cap - 1);
                for (uint32_t c = tab[s]; c; c = next[c]) {
                    bl_ent *e = &j->e[c - 1]; if (e->placed) continue;
                    const uint8_t *src = buf + i;   // the block's bytes: in the piece just read when it ends inside it, else read from the file
                    if (i + e->n > (size_t)got) {
                        if (e->n > blkcap) { free(blk); blk = malloc(blkcap = e->n); if (!blk) { blkcap = 0; continue; } }
                        if (dm_data_pread(fd, blk, e->n, (off_t)(at + i)) != (ssize_t)e->n) continue;
                        src = blk; }
                    if (bl_fp(src, e->n) != e->fp) continue;
                    e->placed = 1; e->name = (uint8_t)cur; e->off = at + i; placed++;
                }
            }
            at += want;
        }
    }
    if (fd >= 0) { dm_data_forget_fd(fd); close(fd); }
    free(buf); free(blk); free(tab); free(next); free(two); return placed;
}
static void *bl_write(void *arg) {
    bl_job *j = arg; pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    uint64_t t0 = now_ns(); uint32_t placed = bl_place(j);
    char tmp[1120]; snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
    FILE *f = placed ? fopen(tmp, "w") : NULL; int ok = f != NULL;
    if (ok) ok = fprintf(f, "# BUILD_ID\t%s\n", j->id) > 0;
    for (uint32_t i = 0; ok && i < j->n; i++) {
        const bl_ent *e = &j->e[i]; char kh[33]; if (!e->placed) continue;
        for (int k = 0; k < 16; k++) snprintf(kh + 2 * k, 3, "%02x", e->key[k]);
        ok = fprintf(f, "%s\t%llu\t%u\t%u\t%s\t%#x\n", j->names[e->name], (unsigned long long)e->off, e->n, e->dl, kh, e->th) > 0;
    }
    if (f && fclose(f)) ok = 0;
    if (ok && rename(tmp, j->path)) ok = 0;
    if (!placed) LOG("boot list: none of %u blocks could be placed in the archives; nothing written", j->n);
    else if (!ok) { unlink(tmp); LOG("boot list: could not write %s (%s); the next launch learns again", j->path, strerror(errno)); }
    else LOG("boot list: %u of %u blocks placed in %.2f s, written to %s", placed, j->n, (now_ns() - t0) / 1e9, j->path);
    DM_EV("bootlist_written", "placed:d blocks:d ok:d", (int)placed, (int)j->n, ok);
    free(j->e); free(j->rd); free(j); return NULL;
}
// At the main menu. in_use: 0 = no list was loaded, 1 = the learned list was, -1 = another list was (a developer's): then nothing is written.
// known(key, n) says whether the list in use has a block with this key and length; listed / unused describe that list
void dm_bootlist_finish(int in_use, uint32_t listed, uint32_t unused, int (*known)(const uint8_t *key, uint32_t n)) {
    if (!atomic_exchange(&g_bl_on, 0)) return;
    g_fs_open_hook = NULL; g_fs_read_hook = NULL;
    pthread_mutex_lock(&g_bl_mu);   // a hook or a decode that was already past its check finishes before the tables are taken
    bl_ent *e = g_bl; uint32_t n = g_bl_n, over = g_bl_over, nrd = g_bl_nrd; bl_read *rd = g_bl_rd; g_bl = NULL; g_bl_rd = NULL; g_bl_n = g_bl_nrd = 0;
    pthread_mutex_unlock(&g_bl_mu);
    uint32_t raw = n; n = dm_bootlist_dedup(e, n);
    uint32_t unlisted = 0; if (in_use > 0 && known) for (uint32_t i = 0; i < n; i++) if (!known(e[i].key, e[i].n)) unlisted++;
    int write = n && nrd && (in_use == 0 || (in_use > 0 && dm_bootlist_stale(n, unlisted, listed, unused)));
    LOG("boot list: this launch decoded %u blocks before the menu (%u distinct) from %u archive reads%s; %s", raw, n, nrd, over ? " (tables full, some left out)" : "",
        in_use < 0 ? "another list is in use, nothing written" : in_use == 0 ? (write ? "none was in use, writing one" : "nothing to write")
        : write ? "the list in use no longer fits, writing a new one" : "the list in use still fits");
    if (in_use > 0) LOG("boot list: %u of this launch's blocks were not on the list; %u of the list's %u went unused", unlisted, unused, listed);
    DM_EV("bootlist", "learned:d reads:d in_use:d unlisted:d listed:d unused:d write:d", (int)n, (int)nrd, in_use, (int)unlisted, (int)listed, (int)unused, write);
    bl_job *j = write ? calloc(1, sizeof *j) : NULL;
    if (!j) { free(e); free(rd); return; }
    j->e = e; j->n = n; j->rd = rd; j->nrd = nrd; j->nn = g_bl_nn; memcpy(j->names, g_bl_names, sizeof j->names); memcpy(j->paths, g_bl_paths, sizeof j->paths);
    snprintf(j->path, sizeof j->path, "%s", g_bl_path); snprintf(j->id, sizeof j->id, "%s", g_bl_id);
    if (pthread_create(&g_bl_thr, NULL, bl_write, j)) { free(e); free(rd); free(j); return; }
    atomic_store(&g_bl_thr_on, 1);
}
// Waits for a list being written (tests, and a quit right after the menu)
void dm_bootlist_join(void) { if (atomic_exchange(&g_bl_thr_on, 0)) pthread_join(g_bl_thr, NULL); }
