"""控制口 Mac 传输（dm_ctl.c，DM_CTL_PORT）的单测：t_ctl --serve 当宿主往这边连，这边用 dmctl.py 的监听模式当控制端；回执、推送的事件、断开后重连。 | Unit test of the control port's Mac transport (dm_ctl.c, DM_CTL_PORT): t_ctl --serve plays the host and connects here, and this side is the controller using dmctl.py's listen mode; replies, pushed events, reconnecting after a drop."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / '..' / '..' / 'tools'))
import dmctl  # noqa: E402


class CtlTransportTest(unittest.TestCase):
    def test_connect_reply_event_reconnect(self):
        probe = socket.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
        host = subprocess.Popen([str(HERE / 'build' / 't_ctl'), '--serve'], env=dict(os.environ, DM_CTL_PORT=str(port)), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            c = dmctl.Ctl(port, listen=True, connect_timeout=10)   # 宿主先起、控制端后听：宿主每 0.5 秒重连 | host first, controller listening later: the host retries every 0.5 s
            self.assertEqual(c.wait_for('ctl', timeout=5)['state'], 'connected')
            self.assertEqual(c.wait_for('start', timeout=5)['v'], 1)   # 连上之前的事件也补发了 | events from before the connection are replayed too
            self.assertTrue(c.cmd('ping')['pong'])
            self.assertEqual(c.cmd('echo', v='中文"q', n=2)['v'], '中文"q')
            with self.assertRaises(dmctl.CtlError): c.cmd('echo')
            m = c.mark(); c.cmd('emit', k=7)
            self.assertEqual(c.wait_for('hello', after=m, timeout=3)['k'], 7)
            c.close()
            c = dmctl.Ctl(port, listen=True, connect_timeout=10)   # 控制端断开后再听：宿主自己重连 | the controller listens again after dropping: the host reconnects by itself
            self.assertTrue(c.cmd('ping')['pong'])
            self.assertEqual(c.wait_for('hello', timeout=3)['k'], 7)   # 重连后历史照样补发 | the history is replayed again after reconnecting
            c.close()
        finally:
            host.kill(); host.wait()


if __name__ == '__main__':
    unittest.main()
