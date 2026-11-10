#!/usr/bin/env python3
# Feature map: which of the game's code runs only during which action, found by the live hooks' block coverage (dm_live.c) instead of by reading.
# A plan lists actions as scenario steps; a headless game is driven through them one by one. Before each action the blocks that already ran are
# trapped again, after it the blocks that ran are dumped; what an action ran less what runs anyway (the idle windows) is that action's own code.
#   run <plan> [--name N] [--idle S]   drive the plan into a run of the run store ($DM_ROOT/re/live/runs), then report
#   report [run] [--top N]             per action: the functions that ran only then (new = no block of theirs runs while idle), most blocks first
#   merge [run]                        add the run's rows to $DM_ROOT/re/live/features.tsv: what the dm-live `lookup` tool shows and `re.sh ingest`
#                                      writes into the analysis project as comments (a tag is evidence of when code runs, never a name)
# Plan (text, # comments):   save <name>   |   env K=V …   |   setup + its steps (run once, before coverage)   |   action <label> + its steps
#   Labels group by a prefix: ui/inventory, ui/skills, move/walk, area/a2_sewers. An action labelled idle… is a baseline window, like the first
#   one (--idle seconds, default 10): its blocks count as running anyway. A function that ran in many actions of several groups is tagged
#   `shared: area x13, combat x2`. End an action with its own `sleep` so what it starts can finish.
# The run folder and features.tsv name game functions: they stay under $DM_ROOT/re. Needs the snapshot (re.sh live-index) and $DM_ROOT/re/venv
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
import guestsyms  # noqa: E402
import live  # noqa: E402

MERGED = os.path.join(live.LIVE, 'features.tsv')


def parse_plan(path):
    """→ (save, env text, setup steps, [(label, steps)])"""
    save, env, setup, actions, cur = 'warlock', '', [], [], None
    with open(path, encoding='utf-8') as f:
        for n, raw in enumerate(f, 1):
            line = raw.split('#', 1)[0].strip() if raw.lstrip().startswith('#') else raw.strip()
            if not line: continue
            w = line.split(None, 1)
            if w[0] == 'save' and cur is None: save = w[1]
            elif w[0] == 'env' and cur is None: env = (env + ' ' + w[1]).strip()
            elif line == 'setup': cur = setup
            elif w[0] == 'action':
                if len(w) < 2: raise SystemExit(f'{path}:{n}: action needs a label')
                actions.append((w[1].strip(), [])); cur = actions[-1][1]
            elif cur is None: raise SystemExit(f'{path}:{n}: a step before setup or action')
            else: cur.append(line)
    if not actions: raise SystemExit(f'{path}: no action')
    return save, env, setup, actions


def tag_of(acts):
    """One function's tag from the actions it ran in: the action, the group they share (ui/*), up to three joined, or the groups with how many
    actions of each (shared: area x13, combat x2)."""
    if len(acts) == 1: return acts[0]
    groups = collections.Counter(a.split('/')[0] for a in acts)
    if len(groups) == 1 and all('/' in a for a in acts): return f'{next(iter(groups))}/* ({len(acts)})'
    return '+'.join(acts) if len(acts) <= 3 else 'shared: ' + ', '.join(f'{g} x{n}' for g, n in groups.most_common())


def features(d):
    """Read a run's coverage files (cov_NN_<label>.tsv, in order) → (actions, baseline blocks, {func: {action: blocks}}, {func: baseline blocks})."""
    files = sorted(f for f in os.listdir(d) if f.startswith('cov_') and f.endswith('.tsv'))
    if not files: raise SystemExit(f'no coverage files in {d} (featuremap.py run <plan>)')
    label = lambda f: f[len('cov_00_'):-4].replace('~', '/')   # noqa: E731
    base, acts = set(), []
    for f in files:
        if label(f).startswith('idle'): base |= live.cov_read(os.path.join(d, f))
        else: acts.append((label(f), f))
    per = collections.defaultdict(dict)
    for lab, f in acts:
        for fn, n in live.by_function(live.cov_read(os.path.join(d, f)) - base).items(): per[fn][lab] = per[fn].get(lab, 0) + n
    return [a for a, _ in acts], base, per, live.by_function(base)


def write_features(d):
    acts, base, per, base_fn = features(d); order = list(dict.fromkeys(acts)); rows = []
    for fn in sorted(per, key=lambda f: -sum(per[f].values())):
        ran = [a for a in order if per[fn].get(a)]
        rows.append((fn, (guestsyms.lookup(f'main+{fn:#x}') or '').split('+0x0')[0], tag_of(ran), 0 if base_fn.get(fn) else 1, [per[fn].get(a, 0) for a in order]))
    with open(os.path.join(d, 'features.tsv'), 'w', encoding='utf-8') as f:
        f.write('func\tname\ttag\tnew\t' + '\t'.join(order) + '\n')
        f.writelines(f'main+{fn:#x}\t{nm}\t{tag}\t{new}\t' + '\t'.join(map(str, c)) + '\n' for fn, nm, tag, new, c in rows)
    return order, base, rows


