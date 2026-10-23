// 装载器重定位的边界核对（F-CORE-13）：未知类型、偏移越出映像、符号下标越出符号表都要当场 die，不能按 ABS64 照写或写到映像外。
// 自造一个小模块，每个坏用例在子进程里跑 relocate：期望子进程以 SIGABRT 结束（die → abort）；好的那张表照常写进槽里。
// Loader relocation bounds checks (F-CORE-13): an unknown type, an offset past the image or a symbol index past the symbol table must die on the spot, not be written as ABS64 or outside the image.
// A small synthetic module; each bad case runs relocate in a child process and is expected to end with SIGABRT (die → abort); the good table is written into its slots as usual.
#include "../loader.c"
#include <signal.h>
#include <sys/wait.h>

struct dm_flags g_flag;
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind, const char *name, void *function) { (void)kind; (void)name; return function; }

static _Alignas(16) uint8_t img[0x1000];
static const char names[] = "\0defined";
static Elf64_Sym syms[2] = {{0}, {.st_name = 1, .st_info = 0x10, .st_shndx = 1, .st_value = 0x100}};
static Elf64_Dyn dyn[] = {{.d_tag = DT_RELA, .d_un.d_ptr = 0x300}, {.d_tag = DT_RELASZ, .d_un.d_val = sizeof(Elf64_Rela)}, {0}};
static void setup(Elf64_Rela r) {
    memset(img, 0, sizeof img);
    *(Elf64_Rela *)(img + 0x300) = r;
    g_mod[0] = (module_t){.base = img, .size = sizeof img, .symtab = syms, .strtab = names, .nsym = 2, .dyn = dyn};
    g_nmod = 1;
}
// 子进程里重定位：0 = 正常返回，否则 = 结束它的信号；*fatal = 它的日志里有没有 die 的 FATAL 行（别的中止不算）
// relocate in a child: 0 = returned normally, otherwise the signal that ended it; *fatal = whether its log has die's FATAL line (any other abort does not count)
static int child_sig(Elf64_Rela r, int *fatal) {
    fflush(stdout);
    int pfd[2]; if (pipe(pfd)) abort();
    pid_t p = fork();
    if (!p) { close(pfd[0]); dup2(pfd[1], 2); signal(SIGABRT, SIG_DFL); setup(r); relocate(&g_mod[0]); _exit(0); }
    close(pfd[1]);
    char log[4096]; size_t n = 0; ssize_t k;
    while ((k = read(pfd[0], log + n, sizeof log - 1 - n)) > 0) n += (size_t)k;
    log[n] = 0; close(pfd[0]);
    int st = 0; waitpid(p, &st, 0);
    *fatal = strstr(log, "FATAL ") != NULL;
    return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}
static int g_fail;
static void expect(const char *what, Elf64_Rela r) {
    int fatal = 0, got = child_sig(r, &fatal);
    printf("  %-44s -> %s%s (expect die: FATAL + SIGABRT)\n", what, got ? strsignal(got) : "returned", fatal ? " after FATAL" : "");
    if (got != SIGABRT || !fatal) g_fail++;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // 好表：RELATIVE、按符号的 ABS64 / GLOB_DAT / JUMP_SLOT 各一条，写在最后一个完整槽 | good tables: one RELATIVE and one symbol-based ABS64 / GLOB_DAT / JUMP_SLOT each, written in the last whole slot
    const unsigned good[] = {R_AARCH64_RELATIVE, R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT};
    for (int i = 0; i < 4; i++) {
        setup((Elf64_Rela){.r_offset = sizeof img - 8, .r_info = (good[i] == R_AARCH64_RELATIVE ? 0 : 1ull << 32) | good[i], .r_addend = 8});
        relocate(&g_mod[0]);
        uint64_t v; memcpy(&v, img + sizeof img - 8, 8);
        uint64_t want = (uint64_t)img + (good[i] == R_AARCH64_RELATIVE ? 8 : good[i] == R_AARCH64_JUMP_SLOT ? 0x100 : 0x108);
        printf("  type %4u at the last slot -> %s\n", good[i], v == want ? "applied" : "WRONG");
        if (v != want) g_fail++;
    }
    expect("unknown type 1028 (R_AARCH64_TLS_DTPMOD)", (Elf64_Rela){.r_offset = 0x200, .r_info = 1ull << 32 | 1028});
    expect("unknown type 0 (R_AARCH64_NONE)", (Elf64_Rela){.r_offset = 0x200, .r_info = 0});
    expect("RELATIVE with r_offset + 8 > size", (Elf64_Rela){.r_offset = sizeof img - 4, .r_info = R_AARCH64_RELATIVE});
    expect("RELATIVE with r_offset far past the image", (Elf64_Rela){.r_offset = 1ull << 40, .r_info = R_AARCH64_RELATIVE});
    expect("GLOB_DAT with symbol index >= nsym", (Elf64_Rela){.r_offset = 0x200, .r_info = 7ull << 32 | R_AARCH64_GLOB_DAT});
    dyn[1].d_un.d_val = 1ull << 40;   // 表本身越出映像 | the table itself runs past the image
    expect("DT_RELASZ past the image", (Elf64_Rela){.r_offset = 0x200, .r_info = R_AARCH64_RELATIVE});
    printf("t_loader_reloc: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
