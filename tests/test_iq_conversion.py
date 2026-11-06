"""Verify hardware IQ conversion, quantization and sample ordering."""
from pathlib import Path
import random
import struct
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ConversionTests(unittest.TestCase):
    def convert(self, fmt, data):
        return subprocess.run([ROOT / "build/iq_conversion_probe", fmt], input=data,
                              capture_output=True, check=True).stdout

    def test_hackrf_all_byte_values_and_block_boundaries(self):
        data = bytes(range(256))*33 + random.Random(42).randbytes(20000)
        # Signed two's-complement samples become offset-binary samples.
        expected = bytes(value + 128 for (value,) in struct.iter_unpack("b", data))
        self.assertEqual(self.convert("cs8", data), expected)

    def test_b210_all_signed_values(self):
        values = list(range(-32768, 32768))
        data = struct.pack("=65536h", *values)
        expected = bytes(i for i in range(256) for _ in range(256))
        self.assertEqual(self.convert("cs16", data), expected)

    def test_same_waveform_same_output_no_iq_swap_or_conjugation(self):
        # Asymmetric quadrature values reveal a swap, polarity inversion or clipping.
        values = [-128, 0, -64, 32, 0, 127, 64, -32, 127, -128, 0, 0]
        cs8 = struct.pack("=12b", *values)
        cs16 = struct.pack("=12h", *(v*256 for v in values))
        expected = bytes([0,128,64,160,128,255,192,96,255,0,128,128])
        self.assertEqual(self.convert("cs8", cs8), expected)
        self.assertEqual(self.convert("cs16", cs16), expected)

    def test_sdriq_float_conversion(self):
        # Float32 rounding, clipping and I/Q order as in the Python SDR-IQ source.
        import numpy as np
        values = np.array([-2, -1, -.5, -.25, 0, .25, .5, 1, 2, .0001] +
                          [random.Random(i).uniform(-1.2, 1.2) for i in range(10000)], dtype=np.float32)
        expected = np.round(np.clip(values, -1, 1) * 32767).astype(np.int16).tobytes()
        self.assertEqual(self.convert("cf32", values.tobytes()), expected)

    def test_s16_quantization_edges_and_dc_zero(self):
        values = [-32768,-32767,-257,-256,-255,-1,0,1,255,256,32766,32767]
        self.assertEqual(self.convert("cs16", struct.pack("=12h", *values)),
                         bytes([0,0,126,127,127,127,128,128,128,129,255,255]))

if __name__ == "__main__":
    unittest.main(verbosity=2)
