// dm_tables.c and dm_tables.h without the game: the table reader, the experience table's status text and its red marking in the Mac App.
// Table files go under $TMPDIR.
#include "../dm.h"
#include "../dm_tables.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_rows;
int dm_hud_exp_rows(void) { return g_rows; }
int dm_tables_status(char *buf, size_t n); void dm_tables_report(void);

static char g_log[4096];
static void log_cap(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", b); if (strlen(g_log) + strlen(b) + 2 < sizeof g_log) { strcat(g_log, b); strcat(g_log, "\n"); } }
static int fail, count;
#define EXPECT(name, want, got) do { long long w = (want), g = (got); count++; printf("%s want %lld got %lld %s\n", name, w, g, w == g ? "✓" : "✗"); fail += w != g; } while (0)
static void report(int rows, int app) { g_rows = rows; g_log[0] = 0; if (app) setenv("DM_APP", "1", 1); else unsetenv("DM_APP"); dm_tables_report(); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); g_dm_log_hook = log_cap;
    char d[1100], f[1200], s[1400], path[PATH_MAX], want[1400]; long v[4];
    snprintf(d, sizeof d, "%s/t_tables.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(d, 0755); snprintf(f, sizeof f, "%s/x.tsv", d);
    setenv("DM_TABLES", d, 1);

    // The reader: tab-separated integers per row, comments and blank lines skipped, what follows the columns ignored, a short row left out; -1 when the file is missing
    EXPECT("reader: a missing file reads -1", -1, dm_table_read("x.tsv", 2, v, 2, path));
    EXPECT("reader: gives the path it looked for", 0, strcmp(path, f));
    FILE *o = fopen(f, "w"); fputs("# comment\n1\t2\tnote\n\n7\n3\t4\n5\t6\n", o); fclose(o);
    EXPECT("reader: two rows, up to the maximum", 2, dm_table_read("x.tsv", 2, v, 2, path));
    EXPECT("reader: the values", 1, v[0] == 1 && v[1] == 2 && v[2] == 3 && v[3] == 4);
    unlink(f);

    // The status text: the folder is DM_TABLES', else tables/ beside the executable
    g_rows = 100; EXPECT("status: 100 rows are usable", 1, dm_tables_status(s, sizeof s));
    snprintf(want, sizeof want, "hud_exp.tsv 100 rows (%s)", d); EXPECT("status: the text", 0, strcmp(s, want));
    g_rows = -1; EXPECT("status: missing is not usable", 0, dm_tables_status(s, sizeof s));
    snprintf(want, sizeof want, "hud_exp.tsv missing (%s)", d); EXPECT("status: says missing", 0, strcmp(s, want));
    g_rows = 0; EXPECT("status: empty is not usable", 0, dm_tables_status(s, sizeof s)); EXPECT("status: says empty", 1, strstr(s, "hud_exp.tsv empty (") == s);
    g_rows = 40; EXPECT("status: a short table is not usable", 0, dm_tables_status(s, sizeof s)); EXPECT("status: says what it needs", 1, strstr(s, "hud_exp.tsv 40 rows (needs 100) (") == s);
    unsetenv("DM_TABLES"); g_rows = 100; dm_tables_status(s, sizeof s); EXPECT("status: without DM_TABLES, tables/ beside the executable", 1, strstr(s, "/tables)") != NULL); setenv("DM_TABLES", d, 1);

    // The Tables: line: plain when usable, whoever runs; a missing or short table says the bar stays empty, red only in the Mac App (DM_APP), where it means a broken install
    report(100, 0); EXPECT("report: a plain line", 1, strstr(g_log, "Tables: hud_exp.tsv 100 rows") != NULL && !strstr(g_log, "【红】") && !strstr(g_log, "stays empty"));
    report(100, 1); EXPECT("report (Mac App): usable is not red", 1, strstr(g_log, "Tables: hud_exp.tsv 100 rows") != NULL && !strstr(g_log, "【红】"));
    report(-1, 0); EXPECT("report (CLI host): missing, not red", 1, strstr(g_log, "Tables: hud_exp.tsv missing") != NULL && strstr(g_log, "stays empty") != NULL && !strstr(g_log, "【红】"));
    report(-1, 1); EXPECT("report (Mac App): missing is red", 1, strstr(g_log, "【红】Tables: hud_exp.tsv missing") != NULL);
    report(40, 1); EXPECT("report (Mac App): short is red", 1, strstr(g_log, "【红】Tables: hud_exp.tsv 40 rows (needs 100)") != NULL);

    rmdir(d);
    printf("== t_tables: %d checks, %d failed ==\n", count, fail); return fail != 0;
}
