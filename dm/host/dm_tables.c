// The game-derived table at a glance: the experience table hud_exp.tsv (read by diag.c through dm_tables.h, compiled in on iOS) as one status text, for the
// Tables: line at host start and Share Logs' info.txt.
#include "dm.h"
#include <TargetConditionals.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !TARGET_OS_IOS
#include "dm_tables.h"
#endif

int dm_hud_exp_rows(void);   // diag.c

// "hud_exp.tsv 100 rows (<folder>)", 'missing' / 'empty' / '(needs 100)' when not. Returns 1 when the table is usable
int dm_tables_status(char *buf, size_t n) {
    int h = dm_hud_exp_rows(); char hs[48], where[PATH_MAX + 8];
    if (h < 0) snprintf(hs, sizeof hs, "missing"); else if (!h) snprintf(hs, sizeof hs, "empty"); else snprintf(hs, sizeof hs, h == 100 ? "%d rows" : "%d rows (needs 100)", h);
#if TARGET_OS_IOS
    snprintf(where, sizeof where, " (built in)");
#else
    { char d[PATH_MAX]; dm_table_path("hud_exp.tsv", d); char *sl = strrchr(d, '/'); if (sl) *sl = 0; else strlcpy(d, ".", sizeof d); snprintf(where, sizeof where, " (%s)", d); }
#endif
    snprintf(buf, n, "hud_exp.tsv %s%s", hs, where);
    return h == 100;
}
// One line at host start (main.c host_preload). Red only in the Mac App, where a missing table means a broken install; developer CLI hosts may run without it
void dm_tables_report(void) {
    char s[PATH_MAX + 96]; int ok = dm_tables_status(s, sizeof s);
    LOG("%sTables: %s%s", !ok && getenv("DM_APP") ? "【红】" : "", s, ok ? "" : ": the experience bar stays empty");
}
