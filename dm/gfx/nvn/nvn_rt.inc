// Ray-traced key-light shadows and ambient occlusion, first version: the Mac's Ray Tracing row (dm_video.c dm_video_rt; DM_RT=0/1/2 forces it), live, off by default. Included by nvn_mtl.m after nvn_mv.inc; replay
// thread. Needs hardware ray tracing and the motion vector pass, whose record of the depth prepass it reuses (mv_note_draw): every opaque object the prepass drew, with what it takes to draw it again.
//
// Before the first forward draw of a frame, rtx_frame:
//   1. draws again what is new or has moved, with the shader's own vertex code (dm_capture, dmsh_rules.py capture_entry) and no rasterization: skinning, cloth and the object's transform come out of the game's
//      code, as world positions in a buffer;
//   2. builds one primitive acceleration structure per object from those positions and the draw's own index buffer. What hasn't moved keeps the one it has, so a static scene costs nothing here;
//   3. builds one instance acceleration structure over them (the geometry is already in world space, the transforms are identity);
//   4. traces one ray toward the key light per pixel of half the picture each way (DM_RT_KEY_HALF=0: the whole of it), spread over a small sun (DM_RT_SUN=<radius, tangent>), then a depth-aware box over the 4 x 4
//      pattern of those spreads, into the mask in the support buffer (support_buf), which every shader has bound: sharp at contact, softer with distance;
//   Each pass skips what was black in last frame's picture (DM_RT_DARK=<linear luminance, 0 = nothing skipped>): it shows nothing whatever a ray finds, and most of a dungeon's picture is black.
//   7. traces mirror rays off every surface at half the picture each way, every other pixel, and reads what they hit from last frame's picture; the forward shaders (dmsh_rules.py rt_env_hook) add that to each surface by its own normal, roughness and reflectance, the game's image lighting cube standing in where a ray hit nothing (DM_RT_SHOW=1 roughness bands, 2 normals, 3 this term alone, 4 reflectance, 5 the cube's part, 6 the rays' part; DM_RT_REFL=0 turns it
//      off, DM_RT_REFL_REACH=<world units>, DM_RT_REFL_K=<times the physical strength, 1 by default>, DM_RT_WET=<0..1 forces how damp the place is, else dm_video.c's table by level>, DM_RT_REFL_SEEN=<how lit a surface must be to show them all, 0 = always>: a floor in the dark stays dark, as the light radius is gameplay);
//   6. traces rays to the first four local lights of each pixel's tile (torches, spot lights, the hero's light, missiles) at half the picture each way, then the same box (DM_RT_LOCAL=0 turns it off, DM_RT_LOCAL_SRC=<world
//      units> is the size of the flame, DM_RT_LOCAL_STOP=<world units> how far short of the light a ray stops, so a torch doesn't shadow itself, DM_RT_LOCAL_FILL=<0..1> what a blocked ray leaves, for the bounced light nothing traces);
//   5. traces short rays (DM_RT_AO=<rays>, 1 by default, 0 = none) around each pixel's normal (from depth) into a plane of their own, then a box over the 4 x 4 pattern of their directions into the plane behind the mask.
// The forward shaders read both at their own pixel (dmsh_rules.py rt_shadow_hook, rt_ao_hook): for layer 0 the darker of the mask and the game's shadow map (DM_RT=2: the mask alone), and the colour times the occlusion,
// which the key and local lights divide back out, so it stays on the ambient terms. Alpha-tested casters (foliage, hair) aren't in the structures, as opaque triangles would cast solid cards: they stay in the map. Characters (what the game draws
// into its layer 1, nvn_mv.inc rtu_*) stay out of the key light's rays by their instance mask, since that view's axis differs by a few degrees and they would have two shadows. They get rays of their own along it (DM_RT_CHAR=0
// turns those off, DM_RT_CHAR_SUN=<radius, tangent> spreads them), which hit characters only, and the game's layer 1 then draws only what can't be in the structures (hair, cloaks), so the rays decide the shape: sharp at
// the feet, softer toward the head. The occlusion's rays hit them too.
// DM_RT_BIAS=<world units> moves the ray's start off the surface, DM_RT_AO_R=<world units> is the occlusion's reach, DM_RT_AO_P=<power> its curve, DM_RT_AO_HALF=0 traces it at the whole picture rather than half each way, DM_RT=3 traces no key light, DM_RT_AO_CHARS=2/1/0 lets its rays hit every character, the hero alone or none, DM_RT_DUMP=<frame>:<file.pgm> writes the planes as they stand.
@interface RtBlas : NSObject { @public __strong id<MTLAccelerationStructure> as; uint64_t last, hash; NSUInteger tris, bytes; int built, chr, sized; float pos[3]; NSUInteger sz_tris; MTLAccelerationStructureSizes sz; } @end
@implementation RtBlas @end
enum { RT_NEW_PER_FRAME = 24, RT_WORLD = 1, RT_CHAR = 2, RT_MAX_INST = 8192, RT_CAP_BYTES = 32 << 20, RT_SCRATCH_BYTES = 96 << 20, RT_U_STRIDE = 512, RT_KEEP_FRAMES = 600, RT_KEEP_RADIUS = 70, RT_AS_MAX_MB = 256 };
typedef struct { float ivp[16]; float iview[16]; float pvp[16]; float view[4]; float dir[4]; uint32_t size[2]; float bias, maxd; float ao_r, ao_cam[3], ao_pow; uint32_t ao_rays, ao_shift, tiles[2]; float l_src, l_stop, sun; uint32_t key_shift; float l_fill; float dir2[3], char_sun, refl_reach, dark; } rtx_u;
static struct {
    int inited, bad; NSMutableDictionary<NSNumber *, RtBlas *> *blas;
    __strong id<MTLBuffer> cap, scratch, inst, unif; __strong id<MTLAccelerationStructure> tlas; NSUInteger tlas_bytes; __strong id<MTLComputePipelineState> rpso, rbpso, cpso, kpso, kblurpso, aopso, blurpso, lpso, lblurpso; __strong id<MTLBuffer> aoraw, lraw, kraw, craw, rraw; __strong id<MTL4ArgumentTable> ct;
    uint64_t built_frame, frames, built_blas, built_tris, reused, since; float cam[3]; float dir[3]; size_t as_bytes; float bias, ao_r, ao_p; int dir_logged, mode, ao_rays, ao_shift, ao_chars, local, key_shift; float l_src, l_stop, sun, l_fill, char_sun, char_dir[3], refl_reach, refl_k, refl_seen, dark; int show; int char_on, refl_on;
} g_rtx;
static const char *g_rtx_who = "";
int rtx_chars_traced(void) { return g_rtx.inited && !g_rtx.bad && g_rtx.char_on; }
static const char *g_rtx_msl =
    "#include <metal_stdlib>\n#include <metal_raytracing>\nusing namespace metal;\nusing namespace metal::raytracing;\n"
    "struct U { float4x4 ivp; float4x4 iview; float4x4 pvp; float4 view; float4 dir; uint2 size; float bias; float maxd; float ao_r; packed_float3 ao_cam; float ao_pow; uint ao_rays; uint ao_shift; packed_uint2 tiles; float l_src; float l_stop; float sun; uint key_shift; float l_fill; packed_float3 dir2; float char_sun; float refl_reach; float dark; };\n"
    "constant float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};\n"
    "static float3 rt_world(constant U& u, uint2 id, float d) { float2 ndc = float2(2 * (id.x + 0.5 - u.view.x) / u.view.z - 1, 1 - 2 * (id.y + 0.5 - u.view.y) / u.view.w); float4 w = u.ivp * float4(ndc, d, 1); return w.xyz / w.w; }\n"
    // What was black last frame shows nothing whatever a ray finds, and most of a dungeon's picture is black: those pixels trace nothing
    "static bool rt_dark(constant U& u, texture2d<float> last, uint2 fp) { return dot(last.read(min(fp, uint2(last.get_width() - 1, last.get_height() - 1))).rgb, float3(0.2126, 0.7152, 0.0722)) < u.dark; }\n"
    "kernel void rt_shadow(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> u.key_shift; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; uint2 fp = id << u.key_shift; float d = dp.read(fp);\n"
    "  if (d <= 0.0 || rt_dark(u, last, fp)) { out[at] = 255; return; }\n"
    "  float3 p = rt_world(u, fp, d), dir = u.dir.xyz; float3 t = abs(dir.y) < 0.9 ? normalize(cross(dir, float3(0, 1, 0))) : normalize(cross(dir, float3(1, 0, 0))), b = cross(dir, t);\n"
    "  float dith = (bayer[(id.y & 3) * 4 + (id.x & 3)] + 0.5) / 16.0, a = 6.2831853 * fract(dith * 5.0), rr = sqrt(dith) * u.sun; float3 dj = normalize(dir + t * (rr * cos(a)) + b * (rr * sin(a)));\n"
    "  ray r; r.origin = p + dir * u.bias; r.direction = dj; r.min_distance = 0; r.max_distance = u.maxd;\n"
    "  intersector<instancing> i; i.accept_any_intersection(true); i.force_opacity(forced_opacity::opaque);\n"
    "  auto h = i.intersect(r, as, 1); out[at] = h.type == intersection_type::none ? 255 : 0; }\n"
    "kernel void rt_shadow_blur(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], const device uchar *in [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> u.key_shift; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; if (rt_dark(u, last, id << u.key_shift)) { out[at] = in[at]; return; } float d = dp.read(id << u.key_shift), sum = 0, wt = 0;\n"
    "  for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) { int2 q = int2(id) + int2(x, y); if (q.x < 0 || q.y < 0 || q.x >= int(gs.x) || q.y >= int(gs.y)) continue; float dq = dp.read(uint2(q) << u.key_shift); if (abs(dq - d) < 0.0015) { sum += float(in[uint(q.y) * gs.x + uint(q.x)]); wt += 1; } }\n"
    "  out[at] = wt > 0 ? uchar(sum / wt + 0.5) : in[at]; }\n"
    "kernel void rt_char(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> u.key_shift; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; uint2 fp = id << u.key_shift; float d = dp.read(fp);\n"
    "  if (d <= 0.0 || rt_dark(u, last, fp)) { out[at] = 255; return; }\n"
    "  float3 p = rt_world(u, fp, d), dir = float3(u.dir2); float3 t = abs(dir.y) < 0.9 ? normalize(cross(dir, float3(0, 1, 0))) : normalize(cross(dir, float3(1, 0, 0))), b = cross(dir, t);\n"
    "  float dith = (bayer[(id.y & 3) * 4 + (id.x & 3)] + 0.5) / 16.0, a = 6.2831853 * fract(dith * 5.0), rr = sqrt(dith) * u.char_sun; float3 dj = normalize(dir + t * (rr * cos(a)) + b * (rr * sin(a)));\n"
    "  ray r; r.origin = p + dir * 0.1; r.direction = dj; r.min_distance = 0; r.max_distance = 30;\n"
    "  intersector<instancing> i; i.accept_any_intersection(true); i.force_opacity(forced_opacity::opaque);\n"
    "  auto h = i.intersect(r, as, 2); out[at] = h.type == intersection_type::none ? 255 : 0; }\n"
    "constexpr sampler rt_mip(filter::linear, mip_filter::linear, address::clamp_to_edge);\n"
    "kernel void rt_refl(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar4 *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> 1; if (id.x >= gs.x || id.y >= gs.y || ((id.x + id.y) & 1u) != 0) return; uint at = id.y * gs.x + id.x; uchar4 o = uchar4(0); uint2 fp = id * 2; float d = dp.read(fp);\n"
    "  if (d <= 0.0 || fp.x < 2 || fp.y < 2 || fp.x + 2 >= u.size.x || fp.y + 2 >= u.size.y || rt_dark(u, last, fp)) { out[at] = o; return; }\n"
    "  float3 p = rt_world(u, fp, d);\n"
    "  float dl = dp.read(uint2(fp.x - 2, fp.y)), dr = dp.read(uint2(fp.x + 2, fp.y)), du = dp.read(uint2(fp.x, fp.y - 2)), dd = dp.read(uint2(fp.x, fp.y + 2));\n"
    "  float3 ax = abs(dl - d) < abs(dr - d) ? p - rt_world(u, uint2(fp.x - 2, fp.y), dl) : rt_world(u, uint2(fp.x + 2, fp.y), dr) - p;\n"
    "  float3 ay = abs(du - d) < abs(dd - d) ? p - rt_world(u, uint2(fp.x, fp.y - 2), du) : rt_world(u, uint2(fp.x, fp.y + 2), dd) - p;\n"
    "  float3 n = cross(ax, ay); float nn = length(n); if (nn < 1e-9) { out[at] = o; return; } n /= nn; float3 cam = float3(u.ao_cam); if (dot(n, cam) < 0) n = -n;\n"
    "  float3 dir = reflect(-cam, n);\n"
    "  ray r; r.origin = p + n * u.bias; r.direction = dir; r.min_distance = 0; r.max_distance = u.refl_reach;\n"
    "  intersector<instancing> i; i.force_opacity(forced_opacity::opaque); auto h = i.intersect(r, as, 3);\n"
    "  if (h.type == intersection_type::none) { out[at] = o; return; }\n"
    "  float3 q = r.origin + dir * h.distance; float4 c = u.pvp * float4(q, 1); float2 ndc = c.xy / c.w, uv = ndc * float2(0.5, -0.5) + 0.5;\n"
    "  if (any(uv < 0.0) || any(uv > 1.0)) { out[at] = o; return; }\n"
    "  float2 px = u.view.xy + uv * u.view.zw; uint2 hp = uint2(px); float dq = dp.read(min(hp, u.size - 1)); if (abs(dq - c.z / c.w) > 0.004) { out[at] = o; return; }\n"   // what the ray hit must be what the picture shows there: a hidden surface has no colour to give
    "  float edge = min(min(uv.x, 1 - uv.x), min(uv.y, 1 - uv.y)) * 10.0, conf = clamp(edge, 0.0, 1.0) * (1 - h.distance / u.refl_reach);\n"
    "  float3 col = min(last.sample(rt_mip, px / float2(u.size), level(0.0)).rgb, float3(16.0)) * conf;\n"
    "  out[at] = uchar4(uchar3(sqrt(col / 16.0) * 255.0 + 0.5), uchar(conf * 255)); }\n"
    "kernel void rt_ao(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> u.ao_shift; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; uint st = 1u << u.ao_shift; uint2 fp = id * st; float d = dp.read(fp);\n"
    "  if (d <= 0.0 || fp.x < st || fp.y < st || fp.x + st >= u.size.x || fp.y + st >= u.size.y || rt_dark(u, last, fp)) { out[at] = 255; return; }\n"
    "  float3 p = rt_world(u, fp, d);\n"
    "  float dl = dp.read(uint2(fp.x - st, fp.y)), dr = dp.read(uint2(fp.x + st, fp.y)), du = dp.read(uint2(fp.x, fp.y - st)), dd = dp.read(uint2(fp.x, fp.y + st));\n"
    "  float3 ax = abs(dl - d) < abs(dr - d) ? p - rt_world(u, uint2(fp.x - st, fp.y), dl) : rt_world(u, uint2(fp.x + st, fp.y), dr) - p;\n"
    "  float3 ay = abs(du - d) < abs(dd - d) ? p - rt_world(u, uint2(fp.x, fp.y - st), du) : rt_world(u, uint2(fp.x, fp.y + st), dd) - p;\n"
    "  float3 n = cross(ax, ay); float nl = length(n); if (nl < 1e-9) { out[at] = 255; return; } n /= nl; float3 cam = float3(u.ao_cam); if (dot(n, cam) < 0) n = -n;\n"
    "  float3 t = abs(n.y) < 0.9 ? normalize(cross(n, float3(0, 1, 0))) : normalize(cross(n, float3(1, 0, 0))); float3 b = cross(n, t);\n"
    "  float dith = (bayer[(id.y & 3) * 4 + (id.x & 3)] + 0.5) / 16.0; float occ = 0;\n"
    "  intersector<instancing> i; i.force_opacity(forced_opacity::opaque);\n"
    "  for (uint k = 0; k < u.ao_rays; k++) {\n"
    "    float a = 6.2831853 * fract(dith + k * 0.61803398), rr = sqrt((k + fract(dith * 7.0 + 0.5)) / float(u.ao_rays)); float3 dir = normalize(t * (rr * cos(a)) + b * (rr * sin(a)) + n * sqrt(max(0.0, 1 - rr * rr)));\n"
    "    ray r; r.origin = p + n * u.bias; r.direction = dir; r.min_distance = 0; r.max_distance = u.ao_r;\n"
    "    auto h = i.intersect(r, as, 0xff); if (h.type != intersection_type::none) { float f = 1 - h.distance / u.ao_r; occ += f * f; } }\n"
    "  out[at] = uchar(clamp(1 - occ / float(u.ao_rays), 0.0, 1.0) * 255 + 0.5); }\n"
    "kernel void rt_local(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture_buffer<float> lights [[texture(1)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]], device uchar4 *out [[buffer(2)]], const device uint *cnt [[buffer(3)]], const device uint *list [[buffer(4)]]) {\n"
    "  uint2 gs = u.size >> 1; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; uchar4 o = uchar4(255); uint2 fp = id * 2; float d = dp.read(fp);\n"
    "  if (d <= 0.0 || fp.x < 2 || fp.y < 2 || fp.x + 2 >= u.size.x || fp.y + 2 >= u.size.y || rt_dark(u, last, fp)) { out[at] = o; return; }\n"
    "  uint tx = min(uint((float(fp.x) + 0.5 - u.view.x) / u.view.z * float(u.tiles.x)), u.tiles.x - 1), ty = min(uint((float(fp.y) + 0.5 - u.view.y) / u.view.w * float(u.tiles.y)), u.tiles.y - 1), tile = tx + ty * u.tiles.x;\n"
    "  uint c = cnt[tile], n1 = c & 63, n2 = (c >> 6) & 1023, nl = n2 + ((c >> 16) & 255) + (c >> 24); if (nl == 0) { out[at] = o; return; }\n"
    "  float3 p = rt_world(u, fp, d);\n"
    "  float dl = dp.read(uint2(fp.x - 2, fp.y)), dr = dp.read(uint2(fp.x + 2, fp.y)), du = dp.read(uint2(fp.x, fp.y - 2)), dd = dp.read(uint2(fp.x, fp.y + 2));\n"
    "  float3 ax = abs(dl - d) < abs(dr - d) ? p - rt_world(u, uint2(fp.x - 2, fp.y), dl) : rt_world(u, uint2(fp.x + 2, fp.y), dr) - p;\n"
    "  float3 ay = abs(du - d) < abs(dd - d) ? p - rt_world(u, uint2(fp.x, fp.y - 2), du) : rt_world(u, uint2(fp.x, fp.y + 2), dd) - p;\n"
    "  float3 n = cross(ax, ay); float nn = length(n); if (nn < 1e-9) { out[at] = o; return; } n /= nn; if (dot(n, float3(u.ao_cam)) < 0) n = -n;\n"
    "  float dith = (bayer[(id.y & 3) * 4 + (id.x & 3)] + 0.5) / 16.0; float a = 6.2831853 * dith, b = fract(dith * 7.0 + 0.5); float3 jit = float3(cos(a) * sqrt(1 - (2 * b - 1) * (2 * b - 1)), 2 * b - 1, sin(a) * sqrt(1 - (2 * b - 1) * (2 * b - 1))) * u.l_src;\n"
    "  intersector<instancing> i; i.accept_any_intersection(true); i.force_opacity(forced_opacity::opaque);\n"
    "  for (uint k = 0; k < min(nl, 4u); k++) {\n"
    "    uint li = list[(tile << 8) + n1 + k]; float3 lv = lights.read(li).xyz; float3 lw = (u.iview * float4(lv, 1)).xyz + jit; float3 L = lw - p; float ds = length(L);\n"
    "    if (k < n2) { float rad = lights.read(li + 1).w; if (rad > 0 && ds > rad + u.l_src) continue; }\n"
    "    if (ds <= u.l_stop || dot(n, L) <= 0) continue;\n"
    "    ray r; r.origin = p + n * u.bias; r.direction = L / ds; r.min_distance = 0; r.max_distance = ds - u.l_stop;\n"
    "    auto h = i.intersect(r, as, 1); if (h.type != intersection_type::none) o[k] = uchar(u.l_fill * 255); }\n"
    "  out[at] = o; }\n"
    "kernel void rt_local_blur(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], const device uchar4 *in [[buffer(1)]], device uchar4 *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> 1; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; if (rt_dark(u, last, id * 2)) { out[at] = in[at]; return; } float d = dp.read(id * 2), wt = 0; float4 sum = 0;\n"
    "  for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) { int2 q = int2(id) + int2(x, y); if (q.x < 0 || q.y < 0 || q.x >= int(gs.x) || q.y >= int(gs.y)) continue; float dq = dp.read(uint2(q) * 2); if (abs(dq - d) < 0.0015) { sum += float4(in[uint(q.y) * gs.x + uint(q.x)]); wt += 1; } }\n"
    "  out[at] = wt > 0 ? uchar4(sum / wt + 0.5) : in[at]; }\n"
    // The mirror rays leave every other pixel out, a checkerboard: what they find is smooth, so half the rays look the same
    "kernel void rt_refl_blur(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], const device uchar4 *in [[buffer(1)]], device uchar4 *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> 1; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; if (rt_dark(u, last, id * 2)) { out[at] = uchar4(0); return; } float d = dp.read(id * 2), wt = 0; float4 sum = 0;\n"
    "  for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) { int2 q = int2(id) + int2(x, y); if (q.x < 0 || q.y < 0 || q.x >= int(gs.x) || q.y >= int(gs.y) || ((q.x + q.y) & 1) != 0) continue; float dq = dp.read(uint2(q) * 2); if (abs(dq - d) < 0.0015) { sum += float4(in[uint(q.y) * gs.x + uint(q.x)]); wt += 1; } }\n"
    "  out[at] = wt > 0 ? uchar4(sum / wt + 0.5) : uchar4(0); }\n"
    "kernel void rt_ao_blur(uint2 id [[thread_position_in_grid]], depth2d<float> dp [[texture(0)]], texture2d<float> last [[texture(2)]], constant U& u [[buffer(0)]], const device uchar *in [[buffer(1)]], device uchar *out [[buffer(2)]]) {\n"
    "  uint2 gs = u.size >> u.ao_shift; if (id.x >= gs.x || id.y >= gs.y) return; uint at = id.y * gs.x + id.x; if (rt_dark(u, last, id << u.ao_shift)) { out[at] = in[at]; return; } float d = dp.read(id << u.ao_shift), sum = 0, wt = 0;\n"
    "  for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) { int2 q = int2(id) + int2(x, y); if (q.x < 0 || q.y < 0 || q.x >= int(gs.x) || q.y >= int(gs.y)) continue; float dq = dp.read(uint2(q) << u.ao_shift); float w = abs(dq - d) < 0.0015 ? 1.0 : 0.0; sum += w * float(in[uint(q.y) * gs.x + uint(q.x)]); wt += w; }\n"
    "  float v = (wt > 0 ? sum / wt : float(in[at])) / 255.0; out[at] = uchar(pow(v, u.ao_pow) * 255 + 0.5); }\n";
