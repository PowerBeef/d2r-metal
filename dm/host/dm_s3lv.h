// Frame Drops levels: the 3D scales the dynamic 3D scale controller (hle_oe.c) steps down to below the chosen 3D Resolution. A pure header, so the
// Recommended model's tests (t_picrec, invariant I3) and the settings code read the levels the game really has without linking hle_oe.c.
// Levels are up to S3_LEVELS steps of x S3_STEP below the configured scale (about 13.5% fewer pixels each), rounded to hundredths and never below
// S3_MIN: 75% -> 0.70 / 0.65 / 0.60, 100% -> 0.93 / 0.86 / 0.80, 85% -> 0.79 / 0.74 / 0.68, 67% -> 0.62 / 0.58 / 0.54, 50% -> none.
// dm_picrec.c LIGHT_MIN (the light picture's lowest 3D Resolution, 67%) comes with Frame Drops = Lower and relies on at least two levels below it
// (t_picrec MIN_DROP_LEVELS): change these constants together with it.
#pragma once
#include <math.h>
#define S3_STEP 0.93f
#define S3_LEVELS 3
#define S3_MIN 0.50f
// The first level at a chosen 3D Resolution of 95% or more is step S3_FIRST_AT_FULL: 1 = 0.93 (at 100% the MetalFX 3D pass upscales by 1.075),
// 2 = 0.86 / 0.80 / 0.75. Set by the wave-2 exit ABBA (first-step saving on this Mac's Recommended); a build define can override it for tests
#ifndef S3_FIRST_AT_FULL
#define S3_FIRST_AT_FULL 1
#endif
static inline int dm_s3_k0(float cfg) { return cfg >= 0.95f ? S3_FIRST_AT_FULL - 1 : 0; }
// Level lv's scale rounded to hundredths, before the S3_MIN clamp (lv >= 1)
static inline float dm_s3_rounded(float cfg, int lv) { return roundf(cfg * powf(S3_STEP, (float)(lv + dm_s3_k0(cfg))) * 100) / 100; }
// The scale of level lv (0 = the configured value)
static inline float dm_s3_level_scale(float cfg, int lv) {
    if (lv <= 0) return cfg;
    float v = dm_s3_rounded(cfg, lv);
    return v < S3_MIN ? S3_MIN : v;
}
// The deepest level: levels 1, 2, ... while the scale the level runs at (rounded, as dm_s3_level_scale gives it) stays at or above the floor, so a
// floor equal to a level's scale keeps that level. floor_env > 0 is DM_3D_SCALE_FLOOR (device sweeps): that floor (never below S3_MIN) and up to 12
// levels; otherwise S3_MIN and S3_LEVELS
static inline int dm_s3_max_level(float cfg, float floor_env) {
    float fl = floor_env > 0 ? (floor_env > S3_MIN ? floor_env : S3_MIN) : S3_MIN; int cap = floor_env > 0 ? 12 : S3_LEVELS, n = 0;
    for (int lv = 1; lv <= cap && dm_s3_rounded(cfg, lv) >= fl - 1e-4f; lv++) n = lv;
    return n;
}
// How many Frame Drops levels a chosen 3D Resolution has (0 at 50%: Lower can do nothing there)
static inline int dm_3d_scale_levels(float cfg) { return dm_s3_max_level(cfg, 0); }
