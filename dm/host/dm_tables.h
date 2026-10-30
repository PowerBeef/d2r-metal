// The two tables taken from the game's own data files: the experience needed per level (dm/ios/tools/gen_hud_exp.py) and, per level, its act, its
// layout type and whether its picture follows day and night (dm/tools/gen_bright_levels.py). The iOS app is built from the player's own files and
// compiles them in (dm_hud_exp.h, dm_bright_levels.inc, which ios/run.sh generates). The Mac host reads them at start instead, so a host built without
// the game's files can be shipped, and the launcher generates the tables on the player's Mac (dm/tools/install.py): from <DM_TABLES>/<name>, else the
// tables/ folder beside the executable (dm/host/Makefile writes it next to dmhost; the app sets DM_TABLES from its paths.txt).
// Format: one row per line, tab-separated integers; anything after them, and lines starting with '#', is ignored.
#pragma once
#include <TargetConditionals.h>
#if !TARGET_OS_IOS
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>

// Reads up to max rows of ncol integers into out[row * ncol + col]; returns the number of rows read, or -1 when the file is missing.
// *path receives the file it looked for (for the log line when it is missing)
static inline int dm_table_read(const char *name, int ncol, long *out, int max, char path[PATH_MAX]) {
    char exe[PATH_MAX]; uint32_t n = sizeof exe;
    const char *dir = getenv("DM_TABLES");
    if (dir) snprintf(path, PATH_MAX, "%s/%s", dir, name);
    else if (!_NSGetExecutablePath(exe, &n)) snprintf(path, PATH_MAX, "%s/tables/%s", dirname(exe), name);
    else { snprintf(path, PATH_MAX, "tables/%s", name); }
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512]; int rows = 0;
    while (rows < max && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *p = line, *e; int c = 0;
        for (; c < ncol; c++) { out[rows * ncol + c] = strtol(p, &e, 10); if (e == p) break; p = e; }
        if (c == ncol) rows++;
    }
    fclose(f);
    return rows;
}
#endif
