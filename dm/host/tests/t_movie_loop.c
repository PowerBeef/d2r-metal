// 循环视频空播的计时单测（直接包含 hle_movie.c）：按游戏的循环协议走（到点 → 读到流尾 → SeekTo 回开头 → 再挂下一轮），
// 上次到点停在很久以前（App 挂起 / 最小化时单调钟照走）时，恢复后最多补一轮，不把错过的每一轮都背靠背补发；平常照旧按上次到点的时刻续。
// Timer unit test for looping no-op video playback (includes hle_movie.c directly): follows the game's loop protocol (due -> read to end of stream -> SeekTo back to the start -> arm the next round);
// when the previous firing is long in the past (the monotonic clock keeps running while the App is suspended / minimized), at most one round is caught up after resuming instead of firing every missed round back to back; normally it still continues from the previous firing time.
#include "../hle_movie.c"
#include <stdio.h>
void *guest_sym(const char *n) { (void)n; return NULL; }   // 被测文件里的客体调用桩（这里不建计时线程） | stub for the guest calls in the file under test (no timer thread is created here)
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return (uint64_t)mono_ns(); }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL "); } else printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void fire(mv_t *m) { pthread_mutex_lock(&g_lock); g_armed = NULL; m->fired = m->deadline; pthread_mutex_unlock(&g_lock); }   // 计时线程到点那一步（timer_main） | the timer thread's firing step (timer_main)
// 离开 away_s 秒后回来：数一数有几轮已经到点、会被背靠背连发 | coming back after away_s seconds: count how many rounds are already due and would be fired back to back
static long due_after(double away_s, int64_t *next_in) {
    mv_t *m = calloc(1, sizeof *m); m->dur_ns = MV_SKIP_NS; void *self[2] = { NULL, m };
    pthread_mutex_lock(&g_lock); arm(m, mono_ns() + m->dur_ns); pthread_mutex_unlock(&g_lock);   // 同 dec_Start | as dec_Start
    fire(m); m->fired -= (int64_t)(away_s * 1e9);                                                 // 最后一次到点就在挂起之前 | the last firing was just before the suspension
    long loops = 0; int idx, rem;
    for (;;) { ext_SeekTo(self, 0, 0); dec_AcquireInputBufferIndex(self, &idx, &rem); if (idx < 0 || loops > 1000000) break; fire(m); loops++; }
    *next_in = m->deadline - mono_ns(); free(m); return loops;
}
int main(void) {
    int64_t next; long n = due_after(3600, &next);
    CHECK(n <= 1 && next > 0 && next <= MV_SKIP_NS, "离开 1 小时回来：立刻到点的 %ld 轮（≤ 1），下一轮 %.3f 秒后 | back after 1 hour: %ld rounds due at once (<= 1), next round in %.3f s", n, next / 1e9, n, next / 1e9);
    n = due_after(60, &next);
    CHECK(n <= 1 && next > 0 && next <= MV_SKIP_NS, "离开 60 秒回来：立刻到点的 %ld 轮（≤ 1） | back after 60 s: %ld rounds due at once (<= 1)", n, n);
    {   // 平常：上次到点就在一轮之内，下一轮接着上次到点的时刻（媒体时间续接，不累积唤醒延迟） | normal case: the previous firing is within one round, so the next one continues from it (media time, no accumulated wake-up delay)
        mv_t *m = calloc(1, sizeof *m); m->dur_ns = MV_SKIP_NS; void *self[2] = { NULL, m };
        m->fired = mono_ns() - MV_SKIP_NS / 4; int64_t f = m->fired; ext_SeekTo(self, 0, 0);
        CHECK(m->deadline == f + MV_SKIP_NS, "上次到点在一轮之内：下一轮 = 上次到点 + 时长 | previous firing within one round: next = previous firing + duration");
        free(m); }
    printf("== t_movie_loop %d 项，失败 %d | %d checks, %d failed ==\n", checks, fails, checks, fails);
    return fails != 0;
}
