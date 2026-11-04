#!/usr/bin/env python3
# 我们自己构建录的基线（代替模拟器里原版游戏的真值录制）：逻辑、画面、存档三样，都放在 $DM_ROOT/data/switch/golden/，只在有意时重录（--reason 写进 LOG.tsv）。
#   replay record [路线…] --reason 原因     按 dm/verify/golden/<路线>.scn 跑一局，录命令包与逐拍状态（replay.py record），连同那一局提交的存档
#   replay check [路线…] [--dmhost 宿主]…   在每个宿主（缺省 dmhost）上回放，逐拍比录制；回放里在录制请存盘的那一拍也请一次，
#                                           回放存下的档与录制的档用 savecmp.py 比（校验和自洽、时间核来源、其余逐字节相同）；
#                                           每条路线再跑一次负对照：从中间那条客户端命令包起往后全删，回放必须在某一拍分叉，否则说明比对是瞎的、判红
#   prefix [--reason 原因]                   触屏用例的野外前缀：按 golden/prefix_wild.scn 从原版出生点录一段命令包，写成 $DM_ROOT/data/switch/gate/prefix_wild_warlock，
#                                           并给 save_ref_warlock 写 MANIFEST.sha256（touchchain.sh 按它的指纹找起跑存档）
#   list                                    列出已有基线与重录记录
# 末行 GOLDEN OK|RED {json}；退出码 0 绿、1 红、3 没有基线（先 record）
# 这些基线都是由游戏程序推出来的数据：只放本机，不入库
# Baselines recorded from our own build (instead of ground-truth recordings of the original game in an emulator): logic, screens and saves, all kept in $DM_ROOT/data/switch/golden/, re-recorded only on purpose (--reason goes into LOG.tsv).
#   replay record [route...] --reason why     runs dm/verify/golden/<route>.scn and records its packets and per-tick state (replay.py record), together with the save that run committed
#   replay check [route...] [--dmhost host]...  replays on each host (default dmhost) and compares tick by tick with the recording; the replay also asks for a save at the tick the recording did,
#                                             and the replay's save is compared with the recorded one by savecmp.py (checksums self-consistent, times checked against their source, everything else byte for byte);
#                                             each route also runs a negative control: the client packets from the middle one onward are dropped and the replay must diverge at some tick, otherwise the comparison is blind and the check is red
#   shots record [page...] [--set S[,S...]] --reason why [--no-controls] [--allow-blind control=why]...
#                                             screenshots a set's pages with this tree's host and keeps them as its baseline. Set 'pages' (the default): the pages in
#                                             dm/verify/shotgate_pages that need no recorded input, judged by shotgate.py (shots/m4). The FLIP sets (SETS below: orig, rec,
#                                             rec75, bright, phone) shoot each page twice (it must be frame-stable, with a clean log), then run the set's controls: each
#                                             turns one setting back off and must read RED on at least one page, unless --allow-blind gives a measured reason
#   shots controls --set S                    re-runs only a FLIP set's controls against its baseline
#   shots check [page...] [--set S[,S...]|all]  screenshots again and judges against the baseline: shotgate.py for 'pages', FLIP per region for the FLIP sets
#                                             (RED pages keep their shot, log and <page>.flip.png; OK pages leave nothing behind)
#   prefix [--reason why]                     the wilderness prefix for the touch cases: records the packets of golden/prefix_wild.scn from the original spawn point as $DM_ROOT/data/switch/gate/prefix_wild_warlock,
#                                             and writes MANIFEST.sha256 for save_ref_warlock (touchchain.sh finds the starting save by its fingerprint)
#   list                                      lists the baselines and the re-record log
# Last line GOLDEN OK|RED {json}; exit code 0 green, 1 red, 3 no baseline (record first)
# These baselines are data derived from the game program: local only, never committed
import argparse
import datetime
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(DM, 'tools'))
import dmevents   # noqa: E402  # pyright: ignore[reportMissingImports]
import picture    # noqa: E402  # pyright: ignore[reportMissingImports]
import replay     # noqa: E402  # pyright: ignore[reportMissingImports]
import scenario   # noqa: E402  # pyright: ignore[reportMissingImports]
import tickdiff   # noqa: E402  # pyright: ignore[reportMissingImports]

GOLD = os.path.join(scenario.DM_ROOT, 'data', 'switch', 'golden')
ROUTES = os.path.join(HERE, 'golden')
PAGES = os.path.join(HERE, 'shotgate_pages')
NO_FIXTURE = {'cube': 'the Warlock save has no Horadric Cube'}   # 本机存档里没有这页要的东西：不录，报不可用 | the local save lacks what the page needs: not recorded, reported unavailable


def head():
    sha = subprocess.run(['git', '-C', DM, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, check=False).stdout.strip()
    dirty = subprocess.run(['git', '-C', DM, 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True, check=False).stdout.split('\n')
    keep = ('dm/gfx/tools/dmsh_chain.lock', 'dm/gfx/nvn/dmsh_chain.h', 'dm/gfx/nvn/c1_full.tsv')   # setup.sh 改写的三份不算 | the three files setup.sh rewrites do not count
    return sha + ('+' if any(ln.strip() and not ln[3:].strip().endswith(keep) for ln in dirty) else '')


def log(kind, name, reason):
    os.makedirs(GOLD, exist_ok=True)
    with open(os.path.join(GOLD, 'LOG.tsv'), 'a', encoding='utf-8') as f:
        f.write(f"{datetime.datetime.now().isoformat(timespec='seconds')}\t{head()}\t{kind}\t{name}\t{reason}\n")


def pick(folder, ext, names):
    have = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(folder, '*' + ext)))
    bad = [n for n in names if n not in have]
    if bad: raise SystemExit(f'🔴 unknown: {", ".join(bad)} (known: {", ".join(have)})')
    return names or have


def host_bins(given):
    """缺省：本工作树的宿主（逻辑基线要逐拍相同）；--dmhost 可给多个。 | Default: this tree's host (the logic baseline must be tick-identical); --dmhost may be given several times."""
    if given: return [os.path.abspath(h) for h in given]
    return [replay.default_host()]   # 本 fork 只支持 Metal 4（macOS / iOS 27 起）：旧接口宿主不再一起比 | this fork supports Metal 4 only (macOS / iOS 27 and later): the older-API host is no longer compared too


