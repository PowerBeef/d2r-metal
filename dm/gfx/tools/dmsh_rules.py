#!/usr/bin/env python3
# MSL rewrite rules for the B shader library: dmsh_build.py applies them to spirv-cross's output before the Metal compile.
# A rule is (function, applies(name, key)). The function takes a shader's MSL and returns the rewritten MSL, or raises ValueError when the code it
# rewrites is missing (that shader then fails the build, so a translator change cannot drop a rule silently). The source of every rule that applies to
# a shader is part of that shader's cache fingerprint (dmsh_build.shader_rule_source), so editing a rule rebuilds only its shaders: keep each rule one
# self-contained function. This file is also in the chain version (dmsh_chain.FILES).
import re

# The fragment shaders that read the game's HDR output switch (c13[51].w, 0 on this port: the Switch has no HDR output). e5fb9d7722af87ad is the
# SMAA edge pass, which measured 0.68 ms against FXAA's 0.20 ms at Recommended on the Mac and ~18% of the iPhone's GPU work with the blend pass.
HDR_SWITCH_KEYS = frozenset({'026014cb2c3e2f90', '05fa4ac2ebf8af59', '618342d86ec36c81', '776420a8fa59672e', '9a650561e5c40791', 'ca25698067fe0e78',
                             'e5fb9d7722af87ad'})


def hdr_switch_split(src):
    """Split the entry function at the HDR output switch: the code after it runs as `if (switch) { as before } else { the same with the switch false }`.
    These shaders compute the PQ curve (SMPTE ST 2084) for every sample and only then keep it or the SDR value by the switch, and the Metal compiler does
    not move that work under the test. In the else copy the switch is a constant, so the curve is dead code. Each copy computes what the original did
    for its switch value."""
    decl = list(re.finditer(r'^( +)bool (_\d+) = 0 != as_type<int>\(\(\*spvDescriptorSet0\.fp_c13\)\._m0\[51\]\.w\);\n', src, re.M))
    if len(decl) != 1: raise ValueError(f'HDR switch read found {len(decl)} times, expected once')
    m = decl[0]
    entry = re.search(r'^(fragment|vertex|kernel) \w+ main0\(', src, re.M)
    end = src.find('\n}\n', m.end() - 1)   # the entry function's closing brace (spirv-cross indents everything inside it)
    if not entry or entry.start() > m.start() or end < 0: raise ValueError('HDR switch read is not in the entry function')
    ind, var, rest = m.group(1), m.group(2), src[m.end():end + 1]
    def block(body): return ''.join(ind + '    ' + line if line.strip() else line for line in body.splitlines(True))
    off = re.sub(r'\b%s\b' % var, 'false', rest)
    return src[:m.end()] + f'{ind}if ({var})\n{ind}{{\n' + block(rest) + f'{ind}}}\n{ind}else\n{ind}{{\n' + block(off) + f'{ind}}}' + src[end:]


# The vertex shaders that read the per-object block (vp_c3): the models, characters, terrain and clutter the depth prepass draws, 39 of the 2344 in the
# game's packages. velocity_entry gives them the motion-vector entry the replay layer's velocity pass draws with (nvn_mv.inc).
VELOCITY_VS_KEYS = frozenset("""
00da6be561241990 06ece8336c8c242c 0fa4eb7559f1637a 0fbb232e68f9fbfa 1a3a3e8ff0efc07a 1d97d33ab66abcba 24373179821a89b8 36623b65377c7269 3deaf68fd65b5298
4dffa355f2fbcdaf 4e8457679851b223 54c93fa22abff9ee 54f730a6c0efc118 59c7313a9385b4d6 5b39fba13f4ce7d5 64dd7d022f3fae39 67e2ebc28c830f44 69c8f107f5173820
7f10e4162e65cdb4 7fbab804c6ba3dcf 8160c480b3a4cab0 8ac93c54cfb3cbe3 96757bb36a024b91 a9a839a220417792 add86ee8e20f89b9 af62495001e7d572 af64b417689666bf
b28286da8956e9a4 b3ae9be5cc670f35 b76211745124af86 c44a5c9fab563126 d5e92ababb689713 e92943104b293de9 ed8df20b114afc67 f70970fd3d9d4f2c f88d27f131d71049
f9c6bce55364f980 fbf6dc1873a449e9 ff91743a2d7561cc""".split())


