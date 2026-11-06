#!/usr/bin/env python3
"""Deterministic file-only DX stress fixtures from an existing s16 IQ recording.

Noise values refer to wideband sampled IQ power, NOT decoder-reported SNR.
The output manifest records source hash, noise, fading and multipath settings.
Requires NumPy. No SDR or transmitter is opened.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def generate(case, out, selected=None):
    source = Path(case['path'])
    raw = np.fromfile(source, dtype='<i2').reshape(-1, 2)
    z = (raw[:, 0].astype(np.float64) + 1j * raw[:, 1]) / 32768
    power = float(np.mean(abs(z)**2))
    rate = case['rate']
    rows = []
    scenarios = [('noise3', 3.0, False), ('noise0', 0.0, False),
                 ('noise-3', -3.0, False), ('fast-qsb-echo', 3.0, True),
                 ('light-doppler', 3.0, False), ('qsb-recovery', 3.0, True)]
    for name, noise_db, qsb in scenarios:
        if selected is not None and name not in selected:
            continue
        seed = 20261107
        rng = np.random.default_rng(seed)
        target = out/(case['name']+'-'+name+'.iq')
        clipped = 0
        with target.open('wb') as f:
            for start in range(0, len(z), 65536):
                end = min(start+65536, len(z))
                t = np.arange(start, end)/rate
                x = z[start:end].copy()
                if qsb:
                    # 4 Hz QSB: amplitude 0.15..1; short deep fades every 2 s.
                    gain = 0.15 + 0.85 * (0.5 + 0.5*np.cos(2*np.pi*4*t))
                    gain[np.mod(t, 2.0) < 0.08] *= 0.1
                    active = (t >= 8) & (t < 24) if name == 'qsb-recovery' else np.ones(len(t), dtype=bool)
                    gain[~active] = 1
                    # A 12 us delayed path rotating at 3 Hz plus carrier drift.
                    delay = max(1, round(rate*12e-6))
                    indices = np.arange(start, end)-delay
                    echo = z[np.maximum(indices, 0)].copy()
                    echo[indices < 0] = 0
                    x = (x + 0.35*active*echo*np.exp(1j*2*np.pi*3*t))*gain
                    drift = 0.25 if name == 'qsb-recovery' else 1.0
                    x *= np.exp(1j*2*np.pi*(2*t+0.5*drift*t*t))
                if name == 'light-doppler':
                    # Controlled aircraft-scatter-like stress, not a channel
                    # calibration: common offset sweeps -6..+6 Hz, while a
                    # weaker 12 us reflected path has an additional +8 Hz.
                    delay = max(1, round(rate*12e-6))
                    indices = np.arange(start, end)-delay
                    echo = z[np.maximum(indices, 0)].copy()
                    echo[indices < 0] = 0
                    x += 0.35*echo*np.exp(1j*2*np.pi*8*t)
                    duration = len(z)/rate
                    x *= np.exp(1j*2*np.pi*(-6*t+6*t*t/duration))
                sigma = np.sqrt(power / (10**(noise_db/10)) / 2)
                noise = rng.standard_normal((len(x), 2))
                x += sigma*(noise[:, 0]+1j*noise[:, 1])
                # Fixed attenuation prevents clipping; it also scales noise.
                y = np.rint(np.column_stack((x.real, x.imag))*32768*0.25)
                clipped += int(np.count_nonzero((y < -32768) | (y > 32767)))
                f.write(np.clip(y, -32768, 32767).astype('<i2').tobytes())
        assert clipped == 0, 'Clipping invalidates this impairment fixture'
        row = {k: v for k, v in case.items() if k not in
               ('duration_s', 'offset_s', 'sha256', 'expected_clean', 'ts_pattern', 'repeat')}
        row.update(name=target.stem, path=str(target.resolve()),
                   input_samples=len(z), nominal_duration_s=len(z)/rate,
                   sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                   source_path=str(source), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                   impairment=dict(seed=seed, noise_sample_snr_db=noise_db,
                       qsb_hz=4 if qsb else 0, amplitude_min=0.15 if qsb else 1,
                       deep_fade_duration_s=0.08 if qsb else 0,
                       deep_fade_period_s=2 if qsb else 0,
                       echo_delay_s=12e-6 if qsb else 0,
                       echo_amplitude=0.35 if qsb else 0,
                       echo_rotation_hz=3 if qsb else 0,
                       cfo_hz=2 if qsb else 0, drift_hz_per_s=1 if qsb else 0,
                       output_gain=0.25, clipped_components=clipped))
        if name == 'light-doppler':
            row['impairment'].update(cfo_start_hz=-6, cfo_end_hz=6,
                drift_hz_per_s=12/(len(z)/rate), echo_delay_s=12e-6,
                echo_amplitude=0.35, echo_rotation_hz=8)
        if name == 'qsb-recovery':
            row['impairment'].update(qsb_echo_interval_s=[8, 24], drift_hz_per_s=0.25)
        rows.append(row)
        print(target.stem, 'samples', len(z), 'clipped', clipped, flush=True)
    (out/'dx.json').write_text(json.dumps(rows, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('case')
    parser.add_argument('output', type=Path)
    parser.add_argument('--scenario', action='append', choices=('noise3', 'noise0', 'noise-3',
        'fast-qsb-echo', 'light-doppler', 'qsb-recovery'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    case = next(c for c in json.loads(args.manifest.read_text()) if c['name'] == args.case)
    if case['format'] != 's16':
        parser.error('requires s16 IQ')
    generate(case, args.output, args.scenario)


if __name__ == '__main__':
    main()
