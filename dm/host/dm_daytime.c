// Test switch DM_TEST_DAYTIME=<angle>@<tick>: at tick <tick>, write that sun angle into every act's environment on the server and into the client's (to see noon /
// dusk / midnight; needs the tick-entry observation gate, attached by any of DM_C2S_LOG and the like). Writing only the client's copy gets synced back by the server.
// The game's environment: client current act = [main+0x2d20bd0], environment = [act + 8]; +0x0 int period index, +0x4 int period type, +0x8 float accumulated time,
// +0x2c int rate (sun angle = time / rate, 0-360 degrees), +0x34 int flags. The time periods are written the way the game's period advance (main+0x55a9d0) reads
// them, from its normal period table (main+0x1dab370). The server's game object: +0x170 tick, +0x190 the acts.
#include "dm.h"
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd(uint64_t a, void *buf, size_t n) { vm_size_t got = 0; return a && !vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)buf, &got) && got == n; }
static uint64_t main_base(void) { static uint64_t b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; return b; }
static uint64_t env_ptr(void) { uint64_t act = 0, env = 0, mb = main_base(); return mb && rd(mb + 0x2d20bd0, &act, 8) && rd(act + 8, &env, 8) ? env : 0; }

void dm_daytime_server_tick(uint64_t G) {   // server per-tick entry (diag.c, while the tick-entry observation gate is attached)
    static int ta = -2; static unsigned at;
    if (ta == -2) { const char *e = getenv("DM_TEST_DAYTIME"); ta = -1; if (e) { int a; unsigned t; if (sscanf(e, "%d@%u", &a, &t) == 2) { ta = a; at = t; } } }
    uint32_t tick = 0; if (ta < 0 || !rd(G + 0x170, &tick, 4) || tick < at) return;
    uint64_t acts = 0; int n = 0; rd(G + 0x190, &acts, 8);
    // The game's period advance (main+0x55a9d0) moves one period at a time, and once the time passes the next period's start it is pulled back to that start:
    // write both the period index (+0) and the period type (+4). The period table is the game's normal one (main+0x1dab370, 6 entries of start angle, type,
    // light color): the last entry whose start is at or below the target angle
    int idx = -1, kind = 0; uint64_t mb = main_base(); int32_t tbl[18];
    if (mb && rd(mb + 0x1dab370, tbl, sizeof tbl)) { int best = -1; for (int i = 0; i < 6; i++) if (tbl[3 * i] <= ta && tbl[3 * i] > best) { best = tbl[3 * i]; idx = i; kind = tbl[3 * i + 1]; } }
    uint64_t envs[6]; int ne = 0;
    for (int i = 0; acts && i < 5; i++) { uint64_t act = 0, env = 0; if (rd(acts + 8 * i, &act, 8) && rd(act + 8, &env, 8)) envs[ne++] = env; }
    uint64_t ce = env_ptr(); if (ce) envs[ne++] = ce;
    for (int i = 0; i < ne; i++) { int32_t rate = 0, fl = 0; if (!rd(envs[i] + 0x2c, &rate, 4) || rate <= 0 || !rd(envs[i] + 0x34, &fl, 4) || fl) continue;   // environments with the special flag set are left alone
        if (idx >= 0) { *(int32_t *)(uintptr_t)envs[i] = idx; *(int32_t *)(uintptr_t)(envs[i] + 4) = kind; }
        *(float *)(uintptr_t)(envs[i] + 8) = (float)(ta * rate); n++; }
    LOG("DM_TEST_DAYTIME: tick %u: wrote %d environments (the server's acts + the client's%s) with sun angle %d, period %d / type %d", tick, n, ce ? "" : ", which could not be read", ta, idx, kind);
    ta = -1;
}
