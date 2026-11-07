// nvn_replay's rays switch: strong definitions of the host's dm_video_rt / dm_video_rt_level (dm/host/dm_video.c), which nvn_rt.inc leaves weak, answering "off",
// in a test build. 0 by default, so a plain replay draws what it always drew; nvn_replay --rays sets it: Full, every kind of ray, in the mode DM_RT gives (1 by default:
// the darker of the map and the rays; 2 the rays alone), as the host's DM_RT forces it.
#include <stdlib.h>
int g_rp_rays;
int dm_video_rt(void) { const char *e = getenv("DM_RT"); int m = e && *e ? atoi(e) : 1; return g_rp_rays ? (m > 0 ? m : 1) : 0; }
int dm_video_rt_level(void) { return g_rp_rays ? 3 : 0; }
