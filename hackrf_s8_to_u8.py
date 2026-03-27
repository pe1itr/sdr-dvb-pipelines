#!/usr/bin/env python3
"""
Convert HackRF signed 8-bit interleaved IQ stream to unsigned 8-bit IQ stream
for use with LeanDVB --u8.

HackRF stream format:
    I0, Q0, I1, Q1, I2, Q2, ...   as signed int8 bytes

LeanDVB --u8 expects:
    I0, Q0, I1, Q1, I2, Q2, ...   as unsigned uint8 bytes

Conversion:
    u8 = s8 ^ 0x80
which is equivalent to:
    u8 = s8 + 128
for 8-bit two's-complement samples.

Usage examples:

    hackrf_transfer -r - -f 437000000 -s 1000000 -l 24 -g 20 \
      | python3 hackrf_s8_to_u8.py \
      | leandvb --u8 -f 1000000 --sr 125000 --standard DVB-S2 --fastlock

    hackrf_transfer -r - -f 437000000 -s 1000000 -l 24 -g 20 \
      | python3 hackrf_s8_to_u8.py --stats \
      | leandvb --u8 -f 1000000 --sr 125000 --standard DVB-S2 --fastlock

Optional:
    --iq-swap   swaps I and Q bytes per complex sample
"""

from __future__ import annotations

import argparse
import sys
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert HackRF signed 8-bit IQ stream to unsigned 8-bit IQ stream for LeanDVB --u8."
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=262144,
        help="Number of bytes to process per read (default: 262144).",
    )
    parser.add_argument(
        "--iq-swap",
        action="store_true",
        help="Swap I and Q within each IQ pair.",
    )
    parser.add_argument(
        "--stats",
        action="store_true",
        help="Print simple throughput statistics to stderr once per second.",
    )
    return parser.parse_args()


def process_chunk_inplace(buf: bytearray, iq_swap: bool) -> None:
    # Convert signed int8 byte representation to unsigned by flipping bit 7.
    # This works directly on the raw byte values.
    if iq_swap:
        n = len(buf) & ~1  # only full IQ pairs
        for i in range(0, n, 2):
            i_byte = buf[i] ^ 0x80
            q_byte = buf[i + 1] ^ 0x80
            buf[i] = q_byte
            buf[i + 1] = i_byte

        # If an odd byte ever appears, still convert it.
        if len(buf) & 1:
            buf[-1] ^= 0x80
    else:
        for i in range(len(buf)):
            buf[i] ^= 0x80


def main() -> int:
    args = parse_args()

    if args.chunk_size <= 0:
        print("Error: --chunk-size must be > 0", file=sys.stderr)
        return 2

    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    stderr = sys.stderr

    total_bytes = 0
    start_time = time.time()
    last_report = start_time
    last_bytes = 0

    try:
        while True:
            data = stdin.read(args.chunk_size)
            if not data:
                break

            buf = bytearray(data)
            process_chunk_inplace(buf, args.iq_swap)
            stdout.write(buf)

            total_bytes += len(buf)

            if args.stats:
                now = time.time()
                if now - last_report >= 1.0:
                    interval = now - last_report
                    delta = total_bytes - last_bytes
                    rate_bps = delta / interval
                    msps = rate_bps / 2.0 / 1e6  # 2 bytes per IQ sample
                    print(
                        f"[hackrf_s8_to_u8] processed={total_bytes} bytes  "
                        f"rate={rate_bps:.0f} B/s  complex_rate={msps:.3f} MS/s",
                        file=stderr,
                        flush=True,
                    )
                    last_report = now
                    last_bytes = total_bytes

        stdout.flush()

        if args.stats:
            elapsed = max(time.time() - start_time, 1e-9)
            avg_rate_bps = total_bytes / elapsed
            avg_msps = avg_rate_bps / 2.0 / 1e6
            print(
                f"[hackrf_s8_to_u8] done: processed={total_bytes} bytes in {elapsed:.3f} s  "
                f"avg_rate={avg_rate_bps:.0f} B/s  avg_complex_rate={avg_msps:.3f} MS/s",
                file=stderr,
                flush=True,
            )

        return 0

    except BrokenPipeError:
        # Downstream closed early; exit quietly.
        try:
            stdout.close()
        except Exception:
            pass
        return 0
    except KeyboardInterrupt:
        print("[hackrf_s8_to_u8] interrupted", file=stderr, flush=True)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

