// Live hooks for the engine-analysis harness (dm_live.c): arm, disarm and read hooks on the game's code while it runs. Mac development runs
// only (DM_LIVE=1); on iOS every call is a no-op or refuses.
#pragma once
#include <stddef.h>
#include <stdint.h>

// One record per recorded hit, in the output file after its header. hook has DM_LIVE_RET set for a return record (x[0], x[1] = the return value,
// aux = the call's duration in timer ticks); for an entry or instruction record aux is 0
#define DM_LIVE_RET 0x80000000u
typedef struct { uint32_t hook, tid; uint64_t t, lr, sp, x[4], cap[4], aux; } dm_live_rec;
// The output file starts with this header (DM_LIVE_OUT, default live.bin in the working folder)
typedef struct { char magic[8]; uint64_t base, size, timer_hz; uint32_t rec_size, version; } dm_live_hdr;

void dm_live_boot(void);   // after the modules are loaded: arms DM_LIVE_HOOKS (DM_LIVE=1 only)
void dm_live_ctl_init(void);   // registers the control port's live command
// The rest is the engine's interface for the control port and the unit test
int dm_live_attach(uint8_t *base, size_t size, uint64_t text_end, uint64_t patch_off, const char *name);
int dm_live_add(const char *spec, char *err, size_t ecap);   // "at=main+0x… kind=entry|insn|ret cap=… filt=… max=… every=… rate=… name=…"; returns the hook id or -1
int dm_live_del(int id);   // disarm (the trampoline stays: a thread may still be in it); -1 = every hook
void dm_live_flush(void);   // drain every thread's ring into the output file now
typedef struct { uint64_t hits, matched, recorded, dropped; int armed, disarmed_for_rate; } dm_live_counts;
int dm_live_counts_of(int id, dm_live_counts *out);
uint64_t dm_live_tramp(int id);   // the hook's trampoline address (tests check it is in branch range)
// The loader reserves the trampoline arena right after main, so every trampoline is in branch range of every instruction (DM_LIVE=1 only)
size_t dm_live_arena_size(void);
void dm_live_set_arena(uint8_t *p, size_t sz);