# ---------- 逻辑与存档 | logic and saves ----------
def replay_record(names, reason):
    bad = 0
    for r in pick(ROUTES, '.scn', names):
        out = os.path.join(GOLD, 'replay', r); tmp = out + '.new'
        t0 = int(time.time())
        rc = replay.main(['record', os.path.join(ROUTES, r + '.scn'), '--out', tmp])
        t1 = int(time.time()) + 1
        if rc: bad += 1; print(f'GOLDEN-REC {r} RED recording run failed'); shutil.rmtree(tmp, ignore_errors=True); continue
        evs = dmevents.load(os.path.join(tmp, 'run', 'events.ndjson'))
        req = [e for e in evs if e['ev'] == 'save_request' and e.get('tick') is not None]
        with open(os.path.join(tmp, 'meta.json'), encoding='utf-8') as f: meta = json.load(f)
        meta.update(save_tick=req[-1]['tick'] if req else None, wall_start=t0, wall_end=t1, sha=head(), reason=reason, recorded=datetime.datetime.now().isoformat(timespec='seconds'))
        with open(os.path.join(tmp, 'meta.json'), 'w', encoding='utf-8') as f: json.dump(meta, f, ensure_ascii=False, indent=1)
        shutil.rmtree(out, ignore_errors=True); os.rename(tmp, out); log('replay', r, reason)
        print(f"GOLDEN-REC {r} OK {meta['ticks']} ticks, {meta['packets']} packets, save at tick {meta['save_tick']}")
    return bad


def savecmp(a_save, a_meta, b_save, b_meta, work):
    """savecmp.py 要「目录/0/…」：各建一个 0 指过去。 | savecmp.py wants "dir/0/...": make a 0 link pointing at each."""
    for side, save in (('A', a_save), ('B', b_save)):
        d = os.path.join(work, side); os.makedirs(d, exist_ok=True)
        if not os.path.lexists(os.path.join(d, '0')): os.symlink(save, os.path.join(d, '0'))
    p = subprocess.run([sys.executable, os.path.join(HERE, 'savecmp.py'), os.path.join(work, 'A'), os.path.join(work, 'B'), f'--a-meta={a_meta}', f'--b-meta={b_meta}'],
                       capture_output=True, text=True, check=False)
    return p.returncode, (p.stdout.strip().splitlines() or ['?'])[-1], p.stdout


