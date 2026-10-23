// 苹果平台的系统时间线打点（xtrace.h 的 XTRACE_*）：os_signpost 的日志句柄与每线程的区间栈。别的平台整个文件为空
// System timeline markers on Apple platforms (XTRACE_* in xtrace.h): the os_signpost log handle and the per-thread interval stack. Empty on other platforms
#ifdef __APPLE__
#include "xtrace.h"
#include <pthread.h>

static os_log_t g_log;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init(void) { g_log = os_log_create("org.d2rmobile", OS_LOG_CATEGORY_POINTS_OF_INTEREST); }
os_log_t dm_xtrace_log(void) { pthread_once(&g_once, init); return g_log; }

#define XDEPTH 32   // 嵌套深度上限；超过的那几层不配对（只记深度，出栈照样减） | nesting limit; levels beyond it are not paired (only the depth is kept, popping still decrements)
static __thread os_signpost_id_t t_ids[XDEPTH];
static __thread int t_n;
os_signpost_id_t dm_xtrace_push(void) {
    os_signpost_id_t id = os_signpost_id_generate(dm_xtrace_log());
    if (t_n < XDEPTH) t_ids[t_n] = id;
    t_n++;
    return id;
}
os_signpost_id_t dm_xtrace_pop(void) {
    if (t_n <= 0) return OS_SIGNPOST_ID_NULL;   // 多出来的结束：不配 | an extra end: not paired
    t_n--;
    return t_n < XDEPTH ? t_ids[t_n] : OS_SIGNPOST_ID_NULL;
}
#endif
