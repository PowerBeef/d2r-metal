// dm_bright.c without the game: the game-derived table status and its red marking in the Mac App.
// Each case runs in a forked child (dm_bright.c keeps its state in statics behind pthread_once); table files go under $TMPDIR.
#include "../dm.h"
#include "../hle.h"
#include "../dm_hud.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

module_t g_mod[MAX_MOD]; int g_nmod;
static uint64_t g_now; static int g_in, g_area;
uint64_t now_ns(void) { return g_now; }
int dm_uiscreen_ingame(void) { return g_in; }
void dm_hud_get(dm_hud_info *h) { memset(h, 0, sizeof *h); h->valid = 1; h->area = g_area; }
int dm_loadscreen_up(void) { return 0; }
int dm_hud_exp_rows(void) { return 100; }
int dm_bright_level_rows(void); int dm_tables_status(char *buf, size_t n); void dm_tables_report(void);

static char g_log[16384];
static void log_cap(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", b); if (strlen(g_log) + strlen(b) + 2 < sizeof g_log) { strcat(g_log, b); strcat(g_log, "\n"); } }
static int fail, count;
#define EXPECT(name, want, got) do { long long w = (want), g = (got); count++; printf("%s want %lld got %lld %s\n", name, w, g, w == g ? "✓" : "✗"); fail += w != g; } while (0)
static char g_tmp[1024];

typedef void (*case_fn)(void);
static int run_case(const char *name, case_fn fn) {   // 0 = passed; the child's exit code is its failure count
    fflush(stdout); pid_t p = fork();
    if (p == 0) { g_dm_log_hook = log_cap; fail = count = 0; fn(); printf("-- %s: %d checks, %d failed\n", name, count, fail); fflush(stdout); _exit(fail ? 1 : 0); }
    int st = 0; waitpid(p, &st, 0); return !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// Both tables present (hud_exp.tsv comes from the dm_hud_exp_rows stub: 100 rows)
static void tables_ok(void) {
    char d[1100], f[1200]; snprintf(d, sizeof d, "%s/t1", g_tmp); mkdir(d, 0755); snprintf(f, sizeof f, "%s/bright_levels.tsv", d);
    FILE *o = fopen(f, "w"); fputs("34\t0\t1\t-1\n1\t0\t0\t1\n# x\n", o); fclose(o);
    setenv("DM_TABLES", d, 1); unsetenv("DM_APP");
    EXPECT("tables ok: two level rows", 2, dm_bright_level_rows());
    char s[1400]; EXPECT("tables ok: status ok", 1, dm_tables_status(s, sizeof s));
    printf("   status: %s\n", s);
    EXPECT("tables ok: the status text", 1, !strncmp(s, "bright_levels.tsv 2 rows, hud_exp.tsv 100 rows (", 48) && strstr(s, "/t1)") != NULL);
    dm_tables_report(); EXPECT("tables ok: a plain Tables: line", 1, strstr(g_log, "Tables: bright_levels.tsv 2 rows") != NULL && !strstr(g_log, "【红】"));
}
// No bright_levels.tsv: a plain line on the CLI host
static void tables_missing(void) {
    char d[1100]; snprintf(d, sizeof d, "%s/none", g_tmp); mkdir(d, 0755);
    setenv("DM_TABLES", d, 1); unsetenv("DM_APP");
    EXPECT("tables missing: rows -1", -1, dm_bright_level_rows());
    char s[1400]; EXPECT("tables missing: status not ok", 0, dm_tables_status(s, sizeof s));
    EXPECT("tables missing: the status says so", 1, strstr(s, "bright_levels.tsv missing") != NULL);
    dm_tables_report(); EXPECT("tables missing (CLI host): a Tables: line, not red", 1, strstr(g_log, "Tables: bright_levels.tsv missing") != NULL && !strstr(g_log, "【红】"));
}
// The same in the Mac App (DM_APP set): a broken install, so the line is red
static void tables_missing_app(void) {
    char d[1100]; snprintf(d, sizeof d, "%s/none", g_tmp); mkdir(d, 0755);
    setenv("DM_TABLES", d, 1); setenv("DM_APP", "1", 1);
    dm_tables_report(); EXPECT("tables missing (Mac App): the Tables: line is red", 1, strstr(g_log, "【红】Tables: bright_levels.tsv missing") != NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(g_tmp, sizeof g_tmp, "%s/t_bright.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(g_tmp, 0755);
    int bad = 0;
    bad += run_case("tables_ok", tables_ok); bad += run_case("tables_missing", tables_missing); bad += run_case("tables_missing_app", tables_missing_app);
    { char f[1200]; snprintf(f, sizeof f, "%s/t1/bright_levels.tsv", g_tmp); unlink(f); snprintf(f, sizeof f, "%s/t1", g_tmp); rmdir(f); snprintf(f, sizeof f, "%s/none", g_tmp); rmdir(f); rmdir(g_tmp); }
    printf("== t_bright: %d case(s) failed ==\n", bad); return bad != 0;
}
