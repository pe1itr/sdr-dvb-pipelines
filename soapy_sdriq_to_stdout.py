#!/usr/bin/env python3
from __future__ import annotations

import argparse, os, sys, time
from datetime import datetime, timezone

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

def normalize_device_arg(devarg: str) -> str:
    """
    Accept:
      --device /dev/ttyUSB0
      --device ttyUSB0
      --device "driver=rfspace,sdr-iq=/dev/ttyUSB0"
    Always returns a Soapy device string for SDR-IQ via rfspace.
    """
    devarg = (devarg or "").strip()

    # already a full soapy string
    if "=" in devarg and "," in devarg:
        return devarg

    if devarg == "":
        return "driver=rfspace,sdr-iq=/dev/ttyUSB0"

    if devarg.startswith("ttyUSB"):
        devarg = "/dev/" + devarg

    if devarg.startswith("/dev/"):
        return f"driver=rfspace,sdr-iq={devarg}"

    # fallback
    return "driver=rfspace,sdr-iq=/dev/ttyUSB0"

def main() -> int:
    ap = argparse.ArgumentParser(description="Stream SDR-IQ IQ to stdout (CS16LE IQIQ...) for leandvb --s16.")
    ap.add_argument("--device", default="/dev/ttyUSB0",
                    help='SDR-IQ device as "/dev/ttyUSB0" or "ttyUSB0" (default). Full Soapy string also allowed.')
    ap.add_argument("-f", "--freq", type=float, required=True, help="Center frequency in Hz")
    ap.add_argument("-r", "--rate", type=float, default=196078.0,
                    help=f"Sample rate in sps (default 196078). Supported: {', '.join(map(str, SUPPORTED_RATES))}")
    ap.add_argument("-g", "--gain", type=float, default=None,
                    help="ATT gain in dB (steps -20,-10,0,10). If omitted: leave device default.")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="Optional duration in seconds. 0 = run until killed (default).")
    ap.add_argument("--buf_len", type=int, default=16384, help="Complex samples per read (default 16384)")
    ap.add_argument("--progress", action="store_true", help="Status to stderr")
    args = ap.parse_args()

    # Ensure downstream pipe close doesn't crash with traceback
    # (BrokenPipeError will be handled)
    fc = int(round(float(args.freq)))

    # Rate enforcement
    rate_req = float(args.rate)
    rate = nearest_rate(rate_req)
    if rate != int(rate_req):
        print(f"NOTE: requested rate {rate_req} not exact; using nearest supported {rate}.", file=sys.stderr)
    if rate not in SUPPORTED_RATES:
        raise SystemExit(f"Unsupported rate {rate}. Supported: {SUPPORTED_RATES}")

    devstr = normalize_device_arg(args.device)

    # Open device (force rfspace driver string to avoid SoapyRemote defaulting)
    dev = SoapySDR.Device(devstr)

    dev.setSampleRate(SOAPY_SDR_RX, 0, float(rate))
    dev.setFrequency(SOAPY_SDR_RX, 0, float(fc))

    if args.gain is not None:
        g = quantize_att_gain(float(args.gain))
        if g != float(args.gain):
            print(f"NOTE: requested gain {args.gain} dB -> using nearest ATT step {g} dB.", file=sys.stderr)
        # SDR-IQ via rfspace typically exposes "ATT"
        dev.setGain(SOAPY_SDR_RX, 0, "ATT", g)

    # Stream as CF32 and convert to int16 IQIQ...
    rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
    dev.activateStream(rx)

    buf_len = int(args.buf_len)
    if buf_len < 256:
        buf_len = 256
    cbuf = np.empty(buf_len, dtype=np.complex64)

    out_fd = sys.stdout.fileno()

    total = int(round(args.seconds * rate)) if args.seconds and args.seconds > 0 else 0
    got = 0
    last = time.time()

    if args.progress:
        print(f"SDR-IQ    : {dev.getHardwareKey()}  ({dev.getDriverKey()})", file=sys.stderr)
        print(f"Device    : {devstr}", file=sys.stderr)
        print(f"Freq      : {fc} Hz", file=sys.stderr)
        print(f"Rate      : {rate} sps", file=sys.stderr)
        if args.gain is not None:
            print(f"ATT gain  : {g} dB", file=sys.stderr)
        print("Output    : stdout CS16LE IQIQ... (use leandvb --s16)", file=sys.stderr)

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
            b = iq16.tobytes()

            try:
                os.write(out_fd, b)
            except BrokenPipeError:
                # downstream closed (e.g. leandvb stopped)
                return 0

            got += k

            if args.progress:
                now = time.time()
                if now - last >= 0.5:
                    mb = (got * 4) / (1024 * 1024)  # 4 bytes per complex sample output
                    if total:
                        pct = 100.0 * got / total
                        print(f"\r{pct:6.2f}%  wrote~{mb:8.2f} MB  samples={got}/{total}",
                              end="", file=sys.stderr)
                    else:
                        print(f"\rwrote~{mb:8.2f} MB  samples={got}",
                              end="", file=sys.stderr)
                    last = now

        if args.progress:
            print(file=sys.stderr)

    finally:
        try: dev.deactivateStream(rx)
        except Exception: pass
        try: dev.closeStream(rx)
        except Exception: pass

    return 0

if __name__ == "__main__":
    raise SystemExit(main())

