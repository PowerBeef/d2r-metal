#!/usr/bin/env python3
# 我们自己构建录的基线（代替模拟器里原版游戏的真值录制）：逻辑、画面、存档三样，都放在 $DM_ROOT/data/switch/golden/，只在有意时重录（--reason 写进 LOG.tsv）。
#   replay record [路线…] --reason 原因     按 dm/verify/golden/<路线>.scn 跑一局，录命令包与逐拍状态（replay.py record），连同那一局提交的存档
#   replay check [路线…] [--dmhost 宿主]…   在每个宿主（缺省 dmhost，编过 dmhost_m3 就也带上）上回放，逐拍比录制；回放里在录制请存盘的那一拍也请一次，
#                                           回放存下的档与录制的档用 savecmp.py 比（校验和自洽、时间核来源、其余逐字节相同）；
#                                           每条路线再跑一次负对照：从中间那条客户端命令包起往后全删，回放必须在某一拍分叉，否则说明比对是瞎的、判红
#   shots record [页…] --reason 原因        dm/verify/shotgate_pages 里不用录制输入的页，用本工作树的宿主截图存成基线（按渲染接口分目录：m4 / m3）
#   shots check [页…]                       再截一次，与基线用 shotgate.py 比（阈值同页规格）
#   prefix [--reason 原因]                   触屏用例的野外前缀：按 golden/prefix_wild.scn 从原版出生点录一段命令包，写成 $DM_ROOT/data/switch/gate/prefix_wild_warlock，
#                                           并给 save_ref_warlock 写 MANIFEST.sha256（touchchain.sh 按它的指纹找起跑存档）
#   list                                    列出已有基线与重录记录
# 末行 GOLDEN OK|RED {json}；退出码 0 绿、1 红、3 没有基线（先 record）
# 这些基线都是由游戏程序推出来的数据：只放本机，不入库
# Baselines recorded from our own build (instead of ground-truth recordings of the original game in an emulator): logic, screens and saves, all kept in $DM_ROOT/data/switch/golden/, re-recorded only on purpose (--reason goes into LOG.tsv).
#   replay record [route...] --reason why     runs dm/verify/golden/<route>.scn and records its packets and per-tick state (replay.py record), together with the save that run committed
#   replay check [route...] [--dmhost host]...  replays on each host (default dmhost, plus dmhost_m3 when built) and compares tick by tick with the recording; the replay also asks for a save at the tick the recording did,
#                                             and the replay's save is compared with the recorded one by savecmp.py (checksums self-consistent, times checked against their source, everything else byte for byte);
#                                             each route also runs a negative control: the client packets from the middle one onward are dropped and the replay must diverge at some tick, otherwise the comparison is blind and the check is red
#   shots record [page...] --reason why       screenshots the pages in dm/verify/shotgate_pages that need no recorded input with this tree's host and keeps them as the baseline (one folder per rendering API: m4 / m3)
#   shots check [page...]                     screenshots again and compares with the baseline using shotgate.py (thresholds as in the page spec)
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
import replay     # noqa: E402  # pyright: ignore[reportMissingImports]
import scenario   # noqa: E402  # pyright: ignore[reportMissingImports]
import tickdiff   # noqa: E402  # pyright: ignore[reportMissingImports]

GOLD = os.path.join(scenario.DM_ROOT, 'data', 'switch', 'golden')
ROUTES = os.path.join(HERE, 'golden')
PAGES = os.path.join(HERE, 'shotgate_pages')
NEEDS_INPUT = ('DM_C2S_REPLAY',)   # 页规格里带这些的页要真值录制的输入，不录基线 | pages whose spec carries these need recorded ground-truth input: no baseline
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
    """缺省：普通宿主与旧接口宿主都按本工作树编好（逻辑基线两边都要逐拍相同）。 | Default: the normal host and the older-API host, both built from this tree (the logic baseline must match tick by tick on both)."""
    if given: return [os.path.abspath(h) for h in given]
    hs = [replay.default_host()]
    if subprocess.run(['make', '-s', '-C', os.path.join(DM, 'host'), 'dmhost_m3'], capture_output=True, check=False).returncode: raise SystemExit('🔴 make -C dm/host dmhost_m3 failed')
    return hs + [os.path.join(DM, 'host', 'dmhost_m3')]


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
def renderer(host):
    return 'm3' if host.endswith('_m3') else 'm4'


