"""switches.py 的单测：在临时 git 仓库里放几种语言的读 / 设写法，核对登记表与「设了没人读」的检查。 | Unit tests for switches.py: put read / set forms of several languages into a temporary git repository and check the registry and the "set but never read" check."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parent / 'switches.py'
FILES = {
    'host/a.c': 'const char *e = getenv("DM_A"); static const char *tab[] = {"DM_TABLE"};\n',
    'ios/project.yml': 'DEVELOPMENT_TEAM: ${DM_TEAM}\nCURRENT_PROJECT_VERSION: $(DM_BUILD)\n',
    'tools/r.py': "import os\nx = os.environ.get('DM_PY')\ny = os.environ['DM_PY2']\nenv = dict(os.environ, DM_A='1', DM_PYTYPO='1')\nsubprocess.run(['open', '--env', 'DM_TABLE=1'])\ncfg = {'DM_PY': '1'}\n",
    'tools/s.sh': '# DM_COMMENT=1 只是注释 | only a comment\nDM_A=1 ./x\nexport DM_TEAM=t DM_SHREAD=1\necho "${DM_SHREAD:-0}" $DM_OTHER  # DM_TRAIL=1\nenv -u DEVICECTL_CHILD_DM_GONE ./y\nDEVICECTL_CHILD_DM_TYPO=1 xcrun devicectl\nxcodebuild DM_BUILD=3\n',
    'ios/Sources/A.m': 'const char *v = getenv("DM_OBJC");\n',
    'docs.md': 'Set `DM_A` to test.\n',
}


class SwitchesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='switches ')
        self.root = Path(self.temp.name)
        for f, t in FILES.items():
            (self.root / f).parent.mkdir(parents=True, exist_ok=True); (self.root / f).write_text(t, encoding='utf-8')
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True)

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, cmd):
        p = subprocess.run([sys.executable, str(TOOL), cmd], capture_output=True, text=True, env=dict(os.environ, SWITCHES_ROOT=str(self.root)), check=False)
        return p.returncode, p.stdout

    def test_registry_and_unread_sets(self):
        self.assertEqual(self.run_tool('gen')[0], 0)
        rows = {r.split('\t')[0]: r.split('\t') for r in (self.root / 'switches.tsv').read_text(encoding='utf-8').splitlines()[1:]}
        self.assertEqual(sorted(rows), ['DM_A', 'DM_BUILD', 'DM_OBJC', 'DM_OTHER', 'DM_PY', 'DM_PY2', 'DM_SHREAD', 'DM_TABLE', 'DM_TEAM'])
        self.assertEqual((rows['DM_A'][1], rows['DM_A'][3], rows['DM_OBJC'][1], rows['DM_TEAM'][3]), ('host', 'y', 'ios', '-'))
        rc, out = self.run_tool('check')
        self.assertEqual(rc, 1)
        unread = sorted(line.split()[0] for line in out.splitlines() if '没有地方读' in line)
        self.assertEqual(unread, ['DM_GONE', 'DM_PYTYPO', 'DM_TYPO'])   # 注释里的 DM_COMMENT / DM_TRAIL 不算 | DM_COMMENT / DM_TRAIL in comments do not count
        self.assertIn('dm/tools/s.sh:6', out)

    def test_drift_is_red(self):
        self.run_tool('gen')
        (self.root / 'host/b.c').write_text('getenv("DM_NEW");\n', encoding='utf-8')
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True)
        rc, out = self.run_tool('check')
        self.assertEqual(rc, 1); self.assertIn('switches.py gen', out)

    def test_new_file_counts_before_git_add(self):   # the gate runs before the commit: a switch read in a file not yet added is drift too
        self.run_tool('gen')
        (self.root / 'tools/new.py').write_text('import os\nos.environ.get("DM_UNADDED")\n', encoding='utf-8')
        rc, out = self.run_tool('check')
        self.assertEqual(rc, 1); self.assertIn('switches.py gen', out)
        self.run_tool('gen')
        self.assertIn('DM_UNADDED', (self.root / 'switches.tsv').read_text(encoding='utf-8'))


    def test_scenario_env_lines_are_sets(self):   # a scenario's env line sets switches: one nothing reads is reported with its file and line
        (self.root / 'verify').mkdir(exist_ok=True)
        (self.root / 'verify/x.scn').write_text('env DM_NOT_READ=1 DM_A=2 "DM_QUOTED=a b"  # DM_IN_COMMENT=1\nlog env DM_NOT_ENV=1\nenv SCN_X=1\n', encoding='utf-8')
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True)
        sys.path.insert(0, str(TOOL.parent)); import switches   # noqa: PLC0415
        _, sets, _ = switches.scan(str(self.root))
        self.assertEqual(sets['DM_NOT_READ'], [('verify/x.scn', 1)]); self.assertEqual(sets['DM_QUOTED'], [('verify/x.scn', 1)])
        self.assertNotIn('DM_IN_COMMENT', sets); self.assertNotIn('DM_NOT_ENV', sets)
        self.run_tool('gen'); rc, out = self.run_tool('check')
        unread = sorted(line.split()[0] for line in out.splitlines() if 'never read' in line)
        self.assertIn('DM_NOT_READ', unread); self.assertIn('DM_QUOTED', unread); self.assertNotIn('DM_A', unread)
        self.assertIn('dm/verify/x.scn:1', out)


if __name__ == '__main__':
    unittest.main()
