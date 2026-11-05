#!/usr/bin/env python3
"""Make reproducible s16 DVB-T2 replay stress fixtures; never opens a radio."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iq', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sample-rate', type=int, default=10000000)
    p.add_argument('--noise-db', type=float, default=24, help='source mean power / added noise power, dB (not resulting SNR)')
    p.add_argument('--fade', action='store_true', help='deep fade at 4..6 seconds plus 0.5 Hz/s frequency drift')
    args = p.parse_args()
    if args.output.exists() or args.output.with_suffix('.json').exists(): p.error('output already exists')
    if args.sample_rate <= 0 or args.iq.stat().st_size % 4: p.error('invalid sample rate or partial s16 sample')
    raw = np.memmap(args.iq, dtype='<i2', mode='r').reshape(-1, 2)
    chunk = 262144
    power = sum(float(np.square(raw[i:i+chunk].astype(np.float64)/32768).sum()) for i in range(0, len(raw), chunk))/len(raw)
    sigma = np.sqrt(power / 10**(args.noise_db/10) / 2)
    rng = np.random.default_rng(20261105)
    clipped = 0; source_hash = hashlib.sha256(); output_hash = hashlib.sha256()
    with args.output.open('xb') as out:
        for start in range(0, len(raw), chunk):
            source = raw[start:start+chunk]
            source_hash.update(source.tobytes())
            z = source[:, 0].astype(np.float64)/32768 + 1j*source[:, 1].astype(np.float64)/32768
            if args.fade:
                t = (start + np.arange(len(z)))/args.sample_rate
                z *= np.where((t >= 4) & (t < 6), .08, 1.) * np.exp(1j*np.pi*.5*t*t)
            noise = rng.standard_normal((len(z), 2))*sigma
            y = np.rint((np.column_stack((z.real, z.imag)) + noise)*32768)
            clipped += int(np.count_nonzero((y < -32768) | (y > 32767)))
            data = np.clip(y, -32768, 32767).astype('<i2').tobytes()
            out.write(data); output_hash.update(data)
    result = dict(source=str(args.iq.resolve()), source_sha256=source_hash.hexdigest(),
        output_sha256=output_hash.hexdigest(), samples=len(raw), sample_rate_hz=args.sample_rate,
        format='cs16_le', seed=20261105, added_noise_db=args.noise_db, fade=args.fade,
        fade_interval_s=[4, 6] if args.fade else None, fade_amplitude=.08 if args.fade else None,
        drift_hz_per_s=.5 if args.fade else 0, clipped_components=clipped)
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
