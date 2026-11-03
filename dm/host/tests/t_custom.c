// Custom settings unit test (dm_custom.c, takes a second): Frame Drops (q3d) follows its default, which the front end gives per device (front_q3d_default:
// the Mac App's recommendation, 0 on phones, -1 = k_def, 1 on the CLI host), unless the player picked the other value. sw_custom.txt format 2 (custom_v)
// writes q3d only while it differs from that default; an older file's q3d equal to the old default (k_def) follows today's default instead of pinning it.
// Also checked: the Extras key bright (Brighten dark areas) defaults to on, is saved when turned off and read back. Each case runs in a forked child after
// dm_custom_init(<tmpdir>), since dm_custom.c loads its file once per process. The stubs below stand in for the front end, the language, the 3D scale
// controller and the picture settings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../dm_custom.h"
#include "../dm_i18n.h"
#include "../hle.h"

module_t g_mod[MAX_MOD]; int g_nmod;
static int g_q3d_def = -1, g_qmode = -1;
int front_q3d_default(void) { return g_q3d_def; }
int dm_lang_group(void) { return DM_LG_EN; }
const char *dm_save_commitdir(void) { return NULL; }
void dm_3d_quality_mode_set(int mode) { g_qmode = mode; }
int dm_3d_quality_mode_get(void) { return g_qmode; }
float dm_3d_scale_cur(void) { return 1; }
void dm_video_on_uimsg(const char *cat, const char *act, const char *arg) { (void)cat; (void)act; (void)arg; }

static char g_tdir[512];
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void file_put(const char *txt) { char p[600]; snprintf(p, sizeof p, "%s/sw_custom.txt", g_tdir); if (!txt) { unlink(p); return; } FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); }
static const char *file_get(void) { static char b[600]; char p[600]; snprintf(p, sizeof p, "%s/sw_custom.txt", g_tdir); FILE *f = fopen(p, "r"); if (!f) return ""; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }

// One load in a child: q3d and bright as loaded, the 3D quality mode handed to the controller, the file after the load (file0); then, when set_key is
// given, that key's pending value and an Apply, q3d after it and the file (file)
typedef struct { int q3d, bright, qmode, q3d_after; char file0[600], file[600]; } result;
static result run(const char *txt, int q3d_def, const char *set_key, int set_v) {
    file_put(txt); int pp[2]; result r; memset(&r, 0, sizeof r); r.q3d = -9; if (pipe(pp)) return r;
    pid_t pid = fork();
    if (!pid) { close(pp[0]); result x; memset(&x, 0, sizeof x); g_q3d_def = q3d_def; dm_custom_init(g_tdir);
        x.q3d = dm_custom_get("q3d"); x.bright = dm_custom_get("bright"); x.qmode = g_qmode; strlcpy(x.file0, file_get(), sizeof x.file0);
        if (set_key) { void dm_custom_set_pending(const char *key, int v); dm_custom_set_pending(set_key, set_v); dm_custom_on_uimsg("OptionsPanelMessage", "AcceptChanges", ""); }
        x.q3d_after = dm_custom_get("q3d"); strlcpy(x.file, file_get(), sizeof x.file);
        if (write(pp[1], &x, sizeof x) != (ssize_t)sizeof x) _exit(1); _exit(0); }
    close(pp[1]); if (read(pp[0], &r, sizeof r) != (ssize_t)sizeof r) r.q3d = -9; close(pp[0]); waitpid(pid, NULL, 0); return r;
}

int main(void) {
    snprintf(g_tdir, sizeof g_tdir, "%s/t_custom.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(g_tdir, 0755);
    result r;
    // (c1) no file, the device's default Lower (0): q3d 0, handed to the controller; an Apply of another key writes custom_v 2 and no q3d (nor lang,
    //      never picked)
    r = run(NULL, 0, "eco_fps", 1);
    CHECK(r.q3d == 0 && r.qmode == 0, "(c1) no file: q3d 0 (%d), controller mode 0 (%d)", r.q3d, r.qmode);
    CHECK(strstr(r.file, "custom_v=2\n") == r.file && strstr(r.file, "eco_fps=1\n") && !strstr(r.file, "q3d=") && !strstr(r.file, "lang="), "(c1) saved: custom_v=2 first, eco_fps=1, no q3d or lang\n%s", r.file);
    // (c2) an older file (no custom_v) with q3d=1, the old default (k_def), and today's default Lower: q3d follows the default; nothing is written at load,
    //      and the next Apply drops the q3d line
    r = run("drop_light=1\neco_fps=0\nq3d=1\n", 0, "eco_fps", 1);
    CHECK(r.q3d == 0 && r.qmode == 0 && !strcmp(r.file0, "drop_light=1\neco_fps=0\nq3d=1\n"), "(c2) older q3d=1 (the old default): follows Lower, q3d %d (mode %d), not rewritten at load\n%s", r.q3d, r.qmode, r.file0);
    CHECK(strstr(r.file, "custom_v=2\n") && !strstr(r.file, "q3d="), "(c2) after an Apply: custom_v=2, no q3d line\n%s", r.file);
    // (c3) an older file with q3d=0 and today's default Keep: 0 differs from the old default, so it is the player's choice (a v1.2.0 Lower) and stays saved
    r = run("eco_fps=0\nq3d=0\n", 1, "eco_fps", 1);
    CHECK(r.q3d == 0 && strstr(r.file, "q3d=0\n"), "(c3) older q3d=0 with a Keep default: kept 0 (%d), still saved\n%s", r.q3d, r.file);
    // (c4) a format-2 file's q3d is always the player's choice
    r = run("custom_v=2\nq3d=1\n", 0, NULL, 0);
    CHECK(r.q3d == 1 && r.qmode == 1, "(c4) custom_v 2, q3d=1 with a Lower default: the player's Keep (%d, mode %d)", r.q3d, r.qmode);
    // (c5) a format-2 file without q3d follows the default, whichever it is
    r = run("custom_v=2\neco_fps=0\n", 1, NULL, 0);
    CHECK(r.q3d == 1, "(c5) no q3d, default Keep: 1, got %d", r.q3d);
    r = run("custom_v=2\neco_fps=0\n", 0, NULL, 0);
    CHECK(r.q3d == 0, "(c5) no q3d, default Lower: 0, got %d", r.q3d);
    // (c6) the player sets Frame Drops back to its default: the q3d line goes, so it follows the default again
    r = run("custom_v=2\nq3d=1\n", 0, "q3d", 0);
    CHECK(r.q3d_after == 0 && !strstr(r.file, "q3d="), "(c6) q3d back to the default: %d, no q3d line\n%s", r.q3d_after, r.file);
    // (c7) the CLI host (no front-end default: k_def, Keep): an older q3d=1 reads 1, and an Apply leaves it out
    r = run("eco_fps=0\nq3d=1\n", -1, "eco_fps", 1);
    CHECK(r.q3d == 1 && !strstr(r.file, "q3d="), "(c7) CLI: q3d 1 (%d), not saved at its default\n%s", r.q3d, r.file);
    // (c8) bright (Brighten dark areas, the Extras page): on by default, saved when turned off, read back
    r = run(NULL, 0, "bright", 0);
    CHECK(r.bright == 1 && strstr(r.file, "bright=0\n"), "(c8) bright: default on (%d), saved off\n%s", r.bright, r.file);
    r = run("custom_v=2\nbright=0\n", 0, NULL, 0);
    CHECK(r.bright == 0, "(c8) bright=0 read back, got %d", r.bright);
    file_put(NULL); rmdir(g_tdir);
    printf("t_custom: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
