"""New Soapy backends: model identity, IQ format, controls and guards without radios."""
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SoapyInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.library = str(Path(cls.tmp.name) / 'mock.so')
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'SoapySDR'], text=True))
        subprocess.run(['cc', '-shared', '-fPIC', str(ROOT / 'tests/mock_soapy.c'),
                        '-o', cls.library, *flags], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def receiver(self, source, *args, commands=b'', model=None, actual_rate=None, no_2m5=False):
        env = dict(os.environ, LD_PRELOAD=self.library,
                   MOCK_MODEL=model or {'rsp1a': 'RSP1A', 'rsp1b': 'RSP1B', 'airspyhf': 'AirspyHF', 'airspyr2': 'Airspy'}[source])
        if no_2m5:
            env["MOCK_NO_2M5"] = "1"
        if actual_rate:
            env['MOCK_RATE'] = str(actual_rate)
        return subprocess.run([ROOT / 'build/sdr_to_stdout', '--source', source,
            '--sample-rate', '10000000' if source == 'airspyr2' else '768000' if source == 'airspyhf' else '2000000',
            '--frequency', '144000000', '--gain', '0' if source == 'airspyhf' else '40',
            '--control-stdin', *args], input=commands, capture_output=True, env=env, timeout=5)

    def test_stream_conversion_and_live_controls(self):
        for source in ('rsp1a', 'rsp1b', 'airspyhf', 'airspyr2'):
            for fmt in ('s16', 'u8'):
                with self.subTest(source=source, fmt=fmt):
                    result = self.receiver(source, '--format', fmt,
                        commands=b'145000000\ngain 41\n')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = struct.pack('<4h', 32767, -32768 if source == 'airspyr2' else -32767, 16384, -16384) if fmt == 's16' else bytes([255, 0, 192, 64])
                    self.assertEqual(result.stdout, expected, result.stderr)
                    self.assertIn(b'MOCK frequency 145000000', result.stderr)
                    if source == 'airspyhf':
                        self.assertIn(b'MOCK agc 1', result.stderr)
                        self.assertNotIn(b'MOCK gain', result.stderr)
                    else:
                        self.assertIn(b'MOCK agc 0', result.stderr)
                        self.assertIn(b'MOCK gain aggregate 41' if source == 'airspyr2' else b'MOCK gain IFGR 41', result.stderr)

    def test_wrong_model_driver_and_coerced_rate_do_not_stream(self):
        for result in (self.receiver('rsp1b', model='RSP1A'),
                       self.receiver('rsp1a', model='RSPdx'),
                       self.receiver('airspyhf', model='Airspy'),
                       self.receiver('airspyhf', '--args', 'driver=airspy'),
                       self.receiver('rsp1b', actual_rate=2048000),
                       self.receiver('airspyhf', '--sample-rate', '700000'),
                       self.receiver('airspyr2', model='AirspyHF'),
                       self.receiver('airspyr2', '--args', 'driver=airspyhf'),
                       self.receiver('airspyr2', actual_rate=2500000),
                       self.receiver('airspyr2', '--sample-rate', '2500000', no_2m5=True)):
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(b'MOCK streaming', result.stderr)

    def test_r2_low_rate_and_uhf_tuning(self):
        result = self.receiver('airspyr2', '--sample-rate', '2500000', '--frequency', '437000000',
                               commands=b'1291000000\ngain 45\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'MOCK open airspy', result.stderr)
        self.assertIn(b'MOCK frequency 1291000000', result.stderr)
        self.assertIn(b'MOCK gain aggregate 45', result.stderr)
        result = self.receiver('airspyr2', '--sample-rate', '2500000', '--channel-bandwidth', '2250001')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNotIn(b'MOCK open', result.stderr)

    def test_invalid_frequency_bandwidth_and_live_tuning(self):
        for source, option, value in (('airspyhf', '--frequency', '31000000'),
                ('airspyhf', '--frequency', '437000000'), ('rsp1a', '--frequency', '999'),
                ('rsp1b', '--frequency', '2000000001'), ('airspyhf', '--channel-bandwidth', '675000'),
                ('rsp1b', '--channel-bandwidth', '1600000'),
                ('airspyr2', '--frequency', '23999999'), ('airspyr2', '--frequency', '1700000001'),
                ('airspyr2', '--sample-rate', '3000000'), ('airspyr2', '--gain', '46'),
                ('airspyr2', '--gain', '30.5'), ('airspyr2', '--channel-bandwidth', '9000001')):
            result = self.receiver(source, option, value)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertNotIn(b'MOCK open', result.stderr)
        for source, command in (('airspyhf', b'437000000\n'), ('rsp1b', b'2000000001\n')):
            result = self.receiver(source, commands=command)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr.count(b'MOCK frequency'), 1, result.stderr)
        result = self.receiver('airspyhf', '--frequency', '259900000', '--receiver-offset', '100000',
                               commands=b'259950000\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'Ignoring tune outside model/input range', result.stderr)
        self.assertNotIn(b'MOCK frequency 259950000', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