def velocity_entry(src):
    """Add a second vertex entry, dm_velocity, after the shader's own, which stays as it is. It runs a copy of the shader's body twice, on the constants
    in buffer(0) and on last frame's in buffer(1) (the same argument block layout), and hands the rasterizer both clip positions: the position as the
    shader computes it, so the velocity pass's depth-equal test holds against the depth prepass, and the previous one for the fragment's motion vector.
    Skinning, the object's transform and the camera all come from the shader's own code."""
    m = re.search(r'^vertex main0_out main0\((.*)\)\n\{\n', src, re.M)
    if not m: raise ValueError('the vertex entry main0 was not found')
    params = m.group(1).split(', ')
    if not params[0].endswith('in [[stage_in]]') or 'spvDescriptorSet0 [[buffer(0)]]' not in params[1] or '[[buffer(1)]]' in m.group(1):
        raise ValueError(f'unexpected vertex entry parameters: {m.group(1)}')
    end = src.find('\n}\n', m.end() - 1)
    if end < 0: raise ValueError('the end of the vertex entry was not found')
    plain = [re.sub(r' \[\[[^]]*\]\]$', '', p) for p in params]                # the parameters without their attributes, for the helper
    names = [p.rsplit(' ', 1)[1] for p in plain]
    cur = ', '.join(names); prev = ', '.join('dm_prev' if n == 'spvDescriptorSet0' else n for n in names)
    return (src.rstrip('\n') + '\n\n'
            f'static inline __attribute__((always_inline)) main0_out dm_main0({", ".join(plain)})\n{{\n' + src[m.end():end + 1] + '}\n\n'
            'struct dm_vel_out\n{\n    float4 gl_Position [[position, invariant]];\n    float4 dm_cur [[user(dm_cur)]];\n    float4 dm_prev [[user(dm_prev)]];\n};\n\n'
            f'vertex dm_vel_out dm_velocity({m.group(1)}, constant spvDescriptorSetBuffer0& dm_prev [[buffer(1)]])\n{{\n'
            f'    main0_out c = dm_main0({cur});\n    main0_out p = dm_main0({prev});\n'
            '    dm_vel_out o;\n    o.gl_Position = c.gl_Position;\n    o.dm_cur = c.gl_Position;\n    o.dm_prev = p.gl_Position;\n    return o;\n}\n')


def capture_entry(src):
    """Add a vertex entry, dm_capture, for a pipeline with no rasterization: the shader's own body, and the world position it computes written by vertex id to the buffer at buffer(2), through the inverse
    view-projection at buffer(3). Ray tracing builds its geometry from these (nvn_rt.inc). A vertex with no finite position (w 0, as some have in the first frames of a level) comes out NaN, which leaves its triangle out of the structure: an infinite one must not reach the builder. Needs velocity_entry's dm_main0 helper, so it runs after it."""
    m = re.search(r'^vertex main0_out main0\((.*)\)\n\{\n', src, re.M)
    if not m: raise ValueError('the vertex entry main0 was not found')
    params = m.group(1).split(', ')
    plain = [re.sub(r' \[\[[^]]*\]\]$', '', p) for p in params]; names = [p.rsplit(' ', 1)[1] for p in plain]
    vid = next((p.rsplit(' ', 2)[-2].strip('& ') for p in params if '[[vertex_id]]' in p), None)
    extra = '' if vid else ', uint dm_vid [[vertex_id]]'
    return (src.rstrip('\n') + '\n\n'
            f'vertex void dm_capture({m.group(1)}{extra}, device float4* dm_out [[buffer(2)]], constant float4x4& dm_ivp [[buffer(3)]])\n{{\n'
            f'    main0_out c = dm_main0({", ".join(names)});\n    float4 w = dm_ivp * c.gl_Position;\n    float3 p = w.xyz / w.w;\n    dm_out[{vid or "dm_vid"}] = all(abs(p) < 1000000.0) ? float4(p, 1.0) : float4(as_type<float>(0x7fc00000u));\n}}\n')


# The forward shaders whose shadow reads take the ray-traced mask: what is opaque in the depth prepass. Glass, water, lava, blends and the atmosphere see what's behind them at that pixel, so they keep the map
RT_SHADOW_NAMES = ('ps_models_p', 'ps_terrain', 'ps_hair_p', 'ps_alpha_threshold_p', 'ps_foliage_p', 'ps_detail_normal_p', 'ps_grunge_p', 'ps_default', 'ps_grass_lighting_p')


