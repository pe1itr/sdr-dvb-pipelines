import subprocess
import unittest
from pathlib import Path
import numpy as np

PROBE = Path(__file__).resolve().parents[1] / 'build/iq_shift_probe'


class ShiftTests(unittest.TestCase):
    def run_shift(self, iq, offset, width=0, block=2048, rate=192000, u8=False):
        p = subprocess.run([PROBE, str(rate), str(offset), str(width), str(block), str(int(u8))],
                           input=iq.tobytes(), capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(p.stdout), iq.nbytes//(2 if u8 else 1))
        return np.frombuffer(p.stdout, 'u1' if u8 else '<i2').reshape(-1, 2)

    @staticmethod
    def pack(z):
        return np.rint(np.column_stack((z.real, z.imag))).astype('<i2')

    @staticmethod
    def complex(iq):
        return iq[:, 0].astype(float) + 1j*iq[:, 1]

    def test_zero_offset_bit_exact_including_full_scale(self):
        iq = np.random.default_rng(92).integers(-32768, 32768, (20001, 2), dtype=np.int16)
        np.testing.assert_array_equal(self.run_shift(iq, 0, block=127), iq)

    def test_sign_amplitude_phase_and_arbitrary_chunk_boundaries(self):
        n = np.arange(50001)
        for offset in (50000, -50000, 1234.567):
            z = 9000*np.exp(2j*np.pi*(offset+7000)*n/192000)
            iq = self.pack(z)
            expected = self.complex(iq)*np.exp(-2j*np.pi*offset*n/192000)
            results = [self.run_shift(iq, offset, block=block) for block in (1, 127, 2048, 65536)]
            for out in results:
                self.assertLess(np.max(abs(self.complex(out)-expected)), .72)
                np.testing.assert_array_equal(out, results[0])

    def test_40khz_channel_preserved_and_dc_moves_outside_filter(self):
        # A multicarrier waveform across the useful channel plus a large DC
        # spur. Compare with the original modulation and known FIR delay.
        n = np.arange(70000)
        frequencies = np.arange(-17000, 18000, 1000)
        phases = np.random.default_rng(4).uniform(-np.pi, np.pi, len(frequencies))
        for offset in (50000, -50000):
            base = sum(500*np.exp(1j*(2*np.pi*f*n/192000+phase))
                       for f, phase in zip(frequencies, phases))
            iq = self.pack(base*np.exp(2j*np.pi*offset*n/192000) + 6000+4000j)
            actual = self.complex(self.run_shift(iq, offset, width=40000))[12000:]
            expected = base[12000-4096:len(n)-4096]
            evm = np.sqrt(np.mean(abs(actual-expected)**2)/np.mean(abs(expected)**2))
            self.assertLess(evm, .001)
            residual_spur = abs(np.mean((actual-expected)*np.exp(2j*np.pi*offset*n[12000:]/192000)))
            self.assertLess(residual_spur, 2)

    def test_no_clipping_before_filter_and_no_new_u8_dc_bias(self):
        n = np.arange(96000)
        for offset in (50000, -50000):
            # Both ADC components fit, but the complex magnitude exceeds s16.
            # A float path must avoid clipping the rotated spur before filtering.
            z = 24000+24000j + 3000*np.exp(2j*np.pi*(offset+7000)*n/192000)
            iq = self.pack(z)
            out = self.complex(self.run_shift(iq,offset,width=40000))[12000:]
            expected = 3000*np.exp(2j*np.pi*7000*(n[12000:]-4096)/192000)
            self.assertLess(np.sqrt(np.mean(abs(out-expected)**2)), 2)
            byte_iq = self.run_shift(iq,offset,width=40000,u8=True)[12000:].astype(float)-128
            np.testing.assert_allclose(byte_iq.mean(axis=0),[0,0],atol=.01)
            byte_z = self.complex(byte_iq)*256
            self.assertLess(np.sqrt(np.mean(abs(byte_z-expected)**2))/3000,.04)

    def test_invalid_offset_or_shifted_passband(self):
        for offset, width in ((0, 192000), (0, 333000), (96000, 0), (-96000, 0), (50000, 150000),
                              (50000, 168750), ('nan', 40000), ('inf', 0)):
            p = subprocess.run([PROBE, '192000', str(offset), str(width), '2048'], capture_output=True)
            self.assertEqual(p.returncode, 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
