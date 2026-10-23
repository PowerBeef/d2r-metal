// 脏页跟踪的槽表单测：销池后槽复用、同时存活的池超过槽数、相邻池共用宿主页（槽复用后的缺页、按池键销池）。
// Unit tests of the dirty-page tracker's slot table: slot reuse after pools are destroyed, more live pools than slots, adjacent pools sharing a host page (faults after slot reuse, destroying a pool by its key).
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-52s 期望 %-4lld → 真结果 %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static void *page_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

// 共页的池：NVN 池只按 4 KiB 对齐，跟踪按宿主页（Apple 芯片 16 KiB）取整，相邻两池共用边界那一页。
// 下面每项在子进程里跑（改坏时缺页处理不认这一页，子进程被 SIGSEGV 结束，父进程照常记失败）
// Pools sharing a page: NVN pools are only 4 KiB aligned, tracking rounds out to host pages (16 KiB on Apple silicon), so two adjacent pools share the page at their boundary.
// Each case below runs in a child process (when broken, the fault handler does not claim the page and the child is ended by SIGSEGV; the parent records the failure as usual)
static uint8_t *arena4(void) {   // 4 个宿主页、按 4 个宿主页对齐 | 4 host pages, aligned to 4 host pages
    size_t pg = (size_t)getpagesize();
    uint8_t *p = mmap(NULL, 8 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return (uint8_t *)(((uintptr_t)p + 4 * pg - 1) & ~(uintptr_t)(4 * pg - 1));
}
// 甲（槽 1）保护了一页，丙（槽 0）销毁后，乙复用槽 0 且与甲共用这一页；游戏再写甲在这页上的贴图：写要成功（缺页被接住）且被记成脏
// A (slot 1) protected a page; after C (slot 0) is destroyed, B reuses slot 0 and shares that page with A; the game then writes A's texture on that page: the write must succeed (fault handled) and be recorded as dirty
static int case_shared_reuse(void) {
    size_t pg = (size_t)getpagesize();
    uint8_t *m = arena4(), *other = arena4();
    nvn_dirty_add_range(other, 0x1000);                                     // 丙：先占槽 0 | C: takes slot 0 first
    uint8_t *A = m, *tex = m + pg, *B = m + pg + 0x1000;                    // 甲 = [m, m + 宿主页 + 4K)：第 0、1 页；乙从第 1 页 +4K 起 | A = [m, m + host page + 4K): pages 0 and 1; B starts at page 1 + 4K
    nvn_dirty_add_range(A, pg + 0x1000);                                    // 槽 1 | slot 1
    nvn_dirty_protect(tex, 0x1000);
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_remove_range(other);
    nvn_dirty_add_range(B, 0x3000);                                         // 复用槽 0，与甲共用第 1 页 | reuses slot 0, shares page 1 with A
    ((volatile uint8_t *)tex)[0] = 0x5a;                                    // 游戏写 | game write
    return tex[0] == 0x5a && nvn_dirty_since(tex, 0x1000, stamp) == 1 ? 0 : 1;
}
// 乙从甲最后一页中间开始；销毁乙不得把甲当成乙销毁（原先按「覆盖乙起点的第一个在册区间」找，找到的是甲）
// B starts in the middle of A's last page; destroying B must not destroy A in B's place (it used to look up "the first registered range covering B's start", which is A)
static int case_remove_exact(void) {
    size_t pg = (size_t)getpagesize();
    uint8_t *m = arena4(), *A = m, *B = m + pg + 0x1000;
    nvn_dirty_add_range(A, pg + 0x1000);
    nvn_dirty_add_range(B, 2 * pg);
    nvn_dirty_remove_range(B);
    int a_ok = nvn_dirty_tracked(A, 0x1000), b_gone = !nvn_dirty_tracked(m + 2 * pg + 0x1000, 0x1000);
    nvn_dirty_remove_range(A);
    return a_ok && b_gone && !nvn_dirty_tracked(A, 0x1000) ? 0 : 1;
}
static int in_child(int (*fn)(void)) {   // 子进程跑 fn：返回 0 = 通过 | Run fn in a child process: returns 0 = passed
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) _exit(fn());
    int st = 0; waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) printf("  （子进程被信号 %d 结束）\n", WTERMSIG(st));
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(void) {
    crash_install();
    EXPECT("共页池：槽复用后写甲受保护的贴图页，写成功且判脏", 0, in_child(case_shared_reuse));
    EXPECT("共页池：销毁乙只摘乙，甲仍在册", 0, in_child(case_remove_exact));

    for (int i = 0; i < 65; i++) {
        void *p = page_map();
        nvn_dirty_add_range(p, 4096);
        nvn_dirty_remove_range(p);
        munmap(p, 4096);
    }

    void *reused = page_map();
    nvn_dirty_add_range(reused, 4096);
    nvn_dirty_protect(reused, 4096);
    uint64_t stamp = nvn_dirty_now();
    ((volatile uint8_t *)reused)[0] = 1;
    EXPECT("累计 65 次销池后新池写入被判脏", 1, nvn_dirty_since(reused, 4096, stamp));
    nvn_dirty_remove_range(reused);
    munmap(reused, 4096);

    void *live[300];
    for (int i = 0; i < 300; i++) {
        live[i] = page_map();
        nvn_dirty_add_range(live[i], 4096);
    }
    void *overflow = live[299];
    nvn_dirty_protect(overflow, 4096);
    stamp = nvn_dirty_now();
    ((volatile uint8_t *)overflow)[0] = 2;
    EXPECT("同时存活超过上限的池保守判脏", 1, nvn_dirty_since(overflow, 4096, stamp));
    for (int i = 0; i < 300; i++) {
        nvn_dirty_remove_range(live[i]);
        munmap(live[i], 4096);
    }
    return g_fail != 0;
}
