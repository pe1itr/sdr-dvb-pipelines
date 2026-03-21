#!/usr/bin/env python3
import sys
import math
import argparse
import numpy as np


def db10(x: float) -> float:
    if x <= 0:
        return float("-inf")
    return 10.0 * math.log10(x)

def bw_normalized_snr_db(snr_db: float, from_bw_hz: float, to_bw_hz: float) -> float:
    return snr_db + 10.0 * math.log10(from_bw_hz / to_bw_hz)

def parse_args():
    p = argparse.ArgumentParser(
        description=(
            "Estimate SNR from complex s16 IQ on stdin by comparing the central "
            "signal band around DC to noise measured in outer bands."
        )
    )
    p.add_argument("--fs", type=float, required=True,
                   help="Sample rate in Hz, e.g. 1010526")
    p.add_argument("--signal-bw", type=float, required=True,
                   help="Estimated occupied signal bandwidth around DC in Hz, e.g. 170000")
    p.add_argument("--exclude-bw", type=float, required=True,
                   help="Central exclusion bandwidth for noise estimation in Hz, e.g. 300000")
    p.add_argument("--fft-size", type=int, default=8192,
                   help="FFT size, default 8192")
    p.add_argument("--avg-blocks", type=int, default=64,
                   help="Number of FFT blocks to average, default 64")
    p.add_argument("--raw-power", action="store_true",
                   help="Also print raw signal/noise powers")
    p.add_argument("--pass-through", action="store_true",
                   help="Pass original IQ bytes to stdout unchanged")
    p.add_argument("--ref-bw", type=float, default=2500.0,
               help="Reference bandwidth in Hz for normalized SNR output, default 2500")
    return p.parse_args()


def main():
    args = parse_args()

    if args.fft_size <= 0 or args.avg_blocks <= 0:
        print("fft-size and avg-blocks must be > 0", file=sys.stderr)
        sys.exit(1)

    if args.signal_bw <= 0 or args.exclude_bw <= 0 or args.fs <= 0:
        print("fs, signal-bw and exclude-bw must be > 0", file=sys.stderr)
        sys.exit(1)

    if args.signal_bw >= args.exclude_bw:
        print("signal-bw must be smaller than exclude-bw", file=sys.stderr)
        sys.exit(1)

    if args.exclude_bw >= args.fs:
        print("exclude-bw must be smaller than fs", file=sys.stderr)
        sys.exit(1)

    samples_per_block = args.fft_size
    iq_bytes_per_block = samples_per_block * 4  # s16 I + s16 Q
    total_bytes = iq_bytes_per_block * args.avg_blocks

    raw = sys.stdin.buffer.read(total_bytes)
    if args.pass_through:
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()

    if len(raw) < iq_bytes_per_block:
        print("Not enough input data for one FFT block.", file=sys.stderr)
        sys.exit(1)

    usable_blocks = len(raw) // iq_bytes_per_block
    usable_bytes = usable_blocks * iq_bytes_per_block
    raw = raw[:usable_bytes]

    data_i16 = np.frombuffer(raw, dtype="<i2")
    if len(data_i16) % 2 != 0:
        data_i16 = data_i16[:-1]

    i = data_i16[0::2].astype(np.float32)
    q = data_i16[1::2].astype(np.float32)
    z = i + 1j * q

    z = z[:usable_blocks * samples_per_block]
    z = z.reshape((usable_blocks, samples_per_block))

    window = np.hanning(samples_per_block).astype(np.float32)
    win_power = np.mean(window * window)

    acc_psd = np.zeros(samples_per_block, dtype=np.float64)

    for blk in z:
        spec = np.fft.fftshift(np.fft.fft(blk * window))
        psd = (np.abs(spec) ** 2) / (samples_per_block * win_power)
        acc_psd += psd

    avg_psd = acc_psd / usable_blocks

    freqs = np.fft.fftshift(np.fft.fftfreq(samples_per_block, d=1.0 / args.fs))

    signal_mask = np.abs(freqs) <= (args.signal_bw / 2.0)
    noise_mask = np.abs(freqs) >= (args.exclude_bw / 2.0)

    n_signal_bins = int(np.count_nonzero(signal_mask))
    n_noise_bins = int(np.count_nonzero(noise_mask))

    if n_signal_bins == 0 or n_noise_bins == 0:
        print("No signal or noise bins left. Adjust bandwidth settings.", file=sys.stderr)
        sys.exit(1)

    signal_plus_noise_power = float(np.sum(avg_psd[signal_mask]))

    noise_density = float(np.mean(avg_psd[noise_mask]))
    estimated_noise_in_signal_band = noise_density * n_signal_bins

    estimated_signal_power = signal_plus_noise_power - estimated_noise_in_signal_band

    cnr_db = db10(signal_plus_noise_power / estimated_noise_in_signal_band) \
        if estimated_noise_in_signal_band > 0 else float("-inf")

    if estimated_signal_power > 0 and estimated_noise_in_signal_band > 0:
        snr_db = db10(estimated_signal_power / estimated_noise_in_signal_band)
    else:
        snr_db = float("-inf")
    
    eq_snr_ref_db = float("-inf")
    if math.isfinite(snr_db):
        eq_snr_ref_db = bw_normalized_snr_db(snr_db, args.signal_bw, args.ref_bw)


    print(f"Blocks averaged        : {usable_blocks}")
    print(f"FFT size               : {samples_per_block}")
    print(f"Sample rate            : {args.fs:.0f} Hz")
    print(f"Signal bandwidth       : {args.signal_bw:.0f} Hz")
    print(f"Noise exclude bandwidth: {args.exclude_bw:.0f} Hz")
    print(f"Signal bins            : {n_signal_bins}")
    print(f"Noise bins             : {n_noise_bins}")
    if args.raw_power:
        print(f"Signal+Noise power     : {signal_plus_noise_power:.6g}")
        print(f"Noise density/bin      : {noise_density:.6g}")
        print(f"Noise in signal band   : {estimated_noise_in_signal_band:.6g}")
        print(f"Estimated signal power : {estimated_signal_power:.6g}")
    print(f"C+N / N                : {cnr_db:.2f} dB")
    print(f"SNR                    : {snr_db:.2f} dB")
    print(f"Eq. SNR @ {args.ref_bw:.0f} Hz      : {eq_snr_ref_db:.2f} dB")

if __name__ == "__main__":
    main()

