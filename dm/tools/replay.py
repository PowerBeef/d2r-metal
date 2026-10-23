#!/usr/bin/env python3
# 自录自放：跑一个场景时把客户端命令包（DM_C2S_LOG）与逐拍状态（DM_TICKS_FULL）录下来，之后在任意宿主构建上按拍回放（DM_C2S_REPLAY），逐拍比对，找第一个不同的拍（tickdiff.py）。
# 用来问「这个改动 / 这个构建改没改游戏逻辑」：同一份录制在两个构建上回放应逐拍相同。实测：消毒器构建、旧接口构建与普通构建逐拍相同；删掉一条移动包，第一个不同的拍正是那一拍、说是玩家的模式变了。
#   record <场景> [--seed N] [--time 秒] [--out 目录] [--window]   录制目录：meta.json（存档、种子、时刻、进局键序、拍数）、c2s.jsonl、ticks.jsonl、run/（场景结果）
#   run <录制目录> [--dmhost 宿主] [--out 目录]                     回放一次，写 <目录>/ticks.jsonl
#   check <录制目录> [--dmhost 宿主]…                               在每个宿主上回放，与录制逐拍比；判定行 REPLAY OK|RED，末行 RESULT {json}
#   确定性所需（dm/verify/tick_state_format.md「确定性」）：固定开局种子 DM_GAME_SEED 与时刻 DM_FIXED_TIME；「进局落在城镇小站旁」（dm_townwp.c）回放时缺省不挪（原版录制从原版出生点走起），这里录制照常开着、回放显式 DM_TOWNWP_OFF=0 也挪，
#   否则录制从挪过的出生点走起、回放从原版出生点走起，移动包全部落空；
#   进局前的加入握手（拍 0 的系统类消息）不进回放文件：回放只在文件首末拍之间丢弃客户端自发的消息，首拍若是 0，宿主客户端自己的加入也被丢掉，服务端永远等不到玩家、拍不前进
#   录制目录里是由游戏程序推出来的数据：只放本机，不入库
# Self-recorded replay: while a scenario runs, record the client command packets (DM_C2S_LOG) and the per-tick state (DM_TICKS_FULL), then replay by tick on any host build (DM_C2S_REPLAY), compare tick by tick and find the first divergent tick (tickdiff.py).
# It answers "does this change / this build change the game logic": the same recording replayed on two builds should be identical tick by tick. Measured: the sanitizer builds and the older-API build match the normal build tick by tick; with one movement packet removed, the first divergent tick is exactly that tick, naming the player's mode.
#   record <scenario> [--seed N] [--time seconds] [--out dir] [--window]   recording folder: meta.json (save, seed, time, entry key script, tick count), c2s.jsonl, ticks.jsonl, run/ (the scenario result)
#   run <recording> [--dmhost host] [--out dir]                           replay once, writes <dir>/ticks.jsonl
#   check <recording> [--dmhost host]...                                  replay on each host and compare with the recording tick by tick; verdict line REPLAY OK|RED, last line RESULT {json}
#   Needed for determinism (dm/verify/tick_state_format.md, "Determinism"): a fixed game-creation seed DM_GAME_SEED and time DM_FIXED_TIME; the "join lands next to the town waypoint" feature (dm_townwp.c) does not move the spawn during replay by default (original-game recordings walk from the original spawn), so here the recording keeps it on as usual and the replay sets DM_TOWNWP_OFF=0 explicitly to move it too,
#   otherwise the recording walks from the moved spawn point and the replay from the original one, and every movement packet misses;
#   the join handshake before the game (system-class messages at tick 0) is left out of the replay file: replay drops the client's own messages only between the file's first and last ticks, and with a first tick of 0 the host client's own join is dropped too, so the server never gets the player and ticks never advance
#   A recording folder holds data derived from the game program: local only, never committed
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scenario   # noqa: E402  # pyright: ignore[reportMissingImports]
import tickdiff   # noqa: E402  # pyright: ignore[reportMissingImports]


def entry_keys(slot):
    """回放进局的键序，与场景宏 enter_game 同一条路：主菜单按 X 进选角页，停 1 秒发 CharacterSelect:LoadCharacter:<槽>。 | The key script that enters the game for the replay, along the same path as the enter_game scenario macro: X at the main menu to open character select, then after 1 s send CharacterSelect:LoadCharacter:<slot>."""
    return (f'X@OpenPanel:MainMenuButtonRibbon>OpenPanel:CharacterSelectPanel,'
            f'MSG:CharacterSelect:LoadCharacter:{slot}@OpenPanel:CharacterSelectPanel%1>FrontEndNavigation:ToGame')


