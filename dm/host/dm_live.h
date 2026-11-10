// Live hooks for the engine-analysis harness (dm_live.c): arm, disarm and read hooks on the game's code while it runs. Mac development runs
// only (DM_LIVE=1); on iOS every call is a no-op or refuses.
#pragma once
#include <stddef.h>
#include <stdint.h>

// One record per recorded hit, in the output file after its header. hook has DM_LIVE_RET set for a return record (x[0], x[1] = the return value,
// aux = the call's duration in timer ticks); for an entry or instruction record aux is 0
#define DM_LIVE_RET 0x80000000u
// A watchpoint record: hook = DM_LIVE_WATCH | its number; cap[0] = the address written, cap[1] = the writing instruction, cap[2] = the value after
#define DM_LIVE_WATCH 0x40000000u
// A tick marker from the host (no hook): cap[0] = the game's tick number as that tick starts
#define DM_LIVE_TICK 0x20000000u
// A phase marker: g_live_ab changed (cap[0] = its new value), written by the collector within 20 ms of the change
#define DM_LIVE_AB 0x10000000u
// A cost=1 hook's return record: cap[0], cap[1] = the instructions and cycles the call ran on its thread, net of the hooks; cap[2], cap[3] = as counted
typedef struct { uint32_t hook, tid; uint64_t t, lr, sp, x[4], cap[4], aux; } dm_live_rec;
// The output file starts with this header (DM_LIVE_OUT, default live.bin in the working folder)
typedef struct { char magic[8]; uint64_t base, size, timer_hz; uint32_t rec_size, version; } dm_live_hdr;

void dm_live_boot(void);   // after the modules are loaded: arms DM_LIVE_HOOKS (DM_LIVE=1 only)
void dm_live_ctl_init(void);   // registers the control port's live command
// The rest is the engine's interface for the control port and the unit test
int dm_live_attach(uint8_t *base, size_t size, uint64_t text_end, uint64_t patch_off, const char *name);
int dm_live_add(const char *spec, char *err, size_t ecap);   // "at=main+0x… kind=entry|insn|ret cap=… filt=… max=… every=… rate=… name=… [act=skip ret=… set=… retval=… ab=1] [cost=1] [tag=1|set] [nameof=…]"; returns the hook id or -1
int dm_live_del(int id);   // disarm (the trampoline stays: a thread may still be in it); -1 = every hook
void dm_live_flush(void);   // drain every thread's ring into the output file now
typedef struct { uint64_t hits, matched, recorded, dropped; int armed, disarmed_for_rate; uint64_t acted; } dm_live_counts;   // acted: hits on which the hook changed the game
int dm_live_counts_of(int id, dm_live_counts *out);
uint64_t dm_live_tramp(int id);   // the hook's trampoline address (tests check it is in branch range)
// The loader reserves the trampoline arena right after main, so every trampoline is in branch range of every instruction (DM_LIVE=1 only)
void dm_live_tick(uint64_t tick);   // the host's tick entry: a marker record with the tick number (nothing unless hooks are attached)
extern int g_live_ab;   // ab=1 hooks act only while this is non-zero (Mac)
void dm_live_thread_start(void);   // a new game thread: it gets the watchpoints that are set (thread.c)
size_t dm_live_arena_size(void);
void dm_live_set_arena(uint8_t *p, size_t sz);
