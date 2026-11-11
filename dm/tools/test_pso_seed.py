"""Unit tests for pso_seed.py: the byte-level key normaliser against hand-made rows and against the C one (one hash over t_pso_skey's random keys),
the row check (pso_skey_check's port), normalise, diff, check with the allow-list, merge, the route matrix and its generated scenarios, the cold-run
summary and the cleanup planners, the harvest's PSO-7 evidence on ios-mac, on synthetic files; no game files, nothing is run."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pso_seed  # noqa: E402

PSO_NORM_FNV = 0xe615b40ab55121b6   # the same constant as dm/gfx/tests/t_pso_skey.m
M64 = (1 << 64) - 1


def blend(target, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a):
    """blend_st as its 8 bytes (bit fields 3, 7, 7, 7, 7, 3, 3 from the low bit)."""
    v = target | src_rgb << 3 | dst_rgb << 10 | src_a << 17 | dst_a << 24 | eq_rgb << 31 | eq_a << 34
    return struct.pack('<Q', v)


def pat(i):
    return blend(i, 5, 6, 2, 1, 1, 1)


def row(cfmt, blend_en, chmask, blends, lmap, merged=0, vs=b'aaaaaaaaaaaaaaaa_b', fs=b''):
    b = bytearray(pso_seed.SKEY_LEN)
    b[0:len(vs)] = vs; b[24:24 + len(fs)] = fs; b[50] = merged
    struct.pack_into('<8I', b, 104, *cfmt); struct.pack_into('<II', b, 148, blend_en, chmask)
    b[160:224] = b''.join(blends); b[416:424] = bytes(lmap)
    return bytes(b)


def n1():
    return row([70, 70, 0, 0, 0, 0, 0, 0], 0x5, 0xffffffff, [pat(i) for i in range(8)], [3] * 8)


def n2():
    return row([70, 70, 70, 0, 0, 0, 0, 0], 0x7, 0x87654321, [pat(i) for i in range(8)], [0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff], merged=1)


class Xorshift:
    def __init__(self, x=0x9e3779b97f4a7c15): self.x = x

    def next(self):
        x = self.x; x ^= (x << 13) & M64; x ^= x >> 7; x ^= (x << 17) & M64; self.x = x
        return x


def gen(g):
    """One random row exactly as t_pso_skey.m gen() builds it."""
    b = bytearray(pso_seed.SKEY_LEN); b[0:18] = b'aaaaaaaaaaaaaaaa_b'
    r = g.next(); merged = r & 1; b[50] = merged
    for i in range(8): struct.pack_into('<I', b, 104 + 4 * i, 70 + i if (r >> (8 + i)) & 1 else 0)
    struct.pack_into('<I', b, 148, g.next() & 0xffffffff); struct.pack_into('<I', b, 152, g.next() & 0xffffffff)
    for i in range(8): struct.pack_into('<Q', b, 160 + 8 * i, g.next())
    r = g.next()
    for i in range(8):
        v = (r >> (8 * i)) & 0xff
        b[416 + i] = (0xff if v % 9 == 8 else v % 9) if merged else v
    return bytes(b)


class NormTest(unittest.TestCase):
    def test_n1_normal_pass(self):
        out = pso_seed.norm_row(n1())
        want = row([70, 70, 0, 0, 0, 0, 0, 0], 0x1, 0xff, [pat(0)] + [bytes(8)] * 7, [0] * 8)
        self.assertEqual(out, want)
        self.assertEqual(pso_seed.norm_row(out), out)

    def test_n2_merged_pass(self):
        out = pso_seed.norm_row(n2())
        want = row([70, 70, 70, 0, 0, 0, 0, 0], 0x3, 0x21, [pat(0), pat(1)] + [bytes(8)] * 6, [0, 0xff, 1, 0xff, 0xff, 0xff, 0xff, 0xff], merged=1)
        self.assertEqual(out, want)
        self.assertEqual(pso_seed.norm_row(out), out)

    def test_hash_shared_with_c(self):
        g, h = Xorshift(), 0xcbf29ce484222325
        for _ in range(5000):
            n = pso_seed.norm_row(gen(g))
            self.assertEqual(pso_seed.norm_row(n), n)
            for c in n: h = ((h ^ c) * 0x100000001b3) & M64
        self.assertEqual(h, PSO_NORM_FNV)


class CheckTest(unittest.TestCase):
    def test_check_row(self):
        for raw, merged in ((n1(), 0), (n2(), 1)):
            why, h = pso_seed.check_row(raw.hex())
            self.assertIsNone(why); self.assertEqual(h, pso_seed.norm_row(raw).hex())
        b = bytearray(n1()); b[86] = b[87] = 0xff
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'pad')
        b = bytearray(n2()); b[51] = 1; b[52] = 2
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'kind')
        b = bytearray(n2()); b[416] = 9
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'lmap')
        b = bytearray(n1()); b[416] = 9   # an unmerged row's lmap is cleared, not refused
        self.assertIsNone(pso_seed.check_row(b.hex())[0])
        b = bytearray(n1()); b[0] = 0
        self.assertEqual(pso_seed.check_row(b.hex())[0], 'key')
        self.assertEqual(pso_seed.check_row('00')[0], 'parse')

    def test_normalise(self):
        bad = bytearray(n1()); bad[86] = 0xff
        lines = ['# old comment', f'R\t{n1().hex()}', 'S\tssss_b\t0', f'R\t{bad.hex()}', 'C\t', pso_seed.BG_MARK, f'R\t{pso_seed.norm_row(n1()).hex()}', 'C\tcccc_b']
        out, dropped, kept = pso_seed.normalise(lines)
        self.assertEqual(out, [f'R\t{pso_seed.norm_row(n1()).hex()}', 'S\tssss_b\t0', pso_seed.BG_MARK, 'C\tcccc_b'])
        self.assertEqual([(t, w) for t, w, _ in dropped], [('R', 'pad'), ('C', 'empty'), ('R', 'duplicate')])
        self.assertEqual(kept, {'S': 1, 'R': 1, 'C': 1})


def parse_scn(path):
    """scenario.parse, without the ResourceWarning of its unclosed file (not this tool's)."""
    import scenario   # noqa: PLC0415
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', ResourceWarning)
        return scenario.parse(path)


class RunToolsTest(unittest.TestCase):
    def test_check_allow_and_run_dirs(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); other = row([80, 0, 0, 0, 0, 0, 0, 0], 0, 0xf, [bytes(8)] * 8, [0] * 8)
            seed = d / 'seed.tsv'; seed.write_text(pso_seed.HEAD + '\n', encoding='utf-8')
            man = d / 'm.tsv'; man.write_text(f'{pso_seed.HEAD}\nR\t{n1().hex()}\nR\t{other.hex()}\n', encoding='utf-8')
            allow = d / 'allow.tsv'; allow.write_text(f'# why: test\nR\t{n1().hex()}\n', encoding='utf-8')   # the raw, non-normalised form
            rows, bad, errors = pso_seed.check([str(man)], str(seed), str(allow))
            self.assertEqual(([t for t, _ in rows], bad, errors), (['R'], [], []))
            self.assertEqual(rows[0][1], pso_seed.norm_row(other).hex())
            allow.write_text(f'R\t{n1().hex()}\nR\t{other.hex()}\n', encoding='utf-8')
            self.assertEqual(pso_seed.check([str(man)], str(seed), str(allow)), ([], [], []))
            run = d / 'run'; (run / 'support').mkdir(parents=True); (run / 'support' / 'pso_user.tsv').write_text(f'S\tssss_b\t0\n', encoding='utf-8')
            self.assertEqual(pso_seed.check([str(run)], str(seed), None)[0], [('S', 'ssss_b')])
            empty = d / 'empty'; empty.mkdir()
            self.assertEqual(pso_seed.check([str(empty)], str(seed), None), ([], [], []))   # a run that built nothing wrote no manifest
            self.assertEqual(len(pso_seed.check([str(d / 'nope')], str(seed), None)[2]), 1)

    def test_harvest_needs_pso7_on_ios_mac(self):
        from unittest import mock   # noqa: PLC0415
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); run = d / 'run'; run.mkdir(); man = d / 'container' / 'settings.test' / 'pso_user.tsv'; dest = d / 'h.tsv'
            with mock.patch.object(pso_seed, 'run_manifest', return_value=str(man)):
                (run / 'port.log').write_text('[dm] boot\n', encoding='utf-8')   # a package from before PSO-7: no manifest, no boot line
                self.assertIsNone(pso_seed.harvest('ios-mac', str(run), str(dest)))
                self.assertFalse(dest.exists())
                (run / 'port.log').write_text(f'[dm] {pso_seed.PSO7_LINE} (the player\'s is untouched)\n', encoding='utf-8')
                self.assertEqual(pso_seed.harvest('ios-mac', str(run), str(dest)), str(dest))   # PSO-7 and no manifest: it built nothing
                self.assertEqual(dest.read_text(encoding='utf-8'), pso_seed.HEAD + '\n')
                man.parent.mkdir(parents=True); man.write_text(f'{pso_seed.HEAD}\nS\tssss_b\t0\n', encoding='utf-8'); (run / 'port.log').unlink()
                self.assertEqual(pso_seed.harvest('ios-mac', str(run), str(dest)), str(dest))   # a manifest there is evidence enough
                self.assertEqual(dest.read_text(encoding='utf-8'), man.read_text(encoding='utf-8'))
            dest.unlink()
            self.assertEqual(pso_seed.harvest('cli', str(run), str(dest)), str(dest))   # the CLI host: no manifest is an empty one
            self.assertEqual(dest.read_text(encoding='utf-8'), pso_seed.HEAD + '\n')

    def test_merge_order(self):
        s1, s2 = 'S\tseed1_b\t0', 'S\tseed2_b\t1'
        f1, a1, t1 = 'S\tfront1_b\t0', f'R\t{pso_seed.norm_row(n1()).hex()}', 'C\ttour1_b'
        lines, counts = pso_seed.merge([pso_seed.HEAD + '\n', '# old note\n', s1 + '\n', s2 + '\n'],
                                       {'front': [f1, s1], 'act1': [a1, f1], 'tour': [t1]}, ['note'])
        body = [x for x in lines if not x.startswith('#') or x == pso_seed.BG_MARK]
        self.assertEqual(lines[0], pso_seed.HEAD)
        self.assertEqual(body, [s1, s2, f1, a1, pso_seed.BG_MARK, t1])   # the seed's own rows keep their place
        self.assertEqual(counts, dict(front=1, seed=2, act1=1, seed_background=0, tour=1))
        again, _ = pso_seed.merge(lines, {'front': [], 'act1': [], 'tour': ['C\ttour2_b']}, [])
        self.assertEqual([x for x in again if not x.startswith('#') or x == pso_seed.BG_MARK], [s1, s2, f1, a1, pso_seed.BG_MARK, t1, 'C\ttour2_b'])
        with self.assertRaises(ValueError): pso_seed.merge([f'R\t{n1().hex()}\n'], {}, [])   # a seed row that is not normalised stops the merge

    def test_matrix_and_routes(self):
        m = pso_seed.matrix()
        self.assertEqual(len(m), 77); self.assertEqual(len({r['nn'] for r in m}), 77)
        self.assertEqual([r['nn'] for r in m], [f'{i:02d}' for i in range(1, 78)])
        self.assertFalse([r for r in m if r['env'].get('DM_AA_MODE') == '3' or (r['env'].get('DM_AA_MODE') == '0' and r['env'].get('DM_MFXT') != '1')])   # AA off only with the temporal scaler
        self.assertTrue(all(r['env'].get('DM_TEST_VIDEO_FIRSTBOOT') == '1' for r in m if r['backend'] == 'mac-app'))
        self.assertEqual(sum(r['set'] == 'tour' for r in m), 12); self.assertEqual(sum(r['set'] == 'warps' for r in m), 43)
        self.assertEqual(len({r['stop'][1] for r in m if r['set'] == 'warps'}), 43)   # no area twice
        self.assertEqual([(r['backend'], r['route']) for r in m if r['set'] == 'skills'], [('mac-app', pso_seed.SKILLS)] + [('cli', pso_seed.SKILLS)] * len(pso_seed.FORCES))   # the skills route: the Mac App, then the CLI host under each force
        with tempfile.TemporaryDirectory() as d:
            texts = [pso_seed.front_text(v, b, pso_seed.REC_ENV if b == 'mac-app' else {}) for v in ('create', 'menu') for b in ('mac-app', 'ios-mac')]
            texts += [pso_seed.row_text(r, 'create') for r in m]
            for i, t in enumerate(texts):
                p = Path(d) / f'{i}.scn'; p.write_text(t, encoding='utf-8')
                meta, steps, checks = parse_scn(str(p))
                self.assertTrue(steps and checks, t)
            tour = pso_seed.row_text(next(r for r in m if r['name'] == 'tour_a3_flayer'), 'create')
            self.assertEqual(tour.count('press keys=ZR'), 2); self.assertIn('tap 0.5 0.446', tour); self.assertIn('save tour', tour)
            warp = pso_seed.row_text(next(r for r in m if r['name'] == 'warp_a1_den'), 'create')
            self.assertIn('DM_TEST_GOTO_LEVEL=150:8', warp); self.assertIn('expect_state area=8', warp); self.assertIn('save warlock', warp); self.assertIn('DM_TEST_VIDEO_FIRSTBOOT=1', warp)
            cli = next(r for r in m if r['name'] == 'cli_town_rec_mfx3d')
            p = Path(d) / 'cli.scn'; p.write_text(pso_seed.row_text(cli, 'create'), encoding='utf-8')
            self.assertEqual(parse_scn(str(p))[0]['env'], {'DM_GAME_SEED': '305419896', **pso_seed.CLI_REC})
            self.assertNotIn('requires', pso_seed.front_text('create', 'ios-mac', {}))
            tour_all = pso_seed.warp_tour_text()   # every warp stop in one session: the committed route is the generator's output
            self.assertEqual(tour_all.count('\ngoto '), len(pso_seed.WARPS)); self.assertIn('DM_TEST_KEEP_LIFE=1', tour_all)
            self.assertEqual(tour_all, (Path(pso_seed.__file__).resolve().parents[1] / 'verify' / 'bench' / 'warp_tour.scn').read_text(encoding='utf-8'))

    def test_cold_summary(self):
        log = '\n'.join([
            '[dm] [nvn-mtl] pipeline warm-up: foreground rows done in 412.5 ms (377 rows, 2 workers)',
            '[dm] [nvn-mtl] [bootprof] t=1.000 s 出图第 1 次：metallib 载入 1 个 / 2.0 ms；管线创建 2 个 / 9.0 ms（均在提交线程同步）；并行预编 丢出 30 / 领用 1 / 过期回收 0 / 表满没丢 0 / 在表 3；host builds lib 1 / pso 2 / 7.5 ms',
            '[dm] [nvn-mtl] host mfx scaler pipeline built in 18.2 ms (submit thread)',
            '[dm] EV {"seq":9,"t":3.0,"ev":"game_enter"}',
            '[dm] [nvn] 进局读取画面等管线预热 812 ms',
            '[dm] [nvn-mtl] pipeline warm-up done: shaders 181, render pipelines 184, compute pipelines 12; skipped: bad rows 0, duplicates 0, specialised 0, missing shaders 0, files 0; 1803.0 ms; workers 2, foreground 412.5 ms',
            '[dm] [nvn-mtl] [bootprof] t=9.000 s 出图第 120 次：metallib 载入 2 个 / 3.0 ms；管线创建 5 个 / 19.0 ms（均在提交线程同步）；并行预编 丢出 44 / 领用 2 / 过期回收 0 / 表满没丢 0 / 在表 3；host builds lib 1 / pso 3 / 9.5 ms'])
        nvn = {1: dict(frame_ms=16.7, pso_new=2, lib_new=1), 2: dict(frame_ms=41.2, pso_new=1, lib_new=0), 3: dict(frame_ms=16.6, pso_new=0, lib_new=0),
               4: dict(frame_ms=16.7, pso_new=0), 5: dict(frame_ms=35.0, pso_new=1), 6: dict(frame_ms=16.6, pso_new=0)}
        marks = [('town', 4), ('wild', 5), ('end', 7)]
        r = pso_seed.cold_summary(log, nvn, marks)
        self.assertEqual((r['warm_fg_ms'], r['warm_all_ms'], r['load_wait_ms']), (412.5, 1803.0, 812))
        self.assertEqual((r['sync_pso_front'], r['sync_lib_front'], r['sync_pso_game']), (3, 1, 1))
        self.assertEqual(r['game_p99_ms'], 35.0)
        self.assertEqual(r['slow_with_creations'], [41.2, 35.0])
        self.assertEqual((r['kicked'], r['taken'], r['host_lib'], r['host_pso'], r['host_ms']), (44, 2, 1, 3, 9.5))
        self.assertEqual(r['host_built'], [dict(what='mfx scaler', kind='pipeline', ms=18.2)])
        eng = pso_seed.cold_summary(log.replace('进局读取画面等管线预热 812 ms', 'loading screen waited for the pipeline warm-up 812 ms'), nvn, marks)
        self.assertEqual(eng['load_wait_ms'], 812)
        ios = pso_seed.cold_summary(log)   # the iOS package on the Mac: no nvn.tsv, the [bootprof] line before game_enter is the front end
        self.assertEqual((ios['sync_pso_front'], ios['sync_lib_front'], ios['sync_pso_game']), (2, 1, 3))

    def test_cleanup_planners(self):
        bid = 'org.d2rmobile.mac.cold20261103120000'
        root = '/w/dm-work'; copy = f'{root}/out/macapp-cold'; caches = '/var/folders/x/C'; home = '/Users/u'
        plan = pso_seed.mac_cleanup_plan(bid, copy, [f'{root}/run/pso/c/run/support', f'{root}/run/pso/c/warm_sup'], caches, home)
        self.assertEqual(plan['moves'], [copy, f'{caches}/{bid}', f'{root}/run/pso/c/run/support', f'{root}/run/pso/c/warm_sup'])
        self.assertEqual(plan['optional'], [f'{home}/Library/Saved Application State/{bid}.savedState', f'{home}/Library/Caches/{bid}', f'{home}/Library/HTTPStorages/{bid}'])
        self.assertEqual(plan['cmds'], [[pso_seed.LSREGISTER, '-u', f'{copy}/D2R Metal.app'], ['defaults', 'delete', bid]])
        everything = plan['moves'] + plan['optional'] + [x for c in plan['cmds'] for x in c]
        self.assertFalse([x for x in everything if '/out/macapp/' in x + '/' or '/out/macapp_test' in x or x.endswith('org.d2rmobile.mac') or 'org.d2rmobile.mac.test' in x])
        for wrong in ('org.d2rmobile.mac', 'org.d2rmobile.mac.test', 'com.patricedery.d2rmobile', pso_seed.COLD_ID):
            with self.assertRaises(ValueError): pso_seed.mac_cleanup_plan(wrong, copy, [], caches, home)
        b = 'com.patricedery.d2rmobile'
        self.assertEqual(pso_seed.ios_cache_plan(b, caches), [f'{caches}/{b}/{b}/com.apple.metal', f'{caches}/{b}/{b}/com.apple.metalfe', f'{caches}/{b}/{b}/com.apple.gpuarchiver'])


