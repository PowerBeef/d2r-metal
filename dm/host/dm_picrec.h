// The Mac App's Recommended picture, worked out from this Mac's chip (dm_picrec.c). Pure C: the front end (front_mac.m) reads the machine,
// t_picrec feeds made-up machines.
#pragma once

typedef struct {
    const char *gpu;      // the GPU's Metal name: "Apple M6", "Apple M4 Pro", "Apple A18 Pro GPU"
    int cores;            // GPU cores (IORegistry gpu-core-count; 0 = unknown)
    int out_w, out_h;     // the in-game picture at Native (front_mac.m native_picture): the main screen's backing store in its current mode, below the notch,
                          // in the picture's shape (out_w even, 1600–3840, within the heap cap DM_SCR_MAX_PX)
    int mfx;              // MetalFX can enlarge on this GPU
    double aspect;        // the game picture's aspect (16:10 on a 16:10 MacBook, true full screen); 0 = 16:9. A fixed resolution takes this shape too
    int drw_w, drw_h;     // where the present draws the Native picture in full screen: its viewport on the main screen's drawable, in pixels (front_mac.m
                          // mac_rec); larger than out_w x out_h past the heap cap (a 5K display: 3840 x 2160 into 5120 x 2880); 0 = the picture's own size
    double pace_hz;       // the frame rate on the main screen (dm_pace_hz(refresh, 60): 60 on 60 and 120 Hz screens, 72 on 144 Hz, 100 on 100 Hz, 50 on
                          // 50 Hz); 0 = 60
} dm_mac_spec;

typedef struct {
    int native;           // 1 = Native output (the 3D enlarged by MetalFX); 0 = a fixed resolution (tier_res), when there is no MetalFX or Native is too heavy
    int tier_res;         // without Native: the Resolution row's fixed width, 0 = 1600, 1 = 1920, 2 = 2560
    int s3d;              // 3D Resolution row: 0 = 100%, 1 = 85%, 2 = 75%, 3 = 67%, 4 = 50%
    int envsh, shq;       // Environment Shadows (0 / 1), Shadow Quality (0 Standard, 1 High, 2 Ultra)
    int q3d;              // Frame Drops: 0 = lower the 3D resolution for a while, 1 = keep it
    int gen, tier;        // the chip as read: generation (0 = unknown), tier 0 base / 1 Pro / 2 Max / 3 Ultra; tier -1 = an iPhone chip (A)
    double speed;         // estimated GPU speed against the M6 the model is anchored on (1.0)
    double load;          // the recommendation's share of the GPU's estimated capacity at the screen's frame rate (0–1; above 1 = nothing lighter left)
    double pres;          // the load units of the present's enlargement counted in it (Native picture smaller than its viewport; 0 otherwise)
} dm_mac_rec;

void dm_mac_recommend(const dm_mac_spec *s, dm_mac_rec *r);

// Whether a run gets the real recommendation (dm_mac_recommend, front_mac.m): only the Mac App (app = DM_APP set), and a test copy
// (app_support = DM_APP_SUPPORT set) only with test_firstboot (DM_TEST_VIDEO_FIRSTBOOT=1). The CLI host and test copies keep the original look
// for every picture key (windowed, 1600, Standard, no MetalFX), so automated runs, golden baselines and benchmarks don't move; runs that want
// MetalFX set DM_MFX / DM_MFX3D.
int dm_picrec_real(int app, int app_support, int test_firstboot);

// rec_id: which machine a recommendation was worked out for, a hash of everything dm_mac_recommend reads (the GPU name, its cores, the Native size,
// MetalFX, the present's enlargement, the frame rate). dm_video.c stores it with the recommendation and works Recommended out again at launch when it
// changes (another main screen, another display mode or refresh rate, another Mac). 32-bit FNV-1a over the name's bytes (NULL = ""), then cores,
// out_w, out_h and mfx as 4 little-endian bytes each; then, only where they change the model's input, drw_w and drw_h (when the present enlarges
// the picture, dm_picrec_enlarges) and the frame rate in hundredths (when it is not 60), 4 bytes each, so a plain 60 Hz machine keeps its id; the
// top bit is cleared (0..INT_MAX)
int dm_picrec_id(const dm_mac_spec *s);
// Whether the present enlarges the Native picture into its viewport (drw_w x drw_h): more than 5% larger both ways, the present's own rule for
// enlarging (nvn_mtl.m mfx_want)
int dm_picrec_enlarges(const dm_mac_spec *s);