def record(a):
    meta_s, steps, _ = scenario.parse(a.scenario)
    entries = [st for st in steps if st['op'] == 'enter_game']
    if len(entries) != 1 or steps[0]['op'] != 'enter_game': raise scenario.ScenarioError('the scenario must start with one enter_game (the replay enters the game the same way)')
    out = os.path.abspath(a.out or os.path.join(scenario.DM_ROOT, 'run', 'replay', f"{meta_s['name']}_{a.seed}"))
    if os.path.exists(out): shutil.rmtree(out)
    os.makedirs(out)
    scenario.ensure_save(meta_s['save'], not a.window)
    r = scenario.Runner(a.scenario, 'cli', os.path.join(out, 'run'), not a.window)
    env = r.meta['env']
    env.setdefault('DM_GAME_SEED', str(a.seed)); env.setdefault('DM_FIXED_TIME', str(a.time))   # 场景自己定了种子 / 时刻（基准路线的坐标靠它）就用场景的 | a seed / time the scenario sets itself (benchmark routes rely on it for their coordinates) wins
    env.update(DM_C2S_LOG=os.path.join(out, 'c2s.raw.jsonl'), DM_TICKS=os.path.join(out, 'ticks.jsonl'), DM_TICKS_FULL='1')
    res, code = r.go()
    if code: print(f"REPLAY RED 录制那一局红了 | the recorded run went red: {res.get('reason')}"); return code
    with open(os.path.join(out, 'c2s.raw.jsonl'), encoding='utf-8') as f: raw = [ln for ln in f if ln.strip()]
    kept = [ln for ln in raw if json.loads(ln)['tick'] > 0]   # 拍 0 的加入握手不回放（见文件头） | the tick-0 join handshake is not replayed (see the file header)
    with open(os.path.join(out, 'c2s.jsonl'), 'w', encoding='utf-8') as f: f.writelines(kept)
    ticks = sum(1 for row in tickdiff.load(out) if row)
    meta = dict(save=os.path.dirname(scenario.save_dir(meta_s['save'])), save_name=meta_s['save'], game_seed=int(env['DM_GAME_SEED'], 0), fix_time=int(env['DM_FIXED_TIME'], 0),
                keys=entry_keys(entries[0]['kw'].get('slot', 0)), ticks=ticks, packets=len(kept), scenario=os.path.abspath(a.scenario))
    with open(os.path.join(out, 'meta.json'), 'w', encoding='utf-8') as f: json.dump(meta, f, ensure_ascii=False, indent=1)
    print(f'REPLAY 已录 | recorded {ticks} 拍 | ticks、{len(kept)} 条命令包 | packets（丢掉拍 0 的 {len(raw) - len(kept)} 条 | dropped {len(raw) - len(kept)} at tick 0）→ {out}')
    return 0


def replay(rec, dmhost, out):
    """在 dmhost 上回放录制 rec 一次：返回 (退出码, ticks.jsonl 路径)。 | Replay recording rec once on dmhost: returns (exit code, path of ticks.jsonl)."""
    with open(os.path.join(rec, 'meta.json'), encoding='utf-8') as f: meta = json.load(f)
    if os.path.exists(out): shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'tmp'))
    shutil.copytree(os.path.join(meta['save'], '0'), os.path.join(out, 'save'))
    prod = subprocess.run(['bash', os.path.join(scenario.DM, 'tools', 'product.sh'), 'path', 'touch'], capture_output=True, text=True, check=False).stdout.strip()
    env = dict(os.environ, DM_ROMFS=os.environ.get('DM_ROMFS') or os.path.join(scenario.DM_ROOT, 'data', 'switch', '3.3.94012', 'Update', 'RomFS'),
               DM_SAVE=os.path.join(out, 'save'), DM_TEMP=os.path.join(out, 'tmp'), DM_MSL_DIR=os.path.join(scenario.DM_ROOT, 'build', 'cache', 'metal_shader_cache'),
               DM_GAME_SEED=str(meta['game_seed']), DM_FIXED_TIME=str(meta['fix_time']), DM_KEYS=meta['keys'], DM_C2S_REPLAY=os.path.join(rec, 'c2s.jsonl'),
               DM_TICKS=os.path.join(out, 'ticks.jsonl'), DM_TICKS_FULL='1', DM_TICKS_N=str(meta['ticks']), DM_WATCH='600', DM_EVENTS=os.path.join(out, 'events.ndjson'),
               DM_TOWNWP_OFF='0')   # 录制时城镇落点照常挪过，回放也挪（dm_townwp.c） | the recording had the town spawn moved as usual, so the replay moves it too (dm_townwp.c)
    for k in ('DM_CTL_PORT', 'DM_WINDOW', 'DM_C2S_LOG'): env.pop(k, None)
    with open(os.path.join(out, 'host.log'), 'wb') as log:
        rc = subprocess.run([dmhost, prod, 'main', 'sdk'], cwd=os.path.join(scenario.DM, 'host'), env=env, stdout=log, stderr=subprocess.STDOUT, check=False).returncode
    return rc, os.path.join(out, 'ticks.jsonl')


