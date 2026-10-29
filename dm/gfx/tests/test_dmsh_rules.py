#!/usr/bin/env python3
"""Unit test for dmsh_rules.py: the HDR-switch split on synthetic MSL shaped like spirv-cross output (no game shader text). Checks the structure,
that the else copy has the switch replaced by false and nothing else changed, the refusals, and (when the Metal compiler is there) that the result
compiles and that the switch-off copy loses the work the switch guarded."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import dmsh_rules  # noqa: E402

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
        (f, applies), = dmsh_rules.RULES
        self.assertIs(f, dmsh_rules.hdr_switch_split)
        self.assertTrue(applies('any', 'e5fb9d7722af87ad'))
        self.assertFalse(applies('any', '3979464914c7fede'))

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
