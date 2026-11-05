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
    view-projection at buffer(3). Ray tracing builds its geometry from these (nvn_rt.inc). Needs velocity_entry's dm_main0 helper, so it runs after it."""
    m = re.search(r'^vertex main0_out main0\((.*)\)\n\{\n', src, re.M)
    if not m: raise ValueError('the vertex entry main0 was not found')
    params = m.group(1).split(', ')
    plain = [re.sub(r' \[\[[^]]*\]\]$', '', p) for p in params]; names = [p.rsplit(' ', 1)[1] for p in plain]
    vid = next((p.rsplit(' ', 2)[-2].strip('& ') for p in params if '[[vertex_id]]' in p), None)
    extra = '' if vid else ', uint dm_vid [[vertex_id]]'
    return (src.rstrip('\n') + '\n\n'
            f'vertex void dm_capture({m.group(1)}{extra}, device float4* dm_out [[buffer(2)]], constant float4x4& dm_ivp [[buffer(3)]])\n{{\n'
            f'    main0_out c = dm_main0({", ".join(names)});\n    float4 w = dm_ivp * c.gl_Position;\n    dm_out[{vid or "dm_vid"}] = float4(w.xyz / w.w, 1.0);\n}}\n')


# The forward shaders whose shadow reads take the ray-traced mask: what is opaque in the depth prepass. Glass, water, lava, blends and the atmosphere see what's behind them at that pixel, so they keep the map
RT_SHADOW_NAMES = ('ps_models_p', 'ps_terrain', 'ps_hair_p', 'ps_alpha_threshold_p', 'ps_foliage_p', 'ps_detail_normal_p', 'ps_grunge_p', 'ps_default', 'ps_grass_lighting_p')


def rt_shadow_hook(src):
    """Each compare on the directional shadow map (reg 39, the texture this name stands for in every package) also takes the ray-traced mask, in the support buffer past Ryujinx's layout, when it is on and the
    layer is the key light's (0): the darker of the two, or the mask alone with header word 0 at 2. nvn_rt.inc writes it; off, the header reads 0 and the result is the map's alone."""
    pat = re.compile(r'(float _\d+ = )(spvDepthCast\(spvDescriptorSet0\.fp_t_cb196610_105980A8\)\.sample_compare\(spvDescriptorSet0\.fp_t_cb196610_105980A8Smplr, (_\d+)\.xy, uint\(rint\(\3\.z\)\), _\d+\));')
    if not pat.search(src): return src
    m = re.search(r'^fragment \w+ main0\(([^\n]*)\)\n', src, re.M)
    if not m: raise ValueError('the fragment entry main0 was not found')
    if '[[position]]' not in m.group(1): src = src[:m.end(1)] + ', float4 gl_FragCoord [[position]]' + src[m.end(1):]
    elif 'gl_FragCoord [[position]]' not in m.group(1): raise ValueError('the fragment position has another name')
    src = pat.sub(lambda x: f'{x.group(1)}dm_rt_key(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord, {x.group(2)}, uint(rint({x.group(3)}.z)));', src)
    helper = ('static inline float dm_rt_key(constant uchar* sb, float4 fc, float cmp, uint layer)\n{\n'
              '    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u || layer != 0u) return cmp;\n'
              '    uint x = min(uint(fc.x), h[1] - 1u), y = min(uint(fc.y), h[2] - 1u);\n    float mask = float(sb[4096u + y * h[1] + x]) / 255.0;\n    return h[0] == 2u ? mask : min(cmp, mask);\n}\n\n')
    e = re.search(r'^fragment \w+ main0\(', src, re.M)
    return src[:e.start()] + helper + src[e.start():]


def rt_ao_hook(src):
    """The opaque colour takes the ray-traced ambient occlusion (a first look: the whole colour, not only its ambient terms). Off, header word 3 reads 0 and the colour is untouched."""
    if 'dm_rt_key(' not in src: return src   # rt_shadow_hook found no shadow read here, and gives the position this needs
    out = re.search(r'struct main0_out\s*\{[^}]*?float4 (\w+) \[\[color\(0\)\]\]', src, re.S)
    if not out: raise ValueError('the fragment has no colour output')
    helper = ('static inline float dm_rt_ao(constant uchar* sb, float4 fc)\n{\n    constant uint* h = reinterpret_cast<constant uint*>(sb + 2048);\n    if (h[0] == 0u || h[3] == 0u) return 1.0;\n'
              '    uint x = min(uint(fc.x), h[1] - 1u), y = min(uint(fc.y), h[2] - 1u);\n    return float(sb[4096u + h[1] * h[2] + y * h[1] + x]) / 255.0;\n}\n\n')
    src = src.replace('return out;', f'out.{out.group(1)}.xyz *= dm_rt_ao(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord);\n    return out;')
    e = re.search(r'^fragment \w+ main0\(', src, re.M)
    return src[:e.start()] + helper + src[e.start():]


RULES = [(hdr_switch_split, lambda name, key: key in HDR_SWITCH_KEYS), (velocity_entry, lambda name, key: key in VELOCITY_VS_KEYS), (capture_entry, lambda name, key: key in VELOCITY_VS_KEYS),
         (rt_shadow_hook, lambda name, key: name.split('.')[0].split('#')[0] in RT_SHADOW_NAMES), (rt_ao_hook, lambda name, key: name.split('.')[0].split('#')[0] in RT_SHADOW_NAMES)]
