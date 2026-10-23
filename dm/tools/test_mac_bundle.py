"""Synthetic Mach-O and app packaging tests; no game files or application launch."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, **kwargs).stdout


@unittest.skipUnless(sys.platform == 'darwin', 'requires native macOS tools')
class MacBundleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mac bundle ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lib = self.root / 'external libraries'
        self.lib.mkdir()
        self.app = self.root / 'Fixture.app'
        self.exe = self.app / 'Contents/MacOS/D2RMobile'
        self.exe.parent.mkdir(parents=True)
        for name, body in [('leaf', 'int leaf(void){return 42;}'),
                           ('parent', 'int leaf(void); int answer(void){return leaf();}'),
                           ('main', '#include <stdio.h>\nint answer(void); int main(void){printf("%d\\n",answer());}')]:
            (self.root / (name + '.c')).write_text(body)
        flags = ['-mmacosx-version-min=26.0', '-Wl,-headerpad_max_install_names']
        run('clang', *flags, '-dynamiclib', self.root/'leaf.c', '-Wl,-install_name,@rpath/libleaf.dylib', '-o', self.lib/'libleaf.dylib')
        run('clang', *flags, '-dynamiclib', self.root/'parent.c', self.lib/'libleaf.dylib', '-Wl,-install_name,@rpath/libparent.dylib', '-o', self.lib/'libparent.dylib')
        run('clang', *flags, self.root/'main.c', self.lib/'libparent.dylib', '-Wl,-rpath,'+str(self.lib), '-o', self.exe)

    def packager_fixture(self):
        """带 mac_app.sh 与假宿主 / 产物 / 着色器库的源码树；返回 (dm, data, env)。 | A source tree with mac_app.sh and a stub host / product / shader library; returns (dm, data, env)."""
        import struct
        import zlib
        dm = self.root/'source/dm'
        (dm/'tools').mkdir(parents=True)
        for name in ('mac_app.sh', 'mac_bundle.py'):
            shutil.copy2(TOOLS/name, dm/'tools'/name)
        (dm/'host').mkdir()
        shutil.copy2(self.exe, dm/'host/dmhost')
        (dm/'host/Makefile').write_text('all:\n\t@true\nprint-macos-min:\n\t@echo 26.0\n')
        (dm/'gfx/nvn').mkdir(parents=True)
        for name in ('shader_map.tsv', 'pso_manifest.tsv'):
            (dm/'gfx/nvn'/name).write_text('fixture\n')
        (dm/'gfx/nvn/dmsh_chain.h').write_text('#define DMSH_CHAIN "fixture"\n')
        data = self.root/'own data'
        product = data/'product'
        product.mkdir(parents=True)
        (product/'main.patched.bin').write_bytes(b'synthetic product')
        (dm/'tools/product.sh').write_text('echo "$DM_ROOT/product"\n')
        rom = data/'romfs'
        (rom/'data').mkdir(parents=True)
        (rom/'data/build_manifest.psv').write_text('synthetic resources\n')
        msl = data/'msl/b-fixture'
        msl.mkdir(parents=True)
        metal = data/'fixture.metal'
        metal.write_text('#include <metal_stdlib>\nkernel void noop() {}\n')
        run('xcrun', 'metal', '-mmacosx-version-min=26.0', '-c', metal, '-o', data/'fixture.air')
        run('xcrun', 'metallib', data/'fixture.air', '-o', msl/'fixture_b.metallib')
        (msl/'fixture.json').write_text('{}')
        (msl/'not-runtime.metal').write_text('must not ship')
        def chunk(kind, payload):
            return struct.pack('>I', len(payload))+kind+payload+struct.pack('>I', zlib.crc32(kind+payload))
        icon = data/'own.png'
        pixels = (b'\0'+b'\0\xff\0\xff'*1024)*1024
        icon.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB',1024,1024,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b''))
        env = dict(os.environ, DM_ROOT=str(data), DM_MAC_LINK='', DM_MAC_OVERLAY='none',
                   DM_ROMFS_SRC=str(rom), DM_MSL_DIR=str(data/'msl'), DM_MAC_ROMFS_COPY='1', DM_MAC_ICON_SOURCE=str(icon))
        return dm, data, env

    def test_packager_copies_private_resources_and_preserves_existing_output(self):
        import plistlib
        dm, data, env = self.packager_fixture()
        product = data/'product'
        output = self.root/'output'
        run('bash', dm/'tools/mac_app.sh', output, env=env)
        app = output/'D2R Mobile.app'
        res = app/'Contents/Resources'
        info = plistlib.loads((app/'Contents/Info.plist').read_bytes())
        self.assertEqual(info['CFBundleIconFile'], 'AppIcon')
        self.assertEqual((res/'romfs/data/build_manifest.psv').read_text(), 'synthetic resources\n')
        self.assertFalse((res/'paths.txt').exists())
        self.assertFalse((res/'msl/b-fixture/not-runtime.metal').exists())
        self.assertTrue((res/'AppIcon.icns').is_file())
        linked_env = dict(env)
        for key in ('DM_MAC_ROMFS_COPY', 'DM_MAC_ICON_SOURCE'):
            linked_env.pop(key)
        linked_env['DM_MAC_LINK'] = '1'
        run('bash', dm/'tools/mac_app.sh', self.root/'linked', env=linked_env)
        linked_res = self.root/'linked/D2R Mobile.app/Contents/Resources'
        self.assertIn('product=', (linked_res/'paths.txt').read_text())
        self.assertFalse((linked_res/'romfs').exists())
        self.assertFalse((linked_res/'product').exists())
        self.assertNotIn('CFBundleIconFile', plistlib.loads((self.root/'linked/D2R Mobile.app/Contents/Info.plist').read_bytes()))
        self.assertFalse((linked_res/'AppIcon.icns').exists())
        missing_icon = subprocess.run(['bash', str(dm/'tools/mac_app.sh'), str(self.root/'no icon')],
                                      env=dict(linked_env, DM_MAC_ICON_SOURCE=str(self.root/'missing.png')), capture_output=True, text=True)
        self.assertNotEqual(missing_icon.returncode, 0)
        self.assertIn('DM_MAC_ICON_SOURCE', missing_icon.stdout)
        (product/'external.link').symlink_to(self.root/'main.c')
        rejected = subprocess.run(['bash', str(dm/'tools/mac_app.sh'), str(self.root/'rejected')],
                                  env=env, capture_output=True, text=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn('不许有符号链接', rejected.stdout)
        self.lib.rename(self.root/'moved libraries')
        self.assertEqual(run(app/'Contents/MacOS/D2RMobile').strip(), '42')
        run('codesign', '--verify', '--deep', '--strict', app)
        sentinel = res/'keep.txt'
        sentinel.write_text('keep')
        (product/'external.link').unlink(); (self.root/'moved libraries').rename(self.lib)
        run('bash', dm/'tools/mac_app.sh', output, env=env)   # 同一输出目录再出一次：整包重建（各条出包流程都靠这一点），旧包里多出的东西不留 | Build again into the same output directory: the whole package is rebuilt (every packaging flow relies on this), and nothing extra from the old package survives
        self.assertFalse(sentinel.exists())
        run('codesign', '--verify', '--deep', '--strict', app)

    def test_failed_rebuild_keeps_previous_package_and_overlay(self):
        # 重出失败（这里让图标图不存在，挂在覆盖层与大半个包都做完之后）：上一次的包与链接式的覆盖层原样留着、能验签，输出目录里不留暂存
        # A failed rebuild (here the icon image is missing, which fails after the overlay and most of the package are done): the previous package and the
        # link-style overlay stay exactly as they were and still verify, and no staging folder is left in the output folder
        dm, data, env = self.packager_fixture()
        for name, variant in (('overlay one', True), ('overlay two', False)):
            (data/name/'data').mkdir(parents=True)
            (data/name/'data/marker.txt').write_text(name)
            if variant:
                (data/(name + '_variants')/'combo/data').mkdir(parents=True)
        linked = dict(env, DM_MAC_LINK='1', DM_MAC_OVERLAY=str(data/'overlay one'))
        for key in ('DM_MAC_ROMFS_COPY', 'DM_MAC_ICON_SOURCE'):
            linked.pop(key)
        output = self.root/'linked output'
        run('bash', dm/'tools/mac_app.sh', output, env=linked)
        app = output/'D2R Mobile.app'
        info = (app/'Contents/Info.plist').read_bytes()
        overlay = output/'romfs_overlay.local'
        self.assertIn(f'romfs_overlay={overlay}', (app/'Contents/Resources/paths.txt').read_text())
        failed = subprocess.run(['bash', str(dm/'tools/mac_app.sh'), str(output)], check=False, capture_output=True, text=True,
                                env=dict(linked, DM_MAC_OVERLAY=str(data/'overlay two'), DM_MAC_ICON_SOURCE=str(self.root/'missing.png')))
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('DM_MAC_ICON_SOURCE', failed.stdout)
        self.assertEqual((app/'Contents/Info.plist').read_bytes(), info)
        run('codesign', '--verify', '--deep', '--strict', app)
        self.assertEqual((overlay/'data/marker.txt').read_text(), 'overlay one')
        self.assertTrue((output/'romfs_overlay.local_variants/combo/data').is_dir())
        self.assertEqual(sorted(p.name for p in output.iterdir() if p.name.startswith('.')), [])
        run('bash', dm/'tools/mac_app.sh', output, env=dict(linked, DM_MAC_OVERLAY=str(data/'overlay two')))
        self.assertEqual((overlay/'data/marker.txt').read_text(), 'overlay two')
        self.assertFalse((output/'romfs_overlay.local_variants').exists())
        self.assertNotIn('romfs_overlay_variants=', (app/'Contents/Resources/paths.txt').read_text())
        run('codesign', '--verify', '--deep', '--strict', app)

    def test_loader_relative_paths_are_resolved_at_original_host(self):
        source = self.root/'build/dmhost'
        source.parent.mkdir()
        run('clang', '-mmacosx-version-min=26.0', '-Wl,-headerpad_max_install_names',
            self.root/'main.c', self.lib/'libparent.dylib', '-Wl,-rpath,@loader_path/../external libraries', '-o', source)
        shutil.copy2(source, self.exe)
        before = source.read_bytes()
        run(sys.executable, TOOLS/'mac_bundle.py', self.exe, source)
        self.lib.rename(self.root/'moved sources')
        self.assertEqual(run(self.exe).strip(), '42')
        self.assertEqual(source.read_bytes(), before)

    def test_executable_relative_dependency_uses_original_host(self):
        source = self.root/'build/dmhost'
        source.parent.mkdir()
        run('install_name_tool', '-id', '@executable_path/../external libraries/libparent.dylib', self.lib/'libparent.dylib')
        run('clang', '-mmacosx-version-min=26.0', '-Wl,-headerpad_max_install_names', self.root/'main.c',
            self.lib/'libparent.dylib', '-Wl,-rpath,'+str(self.lib), '-o', source)
        shutil.copy2(source, self.exe)
        run(sys.executable, TOOLS/'mac_bundle.py', self.exe, source)
        self.lib.rename(self.root/'moved sources')
        self.assertEqual(run(self.exe).strip(), '42')

    def test_recursive_dependencies_run_without_build_tree(self):
        run(sys.executable, TOOLS/'mac_bundle.py', self.exe)
        self.lib.rename(self.root/'moved sources')
        self.assertEqual(run(self.exe).strip(), '42')
        for lib in (self.app/'Contents/Frameworks').iterdir():
            run('codesign', '--verify', '--strict', lib)
            self.assertNotIn(str(self.lib), run('otool', '-L', lib))


if __name__ == '__main__':
    unittest.main()