def default_host():
    h = os.path.join(scenario.DM, 'host', 'dmhost')
    if subprocess.run(['make', '-s', '-C', os.path.dirname(h), 'dmhost'], capture_output=True, check=False).returncode: raise scenario.ScenarioError('host build failed (make -C dm/host dmhost)')
    return h


def main(argv):
    ap = argparse.ArgumentParser(prog='replay.py', description='Record a scenario run (command packets + per-tick state), replay it on host builds, find the first divergent tick.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record'); r.add_argument('scenario'); r.add_argument('--seed', type=int, default=4242, help='game seed unless the scenario sets DM_GAME_SEED'); r.add_argument('--time', type=int, default=1790000000, help='fixed time unless the scenario sets DM_FIXED_TIME')
    r.add_argument('--out'); r.add_argument('--window', action='store_true')
    p = sub.add_parser('run'); p.add_argument('recording'); p.add_argument('--dmhost'); p.add_argument('--out')
    c = sub.add_parser('check'); c.add_argument('recording'); c.add_argument('--dmhost', action='append', help='host executable (repeatable; default dm/host/dmhost)')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'record': return record(a)
        rec = os.path.abspath(a.recording)
        if a.cmd == 'run':
            rc, ticks = replay(rec, os.path.abspath(a.dmhost) if a.dmhost else default_host(), os.path.abspath(a.out or os.path.join(rec, 'replay')))
            print(f"REPLAY {'OK' if rc == 0 else 'RED'} 退出码 | exit code {rc} → {ticks}"); return 0 if rc == 0 else 1
        hosts = [os.path.abspath(h) for h in a.dmhost] if a.dmhost else [default_host()]
        results, bad = [], 0
        for h in hosts:
            name = os.path.basename(h)
            rc, ticks = replay(rec, h, os.path.join(rec, f'replay_{name}'))
            if rc != 0 or not os.path.exists(ticks): bad += 1; results.append(dict(host=name, verdict='RED', exit=rc)); print(f'  {name}: 回放失败 | replay failed (exit code {rc})'); continue
            d = tickdiff.compare(tickdiff.load(rec), tickdiff.load(ticks))
            ok = d['same'] and d['common'] == len(tickdiff.load(rec)); bad += not ok
            f = d['first'] or {}
            print(f"  {name}: " + (f"{d['common']} 拍逐拍相同 | ticks identical" if ok else
                                    f"第一个不同的拍 | first divergent tick {f.get('tick')}：{', '.join(f.get('slots') or []) or '-'}" if f else f"只比到 {d['common']} 拍 | only {d['common']} ticks compared"))
            for line in (f.get('units') or [])[:10]: print(f'    {line}')
            results.append(dict(host=name, verdict='OK' if ok else 'RED', common=d['common'], first=d['first']))
        print(f"REPLAY {'RED' if bad else 'OK'} {len(hosts) - bad}/{len(hosts)} 个宿主与录制逐拍相同 | hosts match the recording tick by tick")
        print('RESULT ' + json.dumps(dict(name='replay-check', verdict='RED' if bad else 'OK', recording=rec, hosts=results), ensure_ascii=False))
        return 1 if bad else 0
    except (scenario.ScenarioError, OSError, ValueError, KeyError) as e:
        print(f'REPLAY RED {e}'); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
