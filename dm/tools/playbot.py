#!/usr/bin/env python3
# 试玩机器人：按种子从一张「玩家意图」表里抽一串（开关总面板翻页签、暂停页、喝腰带、营地里走、出城打几下再回来、保存并退出再进局、站一会儿），
# 每个意图只走玩家走得到的路（按键、走路、点世界；不发 msg、不碰已知会崩的页：无名 ClosePanel、LootFilterOptionsPanel、大小写变体的页名），
# 展开成场景交给场景运行器跑（dm/tools/scenario.py）。意图之间查不变量：拍在走（没暂停时）、面板栈正常（HUD 在、该关的关了）、有面板时焦点不为空；
# 收尾查：没有红、没有 save_stuck、已提交的存档合法（运行器每局都查）。红了先重跑看能不能复现，再按意图做增量调试（explore.py 的 ddmin）缩到最短。
#   playbot.py run [--seed N] [--episodes K] [--intents N] [--save 存档] [--dmhost 宿主] [--window] [--no-shrink] [--budget 次数]
#   playbot.py soak --minutes M [--seed N] [--window]   一局跑 M 分钟（开 DM_SOAK），收尾用 dm/ios/tools/soak_report.py 的四条判据（拍、内存斜率、句柄 / 线程、帧时间）
#   结果目录 $DM_ROOT/run/playbot/<时刻>_<种子>/；判定行 PLAYBOT OK|RED，末行 RESULT {json}
#   作为模块：plan(种子, 个数) 抽意图，expand(意图串) 展开成场景步（单测用）
# Playtest bot: draws a seeded sequence from a table of "player intents" (open the combined panel and flip tabs, the pause page, drink from the belt, walk around the camp, leave town to fight briefly and come back, save and exit then rejoin, stand still),
# each intent taking only paths a player can take (buttons, walking, world taps; no msg steps and none of the pages known to crash: a nameless ClosePanel, LootFilterOptionsPanel, case-variant page names),
# expanded into a scenario for the scenario runner (dm/tools/scenario.py). Between intents it checks invariants: ticks advance (when not paused), the panel stack is sane (HUD present, closed panels gone), focus is not null while a panel is open;
# at the end: nothing red, no save_stuck, a valid committed save (the runner checks that every run). When red, it reruns to see whether it reproduces, then shrinks by intent with delta debugging (explore.py's ddmin).
#   playbot.py run [--seed N] [--episodes K] [--intents N] [--save save] [--dmhost host] [--window] [--no-shrink] [--budget runs]
#   playbot.py soak --minutes M [--seed N] [--window]   one run of M minutes (with DM_SOAK), judged at the end by dm/ios/tools/soak_report.py's four criteria (ticks, memory slope, handles / threads, frame time)
#   Result folder $DM_ROOT/run/playbot/<time>_<seed>/; verdict line PLAYBOT OK|RED, last line RESULT {json}
#   As a module: plan(seed, count) draws intents, expand(intents) turns them into scenario steps (used by the unit tests)
import argparse
import datetime
import json
import os
import random
import subprocess
import sys
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..'))
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
sys.path.insert(0, HERE)
import explore   # noqa: E402  # pyright: ignore[reportMissingImports]

TOWN = ((5789, 4470), (5762, 4413), (5789, 4414), (5789, 4495))   # 罗格营地里走得到的几格（同基准路线） | cells reachable inside the Rogue Encampment (as in the benchmark route)
GATE, WILD = (5791, 4523), ((5791, 4560), (5780, 4575), (5800, 4570))   # 南门外第一格（进血腥荒野）与荒野里的几格 | the first cell past the south gate (into the Blood Moor) and a few cells in the moor
TABS = ('L', 'R')

# 每个意图：(权重, 名字, 生成函数 r -> 场景步)。都从「局内、没开面板、在营地或刚回营地」出发，也回到这个状态
# Each intent: (weight, name, generator r -> scenario steps). Each starts from "in a run, no panel open, in or just back in the camp" and returns to that state
def _tabs(r):
    steps = ['press PLUS', 'expect_state panels~=UISwitcher focus_panel!=None timeout=5']
    for _ in range(r.randint(1, 5)): steps += [f'press {r.choice(TABS)}', 'sleep 0.3', 'expect_state panels~=UISwitcher timeout=5']
    return steps + ['press B', 'expect_state focus_panel=None timeout=5']


def _pause(r):
    return ['open_pause', f'sleep {r.choice((0.3, 1))}', 'close_pause', 'expect_state focus_panel=None paused=false timeout=5']


def _belt(r):
    return [f'press {r.choice(("LEFT", "UP", "RIGHT", "DOWN"))}', 'sleep 0.5']


