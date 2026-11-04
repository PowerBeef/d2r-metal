// GPU heap 0 readings (LIF-4, SHD-1). The game keeps every compressible texture (render targets, depth, the display textures) in heap 0, one pool
// carved out at device creation whose size the rewriter raises (768 MiB since product r49) and which never grows. Its allocator is first-fit over
// address-ordered blocks and the texture code never checks for a failed allocation, so a render target that finds no gap large enough crashes the game.
// What decides the next rebuild (a 3D Resolution step, a Frame Drops level, SMAA's first targets, a run's 16:9 <-> 16:10 switch) is therefore the
// largest free gap, not the total in use. This reads the heap's own bookkeeping once a second on the game main thread and reports what is used, the
// end of the highest live block (the high-water mark: the space above it is contiguous) and the largest free gap, as a log line and a `gpuheap` event
// whenever a value moved by 4 MiB or more, with one red line when the largest gap falls under a 64 MiB margin. Every read goes through
// vm_read_overwrite, so a wrong offset or an object being changed under the walk gives up that reading instead of crashing the host; the pool size is
// the positive control (768 MiB, the rewriter's own budget), and a mismatch turns the readings off for the launch.
// Game addresses and layout (private engine notes; checked on the CLI host 2026-11-03: pool 768 MiB, blocks in address order, each with its magic):
//   main+0x31df3a0   the graphics device pointer (the graphics context's device)
//   device+0x20      heap 0's object, embedded in the device: +0x40 its chunk list (a single chunk, the whole pool), +0x88 bytes used, +0xb8 pool size
//   chunk            +0x00 the pool's base address, +0x20 its size, +0x30 the first live block
//   block            +0x08 start, +0x18 next block, +0x28 bytes taken (alignment padding included), +0x38 magic 0xc001c0de
//   main+0x8524c4    the heap-0 budget the rewriter raises (patch_nso.py); main+0x17417a4 where a failed allocation faults
#include "dm_gpuheap.h"
#include "dm.h"
#include "hle.h"
#include <mach/mach.h>
#include <string.h>

#define HEAP0_POOL 0x30000000ull   // 768 MiB: the rewriter's heap-0 budget (patch_nso.py), the positive control
#define MIB (1024.0 * 1024.0)

static int rd(uint64_t a, void *buf, size_t n) { vm_size_t got = 0; return a && !vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)buf, &got) && got == n; }
static uint64_t main_base(void) { static uint64_t b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; return b; }

int dm_gpuheap_read(dm_gpuheap_info *o) {
    memset(o, 0, sizeof *o);
    uint64_t mb = main_base(), dev = 0, heap, chunk = 0, csz = 0, base = 0, blk = 0;
    if (!mb || !rd(mb + 0x31df3a0, &dev, 8) || !dev) return 0;
    heap = dev + 0x20;
    if (!rd(heap + 0xb8, &o->pool, 8) || !rd(heap + 0x88, &o->used, 8)) return -1;
    if (!o->pool) return 0;   // not created yet
    if (o->pool != HEAP0_POOL || o->used > o->pool) return -1;
    if (!rd(heap + 0x40, &chunk, 8) || !chunk) return 0;
    if (!rd(chunk + 0x00, &base, 8) || !rd(chunk + 0x20, &csz, 8) || !rd(chunk + 0x30, &blk, 8)) return 0;
    if (csz != o->pool) return -1;   // one chunk spanning the whole pool, as the notes say
    uint64_t end = base;   // the end of the previous block
    for (unsigned n = 0; blk; n++) {
        uint64_t b[8];
        if (n >= 16384 || !rd(blk, b, sizeof b) || (uint32_t)b[7] != 0xc001c0de) return 0;   // a block freed or moved under the walk: read again later
        uint64_t start = b[1], take = b[5];
        if (start < end || start + take > base + csz) return 0;   // out of address order: changed under the walk
        if (start - end > o->gap) o->gap = start - end;
        end = start + take; o->blocks++; blk = b[3];
    }
    o->top = end - base;
    if (base + csz - end > o->gap) o->gap = base + csz - end;
    return 1;
}

void dm_gpuheap_pump(void) {
    static unsigned n; static int off, warned; static dm_gpuheap_info last; static uint64_t pk_used, pk_top, min_gap = UINT64_MAX;
    if (off || n++ % 60) return;
    dm_gpuheap_info h; int r = dm_gpuheap_read(&h);
    if (r < 0) { off = 1; LOG("GPU heap 0: counters not as expected (pool %llu bytes, not the 768 MiB budget, or the block list unreadable): not reported", (unsigned long long)h.pool); return; }
    if (r == 0) return;
    if (h.used > pk_used) pk_used = h.used;
    if (h.top > pk_top) pk_top = h.top;
    if (h.gap < min_gap) min_gap = h.gap;
    const uint64_t step = 4ull << 20;
#define MOVED(f) (h.f > last.f ? h.f - last.f >= step : last.f - h.f >= step)
    if (last.pool && !MOVED(used) && !MOVED(top) && !MOVED(gap)) return;
#undef MOVED
    last = h;
    LOG("GPU heap 0: %.1f MiB used, the highest block ends at %.1f MiB, largest free gap %.1f MiB, of %.0f MiB (%u blocks; peaks: %.1f used, %.1f top, smallest gap %.1f)",
        h.used / MIB, h.top / MIB, h.gap / MIB, h.pool / MIB, h.blocks, pk_used / MIB, pk_top / MIB, min_gap / MIB);
    DM_EV("gpuheap", "used_mb:u top_mb:u gap_mb:u pool_mb:u blocks:u peak_used_mb:u peak_top_mb:u min_gap_mb:u", (unsigned long long)(h.used >> 20),
          (unsigned long long)(h.top >> 20), (unsigned long long)(h.gap >> 20), (unsigned long long)(h.pool >> 20), (unsigned long long)h.blocks,
          (unsigned long long)(pk_used >> 20), (unsigned long long)(pk_top >> 20), (unsigned long long)(min_gap >> 20));
    if (!warned && h.gap < (64ull << 20)) { warned = 1;
        LOG("【红】GPU heap 0: the largest free gap is %.1f MiB of %.0f MiB, under the 64 MiB margin: the next large render target may not fit (a failed allocation faults at main+0x17417a4)",
            h.gap / MIB, h.pool / MIB); }
}