__attribute__((weak)) int dm_video_rt(void) { return 0; }   // the host's dm_video.c
__attribute__((weak)) float dm_env_wetness(void) { return 0.0f; }   // the host's dm_video.c: how damp this place is
__attribute__((weak)) float dm_env_ibl_intensity(void) { return 1.0f; }   // the host's dm_video.c: the iblIntensity this environment's artists set
// Hardware ray tracing only: supportsRaytracing also answers yes on older chips, which do it in software
int mtl_rt_supported(void) { static int c = -1; if (c < 0) { id<MTLDevice> d = MTLCreateSystemDefaultDevice(); c = [d supportsRaytracing] && [d supportsFamily:MTLGPUFamilyApple9]; } return c; }
static int g_rtx_on;   // read at the start of each submission
int rtx_want(void) { return g_rtx_on; }
static int rtx_init(void) {
    if (g_rtx.inited) return !g_rtx.bad; g_rtx.inited = 1;
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(g_rtx_msl) options:nil error:&e]; id<MTLFunction> fn = [lib newFunctionWithName:@"rt_shadow"], fa = [lib newFunctionWithName:@"rt_ao"], fb = [lib newFunctionWithName:@"rt_ao_blur"];
    g_rtx.kpso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    { id<MTLFunction> fk = [lib newFunctionWithName:@"rt_shadow_blur"]; g_rtx.kblurpso = fk ? [g_dev newComputePipelineStateWithFunction:fk error:&e] : nil; g_rtx.kraw = [g_dev newBufferWithLength:(NSUInteger)3840 * 2160 options:MTLResourceStorageModePrivate]; const char *kh = getenv("DM_RT_KEY_HALF"), *su = getenv("DM_RT_SUN"); g_rtx.key_shift = kh ? atoi(kh) > 0 : 1; g_rtx.sun = su ? (float)atof(su) : 0.03f; if (g_rtx.kraw) res_add(g_rtx.kraw);
      { const char *re_ = getenv("DM_RT_REFL"), *rr_ = getenv("DM_RT_REFL_REACH"), *rk_ = getenv("DM_RT_REFL_K"), *rg_ = getenv("DM_RT_REFL_SEEN"), *rs_ = getenv("DM_RT_SHOW"); g_rtx.show = rs_ ? atoi(rs_) : 0; g_rtx.refl_seen = rg_ ? (float)atof(rg_) : 0.005f; g_rtx.refl_on = re_ ? atoi(re_) > 0 : 1; g_rtx.refl_reach = rr_ ? (float)atof(rr_) : 40.0f; g_rtx.refl_k = rk_ ? (float)atof(rk_) : 1.0f;
        if (g_rtx.refl_on) { id<MTLFunction> fr_ = [lib newFunctionWithName:@"rt_refl"]; g_rtx.rpso = fr_ ? [g_dev newComputePipelineStateWithFunction:fr_ error:&e] : nil; { id<MTLFunction> frb_ = [lib newFunctionWithName:@"rt_refl_blur"]; g_rtx.rbpso = frb_ ? [g_dev newComputePipelineStateWithFunction:frb_ error:&e] : nil; } g_rtx.rraw = [g_dev newBufferWithLength:(NSUInteger)1920 * 1080 * 4 options:MTLResourceStorageModePrivate]; if (!g_rtx.rpso || !g_rtx.rbpso || !g_rtx.rraw) { MLOG("【红】ray tracing: can't start the reflections (%s)", e.localizedDescription.UTF8String); g_rtx.refl_on = 0; } else res_add(g_rtx.rraw); } }
      { const char *dk_ = getenv("DM_RT_DARK"); g_rtx.dark = dk_ ? (float)atof(dk_) : 0.0005f; }
      const char *ce_ = getenv("DM_RT_CHAR"), *cs_ = getenv("DM_RT_CHAR_SUN"); g_rtx.char_on = ce_ ? atoi(ce_) > 0 : 1; g_rtx.char_sun = cs_ ? (float)atof(cs_) : 0.06f;
      if (g_rtx.char_on) { id<MTLFunction> fc_ = [lib newFunctionWithName:@"rt_char"]; g_rtx.cpso = fc_ ? [g_dev newComputePipelineStateWithFunction:fc_ error:&e] : nil; g_rtx.craw = [g_dev newBufferWithLength:(NSUInteger)3840 * 2160 options:MTLResourceStorageModePrivate]; if (!g_rtx.cpso || !g_rtx.craw) { MLOG("【红】ray tracing: can't start the character rays (%s)", e.localizedDescription.UTF8String); g_rtx.char_on = 0; } else res_add(g_rtx.craw); } }
    { const char *le = getenv("DM_RT_LOCAL"), *sr = getenv("DM_RT_LOCAL_SRC"), *st = getenv("DM_RT_LOCAL_STOP"); g_rtx.local = le ? atoi(le) > 0 : 1; g_rtx.l_src = sr ? (float)atof(sr) : 1.0f; { const char *fi = getenv("DM_RT_LOCAL_FILL"); g_rtx.l_fill = fi ? (float)atof(fi) : 0.35f; } g_rtx.l_stop = st ? (float)atof(st) : 2.0f;
      if (g_rtx.local) { id<MTLFunction> fl = [lib newFunctionWithName:@"rt_local"], flb = [lib newFunctionWithName:@"rt_local_blur"]; g_rtx.lpso = fl ? [g_dev newComputePipelineStateWithFunction:fl error:&e] : nil; g_rtx.lblurpso = flb ? [g_dev newComputePipelineStateWithFunction:flb error:&e] : nil; g_rtx.lraw = [g_dev newBufferWithLength:(NSUInteger)1920 * 1080 * 4 options:MTLResourceStorageModePrivate];
          if (!g_rtx.lpso || !g_rtx.lblurpso || !g_rtx.lraw) { MLOG("【红】ray tracing: can't start the local lights (%s)", e.localizedDescription.UTF8String); g_rtx.local = 0; } else { res_add(g_rtx.lraw); MLOG("ray tracing: local lights on, source %.2f, stop %.2f short", g_rtx.l_src, g_rtx.l_stop); } } }
    { const char *ae = getenv("DM_RT_AO"); g_rtx.ao_rays = ae ? atoi(ae) : 1; }
    if (g_rtx.ao_rays > 0) { const char *r = getenv("DM_RT_AO_R"), *pw = getenv("DM_RT_AO_P"), *hf = getenv("DM_RT_AO_HALF"); g_rtx.ao_shift = hf ? atoi(hf) > 0 : 1; { const char *ch = getenv("DM_RT_AO_CHARS"); g_rtx.ao_chars = ch ? atoi(ch) : 2; } g_rtx.ao_r = r ? (float)atof(r) : 4.0f; g_rtx.ao_p = pw ? (float)atof(pw) : 2.0f;
        g_rtx.aopso = fa ? [g_dev newComputePipelineStateWithFunction:fa error:&e] : nil; g_rtx.blurpso = fb ? [g_dev newComputePipelineStateWithFunction:fb error:&e] : nil; g_rtx.aoraw = [g_dev newBufferWithLength:(NSUInteger)3840 * 2160 options:MTLResourceStorageModePrivate];
        if (!g_rtx.aopso || !g_rtx.blurpso || !g_rtx.aoraw) { MLOG("【红】ray tracing: can't start ambient occlusion (%s)", e.localizedDescription.UTF8String); g_rtx.ao_rays = 0; } else { res_add(g_rtx.aoraw); MLOG("ray tracing: ambient occlusion on, %d rays, reach %.2f, power %.2f", g_rtx.ao_rays, g_rtx.ao_r, g_rtx.ao_p); } }
    g_rtx.cap = [g_dev newBufferWithLength:RT_CAP_BYTES options:MTLResourceStorageModePrivate]; g_rtx.scratch = [g_dev newBufferWithLength:RT_SCRATCH_BYTES options:MTLResourceStorageModePrivate];
    g_rtx.inst = [g_dev newBufferWithLength:RT_MAX_INST * sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) * MV_RING options:MTLResourceStorageModeShared]; g_rtx.unif = [g_dev newBufferWithLength:RT_U_STRIDE * MV_RING options:MTLResourceStorageModeShared];
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 3; ad.maxBufferBindCount = 5; g_rtx.ct = [g_dev newArgumentTableWithDescriptor:ad error:&e];
    if (!g_rtx.kpso || !g_rtx.kblurpso || !g_rtx.kraw || !g_rtx.cap || !g_rtx.scratch || !g_rtx.inst || !g_rtx.unif || !g_rtx.ct) { MLOG("【红】ray tracing: can't start (%s)", e.localizedDescription.UTF8String); g_rtx.bad = 1; return 0; }
    res_add(g_rtx.cap); res_add(g_rtx.scratch); res_add(g_rtx.inst); res_add(g_rtx.unif); res_commit();
    g_rtx.blas = [NSMutableDictionary dictionary];
    const char *b = getenv("DM_RT_BIAS"); g_rtx.bias = b ? (float)atof(b) : 0.02f; g_rtx.mode = g_rtx_on;
    MLOG("ray tracing: on, ray start %.3f toward the light", g_rtx.bias);
    return 1;
}
// A moving draw's capture pipeline: the shader's dm_capture entry, no rasterization. Built on the background queue, as mv_pso
static id<MTLRenderPipelineState> rtx_cap_pso(NvShader *vs) {
    uint32_t cfmt[8] = {0}; pso_key k; pso_key_fill(&k, vs, nil, nil, cfmt, MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1, NULL);
    k.blend_en = 0; k.chmask = 0xfffffffe; k.a2c = 0; memset(k.blend, 0, sizeof k.blend);
    id ps = hm_get_id(&g_mv_pso, &k);
    if (ps) return ps == g_mv_marker ? nil : ps;
    if (!g_mv_marker) { g_mv_marker = [NSObject new]; g_mv_done = [NSMutableArray array]; }
    hm_put_id(&g_mv_pso, &k, g_mv_marker);
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, nil, nil, 0);
    d.vertexFunction = vs->cap_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO;
    for (int i = 0; i < 8; i++) d.colorAttachments[i].pixelFormat = MTLPixelFormatInvalid;
    d.depthAttachmentPixelFormat = MTLPixelFormatInvalid; d.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k]; NSString *who = @(vs->key);
    dispatch_async(mv_queue(), ^{ NSError *e = nil; id<MTLRenderPipelineState> r = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!r) { MLOG("【红】ray tracing: capture pipeline for %s failed (%s)", who.UTF8String, e.localizedDescription.UTF8String); return; }
        os_unfair_lock_lock(&g_mv_done_lk); [g_mv_done addObject:@[key, r]]; os_unfair_lock_unlock(&g_mv_done_lk); });
    return nil;
}
static uint64_t rtx_fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
// What says an object is where it was: its position and its bones
static uint64_t rtx_state_hash(const mv_draw *d) {
    const uint8_t *base = g_mv.snap[g_mv.ri].contents; float w[6]; mv_where(d, base, w); uint64_t h = rtx_fnv(1469598103934665603ull, w, 3 * sizeof(float));
    const mv_cb *b = mv_cb_of(d, 2); if (b) h = rtx_fnv(h, base + b->off, b->size);
    return h;
}
static uint32_t rtx_vmax(const mv_draw *d) {
    uint32_t m = 0; if (d->itype == 1) { const uint16_t *p = d->ib_cpu; for (int i = 0; i < d->count; i++) if (p[i] > m) m = p[i]; } else { const uint32_t *p = d->ib_cpu; for (int i = 0; i < d->count; i++) if (p[i] > m) m = p[i]; }
    return m;
}
// Whether a character goes into the structures at all: only the occlusion's rays hit them. Each moving one is rebuilt every frame, so DM_RT_AO_CHARS=1 keeps the hero alone (the camera holds her at the middle of the
// picture) and 0 none, which holds the cost steady in a crowd
static int rtx_char_wanted(const mv_draw *d, int c) {
    if (!g_rtx.ao_rays || !g_rtx.ao_chars) return 0;
    if (g_rtx.ao_chars >= 2) return 1;
    const float *V = g_mv.vp[c]; float w[6]; mv_where(d, g_mv.snap[g_mv.ri].contents, w);
    float x = V[0] * w[0] + V[4] * w[1] + V[8] * w[2] + V[12], y = V[1] * w[0] + V[5] * w[1] + V[9] * w[2] + V[13];
    return fabsf(x) < 0.08f && fabsf(y) < 0.15f;
}
// Whether this draw's object changed lately. Its slot (d->key) is the same from frame to frame, so a change of position or shape shows up here: anything that changed in the last second keeps its slot's structure, rebuilt in
// place. A key made of its state would be a new structure every frame for anything animated, and they would pile up
enum { RT_SLOTS = 16384 }; static struct { uint64_t key, hash, until; } g_rt_slot[RT_SLOTS];
static int rtx_slot_moving(uint64_t key, uint64_t h, uint64_t frame) {
    for (int i = 0; i < 32; i++) { typeof(&g_rt_slot[0]) e = &g_rt_slot[(key + (uint64_t)i) % RT_SLOTS];
        if (e->key == key) { if (e->hash != h) { e->hash = h; e->until = frame + 30; } return frame < e->until; }
        if (!e->key) { e->key = key; e->hash = h; e->until = 0; return 0; } }
    return 1;   // the table is full around here: treat it as moving, which never adds a structure
}
// Replay thread, at a draw that reads the shadow map, before its pass opens: this frame's mask, once. Leaves the state so that the draw goes on as usual
static void rtx_mask_off(void) { ((uint32_t *)((uint8_t *)support_buf().contents + 2048))[0] = 0; }
static void rtx_frame(void) {
    uint64_t frame = atomic_load(&g_cur_frame); int c = g_mv.cur;
    if (g_rtx.built_frame == frame) return;
    g_rtx.built_frame = frame;
    { static long ln; if (getenv("DM_RT_LIGHTLOG") && g_rtx.inited && ++ln % 120 == 0) { void *mb; uint64_t mo; const p_bind_buf *sc = &S.ssbo[1][4];   // how many lights each tile's list holds, by kind (last frame's, the CPU sees it late)
          if (sc->addr && mtl_addr(sc->addr, 4, &mb, &mo)) { const uint32_t *cnt = (const uint32_t *)((const uint8_t *)((__bridge id<MTLBuffer>)mb).contents + mo); unsigned c1 = 0, c2 = 0, c3 = 0, c4 = 0, t2 = 0, t3 = 0, t4 = 0, mx = 0;
              for (int i = 0; i < 640; i++) { uint32_t v = cnt[i]; unsigned a1 = v & 63, a2 = v >> 6 & 1023, a3 = v >> 16 & 255, a4 = v >> 24; c1 += a1; c2 += a2; c3 += a3; c4 += a4; t2 += !!a2; t3 += !!a3; t4 += !!a4; if (a2 + a3 + a4 > mx) mx = a2 + a3 + a4; }
              MLOG("ray tracing: frame %llu tile lists: decals %u, point %u in %u tiles, spot %u in %u, unit %u in %u; most in a tile %u", (unsigned long long)frame, c1, c2, t2, c3, t3, c4, t4, mx); } } }
    // Only the world: its camera is orthographic. Elsewhere (the character screen) and in a frame with no prepass the shaders must not read an old mask
    { const float *V = g_mv.vp[c]; if (g_mv.frame[c] != frame || !g_mv.n[c] || !g_mv.depth || !g_mv.has_vp[c] || V[3] != 0 || V[7] != 0 || V[11] != 0 || V[15] != 1 || !rtx_init()) { rtx_mask_off(); return; } }
    { static long want = -2; static char path[512]; if (want == -2) { const char *e = getenv("DM_RT_DUMP"), *q = e ? strchr(e, ':') : NULL; want = q ? atol(e) : -1; if (q) strlcpy(path, q + 1, sizeof path); }   // DM_RT_DUMP=<frame>:<file.pgm>: the mask as it stands, for looking at it alone
      if (want >= 0 && (long)frame >= want) { want = -1; const uint32_t *h = (const uint32_t *)((const uint8_t *)support_buf().contents + 2048); FILE *f = h[0] ? fopen(path, "wb") : NULL; if (f) { fprintf(f, "P5\n%u %u\n255\n", h[1] >> h[6], h[2] >> h[6]); fwrite((const uint8_t *)support_buf().contents + 4096, 1, (size_t)(h[1] >> h[6]) * (h[2] >> h[6]), f); fclose(f); MLOG("ray tracing: mask written to %s", path); if (h[5]) { const uint8_t *pl = (const uint8_t *)support_buf().contents + 4096 + 2 * (size_t)h[1] * h[2]; unsigned w2 = h[1] >> 1, h2 = h[2] >> 1; for (int ch = 0; ch < 2; ch++) { char pl2[540]; snprintf(pl2, sizeof pl2, "%s.l%d.pgm", path, ch); FILE *fl = fopen(pl2, "wb"); if (!fl) continue; fprintf(fl, "P5\n%u %u\n255\n", w2, h2); for (unsigned i = 0; i < w2 * h2; i++) fputc(pl[i * 4 + ch], fl); fclose(fl); } } if (h[3]) { char p2[540]; snprintf(p2, sizeof p2, "%s.ao.pgm", path); f = fopen(p2, "wb"); if (f) { fprintf(f, "P5\n%u %u\n255\n", h[1] >> h[4], h[2] >> h[4]); fwrite((const uint8_t *)support_buf().contents + 4096 + (size_t)h[1] * h[2], 1, (size_t)(h[1] >> h[4]) * (h[2] >> h[4]), f); fclose(f); } } } } }
    // The key light: the axis of the game's own shadow view (scene block rows 14..17, columns), so that the rays agree with its map. Those rows take a position in view space, as all the forward lighting does, and
    // the structures are in world space, hence the camera's rotation. Rows 0 and 1 of that matrix say where in the map a point lands: what they leave unchanged is the light's axis. The light comes from above
    float q[12]; for (int r = 0; r < 3; r++) for (int i = 0; i < 3; i++) if (!ubo_float(1, (uint16_t)(10 << 8 | (14 + r) << 2 | i), &q[r * 3 + i])) { static int once; if (!once++) MLOG("ray tracing: can't read the shadow view, shader %s", g_rtx_who); rtx_mask_off(); return; }
    double r0[3] = {q[0], q[3], q[6]}, r1[3] = {q[1], q[4], q[7]}, ax[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    const float *vm = g_mv.view_m[c]; double wd[3]; for (int i = 0; i < 3; i++) wd[i] = vm[i * 4] * ax[0] + vm[i * 4 + 1] * ax[1] + vm[i * 4 + 2] * ax[2];
    double len = sqrt(wd[0] * wd[0] + wd[1] * wd[1] + wd[2] * wd[2]); if (len < 1e-12) return; if (wd[1] < 0) len = -len;
    for (int i = 0; i < 3; i++) g_rtx.dir[i] = (float)(wd[i] / len);
    if (!g_rtx.dir_logged++) MLOG("ray tracing: toward the key light %.3f %.3f %.3f", g_rtx.dir[0], g_rtx.dir[1], g_rtx.dir[2]);
    { float q1[9]; int ok1 = 1; for (int r = 0; r < 3; r++) for (int i = 0; i < 3; i++) if (!ubo_float(1, (uint16_t)(10 << 8 | (18 + r) << 2 | i), &q1[r * 3 + i])) ok1 = 0;   // the character shadow's view, the same way
      if (ok1) { double a0[3] = {q1[0], q1[3], q1[6]}, a1[3] = {q1[1], q1[4], q1[7]}, ac[3] = {a0[1] * a1[2] - a0[2] * a1[1], a0[2] * a1[0] - a0[0] * a1[2], a0[0] * a1[1] - a0[1] * a1[0]}, w1[3]; for (int i = 0; i < 3; i++) w1[i] = vm[i * 4] * ac[0] + vm[i * 4 + 1] * ac[1] + vm[i * 4 + 2] * ac[2];
          double l1 = sqrt(w1[0] * w1[0] + w1[1] * w1[1] + w1[2] * w1[2]); if (l1 > 1e-12) { if (w1[1] < 0) l1 = -l1; for (int i = 0; i < 3; i++) g_rtx.char_dir[i] = (float)(w1[i] / l1); } } }
    double vd[16], inv[16]; for (int i = 0; i < 16; i++) vd[i] = g_mv.vp[c][i]; if (!mat4_inv(vd, inv)) return;
    rtx_u u; for (int i = 0; i < 16; i++) u.ivp[i] = (float)inv[i];
    { double vv[16], iv[16]; for (int i = 0; i < 16; i++) vv[i] = g_mv.view_m[c][i]; if (!mat4_inv(vv, iv)) { rtx_mask_off(); return; } for (int i = 0; i < 16; i++) u.iview[i] = (float)iv[i]; }
    for (int i = 0; i < 4; i++) u.view[i] = (float)g_mv.view[i];
    for (int i = 0; i < 3; i++) u.dir[i] = g_rtx.dir[i]; u.dir[3] = 0; u.size[0] = (uint32_t)g_mv.depth.width; u.size[1] = (uint32_t)g_mv.depth.height; u.bias = g_rtx.bias; u.maxd = 120; u.ao_r = g_rtx.ao_r; u.ao_rays = (uint32_t)g_rtx.ao_rays; u.ao_pow = g_rtx.ao_p; for (int i = 0; i < 3; i++) u.dir2[i] = g_rtx.char_dir[i]; u.ao_shift = (uint32_t)g_rtx.ao_shift; u.l_src = g_rtx.l_src; u.l_stop = g_rtx.l_stop; u.sun = g_rtx.sun; u.refl_reach = g_rtx.refl_reach; u.dark = g_rtx.dark; { const float *pv = g_mv.has_vp[!c] ? g_mv.vp[!c] : g_mv.vp[c]; memcpy(u.pvp, pv, sizeof u.pvp); } u.char_sun = g_rtx.char_sun; u.l_fill = g_rtx.l_fill; u.key_shift = (uint32_t)g_rtx.key_shift;
    { float tc[2]; u.tiles[0] = u.tiles[1] = 0; if (ubo_float(1, (uint16_t)(10 << 8 | 13 << 2 | 2), &tc[0]) && ubo_float(1, (uint16_t)(10 << 8 | 13 << 2 | 3), &tc[1])) { memcpy(&u.tiles[0], &tc[0], 4); memcpy(&u.tiles[1], &tc[1], 4); } }   // the tile grid's size, integers in the scene block
    { double c2[3]; for (int i = 0; i < 3; i++) c2[i] = inv[8 + i]; double ln = sqrt(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]); for (int i = 0; i < 3; i++) u.ao_cam[i] = (float)(ln > 0 ? c2[i] / ln : 0); }   // the orthographic camera's axis: depth grows toward the viewer
    if (rtx_mask_bytes() < 4096 + 256 + (size_t)u.size[0] * u.size[1] * 4 + ((size_t)u.size[0] * u.size[1] >> (2 * g_rtx.key_shift))) { if (!g_rtx.bad++) MLOG("【红】ray tracing: the mask doesn't fit a %ux%u picture", u.size[0], u.size[1]); return; }
    NSUInteger ui = (NSUInteger)g_mv.ri * RT_U_STRIDE; memcpy((uint8_t *)g_rtx.unif.contents + ui, &u, sizeof u); uint64_t u_gpu = g_rtx.unif.gpuAddress + ui;
    // Last frame's picture, still in the forward pass's own colour target since that pass hasn't opened
    id<MTLTexture> last = R.has_rt && R.rt.n >= 1 && R.rt.color[0] ? tex_mtl(R.rt.color[0]) : nil; if (!last) { rtx_mask_off(); return; }
    rp_end();
    // 1. what is new or has moved, drawn again with no rasterization
    enum { MAXB = 2048 }; static mv_draw *todo[MAXB]; static uint64_t toff[MAXB], tlen[MAXB], thash[MAXB]; static RtBlas *tb[MAXB]; int nt = 0, nfresh = 0; size_t cap_at = 0, scr_at = 0;
    static uint64_t ikey[RT_MAX_INST], tkey[2048]; int ni = 0;
    for (int i = 0; i < g_mv.n[c] && ni < RT_MAX_INST; i++) {
        mv_draw *d = &g_mv.d[c][i]; if (!d->rt_opaque || !d->ib_cpu || d->count < 3 || (d->rt_char && !g_rtx.char_on && !rtx_char_wanted(d, c))) continue;
        uint64_t h = rtx_state_hash(d), okey = d->rt_char || rtx_slot_moving(d->key, h, frame) ? d->key : d->base ^ h; NSNumber *k = @(okey); RtBlas *b = g_rtx.blas[k]; if (b) b->chr = d->rt_char;   // what stands still is known by its mesh and place, so a rock keeps its structure whatever else comes and goes; what moves keeps its slot
        if (b && b->built && b->hash == h) { b->last = frame; ikey[ni++] = okey; g_rtx.reused++; continue; }
        int fresh = !b || !b->built;   // a level brings hundreds of new structures at once: they come in over a few frames (RT_NEW_PER_FRAME), as over a hundred in one frame stalled the GPU for good in about one level change in ten
        if (!d->cap_pso || nt >= MAXB || (fresh && nfresh >= RT_NEW_PER_FRAME)) { if (b && b->built) { b->last = frame; ikey[ni++] = okey; } continue; }   // its pipeline is still building: last frame's shape, or none yet
        uint32_t vm = rtx_vmax(d); uint64_t len = (uint64_t)(vm + 1) * 16; if (cap_at + len > RT_CAP_BYTES) { if (b && b->built) { b->last = frame; ikey[ni++] = okey; } continue; }
        if (!b) { b = [RtBlas new]; g_rtx.blas[k] = b; b->chr = d->rt_char; }
        { float w[6]; mv_where(d, g_mv.snap[g_mv.ri].contents, w); b->pos[0] = w[0]; b->pos[1] = w[1]; b->pos[2] = w[2]; }
        nfresh += fresh; tkey[nt] = okey; todo[nt] = d; toff[nt] = cap_at; tlen[nt] = len; thash[nt] = h; tb[nt] = b; nt++; cap_at += len; ikey[ni++] = okey; b->last = frame;
    }
    // What the prepass didn't draw this frame but drew lately is still there, just off the picture: the rays go on seeing it, or the shading at the picture's edges would change as it scrolls away. A jump of the camera
    // is a new place: nothing before it counts
    { const float *vm = g_mv.view_m[c]; float cx = -(vm[0] * vm[12] + vm[1] * vm[13] + vm[2] * vm[14]), cy = -(vm[4] * vm[12] + vm[5] * vm[13] + vm[6] * vm[14]), cz = -(vm[8] * vm[12] + vm[9] * vm[13] + vm[10] * vm[14]);
      float jx = cx - g_rtx.cam[0], jy = cy - g_rtx.cam[1], jz = cz - g_rtx.cam[2]; if (jx * jx + jy * jy + jz * jz > 40 * 40) g_rtx.since = frame; g_rtx.cam[0] = cx; g_rtx.cam[1] = cy; g_rtx.cam[2] = cz;
      for (NSNumber *k in g_rtx.blas) { RtBlas *b = g_rtx.blas[k]; if (ni >= RT_MAX_INST) break; if (!b->built || b->chr || b->last == frame || b->last < g_rtx.since) continue;
          float dx = b->pos[0] - cx, dy = b->pos[1] - cy, dz = b->pos[2] - cz, al = dx * vm[2] + dy * vm[6] + dz * vm[10], ox = dx - al * vm[2], oy = dy - al * vm[6], oz = dz - al * vm[10]; if (ox * ox + oy * oy + oz * oz < RT_KEEP_RADIUS * RT_KEEP_RADIUS) ikey[ni++] = k.unsignedLongLongValue; } }   // near the picture only: the distance from the camera's axis
    if (nt) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new]; rp.renderTargetWidth = 16; rp.renderTargetHeight = 16; rp.defaultRasterSampleCount = 1;
        RENC en = [g_cb renderCommandEncoderWithDescriptor:rp];
        if (en) {
            [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
            [en setArgumentTable:g_mv.vt atStages:MTLRenderStageVertex]; [g_mv.vt setAddress:g_mv.zero.gpuAddress atIndex:VB_ZERO]; [g_mv.vt setAddress:u_gpu atIndex:3];
            uint8_t *ab = g_mv.snap[g_mv.ri].contents; uint64_t ab_gpu = g_mv.gpu[g_mv.ri];
            for (int i = 0; i < nt; i++) {
                mv_draw *d = todo[i]; size_t need = (d->vs->arg_len + 15) & ~(size_t)15; if (g_mv.used + need > MV_SNAP_BYTES) { nt = i; break; }
                uint64_t *cs = (uint64_t *)(ab + g_mv.used); uint64_t cg = ab_gpu + g_mv.used; g_mv.used += need; memset(cs, 0, need);
                for (int k = 0; k < d->ncb; k++) { const mv_cb *a = &d->cb[k]; cs[a->aid] = ab_gpu + a->off; }
                [en setRenderPipelineState:d->cap_pso];
                for (int s = 0; s < 15; s++) if (d->vb[s]) [g_mv.vt setAddress:d->vb[s] atIndex:16 + s];
                [g_mv.vt setAddress:cg atIndex:d->vs->rf.abi]; [g_mv.vt setAddress:g_rtx.cap.gpuAddress + toff[i] - (int64_t)d->base_vertex * 16 atIndex:2];   // written by vertex id, which counts the base vertex; the structure reads by index, which doesn't
               
                [en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d->count indexType:d->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:d->ib_gpu indexBufferLength:d->ib_len instanceCount:1 baseVertex:d->base_vertex baseInstance:d->base_instance];
            }
            enc_host_r_end(en);
        } else nt = 0;
    }
    CENC ce = enc_c_begin(NULL);
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    // 2. their structures
    for (int i = 0; i < nt; i++) {
        mv_draw *d = todo[i]; RtBlas *b = tb[i];
        MTL4AccelerationStructureTriangleGeometryDescriptor *g = [MTL4AccelerationStructureTriangleGeometryDescriptor new];
        g.vertexBuffer = MTL4BufferRangeMake(g_rtx.cap.gpuAddress + toff[i], tlen[i]); g.vertexFormat = MTLAttributeFormatFloat3; g.vertexStride = 16;
        g.indexBuffer = MTL4BufferRangeMake(d->ib_gpu, d->ib_len); g.indexType = d->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32; g.triangleCount = (NSUInteger)d->count / 3; g.opaque = YES;
        MTL4PrimitiveAccelerationStructureDescriptor *desc = [MTL4PrimitiveAccelerationStructureDescriptor new]; desc.geometryDescriptors = @[g]; desc.usage = tkey[i] == d->key ? MTLAccelerationStructureUsagePreferFastBuild : MTLAccelerationStructureUsageNone;   // what stands still is built once: a better structure is worth its build, as every ray goes through it
        if (!b->sized || b->sz_tris != g.triangleCount) { b->sz = [g_dev accelerationStructureSizesWithDescriptor:desc]; b->sized = 1; b->sz_tris = g.triangleCount; } MTLAccelerationStructureSizes sz = b->sz;   // the same mesh takes the same size, and asking costs more than the rest of this loop
        if (scr_at + sz.buildScratchBufferSize > RT_SCRATCH_BYTES) { if (!b->built) { for (int j = 0; j < ni; j++) if (ikey[j] == tkey[i]) { ikey[j] = ikey[--ni]; break; } } continue; }
        if (!b->as || b->bytes < sz.accelerationStructureSize) { if (b->as) { g_rtx.as_bytes -= b->bytes; fg_bury(b->as); } b->as = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize]; b->bytes = sz.accelerationStructureSize; g_rtx.as_bytes += b->bytes; res_add(b->as); }
        if (!b->as) continue;
        [ce buildAccelerationStructure:b->as descriptor:desc scratchBuffer:MTL4BufferRangeMake(g_rtx.scratch.gpuAddress + scr_at, sz.buildScratchBufferSize)];
        scr_at += (sz.buildScratchBufferSize + 255) & ~(size_t)255; b->built = 1; b->hash = thash[i]; b->tris = g.triangleCount; g_rtx.built_blas++; g_rtx.built_tris += g.triangleCount;
    }
    if (nt) res_commit();
    [ce barrierAfterEncoderStages:MTLStageAccelerationStructure beforeEncoderStages:MTLStageAccelerationStructure | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    // 3. the scene
    MTLIndirectAccelerationStructureInstanceDescriptor *ins = (MTLIndirectAccelerationStructureInstanceDescriptor *)g_rtx.inst.contents + (NSUInteger)g_mv.ri * RT_MAX_INST; int nu = 0;
    for (int i = 0; i < ni; i++) { RtBlas *b = g_rtx.blas[@(ikey[i])]; if (!b || !b->built || !b->as) continue;
        MTLIndirectAccelerationStructureInstanceDescriptor *x = &ins[nu++]; memset(x, 0, sizeof *x); x->transformationMatrix.columns[0] = (MTLPackedFloat3){1, 0, 0}; x->transformationMatrix.columns[1] = (MTLPackedFloat3){0, 1, 0}; x->transformationMatrix.columns[2] = (MTLPackedFloat3){0, 0, 1};
        x->mask = b->chr ? RT_CHAR : RT_WORLD; x->options = MTLAccelerationStructureInstanceOptionOpaque; x->accelerationStructureID = b->as.gpuResourceID; }
    MTL4InstanceAccelerationStructureDescriptor *td = [MTL4InstanceAccelerationStructureDescriptor new];
    td.instanceDescriptorBuffer = MTL4BufferRangeMake(g_rtx.inst.gpuAddress + (uint64_t)g_mv.ri * RT_MAX_INST * sizeof *ins, (uint64_t)nu * sizeof *ins); td.instanceDescriptorStride = sizeof *ins; td.instanceCount = (NSUInteger)nu; td.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect; td.usage = MTLAccelerationStructureUsagePreferFastBuild;
    MTLAccelerationStructureSizes ts = [g_dev accelerationStructureSizesWithDescriptor:td];
    if (!g_rtx.tlas || g_rtx.tlas_bytes < ts.accelerationStructureSize) { if (g_rtx.tlas) fg_bury(g_rtx.tlas); g_rtx.tlas_bytes = ts.accelerationStructureSize * 2; g_rtx.tlas = [g_dev newAccelerationStructureWithSize:g_rtx.tlas_bytes]; res_add(g_rtx.tlas); res_commit(); }
    if (scr_at + ts.buildScratchBufferSize <= RT_SCRATCH_BYTES && nu) {
        [ce buildAccelerationStructure:g_rtx.tlas descriptor:td scratchBuffer:MTL4BufferRangeMake(g_rtx.scratch.gpuAddress + scr_at, ts.buildScratchBufferSize)];
        [ce barrierAfterEncoderStages:MTLStageAccelerationStructure beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        // 4. the mask
        [ce setArgumentTable:g_rtx.ct]; [g_rtx.ct setTexture:g_mv.depth.gpuResourceID atIndex:0]; [g_rtx.ct setTexture:last.gpuResourceID atIndex:2]; [g_rtx.ct setAddress:u_gpu atIndex:0]; [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1];
        // Where the game gives the key light no strength (scene block row 51: nothing in most dungeons) its rays would change nothing
        int key_rays = g_rtx.mode != 3; if (key_rays) { float kc[3]; int kok = 1; for (int i = 0; i < 3; i++) if (!ubo_float(1, (uint16_t)(10 << 8 | 51 << 2 | i), &kc[i])) kok = 0; if (kok && fabsf(kc[0]) < 0.01f && fabsf(kc[1]) < 0.01f && fabsf(kc[2]) < 0.01f) key_rays = 0; }
        if (key_rays) {
            MTLSize kg = MTLSizeMake(u.size[0] >> u.key_shift, u.size[1] >> u.key_shift, 1);
            [g_rtx.ct setAddress:g_rtx.kraw.gpuAddress atIndex:2]; [ce setComputePipelineState:g_rtx.kpso]; [ce dispatchThreads:kg threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            [g_rtx.ct setAddress:g_rtx.kraw.gpuAddress atIndex:1]; [g_rtx.ct setAddress:support_gpu() + 4096 atIndex:2]; [ce setComputePipelineState:g_rtx.kblurpso]; [ce dispatchThreads:kg threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1];
        }
        int con = 0;
        if (g_rtx.char_on && (g_rtx.char_dir[0] != 0 || g_rtx.char_dir[1] != 0 || g_rtx.char_dir[2] != 0)) {   // the characters' own shadow, along their own axis, hitting characters only
            MTLSize kg = MTLSizeMake(u.size[0] >> u.key_shift, u.size[1] >> u.key_shift, 1);
            [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1]; [g_rtx.ct setAddress:g_rtx.craw.gpuAddress atIndex:2]; [ce setComputePipelineState:g_rtx.cpso]; [ce dispatchThreads:kg threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            [g_rtx.ct setAddress:g_rtx.craw.gpuAddress atIndex:1]; [g_rtx.ct setAddress:support_gpu() + 4096 + 3 * (uint64_t)u.size[0] * u.size[1] atIndex:2]; [ce setComputePipelineState:g_rtx.kblurpso]; [ce dispatchThreads:kg threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1]; con = 1;
        }
        if (g_rtx.ao_rays) {   // 5. the occlusion: its rays into a plane of its own, then a box over the 4 x 4 pattern of their directions, into the plane behind the mask
            uint64_t plane = support_gpu() + 4096 + (uint64_t)u.size[0] * u.size[1];
            [g_rtx.ct setAddress:g_rtx.aoraw.gpuAddress atIndex:2]; [ce setComputePipelineState:g_rtx.aopso]; [ce dispatchThreads:MTLSizeMake(u.size[0] >> u.ao_shift, u.size[1] >> u.ao_shift, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            [g_rtx.ct setAddress:g_rtx.aoraw.gpuAddress atIndex:1]; [g_rtx.ct setAddress:plane atIndex:2]; [ce setComputePipelineState:g_rtx.blurpso]; [ce dispatchThreads:MTLSizeMake(u.size[0] >> u.ao_shift, u.size[1] >> u.ao_shift, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
        }
        // 6. the first four local lights of each pixel's tile: rays at half the picture each way, then the same box
        int lon = 0;
        if (g_rtx.local && u.tiles[0] && u.tiles[1] && u.tiles[0] <= 64 && u.tiles[1] <= 64) {
            const p_bind_buf *sc = &S.ssbo[1][4], *sl = &S.ssbo[1][5]; void *mb; uint64_t mo, gc = 0, gl = 0, trid = 0; void *tv = NULL; int tid = (int)(S.tex[1][8] & 0xfffff); if (tid) trid = tex_fast_rid(tid, &tv);
            if (sc->addr && sl->addr && trid && mtl_gpu_addr(sc->addr, sc->size ? sc->size : 1, &mb, &mo, &gc) && mtl_gpu_addr(sl->addr, sl->size ? sl->size : 1, &mb, &mo, &gl)) {
                uint64_t plane = support_gpu() + 4096 + 2 * (uint64_t)u.size[0] * u.size[1];
                [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
                [g_rtx.ct setTexture:g_mv.depth.gpuResourceID atIndex:0]; [g_rtx.ct setTexture:(MTLResourceID){trid} atIndex:1]; [g_rtx.ct setAddress:u_gpu atIndex:0]; [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1]; [g_rtx.ct setAddress:g_rtx.lraw.gpuAddress atIndex:2]; [g_rtx.ct setAddress:gc atIndex:3]; [g_rtx.ct setAddress:gl atIndex:4];
                [ce setComputePipelineState:g_rtx.lpso]; [ce dispatchThreads:MTLSizeMake(u.size[0] >> 1, u.size[1] >> 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
                [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
                [g_rtx.ct setAddress:g_rtx.lraw.gpuAddress atIndex:1]; [g_rtx.ct setAddress:plane atIndex:2]; [ce setComputePipelineState:g_rtx.lblurpso]; [ce dispatchThreads:MTLSizeMake(u.size[0] >> 1, u.size[1] >> 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
                lon = 1;
            } else { static int once; if (!once++) MLOG("ray tracing: the local light lists aren't bound (shader %s)", g_rtx_who); }
        }
        // 7. the mirror direction, off flat surfaces: what's there, read from last frame's picture, still in the forward pass's own colour target since that pass hasn't opened. A surface that isn't in the picture gives nothing
        int ron = 0; uint32_t refl_off = (uint32_t)((4096 + 3 * (uint64_t)u.size[0] * u.size[1] + (((uint64_t)u.size[0] * u.size[1]) >> (2 * u.key_shift)) + 255) & ~(uint64_t)255);
        if (g_rtx.refl_on) {
            {
                uint64_t plane = support_gpu() + refl_off;   // four channels: on a four-byte boundary, unlike what the plane before it leaves
                [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
                [g_rtx.ct setTexture:g_mv.depth.gpuResourceID atIndex:0]; [g_rtx.ct setAddress:u_gpu atIndex:0]; [g_rtx.ct setResource:g_rtx.tlas.gpuResourceID atBufferIndex:1]; [g_rtx.ct setAddress:g_rtx.rraw.gpuAddress atIndex:2];
                [ce setComputePipelineState:g_rtx.rpso]; [ce dispatchThreads:MTLSizeMake(u.size[0] >> 1, u.size[1] >> 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
                [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
                [g_rtx.ct setAddress:g_rtx.rraw.gpuAddress atIndex:1]; [g_rtx.ct setAddress:plane atIndex:2]; [ce setComputePipelineState:g_rtx.rbpso]; [ce dispatchThreads:MTLSizeMake(u.size[0] >> 1, u.size[1] >> 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
                ron = 1;
            }
        }
        uint32_t *hdr = (uint32_t *)((uint8_t *)support_buf().contents + 2048); hdr[1] = u.size[0]; hdr[2] = u.size[1]; hdr[3] = g_rtx.ao_rays > 0; hdr[4] = (uint32_t)g_rtx.ao_shift; hdr[5] = (uint32_t)lon; hdr[6] = (uint32_t)g_rtx.key_shift; hdr[7] = (uint32_t)con; hdr[8] = (uint32_t)ron; memcpy(&hdr[9], &g_rtx.refl_k, 4); hdr[10] = refl_off; memcpy(&hdr[11], &g_rtx.refl_seen, 4); hdr[12] = (uint32_t)g_rtx.show; { float wt_ = dm_env_wetness(); memcpy(&hdr[13], &wt_, 4); }
        { float ib = dm_env_ibl_intensity(), ex = 1, rot[9]; ubo_float(1, (uint16_t)(10 << 8), &ex);
          { static float seen_ib = -1; if (ib != seen_ib) { seen_ib = ib; MLOG("ray tracing: image lighting %.3f, exposure %.3f", ib, ex); } }
          for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) rot[i * 3 + k] = u.iview[i * 4 + k]; memcpy(&hdr[15], &ib, 4); memcpy(&hdr[16], &ex, 4); memcpy(&hdr[17], rot, sizeof rot); }
        hdr[0] = key_rays ? (uint32_t)g_rtx.mode : 3;
    }
    m4_end(ce);
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    if (++g_rtx.frames % 600 == 1) MLOG("ray tracing: frame %llu, %d objects, %d rebuilt this frame; %llu builds, %llu triangles, %llu reused so far; structures %.1f MiB", (unsigned long long)frame, nu, nt, (unsigned long long)g_rtx.built_blas, (unsigned long long)g_rtx.built_tris, (unsigned long long)g_rtx.reused, g_rtx.as_bytes / 1048576.0);
    // a safety net: past this, everything off the picture goes now rather than after its time
    if (g_rtx.as_bytes > (size_t)RT_AS_MAX_MB << 20) { NSMutableArray *gone = [NSMutableArray array]; for (NSNumber *k in g_rtx.blas) { RtBlas *b = g_rtx.blas[k]; if (b->last != frame) { if (b->as) { g_rtx.as_bytes -= b->bytes; fg_bury(b->as); } [gone addObject:k]; } } [g_rtx.blas removeObjectsForKeys:gone]; MLOG("ray tracing: structures over %d MiB, %lu off the picture dropped", RT_AS_MAX_MB, (unsigned long)gone.count); }
    // what hasn't been drawn for a while goes
    if (g_rtx.frames % 120 == 0) { NSMutableArray *gone = [NSMutableArray array]; for (NSNumber *k in g_rtx.blas) { RtBlas *b = g_rtx.blas[k]; if (frame - b->last > RT_KEEP_FRAMES) { if (b->as) { g_rtx.as_bytes -= b->bytes; fg_bury(b->as); } [gone addObject:k]; } } [g_rtx.blas removeObjectsForKeys:gone]; }
}
static void rtx_who(const char *k) { g_rtx_who = k; }
// Replay thread, at the start of a submission: the setting. Off, the shaders go back to the map at once and everything goes down
static void rtx_sub_begin(void) {
    int on = dm_video_rt(); if (on == g_rtx_on) return;
    g_rtx_on = on; MLOG("ray tracing: %s", on ? "wanted" : "no longer wanted");
    if (on) { g_rtx.mode = on; return; }
    rtx_mask_off();
    if (!g_rtx.inited) return;
    for (NSNumber *k in g_rtx.blas) fg_bury(g_rtx.blas[k]->as);
    [g_rtx.blas removeAllObjects]; fg_bury(g_rtx.tlas); g_rtx.tlas = nil; g_rtx.tlas_bytes = 0; g_rtx.as_bytes = 0;
    fg_bury(g_rtx.cap); fg_bury(g_rtx.scratch); fg_bury(g_rtx.inst); fg_bury(g_rtx.unif); fg_bury(g_rtx.aoraw); fg_bury(g_rtx.lraw); fg_bury(g_rtx.kraw); fg_bury(g_rtx.craw); fg_bury(g_rtx.rraw); g_rtx.cap = g_rtx.scratch = g_rtx.inst = g_rtx.unif = g_rtx.aoraw = g_rtx.lraw = g_rtx.kraw = g_rtx.craw = g_rtx.rraw = nil;
    g_rtx.inited = 0; g_rtx.bad = 0; g_rtx.built_frame = 0; memset(g_rt_slot, 0, sizeof g_rt_slot);
}