def rt_shadow_hook(src):
    """Each compare on the directional shadow map (reg 39, the texture this name stands for in every package) also takes the ray-traced mask, in the support buffer past Ryujinx's layout, when it is on and the
    layer is the key light's (0): the darker of the two, or the mask alone with header word 0 at 2; at 3 no mask was traced. nvn_rt.inc writes it; off, the header reads 0 and the result is the map's alone. Layer 1, the characters' own shadow, takes the darker of its map and the characters' rays when header word 7 says they ran."""
    pat = re.compile(r'(float _\d+ = )(spvDepthCast\(spvDescriptorSet0\.fp_t_cb196610_105980A8\)\.sample_compare\(spvDescriptorSet0\.fp_t_cb196610_105980A8Smplr, (_\d+)\.xy, uint\(rint\(\3\.z\)\), _\d+\));')
    if not pat.search(src): return src
    m = re.search(r'^fragment \w+ main0\(([^\n]*)\)\n', src, re.M)
    if not m: raise ValueError('the fragment entry main0 was not found')
    if '[[position]]' not in m.group(1): src = src[:m.end(1)] + ', float4 gl_FragCoord [[position]]' + src[m.end(1):]
    elif 'gl_FragCoord [[position]]' not in m.group(1): raise ValueError('the fragment position has another name')
    src = pat.sub(lambda x: f'{x.group(1)}dm_rt_key(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord, {x.group(2)}, uint(rint({x.group(3)}.z)));', src)
    helper = ('static inline float dm_rt_ao(constant uchar* sb, float4 fc);\nstatic inline float dm_rt_key(constant uchar* sb, float4 fc, float cmp, uint layer)\n{\n'
              '    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u) return cmp;\n    if (layer != 0u) { if (h[7] == 0u) return cmp; uint cw = h[1] >> h[6], cx = min(uint(fc.x) >> h[6], cw - 1u), cy = min(uint(fc.y) >> h[6], (h[2] >> h[6]) - 1u); return min(cmp, float(sb[4096u + 3u * h[1] * h[2] + cy * cw + cx]) / 255.0); }\n    if (h[0] == 3u) return cmp / dm_rt_ao(sb, fc);\n'
              '    uint w = h[1] >> h[6], x = min(uint(fc.x) >> h[6], w - 1u), y = min(uint(fc.y) >> h[6], (h[2] >> h[6]) - 1u);\n    float mask = float(sb[4096u + y * w + x]) / 255.0;\n    return (h[0] == 2u ? mask : min(cmp, mask)) / dm_rt_ao(sb, fc);\n}\n\n')
    e = re.search(r'^fragment \w+ main0\(', src, re.M)
    return src[:e.start()] + helper + src[e.start():]


def rt_ao_hook(src):
    """The opaque colour takes the ray-traced ambient occlusion. The translated sum can't be split into its terms, so the key light (rt_shadow_hook) and the local lights (rt_local_hook) divide it back out where they read their shadow, which leaves it on the ambient terms: the fill and back lights and the light grid. Off, header word 3 reads 0 and the colour is untouched."""
    if 'dm_rt_key(' not in src: return src   # rt_shadow_hook found no shadow read here, and gives the position this needs
    out = re.search(r'struct main0_out\s*\{[^}]*?float4 (\w+) \[\[color\(0\)\]\]', src, re.S)
    if not out: raise ValueError('the fragment has no colour output')
    helper = ('static inline float dm_rt_ao(constant uchar* sb, float4 fc)\n{\n    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u || h[3] == 0u) return 1.0;\n'
              '    uint w = h[1] >> h[4], x = min(uint(fc.x) >> h[4], w - 1u), y = min(uint(fc.y) >> h[4], (h[2] >> h[4]) - 1u);\n    return max(float(sb[4096u + h[1] * h[2] + y * w + x]) / 255.0, 0.05);\n}\n\n')
    src = src.replace('return out;', f'out.{out.group(1)}.xyz *= dm_rt_ao(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord);\n    return out;')
    e = re.search(r'^fragment \w+ main0\(', src, re.M)
    return src[:e.start()] + helper + src[e.start():]


