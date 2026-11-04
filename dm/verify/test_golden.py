#!/usr/bin/env python3
# golden.py 的单测（合成数据，不跑游戏）：负对照从中间那条客户端包起往后全删、系统包留着；存档指纹与 savesha.sh 一致
# Unit test for golden.py (synthetic data, no game run): the negative control drops the client packets from the middle one onward and keeps system packets; the save fingerprint matches savesha.sh
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import golden  # noqa: E402

bad = 0
def expect(cond, what):
    global bad
    print(f"{'✓' if cond else '✗'} {what}"); bad += not cond


with tempfile.TemporaryDirectory() as t:
    rec = os.path.join(t, 'rec'); os.makedirs(os.path.join(rec, 'run')); os.makedirs(os.path.join(rec, 'replay_dmhost'))
    rows = [dict(tick=10, q='sys', unit=None, len=5, pkt='6f00000000')] + [dict(tick=20 + i, unit=1, len=3, pkt=f'03{i:04x}') for i in range(6)] + [dict(tick=40, q='sys', unit=None, len=5, pkt='6f01000000')]
    with open(os.path.join(rec, 'c2s.jsonl'), 'w') as f: f.writelines(json.dumps(r) + '\n' for r in rows)
    open(os.path.join(rec, 'meta.json'), 'w').write('{}')
    tick = golden.drop_second_half(rec, os.path.join(t, 'neg'))
    kept = [json.loads(ln) for ln in open(os.path.join(t, 'neg', 'c2s.jsonl'))]
    expect(tick == 23, f'the cut starts at the middle client packet (tick {tick})')
    expect([r['tick'] for r in kept] == [10, 20, 21, 22, 40], 'client packets from the middle on are dropped, system packets kept')
    expect(not os.path.exists(os.path.join(t, 'neg', 'run')) and not os.path.exists(os.path.join(t, 'neg', 'replay_dmhost')), 'the copy leaves out the run and earlier replays')
    os.makedirs(os.path.join(t, 'empty')); open(os.path.join(t, 'empty', 'c2s.jsonl'), 'w').write(json.dumps(rows[0]) + '\n')
    expect(golden.drop_second_half(os.path.join(t, 'empty'), os.path.join(t, 'neg2')) is None, 'no client packets: nothing to drop (None)')

    root = os.path.join(t, 'dmroot'); user = os.path.join(root, 'data', 'switch', 'save_ref_x', '0000000000000001')
    for rel, data in (('0/B/B.d2s', b'\x55\xaa\x55\xaa'), ('0/a/a.txt', b'x'), ('0/Settings/S - 1.json', b'{}'), ('0/.lock', b'')):
        os.makedirs(os.path.dirname(os.path.join(user, rel)), exist_ok=True); open(os.path.join(user, rel), 'wb').write(data)
    old = golden.scenario.DM_ROOT; golden.scenario.DM_ROOT = root
    try: sha = golden.save_manifest('x')
    finally: golden.scenario.DM_ROOT = old
    ref = subprocess.run(['bash', os.path.join(HERE, 'savesha.sh'), user], capture_output=True, text=True, check=False).stdout.strip()
    expect(sha == ref, 'the save fingerprint equals savesha.sh')
    expect('.lock' not in open(os.path.join(root, 'data', 'switch', 'save_ref_x', 'MANIFEST.sha256')).read(), '.lock is left out of the manifest')

# ---- shot environment: brightening fixed at k=1 < the page spec < the caller or set (INH-1) ----
old_k = os.environ.get('DM_BRIGHT_K'); os.environ['DM_BRIGHT_K'] = '3'
try:
    expect(golden.shot_env({}, {})['DM_BRIGHT_K'] == '1', 'a shell DM_BRIGHT_K never reaches a shot (k=1)')
    expect(golden.shot_env({'DM_BRIGHT_K': '4'}, {})['DM_BRIGHT_K'] == '4', "the page spec's DM_BRIGHT_K wins over k=1")
    expect(golden.shot_env({'DM_BRIGHT_K': '4'}, {'DM_BRIGHT_K': '2'})['DM_BRIGHT_K'] == '2', "the caller's DM_BRIGHT_K wins over the page's")
