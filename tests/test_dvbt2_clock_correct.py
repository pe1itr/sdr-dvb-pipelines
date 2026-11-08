"""Compare runtime-dispatched clock correction with its generic fallback."""
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
results = []
for generic in ('1', '0'):
    result = subprocess.run([str(ROOT / 'build/test_dvbt2_clock_correct')],
        env=dict(os.environ, DECT2_CLOCK_GENERIC=generic), capture_output=True, text=True, check=True)
    print(result.stdout, end='')
    checksum = re.search(r'clock hash=([0-9a-f]+) samples=(\d+)', result.stdout)
    assert checksum, result.stdout
    results.append(checksum.groups())
assert results[0] == results[1], results
print('Clock correction: generic and runtime-selected paths are byte-identical')