def rt_local_hook(src):
    """Torches, spot lights and unit lights take the ray-traced visibility nvn_rt.inc works out for the first four local lights of a pixel's tile. All three loops read their light's position from the same texel buffer,
    and walk the tile's list with a running offset: the offset past the decals is the slot. The point lights have no shadow to hook, so their contribution is scaled where it joins the sum; spot and unit lights
    scale the shadow they already have. Off, header word 5 reads 0 and every light is as it was."""
    if 'dm_rt_key(' not in src: return src
    count = re.search(r'float (_\d+) = as_type<float>\(\(\*spvDescriptorSet0\.fp_s0\)\._m0\[int\(_\d+\)\]\);', src)
    pat = re.compile(r'float3 _\d+ = spvDescriptorSet0\.fp_t_tcb_6A\.read\(uint\(as_type<int>\(_\d+\)\)\)\.xyz;')
    reads = list(pat.finditer(src))
    if not count or len(reads) != 3: raise ValueError('the local light loops are not the three expected')
    sp = re.search(r'float (_\d+) = spvDepthCast\(spvDescriptorSet0\.fp_t_cb196610_105980AA\)\.sample_compare\([^\n]*\n\s*(_\d+) = \1;', src)
    cu = re.search(r'float (_\d+) = spvDepthCast\(spvDescriptorSet0\.fp_t_cb196610_105A009C\)\.sample_compare\([^\n]*\n\s*(_\d+) = \1;', src)
    if not sp or not cu: raise ValueError('the spot and unit light shadows were not found')
    # the point lights' sum: the three values the loop starts from, each joined by one fma
    head = src[max(0, reads[0].start() - 900):reads[0].start()]
    accs = re.findall(r'float (_\d+) = _\d+;', head)[-3:]
    body = src[reads[0].end():reads[1].start()]
    for a in accs:
        body, n = re.subn(r'(float _\d+ = fma\()(_\d+), (_\d+), ' + a + r'\);', lambda m: f'{m.group(1)}{m.group(2)} * dm_lv0, {m.group(3)}, {a});', body)
        if n != 1: raise ValueError('the point lights do not join the sum as expected')
    src = src[:reads[0].end()] + body + src[reads[1].start():]
    for name, m in (('dm_lv1', sp), ('dm_lv2', cu)):
        src, n = re.subn(re.escape(m.group(2)) + r' = ' + re.escape(m.group(1)) + r';', f'{m.group(2)} = {m.group(1)} * {name};', src, count=1)
        src, k = re.subn(r'(else\s*\n\s*\{\s*\n\s*)' + re.escape(m.group(2)) + r' = 1\.0;', lambda x: f'{x.group(1)}{m.group(2)} = {name};', src, count=1)
        if n != 1 or k != 1: raise ValueError('a local shadow does not end as expected')
    out, last = [], 0
    for i, m in enumerate(pat.finditer(src)):
        off = re.findall(r'int (_\d+) = (_\d+) << 2;\n\s*int _\d+ = \1 >> 2;\n\s*int _\d+ = _\d+ << 8;', src[max(0, m.start() - 1600):m.start()])
        if not off: raise ValueError('the walk of the light list was not found')
        out += [src[last:m.end()], f'\n            float dm_lv{i} = dm_rt_local(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord, {off[-1][1]} - (as_type<int>({count.group(1)}) & 63));']
        last = m.end()
    src = ''.join(out) + src[last:]
    helper = ('static inline float dm_rt_local(constant uchar* sb, float4 fc, int slot)\n{\n    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u) return 1.0;\n    if (h[5] == 0u || slot < 0 || slot >= 4) return 1.0 / dm_rt_ao(sb, fc);\n'
              '    uint w = h[1] >> 1, x = min(uint(fc.x) >> 1, w - 1u), y = min(uint(fc.y) >> 1, (h[2] >> 1) - 1u);\n    return float(sb[4096u + 2u * h[1] * h[2] + (y * w + x) * 4u + uint(slot)]) / 255.0 / dm_rt_ao(sb, fc);\n}\n\n')
    e = re.search(r'^fragment \w+ main0\(', src, re.M)
    return src[:e.start()] + helper + src[e.start():]