def drop_second_half(rec, out):
    """负对照：把录制拷一份，从中间那条客户端命令包起往后全删（玩家半路不再有任何操作）。只删一条不够：走路时后面的包会重新指同一个目标，删一条可能真的不改变什么。
    返回被删的第一条包所在的拍；没有客户端包返回 None
    Negative control: copy the recording and drop every client packet from the middle one onward (the player stops doing anything halfway). Dropping a single packet is not enough: while walking the next packets
    re-target the same destination, so one dropped packet may really change nothing. Returns the tick of the first dropped packet; None when there are no client packets"""
    shutil.copytree(rec, out, ignore=shutil.ignore_patterns('replay*', 'run'))
    with open(os.path.join(rec, 'c2s.jsonl'), encoding='utf-8') as f: rows = f.readlines()
    mine = [i for i, ln in enumerate(rows) if json.loads(ln).get('q') != 'sys']
    if not mine: return None
    cut = set(mine[len(mine) // 2:]); tick = json.loads(rows[mine[len(mine) // 2]])['tick']
    with open(os.path.join(out, 'c2s.jsonl'), 'w', encoding='utf-8') as f: f.writelines(ln for i, ln in enumerate(rows) if i not in cut)
    return tick


def replay_check(names, hosts):
    results, bad = [], 0
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
    runs = os.path.join(scenario.DM_ROOT, 'run', 'golden', stamp)
    routes = [r for r in pick(ROUTES, '.scn', names) if os.path.exists(os.path.join(GOLD, 'replay', r, 'meta.json'))]
    if not routes: print('⚪ GOLDEN no replay baselines (golden.py replay record --reason …)'); return None, []
    for r in routes:
        rec = os.path.join(GOLD, 'replay', r)
        with open(os.path.join(rec, 'meta.json'), encoding='utf-8') as f: meta = json.load(f)
        rows = tickdiff.load(rec)
        env_old = os.environ.get('DM_TEST_SAVE_AT')
        if meta.get('save_tick') is not None: os.environ['DM_TEST_SAVE_AT'] = str(meta['save_tick'])
        try:
            for h in hosts:
                hn = os.path.basename(h); out = os.path.join(runs, f'{r}_{hn}')
                t0 = int(time.time()); rc, ticks = replay.replay(rec, h, out); t1 = int(time.time()) + 1
                res: dict[str, Any] = dict(route=r, host=hn, kind='replay')
                if rc != 0 or not os.path.exists(ticks): res.update(ok=False, why=f'replay exited {rc}')
                else:
                    d = tickdiff.compare(rows, tickdiff.load(ticks)); f = d['first'] or {}
                    same = d['same'] and d['common'] == len(rows)
                    res.update(ok=same, why=f"{d['common']} ticks identical" if same else f"first divergent tick {f.get('tick')}: {', '.join(f.get('slots') or []) or '-'}")
                    if same and meta.get('save_tick') is not None:   # 存档：回放在同一拍存下的档要与录制的相同 | saves: the save the replay made at the same tick must match the recorded one
                        with open(os.path.join(out, 'wall.json'), 'w', encoding='utf-8') as wf: json.dump(dict(wall_start=t0, wall_end=t1), wf)
                        src, last, _ = savecmp(os.path.join(rec, 'run', 'save'), os.path.join(rec, 'meta.json'), os.path.join(out, 'save'), os.path.join(out, 'wall.json'), os.path.join(out, 'savecmp'))
                        res.update(save_ok=src == 0, save=last)
                        if src: res['ok'] = False; res['why'] += f'; save differs: {last}'
                bad += not res['ok']; results.append(res)
                print(f"  {r} on {hn}: {'OK' if res['ok'] else 'RED'} {res['why']}" + (f" | {res.get('save')}" if res.get('save') else ''))
            # 负对照：只在第一个宿主上 | negative control: on the first host only
            neg = os.path.join(runs, f'{r}_negative'); dt = drop_second_half(rec, neg)
            res = {'route': r, 'host': os.path.basename(hosts[0]), 'kind': 'negative'}
            if dt is None: res.update(ok=False, why='no client packet to drop')
            else:
                rc, ticks = replay.replay(neg, hosts[0], os.path.join(neg, 'replay'))
                d: dict[str, Any] = tickdiff.compare(rows, tickdiff.load(ticks)) if os.path.exists(ticks) else {'same': False, 'first': {'tick': '?'}, 'common': 0}
                first: dict[str, Any] = d['first'] or {}
                diverged = not d['same'] and bool(first)
                res.update(ok=diverged, why=f"dropped the client packets from tick {dt} on: " + (f"diverged at tick {first.get('tick')}" if diverged else 'NO divergence -- the comparison is blind'))
            bad += not res['ok']; results.append(res)
            print(f"  {r} negative control: {'OK' if res['ok'] else 'RED'} {res['why']}")
        finally:
            if env_old is None: os.environ.pop('DM_TEST_SAVE_AT', None)
            else: os.environ['DM_TEST_SAVE_AT'] = env_old
    return bad, results


# ---------- 触屏用例的前缀 | prefixes for the touch cases ----------
PREFIX_TIME = 1790895128   # touchchain.sh 的 DM_FIXED_TIME：前缀在同一时刻下录 | touchchain.sh's DM_FIXED_TIME: the prefix is recorded at the same time


def save_manifest(save):
    """写 save_ref_<名>/MANIFEST.sha256（同 savesha.sh 的格式：「sha  0000000000000001/相对路径」，按字节序），返回清单本身的 sha256。 | Writes save_ref_<name>/MANIFEST.sha256 (savesha.sh's format: "sha  0000000000000001/relative path", in byte order) and returns the manifest's own sha256."""
    ref = os.path.join(scenario.DM_ROOT, 'data', 'switch', f'save_ref_{save}'); user = os.path.join(ref, '0000000000000001')
    rels = sorted((os.path.relpath(os.path.join(r, f), user) for r, _, fs in os.walk(user) for f in fs if f != '.lock'), key=lambda x: x.encode())
    text = ''.join(f"{hashlib.sha256(open(os.path.join(user, x), 'rb').read()).hexdigest()}  0000000000000001/{x}\n" for x in rels)
    mp = os.path.join(ref, 'MANIFEST.sha256')
    if not os.path.exists(mp) or open(mp, encoding='utf-8').read() != text:
        with open(mp, 'w', encoding='utf-8') as f: f.write(text)
    return hashlib.sha256(text.encode()).hexdigest()


def make_prefix(reason):
    sha = save_manifest('warlock')
    out = os.path.join(scenario.DM_ROOT, 'data', 'switch', 'gate', 'prefix_wild_warlock'); tmp = out + '.rec'
    shutil.rmtree(tmp, ignore_errors=True)
    if replay.main(['record', os.path.join(ROUTES, 'prefix_wild.scn'), '--time', str(PREFIX_TIME), '--out', tmp]): print('GOLDEN-PREFIX RED recording run failed'); return 1
    with open(os.path.join(tmp, 'c2s.jsonl'), encoding='utf-8') as f: last = max(json.loads(ln)['tick'] for ln in f if ln.strip())
    if last >= 650: print(f'GOLDEN-PREFIX RED the prefix ends at tick {last}: prefix cases run 720 ticks, too late for the touch script'); return 1
    os.makedirs(out, exist_ok=True); shutil.copy(os.path.join(tmp, 'c2s.jsonl'), os.path.join(out, 'c2s.jsonl'))
    with open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(dict(start_save_sha=sha, route='dm/verify/golden/prefix_wild.scn', last_tick=last, sha=head(), reason=reason, recorded=datetime.datetime.now().isoformat(timespec='seconds')), f, indent=1)
    shutil.rmtree(tmp, ignore_errors=True); log('prefix', 'wild_warlock', reason)
    print(f'GOLDEN-PREFIX OK prefix_wild_warlock: last packet at tick {last}, start save {sha[:12]}'); return 0


# ---------- 画面 | screens ----------
def renderer(host):   # screen baselines live under shots/m4 (the Metal 4 renderer)
    return 'm4'


def page_spec(p):
    with open(os.path.join(PAGES, p + '.json'), encoding='utf-8') as f: return json.load(f)


def shot_pages(names):
    ok = pick(PAGES, '.json', names)
    for p in ok:
        if p in NO_FIXTURE: print(f'⚪ shot {p} unavailable: {NO_FIXTURE[p]}')
    return [p for p in ok if p not in NO_FIXTURE]


# The look set's own 3D pages (moved from lookbook.py) follow the screenshot gate's recipe (hostshot.sh, the keypage product, the deterministic clock): the
# standard save's Warlock enters camp and, except for lb_town_a1 (the Rogue Encampment itself), switches to the given level at tick 150 with the test switch
# DM_TEST_GOTO_LEVEL; the game clock freezes at tick E (DM_TICK_HOLD=E:SHOT, released once the screenshot is written), the host sends "DM:Tick:E#" at tick E and
# the screenshot is taken 2 frames later, so frame-driven animation (fire, idle poses, orb liquid) lands on the same frame too. DM_STATE is only there to install
# the full tick entry (where the freeze lives). With a fixed random seed and time, the same host captures nearly the same picture twice. Levels: 2 Blood Moor
# (distant wilderness), 34 Catacombs (dark), 76 Spider Forest (foliage detail), 107 River of Flame (fire and bloom). The Den of Evil (8) and the Lut Gholein
# sewers (47) are not used: bats, torch flicker and a timed quest prompt differ every run (FLIP about 0.02 between two runs of the same host)
LB_KEYS = 'X@OpenPanel:MainMenuButtonRibbon>OpenPanel:CharacterSelectPanel,DOWN@%0.5+0.03>UpdateCharacterInfo,A@%0.5>SelectCharacter,A@%1>FrontEndNavigation:ToGame'
LB_PAGES = {f'lb_{n}': dict(level=lv, tick=e, viewport=[0, 0, 1, 0.82])
            for n, lv, e in (('town_a1', None, 420), ('wild_a1', 2, 420), ('crypt_a1', 34, 420), ('jungle_a3', 76, 420), ('lava_a4', 107, 420))}
LB_ALL = list(LB_PAGES)


def shot_env(page_env, extra):
    """A shot's environment. Brightening is fixed at k=1 so a stray shell value never changes the gate; the page spec's env wins over that, and the
    caller's or set's env (extra) wins over both. The deterministic clock unless the shell, the page or the caller sets its own."""
    env = dict(os.environ); env['DM_BRIGHT_K'] = '1'; env.setdefault('DM_DETCLOCK', '16666667')
    env.update(page_env or {}); env.update(extra or {})
    return env


def take_shot(host, p, png, extra=None):
    """One page: a screenshot-gate page from its spec (trigger, key script, environment) or a look-set 3D page (LB_PAGES), with extra (a set's or the
    caller's env) over both."""
    os.makedirs(os.path.dirname(png), exist_ok=True)
    if p in LB_PAGES:
        h = LB_PAGES[p]; e = h['tick']
        page_env = dict(DM_TOWNWP_OFF='1', DM_GAME_SEED='0x12345678', DM_FIXED_TIME='1790000000', DM_TICK_HOLD=f'{e}:SHOT', DM_STATE=png[:-4] + '.state.jsonl', DM_KEYS=LB_KEYS)
        if h['level'] is not None: page_env['DM_TEST_GOTO_LEVEL'] = f"150:{h['level']}"
        trigger, timeout = f'DM:Tick:{e}#+2', '300'
    else:
        h = page_spec(p)['host']
        page_env = {k: os.path.expanduser(v) for k, v in h.get('env', {}).items()}
        if h.get('keys'): page_env['DM_KEYS'] = h['keys']
        trigger, timeout = h['trigger'], '240'
    env = shot_env(page_env, extra)
    if env['DM_BRIGHT_K'] != '1': print(f"  {p}: brightening fixed at k={env['DM_BRIGHT_K']}")
    # DM_SHOT_PRODUCT=<output dir>: shoot another rewrite output (to compare rewrite variants such as the shadow-map size); default keypage
    prod = os.environ.get('DM_SHOT_PRODUCT') or subprocess.run(['bash', os.path.join(DM, 'tools', 'product.sh'), 'path', 'keypage'], capture_output=True, text=True, check=False).stdout.strip()
    p_ = subprocess.run(['bash', os.path.join(HERE, 'hostshot.sh'), host, prod, trigger, png, timeout], env=env, capture_output=True, text=True, check=False)
    return p_.returncode == 0 and os.path.exists(png), (p_.stderr.strip().splitlines() or [''])[-1]


def drop_shot(png, keep=()):
    """Removes a shot and what hostshot.sh / the tick freeze leave beside it (.log, .pid, .state.jsonl), except the extensions in keep."""
    for ext in ('.png', '.log', '.pid', '.state.jsonl'):
        if ext in keep: continue
        try: os.remove(png[:-4] + ext)
        except FileNotFoundError: pass


# ---------- FLIP picture sets ----------
# Each set shoots its pages at one picture (dm/tools/picture.py pins every Video-tab key through its switch) and keeps its baselines in shots/<dir>/. A page
# is judged per region with FLIP (imgdiff.py, 67 pixels per degree): the look-set pages as the 3D view and the HUD band, so a HUD change is not diluted by the
# 3D area; a screenshot-gate page as the whole frame without its spec's mask rects. Pixels that moved between the two recording shots are left out too.
# Controls turn one setting back off: each must read RED on at least one page, so a set that cannot see its own setting is caught (--allow-blind takes a
# measured reason). The REC75 and PHONE sets keep the 3D scale fixed (Frame Drops off): a scale that moves can have no picture baseline.
# The character-select page is in no FLIP set: its campfire's flicker lights most of the frame (46-49% of the pixels moved between two recording
# shots, 2026-11-04, then against a limit of 2%), so it keeps only its SSIM screenshot-gate check.
SETS: dict[str, dict[str, Any]] = {
    'pages': dict(env={}, pages=None, judge='ssim', dir='m4', controls={}, log_allow=()),
    'orig': dict(env={}, pages=list(LB_ALL), judge='flip', dir='orig', log_allow=(),
                 controls={'aa_off': {'DM_AA_MODE': '0'}, 'shadow_1024': {'DM_SHADOW_SIZE': '1024'}}),
    'rec': dict(env=picture.REC, pages=LB_ALL + ['inventory'], judge='flip', dir='rec', log_allow=(),
                controls={'fxaa': {'DM_AA_MODE': '1'}, 'aniso_off': {'DM_NVN_ANISO': '0'}, 'envsh_off': {'DM_ENV_SHADOWS': '0'}, 'shadow_2100': {'DM_SHADOW_SIZE': '2100'}}),
    'rec75': dict(env=picture.REC75, pages=['lb_town_a1', 'lb_wild_a1'], judge='flip', dir='rec75', log_allow=(), controls={'mfx3d_off': {'DM_MFX3D': '0'}}),
    'bright': dict(env=dict(picture.REC, DM_BRIGHT_K='4'), pages=['lb_crypt_a1'], judge='flip', dir='bright', log_allow=(), controls={'bright_off': {'DM_BRIGHT_K': '1'}}),
    'phone': dict(env=picture.PHONE, pages=['lb_town_a1', 'lb_crypt_a1'], judge='flip', dir='phone', log_allow=(), controls={'mfx3d_off': {'DM_MFX3D': '0'}}),
}
FLIP_SETS = [s for s in SETS if SETS[s]['judge'] == 'flip']
# A region passes when at most FLIP_HOT_MAX of its pixels have FLIP above FLIP_HOT and its mean is at most FLIP_MEAN_MAX. Measured with imgdiff on synthetic
# images: a uniform +1/+2 LSB shift gives a per-pixel FLIP of at most 0.062 (mean 0.03-0.057) and passes; +3 LSB everywhere fails on the mean; +6 LSB marks
# every pixel hot; a 100 x 100 patch at +0.3 marks 5.9% of a 640 x 295 view hot.
# Real shots of one frozen tick are not identical (measured 2026-11-04 on the 13 look-set pages): the rain, flames and idle animations move, and the
# game's dither shifts about a quarter of the 3D view by one level. Recording therefore takes RECORD_SHOTS shots. The first MASK_SHOTS give the moving
# pixels: every pixel that differs between the first shot and any of the next ones (unstable_mask, grown UNSTABLE_GROW pixels; at most UNSTABLE_MAX
# of the frame), so the zones a torch's flicker lights differently from run to run are left out; the rest must agree to REPEAT_MAX (the dither
# alone gives 0.002-0.011). The remaining shots, each judged against the first outside that mask, give the page's own noise, the worst of them per
# region, which a check meets again (rain lands somewhere new each run). One mask pair and one noise shot were too few: a torch-lit crypt page
# measured 0.002% of its pixels hot in the noise shot, then 0.3-1.0% in checks of the same build (2026-11-04). A check allows each region NOISE_K
# times its recorded noise, never less than the fixed limits (page_limits).
# A softer or sharper picture (the 3D MetalFX pass off, another anti-aliasing) moves FLIP's mean less than a harmless uniform one-level shift does
# (0.023 against 0.03), so each region also compares its fine detail: the mean absolute Laplacian of luminance over the kept pixels, test / base.
# Measured 2026-11-04 on the look-set pages: a repeat shot 0.999-1.002, a uniform one-level shift 1.000, FXAA for SMAA 0.89, the 3D MetalFX pass off
# 0.68-0.79. A region fails when the ratio is more than DETAIL_TOL from 1 (or NOISE_K times its recorded noise)
FLIP_HOT, FLIP_HOT_MAX, FLIP_MEAN_MAX, DETAIL_TOL = 0.1, 0.0005, 0.06, 0.02
UNSTABLE_GROW, UNSTABLE_MAX, REPEAT_MAX, NOISE_K = 2, 0.45, 0.02, 3
RECORD_SHOTS, MASK_SHOTS = 6, 4
NOT_ALLOWED = 'not allowed'
LB_REGIONS = {'view': [0, 0, 1, 0.82], 'hud': [0, 0.82, 1, 1]}


def page_regions(p):
    return dict(LB_REGIONS) if p in LB_PAGES else {'frame': [0, 0, 1, 1]}


def _rect(rect, shape):
    h, w = shape[:2]; x0, y0, x1, y1 = rect
    return slice(int(round(y0 * h)), int(round(y1 * h))), slice(int(round(x0 * w)), int(round(x1 * w)))


def page_keep(p, shape):
    """The pixels a page is judged on: everything but its spec's mask rects (look-set pages have none)."""
    import numpy as np   # noqa: PLC0415
    keep = np.ones(shape[:2], bool)
    if p not in LB_PAGES:
        for r in page_spec(p).get('mask', []): keep[_rect(r, shape)] = False
    return keep


def unstable_mask(a, b, grow=UNSTABLE_GROW, tol=2 / 255):
    """Pixels whose largest channel differs by more than tol between two captures (things that move on their own: critters, flicker, timed text),
    grown by `grow` pixels."""
    import numpy as np   # noqa: PLC0415
    d = np.abs(a - b).max(2) > tol; m = d.copy()
    for dy in range(-grow, grow + 1):
        for dx in range(-grow, grow + 1): m |= np.roll(np.roll(d, dy, 0), dx, 1)
    return m


def detail(img, keep, rect):
    """Fine detail inside rect over the keep pixels: the mean absolute Laplacian of luminance (0 when there are no pixels)."""
    import numpy as np   # noqa: PLC0415
    y = 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]
    lap = np.abs(4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:])
    ys, xs = _rect(rect, img.shape); m = np.zeros(keep.shape, bool); m[ys, xs] = True; k = (keep & m)[1:-1, 1:-1]
    return float(lap[k].mean()) if k.any() else 0.0


