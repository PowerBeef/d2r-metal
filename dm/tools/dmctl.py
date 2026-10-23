#!/usr/bin/env python3
# 控制口客户端（dm/verify/control_port.md）：Mac 宿主（DM_CTL_PORT）由宿主连过来，这边监听（listen=True）；iOS 连 App 的日志端口（真机经 iproxy 转到本机）。发命令、收回执、等事件。
#   作为模块：c = Ctl(端口[, listen=True][, log=文件])；c.cmd('state') → 回执字典（ok=false 抛 CtlError）；c.wait_for('loading_done', timeout=60) → 事件字典
#             （先见到 crash / fatal / watchdog 抛 BadEvent）；c.mark() → 当前事件序号，配 wait_for(after=…) 只等之后的；c.close()
#   命令行：dmctl.py [--listen] <端口> <命令> [名=值 …]（--listen = Mac 宿主）        发一条命令，打印回执；ok=false 退出码 1（reply_timeout=秒 改等回执的时限，缺省 60）
#           dmctl.py <端口> wait <事件> [超时秒]    等事件出现（已发生过的也算）；超时 1、坏事件 2
#   连接上收到的每一行原样追加进 log 文件（iOS 日志端口一连上会先补发整份日志）
# Control-port client (dm/verify/control_port.md): the Mac host (DM_CTL_PORT) connects to us, so this side listens (listen=True); for iOS it connects to the App's log port (a device is forwarded to this machine through iproxy). Sends commands, receives replies and waits for events.
#   As a module: c = Ctl(port[, listen=True][, log=file]); c.cmd('state') -> reply dict (raises CtlError when ok=false); c.wait_for('loading_done', timeout=60) -> event dict
#                (raises BadEvent if crash / fatal / watchdog comes first); c.mark() -> the current event seq, with wait_for(after=...) to wait only for later ones; c.close()
#   Command line: dmctl.py [--listen] <port> <cmd> [name=value ...] (--listen = Mac host)   send one command and print the reply; exit code 1 when ok=false (reply_timeout=seconds changes how long to wait for the reply, default 60)
#                 dmctl.py <port> wait <event> [timeout s]   wait for the event (one that already happened counts); 1 on timeout, 2 on a bad event
#   Every line received on the connection is appended as is to the log file (the iOS log port replays the whole log first when connected)
import json
import socket
import sys
import threading
import time

BAD = ('crash', 'fatal', 'watchdog')


class CtlError(RuntimeError):
    def __init__(self, reply):
        super().__init__(f"{reply.get('cmd')}: {reply.get('err')}")
        self.reply = reply


class BadEvent(RuntimeError):
    def __init__(self, ev):
        super().__init__(json.dumps(ev, ensure_ascii=False))
        self.event = ev


def bind(port=0, host='127.0.0.1'):
    """先占一个监听口（port=0 由系统挑），返回 (socket, 端口)：起宿主之前调，再 Ctl(server=socket)。 | Take a listening socket first (port=0 lets the system pick) and return (socket, port): call before starting the host, then Ctl(server=socket)."""
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind((host, port)); srv.listen(1)
    return srv, srv.getsockname()[1]