def page_spec(p):
    with open(os.path.join(PAGES, p + '.json'), encoding='utf-8') as f: return json.load(f)


def shot_pages(names):
    ok = [p for p in pick(PAGES, '.json', names) if not any(k in page_spec(p)['host'].get('env', {}) for k in NEEDS_INPUT)]
    for p in ok:
        if p in NO_FIXTURE: print(f'⚪ shot {p} unavailable: {NO_FIXTURE[p]}')
    return [p for p in ok if p not in NO_FIXTURE]


def take_shot(host, p, png):
    """同 shotgate.sh 的一页：页规格的触发、键序与环境，加确定时钟。 | One page as in shotgate.sh: the spec's trigger, key script and environment, plus the deterministic clock."""
    h = page_spec(p)['host']; env = dict(os.environ)
    env.update({k: os.path.expanduser(v) for k, v in h.get('env', {}).items()})
    env.update(DM_BRIGHT_K='1', DM_DETCLOCK=env.get('DM_DETCLOCK', '16666667'))
    if h.get('keys'): env['DM_KEYS'] = h['keys']
    prod = subprocess.run(['bash', os.path.join(DM, 'tools', 'product.sh'), 'path', 'keypage'], capture_output=True, text=True, check=False).stdout.strip()
    os.makedirs(os.path.dirname(png), exist_ok=True)
    p_ = subprocess.run(['bash', os.path.join(HERE, 'hostshot.sh'), host, prod, h['trigger'], png, '240'], env=env, capture_output=True, text=True, check=False)
    return p_.returncode == 0 and os.path.exists(png), (p_.stderr.strip().splitlines() or [''])[-1]


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
        for x in (rec, chk): x.add_argument('--dmhost', action='append', help='host executable (repeatable)')
    sub.add_parser('list')
    pf = sub.add_parser('prefix'); pf.add_argument('--reason', default='touch case prefix')
    a = ap.parse_args(argv)
    if a.kind == 'prefix':
        rc = make_prefix(a.reason); print(f"GOLDEN {'RED' if rc else 'OK'} " + json.dumps(dict(kind='prefix'))); return rc
    if a.kind == 'list':
        for kind in ('replay', 'shots'):
            for p in sorted(glob.glob(os.path.join(GOLD, kind, '**', '*.json'), recursive=True)): print(os.path.relpath(p, GOLD))
        lp = os.path.join(GOLD, 'LOG.tsv')
        if os.path.exists(lp): print(open(lp, encoding='utf-8').read().rstrip())
        return 0
    if a.cmd == 'record':
        bad = replay_record(a.names, a.reason) if a.kind == 'replay' else sum(shots_record(a.names, a.reason, h) for h in host_bins(a.dmhost))
        print(f"GOLDEN {'RED' if bad else 'OK'} " + json.dumps(dict(kind=a.kind, cmd='record', bad=bad)))
        return 1 if bad else 0
    if a.kind == 'replay': bad, res = replay_check(a.names, host_bins(a.dmhost))
    else:
        bad, res = 0, []
        for h in host_bins(a.dmhost):
            b, rr = shots_check(a.names, h)
            if b is not None: bad += b; res += rr
        if not res: bad = None
    if bad is None: print('GOLDEN UNAVAIL ' + json.dumps(dict(kind=a.kind, cmd='check'))); return 3
    print(f"GOLDEN {'RED' if bad else 'OK'} " + json.dumps(dict(kind=a.kind, cmd='check', bad=bad, n=len(res), red=[x.get('route', x.get('page')) + ('/' + x['kind']) for x in res if not x['ok']])))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
