// PC keyboard and mouse in game (dm_kbm.c): the Mac front end queues events, the game main thread drains them at the controller poll
#pragma once
#include <stdint.h>
int dm_kbm_vk_from_mac(unsigned short kc);                                    // macOS virtual key code -> Windows virtual key; 0 = unused
int dm_kbm_match(const uint8_t *list, int n, int vk, int mods, int *out, int max);   // bindings a key fires (pure; t_kbm)
void dm_kbm_mouse(int op, float nx, float ny, int btn, int mods);           // any thread: op 0 move 1 down 2 up; btn 0 left 1 right 2-4 mouse 3-5; interface-normalized
void dm_kbm_key(int vk, int down, int mods);                                // any thread: mods 1 Shift 2 Ctrl 4 Alt
void dm_kbm_wheel(float nx, float ny, int notches);                         // any thread: over the world, previous / next skill
int dm_kbm_step(uint64_t *buttons);                                         // game main thread, at the controller poll; 1 = PC input live
int dm_kbm_live(void);                                                      // any thread: in game with the PC interface set
int dm_kbm_shift(void);                                                     // game main thread: Shift held in PC mode
// used by dm_kbm.c
int dm_padmode_pc(void);                                                    // dm_padmode.c
void dm_touch_host_live(int op, float nx, float ny);                         // touch_mac.c
void dm_world_pc_click(int action, int flags, float sx, float sy);         // dm_world.c: the click entry with a PC action at a screen point
