"""A stalled receiver must cause explicit source loss, never slower replay."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]


class SourceTests(unittest.TestCase):
    def replay(self,stall):
        data=bytes(range(256))*1000  # 0.64 seconds at 100k complex s16 samples/s.
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'input.iq';path.write_bytes(data)
            proc=subprocess.Popen([sys.executable,str(ROOT/'dev/fixed_iq_source.py'),
                '--in',str(path),'--sample-rate','100000','--input-format','s16',
                '--buffer-ms','50'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if stall:time.sleep(.3)
            output,stderr=proc.communicate(timeout=3)
            self.assertEqual(proc.returncode,0,stderr)
        report=json.loads(next(line[len('[fixed-iq] '):] for line in stderr.decode().splitlines() if line.startswith('[fixed-iq] ')))
        self.assertLessEqual(report['peak_buffer_samples'],report['buffer_capacity_samples'])
        self.assertEqual(report['input_samples'],report['sent_samples']+report['dropped_samples'])
        self.assertEqual(len(output),4*report['sent_samples'])
        self.assertLess(report['elapsed_s'],.85)
        # The kernel pipe adds capacity beyond the explicitly bounded FIFO;
        # its size varies by host. Require observable loss, not a fixed count.
        if stall:self.assertGreater(report['dropped_samples'],0)
        else:
            self.assertEqual(report['dropped_samples'],0)
            self.assertEqual(output,data)

    def test_normal_reader_is_byte_exact(self):self.replay(False)
    def test_stalled_reader_overflows_instead_of_slowing_clock(self):self.replay(True)


if __name__=='__main__':unittest.main(verbosity=2)
