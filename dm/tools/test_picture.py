"""Unit tests for picture.py: every set pins every picture key with a value the host accepts, and the sets match the recommendation sources
(dm/host/tests/t_picrec_cases.h and front_video_rec in front_mac.m / front_ios.m), so a change to Recommended cannot leave the test picture behind."""
from pathlib import Path
import re
import sys
import unittest

TOOLS = Path(__file__).resolve().parent
DM = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import picture  # noqa: E402

S3D = {0: '1.0', 1: '0.85', 2: '0.75', 3: '0.67', 4: '0.5'}   # dm_video.c dm_video_scale3d (Mac s3d)
SHQ = {0: '2100', 1: '3072', 2: '4096'}                       # dm_video.c k_shq_px


def picrec_rows():
    """t_picrec_cases.h rows by name: GPU cores, Native width x height, MetalFX | native, fixed width tier, 3D, environment shadows, shadow quality, Frame Drops."""
    rows = {}
    for m in re.finditer(r'\{\s*"([^"]+)",\s*"[^"]*",\s*([-\d,\s]+)\}', (DM / 'host' / 'tests' / 't_picrec_cases.h').read_text(encoding='utf-8')):
        v = [int(x) for x in m.group(2).replace(' ', '').split(',') if x]
        rows[m.group(1)] = dict(zip(('cores', 'w', 'h', 'mfx', 'native', 'fixed', 's3d', 'envsh', 'shq', 'q3d'), v, strict=False))
    return rows


def front_rec(path):
    """The key -> value returns of front_video_rec in a front end."""
    text = path.read_text(encoding='utf-8')
    i = text.index('int front_video_rec(const char *key) {'); body = text[i:text.index('\n}', i)]
    return {k: v for k, v in re.findall(r'"(\w+)"\)\) return (\w[\w().]*);', body)}


class PictureTest(unittest.TestCase):
    def test_every_set_pins_every_key_with_a_value_the_host_takes(self):
        for name, s in picture.SETS.items():
            self.assertEqual(sorted(s), sorted(picture.KEYS), name)
            self.assertRegex(s['DM_GAME_RES'], r'^\d+x\d+$', name)
            self.assertTrue(0.5 <= float(s['DM_3D_SCALE']) <= 1.0, name)   # hle_oe.c s3_cfg dies outside 0.5-1.0
            self.assertIn(s['DM_AA_MODE'], ('1', '2'), name)                # T2x (3) fails inside the game
            self.assertIn(s['DM_NVN_ANISO'], ('0', '4', '8', '16'), name)
            self.assertIn(s['DM_SHADOW_SIZE'], SHQ.values(), name)
            self.assertIn(s['DM_COLORSPACE'], ('srgb', 'none'), name)
            self.assertEqual((s['DM_MFX'], s['DM_MFX3D'], s['DM_3D_SCALE_DYN']), ('1', '1', '0'), name)
        reads = {ln.split('\t')[0] for ln in (DM / 'switches.tsv').read_text(encoding='utf-8').splitlines()[1:]}
        for k in list(picture.KEYS) + list(picture.MAC_APP_REC) + list(picture.FULLSCREEN): self.assertIn(k, reads, f'{k} is read by nothing (dm/switches.tsv)')

    def test_rec_is_this_macs_recommended(self):
        r = picrec_rows()['Mac mini M6, 2560 display']
        self.assertEqual((r['native'], picture.REC['DM_GAME_RES']), (1, f"{r['w']}x{r['h']}"))
        self.assertEqual(picture.REC['DM_3D_SCALE'], S3D[r['s3d']])
        self.assertEqual(picture.REC['DM_ENV_SHADOWS'], str(r['envsh']))
        self.assertEqual(picture.REC['DM_SHADOW_SIZE'], SHQ[r['shq']])
        self.assertEqual(r['q3d'], 1, 'Frame Drops keep on this Mac: REC pins DM_3D_SCALE_DYN=0')
        self.assertEqual(r['mfx'], 1)

    def test_rec75_is_the_base_chips_recommended(self):
        rows = picrec_rows()
        for n in ('MacBook Air M1', 'MacBook Air 13 M2 (8-core GPU)', 'MacBook Air 13 M3 (8-core GPU)'):
            r = rows[n]
            self.assertEqual((r['native'], r['mfx']), (1, 1), n)
            self.assertEqual(picture.REC75['DM_3D_SCALE'], S3D[r['s3d']], n)
            self.assertEqual(picture.REC75['DM_ENV_SHADOWS'], str(r['envsh']), n)
            self.assertEqual(picture.REC75['DM_SHADOW_SIZE'], SHQ[r['shq']], n)
            # q3d is exempt on purpose: these rows lower the 3D on frame drops, REC75 keeps it fixed so a picture baseline can exist

    def test_front_end_recommendations(self):
        mac, ios = front_rec(DM / 'host' / 'front_mac.m'), front_rec(DM / 'ios' / 'Sources' / 'front_ios.m')
        for name, fr, s in (('mac', mac, picture.REC), ('ios', ios, picture.PHONE)):
            self.assertEqual((fr.get('aa'), s['DM_AA_MODE']), ('1', '2'), name)          # aa 1 = SMAA = mode byte 2 (dm_video_aa_mode)
            self.assertEqual((fr.get('aniso'), s['DM_NVN_ANISO']), ('3', '16'), name)    # aniso 3 = 16x (dm_video_aniso)
            self.assertEqual((fr.get('color'), s['DM_COLORSPACE']), ('1', 'srgb'), name)  # color 1 = Accurate
        self.assertNotIn('envsh', ios); self.assertNotIn('shq', ios)   # the phone recommends neither: environment shadows off, the Switch's 2100 map
        self.assertEqual((picture.PHONE['DM_ENV_SHADOWS'], picture.PHONE['DM_SHADOW_SIZE']), ('0', '2100'))
        self.assertEqual(picture.PHONE['DM_3D_SCALE'], '0.75')   # front_ios.m front_video_firstboot: 75% + MetalFX

    def test_env(self):
        self.assertEqual(picture.env('rec', 'cli', {}), {**picture.REC, 'DM_TEST_FULLSCREEN_AT': '1'})
        self.assertEqual(picture.env('rec', 'mac-app', {}), {'DM_TEST_VIDEO_FIRSTBOOT': '1'})
        self.assertEqual(picture.env('orig', 'cli', {}), {})
        got = picture.env('rec', 'cli', {'DM_TEST_WINSIZE': '1280x720', 'DM_GAME_RES': '1600x900'})
        self.assertEqual((got['DM_TEST_WINSIZE'], got['DM_GAME_RES']), ('1280x720', '1600x900')); self.assertNotIn('DM_TEST_FULLSCREEN_AT', got)
        self.assertEqual(picture.env('rec75', 'mac-app', {})['DM_3D_SCALE'], '0.75')
        with self.assertRaises(ValueError): picture.env('nope')


if __name__ == '__main__':
    unittest.main()