def _walk(r):
    x, y = r.choice(TOWN)
    return [f'walk_to {x} {y} timeout=30', 'expect_state area=1']


def _leave_town(r, fight=False):
    steps = [f'walk_to {TOWN[3][0]} {TOWN[3][1]} timeout=30', f'walk_to {GATE[0]} {GATE[1]} timeout=25', 'expect_state area=2 timeout=10']
    x, y = r.choice(WILD); steps.append(f'walk_to {x} {y} timeout=25')
    if fight:   # 点世界：有怪就走过去打，没有就什么都不发（不会因为没怪判红） | world taps: walk over and attack a monster if there is one, else nothing is sent (no red for a missing monster)
        for _ in range(r.randint(1, 3)): steps += [f'world_tap {r.uniform(0.35, 0.65):.3f} {r.uniform(0.3, 0.6):.3f}', 'sleep 1']
    return steps + [f'walk_to {GATE[0]} {GATE[1]} timeout=30', f'walk_to {TOWN[3][0]} {TOWN[3][1]} timeout=30', 'expect_state area=1 dead=false timeout=10']


def _save_reload(r):   # 暂停页焦点默认在「保存并退出」：A 退到前端，再从主菜单进同一个角色 | the pause page's focus starts on "Save and Exit": A leaves to the front end, then the same character is loaded from the main menu
    return ['open_pause', 'press A', 'expect game_leave timeout=30', 'expect main_menu timeout=60', 'sleep 1', 'enter_game', 'expect_state in_game=true panels~=HUDPanel timeout=10']


def _idle(r):
    return [f'sleep {r.choice((0.5, 1, 2))}']


INTENTS = (
    (20, 'tabs', _tabs), (10, 'pause', _pause), (8, 'belt', _belt), (20, 'walk', _walk),
    (8, 'leave_town', lambda r: _leave_town(r)), (6, 'fight', lambda r: _leave_town(r, True)), (4, 'save_reload', _save_reload), (10, 'idle', _idle),
)
# 两个意图之间：拍在走（0.5 秒 ≥ 5 拍；没暂停）、HUD 在面板栈上、没有焦点（面板都关了） | between two intents: ticks advance (≥ 5 ticks in 0.5 s; not paused), the HUD is on the panel stack, no focus (all panels closed)
HEARTBEAT = ['remember tick', 'sleep 0.5', 'expect_state tick>=$tick+5 paused=false panels~=HUDPanel focus_panel=None timeout=5']


def plan(seed, n):
    """按种子抽 n 个意图：[(名字, 场景步)]（同种子同结果）。 | Draw n intents from the seed: [(name, scenario steps)] (same seed, same result)."""
    r = random.Random(seed); total = sum(w for w, _, _ in INTENTS); out = []
    for _ in range(n):
        x = r.uniform(0, total)
        for w, name, g in INTENTS:
            x -= w
            if x <= 0: out.append((name, g(r))); break
        else: out.append((INTENTS[-1][1], INTENTS[-1][2](r)))
    return out


def expand(intents):
    steps = []
    for name, st in intents: steps += [f'log intent {name}', *st, *HEARTBEAT]
    return steps


def scenario_text(name, save, intents, timeout, env=''):
    return '\n'.join([f'# playbot.py 生成 | generated by playbot.py: {name}', f'name {name}', f'save {save}', f'timeout {timeout}', *([f'env {env}'] if env else []), '',
                      'enter_game', *HEARTBEAT, *expand(intents), 'quit', '', 'check no_red', 'check no_red_log', 'check no_event save_stuck', '']) + '\n'