finally:
    if old_k is None: os.environ.pop('DM_BRIGHT_K', None)
    else: os.environ['DM_BRIGHT_K'] = old_k
old_clock = os.environ.pop('DM_DETCLOCK', None)
try:
    expect(golden.shot_env({}, {})['DM_DETCLOCK'] == '16666667' and golden.shot_env({'DM_DETCLOCK': '1'}, {})['DM_DETCLOCK'] == '1', 'the deterministic clock is the default, a page value wins')
finally:
    if old_clock is not None: os.environ['DM_DETCLOCK'] = old_clock

# ---- take_shot's command and environment, with subprocess.run captured ----
calls = []
class FakeRun:
    returncode = 0; stdout = ''; stderr = ''
old_run, old_prod = golden.subprocess.run, os.environ.get('DM_SHOT_PRODUCT')
def fake_run(cmd, env=None, **kw):   # only the hostshot.sh call is faked; git and the rest run for real
    if not (cmd[:1] == ['bash'] and cmd[1].endswith('hostshot.sh')): return old_run(cmd, env=env, **kw)
    calls.append((cmd, env)); open(cmd[5], 'wb').close(); return FakeRun()
golden.subprocess.run = fake_run; os.environ['DM_SHOT_PRODUCT'] = '/nonexistent/product'
try:
    with tempfile.TemporaryDirectory() as t:
        golden.take_shot('h', 'lb_town_a1', os.path.join(t, 'a.png')); cmd, env = calls[-1]
        expect('DM_TEST_GOTO_LEVEL' not in env and cmd[4] == 'DM:Tick:420#+2', 'lb_town_a1 stays in camp (no level switch) and shoots at tick 420')
        golden.take_shot('h', 'lb_crypt_a1', os.path.join(t, 'b.png'))
        expect(calls[-1][1]['DM_TEST_GOTO_LEVEL'] == '150:34', 'lb_crypt_a1 switches to level 34 at tick 150')
        golden.take_shot('h', 'lb_crypt_a1', os.path.join(t, 'c.png'), {'DM_BRIGHT_K': '4'})
        expect(calls[-1][1]['DM_BRIGHT_K'] == '4', "a set's DM_BRIGHT_K reaches a look-set shot")
        pages = os.path.join(t, 'pages'); os.makedirs(pages)
        json.dump(dict(host=dict(trigger='X+1', env={'DM_BRIGHT_K': '4'})), open(os.path.join(pages, 'tp.json'), 'w'))
        old_pages = golden.PAGES; golden.PAGES = pages
        try: golden.take_shot('h', 'tp', os.path.join(t, 'd.png')); expect(calls[-1][1]['DM_BRIGHT_K'] == '4' and calls[-1][0][4] == 'X+1', "a page spec's DM_BRIGHT_K reaches its shot")
        finally: golden.PAGES = old_pages
    import lookbook  # noqa: E402  # pyright: ignore[reportMissingImports]
    seen = []
    old_ts, old_out = golden.take_shot, lookbook.OUT
    golden.take_shot = lambda host, p, png, extra=None: seen.append((p, extra)) or (True, '')
    try:
        with tempfile.TemporaryDirectory() as t:
            lookbook.OUT = t; lookbook.shoot('t', {'DM_BRIGHT_K': '4'}, '3200x1800', 'h', ['lb_crypt_a1'])
            expect(seen == [('lb_crypt_a1', {'DM_BRIGHT_K': '4', 'DM_GAME_RES': '3200x1800'})] and 'DM_GAME_RES' not in os.environ, 'lookbook passes its variant as the caller env and leaves os.environ alone')
    finally: golden.take_shot, lookbook.OUT = old_ts, old_out
