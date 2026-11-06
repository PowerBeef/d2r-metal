// The iPhone's and iPad's Recommended Picture Size (dm_picios.c): pure, so that a unit test can walk every device
#ifndef DM_PICIOS_H
#define DM_PICIOS_H
typedef struct {
    const char *machine;   // hw.machine: "iPhone18,1"; tells apart devices that share a chip's name but not its GPU cores
    const char *gpu;       // the MTLDevice's name: "Apple A19 Pro GPU"
    unsigned w, h;         // the screen's native pixels, either way round
    int fg;                // the chip has MetalFX Temporal and Frame Generation: the GPU gets 33.3 ms a frame, not 16.7
} dm_ios_spec;
enum { DM_OSZ_FULL, DM_OSZ_HIGH, DM_OSZ_MEDIUM, DM_OSZ_LOW, DM_OSZ_N };   // the Picture Size row's values, saved as osz
void dm_picios_size(unsigned w, unsigned h, int tier, unsigned out[2]);   // the picture a tier draws on a screen of w x h (w >= h)
int dm_picios_score(const dm_ios_spec *s);                                // Geekbench 7 Metal points; 0 = not a chip the table knows (a Mac running the iOS package counts as the newest known)
double dm_picios_ms(const dm_ios_spec *s, double mp);                     // the estimated GPU time of a drawn frame at this picture size, 3D at 50%
int dm_picios_tier(const dm_ios_spec *s);                                 // the Recommended tier
int dm_picios_id(const dm_ios_spec *s);                                   // names the device for rec_id: a restored backup on another phone works the pick out again
#endif