class Bot:
    def __init__(self, a):
        self.a = a
        self.out = os.path.join(DM_ROOT, 'run', 'playbot', f"{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}_{a.seed}")
        os.makedirs(self.out, exist_ok=True); self.n = 0
        if getattr(a, 'dmhost', None): os.environ['SCN_DMHOST'] = os.path.abspath(a.dmhost)
        import scenario   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
        self.scenario = scenario

    def run(self, tag, intents, timeout=None, env=''):
        self.n += 1
        path = os.path.join(self.out, f'{tag}.scn')
        with open(path, 'w', encoding='utf-8') as f: f.write(scenario_text(f'playbot_{tag}', self.a.save, intents, timeout or 60 + 25 * len(intents), env))
        try: res, _ = self.scenario.Runner(path, 'cli', os.path.join(self.out, tag), not self.a.window).go()
        except self.scenario.ScenarioError as e: res = dict(verdict='RED', reason=str(e))
        return res, explore.signature(res)

    def episodes(self):
        report: dict[str, Any] = dict(name='playbot', seed=self.a.seed, episodes=[], dir=self.out)
        for k in range(self.a.episodes):
            seed = self.a.seed * 1000 + k; intents = plan(seed, self.a.intents)
            res, sig = self.run(f'episode_{k}', intents)
            ep: dict[str, Any] = dict(episode=k, seed=seed, intents=[n for n, _ in intents], verdict=res.get('verdict'), signature=sig, dir=res.get('dir'))
            report['episodes'].append(ep)
            print(f"EPISODE {k} {res.get('verdict')} {' '.join(ep['intents'])}" + (f'：{sig}' if sig else ''), flush=True)
            if not sig: continue
            again, sig2 = self.run(f'episode_{k}_again', intents); ep['reproduced'] = sig2 == sig
            if sig2 != sig: print(f'  重跑没复现（{sig2 or "OK"}）：不缩 | the rerun did not reproduce ({sig2 or "OK"}): not shrinking'); continue
            if self.a.no_shrink: continue
            tries = 0
            def fails(sub):
                nonlocal tries
                tries += 1; _, s = self.run(f'shrink_{k}_{tries}', sub); print(f"  缩 | shrink {tries}: {len(sub)} 个意图 | intents → {s or 'OK'}", flush=True); return s == sig
            small, runs = explore.ddmin(intents, fails, self.a.budget)
            mpath = os.path.join(self.out, f'minimal_{k}.scn')
            with open(mpath, 'w', encoding='utf-8') as f: f.write(scenario_text(f'playbot_minimal_{k}', self.a.save, small, 60 + 25 * len(small)))
            ep.update(minimal=mpath, minimal_intents=[n for n, _ in small], shrink_runs=runs)
            print(f"  最短复现 | minimal reproducer: {' '.join(n for n, _ in small)}（试了 {runs} 次 | {runs} tries）→ {mpath}", flush=True)
        red = [e for e in report['episodes'] if e['signature']]
        print(f"PLAYBOT {'RED' if red else 'OK'} {len(report['episodes'])} 局 | episodes，{self.a.intents} 个意图一局 | intents each" + (f"；第一个红 | first red: episode {red[0]['episode']}：{red[0]['signature']}" if red else ''))
        report.update(verdict='RED' if red else 'OK', runs=self.n)
        return report, 1 if red else 0

    def soak(self):
        # 意图大约 6 秒一个：抽够 M 分钟的量；DM_SOAK 每 10 秒采一行 | about 6 s per intent: draw enough for M minutes; DM_SOAK samples one row every 10 s
        n = max(10, int(self.a.minutes * 60 / 6)); intents = plan(self.a.seed * 1000, n)
        table = os.path.join(self.out, 'soak.tsv')
        res, sig = self.run('soak', intents, timeout=int(self.a.minutes * 60 * 2 + 120), env=f'DM_SOAK={table},10')
        rep = subprocess.run([sys.executable, os.path.join(DM, 'ios', 'tools', 'soak_report.py'), table, os.path.join(self.out, 'soak', 'host.log')], capture_output=True, text=True, check=False)
        print(rep.stdout.rstrip())
        ok = not sig and rep.returncode == 0
        print(f"PLAYBOT {'OK' if ok else 'RED'} soak {self.a.minutes} min, {n} intents" + (f'；{sig}' if sig else '') + ('' if rep.returncode == 0 else '；soak criteria red'))
        return dict(name='playbot-soak', seed=self.a.seed, minutes=self.a.minutes, intents=n, verdict='OK' if ok else 'RED', signature=sig, soak_rc=rep.returncode, dir=self.out), 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(prog='playbot.py', description='Playtest bot: seeded player intents on player paths only, invariants between them, shrinking by intent, and a soak mode.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run'); s = sub.add_parser('soak')
    for p in (r, s):
        p.add_argument('--seed', type=int, default=1); p.add_argument('--save', default='warlock'); p.add_argument('--dmhost'); p.add_argument('--window', action='store_true')
    r.add_argument('--episodes', type=int, default=3); r.add_argument('--intents', type=int, default=12); r.add_argument('--no-shrink', action='store_true'); r.add_argument('--budget', type=int, default=12)
    s.add_argument('--minutes', type=float, default=30)
    a = ap.parse_args(argv)
    import scenario   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    held = scenario.game_lock(__file__, argv, f'playbot {a.cmd} ~{int(getattr(a, "minutes", 0) or getattr(a, "episodes", 1) * 2)} min', a.window)   # 开窗跑局先拿 Mac 游戏锁 | windowed runs take the Mac game lock first
    if held is not None: return held
    b = Bot(a)
    report, rc = b.soak() if a.cmd == 'soak' else b.episodes()
    with open(os.path.join(b.out, 'playbot.json'), 'w', encoding='utf-8') as f: json.dump(report, f, ensure_ascii=False, indent=1)
    print('RESULT ' + json.dumps(report, ensure_ascii=False))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
