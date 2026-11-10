// The frame's lights, read from the game's own component pools. Read-only; nothing here is kept across calls.
// Game addresses relied on (3.3.94012 main), found with the live harness on 2026-11-10 (camp, Blood Moor, Den of Evil):
//   main+0x3109fd8  pointer to the component registry; the pool of component type T is the pointer at registry + T x 0x28 + 0x20
//   pool: +0x20 / +0x28 = begin / end of the entity ids (u32 each, in the components' order), +0x38 = the components, contiguous
//   type numbers (u32, given out as each type is first used): main+0x2d52b10 transform, main+0x2e2dc10 point light, main+0x2e2ddd0 spot light,
//     main+0x2e2d028 unit light (the light a unit carries: the player's, monsters', missiles')
//   transform (0xd0 bytes): +0x70 = world position (the translation of the world matrix at +0x40)
//   point light (0x38): +0x0c power / 4 pi, +0x10 colour x that, +0x1c radius, +0x30 entity
//   spot light (0x4c): +0x0c power over the cone's solid angle (not a number when the cone is empty: point-light entities carry such a component too),
//     +0x10 colour x that, +0x1c range, +0x20 shadow slot (-1 = none), +0x40 entity
//   unit light (0x54): +0x0c power / 4 pi, +0x10 colour x that, +0x1c radius, +0x2c cube-shadow index (-1 = none), +0x3d grid flag, +0x40 entity
// A light's component holds no position: it is the transform of the same entity. World x and z are 2 x the tile + 1 on flat ground; a thing above the
// ground is shifted by the same amount in both (read 2026-11-10 in nine areas).
// A light's component holds no position either way: see the transform. Six unit lights sit at the world origin in every scene read (their units are
// not in play); they are counted as parked and left out.
#include "dm.h"
#include "dm_lights.h"
#include <mach/mach.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a > 0x10000 && !vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) && got == n; }
typedef struct { uint64_t ids, data; uint32_t n; } pool_t;
static int pool_of(uint64_t base, uint64_t reg, uint64_t type_at, pool_t *p) {
    uint32_t t = 0; uint64_t pool = 0, be[2] = {0, 0};
    if (!rd(base + type_at, &t, 4) || t > 4096 || !rd(reg + (uint64_t)t * 0x28 + 0x20, &pool, 8) || !rd(pool + 0x20, be, 16) || !rd(pool + 0x38, &p->data, 8)) return 0;
    if (be[1] < be[0] || (be[1] - be[0]) / 4 > 1u << 20) return 0;
    p->ids = be[0]; p->n = (uint32_t)((be[1] - be[0]) / 4); return 1;
}
static float f32(const uint8_t *b, int o) { float v; memcpy(&v, b + o, 4); return v; }
static int32_t i32(const uint8_t *b, int o) { int32_t v; memcpy(&v, b + o, 4); return v; }

int dm_lights_read(dm_light *out, int cap, int px, int py, dm_lights_info *info) {
    dm_lights_info in = {0}; if (info) *info = in;
    uint64_t base = 0, reg = 0; pool_t xf;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = (uint64_t)g_mod[i].base;
    if (!base || !rd(base + 0x3109fd8, &reg, 8) || !pool_of(base, reg, 0x2d52b10, &xf) || !xf.n) return -1;
    uint32_t *ids = malloc((size_t)xf.n * 4); if (!ids) return -1;
    if (!rd(xf.ids, ids, (size_t)xf.n * 4)) { free(ids); return -1; }
    static const struct { uint64_t type_at; int size, ent, shadow; } k[3] = { {0x2e2dc10, 0x38, 0x30, -1}, {0x2e2ddd0, 0x4c, 0x40, 0x20}, {0x2e2d028, 0x54, 0x40, 0x2c} };
    int n = 0;
    for (int t = 0; t < 3; t++) {
        pool_t p; if (!pool_of(base, reg, k[t].type_at, &p)) continue;
        for (uint32_t i = 0; i < p.n && i < 1024; i++) {
            uint8_t c[0x54]; if (!rd(p.data + (uint64_t)i * (unsigned)k[t].size, c, (size_t)k[t].size)) break;
            dm_light l = { .type = t, .entity = (uint32_t)i32(c, k[t].ent), .power = f32(c, 0x0c), .radius = f32(c, 0x1c) };
            if (!isfinite(l.power) || l.power <= 0 || !isfinite(l.radius) || l.radius <= 0) continue;   // an empty spot cone, or a slot the game left blank
            uint32_t j = 0; while (j < xf.n && ids[j] != l.entity) j++;
            if (j == xf.n || !rd(xf.data + (uint64_t)j * 0xd0 + 0x70, l.pos, 12)) continue;
            for (int a = 0; a < 3; a++) l.colour[a] = f32(c, 0x10 + 4 * a);
            l.shadow = i32(c, k[t].shadow < 0 ? 0x28 : k[t].shadow) >= 0 && k[t].shadow >= 0; l.grid = t == DM_LIGHT_UNIT && c[0x3d] != 0;
            if (t == DM_LIGHT_UNIT && fabsf(l.pos[0]) < 8 && fabsf(l.pos[2]) < 8) { in.parked++; continue; }
            // The player's own light: at the player's tile on flat ground; on raised ground it is shifted by the same amount in x and z (its height
            // along the view diagonal: +8 at height 15 in Lower Kurast, +16 at height 21 in Harrogath), so x - z still equals the tile's and x stays near
            if (t == DM_LIGHT_UNIT && px >= 0 && fabsf((l.pos[0] - l.pos[2]) - 2.0f * (float)(px - py)) < 4 && fabsf(l.pos[0] - (2.0f * (float)px + 1)) < 48) in.player = 1;
            if (t == DM_LIGHT_POINT) in.point++; else if (t == DM_LIGHT_SPOT) in.spot++; else in.unit++;
            if (out && n < cap) out[n] = l;
            n++;
        }
    }
    free(ids); if (info) *info = in;
    return n;
}
