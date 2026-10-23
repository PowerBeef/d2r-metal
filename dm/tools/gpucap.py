#!/usr/bin/env python3
# GPU 截帧与系统 Metal 读数（Xcode 27 命令行工具 gpucapture / gpudebug / metalperftrace 的包装）：
#   capture --pid P [--frames 1] [--out x.gputrace] [--all-resources]   截 P 的 N 帧（按 CAMetalLayer 边界 = 一次上屏；缺省不录没用到的资源，免得一帧几 GB）
#                                                                         进程要带 MTL_CAPTURE_ENABLED=1 起（场景的 gpu_capture 步骤会自动加）
#   summary <x.gputrace>                                                  gpudebug --json：命令缓冲 / 编码器 / 绘制 / 计算派发数，各命令缓冲的编码器数
#   listen --pid P [--seconds 10] [--out f.json]                          metalperftrace listen：每秒的上屏间隔、迟到、跳帧，出汇总
#   hud --pid P on|off                                                    运行中开关系统的 Metal 性能 HUD（metalperftrace setup）
#   末行 RESULT {json}
# GPU frame capture and system Metal readings (wrapping the Xcode 27 command-line tools gpucapture / gpudebug / metalperftrace):
#   capture --pid P [--frames 1] [--out x.gputrace] [--all-resources]   capture N frames of P (at the CAMetalLayer boundary = one present; by default unused resources are not recorded, otherwise one frame takes several GB)
#                                                                         the process must be started with MTL_CAPTURE_ENABLED=1 (the scenarios' gpu_capture step adds it automatically)
#   summary <x.gputrace>                                                  gpudebug --json: command buffers / encoders / draws / compute dispatches, encoders per command buffer
#   listen --pid P [--seconds 10] [--out f.json]                          metalperftrace listen: per-second present intervals, lateness, skipped frames, with a summary
#   hud --pid P on|off                                                    turn the system's Metal performance HUD on / off in a running process (metalperftrace setup)
#   Last line RESULT {json}
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from typing import Any


def run(cmd, timeout, stdin=None):
    try: p = subprocess.run(cmd, input=stdin, capture_output=True, text=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as e: return 1, '', str(e)
    return p.returncode, p.stdout, p.stderr


def layer_boundary(pid):
    """gpucapture boundaries 里 Layer 那一行的 ID（一次迭代 = 一次上屏）；没有就 0（缺省边界）。 | The ID of the Layer row in gpucapture boundaries (one iteration = one present); 0 (the default boundary) when there is none."""
    _, out, _ = run(['gpucapture', 'boundaries', '-p', str(pid)], 30)
    for line in out.splitlines():
        m = re.match(r'\s*(\d+)\s+Layer\b', line)
        if m: return int(m.group(1))
    return 0


MIN_FREE_GB = 15   # 一帧约 4 GB（这台 M6 上局内一帧 4.2 GB）：剩余空间不够就不截 | about 4 GB per frame (4.2 GB for one in-game frame on this M6): no capture when free space is short


def capture(pid, frames, out, all_resources) -> dict[str, Any]:
    st = os.statvfs(os.path.dirname(os.path.abspath(out)) or '.'); free_gb = st.f_bavail * st.f_frsize / 1e9
    if free_gb < MIN_FREE_GB + 4.5 * frames: return dict(ok=False, error=f'only {free_gb:.0f} GB free; a frame takes about 4 GB (need {MIN_FREE_GB + 4.5 * frames:.0f} GB)')
    b = layer_boundary(pid)
    cmd = ['gpucapture', 'start', '-p', str(pid), '-b', str(b), '-c', str(frames), '-o', out] + ([] if all_resources else ['-du'])
    rc, so, se = run(cmd, 120 + 10 * frames)
    ok = rc == 0 and os.path.exists(out)
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(out) for f in fs) if ok else 0
    return dict(ok=ok, path=out, boundary=b, frames=frames, bytes=size, error='' if ok else (se or so).strip()[-300:])


def gpudebug(trace, commands, timeout=300):
    rc, out, err = run(['gpudebug', '-t', trace, '--oneshot', '-q', '--json'], timeout, stdin='\n'.join(commands) + '\n')
    replies = []
    for line in out.splitlines():
        i = line.find('{')
        if i < 0: continue
        try: replies.append(json.loads(line[i:]))
        except ValueError: pass
    return rc, replies, err