def flip_region(base, test, keep, rect, hot=FLIP_HOT, hot_max=FLIP_HOT_MAX, mean_max=FLIP_MEAN_MAX, fmap=None, detail_tol=DETAIL_TOL) -> dict[str, Any]:
    """FLIP of test against base inside rect, over the keep pixels: dict(mean, hot = share of pixels above `hot`, p99, detail = test's fine detail
    over base's, n, ok, why)."""
    import numpy as np   # noqa: PLC0415
    import imgdiff       # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    f = imgdiff.flip(base, test) if fmap is None else fmap
    ys, xs = _rect(rect, f.shape)
    v = f[ys, xs][keep[ys, xs]]
    if not v.size: return dict(mean=0.0, hot=0.0, p99=0.0, detail=1.0, n=0, ok=True, why='no pixels to judge')
    mean, hs = float(v.mean()), float((v > hot).mean())
    db = detail(base, keep, rect); dr = detail(test, keep, rect) / db if db > 1e-4 else 1.0   # a flat region has no detail to lose
    ok = hs <= hot_max and mean <= mean_max and abs(dr - 1) <= detail_tol
    return dict(mean=round(mean, 5), hot=round(hs, 5), p99=round(float(np.percentile(v, 99)), 5), detail=round(dr, 4), n=int(v.size), ok=ok,
                why='' if ok else f'FLIP mean {mean:.4f} (max {mean_max}), {hs:.3%} of pixels above {hot} (max {hot_max:.3%}), detail x{dr:.3f} (within {detail_tol:.3f} of 1)')