class Ctl:
    def __init__(self, port=0, host='127.0.0.1', log=None, connect_timeout=30.0, listen=False, server=None):
        end = time.time() + connect_timeout
        if listen or server:   # Mac 宿主往这边连（它每 0.5 秒重试一次，谁先起都行）；server = 先用 bind() 占好的监听口 | the Mac host connects here (it retries every 0.5 s, so either side may start first); server = a listening socket taken earlier with bind()
            srv = server or bind(port, host)[0]; srv.settimeout(connect_timeout)
            try: self.sock, _ = srv.accept()
            finally: srv.close()
        else:
            while True:
                try: self.sock = socket.create_connection((host, port), timeout=2); break
                except OSError:
                    if time.time() > end: raise
                    time.sleep(0.2)
        self.sock.settimeout(None)
        self.log = open(log, 'ab') if log else None
        self.cv = threading.Condition()
        self.replies, self.events, self.closed, self.next_id = {}, [], False, 1
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b''
        while True:
            try: d = self.sock.recv(65536)
            except OSError: d = b''
            if not d: break
            buf += d
            *lines, buf = buf.split(b'\n')
            for raw in lines:
                if self.log: self.log.write(raw + b'\n')
                s = raw.decode('utf-8', 'replace')
                i = s.find('[dm] RE {'); j = s.find('[dm] EV {')
                try: obj = json.loads(s[i + 8:] if i >= 0 else s[j + 8:]) if i >= 0 or j >= 0 else None
                except ValueError: obj = None
                if obj is None: continue
                with self.cv:
                    if i >= 0: self.replies[obj.get('id')] = obj
                    else: self.events.append(obj)
                    self.cv.notify_all()
            if self.log: self.log.flush()
        with self.cv: self.closed = True; self.cv.notify_all()

    def send_raw(self, line):
        self.sock.sendall(line.encode() + b'\n')

    def cmd(self, name, reply_timeout=30.0, check=True, **args):   # 命令自己的参数可以叫 timeout（walk_to / save_now） | the command's own arguments may be called timeout (walk_to / save_now)
        with self.cv: rid = self.next_id; self.next_id += 1
        self.send_raw(json.dumps(dict(id=rid, cmd=name, **args), ensure_ascii=False))
        end = time.time() + reply_timeout
        with self.cv:
            while rid not in self.replies:
                left = end - time.time()
                if left <= 0 or self.closed: raise TimeoutError(f'{name}: no reply' + (' (connection closed)' if self.closed else ''))
                self.cv.wait(left)
            r = self.replies.pop(rid)
        if check and not r.get('ok'): raise CtlError(r)
        return r

    def mark(self):
        with self.cv: return self.events[-1]['seq'] if self.events else -1

    def wait_for(self, ev, timeout=60.0, after=-1, **match):
        end = time.time() + timeout
        with self.cv:
            k = 0
            while True:
                for e in self.events[k:]:
                    if e.get('seq', 0) <= after: continue
                    if e['ev'] in BAD and ev not in BAD: raise BadEvent(e)
                    if e['ev'] == ev and all(e.get(f) == v for f, v in match.items()): return e
                k = len(self.events)
                left = end - time.time()
                if left <= 0 or self.closed: raise TimeoutError(f'{ev}: not seen' + (' (connection closed)' if self.closed else ''))
                self.cv.wait(left)

    def close(self):
        try: self.sock.close()
        except OSError: pass
        if self.log: self.log.close()


def _value(v):
    if v in ('true', 'false'): return v == 'true'
    try: return int(v)
    except ValueError: pass
    try: return float(v)
    except ValueError: return v


def main(argv):
    listen = argv[:1] == ['--listen']
    if listen: argv = argv[1:]
    if len(argv) < 2: sys.exit('用法 | usage: dmctl.py [--listen] <port> <cmd> [name=value ...] | dmctl.py [--listen] <port> wait <event> [timeout]')
    c = Ctl(int(argv[0]), listen=listen)
    try:
        if argv[1] == 'wait':
            try: print(json.dumps(c.wait_for(argv[2], float(argv[3]) if len(argv) > 3 else 60), ensure_ascii=False)); return 0
            except BadEvent as e: print(e); return 2
            except TimeoutError as e: print(e); return 1
        args = dict(a.split('=', 1) for a in argv[2:])
        r = c.cmd(argv[1], check=False, reply_timeout=float(args.pop('reply_timeout', 60)), **{k: _value(v) for k, v in args.items()})
        print(json.dumps(r, ensure_ascii=False))
        return 0 if r.get('ok') else 1
    finally:
        c.close()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