def report(d, top=12):
    order, base, rows = write_features(d)
    print(f'{len(base)} blocks run while idle; {len(rows)} functions ran something only during an action ({sum(1 for r in rows if r[3])} of them never run while idle) → {os.path.join(d, "features.tsv")}')
    for i, a in enumerate(order):
        mine = [r for r in rows if r[4][i]]; only = [r for r in mine if r[2] == a]
        print(f'{a}: {sum(r[4][i] for r in mine)} blocks in {len(mine)} functions; {len(only)} functions ran in this action alone, {sum(1 for r in only if r[3])} of them new')
        for fn, nm, _, new, c in sorted(only, key=lambda r: (-r[3], -r[4][i]))[:top]: print(f"    {c[i]:5d} blocks  main+{fn:#x} {nm}{'' if new else '  (a branch of code that runs anyway)'}")
    return rows


def merge(d):
    """The run's rows into the store's features.tsv (func, tag, new, actions, run, product), replacing this run's earlier rows."""
    import json
    order, _, rows = write_features(d); rid = os.path.basename(d); prod = '?'
    if os.path.exists(os.path.join(d, 'run.json')):
        with open(os.path.join(d, 'run.json'), encoding='utf-8') as f: prod = json.load(f).get('product', '?')
    keep = []
    if os.path.exists(MERGED):
        with open(MERGED, encoding='utf-8') as f: keep = [ln for ln in f.read().split('\n')[1:] if ln and ln.split('\t')[4] != rid]
    new = [f'main+{fn:#x}\t{tag}\t{nw}\t' + ' '.join(f'{a}:{n}' for a, n in zip(order, c) if n) + f'\t{rid}\t{prod}' for fn, _, tag, nw, c in rows]
    with open(MERGED, 'w', encoding='utf-8') as f: f.write('func\ttag\tnew\tactions\trun\tproduct\n' + '\n'.join(keep + new) + '\n')
    print(f'MERGED {len(new)} functions of {rid} → {MERGED} ({len(keep)} rows of other runs kept)')


def lookup(off):
    """The merged tags of one function: [(tag, new, actions, run)], latest run last."""
    out = []
    if os.path.exists(MERGED):
        with open(MERGED, encoding='utf-8') as f:
            for ln in f:
                p = ln.rstrip('\n').split('\t')
                if p[0] == f'main+{off:#x}': out.append((p[1], p[2] == '1', p[3], p[4]))
    return out


def run(plan, name=None, idle=10.0):
    import live_mcp   # noqa: PLC0415 -- needs the re venv's packages only for the MCP server itself; the Session class is plain
    save, env, setup, actions = parse_plan(plan)
    S = live_mcp.Session(); S.start(save, None, env, False, name or 'featuremap_' + os.path.splitext(os.path.basename(plan))[0]); d = S.dir
    failed = []
    try:
        for st in setup: S.step(st)
        bf = os.path.join(d, 'blocks.txt'); live.blocks(bf)
        r = S.step(f'live do=cover file={bf}'); print(f'coverage armed: {r}')
        k = 0
        def window(label, steps):
            nonlocal k
            S.step('live do=cover_rearm')
            for st in steps:
                try: S.step(st)
                except Exception as e:   # noqa: BLE001 -- a failed step ends this action, not the map
                    failed.append((label, st, str(e)[:200])); print(f'  ✗ {label}: {st}: {str(e)[:160]}'); break
            got = S.step(f"live do=cover_dump file=cov_{k:02d}_{label.replace('/', '~')}.tsv"); k += 1
            print(f"  {label}: {got.get('ran') if isinstance(got, dict) else got} blocks ran")
        window('idle_start', [f'sleep {idle}'])
        for label, steps in actions: window(label, steps)
        window('idle_end', [f'sleep {idle}'])
        S.step('live do=cover_off')
    finally:
        rid = S.stop()
    with open(plan, encoding='utf-8') as f: text = f.read()
    live.run_meta(d, scenario=os.path.abspath(plan), scenario_text=text, verdict=f'feature map, {len(actions)} actions, {len(failed)} failed')
    print(f'RUN {rid} → {d}')
    for label, st, e in failed: print(f'failed: {label}: {st}: {e}')
    report(d)
    return 1 if failed else 0


def main(argv):
    ap = argparse.ArgumentParser(prog='featuremap.py', description='Map the game\'s code by feature from block coverage of scripted actions.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run'); r.add_argument('plan'); r.add_argument('--name'); r.add_argument('--idle', type=float, default=10.0)
    q = sub.add_parser('report'); q.add_argument('run', nargs='?'); q.add_argument('--top', type=int, default=12)
    m = sub.add_parser('merge'); m.add_argument('run', nargs='?')
    a = ap.parse_args(argv)
    if a.cmd == 'run': return run(a.plan, a.name, a.idle)
    if a.cmd == 'report': report(live.resolve_run(a.run), a.top); return 0
    merge(live.resolve_run(a.run)); return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
