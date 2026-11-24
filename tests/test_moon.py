"""Exercise the real pre-demodulator mixer against an independent DE440s case."""
import cmath
import configparser
import json
import math
import re
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RATE = 20000
EPOCH = 1802880540
SPEC = 'JO32ET,JN47CG,1299000000,1298999000,4000'


def replay(path, spec=None):
    args = [ROOT/'build/iq_file_to_stdout', '--in', path, '--sample-rate', str(RATE)]
    if spec:
        args.extend(['--moon-track', spec])
    return subprocess.run(args, capture_output=True, timeout=10)


class MoonTests(unittest.TestCase):
    def test_recorded_tone(self):
        # JPL DE440s, geometric WGS84 grid centres, RX JO32ET + TX JN47CG.
        # Include a 1 kHz tuning offset and leave +4 Hz for the demodulator.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'echo.iq')
            hz = 1000 + 2300.49776 + 4
            raw = bytearray()
            for n in range(RATE * 2):
                z = 12000 * cmath.exp(2j*math.pi*hz*n/RATE)
                raw.extend(struct.pack('<hh', round(z.real), round(z.imag)))
            path.write_bytes(raw)
            sidecar = Path(str(path)+'.json')
            self.assertNotEqual(replay(path, SPEC).returncode, 0)
            sidecar.write_text(json.dumps({'recorded_at_utc':'2027-02-17T16:09:00Z'}))
            self.assertEqual(replay(path).stdout, raw)
            p = replay(path, SPEC)
            self.assertEqual(p.returncode, 0, p.stderr)
            rows = re.findall(rb'sample_utc=([\d.]+) doppler=([+\d.-]+) offset=([+\d.-]+)', p.stderr)
            self.assertEqual(len(rows), 2)
            for i, (utc, doppler, offset) in enumerate(rows):
                self.assertEqual(float(utc), EPOCH+i)
                self.assertAlmostEqual(float(doppler), 2300.49776-.059*i, delta=.3)
                self.assertAlmostEqual(float(offset)-float(doppler), 1000)
            self.assertEqual(len(p.stdout), len(raw))
            samples = [complex(i,q) for i,q in struct.iter_unpack('<hh', p.stdout)]
            # Every adjacent phase increment, including the UTC/step boundary.
            for a,b in zip(samples,samples[1:]):
                self.assertAlmostEqual(cmath.phase(b/a)*RATE/(2*math.pi), 4, delta=1)
            path.write_bytes(raw[:800])
            sidecar.write_text(json.dumps({'capture_start_unix_us':(EPOCH+60)*1000000}))
            r = replay(path, SPEC)
            self.assertEqual(r.returncode, 0, r.stderr)
            doppler = float(re.search(rb'doppler=([+\d.-]+)', r.stderr)[1])
            self.assertAlmostEqual(doppler, 2296.95227, delta=.3)
            self.assertNotEqual(replay(path,SPEC.replace('1298999000','1298000000')).returncode,0)
            self.assertNotEqual(replay(path,SPEC.replace('JN47CG','BAD')).returncode,0)

    def test_simulation_clock(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp,'linrad-dvb-20261123T120000.000000Z-test.iq')
            path.write_bytes(bytes(800))
            sidecar=Path(str(path)+'.json')
            meta={'capture_start_unix_us':(EPOCH-86400)*1000000,
                  'recorded_at_utc':'2025-01-01T00:00:00Z',
                  'simulation_start_utc':'2027-02-17T17:09:00.123456+01:00'}
            sidecar.write_text(json.dumps(meta))
            for spec in (None,SPEC):
                p=replay(path,spec)
                self.assertEqual(p.returncode,0,p.stderr)
                self.assertIn(b'sample_utc=1802880540.123456',p.stderr)
            for bad in ('2027-02-17T16:09:00','unknown',None):
                meta['simulation_start_utc']=bad
                sidecar.write_text(json.dumps(meta))
                p=replay(path,SPEC)
                self.assertNotEqual(p.returncode,0)
                self.assertIn(b'simulation_start_utc',p.stderr)

    def test_pipeline(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'rx.ini')
            ini = (ROOT/'linrad-dvb.example.ini').read_text()
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read_string(ini)
            cfg['logging']['my_locator']='JO32ET'
            cfg['linrad']['frequency']='1299000000'
            cfg['moon']['dx_locator']='JN47CG'
            cfg['moon']['tx_hz']='144000000'  # obsolete setting in an existing INI
            for enabled in (False, True):
                cfg['moon']['enabled']=str(enabled).lower()
                with path.open('w') as f:
                    cfg.write(f)
                p=subprocess.run([ROOT/'build/linrad_dvb','--config',path,'--source','linrad',
                    '--standard','DVB-T','--sr','35000','--print-pipeline'],capture_output=True,text=True)
                self.assertEqual(p.returncode,0,p.stderr)
                self.assertEqual('--moon-track' in p.stdout,enabled)
                if enabled:
                    self.assertIn('JO32ET,JN47CG,1299000000,1299000000,35000',p.stdout)

            iq=Path(tmp,'simulation.iq')
            iq.write_bytes(bytes(800))
            Path(str(iq)+'.json').write_text(json.dumps({
                'format':'cs16_le','standard':'DVB-T','sample_rate_hz':175000,
                'center_frequency_hz':432100000,'bandwidth_hz':35000,
                'simulation_start_utc':'2027-02-17T16:09:00Z'}))
            cfg['file']['receiver_offset']='1500'
            with path.open('w') as f:
                cfg.write(f)
            p=subprocess.run([ROOT/'build/linrad_dvb','--config',path,'--iq-file',iq,
                              '--print-pipeline'],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            self.assertIn('JO32ET,JN47CG,432101500,432100000,35000',p.stdout)


if __name__ == '__main__':
    unittest.main()