def page_limits(meta):
    """{region: (hot_max, mean_max, detail_tol)} for a page from its recorded noise (meta.json 'noise'): NOISE_K times it, never below the fixed
    limits."""
    return {r: (max(FLIP_HOT_MAX, NOISE_K * v['hot']), max(FLIP_MEAN_MAX, NOISE_K * v['mean']), max(DETAIL_TOL, NOISE_K * abs(v.get('detail', 1.0) - 1)))
            for r, v in (meta or {}).get('noise', {}).items()}


def judge_page(base, test, keep, regions, fmap=None, limits=None) -> tuple[dict[str, dict[str, Any]], bool]:
    """Every region judged on its own: ({region: flip_region result}, ok when every region is ok). limits: page_limits (default the fixed ones)."""
    import imgdiff   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    f = imgdiff.flip(base, test) if fmap is None else fmap
    lim = limits or {}; fixed = (FLIP_HOT_MAX, FLIP_MEAN_MAX, DETAIL_TOL)
    res = {r: flip_region(base, test, keep, rect, hot_max=lim.get(r, fixed)[0], mean_max=lim.get(r, fixed)[1], detail_tol=lim.get(r, fixed)[2], fmap=f)
           for r, rect in regions.items()}
    return res, all(x['ok'] for x in res.values())