finally:
    golden.subprocess.run = old_run
    if old_prod is None: os.environ.pop('DM_SHOT_PRODUCT', None)
    else: os.environ['DM_SHOT_PRODUCT'] = old_prod

# ---- FLIP judging (TST-1) ----
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
rng = np.random.default_rng(1)
base = np.round((0.35 + 0.25 * rng.random((360, 640, 3))) * 255) / 255
keep = np.ones(base.shape[:2], bool); view = [0, 0, 1, 0.82]
r = golden.flip_region(base, base, keep, view); expect(r['mean'] == 0.0 and r['hot'] == 0.0 and r['ok'], 'identical images: FLIP 0, OK')
r = golden.flip_region(base, base + 1 / 255, keep, view); expect(r['hot'] == 0.0 and 0.02 < r['mean'] < 0.045 and r['ok'], f"a uniform +1 LSB shift passes (mean {r['mean']})")
patch = base.copy(); patch[100:200, 200:300] += 0.3
r = golden.flip_region(base, patch, keep, view); expect(r['hot'] > 0.04 and not r['ok'], f"a 100 x 100 patch at +0.3 fails (hot {r['hot']})")
k2 = keep.copy(); k2[96:204, 196:304] = False
r = golden.flip_region(base, patch, k2, view); expect(r['hot'] == 0.0 and r['ok'], 'the same patch outside the kept pixels passes')
hud = base.copy(); hud[310:350, 300:340] += 0.3
res, okp = golden.judge_page(base, hud, keep, golden.LB_REGIONS)
expect(res['view']['ok'] and not res['hud']['ok'] and not okp, f"a HUD-band change fails the hud region only (hot {res['hud']['hot']})")
one = np.zeros((100, 100, 3)); two = one.copy(); two[50, 50] = 1.0
expect(int(golden.unstable_mask(one, two).sum()) == 25, 'one moving pixel masks a 5 x 5 block (UNSTABLE_GROW 2)')
lim = golden.page_limits({'noise': {'view': {'mean': 0.011, 'hot': 0.0025}, 'hud': {'mean': 0.0, 'hot': 0.0}}})
expect(abs(lim['view'][0] - 0.0075) < 1e-9 and lim['view'][1] == golden.FLIP_MEAN_MAX and lim['hud'] == (golden.FLIP_HOT_MAX, golden.FLIP_MEAN_MAX),
       f'page_limits: NOISE_K x the recorded noise, never below the fixed limits ({lim})')
r = golden.flip_region(base, patch, keep, view, hot_max=0.06); expect(r['ok'], 'a region limit raised above the change lets it pass')

# ---- record / check / controls without the game: a fake shooter ----
SMALL = np.round((0.35 + 0.25 * np.random.default_rng(2).random((180, 320, 3))) * 255) / 255
def shooter(host, p, png, extra=None):
    e = extra or {}; img = SMALL.copy()
    if e.get('DM_AA_MODE') == '1': img[50:90, 100:140] += 0.3
    if e.get('GT_SHIFT') == '1': img = img + 1 / 255
    os.makedirs(os.path.dirname(png), exist_ok=True)
    Image.fromarray((np.clip(img, 0, 1) * 255).round().astype(np.uint8)).save(png)
    open(png[:-4] + '.log', 'w').write('【红】x\n' if e.get('GT_REDLOG') == '1' else 'clean\n')
    for ext in ('.pid', '.state.jsonl'): open(png[:-4] + ext, 'w').write('1\n')
    return True, ''
def run_check(env=None):
    st = golden.SETS['t']; old = st['env']; st['env'] = dict(old, **(env or {}))
    try:
        b, res = golden.flip_check('t', [], 'h')
        return b, res
    finally: st['env'] = old
