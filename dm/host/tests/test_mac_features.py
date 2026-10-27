#!/usr/bin/env python3
# 执行打包脚本的覆盖层段；全部输入为合成数据，无游戏内容。
# Execute the packager's overlay section; all inputs are synthetic, with no game content.
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

DM = Path(__file__).resolve().parents[2]


class MacFeatures(unittest.TestCase):
    def test_overlay_packaging(self):
        text = (DM / 'tools/mac_app.sh').read_text()
        section = text[text.index('if [ "${DM_MAC_OVERLAY:-}" != none ]; then'):text.index('[ -s "$RES/paths.txt" ]')]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tools = root / 'tools'
            tools.mkdir()
            python = tools / 'python3'
            python.write_text('#!/bin/bash\nset -eu\nmkdir -p "$3/data" "${3}_variants/item_notes/data"\n'
                              'echo synthetic > "$3/data/.build.config"\n'
                              'echo synthetic > "${3}_variants/item_notes/data/.build.config"\n'
                              'echo synthetic > "${3}_variants/item_notes/dm_textbox_plain.tsv"\n')
            python.chmod(0o755)
            external = root / 'external overlay'
            external.mkdir()
            (external / 'sentinel').write_text('synthetic')
            variants = root / 'external overlay_variants/item_notes'
            variants.mkdir(parents=True)
            (variants / 'sentinel').write_text('synthetic')
            bare = root / 'base only'
            bare.mkdir()
            (bare / 'sentinel').write_text('synthetic')
            for link in ('', '1'):
                for mode in ('generated', 'external', 'bare', 'none'):
                    out = root / f'output {link or "bundled"} {mode}'
                    res = out / 'Resources'
                    res.mkdir(parents=True)
                    paths = res / 'paths.txt'
                    paths.touch()
                    ov = out / 'romfs_overlay.local' if link else res / 'romfs_overlay'
                    # 链接式的覆盖层先写进暂存目录 STG，整包验过签名后才换到 ov（mac_app.sh 末尾）；paths.txt 记的是 ov
                    # The link-style overlay is written into the staging folder STG first and only moved to ov once the whole package verifies (end of mac_app.sh); paths.txt records ov
                    stg = out / '.mac_app.new'
                    wr = stg / 'romfs_overlay.local' if link else ov
                    stale = Path(str(wr) + '_variants') / 'stale'
                    stale.mkdir(parents=True)
                    env = dict(os.environ, OUT=str(out), RES=str(res), ROOT=str(root), DM=str(DM), STG=str(stg),
                               LINK=link, PATH=str(tools) + ':' + os.environ['PATH'])
                    env['DM_MAC_OVERLAY'] = {'generated': '', 'external': str(external) + '/',
                                             'bare': str(bare), 'none': 'none'}[mode]
                    subprocess.run(['/bin/bash', '-eu', '-c', section], env=env, check=True)
                    entries = dict(line.split('=', 1) for line in paths.read_text().splitlines())
                    if mode == 'none':
                        self.assertEqual(entries, {})
                        continue
                    self.assertFalse(stale.exists())
                    self.assertTrue(wr.is_dir())
                    have_variants = mode != 'bare'
                    self.assertEqual(Path(str(wr) + '_variants').is_dir(), have_variants)
                    self.assertEqual(entries.get('romfs_overlay'), str(ov) if link else None)
                    self.assertEqual(entries.get('romfs_overlay_variants'), str(ov) + '_variants' if link and have_variants else None)
                    if mode == 'generated':
                        self.assertTrue((Path(str(wr) + '_variants') / 'item_notes/dm_textbox_plain.tsv').is_file())
                    if mode == 'external':
                        self.assertEqual((Path(str(wr) + '_variants') / 'item_notes/sentinel').read_text(), 'synthetic')
                env['DM_MAC_OVERLAY'] = str(ov)
                ov.mkdir(exist_ok=True)
                (ov / 'keep').write_text('synthetic')
                result = subprocess.run(['/bin/bash', '-eu', '-c', section], env=env, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue((ov / 'keep').is_file())


if __name__ == '__main__':
    unittest.main()
