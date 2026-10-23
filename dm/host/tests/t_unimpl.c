// 未实现导入的「首次调用记一行」（F-CORE-15）：直接包含真的 hle.c。原先「见过」是会回绕的 uint8_t 计数（g_stub_seen[idx]++），每 256 次又记一行；同一个桩调 1024 次应只有 1 行
// The "log the first call" of an unimplemented import (F-CORE-15): includes the real hle.c. The "seen" mark used to be a wrapping uint8_t counter (g_stub_seen[idx]++), logging again every 256 calls; 1024 calls of one stub must give exactly 1 line
#include "../hle.c"
#include <stdarg.h>
uint64_t unimpl_stubs[MAX_STUB];
struct dm_flags g_flag;
uint64_t guest_ns(void) { return 0; }
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) { (void)num; (void)regs; (void)orig_addr; return 0; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
static int g_unimpl_lines;
static void count_log(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; if (strstr(fmt, "UNIMPL")) g_unimpl_lines++; }
int main(void) {
    g_dm_log_hook = count_log;
    module_t m = {0};
    resolve_import(&m, "_ZN2nn3foo3BarEv", 0);   // 分到 0 号桩 | gets stub 0
    uint64_t regs[9] = {0};
    for (int i = 0; i < 1024; i++) unimpl_c(0, regs);
    printf("t_unimpl: 1024 calls of one unimplemented stub -> %d UNIMPL lines (expect 1) -> %s\n", g_unimpl_lines, g_unimpl_lines == 1 ? "OK" : "FAIL");
    return g_unimpl_lines == 1 ? 0 : 1;
}
