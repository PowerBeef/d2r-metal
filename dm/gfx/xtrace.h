// 系统时间线打点：鸿蒙走 HiTrace（hitrace 抓取时才有开销）；苹果走 os_signpost（Instruments「Points of Interest」轨道，子系统 org.d2rmobile；不在录制时只多一次判断）；别的平台为空。
// 用来在系统时间线上看长帧时各线程在干活、在等，还是被抢占。
// 苹果那边区间名都是「dm」、真名在消息里（os_signpost 的名字必须是字面量，而 XTRACE_END 不带名字）；每线程一个栈配对开始与结束，不录制时也照样进出栈，录制中途开始也配得上
// System timeline markers: HiTrace on HarmonyOS (costs only while hitrace is capturing); os_signpost on Apple platforms (the Instruments "Points of Interest" track, subsystem org.d2rmobile; one check when not recording); empty elsewhere.
// Used to see on the system timeline whether each thread is working, waiting or preempted during a long frame.
// On Apple every interval is named "dm" with the real name in the message (os_signpost names must be literals, and XTRACE_END carries no name); a per-thread stack pairs begins with ends, pushed and popped even when not recording, so a recording that starts midway still pairs up
#pragma once
#ifdef __OHOS__
#include <hitrace/trace.h>
#define XTRACE_BEGIN(name) OH_HiTrace_StartTrace(name)
#define XTRACE_END() OH_HiTrace_FinishTrace()
#define XTRACE_COUNT(name, v) OH_HiTrace_CountTrace(name, (int64_t)(v))
#elif defined(__APPLE__)
#include <os/signpost.h>
#ifdef __cplusplus
extern "C" {
#endif
os_log_t dm_xtrace_log(void);            // gfx/xtrace.c
os_signpost_id_t dm_xtrace_push(void);
os_signpost_id_t dm_xtrace_pop(void);
#ifdef __cplusplus
}
#endif
#define XTRACE_BEGIN(name) do { os_signpost_id_t xid_ = dm_xtrace_push(); os_log_t xl_ = dm_xtrace_log(); if (os_signpost_enabled(xl_)) os_signpost_interval_begin(xl_, xid_, "dm", "%{public}s", name); } while (0)
#define XTRACE_END() do { os_signpost_id_t xid_ = dm_xtrace_pop(); os_log_t xl_ = dm_xtrace_log(); if (xid_ != OS_SIGNPOST_ID_NULL && os_signpost_enabled(xl_)) os_signpost_interval_end(xl_, xid_, "dm"); } while (0)
#define XTRACE_COUNT(name, v) do { os_log_t xl_ = dm_xtrace_log(); if (os_signpost_enabled(xl_)) os_signpost_event_emit(xl_, OS_SIGNPOST_ID_EXCLUSIVE, "dm-count", "%{public}s %lld", name, (long long)(v)); } while (0)
#else
#define XTRACE_BEGIN(name) ((void)0)
#define XTRACE_END() ((void)0)
#define XTRACE_COUNT(name, v) ((void)0)
#endif
