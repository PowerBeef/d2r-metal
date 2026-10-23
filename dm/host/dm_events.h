// 事件流 v1（DM_EVENTS，dm_events.c）：给脚本 / 代理读的结构化事件，一行一个 JSON（NDJSON），与中文日志并行，不改日志本身。
//   DM_EVENTS=<文件>   写进这个文件（覆盖；iOS 上相对路径落在 Documents 下）
//   DM_EVENTS=log      不写文件，每个事件在日志里打一行「[dm] EV {json}」（iOS 经日志端口传回，logport.py 就能读）
//   DM_EVENTS_OSLOG=1  另写一份到系统日志（子系统 org.d2rmobile，类别 events，%{public}，Console / log stream 可见）
// 每行：{"seq":序号,"t":进程起来后的秒数,"ev":"类型",…字段…[,"tick":局内拍号]}；类型与字段见 dm/verify/event_stream.md。
// 调用方一律经下面几个弱定义的函数指针：没链进 dm_events.c（单测等）或没设 DM_EVENTS 时指针为空，调用点只多一次判断。
// Event stream v1 (DM_EVENTS, dm_events.c): structured events for scripts / agents, one JSON object per line (NDJSON), in parallel with the Chinese log, leaving the log itself unchanged.
//   DM_EVENTS=<file>   write into this file (truncated; on iOS a relative path lands under Documents)
//   DM_EVENTS=log      no file; each event prints one log line "[dm] EV {json}" (on iOS it travels back over the log port, so logport.py can read it)
//   DM_EVENTS_OSLOG=1  also write a copy to the system log (subsystem org.d2rmobile, category events, %{public}, visible in Console / log stream)
// Each line: {"seq":number,"t":seconds since the process started,"ev":"type",...fields...[,"tick":in-run tick]}; types and fields are listed in dm/verify/event_stream.md.
// Callers always go through the weakly defined function pointers below: when dm_events.c is not linked in (unit tests etc.) or DM_EVENTS is not set, the pointers are null and a call site only adds one check.
#pragma once
#include <stdint.h>

// 发一个事件。sig = 字段签名「名:型 名:型 …」，型 s 字符串（NULL 写 null）、d int、l long long、u unsigned long long、x 十六进制串、f double、b 真假（int）；值按签名顺序跟在后面。
// 例：DM_EV("save", "result:s n:d", "commit", n)
// Emit one event. sig = field signature "name:type name:type ...", type s string (NULL writes null), d int, l long long, u unsigned long long, x hex string, f double, b boolean (int); the values follow in signature order.
// Example: DM_EV("save", "result:s n:d", "commit", n)
__attribute__((weak)) void (*g_dm_event_fn)(const char *ev, const char *sig, ...) = 0;
#define DM_EV(...) do { if (g_dm_event_fn) g_dm_event_fn(__VA_ARGS__); } while (0)
// 日志钩子（LOG / MLOG 用）：设了就由它整行写 stderr，并把带【红】/ 🔴 / FATAL 的行记成 red / fatal 事件（code = 格式串哈希，src = 文件:行）
// Log hook (used by LOG / MLOG): when set, it writes the whole line to stderr and records lines carrying 【红】 / 🔴 / FATAL as red / fatal events (code = hash of the format string, src = file:line)
__attribute__((weak)) void (*g_dm_log_hook)(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4))) = 0;
// 崩溃事件（crash.c 的信号处理里调）：不加锁、不分配；mod / off = pc 所在的客体模块与偏移（不在模块里 mod 为 NULL）
// Crash event (called in crash.c's signal handler): no locks, no allocation; mod / off = the guest module and offset of pc (mod is NULL outside the modules)
__attribute__((weak)) void (*g_dm_event_crash_fn)(int sig, uint64_t pc, const char *mod, uint64_t off, uint64_t addr) = 0;
// 局内拍号来源（diag.c 设）：返回当前拍号，不在局内返回 -1；设了以后局内的事件自动带 tick
// Source of the in-run tick (set by diag.c): returns the current tick, or -1 outside a run; once set, events in a run carry tick automatically
__attribute__((weak)) long long (*g_dm_event_tick_fn)(void) = 0;

// 读 DM_EVENTS 等开关、开好输出、填上面的指针；可重复调（只生效一次）。要在起别的线程之前调
// Reads DM_EVENTS and the other switches, opens the outputs and fills the pointers above; may be called repeatedly (takes effect once). Must be called before other threads start
void dm_events_init(void);
