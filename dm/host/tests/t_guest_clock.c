// 客体时钟冻结与读者不同步（F-CORE-8）：直接包含真的 hle_os.c。冻结先存冻结值再置标志，读者读到「未冻结」之后才取 now_ns()，就会返回比冻结值大的数，
// 下一次读到冻结值，客体时间倒退。4 条读者各查自己读到的值是否单调，冻结线程每 100 µs 按「切后台」原因冻结 / 解冻一次，跑 2 秒
// The guest clock freeze is not synchronized with readers (F-CORE-8): includes the real hle_os.c. The freeze stores the frozen value and then the flag, and a reader that saw "not frozen" and only then took now_ns() returns a value above the frozen one,
// and its next read returns the frozen value, so guest time goes backwards. 4 readers each check that what they read is monotonic while a freezer toggles the freeze for the "background" reason every 100 us, for 2 seconds
#include "../hle_os.c"
#include <unistd.h>
struct dm_flags g_flag;
void crash_watch_frame(void) {}
void dm_guest_clock_advanced(void) {}
void uimsg_subscribe(void (*fn)(const char *, const char *, const char *)) { (void)fn; }
static _Atomic int g_stop; static _Atomic long g_back, g_reads; static _Atomic uint64_t g_maxback;
static void *reader(void *p) {
    (void)p; uint64_t last = guest_ns(); long n = 0;
    while (!atomic_load(&g_stop)) {
        uint64_t v = guest_ns(); n++;
        if (v < last) { atomic_fetch_add(&g_back, 1); uint64_t d = last - v, m = atomic_load(&g_maxback); while (d > m && !atomic_compare_exchange_weak(&g_maxback, &m, d)) {} }
        last = v;
    }
    atomic_fetch_add(&g_reads, n); return NULL;
}
int main(int argc, char **argv) {
    int ms = argc > 1 ? atoi(argv[1]) : 2000; long toggles = 0;
    pthread_t r[4]; for (int i = 0; i < 4; i++) pthread_create(&r[i], NULL, reader, NULL);
    uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;
    while (now_ns() < end) { guest_clock_freeze_for(1, 1); usleep(100); guest_clock_freeze_for(1, 0); usleep(100); toggles++; }
    atomic_store(&g_stop, 1); for (int i = 0; i < 4; i++) pthread_join(r[i], NULL);
    uint64_t f0 = guest_ns(); guest_clock_freeze_for(1, 1); usleep(20000); uint64_t f1 = guest_ns(); guest_clock_freeze_for(1, 0); usleep(20000); uint64_t f2 = guest_ns();
    printf("t_guest_clock: %ld freeze/unfreeze pairs, %ld reads: %ld backward steps, largest %.3f us; frozen 20 ms advanced %.3f ms, then 20 ms unfrozen advanced %.3f ms -> %s\n",
           toggles, (long)g_reads, (long)g_back, atomic_load(&g_maxback) / 1e3, (f1 - f0) / 1e6, (f2 - f1) / 1e6,
           !g_back && f1 - f0 < 1000000 && f2 - f1 > 10000000 ? "OK" : "FAIL");
    return !g_back && f1 - f0 < 1000000 && f2 - f1 > 10000000 ? 0 : 1;
}
