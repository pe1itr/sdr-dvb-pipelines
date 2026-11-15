"""Exercise the real bonding coordinator with controlled decoder TS outputs."""
import os
import json
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/linrad_dvb'


class Bonding(unittest.TestCase):
    def test_two_receivers_merge_exact_source_and_relative_tuning(self):
        self.merge_trial(automatic=True, upper=False)

    def test_manual_activation_from_upper_lane(self):
        self.merge_trial(automatic=False, upper=True)

    def test_automatic_partner_below_center(self):
        self.merge_trial(automatic=True, upper=True)

    def test_opt_out_detects_without_starting_partner(self):
        self.merge_trial(automatic=False, upper=False, activate=False)

    def test_native_distribution_from_video_lane(self):
        self.merge_trial(automatic=True, upper=False, native=True)

    def test_native_distribution_from_audio_lane(self):
        self.merge_trial(automatic=True, upper=True, native=True)

    def merge_trial(self, automatic, upper, activate=True, native=False):
        with tempfile.TemporaryDirectory() as folder:
            d = Path(folder)
            source, a, b = [d / name for name in ('source.ts', 'a.ts', 'b.ts')]
            subprocess.run([ROOT/'build/bonding_fixture', source, a, b, *(['native'] if native else [])], check=True)
            fake = d/'decoder.py'
            fake.write_text('''import sys, time
from pathlib import Path
d = Path(sys.argv[1])
offset = float(sys.argv[sys.argv.index('--frequency-offset')+1])
partner = -50000 if sys.argv[2] == "upper" else 50000
assert offset in (0, partner)
assert '--channel-filter' in sys.argv and '--raw-ts-output' in sys.argv
(d/('offset-'+str(int(offset)))).touch()
lane = ('b.ts' if sys.argv[2] == 'upper' else 'a.ts') if offset == 0 else ('a.ts' if sys.argv[2] == 'upper' else 'b.ts')
sys.stdout.buffer.write((d/lane).read_bytes())
sys.stdout.buffer.flush()
if offset == 0 and sys.argv[3] != 'native':
 time.sleep(.5)
 sys.stdout.buffer.write((d/lane).read_bytes())
 sys.stdout.buffer.flush()
while sys.stdin.buffer.read(8192): pass
''')
            control=d/'enable'
            command = [str(EXE), '--decoder-worker=bonding', *([] if automatic else ['--no-auto']),
                       '--', sys.executable, str(fake), str(d), 'upper' if upper else 'lower',
                       'native' if native else 'shared',
                       '--sample-rate', '250000', '--input-format', 's16']
            with (d/'out.ts').open('wb') as output, (d/'log').open('wb') as log:
                proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=output, stderr=log,
                                        env=dict(os.environ, LINRAD_BOND_CONTROL=str(control)), start_new_session=True)
                def cleanup():
                    if proc.poll() is None:
                        os.killpg(proc.pid, signal.SIGTERM)
                        proc.wait(timeout=5)
                    if not proc.stdin.closed:
                        proc.stdin.close()
                self.addCleanup(cleanup)
                proc.stdin.write(bytes(16384)); proc.stdin.flush()
                deadline = time.monotonic()+5
                partner_offset=-50000 if upper else 50000
                partner=d/('offset-'+str(partner_offset))
                while activate and not partner.exists() and time.monotonic() < deadline:
                    if not automatic and '[bond] detected' in (d/'log').read_text():
                        control.touch()
                    time.sleep(.01)
                if activate:
                    self.assertTrue(partner.exists(), (d/'log').read_text())
                proc.stdin.close()
                self.assertEqual(proc.wait(timeout=30), 0)
            actual=(d/'out.ts').read_bytes()
            text=(d/'log').read_text()
            self.assertIn('[bond] detected', text)
            self.assertIn('receiver=0 offset=+0 Hz', text)
            if activate:
                if native:
                    # Native reception remuxes PSI, continuity and nulls.
                    # Compare every ES/PES payload byte independently instead.
                    def payload(data, pid):
                        result=bytearray()
                        self.assertEqual(len(data)%188, 0)
                        for pos in range(0, len(data), 188):
                            p=data[pos:pos+188]
                            self.assertEqual(p[0], 0x47)
                            if ((p[1]&31)<<8 | p[2])==pid and p[3]&0x10:
                                offset=5+p[4] if p[3]&0x20 else 4
                                result.extend(p[offset:])
                        return result
                    for pid in (256,257):
                        expected=payload(source.read_bytes(), pid)
                        self.assertTrue(expected)
                        self.assertEqual(payload(actual, pid), expected)
                else:
                    # Discovery may already have passed the outer PAT/PMT.
                    self.assertTrue(actual.endswith(source.read_bytes()))
                self.assertIn('[bond] pair confirmed', text)
                self.assertIn('receiver=1 offset='+format(partner_offset,'+.0f'), text)
                self.assertEqual(text.count('iq_dropped=0'), 2)
                self.assertIn('"distribution":'+str(2 if native else 0), text)
                if native:
                    status=json.loads([line[len('[bond-status] '):] for line in text.splitlines()
                                       if line.startswith('[bond-status] ')][-1])
                    self.assertEqual(status['profile'], 0)
                    self.assertEqual(status['recovered_symbols'], 0)
            else:
                self.assertNotIn('[bond] active', text)
                self.assertNotIn('receiver=1', text)
                self.assertFalse(partner.exists())

    def test_regular_ts_passes_unchanged_without_discovery(self):
        with tempfile.TemporaryDirectory() as folder:
            d=Path(folder); fake=d/'plain.py'
            packet=bytes([0x47, 0x1f, 0xff, 0x10])+bytes([0xff])*184
            fake.write_text('import sys\nsys.stdout.buffer.write(bytes.fromhex('+repr(packet.hex())+'))\n')
            result=subprocess.run([str(EXE),'--decoder-worker=bonding','--',sys.executable,str(fake),
                                   '--sample-rate','250000'], input=b'', capture_output=True, timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout,packet)
            self.assertNotIn(b'[bond] active', result.stderr)
            self.assertNotIn(b'[bond] detected', result.stderr)

    def test_invalid_passband(self):
        result=subprocess.run([str(EXE),'--decoder-worker=bonding','--anchor-offset','-25000','--',
                               '/unused','--sample-rate','80000'],input=b'',capture_output=True,timeout=5)
        self.assertEqual(result.returncode,2)


if __name__ == '__main__':
    unittest.main()
