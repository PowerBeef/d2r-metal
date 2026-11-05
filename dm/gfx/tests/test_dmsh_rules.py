#!/usr/bin/env python3
"""Unit test for dmsh_rules.py, on synthetic MSL shaped like spirv-cross output (no game shader text). The HDR-switch split: the structure, that the
else copy has the switch replaced by false and nothing else changed, the refusals, and (when the Metal compiler is there) that the result compiles and
that the switch-off copy loses the work the switch guarded. The velocity entry: the original entry is untouched, the new entry's structure, the
refusals, and that the result compiles and runs the body on both argument blocks."""
import builtins
import dis
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import dmsh_rules  # noqa: E402
SHADER_MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'nvn', 'shader_map.tsv')


def globals_used(f):
    """Module-level names a function (and the functions, lambdas and comprehensions inside it) reads"""
    out, todo = set(), [f.__code__]
    while todo:
        code = todo.pop()
        out |= {i.argval for i in dis.get_instructions(code) if i.opname in ('LOAD_GLOBAL', 'LOAD_NAME', 'LOAD_FROM_DICT_OR_GLOBALS')}
        todo += [c for c in code.co_consts if isinstance(c, type(code))]
    return out


_K = 3   # the positive control's module-level constant


def _uses_k(s): return s * _K

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct type_c
{
    float4 _m0[64];
};

struct spvDescriptorSetBuffer0
{
    constant type_c* fp_c13 [[id(0)]];
};

struct main0_out
{
    float4 o [[color(0)]];
};

static inline __attribute__((always_inline))
float helper(float x)
{
    return x * 2.0;
}

'''

BODY = '''fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float _10 = gl_FragCoord.x * 0.008000000379979610443115234375;
    bool _12 = 0 != as_type<int>((*spvDescriptorSet0.fp_c13)._m0[51].w);
    float _14 = exp2(log2(abs(_10)) * 0.1593017578125);
    float _16 = exp2(log2(fma(_14, 18.8515625, 0.8359375) / fma(_14, 18.6875, 1.0)) * 78.84375);
    float _18 = _16;
    if (!_12)
    {
        _18 = helper(_10);
    }
    float _120 = _18 + float(_12);
    out.o = float4(_120);
    return out;
}

'''
SRC = HEAD + BODY


VS = '''#include <metal_stdlib>
using namespace metal;

struct type_c
{
    float4 _m0[32];
};

struct spvDescriptorSetBuffer0
{
    constant type_c* vp_c3 [[id(0)]];
    constant type_c* vp_c6 [[id(1)]];
};

struct main0_out
{
    float4 gl_Position [[position, invariant]];
};

