#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

SUPPORTED_RATES = [8138, 16276, 37793, 55556, 111111, 158730, 196078]
ATT_STEPS = [-20.0, -10.0, 0.0, 10.0]


def nearest_rate(rate: float) -> int:
    arr = np.array(SUPPORTED_RATES, dtype=float)
    return int(arr[np.argmin(np.abs(arr - rate))])


def quantize_att_gain(db: float) -> float:
    arr = np.array(ATT_STEPS, dtype=float)
    return float(arr[np.argmin(np.abs(arr - db))])


def cf32_to_iq_i16(c: np.ndarray) -> np.ndarray:
    i = np.clip(np.real(c), -1.0, 1.0)
    q = np.clip(np.imag(c), -1.0, 1.0)
    i16 = np.round(i * 32767.0).astype(np.int16)
    q16 = np.round(q * 32767.0).astype(np.int16)
    out = np.empty(i16.size * 2, dtype=np.int16)
    out[0::2] = i16
    out[1::2] = q16
    return out


def clip_u8_array(x: np.ndarray) -> np.ndarray:
    return np.clip(x, 0, 255).astype(np.uint8)


def i16_iq_to_u8(iq16: np.ndarray, scale: float = 256.0, dc_offset: int = 128) -> np.ndarray:
    """
    Convert interleaved int16 IQIQ... to interleaved uint8 IQIQ...
    using the same philosophy as the working UDP script:

        u8 = clip(round(dc_offset + (s16 * scale) / 256.0), 0, 255)

    Meaning:
      scale=256 -> u8 ~= offset + s16   (very hot, likely clipping)
      scale=128 -> half that
      scale=64  -> quarter that

    Note:
      Because SDR-IQ CF32 samples are first mapped to int16 via *32767,
      practical values may be much lower than 256.
    """
    if scale <= 0:
        raise ValueError("scale must be > 0")

    x = np.round(dc_offset + (iq16.astype(np.float32) * float(scale)) / 256.0)
    return clip_u8_array(x)