def set_dir(s):
    return os.path.join(GOLD, 'shots', SETS[s]['dir'])


def set_names(sets):
    """'all' or a comma list; unknown names stop the run."""
    out = list(SETS) if sets == 'all' else [s for s in sets.split(',') if s]
    bad = [s for s in out if s not in SETS]
    if bad: raise SystemExit(f'🔴 unknown set: {", ".join(bad)} (known: {", ".join(SETS)})')
    return out


def set_pages(s, names):
    pages = SETS[s]['pages']; bad = [n for n in names if n not in pages]
    if bad: raise SystemExit(f'🔴 not in set {s}: {", ".join(bad)} (its pages: {", ".join(pages)})')
    return [p for p in pages if not names or p in names]


def red_lines(png, st):
    """The shot's host log lines with red markers that the set does not allow (the first three, as text)."""
    import assertlib   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    lp = png[:-4] + '.log'
    return [t for _, t in assertlib.logred(lp, allow=st['log_allow'])][:3] if os.path.exists(lp) else []


def page_meta(s, p):
    """A baseline's meta.json (None when there is none)."""
    try:
        with open(os.path.join(set_dir(s), p + '.meta.json'), encoding='utf-8') as f: return json.load(f)
    except (OSError, ValueError): return None


def stored_keep(s, p, shape):
    """page_keep without the pixels recorded as moving (<page>.unstable.png beside the baseline)."""
    import numpy as np   # noqa: PLC0415
    from PIL import Image   # noqa: PLC0415
    keep = page_keep(p, shape); up = os.path.join(set_dir(s), p + '.unstable.png')
    if os.path.exists(up): keep &= ~(np.asarray(Image.open(up).convert('L')) > 0)
    return keep


def run_dir(tag):
    return os.path.join(scenario.DM_ROOT, 'run', 'golden', datetime.datetime.now().strftime('%Y%m%d_%H%M%S') + '_' + tag)


def flip_record(s, names, reason, host, controls=True, allow_blind=None):
    import numpy as np   # noqa: PLC0415
    import imgdiff       # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    from PIL import Image   # noqa: PLC0415
    st, d, runs, bad = SETS[s], set_dir(s), run_dir(f'rec_{s}'), 0
    os.makedirs(d, exist_ok=True)
    for p in set_pages(s, names):
        shots, why, un, rep, noise = [os.path.join(runs, p + ('.png' if i == 0 else f'.{i + 1}.png')) for i in range(RECORD_SHOTS)], None, None, {}, {}
        for png in shots:
            ok, w = take_shot(host, p, png, st['env'])
            if not ok: why = f'screenshot failed: {w}'; break
            red = red_lines(png, st)
            if red: why = f"red lines in a clean baseline run: {' | '.join(red)}; fix them, or add a regex with a reason to SETS[{s!r}]['log_allow']"; break
        if why is None:
            imgs = [imgdiff.load(x) for x in shots]; a = imgs[0]
            if any(x.shape != a.shape for x in imgs): why = 'the shots differ in size (' + ', '.join(f'{x.shape[1]}x{x.shape[0]}' for x in imgs) + ')'
            else:
                un = unstable_mask(a, imgs[1])
                for x in imgs[2:MASK_SHOTS]: un |= unstable_mask(a, x)
                share = float(un.mean()); keep = page_keep(p, a.shape) & ~un
                reps = [judge_page(a, x, keep, page_regions(p))[0] for x in imgs[1:MASK_SHOTS]]
                rep = {r: dict(mean=max(x[r]['mean'] for x in reps)) for r in reps[0]}
                # the later shots outside the moving pixels: the noise a check meets again, the worst of them per region
                ns = [judge_page(a, x, keep, page_regions(p))[0] for x in imgs[MASK_SHOTS:]]
                noise = {r: dict(mean=max(x[r]['mean'] for x in ns), hot=max(x[r]['hot'] for x in ns), detail=max((x[r]['detail'] for x in ns), key=lambda v: abs(v - 1)))
                         for r in ns[0]}
                worst = max(float(r['mean']) for r in rep.values())
                if share > UNSTABLE_MAX or worst > REPEAT_MAX:
                    why = f'page moves between its shots ({share:.1%} moving, repeat FLIP mean {worst:.4f}): not stable enough for a FLIP baseline'
        if why: bad += 1; print(f'GOLDEN-REC {s}/{p} RED {why}'); continue
        dst = os.path.join(d, p + '.png'); shutil.copy(shots[0], dst + '.new.png'); os.replace(dst + '.new.png', dst)
        up = os.path.join(d, p + '.unstable.png')
        if un is not None and un.any(): Image.fromarray(un.astype(np.uint8) * 255, 'L').save(up)
        elif os.path.exists(up): os.remove(up)
        with open(os.path.join(d, p + '.meta.json'), 'w', encoding='utf-8') as f:
            json.dump(dict(set=s, page=p, env=st['env'], regions=page_regions(p), sha=head(), reason=reason, recorded=datetime.datetime.now().isoformat(timespec='seconds'),
                           repeat_mean={r: v['mean'] for r, v in rep.items()}, noise={r: dict(mean=v['mean'], hot=v['hot'], detail=v['detail']) for r, v in noise.items()},
                           unstable_share=round(float(un.mean()), 5) if un is not None else 0.0), f, indent=1)
        for png in shots: drop_shot(png)
        log('shot', f"{st['dir']}/{p}", reason); print(f"GOLDEN-REC shot {st['dir']}/{p} OK")
    if controls and not bad: bad += flip_controls(s, names, host, allow_blind or {}, reason)
    return bad


