// System timeline markers: os_signpost (the Instruments "Points of Interest" track, subsystem org.d2rmobile; no cost while not recording).
// Used to see on the system timeline whether each thread is working, waiting or preempted during a long frame.
// Every interval is named "dm" with the real name in the message (os_signpost names must be literals, and XTRACE_END carries no name); a per-thread stack pairs begins with ends.
#pragma once
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