with tempfile.TemporaryDirectory() as t:
    old_gold, old_root, old_ts = golden.GOLD, golden.scenario.DM_ROOT, golden.take_shot
    golden.GOLD = os.path.join(t, 'gold'); golden.scenario.DM_ROOT = t; golden.take_shot = shooter
    golden.SETS['t'] = dict(env={'GT_SET': '1'}, pages=['lb_town_a1'], judge='flip', dir='t', controls={'fxaa': {'DM_AA_MODE': '1'}}, log_allow=())
    try:
        d = os.path.join(golden.GOLD, 'shots', 't')
        expect(golden.flip_record('t', [], 'test', 'h') == 0, 'record: two equal shots and a seen control → OK')
        cj = json.load(open(os.path.join(d, 'controls.json')))
        meta = json.load(open(os.path.join(d, 'lb_town_a1.meta.json')))
        expect(cj['fxaa']['seen'] is True and meta['env'] == {'GT_SET': '1'} and os.path.exists(os.path.join(d, 'lb_town_a1.png')), 'record keeps the baseline, its meta (set env) and controls.json')
        expect(set(meta.get('noise', {})) == {'view', 'hud'} and meta['noise']['view']['hot'] == 0.0, 'record keeps the third shot\'s noise per region in meta.json')
        golden.SETS['t']['controls'] = {'noop': {'GT_NOOP': '1'}}
        expect(golden.flip_record('t', [], 'test', 'h') == 1, 'record: a control that reads OK everywhere is RED')
        expect(golden.flip_record('t', [], 'test', 'h', allow_blind={'noop': 'why'}) == 0, 'record: --allow-blind with a reason accepts it')
        golden.SETS['t']['controls'] = {'fxaa': {'DM_AA_MODE': '1'}}
        import contextlib  # noqa: E402
        import io  # noqa: E402
        buf = io.StringIO(); golden.SETS['t']['env'] = {'GT_SET': '1', 'GT_REDLOG': '1'}
        with contextlib.redirect_stdout(buf): rc = golden.flip_record('t', [], 'test', 'h')
        golden.SETS['t']['env'] = {'GT_SET': '1'}
        expect(rc == 1 and 'red lines in a clean baseline run' in buf.getvalue(), 'record: red lines in a clean run are RED')
        golden.flip_record('t', [], 'test', 'h')
        b, res = run_check()
        expect(b == 0 and res and res[0]['ok'], 'check: the same picture is OK')
        chk = [r for r, _, fs in os.walk(os.path.join(t, 'run', 'golden')) if r.endswith('check_t')]
        expect(all(not os.listdir(c) for c in chk), 'check: an OK page leaves no .png / .log / .pid / .state.jsonl behind')
        expect(run_check({'GT_SHIFT': '1'})[0] == 1 and 'environment changed' in run_check({'GT_SHIFT': '1'})[1][0]['why'], 'check: a set env that differs from the baseline meta is RED (record again)')
        old_shoot = golden.take_shot
        golden.take_shot = lambda host, p, png, extra=None: shooter(host, p, png, dict(extra or {}, GT_SHIFT='1'))
        expect(run_check()[0] == 0, 'check: a +1 LSB shift passes')
        golden.take_shot = lambda host, p, png, extra=None: shooter(host, p, png, dict(extra or {}, DM_AA_MODE='1'))
        b, res = run_check(); flips = [os.path.join(r, f) for r, _, fs in os.walk(os.path.join(t, 'run', 'golden')) for f in fs if f.endswith('.flip.png')]
        expect(b == 1 and flips, 'check: a changed block is RED and leaves <page>.flip.png')
        golden.take_shot = lambda host, p, png, extra=None: shooter(host, p, png, dict(extra or {}, GT_REDLOG='1'))
        expect(run_check()[0] == 1, 'check: red lines in the shot log are RED')
        golden.SETS['t']['log_allow'] = ('x$',)
        expect(run_check()[0] == 0, 'check: an allowed red line passes')
        golden.take_shot = old_shoot
    finally:
        golden.GOLD, golden.scenario.DM_ROOT, golden.take_shot = old_gold, old_root, old_ts
        golden.SETS.pop('t', None)
print('OK' if not bad else f'RED {bad}')
sys.exit(1 if bad else 0)