class DiffTest(unittest.TestCase):
    def test_diff(self):
        with tempfile.TemporaryDirectory() as d:
            raw = n1(); other = row([80, 0, 0, 0, 0, 0, 0, 0], 0, 0xf, [bytes(8)] * 8, [0] * 8)
            seed = Path(d) / 'seed.tsv'; seed.write_text(f'# c\nS\tssss_b\t0\nR\t{pso_seed.norm_row(raw).hex()}\n', encoding='utf-8')
            m = Path(d) / 'm.tsv'
            m.write_text(f'S\tssss_b\t0\nS\ttttt_b\t1\nR\t{raw.hex()}\nR\t{other.hex()}\nR\t{other.hex()}\nC\tcccc_b\n', encoding='utf-8')
            p = subprocess.run([sys.executable, str(HERE / 'pso_seed.py'), 'diff', str(m), '--seed', str(seed)], capture_output=True, text=True, check=False)
            self.assertEqual(p.returncode, 0, p.stderr)
            lines = p.stdout.splitlines()
            self.assertIn('DIFF S 1 R 1 C 1', lines)   # the raw n1 row equals the seed's normalised one; the repeated row counts once
            res = json.loads(lines[-1][len('RESULT '):])
            self.assertEqual([(r['tag'], r['vs']) for r in res['rows']], [('S', 'tttt_b'), ('R', 'aaaaaaaaaaaaaaaa_b'), ('C', 'cccc_b')])


if __name__ == '__main__':
    os.environ.setdefault('PYTHONDONTWRITEBYTECODE', '1')
    unittest.main()
