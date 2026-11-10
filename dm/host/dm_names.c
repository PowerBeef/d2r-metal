// What the game calls its things: the model file of each render entity and the file of each texture, read from the game's own tables and
// written as a table for the analysis tools (the live harness's draw log carries entity ids and texture pointers: dm/tools/re/live.py draws
// joins them with this). Read-only; nothing here is kept across calls. Every read goes through vm_read, so a table changing under the loader
// thread gives a short or odd row at worst, never a fault.
// Game addresses relied on (3.3.94012 main), checked on the running game with the live harness on 2026-11-10 (camp, Den of Evil):
//   main+0x3109fd8  pointer to the component registry; the pool of component type T is the pointer at registry + T x 0x28 + 0x20
//   pool: +0x20 / +0x28 = begin / end of the entity ids (u32 each, in the components' order), +0x38 = the components, contiguous
//   main+0x2de0c50  type number of the model component (0x88 bytes): +0x00 = the entity's model instance; instance +0x20 = the model resource
//   model resource: +0xe8 = its path, a string {text pointer, length, capacity}
//   main+0x320e610  bucket count of the texture table (a power of two), main+0x320e618 the buckets (a pointer each)
//   table entry: +0x00 next in the bucket, +0x08 the path (the same string form), +0xc0 the texture object
//   texture object: +0x08 device texture, +0x18 image view, +0xf0 the view bound now, +0xf8 the full view
#include "dm.h"
#include "dm_names.h"
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a > 0x10000 && !vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) && got == n; }
static int str_at(uint64_t at, char *out, size_t cap) {   // the game's string {text, length, capacity}: printable text of 1 to cap - 1 bytes, else 0
    uint64_t s[2] = {0, 0}; out[0] = 0;
    if (!rd(at, s, 16) || !s[1] || s[1] >= cap || !rd(s[0], out, (size_t)s[1])) return 0;
    out[s[1]] = 0;
    for (size_t i = 0; i < s[1]; i++) if ((unsigned char)out[i] < 0x20 || out[i] == 0x7f) { out[0] = 0; return 0; }
    return 1;
}
int dm_names_dump(const char *path, int *models, int *textures) {
    uint64_t base = 0, reg = 0; int nm = 0, nt = 0; char name[512];
    if (models) *models = 0; if (textures) *textures = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = (uint64_t)g_mod[i].base;
    if (!base || !rd(base + 0x3109fd8, &reg, 8) || !reg) return -1;
    FILE *f = fopen(path, "w"); if (!f) return -1;
    fputs("kind\tkey\tname\n", f);
    // models: each entity of the model pool -> instance -> resource -> path
    uint32_t t = 0; uint64_t pool = 0, be[2] = {0, 0}, data = 0;
    if (rd(base + 0x2de0c50, &t, 4) && t <= 4096 && rd(reg + (uint64_t)t * 0x28 + 0x20, &pool, 8) && rd(pool + 0x20, be, 16) && rd(pool + 0x38, &data, 8) && be[1] >= be[0]) {
        uint32_t n = (uint32_t)((be[1] - be[0]) / 4); if (n > 1u << 18) n = 1u << 18;
        uint32_t *ids = malloc((size_t)n * 4 + 4);
        if (ids && n && rd(be[0], ids, (size_t)n * 4))
            for (uint32_t i = 0; i < n; i++) { uint64_t inst = 0, res = 0;
                if (rd(data + (uint64_t)i * 0x88, &inst, 8) && rd(inst + 0x20, &res, 8) && str_at(res + 0xe8, name, sizeof name)) { fprintf(f, "model\t%x\t%s\n", ids[i], name); nm++; } }
        free(ids);
    }
    // textures: every entry of the table, under each pointer a draw or a UI item can carry for it
    uint64_t count = 0, buckets = 0;
    if (rd(base + 0x320e610, &count, 8) && rd(base + 0x320e618, &buckets, 8) && count && count <= 1u << 20)
        for (uint64_t b = 0; b < count; b++) { uint64_t e = 0;
            if (!rd(buckets + 8 * b, &e, 8)) break;
            for (int k = 0; e && k < 4096; k++) { uint64_t next = 0, tex = 0, p[4] = {0, 0, 0, 0};
                if (!rd(e, &next, 8) || !rd(e + 0xc0, &tex, 8)) break;
                if (tex && str_at(e + 8, name, sizeof name)) {
                    fprintf(f, "texture\t%llx\t%s\n", (unsigned long long)tex, name); nt++;
                    rd(tex + 0x08, &p[0], 8); rd(tex + 0x18, &p[1], 8); rd(tex + 0xf0, &p[2], 16);
                    for (int i = 0; i < 4; i++) { int dup = !p[i]; for (int j = 0; j < i; j++) dup |= p[j] == p[i]; if (!dup) fprintf(f, "texture\t%llx\t%s\n", (unsigned long long)p[i], name); }
                }
                e = next; }
        }
    fclose(f);
    if (models) *models = nm; if (textures) *textures = nt;
    return nm + nt;
}
