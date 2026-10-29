"""dmctl.py 的单测：本机起一个假的控制口（照协议回「[dm] RE」、推「[dm] EV」、夹着普通日志行），不跑游戏。 | Unit tests for dmctl.py: a fake control port on this machine (answers "[dm] RE", pushes "[dm] EV", with plain log lines mixed in); the game is not run."""
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dmctl  # noqa: E402


class FakePort:
    def __init__(self, backlog=b''):
        self.srv = socket.socket(); self.srv.bind(('127.0.0.1', 0)); self.srv.listen(1)
        self.port = self.srv.getsockname()[1]; self.backlog = backlog; self.got = []
        threading.Thread(target=self.run, daemon=True).start()

    def send_ev(self, seq, ev, **kw):
        self.conn.sendall(b'[dm] EV ' + json.dumps(dict(seq=seq, t=seq / 10, ev=ev, **kw)).encode() + b'\n')

    def run(self):
        self.conn, _ = self.srv.accept()
        self.conn.sendall(self.backlog)   # iOS 日志端口一连上先补发旧日志 | the iOS log port replays the old log first
        buf = b''
        while True:
            d = self.conn.recv(4096)
            if not d: return
            buf += d
            *lines, buf = buf.split(b'\n')
            for line in lines:
                req = json.loads(line); self.got.append(req)
                self.conn.sendall(b'[dm] some log line\n')
                if req['cmd'] == 'later':   # 先推事件、过一会儿才回执 | push events first, reply a little later
                    self.send_ev(10, 'panel', act='OpenPanel', name='X'); self.send_ev(11, 'pause_open'); time.sleep(0.2)
                if req['cmd'] == 'boom': self.send_ev(20, 'crash', sig=11)
                ok = req['cmd'] != 'bad'
                rep = dict(id=req['id'], cmd=req['cmd'], ok=ok, **({'echo': req.get('x')} if ok else {'err': 'unknown cmd'}))
                self.conn.sendall(b'[dm] RE ' + json.dumps(rep).encode() + b'\n')


class DmctlTest(unittest.TestCase):
    def test_commands_events_and_log(self):
        backlog = b'boot: start\n[dm] EV {"seq":1,"t":0.1,"ev":"main_menu"}\n'
        f = FakePort(backlog)
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'port.log'
            c = dmctl.Ctl(f.port, log=str(log))
            self.assertEqual(c.wait_for('main_menu', timeout=3)['seq'], 1)   # 补发里的事件也算 | events from the replay count
            r = c.cmd('state', x=5)
            self.assertEqual((r['ok'], r['echo'], f.got[0]['x']), (True, 5, 5))
            with self.assertRaises(dmctl.CtlError): c.cmd('bad')
            self.assertFalse(c.cmd('bad', check=False)['ok'])
            m = c.mark()
            c.cmd('later')
            self.assertEqual(c.wait_for('panel', after=m, name='X')['seq'], 10)
            with self.assertRaises(TimeoutError): c.wait_for('main_menu', after=m, timeout=0.3)   # 只等之后的 | only later ones
            c.cmd('boom')
            self.assertEqual(c.wait_for('crash', timeout=1)['sig'], 11)   # waiting for the bad event itself returns normally
            m = c.mark()   # the crash is already read in when the next step takes its mark
            with self.assertRaises(dmctl.BadEvent): c.wait_for('loading_done', after=m, timeout=2)
            with self.assertRaises(TimeoutError): c.wait_for('loading_done', timeout=0.3)   # each bad event fails one wait
            c.close()
            text = log.read_text(encoding='utf-8')
            self.assertIn('boot: start', text); self.assertIn('[dm] RE {', text)

    def test_listen_mode(self):   # Mac 宿主往这边连：先起「宿主」也行 | the Mac host connects here: starting the "host" first is fine too
        probe = socket.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
        def host():
            for _ in range(50):
                try: s = socket.create_connection(('127.0.0.1', port)); break
                except OSError: time.sleep(0.1)
            else: return
            buf = b''
            while b'\n' not in buf: buf += s.recv(100)
            rid = json.loads(buf)['id']; s.sendall(b'[dm] RE {"id":%d,"cmd":"ping","ok":true,"pong":true}\n' % rid); time.sleep(0.2); s.close()
        threading.Thread(target=host, daemon=True).start()
        time.sleep(0.3)
        c = dmctl.Ctl(port, listen=True, connect_timeout=5)
        self.assertTrue(c.cmd('ping')['pong']); c.close()

    def test_reply_timeout_when_closed(self):
        srv = socket.socket(); srv.bind(('127.0.0.1', 0)); srv.listen(1); port = srv.getsockname()[1]
        def accept_and_close(): srv.accept()[0].close()
        threading.Thread(target=accept_and_close, daemon=True).start()
        c = dmctl.Ctl(port)
        time.sleep(0.2)
        with self.assertRaises((TimeoutError, OSError)): c.cmd('state', reply_timeout=1)


if __name__ == '__main__':
    unittest.main()