struct main0_in
{
    float4 m_0 [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    float _1 = in.m_0.x * (*spvDescriptorSet0.vp_c3)._m0[0].x;
    out.gl_Position.x = _1 * (*spvDescriptorSet0.vp_c6)._m0[21].x;
    out.gl_Position.w = 1.0;
    return out;
}

'''


class HdrSwitchSplit(unittest.TestCase):
    def test_structure(self):
        out = dmsh_rules.hdr_switch_split(SRC)
        self.assertTrue(out.startswith(SRC[:SRC.index('    float _14')]), 'everything up to and including the switch read is unchanged')
        self.assertEqual(out.count('return out;'), 2)
        m = re.search(r'\n    if \(_12\)\n    \{\n(.*?)\n    \}\n    else\n    \{\n(.*?)\n    \}\n\}\n', out, re.S)
        assert m is not None
        on, off = m.group(1), m.group(2)
        self.assertIn('if (!_12)', on)
        self.assertIn('float(_12)', on)
        self.assertIsNone(re.search(r'\b_12\b', off))
        self.assertIn('if (!false)', off)
        self.assertIn('float(false)', off)
        self.assertIn('float _120 = ', off, 'only whole names are replaced')
        self.assertEqual(off.replace('false', '_12'), on, 'the off copy differs only by the switch')

    def test_refusals(self):
        with self.assertRaises(ValueError): dmsh_rules.hdr_switch_split(SRC.replace('_m0[51].w', '_m0[50].w'))
        twice = SRC.replace('    float _14', '    bool _13 = 0 != as_type<int>((*spvDescriptorSet0.fp_c13)._m0[51].w);\n    float _14')
        with self.assertRaises(ValueError): dmsh_rules.hdr_switch_split(twice)
        in_helper = HEAD.replace('    return x * 2.0;', '    bool _12 = 0 != as_type<int>((*spvDescriptorSet0.fp_c13)._m0[51].w);\n    return x * 2.0;') + BODY.replace(
            '    bool _12 = 0 != as_type<int>((*spvDescriptorSet0.fp_c13)._m0[51].w);\n', '    bool _12 = false;\n')
        with self.assertRaises(ValueError): dmsh_rules.hdr_switch_split(in_helper)

    def test_rule_table(self):
        self.assertEqual(len(dmsh_rules.HDR_SWITCH_KEYS), 7)
        self.assertTrue(all(re.fullmatch(r'[0-9a-f]{16}', k) for k in dmsh_rules.HDR_SWITCH_KEYS))
        (f, applies), (g, applies2), (h, _), (i, applies3), (j, applies4), (k, _) = dmsh_rules.RULES
        self.assertIs(k, dmsh_rules.rt_local_hook)
        self.assertIs(h, dmsh_rules.capture_entry)
        self.assertIs(i, dmsh_rules.rt_shadow_hook)
        self.assertIs(j, dmsh_rules.rt_ao_hook)
        for a in (applies3, applies4):
            self.assertTrue(a('ps_terrain.bin#0', 'any'))
            self.assertTrue(a('ps_models_p.bin#3', 'any'))
            self.assertFalse(a('ps_glass_p.bin#0', 'any'))
        self.assertIs(f, dmsh_rules.hdr_switch_split)
        self.assertTrue(applies('any', 'e5fb9d7722af87ad'))
        self.assertFalse(applies('any', '3979464914c7fede'))
        self.assertIs(g, dmsh_rules.velocity_entry)
        self.assertEqual(len(dmsh_rules.VELOCITY_VS_KEYS), 39)
        self.assertTrue(all(re.fullmatch(r'[0-9a-f]{16}', k) for k in dmsh_rules.VELOCITY_VS_KEYS))
        self.assertTrue(applies2('any', '3deaf68fd65b5298'))
        self.assertFalse(applies2('any', 'e5fb9d7722af87ad'))

    def test_rt_hooks(self):
        read = 'spvDepthCast(spvDescriptorSet0.fp_t_cb196610_105980A8).sample_compare(spvDescriptorSet0.fp_t_cb196610_105980A8Smplr, _7.xy, uint(rint(_7.z)), _9)'
        src = ('struct main0_out\n{\n    float4 m_5 [[color(0)]];\n};\n\nfragment main0_out main0(float4 a [[stage_in]])\n{\n    main0_out out = {};\n    float _3 = ' + read + ';\n    return out;\n}\n')
        one = dmsh_rules.rt_shadow_hook(src)
        self.assertIn('float4 gl_FragCoord [[position]]', one)
        self.assertIn('float _3 = dm_rt_key(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord, ' + read + ', uint(rint(_7.z)));', one)
        two = dmsh_rules.rt_ao_hook(one)
        self.assertIn('out.m_5.xyz *= dm_rt_ao(', two)
        self.assertLess(two.index('out.m_5.xyz *='), two.index('return out;'))
        self.assertLess(two.index('static inline float dm_rt_ao('), two.index('fragment main0_out main0('))
        # no shadow read: neither touches the shader
        plain = src.replace(read, '1.0')
        self.assertEqual(dmsh_rules.rt_shadow_hook(plain), plain)
        self.assertEqual(dmsh_rules.rt_ao_hook(plain), plain)

    def test_rt_local_hook(self):
        # the three loops as every forward shader has them: the running offset, the light's position, then how its contribution or its shadow ends
        walk = lambda n: f'int _{n}1 = _{n}0 << 2;\n    int _{n}2 = _{n}1 >> 2;\n    int _{n}3 = _9 << 8;\n'
        pos = lambda n: f'float3 _{n}4 = spvDescriptorSet0.fp_t_tcb_6A.read(uint(as_type<int>(_{n}5))).xyz;\n'
        cmp = lambda t, n: f'float _7{n} = spvDepthCast(spvDescriptorSet0.fp_t_cb196610_{t}).sample_compare(s, c, 0);\n    _8{n} = _7{n};\n    else\n    {{\n        _8{n} = 1.0;\n'
        src = ('fragment main0_out main0()\n{\n    float _5 = as_type<float>((*spvDescriptorSet0.fp_s0)._m0[int(_6)]);\n    dm_rt_key(\n'
               '    float _21 = _31;\n    float _22 = _32;\n    float _23 = _33;\n' + walk(10) + pos(10) +
               '    float _41 = fma(_51, _61, _21);\n    float _42 = fma(_51, _62, _22);\n    float _43 = fma(_51, _63, _23);\n' + walk(11) + pos(11) + cmp('105980AA', 1) + walk(12) + pos(12) + cmp('105A009C', 2) + '}\n')
        src = 'x\n' + src
        out = dmsh_rules.rt_local_hook(src)
        self.assertIn('static inline float dm_rt_local(', out)
        for i, off in enumerate(('_100', '_110', '_120')):
            self.assertIn(f'float dm_lv{i} = dm_rt_local(reinterpret_cast<constant uchar*>(spvDescriptorSet0.support_buffer), gl_FragCoord, {off} - (as_type<int>(_5) & 63));', out)
        for acc in ('_21', '_22', '_23'):
            self.assertRegex(out, r'fma\(_51 \* dm_lv0, _6\d, ' + acc + r'\);')
        self.assertIn('_81 = _71 * dm_lv1;', out); self.assertIn('_81 = dm_lv1;', out)
        self.assertIn('_82 = _72 * dm_lv2;', out); self.assertIn('_82 = dm_lv2;', out)
        self.assertEqual(dmsh_rules.rt_local_hook('no shadow read'), 'no shadow read')
        with self.assertRaises(ValueError): dmsh_rules.rt_local_hook('dm_rt_key( but no light loops')

    def test_rules_self_contained(self):
        # The shader cache fingerprints only each applying rule's own text (dmsh_build.shader_rule_source), and the chain version leaves dmsh_rules.py
        # out of the code form: a module-level helper or constant used inside a rule could change without changing any shader's digest, and the
        # cache would hand back stale MSL. So a rule may use only re and builtins
        allowed = {'re'} | set(vars(builtins))
        self.assertEqual(globals_used(_uses_k) - allowed, {'_K'}, 'positive control: a module-level name must be seen')
        for f, _ in dmsh_rules.RULES:
            self.assertEqual(globals_used(f) - allowed, set(), f'{f.__name__} reads module-level names: the cache fingerprint covers only the function\'s own text'
                             ' (dmsh_build.shader_rule_source), so a rule may use only re and builtins')

    def test_keys_in_shader_map(self):
        # Each HDR-switch key against the package it belongs to: the SMAA edge pass, the three variants of the final compose pass (drawn every frame) and
        # the three SMAA resolve variants (AA modes 3-5, never drawn here). The compose keys appear on two rows each (key conflicts), hence the sets
        pk = {}
        with open(SHADER_MAP, encoding='utf-8') as f:
            for line in f:
                if line.startswith('#') or line.startswith('package\t'): continue
                c = line.rstrip('\n').split('\t')
                if len(c) > 3: pk.setdefault(c[3], set()).add(c[0])
        missing = sorted(k for k in dmsh_rules.HDR_SWITCH_KEYS if k not in pk)
        self.assertEqual(missing, [], 'HDR_SWITCH_KEYS has a key no shader has; the rule would silently skip it')
        self.assertEqual({k: sorted(pk[k]) for k in dmsh_rules.HDR_SWITCH_KEYS}, {
            'e5fb9d7722af87ad': ['ps_smaa_edge_detection_p.bin'],
            '05fa4ac2ebf8af59': ['ps_compose_p.bin'], '9a650561e5c40791': ['ps_compose_p.bin'], '618342d86ec36c81': ['ps_compose_p.bin'],
            '026014cb2c3e2f90': ['ps_smaa_resolve_p.bin'], '776420a8fa59672e': ['ps_smaa_resolve_p.bin'], 'ca25698067fe0e78': ['ps_smaa_resolve_p.bin']})
        missing = sorted(k for k in dmsh_rules.VELOCITY_VS_KEYS if k not in pk)
        self.assertEqual(missing, [], 'VELOCITY_VS_KEYS has a key no shader has; the rule would silently skip it')

    def test_velocity_entry(self):
        out = dmsh_rules.velocity_entry(VS)
        self.assertTrue(out.startswith(VS.rstrip('\n')), 'the shader\'s own entry stays as it is')
        self.assertEqual(out.count('vertex main0_out main0('), 1)
        self.assertIn('main0_out dm_main0(main0_in in, constant spvDescriptorSetBuffer0& spvDescriptorSet0)', out)
        self.assertIn('vertex dm_vel_out dm_velocity(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], constant spvDescriptorSetBuffer0& dm_prev [[buffer(1)]])', out)
        self.assertIn('main0_out c = dm_main0(in, spvDescriptorSet0);', out)
        self.assertIn('main0_out p = dm_main0(in, dm_prev);', out)
        self.assertIn('float4 gl_Position [[position, invariant]];', out[out.index('struct dm_vel_out'):])
        inst = VS.replace('[[buffer(0)]])', '[[buffer(0)]], uint gl_InstanceIndex [[instance_id]], uint gl_BaseInstance [[base_instance]])')
        out2 = dmsh_rules.velocity_entry(inst)
        self.assertIn('main0_out dm_main0(main0_in in, constant spvDescriptorSetBuffer0& spvDescriptorSet0, uint gl_InstanceIndex, uint gl_BaseInstance)', out2)
        self.assertIn('main0_out p = dm_main0(in, dm_prev, gl_InstanceIndex, gl_BaseInstance);', out2)

    def test_velocity_refusals(self):
        with self.assertRaises(ValueError): dmsh_rules.velocity_entry(VS.replace('vertex main0_out main0(', 'vertex main0_out other('))
        with self.assertRaises(ValueError): dmsh_rules.velocity_entry(VS.replace('[[buffer(0)]])', '[[buffer(0)]], constant float& x [[buffer(1)]])'))
        with self.assertRaises(ValueError): dmsh_rules.velocity_entry(VS.replace('spvDescriptorSet0 [[buffer(0)]]', 'spvDescriptorSet0 [[buffer(2)]]'))

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Metal compiler')
    def test_velocity_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'v.metal')
            with open(p, 'w') as f: f.write(dmsh_rules.velocity_entry(VS))
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-S', '-emit-llvm', '-fpreserve-invariance', p, '-o', p[:-6] + '.ll'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-600:])
            with open(p[:-6] + '.ll') as f: ir = f.read()
            self.assertIn('@dm_velocity', ir)
            self.assertIn('@main0', ir)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Metal compiler')
    def test_compiles_and_drops_curve(self):
        with tempfile.TemporaryDirectory() as d:
            def ir(src, name):
                p = os.path.join(d, name + '.metal')
                with open(p, 'w') as f: f.write(src)
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-S', '-emit-llvm', '-fpreserve-invariance', p, '-o', p[:-6] + '.ll'], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-400:])
                with open(p[:-6] + '.ll') as f: return f.read()
            calls = lambda t: len(re.findall(r'call [^\n]*@air\.(?:fast_)?exp2', t))
            orig, split = ir(SRC, 'orig'), ir(dmsh_rules.hdr_switch_split(SRC), 'split')
            off = ir(SRC.replace('0 != as_type<int>((*spvDescriptorSet0.fp_c13)._m0[51].w)', 'false'), 'off')
            self.assertGreater(calls(orig), 0)
            self.assertEqual(calls(off), 0, 'with the switch constant the curve is dead code')
            self.assertEqual(calls(split), calls(orig), 'the split keeps the curve once, in the switch-on copy')


if __name__ == '__main__':
    unittest.main()
