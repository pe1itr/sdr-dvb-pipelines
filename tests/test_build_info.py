"""Check build identity across commits, dirty worktrees and source-only builds."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildInfoTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'tools').mkdir()
        shutil.copy(ROOT / 'tools/build-info.sh', self.root / 'tools')
        (self.root / 'VERSION').write_text('1.2.3\n')
        (self.root / '.gitignore').write_text('build/\n')
        self.header = self.root / 'build/generated/linrad_build_info.h'

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args],
                                       text=True, stderr=subprocess.PIPE).strip()

    def generate(self):
        subprocess.run(['bash', self.root / 'tools/build-info.sh', self.header], check=True)
        return self.header.read_text()

    def test_git_transitions_and_noop(self):
        self.git('init')
        self.git('config', 'user.name', 'Build test')
        self.git('config', 'user.email', 'build@example.invalid')
        self.git('add', '.')
        self.git('commit', '-m', 'Initial')
        first = self.git('rev-parse', 'HEAD')
        header = self.generate()
        self.assertIn(f'"1.2.3+g{first[:12]}"', header)
        self.assertIn('LINRAD_SOURCE_STATE "clean"', header)
        stamp = self.header.stat().st_mtime_ns
        self.assertEqual(self.generate(), header)
        self.assertEqual(self.header.stat().st_mtime_ns, stamp)
        (self.root / 'new.c').write_text('/* new source */\n')
        self.assertIn('-dirty"', self.generate())
        self.git('add', '.')
        self.assertIn('-dirty"', self.generate())
        self.git('commit', '-m', 'Add source')
        second = self.git('rev-parse', 'HEAD')
        self.assertIn(second, self.generate())
        self.git('checkout', first)
        self.assertIn(first, self.generate())
        (self.root / 'VERSION').write_text('2.0.0-rc.1\n')
        self.assertIn('"2.0.0-rc.1+g', self.generate())
        self.assertIn('-dirty"', self.generate())

    def test_source_without_git(self):
        header = self.generate()
        self.assertIn('LINRAD_APP_VERSION "1.2.3"', header)
        self.assertIn('LINRAD_GIT_COMMIT "unknown"', header)
        self.assertIn('LINRAD_SOURCE_STATE "unknown"', header)

    def test_invalid_version(self):
        (self.root / 'VERSION').write_text('bad"value\n')
        result = subprocess.run(['bash', self.root / 'tools/build-info.sh', self.header],
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.header.exists())


if __name__ == '__main__':
    unittest.main()
