// The translation thread's shortcuts (nvn_mtl.m): the pools a thread remembers (mtl_gpu_addr), the driver-segment blocks reused within a
// submission (drv_block, g_tab_gen), and the argument-table writes that are skipped when they repeat (at_same). Each must give what the plain
// path gives. No GPU work is submitted.
#include "../nvn/nvn_mtl.m"
#include <pthread.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (uint64_t)(want), g = (uint64_t)(got); count++; printf("%s: expected %llu, got %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w == g ? "ok" : "FAILED"); fail += w != g; } while (0)
static uint8_t *pages(size_t n) { uint8_t *m; if (posix_memalign((void **)&m, getpagesize(), n)) abort(); memset(m, 0, n); return m; }
static void *other_thread(void *arg) { void *b; uint64_t o, g; return (void *)(intptr_t)mtl_gpu_addr((uint64_t)(uintptr_t)arg, 16, &b, &o, &g); }
static int on_other_thread(const uint8_t *addr) { pthread_t t; void *r; pthread_create(&t, NULL, other_thread, (void *)addr); pthread_join(t, &r); return (int)(intptr_t)r; }

static void pool_cases(void) {
    size_t pg = (size_t)getpagesize(); uint8_t *a = pages(4 * pg), *b = pages(4 * pg); void *buf, *buf2; uint64_t off, gpu, off2, gpu2;
    g_poolfast = 1;
    EXPECT("an address in no pool is not found", 0, mtl_gpu_addr((uint64_t)(uintptr_t)a, 16, &buf, &off, &gpu));
    mtl_pool_add(a, 4 * pg);
    EXPECT("after the pool is added it is found", 1, mtl_gpu_addr((uint64_t)(uintptr_t)a + 100, 16, &buf, &off, &gpu)); EXPECT("at its offset", 100, off);
    EXPECT("asked again (remembered): found", 1, mtl_gpu_addr((uint64_t)(uintptr_t)a + 200, 16, &buf2, &off2, &gpu2));
    EXPECT("the same buffer", 1, buf2 == buf); EXPECT("the new offset", 200, off2); EXPECT("the address follows the offset", 100, gpu2 - gpu);
    EXPECT("a range that runs past the pool's end is not found", 0, mtl_gpu_addr((uint64_t)(uintptr_t)a + 4 * pg - 8, 16, &buf2, &off2, &gpu2));
    g_poolfast = 2; uint64_t bad0 = atomic_load(&g_pool_fast_bad);
    EXPECT("checked against the table: found", 1, mtl_gpu_addr((uint64_t)(uintptr_t)a + 300, 16, &buf2, &off2, &gpu2)); EXPECT("and the two answers agree", 0, atomic_load(&g_pool_fast_bad) - bad0);
    g_poolfast = 1;
    mtl_pool_add(b, 4 * pg);
    EXPECT("a second pool is found", 1, mtl_gpu_addr((uint64_t)(uintptr_t)b + 8, 16, &buf2, &off2, &gpu2)); EXPECT("with its own buffer", 1, buf2 != buf);
    EXPECT("and the first one still is", 1, mtl_gpu_addr((uint64_t)(uintptr_t)a + 8, 16, &buf2, &off2, &gpu2));
    mtl_pool_del(a);
    EXPECT("a removed pool is no longer found, though this thread remembered it", 0, mtl_gpu_addr((uint64_t)(uintptr_t)a + 100, 16, &buf2, &off2, &gpu2));
    EXPECT("nor from another thread", 0, on_other_thread(a + 100));
    EXPECT("the other pool is still found", 1, mtl_gpu_addr((uint64_t)(uintptr_t)b + 8, 16, &buf2, &off2, &gpu2));
    EXPECT("and from another thread", 1, on_other_thread(b + 8));
    mtl_pool_add(a, 4 * pg);
    EXPECT("added again at the same place: found", 1, mtl_gpu_addr((uint64_t)(uintptr_t)a + 100, 16, &buf2, &off2, &gpu2)); EXPECT("at its offset", 100, off2);
    g_poolfast = 0;
    EXPECT("DM_NVN_POOLFAST=0: the table answers", 1, mtl_gpu_addr((uint64_t)(uintptr_t)a + 100, 16, &buf, &off, &gpu)); EXPECT("the same buffer as the remembered answer", 1, buf == buf2);
    g_poolfast = 1; mtl_pool_del(a); mtl_pool_del(b);
}
// The block as it was first written: all 0xA00 bytes zeroed, then each table entry's low 32 bits at its place
static void drv_ref(uint8_t *d, int st, int bank) {
    memset(d, 0, 0xA00);
    if (bank == 0) { int base = st == 0 ? 0x110 : st == 5 ? 0x310 : 0x510; for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[st][r].addr; uint32_t n = (uint32_t)S.ssbo[st][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); } }
    else for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[st][r], sh = (uint32_t)S.samp[st][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh, 4); }
}
static void drv_cases(void) {
    state_reset(); uint64_t x = 88172645463325252ull;
    for (int st = 0; st < 6; st++) { for (int r = 0; r < 128; r++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; S.tex[st][r] = x; x ^= x << 13; x ^= x >> 7; x ^= x << 17; S.samp[st][r] = x; }
                                     for (int r = 0; r < 16; r++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; S.ssbo[st][r].addr = x; S.ssbo[st][r].size = x >> 20; } }
    int same = 1; uint8_t now[0xA00], ref[0xA00];
    for (int st = 0; st < 6; st++) for (int bank = 0; bank <= 2; bank += 2) { if (bank == 0 && st != 0 && st != 1 && st != 5) continue; memset(now, 0xee, sizeof now); drv_fill(now, st, bank); drv_ref(ref, st, bank); same &= !memcmp(now, ref, sizeof now); }
    EXPECT("a block's bytes equal the first way of writing them, every stage and both banks", 1, same);
    // generations: a bind that changes a table bumps it, the same bind again does not
    uint32_t g0 = g_tab_gen[1][1], other0 = g_tab_gen[0][1]; p_bind_handles bh = {.stage = 1, .first = 3, .n = 2}; bh.h[0] = 111; bh.h[1] = 222;
    state_op(OP_BIND_TEX, &bh); EXPECT("a texture bind that changes the table bumps its generation", 1, g_tab_gen[1][1] - g0);
    state_op(OP_BIND_TEX, &bh); EXPECT("the same bind again does not", 1, g_tab_gen[1][1] - g0);
    state_op(OP_BIND_SAMP, &bh); EXPECT("a sampler bind that changes the table bumps it", 2, g_tab_gen[1][1] - g0);
    EXPECT("another stage's generation stays", 0, g_tab_gen[0][1] - other0);
    uint32_t s0 = g_tab_gen[1][0]; p_bind_buf bb = {.stage = 1, .index = 2, .addr = 0x4000, .size = 64};
    state_op(OP_BIND_SSBO, &bb); EXPECT("a storage buffer bind that changes the table bumps the other generation", 1, g_tab_gen[1][0] - s0); EXPECT("and not the texture one", 2, g_tab_gen[1][1] - g0);
    uint32_t u0 = g_tab_gen[1][1]; p_bind_buf ub = {.stage = 1, .index = 0, .addr = 0x5000, .size = 64};
    state_op(OP_BIND_UBO, &ub); EXPECT("a uniform buffer bind bumps neither", 0, (g_tab_gen[1][1] - u0) + (g_tab_gen[1][0] - s0 - 1));
    args_dirty_all(); EXPECT("a whole-state restore bumps every generation", 1, g_tab_gen[1][1] - u0);
    // reuse within a submission
    g_drvcache = 1; arena_begin(); g_sub_serial = 500; uint64_t b0 = g_stat_drv_built, r0 = g_stat_drv_reused;
    uint64_t gp = drv_block(1, 2), gp2 = drv_block(1, 2);
    EXPECT("the same tables in the same submission: the block is used again", 1, gp2 == gp); EXPECT("one built, one reused", 1, (g_stat_drv_built - b0 == 1) && (g_stat_drv_reused - r0 == 1));
    bh.h[0] = 333; state_op(OP_BIND_TEX, &bh); uint64_t gp3 = drv_block(1, 2);
    EXPECT("after a texture bind: a new block", 1, gp3 != gp); EXPECT("holding the new handle", 333, *(const uint32_t *)(g_drv_last[1][1].at + 0x168 + 8 * 3));
    EXPECT("the storage buffer block is its own", 1, drv_block(1, 0) != gp3);
    g_sub_serial = 501; uint64_t gp4 = drv_block(1, 2); EXPECT("the next submission builds its own block", 1, gp4 != gp3);
    g_drvcache = 2; uint64_t bad0 = g_drv_bad; drv_block(1, 2); EXPECT("checked reuse: the bytes agree", 0, g_drv_bad - bad0);
    g_drvcache = 0; b0 = g_stat_drv_built; drv_block(1, 2); drv_block(1, 2); EXPECT("DM_NVN_DRVCACHE=0: built every time", 2, g_stat_drv_built - b0);
    g_drvcache = 1;
}
static void at_cases(void) {
    g_atdedup = 1; g_at_set[0] = g_at_set[1] = 0;
    EXPECT("the first address at an index is set", 0, at_same(0, 16, 0x1000)); EXPECT("the same address again is skipped", 1, at_same(0, 16, 0x1000));
    EXPECT("another address is set", 0, at_same(0, 16, 0x2000)); EXPECT("the other table has its own memory", 0, at_same(1, 16, 0x2000));
    EXPECT("another index has its own", 0, at_same(0, 17, 0x2000)); EXPECT("address 0 is an address like any other", 0, at_same(0, 3, 0)); EXPECT("and is skipped when it repeats", 1, at_same(0, 3, 0));
    EXPECT("an index past the table's memory is always set", 0, at_same(0, 40, 0x1000) + at_same(0, 40, 0x1000));
    g_atdedup = 0; at_same(0, 16, 0x7000); EXPECT("DM_NVN_ATDEDUP=0: always set", 0, at_same(0, 16, 0x7000)); g_atdedup = 1;
    EXPECT("what was set while it was off is remembered when it is on again", 1, at_same(0, 16, 0x7000));
}
int main(void) { @autoreleasepool { setvbuf(stdout, NULL, _IONBF, 0); mtl_init();
    pool_cases(); drv_cases(); at_cases();
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0; } }