def rt_env_hook(src):
    """What the Switch's forward shaders leave out and the PC's have: each surface's specular response to its surroundings, for its own normal, roughness and reflectance. The surroundings are what nvn_rt.inc's mirror rays hit, read from last frame's picture, and where they hit nothing the game's own image-based lighting cube for this environment (engine slot 34, which the skybox reads), at the iblIntensity its artists set for that environment and under the rays' occlusion. The split-sum table is Karis's fit. A dielectric's response stops at 0.3. What a draw adds is scaled by its own alpha: the models the artists blend into the ground are drawn in layers, premultiplied, and at full strength they pile up to white. Only a surface that is itself lit shows it, so a surface in the dark stays dark: the light radius is gameplay. A shader whose material terms can't be found is left alone. Off, header word 8 reads 0 and the colour is as it was."""
    if 'dm_rt_key(' not in src: return src
    c1 = r'\(\*spvDescriptorSet0\.fp_c1\)\._m0\[\d+\]\.[xyzw]'
    rough = re.search(r'^    float _\d+ = fast::max\((_\d+), ' + c1 + r'\);', src, re.M)
    out = re.search(r'struct main0_out\s*\{[^}]*?float4 (\w+) \[\[color\(0\)\]\]', src, re.S)
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\n\};', src, re.S)
    if not rough or not out or not body: return src
    # The normal: the one triple that offsets the view vector along it, by a single constant. View x, y, z are declared in that order
    trip = {}
    for a, c, v in re.findall(r'^    float _\d+ = fma\((_\d+), (' + c1 + r'), (_\d+)\);', src, re.M): trip.setdefault(c, []).append((int(v[1:]), a))
    normal = next((sorted(t) for t in trip.values() if len(t) == 3), None)
    if not normal: return src
    # The reflectance at normal incidence: 0.04 toward the albedo by the metalness, three lines; terrain and grass are plain dielectrics
    minus = set(re.findall(r'^    float (_\d+) = _\d+ \+ \(-0\.0399999991059303283691\d*\);', src, re.M))
    metal = {}
    for f, m, a in re.findall(r'^    float (_\d+) = fma\((_\d+), (_\d+), ' + c1 + r'\);', src, re.M):
        if a in minus: metal.setdefault(m, []).append(f)
    mname = next((m for m, v in metal.items() if len(v) >= 3), None)
    f0 = f'float3({metal[mname][0]}, {metal[mname][1]}, {metal[mname][2]})' if mname else 'float3(0.04)'
    # Every return after these exist: a model has two whole lighting paths, each with its own. An earlier return is a path that stops before them
    used = [rough.group(1)] + [t[1] for t in normal] + re.findall(r'_\d+', f0)
    start = max((m.end() for v in used for m in re.finditer(r'\bfloat ' + v + r' = ', src)), default=-1)
    if start < 0 or src.find('return out;', start) < 0: return src
    helper = ('static inline float dm_rt_wet(constant uchar* sb, float r, float metal)\n{\n    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u || h[8] == 0u) return r;\n'
              '    float w = as_type<float>(h[13]) * (1.0 - clamp(metal, 0.0, 1.0)) * (1.0 - smoothstep(0.6, 0.95, r));\n    return r * (1.0 - 0.55 * w);\n}\n\n'
              'static inline float3 dm_rt_env(constant uchar* sb, float4 fc, float rough, float3 f0, float3 n, float3 lit, float alpha, texturecube<float> ibl)\n{\n    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u || h[8] == 0u) return lit;\n'
              '    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge);\n    constant float* hf = reinterpret_cast<constant float*>(sb + 2048);\n'
              '    float r = clamp(rough, 0.0, 1.0);\n    if (!(dot(n, n) > 0.0)) return lit;\n    float3 N = normalize(n);\n    float nv = clamp(N.z, 0.0, 1.0);\n'
              '    float3 R = float3(2.0 * N.z * N.x, 2.0 * N.z * N.y, 2.0 * N.z * N.z - 1.0);\n    float3 W = R.x * float3(hf[17], hf[18], hf[19]) + R.y * float3(hf[20], hf[21], hf[22]) + R.z * float3(hf[23], hf[24], hf[25]);\n'
              '    float3 cube = ibl.sample(s, W, level(r * float(ibl.get_num_mip_levels() - 1))).rgb * hf[15] * hf[16];\n'
              '    uint w = h[1] >> 1, x = min(uint(fc.x) >> 1, w - 1u), y = min(uint(fc.y) >> 1, (h[2] >> 1) - 1u);\n    constant uchar* p = sb + h[10] + (y * w + x) * 4u;\n'
              '    float3 c = float3(p[0], p[1], p[2]) / 255.0;\n    float3 hit = c * c * 16.0, miss = (1.0 - float(p[3]) / 255.0) * cube * dm_rt_ao(sb, fc), env = hit + miss;\n'
              '    float q = 1.0 - nv, q5 = q * q * q * q * q;\n    float3 F = f0 + (max(float3(1.0 - r), f0) - f0) * q5;\n'
              '    float4 r4 = r * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);\n    float a = min(r4.x * r4.x, exp(-6.4325 * nv)) * r4.x + r4.y;\n    float3 spec = F * (-1.04 * a + r4.z) + (1.04 * a + r4.w);\n    spec = min(spec, float3(max(0.3, max(f0.x, max(f0.y, f0.z)))));\n'
              '    float gate = hf[11], seen = gate > 0.0 ? clamp(dot(lit, float3(0.2126, 0.7152, 0.0722)) / gate, 0.0, 1.0) : 1.0;\n    float cover = clamp(alpha, 0.0, 1.0);\n    float3 add = env * spec * hf[9] * seen * cover;\n'
              '    if (h[12] == 1u) return cover * (r < 0.2 ? float3(1.0, 0.0, 0.0) : r < 0.4 ? float3(1.0, 0.45, 0.0) : r < 0.6 ? float3(0.0, 0.8, 0.0) : r < 0.8 ? float3(0.0, 0.3, 1.0) : float3(0.25));\n'
              '    if (h[12] == 2u) return cover * (N * 0.5 + 0.5);\n    if (h[12] == 3u) return add;\n    if (h[12] == 4u) return cover * f0;\n    if (h[12] == 5u) return cover * miss * spec;\n    if (h[12] == 6u) return cover * hit * spec;\n'
              '    return lit + add;\n}\n\n')
    name = 'fp_t_cb196610_1056809E'   # engine slot 34 under the name dmsh_build.reflect reads, as the skybox has it
    ids = [int(x) for x in re.findall(r'\[\[id\((\d+)\)\]\]', body.group(1))]
    call = f'out.{out.group(1)}.xyz = dm_rt_env(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord, dm_rt_wet(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), {rough.group(1)}, {mname or "0.0"}), {f0}, float3({normal[0][1]}, {normal[1][1]}, {normal[2][1]}), out.{out.group(1)}.xyz, out.{out.group(1)}.w, spvDescriptorSet0.{name});\n'
    src = src[:start] + re.sub(r'^( *)return out;', lambda m: m.group(1) + call + m.group(0), src[start:], flags=re.M)
    # Damp stone: the lights' own highlights and the surroundings both see a smoother surface. Metal doesn't change, and what is very rough (earth, cloth, moss) soaks it up
    wet = f'dm_rt_wet(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), {rough.group(1)}, {mname or "0.0"})'
    src = re.sub(r'(^    float _\d+ = fast::max\()' + rough.group(1) + r'(, ' + c1 + r'\);)', lambda m: m.group(1) + wet + m.group(2), src, flags=re.M)
    src = src[:body.end(1)] + f'\n    texturecube<float> {name} [[id({max(ids) + 1})]];' + src[body.end(1):]
    e = re.search(r'^fragment \w+ main0\(', src, re.M)
    return src[:e.start()] + helper + src[e.start():]


RULES = [(hdr_switch_split, lambda name, key: key in HDR_SWITCH_KEYS), (velocity_entry, lambda name, key: key in VELOCITY_VS_KEYS), (capture_entry, lambda name, key: key in VELOCITY_VS_KEYS),
         (rt_shadow_hook, lambda name, key: name.split('.')[0].split('#')[0] in RT_SHADOW_NAMES), (rt_ao_hook, lambda name, key: name.split('.')[0].split('#')[0] in RT_SHADOW_NAMES), (rt_local_hook, lambda name, key: name.split('.')[0].split('#')[0] in RT_SHADOW_NAMES), (rt_env_hook, lambda name, key: name.split('.')[0].split('#')[0] in RT_SHADOW_NAMES)]