def summary(trace):
    rc, rep, err = gpudebug(trace, ['status', 'go commands', 'list'])
    st = next((r for r in rep if 'summary' in r), {})
    m = re.match(r'(\d+) command buffers, (\d+) encoders, (\d+) draw calls, (\d+) dispatches', st.get('summary', ''))
    cbs = []
    for r in rep:
        for ch in r.get('children', []):
            if re.fullmatch(r'cb\d+', ch.get('name', '')):
                vals = [v.get('value', '') for v in ch.get('values', [])]
                enc = next((int(v.split()[0]) for v in vals if v.endswith('encoders')), None)
                cbs.append(dict(name=ch['name'], encoders=enc, label=next((v.strip('"') for v in vals if not v.endswith('encoders')), '')))
    if not m: return dict(ok=False, error=(err or st.get('summary', '') or f'gpudebug rc {rc}')[-300:])
    cb, enc, draws, disp = map(int, m.groups())
    return dict(ok=True, device=st.get('device'), command_buffers=cb, encoders=enc, draws=draws, dispatches=disp, per_command_buffer=cbs)


class Listener:
    """metalperftrace listen 在后台跑，stop() 时收尾并出汇总（基准测试每遍开一个）。 | metalperftrace listen running in the background; stop() wraps up and summarizes (the benchmark opens one per run)."""
    def __init__(self, pid):
        self.p = subprocess.Popen(['metalperftrace', 'listen', '--json', '--pid', str(pid), '-i', '1'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def stop(self, out=None):
        self.p.terminate()
        try: so, _ = self.p.communicate(timeout=5)
        except subprocess.TimeoutExpired: self.p.kill(); so, _ = self.p.communicate()
        return parse_listen(so or '', out)


def parse_listen(text, out=None):
    docs, dec, i = [], json.JSONDecoder(), 0   # 输出是一个接一个的多行 JSON 对象 | the output is one multi-line JSON object after another
    while True:
        j = text.find('{', i)
        if j < 0: break
        try: obj, i = dec.raw_decode(text, j)
        except ValueError: i = j + 1; continue
        docs.append(obj)
    if out:
        with open(out, 'w', encoding='utf-8') as f: json.dump(docs, f, indent=1)
    fog, late, skipped = [], [], 0
    for d in docs:
        for layer in d.get('Layers', []):
            ps = layer.get('Performance Stats', {})
            f = ps.get('Frame-On-Glass Interval Stats', {})
            if f.get('Count'): fog.append((f.get('Average (ms)', 0), f.get('Max (ms)', 0), f.get('Count', 0)))
            lt = ps.get('On-Glass Present Lateness Stats', {})
            if lt.get('Count'): late.append(lt.get('Max (ms)', 0))
            sk = ps.get('Skipped Frame Stats', {}).get('Count')
            skipped += int(sk) if isinstance(sk, (int, float)) else 0
    if not fog: return dict(ok=False, samples=len(docs), error='no frame-on-glass samples (process gone, or not presenting)')
    return dict(ok=True, samples=len(docs), frames=sum(c for _, _, c in fog), on_glass_avg_ms=round(statistics.fmean(a for a, _, _ in fog), 4),
                on_glass_max_ms=round(max(m for _, m, _ in fog), 3), late_max_ms=round(max(late), 3) if late else 0.0, skipped=skipped)


def listen(pid, seconds, out):
    import time   # noqa: PLC0415
    lst = Listener(pid); time.sleep(seconds)
    return lst.stop(out)


def main(argv):
    ap = argparse.ArgumentParser(prog='gpucap.py', description='GPU capture, capture summary, live Metal stats and HUD (gpucapture / gpudebug / metalperftrace).')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture'); c.add_argument('--pid', type=int, required=True); c.add_argument('--frames', type=int, default=1); c.add_argument('--out', default='frame.gputrace'); c.add_argument('--all-resources', action='store_true')
    s = sub.add_parser('summary'); s.add_argument('trace')
    li = sub.add_parser('listen'); li.add_argument('--pid', type=int, required=True); li.add_argument('--seconds', type=float, default=10); li.add_argument('--out')
    h = sub.add_parser('hud'); h.add_argument('--pid', type=int, required=True); h.add_argument('state', choices=('on', 'off'))
    a = ap.parse_args(argv)
    if a.cmd == 'capture': res = capture(a.pid, a.frames, a.out, a.all_resources)
    elif a.cmd == 'summary': res = summary(a.trace)
    elif a.cmd == 'listen': res = listen(a.pid, a.seconds, a.out)
    else:
        rc, so, se = run(['metalperftrace', 'setup', '--enable' if a.state == 'on' else '--disable', 'hud', '--pid', str(a.pid)], 30)
        res = dict(ok=rc == 0, hud=a.state, error='' if rc == 0 else (se or so).strip()[-200:])
    print(json.dumps(res, ensure_ascii=False, indent=1))
    print('RESULT ' + json.dumps(dict(name=f'gpucap-{a.cmd}', verdict='OK' if res.get('ok') else 'RED', **{k: v for k, v in res.items() if k not in ('per_command_buffer',)}), ensure_ascii=False))
    return 0 if res.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
