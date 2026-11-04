// GPU heap 0 readings (dm_gpuheap.c): how full the game's render-target heap is and the largest allocation that still fits. Mac and iOS.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t pool;     // the heap's size (page-rounded): 768 MiB with the rewriter's budget
    uint64_t used;     // bytes in live blocks, as the heap counts them
    uint64_t top;      // the end of the highest live block, from the pool's start (first-fit high-water mark)
    uint64_t gap;      // the largest free gap between live blocks or above the highest one: the largest allocation that still fits
    uint64_t big;      // the largest live block (the largest single target a rebuild makes again)
    unsigned blocks;   // live blocks walked
} dm_gpuheap_info;

// One reading: 1 = valid, 0 = the device or the heap is not built yet, or a block changed under the walk (read again later), -1 = the counters are not
// as expected (the pool is not the 768 MiB budget, or the layout does not match): readings are then off for good. Any thread; reads only
int dm_gpuheap_read(dm_gpuheap_info *o);
// Game main thread, every frame (dm_uiscreen_pump): reads once every 60 calls, logs and emits a gpuheap event when a value moved by 4 MiB or more,
// and logs one red line when the largest free gap falls under 64 MiB
void dm_gpuheap_pump(void);
