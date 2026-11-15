#!/usr/bin/env python3
"""Small isolated protocol checks; --gpu opts into actual hardware decoding.

Stop live reception before enabling --gpu or running throughput comparisons.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true')
    args = parser.parse_args()
    binary = ROOT / 'build/dvbs2_gpu_helper'
    base = [str(binary), '--framed', '--modcod', '14']

    def run(options, data=b'', env=None):
        return subprocess.run(options, input=data, capture_output=True,
                              timeout=30, env=env)

    def request(count, payload):
        return struct.pack('=I', count) + payload

    assert run([str(binary), '--help']).returncode == 0
    # Empty input and malformed requests must not initialize a GPU.
    for options, data, expected in [
        (base, b'', 0),
        (base, b'\x01', 1),
        (base, request(0, b''), 2),
        (base, request(33, b''), 2),
        (base, request(1, b'\x20'), 1),
        (base + ['--shortframes'], b'', 2),
        ([str(binary), '--framed', '--modcod', '4'], b'', 2),
    ]:
        result = run(options, data)
        assert result.returncode == expected, result.stderr
        assert not result.stdout
        assert b'GpuLdpc:' not in result.stderr

    # Zero is a valid codeword. Include a wrong-sign information bit and a
    # wrong-sign parity bit, then a short batch tail in the same process.
    clean = bytes([32]) * 64800
    damaged = bytearray(clean)
    damaged[123] = damaged[50000] = 224  # int8 -32
    stream = request(2, clean + damaged) + request(1, clean)
    reference = run(base + ['--backend', 'cpu-reference'], stream)
    assert reference.returncode == 0, reference.stderr
    assert reference.stdout == bytes([127]) * (3 * 64800)
    assert b'batches=2 frames=3 converged=3' in reference.stderr

    unavailable = run(base, request(1, clean), dict(os.environ, DECT2_NOGPU='1'))
    assert unavailable.returncode == 77, unavailable.stderr
    assert not unavailable.stdout  # Never silently substitutes CPU in GPU tests.
    if args.gpu:
        actual = run(base, stream)
        assert actual.returncode == 0, actual.stderr
        assert actual.stdout == reference.stdout
        assert b'batches=2 frames=3 converged=3' in actual.stderr
    print('S2 helper protocol, CPU reference and unavailable-GPU checks passed.' +
          (' Hardware GPU comparison passed.' if args.gpu else ' Hardware GPU not tested.'))


if __name__ == '__main__':
    main()