def flip_controls(s, names, host, allow_blind, reason='controls'):
    """Each control's env over the set's, on every recorded page, judged against the baseline: seen = RED on at least one page."""
    import imgdiff   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    st, d, runs = SETS[s], set_dir(s), run_dir(f'controls_{s}')
    pages = [p for p in set_pages(s, names) if os.path.exists(os.path.join(d, p + '.png'))]
    if not pages: print(f'⚪ GOLDEN no {s} baselines to run controls against'); return 1
    table: dict[str, Any] = {}; errors = 0
    for c, cenv in st['controls'].items():
        row: dict[str, Any] = {}
        for p in pages:
            png = os.path.join(runs, f'{p}.{c}.png'); ok, w = take_shot(host, p, png, dict(st['env'], **cenv))
            if not ok: row[p] = dict(error=w); errors += 1; print(f'  control {c} on {p}: screenshot failed: {w}'); continue
            base, test = imgdiff.load(os.path.join(d, p + '.png')), imgdiff.load(png)
            if base.shape != test.shape: row[p] = dict(red=True, why='size differs'); drop_shot(png); continue
            res, okp = judge_page(base, test, stored_keep(s, p, base.shape), page_regions(p), limits=page_limits(page_meta(s, p)))
            row[p] = {r: dict(mean=v['mean'], hot=v['hot'], detail=v['detail']) for r, v in res.items()}; row[p]['red'] = not okp
            drop_shot(png)
        row['seen'] = any(isinstance(v, dict) and v.get('red') for v in row.values())
        table[c] = row
        print(f"  control {c}: {'seen' if row['seen'] else 'BLIND'}" + (f' (allowed: {allow_blind[c]})' if not row['seen'] and c in allow_blind else ''))
    blind = [c for c in table if not table[c]['seen']]
    with open(os.path.join(d, 'controls.json'), 'w', encoding='utf-8') as f: json.dump(dict(table, _allowed_blind={c: allow_blind[c] for c in blind if c in allow_blind}), f, indent=1)
    log('controls', s, f"{reason}; seen {','.join(c for c in table if table[c]['seen']) or '-'}; blind {','.join(f'{c}={allow_blind.get(c, NOT_ALLOWED)}' for c in blind) or '-'}")
    unallowed = [c for c in blind if c not in allow_blind]
    if unallowed: print(f"GOLDEN-REC {s} RED control(s) {', '.join(unallowed)} read OK on every page: the set cannot see that setting (--allow-blind {unallowed[0]}=<measured reason>)")
    return len(unallowed) + errors


def flip_check(s, names, host):
    import numpy as np   # noqa: PLC0415
    import imgdiff       # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    from PIL import Image   # noqa: PLC0415
    st, d = SETS[s], set_dir(s)
    pages = [p for p in set_pages(s, names) if os.path.exists(os.path.join(d, p + '.png')) and os.path.exists(os.path.join(d, p + '.meta.json'))]
    if not pages: print(f'⚪ GOLDEN no {s} baselines (golden.py shots record --set {s} --reason …)'); return None, []
    runs, results, bad = run_dir(f'check_{s}'), [], 0
    for p in pages:
        with open(os.path.join(d, p + '.meta.json'), encoding='utf-8') as f: meta = json.load(f)
        res: dict[str, Any] = dict(page=f'{s}/{p}', kind='shot'); png = os.path.join(runs, p + '.png')
        if meta.get('env') != st['env']: res.update(ok=False, why='environment changed: record again with --reason')
        else:
            ok, w = take_shot(host, p, png, st['env'])
            if not ok: res.update(ok=False, why=f'screenshot failed: {w}')
            else:
                base, test = imgdiff.load(os.path.join(d, p + '.png')), imgdiff.load(png); red = red_lines(png, st)
                if base.shape != test.shape: res.update(ok=False, why=f'size {test.shape[1]}x{test.shape[0]}, baseline {base.shape[1]}x{base.shape[0]}')
                else:
                    fmap = imgdiff.flip(base, test)
                    rr, okp = judge_page(base, test, stored_keep(s, p, base.shape), page_regions(p), fmap=fmap, limits=page_limits(meta))
                    why = '; '.join(f"{r}: " + (v['why'] or 'mean %.4f, hot %.3f%%' % (v['mean'], 100 * v['hot'])) for r, v in rr.items())
                    if red: why += f"; red lines: {' | '.join(red)}"
                    res.update(ok=okp and not red, why=why, regions=rr)
                    if not res['ok']: Image.fromarray((np.clip(fmap, 0, 1) * 255).astype(np.uint8), 'L').save(png[:-4] + '.flip.png')
                drop_shot(png, keep=() if res['ok'] else ('.png', '.log'))   # a RED page keeps its shot and log beside <page>.flip.png
        bad += not res['ok']; results.append(res)
        print(f"  shot {s}/{p}: {'OK' if res['ok'] else 'RED'} {res['why']}")
    return bad, results


def shots_record(names, reason, host):
    bad, r = 0, renderer(host)
    for p in shot_pages(names):
        dst = os.path.join(GOLD, 'shots', r, p + '.png'); tmp = dst + '.new.png'
        ok, why = take_shot(host, p, tmp)
        if not ok: bad += 1; print(f'GOLDEN-REC shot {p} RED {why}'); continue
        os.replace(tmp, dst)
        with open(dst[:-4] + '.meta.json', 'w', encoding='utf-8') as f: json.dump(dict(page=p, renderer=r, sha=head(), reason=reason, recorded=datetime.datetime.now().isoformat(timespec='seconds')), f, indent=1)
        log('shot', f'{r}/{p}', reason); print(f'GOLDEN-REC shot {r}/{p} OK')
    return bad


