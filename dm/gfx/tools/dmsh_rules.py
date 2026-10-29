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


RULES = [(hdr_switch_split, lambda name, key: key in HDR_SWITCH_KEYS)]
