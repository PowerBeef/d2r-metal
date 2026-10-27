"""dmtest.py 的单测：用小段 shell 造各种判定行，核对 result.json、JUnit 与退出码；不跑游戏。 | Unit tests for dmtest.py: small shell snippets print each kind of verdict line, checking result.json, JUnit and the exit code; the game is not run."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

TOOL = Path(__file__).resolve().parent / 'dmtest.py'


class DmtestTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dmtest ')
        self.out = Path(self.temp.name) / 'out'

    def tearDown(self):
        self.temp.cleanup()

    def wrap(self, script, *opts):
        p = subprocess.run([sys.executable, str(TOOL), '--out', str(self.out), *opts, '--', 'sh', '-c', script], capture_output=True, text=True, check=False)
        last = p.stdout.rstrip('\n').splitlines()[-1]
        self.assertTrue(last.startswith('RESULT '), p.stdout)
        res = json.loads((self.out / 'result.json').read_text(encoding='utf-8'))
        self.assertEqual(json.loads(last[7:])['verdict'], res['verdict'])
        return p, res

    def test_ok_run_passes_output_through(self):
        p, res = self.wrap('echo "冒烟开跑"; echo "SMOKE OK 3/3 用时 40 秒"')
        self.assertEqual((p.returncode, res['verdict'], res['rc']), (0, 'ok', 0))
        self.assertIn('冒烟开跑', p.stdout)
        self.assertEqual(res['verdicts'], [dict(name='SMOKE', verdict='OK', detail='3/3 用时 40 秒', line=2)])
        self.assertIn('SMOKE OK 3/3', (self.out / 'log.txt').read_text(encoding='utf-8'))

    def test_verdict_with_case_name(self):
        _, res = self.wrap('echo "TOUCHCHAIN move OK 用时 33 秒"; echo "UB 上屏 0 base 帧 15"')
        self.assertEqual([(v['name'], v['verdict'], v['detail']) for v in res['verdicts']], [('TOUCHCHAIN move', 'OK', '用时 33 秒')])

    def test_red_verdict_with_zero_exit_is_red(self):   # lifecycle.sh 有一处打 RED 后 exit 0 | lifecycle.sh has one path that prints RED and exits 0
        p, res = self.wrap('echo "LIFECYCLE RED"; exit 0')
        self.assertEqual((p.returncode, res['verdict']), (1, 'red'))

    def test_rc_only_trusts_the_exit_code(self):   # 测试套的负向用例会打 RED 判定行，退出码才是结论 | a test suite's negative cases print RED verdict lines; the exit code is the verdict
        p, res = self.wrap('echo "SCENARIO t RED step 1"; exit 0', '--rc-only')
        self.assertEqual((p.returncode, res['verdict'], len(res['verdicts'])), (0, 'ok', 1))
        p, res = self.wrap('echo "ALL OK"; exit 2', '--rc-only')
        self.assertEqual((p.returncode, res['verdict']), (1, 'red'))

    def test_nonzero_exit_is_red_and_markers_counted(self):
        p, res = self.wrap('echo "【红】宿主编译失败"; echo "x 🔴 y"; exit 3')
        self.assertEqual((p.returncode, res['verdict'], res['rc']), (1, 'red', 3))
        self.assertEqual(res['red_lines']['count'], 2)
        self.assertEqual(res['red_lines']['first'][0], dict(line=1, text='【红】宿主编译失败'))
        self.assertEqual(json.loads(p.stdout.splitlines()[-1][7:])['first_red'], '【红】宿主编译失败')

    def test_red_markers_only_fail_with_strict_log(self):
        self.assertEqual(self.wrap('echo "【红】某个可容忍的告警"')[1]['verdict'], 'ok')
        self.assertEqual(self.wrap('echo "【红】某个可容忍的告警"', '--strict-log')[1]['verdict'], 'red')

    def test_result_json_lines_and_unavailable(self):
        script = 'echo \'RESULT {"name": "soak", "verdict": "RED", "checks": []}\'; echo "⚪ 逻辑自对拍（不可用：缺 truth_runs）"; echo "UNIT plan 不可用（缺 x）"; echo "⏭ iOS 构建（跳过：fast）"'
        p, res = self.wrap(script)
        self.assertEqual((p.returncode, res['verdict']), (1, 'red'))
        self.assertEqual(res['results'][0]['name'], 'soak')
        self.assertEqual(len(res['unavailable']), 2)
        self.assertEqual(len(res['skipped']), 1)

    def test_timeout_is_error_and_kills_the_group(self):
        p, res = self.wrap('sleep 30 & sleep 30; echo never', '--timeout', '1')
        self.assertEqual((p.returncode, res['verdict'], res['timed_out']), (2, 'error', True))
        self.assertLess(res['elapsed_s'], 15)

    def test_missing_command_is_error(self):
        p = subprocess.run([sys.executable, str(TOOL), '--out', str(self.out), '--', '/nonexistent/dm-no-such-tool'], capture_output=True, text=True, check=False)
        self.assertEqual(p.returncode, 2)
        self.assertEqual(json.loads((self.out / 'result.json').read_text(encoding='utf-8'))['verdict'], 'error')

    def test_junit(self):
        xml = Path(self.temp.name) / 'j.xml'
        self.wrap('echo "BUILD OK 预构建完成"; echo "IOSBUILD RED 用时 3 秒"; echo "⚪ 某项（不可用：缺 x）"; exit 1', '--junit', str(xml), '--name', 'accept')
        suite = ET.parse(xml).getroot().find('testsuite')
        assert suite is not None
        self.assertEqual((suite.get('name'), suite.get('tests'), suite.get('failures'), suite.get('skipped')), ('accept', '3', '1', '1'))
        failed = [tc.get('name') for tc in suite.iter('testcase') if tc.find('failure') is not None]
        self.assertEqual(failed, ['IOSBUILD'])

    def test_child_sees_result_dir(self):
        _, res = self.wrap('echo "$DMTEST_OUT" > "$DMTEST_OUT/extra.txt"; echo "X-Y OK"')
        self.assertEqual(res['verdict'], 'ok')
        self.assertEqual((self.out / 'extra.txt').read_text(encoding='utf-8').strip(), os.fspath(self.out))


if __name__ == '__main__':
    unittest.main()
