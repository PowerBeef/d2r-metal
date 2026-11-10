// The loader's build-output header check (loader.c read_layout): the r54 header with every key it knows loads, and a header with a host key
// it does not know (a build output newer than the host, e.g. a kept bench baseline host on a newer product) stops the host instead of leaving
// that key's thread slot unfilled.
#include "../loader.c"
#include <signal.h>
#include <sys/wait.h>

struct dm_flags g_flag;
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind, const char *name, void *function) {
    (void)kind; (void)name; return function;
}

static const char k_head[] =
    "# TARGET\t0x0\n# PATCH_OFF\t0x3220000\n# KEY_PAGE_OFF\t0x3214000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x1b786a0\n# RO_OFF\t0x1b79000\n# DATA_OFF\t0x2056000\n"
    "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n# KEY_KRES0_IDX\t4\n# KEY_KRES1_IDX\t5\n# KEY_KASPECT_IDX\t6\n"
    "# KEY_KASPECTUI_IDX\t7\n# KEY_KESHADOW_IDX\t8\n# KEY_KSHSIZE_IDX\t9\n# KEY_KJITTER_IDX\t10\n# KEY_KPIPESET_IDX\t11\n";

// Writes <dir>/main.fixups.tsv, then reads it in a child; returns the child's wait status
static int child_read(const char *dir, const char *extra) {
    char p[1024]; snprintf(p, sizeof p, "%s/main.fixups.tsv", dir);
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); }
    fputs(k_head, f); fputs(extra, f); fclose(f);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);   // the FATAL line of the refusal case is expected
        module_t m = { .name = "main" };
        read_layout(&m, dir);
        _exit(m.key_idx[11] == 11 && m.key_idx[9] == 9 && m.key_idx[0] == 0 && m.patch_off == 0x3220000 ? 0 : 3);
    }
    int st = 0; waitpid(pid, &st, 0); return st;
}

int main(void) {
    char dir[] = "/tmp/t_loader_layout.XXXXXX";
    const char *td = getenv("TMPDIR"); char buf[1024];
    if (td && *td) { snprintf(buf, sizeof buf, "%s/t_loader_layout.XXXXXX", td); if (!mkdtemp(buf)) { perror(buf); return 2; } }
    else if (!mkdtemp(dir)) { perror(dir); return 2; }
    const char *d = td && *td ? buf : dir;
    int fail = 0, st;
    st = child_read(d, "");
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0; fail += !ok;
    printf("%s the r54 header loads (every key known, KPIPESET at slot 11)\n", ok ? "✓" : "✗");
    st = child_read(d, "# KEY_KFUTURE_IDX\t12\n");
    ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT; fail += !ok;
    printf("%s a header with an unknown host key (KEY_KFUTURE_IDX) stops the host (status %#x)\n", ok ? "✓" : "✗", st);
    char p[1100]; snprintf(p, sizeof p, "%s/main.fixups.tsv", d); unlink(p); rmdir(d);
    if (fail) printf("t_loader_layout: %d failed\n", fail);
    return fail;
}
