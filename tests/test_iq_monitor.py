import math
import os
import socket
import struct
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class MonitorTests(unittest.TestCase):
    def test_frequency_power_and_unchanged_iq(self):
        for rate in (111111, 158730, 196078):
            with self.subTest(rate=rate), socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind(('127.0.0.1', 0))
                sock.settimeout(2)
                env = dict(os.environ, LINRAD_IQ_MONITOR_PORT=str(sock.getsockname()[1]))
                n = 2048 * 8
                # Positive complex frequency, amplitude 0.25 full scale.
                raw = b''.join(struct.pack('<hh', round(8192*math.cos(2*math.pi*123*i/2048)),
                                          round(8192*math.sin(2*math.pi*123*i/2048))) for i in range(n))
                out = subprocess.run([str(ROOT/'build/monitor_probe'), str(rate)], input=raw,
                                     stdout=subprocess.PIPE, check=True, env=env).stdout
                self.assertEqual(out, raw)
                frame = sock.recv(65535)
                magic, sequence, fs = struct.unpack_from('=IId', frame)
                self.assertEqual(magic, 0x49515331)
                self.assertEqual(sequence, 1)
                self.assertEqual(fs, rate)
                psd = struct.unpack_from('=2048f', frame, 16)
                self.assertEqual(max(range(2048), key=lambda k: psd[k]), 1024+123)
                integrated = sum(10**(p/10) for p in psd)*rate/2048
                self.assertAlmostEqual(10*math.log10(integrated), 20*math.log10(.25), delta=.01)

if __name__ == '__main__':
    unittest.main()
