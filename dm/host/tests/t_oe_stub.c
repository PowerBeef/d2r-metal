// t_oe's picture setting: a strong dm_video_aa_mode, which overrides hle_oe.c's weak default (-1). It lives in its own file because t_oe.c includes hle_oe.c, and one
// translation unit can't define the function twice; a weak definition is never inlined, so aa_tick calls this one.
int g_aa_want = -1;   // -1 = leave the byte alone, as the weak default does; the existing cases don't touch it
int dm_video_aa_mode(void) { return g_aa_want; }