def normalize_device_arg(devarg: str) -> str:
    """
    Accept:
      --device /dev/ttyUSB0
      --device ttyUSB0
      --device "driver=rfspace,sdr-iq=/dev/ttyUSB0"
    Always returns a Soapy device string for SDR-IQ via rfspace.
    """
    devarg = (devarg or "").strip()

    if "=" in devarg and "," in devarg:
        return devarg

    if devarg == "":
        return "driver=rfspace,sdr-iq=/dev/ttyUSB0"

    if devarg.startswith("ttyUSB"):
        devarg = "/dev/" + devarg

    if devarg.startswith("/dev/"):
        return f"driver=rfspace,sdr-iq={devarg}"

    return "driver=rfspace,sdr-iq=/dev/ttyUSB0"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Stream SDR-IQ IQ to stdout for leandvb (--s16 or --u8)."
    )
    ap.add_argument(
        "--device",
        default="/dev/ttyUSB0",
        help='SDR-IQ device as "/dev/ttyUSB0" or "ttyUSB0" (default). Full Soapy string also allowed.',
    )
    ap.add_argument("-f", "--freq", type=float, required=True, help="Center frequency in Hz")
    ap.add_argument(
        "-r",
        "--rate",
        type=float,
        default=196078.0,
        help=f"Sample rate in sps (default 196078). Supported: {', '.join(map(str, SUPPORTED_RATES))}",
    )
    ap.add_argument(
        "-g",
        "--gain",
        type=float,
        default=None,
        help="ATT gain in dB (steps -20,-10,0,10). If omitted: leave device default.",
    )
    ap.add_argument(
        "--seconds",
        type=float,
        default=0.0,
        help="Optional duration in seconds. 0 = run until killed (default).",
    )
    ap.add_argument(
        "--buf_len",
        type=int,
        default=16384,
        help="Complex samples per read (default 16384)",
    )
    ap.add_argument("--progress", action="store_true", help="Status to stderr")

    fmt = ap.add_mutually_exclusive_group()
    fmt.add_argument(
        "--u8",
        action="store_true",
        help="Output interleaved unsigned 8-bit IQIQ... for leandvb --u8",
    )
    fmt.add_argument(
        "--s16",
        action="store_true",
        help="Output interleaved signed 16-bit IQIQ... for leandvb --s16 (default)",
    )

    ap.add_argument(
        "--scale",
        type=float,
        default=256.0,
        help="Scale for --u8 conversion. Formula: u8 = clip(offset + s16*scale/256). Default 256.",
    )
    ap.add_argument(
        "--offset",
        type=int,
        default=128,
        help="Unsigned DC offset for --u8 conversion (default: 128).",
    )

    args = ap.parse_args()

    fc = int(round(float(args.freq)))

    rate_req = float(args.rate)
    rate = nearest_rate(rate_req)
    if rate != int(rate_req):
        print(f"NOTE: requested rate {rate_req} not exact; using nearest supported {rate}.", file=sys.stderr)
    if rate not in SUPPORTED_RATES:
        raise SystemExit(f"Unsupported rate {rate}. Supported: {SUPPORTED_RATES}")

    devstr = normalize_device_arg(args.device)

    dev = SoapySDR.Device(devstr)
    dev.setSampleRate(SOAPY_SDR_RX, 0, float(rate))
    dev.setFrequency(SOAPY_SDR_RX, 0, float(fc))

    if args.gain is not None:
        g = quantize_att_gain(float(args.gain))
        if g != float(args.gain):
            print(f"NOTE: requested gain {args.gain} dB -> using nearest ATT step {g} dB.", file=sys.stderr)
        dev.setGain(SOAPY_SDR_RX, 0, "ATT", g)

    rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
    dev.activateStream(rx)

    buf_len = int(args.buf_len)
    if buf_len < 256:
        buf_len = 256
    cbuf = np.empty(buf_len, dtype=np.complex64)

    total = int(round(args.seconds * rate)) if args.seconds and args.seconds > 0 else 0
    got = 0
    last = time.time()

    output_u8 = bool(args.u8)
    output_desc = "stdout U8 IQIQ... (use leandvb --u8)" if output_u8 else "stdout CS16LE IQIQ... (use leandvb --s16)"

    if args.progress:
        print(f"SDR-IQ    : {dev.getHardwareKey()}  ({dev.getDriverKey()})", file=sys.stderr)
        print(f"Device    : {devstr}", file=sys.stderr)
        print(f"Freq      : {fc} Hz", file=sys.stderr)
        print(f"Rate      : {rate} sps", file=sys.stderr)
        if args.gain is not None:
            print(f"ATT gain  : {g} dB", file=sys.stderr)
        if output_u8:
            print(f"Scale     : {args.scale}", file=sys.stderr)
            print(f"Offset    : {args.offset}", file=sys.stderr)
        print(f"Output    : {output_desc}", file=sys.stderr)

    try:
        while True:
            if total and got >= total:
                break

            need = buf_len if not total else min(buf_len, total - got)
            sr_ret = dev.readStream(rx, [cbuf], need, timeoutUs=int(2e6))
            if sr_ret.ret < 0:
                raise RuntimeError(f"SoapySDR readStream error: {sr_ret.ret}")
            if sr_ret.ret == 0:
                continue

            k = sr_ret.ret
            iq16 = cf32_to_iq_i16(cbuf[:k])

            if output_u8:
                out_bytes = i16_iq_to_u8(iq16, args.scale, args.offset).tobytes()
                bytes_per_complex = 2
            else:
                out_bytes = iq16.tobytes()
                bytes_per_complex = 4

            try:
                sys.stdout.buffer.write(out_bytes)
            except BrokenPipeError:
                return 0

            got += k

            if args.progress:
                now = time.time()
                if now - last >= 0.5:
                    mb = (got * bytes_per_complex) / (1024 * 1024)
                    if total:
                        pct = 100.0 * got / total
                        print(
                            f"\r{pct:6.2f}%  wrote~{mb:8.2f} MB  samples={got}/{total}",
                            end="",
                            file=sys.stderr,
                        )
                    else:
                        print(
                            f"\rwrote~{mb:8.2f} MB  samples={got}",
                            end="",
                            file=sys.stderr,
                        )
                    last = now

        if args.progress:
            print(file=sys.stderr)

    finally:
        try:
            sys.stdout.buffer.flush()
        except Exception:
            pass
        try:
            dev.deactivateStream(rx)
        except Exception:
            pass
        try:
            dev.closeStream(rx)
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
