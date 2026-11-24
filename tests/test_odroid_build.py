"""Bash ODROID planning and INI generation without package or radio access."""
import configparser
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_build_helper import BASH, BuildEnvironment, ROOT


class OdroidTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='linrad-odroid-test-')
        self.addCleanup(self.temp.cleanup)
        self.fixture = BuildEnvironment(self.temp.name)
        self.root = Path(self.temp.name) / 'source with spaces'
        (self.root / 'tools').mkdir(parents=True)
        shutil.copy(ROOT / 'linrad-dvb.example.ini', self.root)
        # Substitute only the build command, keeping the real ODROID control flow.
        helper = self.root / 'tools/build.sh'
        helper.write_text('#!/usr/bin/env bash\nexec make "$@"\n')

    def shell(self, body, *args):
        code = ('source "$1"\nPROJECT_ROOT=$2\nshift 2\n' + body)
        return subprocess.run([BASH, '-c', code, 'test', str(ROOT / 'tools/build_odroid_n2.sh'),
                               str(self.root), *args], cwd=self.temp.name, env=self.fixture.env,
                              text=True, capture_output=True, timeout=20)

    def run_main(self, *args):
        return self.shell('odroid_require_platform() { :; }\n'
                          'odroid_install_dependencies() { echo INSTALL; }\n'
                          'odroid_main "$@"', *args)

    def test_wrong_architecture_is_rejected(self):
        result = self.shell('uname() { if [[ $1 == -m ]]; then echo x86_64; else echo Linux; fi; }\n'
                            'odroid_require_platform')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('aarch64', result.stderr)

    def test_check_never_builds_installs_or_creates_config(self):
        result = self.run_main('--check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('INSTALL', result.stdout)
        commands = self.fixture.commands('make')
        self.assertEqual(len(commands), 1)
        self.assertIn('--check', commands[0]['args'])
        self.assertIn('-march=armv8-a', commands[0]['cflags'])
        self.assertFalse((self.root / 'pi6ehv-odroid.ini').exists())

    def test_build_and_explicit_install_create_config(self):
        self.fixture.env['PROTOCOLS'] = 'Output:\n udp'
        result = self.run_main('--install-deps', '-j', '2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('INSTALL', result.stdout)
        commands = self.fixture.commands('make')
        self.assertEqual(len(commands), 2)
        self.assertNotIn('--check', commands[1]['args'])
        self.assertIn('build-odroid-n2', commands[1]['args'])
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(self.root / 'pi6ehv-odroid.ini')
        self.assertEqual(cfg['session']['renderer'], 'embedded')

    def test_dependency_or_runtime_failure_blocks_build_and_config(self):
        for env in ({'MAKE_STATUS': '4'}, {'PROTOCOLS': 'Input:\n srt\nOutput:\n udp'},
                    {'ENCODERS': ' V..... libx264 H264'}, {'FACTORIES': 'hackrf'}):
            with self.subTest(env=env):
                self.fixture.log.unlink(missing_ok=True)
                self.fixture.env.update(env)
                result = self.run_main('--srt')
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(self.fixture.commands('make')), 1)
                self.assertFalse((self.root / 'pi6ehv-odroid.ini').exists())
                for key in env: self.fixture.env.pop(key)

    def test_config_preserves_existing_and_sets_paths_and_tuning(self):
        path = Path(self.temp.name) / 'receiver.ini'
        result = self.shell('odroid_write_config "$1" ffmpeg 1', str(path))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(path)
        self.assertEqual(cfg['paths']['sdr'], str(self.root / 'build-odroid-n2/sdr_to_stdout'))
        self.assertEqual(cfg['paths']['ffmpeg'], str(self.fixture.bin / 'ffmpeg'))
        self.assertEqual(cfg['rtlsdr']['frequency'], '0')
        self.assertEqual(cfg['session']['source'], 'rtlsdr')
        self.assertEqual(cfg['session']['renderer'], 'srt')
        self.assertEqual(cfg['session']['gi'], 'auto')
        self.assertEqual(cfg['srt']['x264_preset'], 'ultrafast')
        self.assertEqual(cfg['srt']['video_threads'], '3')
        self.assertEqual(cfg['srt']['filter_threads'], '1')
        path.write_text('user settings\n')
        result = self.shell('odroid_write_config "$1" ffmpeg 1', str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_text(), 'user settings\n')
        self.assertEqual(list(path.parent.glob('receiver.ini.*')), [])

    def test_conflicting_and_invalid_options_are_rejected(self):
        for args in (('--check', '--install-deps'), ('--check', '--build-ffmpeg'),
                     ('--build-ffmpeg', '--ffmpeg', '/some/ffmpeg'), ('-j', '0'), ('--config',)):
            result = self.run_main(*args)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse(self.fixture.commands('make'))

    def test_local_ffmpeg_pin_and_isolated_prefix(self):
        source = self.root / 'build-odroid-n2/ffmpeg-src'
        source.mkdir(parents=True)
        (source / 'configure').write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
        (source / 'configure').chmod(0o755)
        result = self.shell('git() { echo "$FFMPEG_COMMIT"; }\nodroid_local_ffmpeg 2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('--prefix=' + str(self.root / 'build-odroid-n2/ffmpeg'), result.stdout)
        self.assertIn('--enable-libsrt', result.stdout)
        self.assertIn('--enable-libx264', result.stdout)
        commands = self.fixture.commands('make')
        self.assertEqual([row['args'] for row in commands], [['-j2'], ['install']])
        result = self.shell('git() { echo wrong-revision; }\nodroid_local_ffmpeg 2')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.fixture.commands('make')), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
