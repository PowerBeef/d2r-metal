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


RULES = [(hdr_switch_split, lambda name, key: key in HDR_SWITCH_KEYS), (velocity_entry, lambda name, key: key in VELOCITY_VS_KEYS)]
