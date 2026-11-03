"""One shader compile convention across the three Metal paths: the Mac offline compile (dmsh_build.py MAC_METAL), the iOS offline compile (ios_metal.py METAL) and
the run-time compile of source-form libraries (nvn_mtl.m src_opts, used by the launcher's installs). A flag that changes generated code must mean the same on all three,
or the same shader key would draw differently depending on how its library was built. A source scan: no chain file is edited or run, so nothing re-translates.
Deployment-target flags (-m*-version-min=) are left out: they name the minimum system, not what the code means."""
import ast
import os
import re
import sys
import unittest
HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(DM, 'ios', 'tools'))
import ios_metal   # noqa: E402

# Offline compiler flag → the MTLCompileOptions property and value that mean the same at run time. Add a row (and the src_opts line) when a flag is added
FLAG_MAP = {'-fpreserve-invariance': ('preserveInvariance', 'YES')}
# Properties src_opts states explicitly because they are the offline compiler's defaults (fast math)
DEFAULTS = {'mathMode': 'MTLMathModeFast'}


def conv(cmd):
    """The flags of a 'xcrun ... metal ...' command that change generated code: the arguments after 'metal' starting with '-', minus -c and the deployment target"""
    args = cmd[cmd.index('metal') + 1:]
    return sorted(a for a in args if a.startswith('-') and a != '-c' and not re.fullmatch(r'-m[\w-]*-version-min=.*', a))


def differ(a, b):
    return sorted(set(a) ^ set(b))


def mac_build():
    """MAC_METAL (a literal list) and the runtime branch of MAC_RECORD (the IfExp body string) from dmsh_build.py, read without running it"""
    with open(os.path.join(DM, 'gfx', 'tools', 'dmsh_build.py'), encoding='utf-8') as f: tree = ast.parse(f.read())
    metal = record = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            if node.targets[0].id == 'MAC_METAL': metal = ast.literal_eval(node.value)
            if node.targets[0].id == 'MAC_RECORD' and isinstance(node.value, ast.IfExp): record = ast.literal_eval(node.value.body)
    return metal, record


def src_opts():
    """The 'o.<prop> = <value>;' pairs of nvn_mtl.m src_opts, or None when it no longer has that form"""
    with open(os.path.join(DM, 'gfx', 'nvn', 'nvn_mtl.m'), encoding='utf-8') as f: text = f.read()
    m = re.search(r'static MTLCompileOptions \*src_opts\(void\) \{(.*?)\n\}', text, re.S)
    if not m: return None
    return dict(re.findall(r'\bo\.(\w+)\s*=\s*([^;]+?)\s*;', m.group(1)))


class CompileConvention(unittest.TestCase):
    def test_offline_mac_and_ios_match(self):
        mac, _ = mac_build()
        self.assertIsNotNone(mac, 'MAC_METAL not found in dmsh_build.py')
        self.assertEqual(conv(mac), conv(ios_metal.METAL), 'dmsh_build.MAC_METAL and ios_metal.METAL compile with different flags')

    def test_every_flag_has_a_runtime_equivalent(self):
        mac, _ = mac_build()
        for flag in conv(mac):
            self.assertIn(flag, FLAG_MAP, f'{flag}: add its MTLCompileOptions equivalent to src_opts (nvn_mtl.m) and to FLAG_MAP')

    def test_src_opts_equal_the_offline_flags(self):
        opts = src_opts()
        self.assertIsNotNone(opts, 'nvn_mtl.m src_opts no longer parses in the "o.<prop> = <value>;" form: keep that form or update test_compile_convention.py')
        mac, _ = mac_build()
        want = dict(FLAG_MAP[f] for f in conv(mac) if f in FLAG_MAP); want.update(DEFAULTS)
        self.assertEqual(opts, want, 'nvn_mtl.m src_opts differs from the offline compile flags (FLAG_MAP / DEFAULTS in test_compile_convention.py)')

    def test_runtime_record_names_every_property(self):
        _, record = mac_build()
        self.assertIsNotNone(record, 'the runtime branch of MAC_RECORD not found in dmsh_build.py')
        for prop, _ in FLAG_MAP.values(): self.assertIn(prop, record)

    def test_negative_control(self):
        mac, _ = mac_build()
        self.assertEqual(differ(conv(mac + ['-fno-fast-math']), conv(ios_metal.METAL)), ['-fno-fast-math'])
        self.assertEqual(conv(['xcrun', '-sdk', 'iphoneos', 'metal', '-c', '-mios-version-min=27.0', '-fx']), ['-fx'])


if __name__ == '__main__':
    unittest.main()
