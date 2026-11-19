"""IQ replay pacing, byte integrity, metadata and graceful pipeline EOF."""
import configparser
import json
import os
import shlex
from pathlib import Path
import select
import signal
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLAYER = ROOT / 'build/iq_file_to_stdout'
UI = ROOT / 'build/linrad_dvb'


class FileReplayTests(unittest.TestCase):
    def test_pacing_headers_and_exact_bytes(self):
        for fmt, stride in [('s16', 4), ('u8', 2)]:
            with tempfile.TemporaryDirectory() as tmp, self.subTest(fmt=fmt):
                path = Path(tmp, 'IQ recording.iq')
                data = bytes(range(256)) * (4000 * stride // 256) + bytes(range(4000 * stride % 256))
                path.write_bytes(b'header' + data)
                start = time.monotonic()
                p = subprocess.run([PLAYER, '--in', path, '--sample-rate', '10000', '--input-format', fmt,
                                    '--header-bytes', '6'], capture_output=True, timeout=4)
                elapsed = time.monotonic() - start
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(p.stdout, data)
                self.assertGreaterEqual(elapsed, .38)
                self.assertLess(elapsed, 2)
                self.assertIn(b'end of file', p.stderr)

    def test_receiver_offset_centers_tones_across_chunks(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'tone.iq')
            for fmt, code, amplitude, zero in [('s16', 'h', 8192, 0), ('u8', 'B', 32, 128),
                                                ('cf32', 'f', .25, 0)]:
                for sign in (-1, 1):
                    with self.subTest(fmt=fmt, sign=sign):
                        cycle = (1, 0, 0, sign, -1, 0, 0, -sign)
                        path.write_bytes(struct.pack('<8' + code, *(zero + amplitude*x for x in cycle))*31)
                        p = subprocess.run([PLAYER, '--in', path, '--sample-rate', '1100',
                                            '--input-format', fmt, '--receiver-offset', str(sign*275)],
                                           capture_output=True, timeout=3)
                        self.assertEqual(p.returncode, 0, p.stderr)
                        expected = struct.pack('<2B', 160, 128) if fmt=='u8' else struct.pack('<2h', 8192, 0)
                        self.assertEqual(p.stdout, expected*124)

    def test_float_conversion_and_invalid_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'samples.cf32')
            path.write_bytes(struct.pack('<8f', -2, -1, -.5, 0, .5, 1, 2, .25))
            p = subprocess.run([PLAYER, '--in', path, '--sample-rate', '1000', '--input-format', 'cf32'],
                               capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(struct.unpack('<8h', p.stdout), (-32767, -32767, -16384, 0, 16384, 32767, 32767, 8192))
            for data in (b'', b'abc', struct.pack('<2f', float('nan'), 0)):
                path.write_bytes(data)
                p = subprocess.run([PLAYER, '--in', path, '--sample-rate', '1000', '--input-format', 'cf32'],
                                   capture_output=True, timeout=3)
                self.assertNotEqual(p.returncode, 0)
                self.assertEqual(p.stdout, b'')

    def test_stop_with_blocked_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'long.iq')
            path.write_bytes(bytes(4 * 500000))
            p = subprocess.Popen([PLAYER, '--in', path, '--sample-rate', '250000'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                time.sleep(.1)  # Fill stdout without a reader.
                start = time.monotonic()
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=1)
                self.assertEqual(p.returncode, 0)
                self.assertLess(time.monotonic() - start, .8)
            finally:
                if p.poll() is None:
                    p.kill(); p.wait()
                p.stdout.close(); p.stderr.close()

    def test_live_filter_control(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'tone.iq')
            path.write_bytes(struct.pack('<2h', 1000, 0) * 25000)
            p = subprocess.Popen([PLAYER, '--in', path, '--sample-rate', '25000', '--control-stdin'],
                                 stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                p.stdin.write(b'bandwidth 5000\n'); p.stdin.flush()
                # Wait for the first applied change: under load startup can take
                # longer than a fixed sleep, and queued controls keep only the latest value.
                log = b''
                deadline = time.monotonic() + 3
                while b'IQ filter width 5000' not in log:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([p.stderr], [], [], remaining)[0]:
                        break
                    chunk = os.read(p.stderr.fileno(), 4096)
                    if not chunk:
                        break
                    log += chunk
                self.assertIn(b'IQ filter width 5000', log)
                p.stdin.write(b'bandwidth 0\n'); p.stdin.flush()
                _, tail = p.communicate(timeout=3)
                log += tail
                self.assertEqual(p.returncode, 0, log)
                self.assertIn(b'IQ filter width 5000', log)
                self.assertIn(b'IQ filter width 0', log)
            finally:
                if p.poll() is None:
                    p.kill(); p.communicate()


class FilePipelineTests(unittest.TestCase):
    def config(self, tmp):
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT / 'linrad-dvb.example.ini')
        cfg.remove_section('session')
        for key, value in cfg['paths'].items():
            if '/' in value:
                cfg['paths'][key] = str((ROOT / value).resolve())
        path = Path(tmp, 'receiver.ini')
        return cfg, path

    def preview(self, cfg, config, iq):
        with config.open('w') as f:
            cfg.write(f)
        return subprocess.run([UI, '--config', config, '--iq-file', iq, '--print-pipeline'],
                              capture_output=True, text=True, timeout=3)

    def test_s2_auto_uses_recording_symbol_rate(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, config = self.config(tmp)
            iq = Path(tmp, 'automatic.iq')
            iq.write_bytes(bytes(400))
            for rate in (125000, 333000, 333001, 5000000):
                with self.subTest(rate=rate):
                    Path(str(iq)+'.json').write_text(json.dumps({
                        'format': 'cs16_le', 'sample_rate_hz': 8000000,
                        'standard': 'DVB-S2', 'symbol_rate_hz': rate,
                        'modulation': '8PSK' if rate == 5000000 else 'QPSK',
                        'fec': '3/4' if rate == 5000000 else '1/2'}))
                    p = self.preview(cfg, config, iq)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    decoder = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")][1]
                    self.assertEqual(decoder[decoder.index('--sr')+1], str(rate))
                    self.assertEqual('--fastlock' in decoder, rate <= 333000)
                    self.assertEqual(decoder[decoder.index('--ldpc-batch')+1], '1' if rate <= 333000 else '32')

    def test_airscatter_metadata_and_filename_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, config = self.config(tmp)
            origin = Path(tmp, 'original.cf32')
            origin.with_suffix('.cf32.json').write_text(json.dumps({
                'format': 'fc32', 'sample_rate': 999999, 'standard': 'DVB-T', 'modulation': 'QPSK',
                'config': {'bandwidth': 40000, 'guard': 32, 'fec': 3, 'frequency': 144600000}}))
            iq = Path(tmp, 'arbitrary recording.iq')
            iq.write_bytes(bytes(400))
            metadata = {'format': 'cs16_le', 'sample_rate_hz': 250000, 'layout': 'IQ', 'byte_order': 'little',
                        'parameters': {'iq_in': str(origin), 'frequency_mhz': 144.6}}
            Path(str(iq) + '.json').write_text(json.dumps(metadata))
            p = self.preview(cfg, config, iq)
            self.assertEqual(p.returncode, 0, p.stderr)
            for fragment in ("'--sample-rate' '250000'", "'--input-format' 's16'", "'--sr' '40000'", "'--fec' '1/2'", "'--gi' '1/32'"):
                self.assertIn(fragment, p.stdout)
            self.assertNotIn('999999', p.stdout)
            # Copied airscatter output without its original file still has filename hints.
            renamed = Path(tmp, 'dvbt-qpsk-bw40000-gi1-32-fec1-2-fs250000-40s_rx.iq')
            renamed.write_bytes(iq.read_bytes())
            metadata['parameters']['iq_in'] = str(Path(tmp, 'missing.cf32'))
            Path(str(renamed) + '.json').write_text(json.dumps(metadata))
            p = self.preview(cfg, config, renamed)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("'--sr' '40000'", p.stdout)

    def test_float_metadata_custom_symbolrate_and_rejections(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, config = self.config(tmp)
            iq = Path(tmp, 'dvbs-qpsk-sr33000-fec1-2-fs528000.cf32')
            iq.write_bytes(bytes(800))
            metadata = {'format': 'fc32', 'sample_rate_hz': 528000, 'header_bytes': 0}
            sidecar = Path(str(iq) + '.json')
            sidecar.write_text(json.dumps(metadata))
            p = self.preview(cfg, config, iq)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("'--input-format' 'cf32'", p.stdout)
            self.assertIn("'--s16'", p.stdout)
            self.assertIn("'--sr' '33000'", p.stdout)
            for bad in ('{bad', '[]', json.dumps(dict(metadata, sample_rate_hz=0)),
                        json.dumps(dict(metadata, format='cs16_be')), json.dumps(dict(metadata, layout='QI')),
                        json.dumps(dict(metadata, sample_rate_hz='528000'))):
                sidecar.write_text(bad)
                p = self.preview(cfg, config, iq)
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertEqual(p.stdout, '')

    def test_finite_pipeline_drains_decoder_and_preserves_tail(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, config = self.config(tmp)
            decoder = Path(tmp, 'decoder')
            decoder.write_text('#!/usr/bin/env python3\nimport sys,time\ndata=sys.stdin.buffer.read()\ntime.sleep(.15)\nsys.stdout.buffer.write(data+b"TAIL")\n')
            decoder.chmod(0o755)
            cfg['leandvb']['backend'] = 'external'
            cfg['paths']['leandvb'] = str(decoder)
            cfg['paths']['rbdvbt_rx'] = str(decoder)
            cfg['session'] = {'renderer': 'mpegts'}
            cfg['renderer'] = {'output_file': 'received.ts'}
            # Exercise generic EOF/tail handling using the external satellite
            # backend; DVB-T now always uses the compiled-in decoder.
            iq = Path(tmp, 'dvbs-qpsk-sr35000-fec1-2-fs250000.iq')
            data = bytes(range(256)) * 100
            iq.write_bytes(data)
            Path(str(iq) + '.json').write_text(json.dumps({'format': 's16', 'sample_rate_hz': 250000}))
            with config.open('w') as f:
                cfg.write(f)
            p = subprocess.run([UI, '--config', config, '--iq-file', iq, '--run'],
                               capture_output=True, timeout=6)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn(b'playback complete', p.stderr)
            self.assertEqual(Path(tmp, 'received.ts').read_bytes(), data + b'TAIL')


if __name__ == '__main__':
    unittest.main(verbosity=2)
