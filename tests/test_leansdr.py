#!/usr/bin/env python3
"""CPU-only integration tests using deterministic TS and RRC-shaped IQ."""
import ctypes
import os
from pathlib import Path
import random
import re
import signal
import socket
import struct
import subprocess
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RX = ROOT / 'build/linrad_dvb'
TX = ROOT / 'build/leandvbtx'
LDPC = ROOT / 'build/ldpc_tool'


def transport(count=2400):
    rng = random.Random(41)
    return b''.join(bytes([0x47, 1, 0, 0x10+(i % 16)]) + struct.pack('>I', i)
                    + rng.randbytes(180) for i in range(count))


def weak_iq(ts, standard, fec, mc, short, pilots, seed, esn0, fading=False):
    import numpy as np
    args = [TX, '--standard', standard, '-f', '4',
            '--rrc-rej', '30', '--s16']
    args += ['--modcod', str(mc)] if standard == 'DVB-S2' else ['--cr', fec]
    if short:
        args += ['--shortframes']
    if pilots:
        args += ['--pilots']
    tx = subprocess.run(args, input=ts, capture_output=True, check=True, timeout=60)
    raw = np.frombuffer(tx.stdout, dtype='<i2').reshape(-1, 2).astype(float)*.1
    signal = raw[:, 0] + 1j*raw[:, 1]
    n = np.arange(len(signal))
    # Small carrier offset plus linear drift, expressed per symbol.
    phase = 2*np.pi*(.005*n/4 + .001*n*n/(8*len(n)))
    signal *= np.exp(1j*phase)
    power = np.mean(np.abs(signal)**2)
    if fading:
        # Acquire at the nominal Es/N0, pass through a deep fade and recover.
        envelope = np.interp(n/len(n), [0, .2, .4, .6, .8, 1], [1, 1, .08, .08, 1, 1])
        signal *= envelope
    # Es/N0 refers to the unfaded waveform, 4 complex samples/symbol.
    sigma = (power*4/(2*10**(esn0/10)))**.5
    noise = np.random.default_rng(seed).normal(size=raw.shape)*sigma
    raw = np.column_stack((signal.real, signal.imag)) + noise
    clipped = int(np.count_nonzero((raw < -32768) | (raw > 32767)))
    if clipped:
        raise AssertionError(f'{clipped} clipped components invalidate noise calibration')
    return np.rint(raw).astype('<i2').tobytes()


class DecoderStatus(ctypes.Structure):
    _fields_ = ([(k, ctypes.c_uint32) for k in ('magic', 'type', 'locks', 'buffer_valid')]
                + [(k, ctypes.c_float) for k in ('snr_db', 'pilot_lock', 'cfo_hz', 'rms_dbfs', 'peak_dbfs', 'buffer_percent')]
                + [(k, ctypes.c_uint32) for k in ('packets', 'written_packets', 'rs_ok', 'rs_uncorrectable', 'cc_errors')]
                + [(k, ctypes.c_char*n) for k,n in [('service',128),('provider',128),('modulation',24),('fec',16),('guard',16)]]
                + [(k, ctypes.c_uint32) for k in ('satellite','constellation_order','lock_valid','fec_good','fec_bad')]
                + [('mer_db',ctypes.c_float)]
                + [(k, ctypes.c_int32) for k in ('modcod','short_frame','pilots')])


class LeanDvbTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ts = transport()
        cls.cache = {}

    def iq(self, standard='DVB-S2', modcod=5, short=False, pilots=True, fec='1/2'):
        key = standard, modcod, short, pilots, fec
        if key not in self.cache:
            args = [TX, '--standard', standard, '-f', '4', '--rrc-rej', '30', '--s16']
            args += ['--modcod', str(modcod)] if standard == 'DVB-S2' else ['--cr', fec]
            if short:
                args += ['--shortframes']
            if pilots:
                args += ['--pilots']
            p = subprocess.run(args, input=self.ts, capture_output=True, timeout=30)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.cache[key] = p.stdout
        return self.cache[key]

    def receive(self, iq, standard='DVB-S2', modcod=5, short=False, fec='1/2',
                soft=True, workers=1, batch=1, rate=125000, env=None):
        args = [RX, '--decoder-worker=leansdr', '--standard', standard, '--s16',
                '-f', str(4*rate), '--sr', str(rate), '--sampler', 'rrc', '--rrc-rej', '30']
        if standard == 'DVB-S2':
            args += ['--modcods', str(1 << modcod), '--framesizes', '2' if short else '1']
            args += (['--ldpc-helper', 'builtin', '--nhelpers', str(workers), '--ldpc-batch', str(batch)]
                     if soft else ['--ldpc-bf', '20'])
        else:
            args += ['--cr', fec, '--viterbi']
        p = subprocess.run(args, input=iq, capture_output=True, timeout=30, env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(p.stdout) % 188, 0)
        self.assertGreater(len(p.stdout), len(self.ts)*.65, p.stderr)
        indices = []
        acquisition_packets = 0
        for pos in range(0, len(p.stdout), 188):
            packet = p.stdout[pos:pos+188]
            index = struct.unpack('>I', packet[4:8])[0]
            if standard == 'DVB-S' and not indices and packet != self.ts[index*188:(index+1)*188]:
                # Legacy DVB-S emits several RS-damaged packets while acquiring
                # the interleaver. Bound this prefix; all following TS must match.
                acquisition_packets += 1
                self.assertLessEqual(acquisition_packets, 16)
                continue
            self.assertEqual(packet, self.ts[index*188:(index+1)*188])
            indices.append(index)
        self.assertEqual(indices, list(range(indices[0], indices[-1]+1)))
        return p.stdout

    def test_acquisition_issue_41_normal_frames(self):
        # Reported failures: QPSK 3/5 and 5/6 normal frames.
        import numpy as np
        for mc in (5, 9):
            for rate in (35000, 125000):
                with self.subTest(modcod=mc, rate=rate):
                    raw = np.frombuffer(self.iq(modcod=mc), dtype='<i2').reshape(-1, 2)
                    z = raw[:, 0].astype(float) + 1j*raw[:, 1]
                    z *= np.exp(2j*np.pi*0.024*np.arange(len(z))/4)
                    shifted = np.column_stack((z.real, z.imag)).clip(-32768, 32767).astype('<i2').tobytes()
                    self.receive(shifted, modcod=mc, rate=rate)

    def test_qpsk_8psk_normal_short_pilots(self):
        for mc, short, pilots in ((2, False, True), (4, False, False), (3, True, True), (5, True, True), (14, False, True), (14, True, False)):
            with self.subTest(modcod=mc, short=short, pilots=pilots):
                self.receive(self.iq(modcod=mc, short=short, pilots=pilots), modcod=mc, short=short)

    def test_soft_decoding_with_noise(self):
        import numpy as np
        raw = np.frombuffer(self.iq(modcod=4),dtype='<i2').reshape(-1,2).astype(float)
        power = np.mean(np.sum(raw*raw,axis=1))
        # Four samples/symbol, complex AWGN at Es/N0 = 10 dB.
        sigma = (power*4/(2*10))**.5
        noisy = np.clip(raw+np.random.default_rng(41).normal(0,sigma,raw.shape),-32768,32767).astype('<i2')
        self.receive(noisy.tobytes(),modcod=4)

    def test_125k_weak_reception_and_fading(self):
        # Pre-optimization reference recovered 512, 206 and 539 correct
        # packets respectively. Keep margin for platform rounding at the
        # threshold, while rejecting a material loss of weak reception.
        ts = transport(900)
        cases = [(4, False, True, 41, 2, False, 450),
                 (5, True, False, 42, 4, False, 180),
                 (4, True, True, 42, 10, True, 480)]
        for mc, short, pilots, seed, esn0, fading, minimum in cases:
            with self.subTest(modcod=mc, short=short, esn0=esn0, fading=fading):
                iq = weak_iq(ts, 'DVB-S2', '1/2', mc, short, pilots, seed, esn0, fading)
                args = [RX, '--decoder-worker=leansdr', '--standard', 'DVB-S2',
                        '--s16', '-f', '500000', '--sr', '125000', '--sampler', 'rrc',
                        '--rrc-rej', '30', '--rrc-steps', '8', '--modcods', str(1 << mc),
                        '--framesizes', '2' if short else '1', '--ldpc-helper', 'builtin',
                        '--nhelpers', '1', '--ldpc-batch', '1']
                p = subprocess.run(args, input=iq, capture_output=True, timeout=60)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(len(p.stdout)%188, 0)
                count = len(p.stdout)//188
                self.assertGreaterEqual(count, minimum)
                self.assertLess(count, 900*.65)  # Actually exercise reception loss.
                previous = -1
                for pos in range(0, len(p.stdout), 188):
                    packet = p.stdout[pos:pos+188]
                    index = struct.unpack('>I', packet[4:8])[0]
                    self.assertEqual(packet, ts[index*188:(index+1)*188])
                    self.assertGreater(index, previous)
                    previous = index

    def test_parallel_batches_preserve_order_and_tail(self):
        iq = self.iq()
        single = self.receive(iq)
        parallel = self.receive(iq, workers=2, batch=32)
        self.assertEqual(single, parallel)
        bitflip = self.receive(iq, soft=False)
        self.assertEqual(single, bitflip)

    def test_dvbs(self):
        for fec in ('1/2', '2/3'):
            with self.subTest(fec=fec):
                self.receive(self.iq(standard='DVB-S', fec=fec), standard='DVB-S', fec=fec)

    def test_helper_fragmented_protocol_and_portable_equivalence(self):
        # All-zero codeword, plus deterministic sign errors: correction must
        # return the same hard bits in narrow, portable-wide and AVX2 modes.
        rng = random.Random(10)
        frame = bytearray([40]*64800)
        for i in rng.sample(range(64800), 200):
            frame[i] = 216  # -40 as int8
        def run(count, portable):
            env = os.environ.copy()
            if portable:
                env['LINRAD_LDPC_PORTABLE'] = '1'
            p = subprocess.Popen([LDPC, '--framed', '--modcod', '4'], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            payload = struct.pack('=I', count) + bytes(frame)*count
            errors = []
            def writer():
                try:
                    for pos in range(0, len(payload), 997):
                        p.stdin.write(payload[pos:pos+997])
                    p.stdin.close()
                except Exception as e:
                    errors.append(e)
            t = threading.Thread(target=writer); t.start()
            output = p.stdout.read(); t.join(10)
            self.assertEqual(p.wait(timeout=10), 0, p.stderr.read())
            p.stdout.close(); p.stderr.close()
            self.assertFalse(errors)
            self.assertEqual(len(output), count*64800)
            self.assertTrue(all(b < 128 for b in output))
            return bytes(b >= 128 for b in output)
        self.assertEqual(run(3, True), run(3, False))
        run(1, False)
        truncated = subprocess.run([LDPC, '--framed', '--modcod', '4'],
                                   input=struct.pack('=I', 1)+b'x', capture_output=True, timeout=10)
        self.assertNotEqual(truncated.returncode, 0)
        self.assertIn(b'truncated', truncated.stderr)

    def test_ldpc_tables_match_independent_helper_tables(self):
        lean = (ROOT/'third_party/leansdr/src/leansdr/dvbs2_data.h').read_text()
        helper = (ROOT/'third_party/ldpc/dvb_s2_tables.hh').read_text()
        rates = ['14', '13', '25', '12', '35', '23', '34', '45', '56', '89', '910']
        for size, prefix in [('nf', 'B'), ('sf', 'C')]:
            for number, rate in enumerate(rates, 1):
                if size == 'sf' and rate == '910':
                    continue
                name = f'ldpc_{size}_fec{rate}'
                body = re.search(r'static const s2_ldpc_table '+name+r'\s*=\s*\{\s*\d+,\s*\d+,\s*\{(.*?)\}\s*\};', lean, re.S)[1]
                rows = re.findall(r'\{\s*(\d+),\s*\{([^{}]+)\}\}', body)
                positions, degrees = [], []
                for degree, row in rows:
                    values = list(map(int, re.findall(r'\d+', row)))
                    self.assertEqual(int(degree), len(values), name)
                    positions.extend(values); degrees.append(int(degree))
                ref = re.search(r'struct DVB_S2_TABLE_'+prefix+str(number)+r'\s*\{(.*?)\n\};', helper, re.S)[1]
                arrays = {}
                for label in ('POS', 'DEG', 'LEN'):
                    arrays[label] = list(map(int, re.findall(r'\d+', re.search(label+r'\[\] = \{(.*?)\}', ref, re.S)[1])))
                self.assertEqual(positions, arrays['POS'], name)
                self.assertEqual(degrees, [degree for degree, count in zip(arrays['DEG'], arrays['LEN']) for _ in range(count)], name)

    def test_satellite_telemetry(self):
        import math
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(('127.0.0.1', 0)); sock.settimeout(.1)
            env = os.environ.copy(); env['LINRAD_DECODER_MONITOR_PORT'] = str(sock.getsockname()[1])
            frames = []; stop = threading.Event()
            def reader():
                while not stop.is_set():
                    try:
                        frames.append(sock.recv(8192))
                    except socket.timeout:
                        pass
            t = threading.Thread(target=reader); t.start()
            try:
                self.receive(self.iq(), env=env)
            finally:
                stop.set(); t.join(2)
            statuses = [DecoderStatus.from_buffer_copy(f) for f in frames if len(f)==ctypes.sizeof(DecoderStatus)]
            self.assertTrue(any(s.satellite==2 and s.modcod==5 and s.constellation_order==4 and s.pilots==1 for s in statuses))
            self.assertTrue(any(math.isfinite(s.mer_db) and s.mer_db>10 and s.fec_good>0 for s in statuses))
            points = [f for f in frames if len(f)==12+768*8]
            self.assertTrue(points)
            values = struct.unpack_from('=IIIf', points[-1])
            self.assertEqual(values[:2], (0x4c444d31, 2))
            self.assertGreater(values[2], 0)
            self.assertLess(abs(values[3]), 4)

    def test_invalid_worker_options(self):
        for args in (['--nhelpers', '0'], ['--ldpc-batch', '33'], ['--ldpc-helper', '/missing/helper']):
            p = subprocess.run([RX, '--decoder-worker=leansdr', '--standard', 'DVB-S2',
                                '--s16', '-f', '500000', '--sr', '125000', '--modcods', '32',
                                '--sampler', 'rrc', *args], input=self.iq(), capture_output=True, timeout=10)
            self.assertNotEqual(p.returncode, 0, args)

    def test_fixed_qpsk_mode_does_not_report_rejected_8psk(self):
        # A real, clean 8PSK carrier must not change the displayed modulation or
        # reach FEC decoding when the receiver is fixed to QPSK 1/2 (MODCOD 4).
        iq = self.iq(modcod=14)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(('127.0.0.1', 0)); sock.settimeout(.1)
            env = dict(os.environ, LINRAD_DECODER_MONITOR_PORT=str(sock.getsockname()[1]))
            frames = []; stop = threading.Event()
            def reader():
                while not stop.is_set():
                    try:
                        frames.append(sock.recv(8192))
                    except socket.timeout:
                        pass
            t = threading.Thread(target=reader); t.start()
            try:
                p = subprocess.run([RX, '--decoder-worker=leansdr', '--standard', 'DVB-S2',
                                    '--s16', '-f', '500000', '--sr', '125000', '--modcods', '16',
                                    '--sampler', 'rrc', '--rrc-rej', '30', '--ldpc-helper', 'builtin'],
                                   input=iq, capture_output=True, env=env, timeout=30)
            finally:
                stop.set(); t.join(2)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b'')
        statuses = [DecoderStatus.from_buffer_copy(f) for f in frames if len(f)==ctypes.sizeof(DecoderStatus)]
        self.assertTrue(statuses)
        for s in statuses:
            self.assertIn(s.modcod, (-1, 4))
            self.assertEqual(s.constellation_order, 4)
            self.assertEqual(s.modulation, b'QPSK (MC 4)')
            self.assertEqual(s.fec, b'1/2')
            self.assertEqual(s.fec_good+s.fec_bad, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
