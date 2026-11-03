// The Mac App's Recommended picture, worked out from this Mac's chip (dm_picrec.c). Pure C: the front end (front_mac.m) reads the machine,
// t_picrec feeds made-up machines.
#pragma once

typedef struct {
    const char *gpu;      // the GPU's Metal name: "Apple M6", "Apple M4 Pro", "Apple A18 Pro GPU"
    int cores;            // GPU cores (IORegistry gpu-core-count; 0 = unknown)
    int out_w, out_h;     // the picture at Native: the main screen's backing-store size in its current mode (out_w 16-aligned, 1600–3840)
    int mfx;              // MetalFX can enlarge on this GPU
} dm_mac_spec;

typedef struct {
    int native;           // 1 = Native output (the 3D enlarged by MetalFX); 0 = a fixed resolution (tier_res), when there is no MetalFX or Native is too heavy
    int tier_res;         // without Native: the Resolution row's fixed width, 0 = 1600, 1 = 1920, 2 = 2560
    int s3d;              // 3D Resolution row: 0 = 100%, 1 = 85%, 2 = 75%, 3 = 67%, 4 = 50%
    int envsh, shq;       // Environment Shadows (0 / 1), Shadow Quality (0 Standard, 1 High, 2 Ultra)
    int q3d;              // Frame Drops: 0 = lower the 3D resolution for a while, 1 = keep it
    int gen, tier;        // the chip as read: generation (0 = unknown), tier 0 base / 1 Pro / 2 Max / 3 Ultra; tier -1 = an iPhone chip (A)
    double speed;         // estimated GPU speed against the M6 the model is anchored on (1.0)
    double load;          // the recommendation's share of the GPU's estimated 60 fps capacity (0–1; above 1 = nothing lighter left)
} dm_mac_rec;

void dm_mac_recommend(const dm_mac_spec *s, dm_mac_rec *r);

// Whether a run gets the real recommendation (dm_mac_recommend, front_mac.m): only the Mac App (app = DM_APP set), and a test copy
// (app_support = DM_APP_SUPPORT set) only with test_firstboot (DM_TEST_VIDEO_FIRSTBOOT=1). The CLI host and test copies keep the original look
// for every picture key (windowed, 1600, Standard, no MetalFX), so automated runs, golden baselines and benchmarks don't move; runs that want
// MetalFX set DM_MFX / DM_MFX3D.
int dm_picrec_real(int app, int app_support, int test_firstboot);

// rec_id: which machine a recommendation was worked out for, a hash of everything dm_mac_recommend reads (the GPU name, its cores, the Native size,
// MetalFX). dm_video.c stores it with the recommendation and works Recommended out again at launch when it changes (another main screen, another Mac).
// 32-bit FNV-1a over the name's bytes (NULL = ""), then cores, out_w, out_h and mfx as 4 little-endian bytes each; the top bit is cleared (0..INT_MAX)
int dm_picrec_id(const dm_mac_spec *s);
