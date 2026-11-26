"""Exercise the Bash build CLI with isolated compilers and package metadata."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASH = shutil.which('bash')
STUB = r'''
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['BUILD_TEST_LOG'], 'a') as out:
    out.write(json.dumps({'name': name, 'args': args, 'cwd': os.getcwd(),
                          'cflags': os.getenv('CFLAGS', '')}) + '\n')
if name in ('rustc', 'cargo'):
    print(name, os.getenv('RUST_VERSION', '1.70.0'))
    sys.exit(0)
if name == 'dpkg-query':
    print(os.getenv('VLC_PLUGIN_STATUS', 'installed'))
    sys.exit(0)
if name == 'pkg-config':
    if args[-1] == os.getenv('MISSING_LIBRARY'):
        sys.exit(1)
    if '--cflags' in args: print('-I"include with spaces"')
    sys.exit(0)
if name == 'SoapySDRUtil':
    print('Available factories... ' + os.getenv('FACTORIES', 'rtlsdr, hackrf'))
    sys.exit(0)
if name == 'ffmpeg':
    if '-protocols' in args: print(os.getenv('PROTOCOLS', 'Input:\n srt\nOutput:\n srt'))
    if '-encoders' in args: print(os.getenv('ENCODERS', ' V..... libx264 H264\n A..... aac AAC'))
    sys.exit(0)
if name == 'make': sys.exit(int(os.getenv('MAKE_STATUS', '0')))
if name in ('cc', 'g++', 'clang', 'ccache'):
    for arg in args:
        if arg.endswith(('.c', '.cc')):
            header = os.getenv('FAIL_HEADER')
            if header and ('<' + header + '>') in pathlib.Path(arg).read_text():
                print('header or linker input missing', file=sys.stderr)
                sys.exit(1)
'''


class BuildEnvironment:
    def __init__(self, path):
        self.path = Path(path)
        self.bin = self.path / 'bin'
        self.bin.mkdir()
        self.log = self.path / 'commands.jsonl'
        self.env = {**os.environ, 'PATH': str(self.bin), 'BUILD_TEST_LOG': str(self.log)}
        for key in ('CC', 'CXX', 'CPPFLAGS', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LDLIBS', 'CARGO'):
            self.env.pop(key, None)
        for name in ('bash', 'dirname', 'uname', 'mktemp', 'rm', 'cat', 'sed', 'awk', 'realpath', 'ln', 'getconf', 'mkdir'):
            (self.bin / name).symlink_to(shutil.which(name))
        for name in ('cc', 'g++', 'clang', 'ccache', 'make', 'pkg-config', 'cargo', 'rustc',
                     'dpkg-query', 'ffmpeg', 'vlc', 'cvlc', 'ffplay', 'direwolf', 'SoapySDRUtil'):
            target = self.bin / name
            target.write_text('#!' + sys.executable + '\n' + STUB)
            target.chmod(0o755)

    def run(self, *args, script='tools/build.sh'):
        return subprocess.run([BASH, str(ROOT / script), *args], cwd=self.path,
                              env=self.env, text=True, capture_output=True, timeout=20)

    def commands(self, name):
        if not self.log.exists(): return []
        return [row for line in self.log.read_text().splitlines()
                if (row := json.loads(line))['name'] == name]


class BuildHelperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='linrad-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.fixture = BuildEnvironment(self.temp.name)

    def test_check_does_not_build_and_works_outside_repository(self):
        result = self.fixture.run('--check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.fixture.commands('make'))
        self.assertTrue(self.fixture.commands('g++'))
        self.assertIn('-std=gnu++20', self.fixture.commands('g++')[0]['args'])
        self.assertEqual(self.fixture.commands('cc')[0]['cwd'], str(ROOT))

    def test_required_libraries_and_broken_headers_block_build(self):
        for env in ({'MISSING_LIBRARY': 'uhd'}, {'FAIL_HEADER': 'vlc/vlc.h'},
                    {'MISSING_LIBRARY': 'sdl2'}, {'FAIL_HEADER': 'fftw3.h'}):
            with self.subTest(env=env):
                self.fixture.env.update(env)
                result = self.fixture.run()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('[ERROR]', result.stdout)
                if 'sdl2' in env.values() or 'fftw3.h' in env.values():
                    self.assertIn('FM-ATV', result.stdout)
                    self.assertIn('development package missing' if 'MISSING_LIBRARY' in env else 'compile/link failed', result.stdout)
                self.assertFalse(self.fixture.commands('make'))
                for key in env: self.fixture.env.pop(key)

    def test_missing_required_tools_block_build(self):
        for program in ('cc', 'g++', 'make', 'pkg-config', 'cargo', 'rustc'):
            with self.subTest(program=program):
                path = self.fixture.bin / program
                backup = path.with_suffix('.disabled')
                path.rename(backup)
                try:
                    result = self.fixture.run()
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn('executable missing', result.stdout)
                    self.assertFalse(self.fixture.commands('make'))
                finally: backup.rename(path)

    def test_missing_tools_do_not_hide_missing_libraries(self):
        (self.fixture.bin / 'cargo').unlink()
        self.fixture.env['MISSING_LIBRARY'] = 'gtk+-3.0'
        result = self.fixture.run('--check')
        self.assertEqual(result.returncode, 1)
        self.assertIn('cargo: executable missing', result.stdout)
        self.assertIn('gtk+-3.0: development package missing', result.stdout)

    def test_old_rust_is_rejected(self):
        self.fixture.env['RUST_VERSION'] = '1.65.0'
        result = self.fixture.run('--check')
        self.assertEqual(result.returncode, 1)
        self.assertIn('Rust/Cargo 1.70+ required', result.stdout)

    def test_mint_and_debian_install_before_build(self):
        for distro in ('linuxmint', 'debian'):
            with self.subTest(distro=distro):
                # Intercept os-release and apt inside the child shell only.
                code = r'''source "$1"
                source() { ID=$TEST_DISTRO; }
                sudo() { "$@"; }
                apt-get() { echo "APT $*"; }
                build_main --install-deps'''
                result = subprocess.run([BASH, '-c', code, 'test', str(ROOT / 'tools/build.sh')],
                    cwd=self.temp.name, env={**self.fixture.env, 'TEST_DISTRO': distro},
                    text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('APT update', result.stdout)
                self.assertIn('vlc-plugin-base', next(line for line in result.stdout.splitlines() if line.startswith('APT install')))
                self.assertIn('libzvbi-dev', result.stdout)
                self.assertIn('libsdl2-dev', result.stdout)
                self.assertIn('libfftw3-dev', result.stdout)
                self.assertTrue(self.fixture.commands('make'))

    def test_ubuntu_missing_vlc_plugins_blocks_build(self):
        result = subprocess.run([BASH, '-c',
            'source "$1"; source() { ID=ubuntu; }; build_main',
            'test', str(ROOT / 'tools/build.sh')], cwd=self.temp.name,
            env={**self.fixture.env, 'VLC_PLUGIN_STATUS': 'config-files'},
            text=True, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('[ERROR] vlc-plugin-base', result.stdout)
        self.assertIn('sudo apt install vlc-plugin-base', result.stdout)
        self.assertFalse(self.fixture.commands('make'))

    def test_package_advice(self):
        for distro, command in [('linuxmint ubuntu debian', 'apt install libgtk-3-dev libavformat-dev'),
                                ('debian', 'apt install libgtk-3-dev libavformat-dev'),
                                ('fedora', r'dnf install pkgconfig\(gtk+-3.0\)'),
                                ('opensuse-tumbleweed', r'zypper install pkgconfig\(gtk+-3.0\)'),
                                ('arch', 'pacman -S --needed gtk3 ffmpeg')]:
            with self.subTest(distro=distro):
                result = subprocess.run([BASH, '-c',
                    'source "$1"; build_package_help "$(build_manager "$2")" gtk+-3.0 libavformat gtk+-3.0',
                    'test', str(ROOT / 'tools/build.sh'), distro], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0)
                self.assertIn(command, result.stdout)
                self.assertEqual(result.stdout.count('libgtk-3-dev'), int('debian' in distro))

    def test_mint_optional_sdr_hints(self):
        result = subprocess.run([BASH, '-c',
            'source "$1"; for driver in rtlsdr airspyhf rfspace lime sdrplay utility; do '
            'build_sdr_hint apt "$driver"; done', 'test', str(ROOT / 'tools/build.sh')],
            text=True, capture_output=True)
        for hint in ('soapysdr-module-rtlsdr', 'soapysdr-module-airspyhf', 'soapysdr-module-rfspace',
                     'liblimesuite-dev', 'SoapySDRPlay3', 'soapysdr-tools'):
            self.assertIn(hint, result.stdout)

    def test_optional_dependencies_only_warn(self):
        self.fixture.env['MISSING_LIBRARY'] = 'LimeSuite'
        (self.fixture.bin / 'ffplay').unlink()
        result = self.fixture.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('[WARNING] ffplay missing', result.stdout)
        self.assertIn('[WARNING] Soapy driver rfspace missing', result.stdout)
        self.assertIn('For rfspace:', result.stdout)
        self.assertEqual(len(self.fixture.commands('make')), 1)

    def test_make_receives_checked_flags_and_propagates_failure(self):
        self.fixture.env.update(CC='ccache clang', CFLAGS='-g -DNAME="two words"', MAKE_STATUS='7')
        result = self.fixture.run('-j3', '--build-dir', 'build with spaces')
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        command = self.fixture.commands('make')[0]['args']
        for arg in ('-j3', 'BUILD_DIR=build with spaces', 'CC=ccache clang', 'CFLAGS=-g -DNAME="two words"'):
            self.assertIn(arg, command)
        self.assertIn('-DNAME=two words', self.fixture.commands('ccache')[0]['args'])
        self.assertIn('-Iinclude with spaces', self.fixture.commands('ccache')[1]['args'])

    def test_flag_parser_never_evaluates_substitutions(self):
        marker = Path(self.temp.name) / 'should-not-exist'
        self.fixture.env['CFLAGS'] = "-DVALUE='$(touch " + str(marker) + ")'"
        result = self.fixture.run('--check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(marker.exists())

    def test_invalid_options_and_unterminated_quotes_do_not_build(self):
        for args in (('-j', '0'), ('-j', '-1'), ('-j', 'abc'), ('--build-dir', ''), ('--unknown',), ('-j',), ('--check', '--install-deps')):
            result = self.fixture.run(*args)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.fixture.env['CC'] = "'unterminated"
        self.assertEqual(self.fixture.run().returncode, 2)
        self.assertFalse(self.fixture.commands('make'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