def shots_check(names, host):
    results, bad, r = [], 0, renderer(host)
    runs = os.path.join(scenario.DM_ROOT, 'run', 'golden', datetime.datetime.now().strftime('%Y%m%d_%H%M%S') + '_shots')
    pages = [p for p in shot_pages(names) if os.path.exists(os.path.join(GOLD, 'shots', r, p + '.png'))]
    if not pages: print(f'⚪ GOLDEN no {r} screen baselines (golden.py shots record --reason …)'); return None, []
    for p in pages:
        png = os.path.join(runs, p + '.png'); ok, why = take_shot(host, p, png)
        res: dict[str, Any] = dict(page=p, renderer=r, kind='shot')
        if not ok: res.update(ok=False, why=f'screenshot failed: {why}')
        else:
            g = subprocess.run([sys.executable, os.path.join(HERE, 'shotgate.py'), os.path.join(PAGES, p + '.json'), os.path.join(GOLD, 'shots', r, p + '.png'), png, os.path.join(runs, p)],
                               capture_output=True, text=True, check=False)
            res.update(ok=g.returncode == 0, why=(g.stdout.strip().splitlines() or ['?'])[-1][:200])
        drop_shot(png, keep=('.png', '.log', '.state.jsonl'))   # the pid file hostshot.sh wrote is of no use once the host is gone
        bad += not res['ok']; results.append(res)
        print(f"  shot {r}/{p}: {'OK' if res['ok'] else 'RED'} {res['why']}")
    return bad, results


def main(argv):
    ap = argparse.ArgumentParser(prog='golden.py', description='Baselines recorded from our own build: logic (replay), saves (savecmp) and screens (shotgate).')
    sub = ap.add_subparsers(dest='kind', required=True)
    for kind in ('replay', 'shots'):
        k = sub.add_parser(kind); ks = k.add_subparsers(dest='cmd', required=True)
        rec = ks.add_parser('record'); rec.add_argument('names', nargs='*'); rec.add_argument('--reason', required=True, help='why the baseline is (re)recorded; goes into LOG.tsv')
        chk = ks.add_parser('check'); chk.add_argument('names', nargs='*')
        xs = [rec, chk]
        if kind == 'shots':
            ctl = ks.add_parser('controls'); ctl.add_argument('names', nargs='*'); xs.append(ctl)
            for x in xs: x.add_argument('--set', default='pages', help="'pages' (shotgate.py, the default), a FLIP set, a comma list, or 'all' (check)")
            for x in (rec, ctl): x.add_argument('--allow-blind', action='append', default=[], metavar='CONTROL=WHY', help='accept a control that reads OK on every page, with the measured reason')
            rec.add_argument('--no-controls', action='store_true', help='record the FLIP baselines without running their controls')
        for x in xs: x.add_argument('--dmhost', action='append', help='host executable (repeatable)')
    sub.add_parser('list')
    pf = sub.add_parser('prefix'); pf.add_argument('--reason', default='touch case prefix')
    a = ap.parse_args(argv)
    if a.kind == 'prefix':
        rc = make_prefix(a.reason); print(f"GOLDEN {'RED' if rc else 'OK'} " + json.dumps(dict(kind='prefix'))); return rc
    if a.kind == 'list':
        for p in sorted(glob.glob(os.path.join(GOLD, 'replay', '**', '*.json'), recursive=True)): print(os.path.relpath(p, GOLD))
        for s, st in SETS.items():
            have = sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(set_dir(s), '*.png')) if not p.endswith(('.unstable.png', '.new.png')))
            print(f"set {s} (shots/{st['dir']}): {', '.join(have) or 'no baselines'}")
        lp = os.path.join(GOLD, 'LOG.tsv')
        if os.path.exists(lp): print(open(lp, encoding='utf-8').read().rstrip())
        return 0
    sets = set_names(a.set) if a.kind == 'shots' else []
    if a.kind == 'shots' and a.cmd != 'check' and 'all' in a.set.split(','): raise SystemExit("🔴 --set all is for check only")
    blind = dict(x.split('=', 1) for x in getattr(a, 'allow_blind', []) if '=' in x)
    per_set = {}   # the given pages that belong to each set ([] = all of its pages); a set holding none of them is skipped
    for s in sets:
        known = SETS[s]['pages'] if s != 'pages' else [os.path.basename(p)[:-5] for p in glob.glob(os.path.join(PAGES, '*.json'))]
        mine = [n for n in a.names if n in known]
        if not a.names or mine: per_set[s] = mine
    stray = [n for n in getattr(a, 'names', []) if not any(n in v for v in per_set.values())]
    if a.kind == 'shots' and stray: raise SystemExit(f"🔴 not in the set(s) {', '.join(sets)}: {', '.join(stray)}")
    if a.cmd in ('record', 'controls'):
        bad = 0
        for h in host_bins(a.dmhost):
            if a.kind == 'replay': bad += replay_record(a.names, a.reason); break
            for s, nm in per_set.items():
                if s == 'pages': bad += shots_record(nm, a.reason, h) if a.cmd == 'record' else 0
                elif a.cmd == 'record': bad += flip_record(s, nm, a.reason, h, controls=not a.no_controls, allow_blind=blind)
                else: bad += flip_controls(s, nm, h, blind)
        print(f"GOLDEN {'RED' if bad else 'OK'} " + json.dumps(dict(kind=a.kind, cmd=a.cmd, sets=sets, bad=bad)))
        return 1 if bad else 0
    if a.kind == 'replay': bad, res = replay_check(a.names, host_bins(a.dmhost))
    else:
        bad, res = 0, []
        for h in host_bins(a.dmhost):
            for s, nm in per_set.items():
                b, rr = shots_check(nm, h) if s == 'pages' else flip_check(s, nm, h)
                if b is not None: bad += b; res += rr
        if not res: bad = None
    if bad is None: print('GOLDEN UNAVAIL ' + json.dumps(dict(kind=a.kind, cmd='check'))); return 3
    print(f"GOLDEN {'RED' if bad else 'OK'} " + json.dumps(dict(kind=a.kind, cmd='check', bad=bad, n=len(res), red=[x.get('route', x.get('page')) + ('/' + x['kind']) for x in res if not x['ok']])))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
