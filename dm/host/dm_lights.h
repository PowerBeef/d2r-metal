// The frame's lights, read from the game's own component pools (dm_lights.c): the point, spot and unit lights with their world position, colour, power,
// radius and shadow use. For tests today (the control port's `lights` and the light counts in `state`) and as the base for ray-traced local lights.
#pragma once
#include <stdint.h>

enum { DM_LIGHT_POINT, DM_LIGHT_SPOT, DM_LIGHT_UNIT };
typedef struct {
    int type;            // DM_LIGHT_*
    uint32_t entity;     // the light's entity in the game's registry
    float pos[3];        // world position (x and z = 2 x the tile coordinate + 1, y = height)
    float colour[3];     // the game's stored colour, already multiplied by `power`
    float power;         // point and unit lights: power / 4 pi; spot lights: power over the cone's solid angle
    float radius;        // radius (point, unit) or range (spot), in world units
    int shadow;          // 1 = it has a shadow slot (a spot's shadow map, a unit light's cube)
    int grid;            // unit lights: 1 = the game put it in its coarse light grid instead of the screen tiles
} dm_light;
typedef struct { int point, spot, unit, parked, player; } dm_lights_info;   // parked = unit lights kept at the world origin (not in play); player = a unit light sits at the player

// Reads up to cap lights into out (out may be NULL to count). px / py = the player's tile, or negative when not in a game. Returns the number of lights in play
// (parked ones are left out), or -1 when the game has no light pools yet. Any thread: every read is a checked copy, so a pool changing underneath gives a short or stale list, never a fault.
int dm_lights_read(dm_light *out, int cap, int px, int py, dm_lights_info *info);
