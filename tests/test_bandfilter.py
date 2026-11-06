import subprocess
import unittest
from pathlib import Path
import numpy as np

PROBE = Path(__file__).resolve().parents[1] / 'build/bandfilter_probe'

class BandfilterTests(unittest.TestCase):
    def run_filter(self, raw, bw, rate=111111, block=2048, u8=False, change=()):
        p = subprocess.run([PROBE, str(rate), str(bw), str(int(u8)), str(block), *map(str, change)],
                           input=raw, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(p.stdout), len(raw))
        return p.stdout

    def tone(self, frequency, bw, block=2048, rate=111111, change=()):
        n = np.arange(40000)
        original = 12000*np.exp(2j*np.pi*frequency*n/rate)
        iq = np.column_stack((original.real, original.imag)).round().astype('<i2')
        raw = self.run_filter(iq.tobytes(), bw, rate=rate, block=block, change=change)
        out = np.frombuffer(raw, '<i2').reshape(-1, 2)
        actual = out[:, 0].astype(float) + 1j*out[:, 1]
        expected = 12000*np.exp(2j*np.pi*frequency*(n-4096)/rate)
        return actual[10000:], expected[10000:]

    def test_bypass_bit_exact_both_formats(self):
        raw = np.random.default_rng(12).integers(0, 256, 100004, dtype=np.uint8).tobytes()
        for u8 in (False, True):
            self.assertEqual(self.run_filter(raw, 0, block=127, u8=u8), raw)

    def test_passband_flat_group_delay_both_bandwidths(self):
        # Compare against a pure delay, not against the filter implementation.
        for width in (35000, 50000):
            for frequency in (-14000, -7000, 1000, 7000, 14000):
                with self.subTest(width=width, frequency=frequency):
                    actual, expected = self.tone(frequency, width)
                    response = np.mean(actual / expected)
                    self.assertAlmostEqual(abs(response), 1, delta=.001)
                    self.assertAlmostEqual(np.angle(response), 0, delta=.001)

    def test_sdriq_high_rates_passband_edges_and_delay(self):
        # Reported RF symptom above 111111 samples/s: verify the real streaming
        # filter at both higher native rates, including the user's saved width.
        for rate in (111111, 158730, 196078):
            for width in (35000, 40000, 50000, 60420):
                for fraction, magnitude in ((-.4, 1), (.4, 1), (.5, .5)):
                    with self.subTest(rate=rate, width=width, fraction=fraction):
                        actual, expected = self.tone(width*fraction, width, rate=rate)
                        response = np.mean(actual/expected)
                        self.assertAlmostEqual(abs(response), magnitude, delta=.001)
                        self.assertAlmostEqual(np.angle(response), 0, delta=.001)
                actual, _ = self.tone(width*.6, width, rate=rate)
                self.assertLess(np.sqrt(np.mean(abs(actual)**2)), 2)

    def test_noise_passband_not_domed_at_sdriq_rates(self):
        # Simulate the receiver's 2048-sample s16 reads in a 4096-point
        # Hann-windowed spectrum, as well as the source/filter stream.
        rng = np.random.default_rng(47)
        for rate in (111111, 158730, 196078):
            iq = np.rint(rng.normal(0, 1000, (2048*256+8192, 2))).astype('<i2')
            raw = self.run_filter(iq.tobytes(), 60420, rate=rate)
            out = np.frombuffer(raw, '<i2').reshape(-1, 2).astype(float)
            samples = (out[:, 0]+1j*out[:, 1])[8192:].reshape(-1, 2048)
            fft = np.fft.fftshift(np.fft.fft(samples*np.hanning(4096)[:2048], n=4096), axes=1)
            power = np.mean(abs(fft)**2, axis=0)
            frequency = abs(np.fft.fftshift(np.fft.fftfreq(4096, 1/rate)))
            center = np.mean(power[frequency < 8000])
            outer = np.mean(power[(frequency >= 18000) & (frequency < 24000)])
            self.assertLess(abs(10*np.log10(outer/center)), .4, rate)

    def test_stopband_and_iq_order(self):
        for frequency in (-35000, 35000):
            actual, _ = self.tone(frequency, 40000)
            self.assertLess(np.sqrt(np.mean(abs(actual)**2)), 2)  # >75 dB suppression

    def test_chunk_boundaries_and_live_width_change(self):
        for block in (127, 2048, 16384):
            actual, expected = self.tone(1000, 50000, block=block, change=(20000, 35000))
            self.assertLess(np.max(abs(actual-expected)), 4)

    def test_high_rate_narrow_filter(self):
        actual, expected = self.tone(1000, 40000, rate=10000000)
        self.assertLess(np.max(abs(actual-expected)), 5)

    def test_impulse_same_delay_i_and_q(self):
        iq = np.zeros((20000, 2), dtype='<i2')
        iq[0] = [32767, -32767]
        for width in (35000, 50000):
            out = np.frombuffer(self.run_filter(iq.tobytes(), width), '<i2').reshape(-1, 2)
            self.assertEqual(np.argmax(out[:, 0]), 4096)
            np.testing.assert_array_equal(out[:8193, 0], out[:8193, 0][::-1])
            np.testing.assert_array_equal(out[:, 0], -out[:, 1])

    def test_u8_filter_preserves_dc_center(self):
        raw = bytes([128, 128])*20000
        self.assertEqual(self.run_filter(raw, 40000, u8=True), raw)

    def test_20msps_strong_adjacent_signal(self):
        # DAB, broadcast DVB-T2 and narrowband DVB: the unwanted carrier is
        # 29 dB stronger, while their sum stays inside the ADC range.
        rate = 20000000
        n = np.arange(131072)
        for width in (150000, 1778112, 8000000):
            for sign in (-1, 1):
                with self.subTest(width=width, sign=sign):
                    wanted = 1000*np.exp(2j*np.pi*(width*.2)*n/rate)
                    blocker = 28000*np.exp(2j*np.pi*(sign*width*.65)*n/rate)
                    z = wanted + blocker
                    iq = np.rint(np.column_stack((z.real,z.imag))).astype('<i2')
                    raw = self.run_filter(iq.tobytes(),width,rate=rate,block=65536)
                    out = np.frombuffer(raw,'<i2').reshape(-1,2).astype(float)
                    actual = (out[:,0]+1j*out[:,1])[10000:]
                    expected = wanted[10000-4096:len(n)-4096]
                    self.assertLess(np.sqrt(np.mean(abs(actual-expected)**2)), 2)

    def test_enable_after_bypass_retains_history(self):
        # Enabling must use samples received during bypass, even when an input
        # block is larger than the FIR history or ends in a SIMD tail.
        rng = np.random.default_rng(983)
        for u8 in (False, True):
            iq = rng.integers(0,256,800000,dtype=np.uint8).tobytes()
            for block in (127, 8193, 65536):
                with self.subTest(u8=u8,block=block):
                    full = self.run_filter(iq,1778112,rate=20000000,block=block,u8=u8)
                    switched = self.run_filter(iq,0,rate=20000000,block=block,u8=u8,
                                               change=(70000,1778112))
                    start = ((70000+block-1)//block)*block*(2 if u8 else 4)
                    self.assertEqual(switched[:start],iq[:start])
                    dtype = 'u1' if u8 else '<i2'
                    a = np.frombuffer(full[start:],dtype).astype(float)
                    b = np.frombuffer(switched[start:],dtype).astype(float)
                    # FFTW may pick a different, equally accurate measured plan.
                    self.assertLessEqual(np.max(abs(a-b)), 1)

if __name__ == '__main__':
    unittest.main(verbosity=2)
