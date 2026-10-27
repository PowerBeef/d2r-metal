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
print('OK' if not bad else f'RED {bad}')
sys.exit(1 if bad else 0)
