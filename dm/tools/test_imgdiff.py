"""imgdiff.py 的单测：合成图（不用游戏截图）——相同为 0、差得越多分越高、尺寸不同报红、命令行阈值。 | Unit tests for imgdiff.py: synthetic images (no game screenshots) -- identical scores 0, more difference scores higher, different sizes fail, the command-line threshold."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import imgdiff  # noqa: E402


def scene(h=120, w=160, seed=1):   # 渐变底 + 几个色块 + 细线：颜色与边缘都有 | gradient background + a few colored blocks + thin lines: both color and edges
    rng = np.random.default_rng(seed)
    img = np.zeros((h, w, 3)); img[..., 0] = np.linspace(0, 1, w)[None, :]; img[..., 1] = np.linspace(0, 1, h)[:, None]; img[..., 2] = 0.3
    for _ in range(6):
        y, x = rng.integers(0, h - 20), rng.integers(0, w - 20); img[y:y + 18, x:x + 18] = rng.random(3)
    img[::15, :, :] = 1.0
    return img


class ImgdiffTest(unittest.TestCase):
    def test_identical_is_zero(self):
        a = scene()
        self.assertEqual(float(imgdiff.flip(a, a).max()), 0.0)

    def test_monotonic_in_noise(self):
        a = scene(); rng = np.random.default_rng(7); n = rng.normal(0, 1, a.shape)
        means = [float(imgdiff.flip(a, np.clip(a + s * n, 0, 1)).mean()) for s in (0.005, 0.03, 0.15)]
        self.assertLess(means[0], 0.05)
        self.assertLess(means[0], means[1]); self.assertLess(means[1], means[2])

    def test_large_change_scores_high_and_is_bounded(self):
        a = scene(); m = imgdiff.flip(a, 1 - a)
        self.assertGreater(float(m.mean()), 0.4)
        self.assertLessEqual(float(m.max()), 1.0); self.assertGreaterEqual(float(m.min()), 0.0)

    def test_local_change_is_local(self):   # 只改一小块：误差集中在那一块附近 | change only a small block: the error stays around that block
        a = scene(); b = a.copy(); b[50:60, 70:80] = [1, 0, 0]
        m = imgdiff.flip(a, b)
        self.assertGreater(float(m[50:60, 70:80].mean()), 0.3)
        self.assertLess(float(m[:30, :30].mean()), 0.01)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            pa, pb, pc, pm = (str(Path(d) / n) for n in ('a.png', 'b.png', 'c.png', 'm.png'))
            a = scene(); Image.fromarray((a * 255).astype(np.uint8)).save(pa); Image.fromarray(((1 - a) * 255).astype(np.uint8)).save(pb)
            Image.fromarray((scene(60, 80) * 255).astype(np.uint8)).save(pc)
            run = lambda *x: subprocess.run([sys.executable, str(TOOLS / 'imgdiff.py'), *x], capture_output=True, text=True, check=False)  # noqa: E731
            self.assertEqual(run(pa, pa, '--max-mean', '0.01').returncode, 0)
            r = run(pa, pb, '--max-mean', '0.05', '--map', pm)
            self.assertEqual(r.returncode, 1); self.assertIn('IMGDIFF RED', r.stdout); self.assertTrue(Path(pm).exists())
            self.assertIn('sizes differ', run(pa, pc).stdout)


if __name__ == '__main__':
    unittest.main()
