// The boot list the machine learns (dm_bootlist.c): blocks are found again in the archive stretches that were read, a block whose first
// bytes and length match another's is told apart by its content, a block that starts across the end of a read is still found, repeats
// are dropped, a block that sits in no read stretch is left out, and the rule that replaces a list in use.
#include "../hle.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static int count, fail;
#define EXPECT(what, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); count++; if (w_ != g_) fail++; printf("%s: expected %lld, got %lld %s\n", what, w_, g_, w_ == g_ ? "ok" : "FAILED"); } while (0)
// What dm_bootlist.c needs from the file layer
void (*g_fs_open_hook)(int, const char *); void (*g_fs_read_hook)(int, int64_t, const void *, size_t);
void dm_data_note_fd(int fd, const char *p) { (void)fd; (void)p; } void dm_data_forget_fd(int fd) { (void)fd; }
ssize_t dm_data_pread(int fd, void *b, size_t n, off_t o) { return pread(fd, b, n, o); }
uint64_t now_ns(void) { return 0; }

static uint8_t g_file[600000];
static uint32_t rnd(void) { static uint32_t s = 12345; s = s * 1664525u + 1013904223u; return s >> 8; }
static void block(size_t at, size_t n, uint8_t tag) { for (size_t i = 0; i < n; i++) g_file[at + i] = (uint8_t)rnd(); g_file[at] = 0x78; g_file[at + 1] = 0xda; g_file[at + 20] = tag; }
int main(void) {
    char dir[] = "/tmp/t_bootlist.XXXXXX"; if (!mkdtemp(dir)) return 2;
    char arc[256], list[256]; snprintf(arc, sizeof arc, "%s/00-00000001.data", dir); snprintf(list, sizeof list, "%s/bootlist_user.tsv", dir);
    for (size_t i = 0; i < sizeof g_file; i++) g_file[i] = (uint8_t)(rnd() | 1);   // odd bytes: no stray 0x78 0xda... pairs are still possible, the fingerprint rejects them
    block(1000, 5000, 1);                        // A: a plain block
    block(262144 - 7, 9000, 2);                  // B: starts 7 bytes before the end of the first read
    block(300000, 700, 3);                       // C and D: the same first 16 bytes and length, different content after them
    memcpy(g_file + 400000, g_file + 300000, 700); g_file[400000 + 300] ^= 0x55;
    block(550000, 2000, 4);                      // E: in a part of the file that is never read
    int w = open(arc, O_CREAT | O_WRONLY, 0600); if (w < 0 || write(w, g_file, sizeof g_file) != (ssize_t)sizeof g_file) return 2; close(w);
    setenv("DM_BOOT_LIST", list, 1); setenv("DM_BOOT_LEARN", "1", 1);
    EXPECT("learning starts with a path", 1, dm_bootlist_start("abc") != NULL && dm_bootlist_learning());
    EXPECT("the file layer's observers are set", 1, g_fs_open_hook != NULL && g_fs_read_hook != NULL);
    int fd = open(arc, O_RDONLY); g_fs_open_hook(fd, arc);
    g_fs_open_hook(fd + 1, "/tmp/some/save.d2s"); g_fs_read_hook(fd + 1, 0, g_file, 100);   // not an archive: ignored
    g_fs_read_hook(fd, 0, g_file, 262144); g_fs_read_hook(fd, 262144, g_file, 262144);       // two cache pages: 0 to 524288
    dm_bootlist_decode(g_file + 1000, 5000, 11111, 0x1001);
    dm_bootlist_decode(g_file + 262144 - 7, 9000, 22222, 0x1005);
    dm_bootlist_decode(g_file + 400000, 700, 33333, 0x1005);    // D before C: the list keeps the order asked
    dm_bootlist_decode(g_file + 300000, 700, 44444, 0x1005);
    dm_bootlist_decode(g_file + 1000, 5000, 11111, 0x1001);     // A again: a repeat
    dm_bootlist_decode(g_file + 550000, 2000, 55555, 0x1001);   // E: nowhere in what was read
    dm_bootlist_finish(0, 0, 0, NULL); dm_bootlist_join();
    EXPECT("learning has stopped", 0, dm_bootlist_learning());
    EXPECT("the observers are cleared", 1, g_fs_open_hook == NULL && g_fs_read_hook == NULL);
    FILE *f = fopen(list, "r"); EXPECT("a list was written", 1, f != NULL); if (!f) return 1;
    char line[256], fn[32], kh[40]; unsigned long long off[8]; unsigned n[8], dl[8], th[8]; int rows = 0, hdr = 0;
    while (fgets(line, sizeof line, f)) { if (!strncmp(line, "# BUILD_ID\tabc", 14)) { hdr = 1; continue; }
        if (rows < 8 && sscanf(line, "%31s %llu %u %u %39s %x", fn, &off[rows], &n[rows], &dl[rows], kh, &th[rows]) == 6 && !strcmp(fn, "00-00000001.data")) rows++; }
    fclose(f);
    EXPECT("the header carries the build id", 1, hdr);
    EXPECT("four blocks listed (the repeat and the unread one are not)", 4, rows);
    EXPECT("A: its place", 1000, off[0]); EXPECT("A: its decoded length", 11111, dl[0]); EXPECT("A: its thread", 0x1001, th[0]);
    EXPECT("B: found across the end of a read", 262144 - 7, off[1]);
    EXPECT("D, asked first: the place whose content matches", 400000, off[2]); EXPECT("D: its decoded length", 33333, dl[2]);
    EXPECT("C: the other place", 300000, off[3]); EXPECT("C: its decoded length", 44444, dl[3]);
    EXPECT("a second finish does nothing", 0, (dm_bootlist_finish(0, 0, 0, NULL), dm_bootlist_learning()));
    // the rule that replaces a list in use
    EXPECT("fits: nothing unlisted, nothing unused", 0, dm_bootlist_stale(5000, 0, 5000, 0));
    EXPECT("fits: 5% unlisted exactly", 0, dm_bootlist_stale(5000, 250, 5000, 0));
    EXPECT("stale: more than 5% unlisted", 1, dm_bootlist_stale(5000, 251, 5000, 0));
    EXPECT("fits: 10% unused exactly", 0, dm_bootlist_stale(5000, 0, 5000, 500));
    EXPECT("stale: more than 10% unused", 1, dm_bootlist_stale(5000, 0, 5000, 501));
    close(fd); unlink(arc); unlink(list); rmdir(dir);
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0;
}
