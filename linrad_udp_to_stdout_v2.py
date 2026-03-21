#!/usr/bin/env python3
import argparse
import math
import signal
import socket
import sys
from typing import Optional


class IQStats:
    def __init__(self, label: str, sample_kind: str) -> None:
        self.label = label
        self.sample_kind = sample_kind
        self.count = 0
        self.sum_i = 0.0
        self.sum_q = 0.0
        self.sumsq_i = 0.0
        self.sumsq_q = 0.0
        self.min_i: Optional[int] = None
        self.max_i: Optional[int] = None
        self.min_q: Optional[int] = None
        self.max_q: Optional[int] = None

    def update_iq_pairs(self, iq_iter) -> None:
        for iv, qv in iq_iter:
            self.count += 1
            self.sum_i += iv
            self.sum_q += qv
            self.sumsq_i += iv * iv
            self.sumsq_q += qv * qv

            self.min_i = iv if self.min_i is None or iv < self.min_i else self.min_i
            self.max_i = iv if self.max_i is None or iv > self.max_i else self.max_i
            self.min_q = qv if self.min_q is None or qv < self.min_q else self.min_q
            self.max_q = qv if self.max_q is None or qv > self.max_q else self.max_q

    def update_from_s16le_iq(self, data: bytes) -> None:
        n = len(data) & ~3
        if n < 4:
            return
        mv = memoryview(data)[:n]

        def gen():
            for i in range(0, n, 4):
                iv = (mv[i + 1] << 8) | mv[i]
                qv = (mv[i + 3] << 8) | mv[i + 2]
                if iv >= 0x8000:
                    iv -= 0x10000
                if qv >= 0x8000:
                    qv -= 0x10000
                yield iv, qv

        self.update_iq_pairs(gen())

    def update_from_u8_iq(self, data: bytes) -> None:
        n = len(data) & ~1
        if n < 2:
            return
        mv = memoryview(data)[:n]

        def gen():
            for i in range(0, n, 2):
                yield mv[i], mv[i + 1]

        self.update_iq_pairs(gen())

    def print_report(self) -> None:
        if self.count == 0:
            print(f"{self.label}: no complete IQ samples measured.", file=sys.stderr)
            return

        def fmt_axis(name: str, total: float, total_sq: float, mn: int, mx: int) -> None:
            mean = total / self.count
            rms = math.sqrt(total_sq / self.count)
            var = max(0.0, total_sq / self.count - mean * mean)
            std = math.sqrt(var)
            print(f"{name}:", file=sys.stderr)
            print(f"  mean = {mean:.3f}", file=sys.stderr)
            print(f"  std  = {std:.3f}", file=sys.stderr)
            print(f"  rms  = {rms:.3f}", file=sys.stderr)
            print(f"  min  = {mn}", file=sys.stderr)
            print(f"  max  = {mx}", file=sys.stderr)

        print(f"[{self.label}] type={self.sample_kind} iq_samples={self.count}", file=sys.stderr)
        fmt_axis("I", self.sum_i, self.sumsq_i, int(self.min_i), int(self.max_i))
        fmt_axis("Q", self.sum_q, self.sumsq_q, int(self.min_q), int(self.max_q))


def clip_u8(v: int) -> int:
    if v < 0:
        return 0
    if v > 255:
        return 255
    return v


def s16le_iq_to_u8(data: bytes, scale: float = 256.0, dc_offset: int = 128) -> bytes:
    n = len(data) & ~1
    if n == 0:
        return b""

    mv = memoryview(data)[:n]
    out = bytearray(n // 2)
    j = 0
    for i in range(0, n, 2):
        v = (mv[i + 1] << 8) | mv[i]
        if v >= 0x8000:
            v -= 0x10000
        uv = int(round(dc_offset + (v * scale) / 256.0))
        out[j] = clip_u8(uv)
        j += 1
    return bytes(out)


def gain_s16le(data: bytes, gain: float = 1.0) -> bytes:
    n = len(data) & ~1
    if n == 0:
        return b""
    if gain == 1.0:
        return data[:n]

    mv = memoryview(data)[:n]
    out = bytearray(n)
    j = 0
    for i in range(0, n, 2):
        v = (mv[i + 1] << 8) | mv[i]
        if v >= 0x8000:
            v -= 0x10000
        gv = int(round(v * gain))
        if gv < -32768:
            gv = -32768
        elif gv > 32767:
            gv = 32767
        if gv < 0:
            gv += 0x10000
        out[j] = gv & 0xFF
        out[j + 1] = (gv >> 8) & 0xFF
        j += 2
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Receive Linrad UDP raw s16 IQ and write payload to stdout, optionally scaled or converted to u8 IQ."
    )
    ap.add_argument("--ip", default="127.0.0.1", help="Listen IP (default: 127.0.0.1)")
    ap.add_argument("--port", type=int, default=50000, help="Listen UDP port (default: 50000)")
    ap.add_argument("--skip", type=int, default=24, help="Bytes to skip from start of each UDP packet (default: 24)")
    ap.add_argument("--bufsize", type=int, default=8192, help="UDP recv buffer size (default: 8192)")
    ap.add_argument("--verbose", action="store_true", help="Log packet stats to stderr")

    ap.add_argument("--u8", action="store_true", help="Convert interleaved s16 IQ to interleaved u8 IQ")
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
        help="Unsigned offset used for --u8 conversion (default: 128)",
    )
    ap.add_argument(
        "--s16-gain",
        type=float,
        default=1.0,
        help="Apply linear gain to s16 samples before writing them out (only used when --u8 is not set)",
    )
    ap.add_argument(
        "--measure",
        type=int,
        default=0,
        metavar="N",
        help="Measure first N input payload bytes and also first N output bytes, print I/Q stats to stderr, then continue streaming.",
    )
    args = ap.parse_args()

    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.ip, args.port))

    pkt_count = 0
    drop_count = 0
    in_byte_count = 0
    out_byte_count = 0

    in_stats = IQStats("INPUT", "s16") if args.measure > 0 else None
    out_stats = IQStats("OUTPUT", "u8" if args.u8 else "s16") if args.measure > 0 else None
    in_measured = 0
    out_measured = 0
    stats_reported = False

    while True:
        pkt, addr = sock.recvfrom(args.bufsize)
        pkt_count += 1

        if len(pkt) <= args.skip:
            drop_count += 1
            if args.verbose:
                print(f"drop short packet len={len(pkt)} from {addr}", file=sys.stderr, flush=True)
            continue

        payload = pkt[args.skip:]
        if len(payload) & 1:
            payload = payload[:-1]
        if not payload:
            continue

        in_byte_count += len(payload)

        if in_stats is not None and in_measured < args.measure:
            remaining = args.measure - in_measured
            chunk = payload[:remaining]
            if chunk:
                in_stats.update_from_s16le_iq(chunk)
                in_measured += len(chunk) & ~3

        if args.u8:
            out = s16le_iq_to_u8(payload, scale=args.scale, dc_offset=args.offset)
        else:
            out = gain_s16le(payload, gain=args.s16_gain)

        if out_stats is not None and out_measured < args.measure:
            remaining = args.measure - out_measured
            chunk = out[:remaining]
            if chunk:
                if args.u8:
                    out_stats.update_from_u8_iq(chunk)
                    out_measured += len(chunk) & ~1
                else:
                    out_stats.update_from_s16le_iq(chunk)
                    out_measured += len(chunk) & ~3

            if in_measured >= args.measure and out_measured >= args.measure and not stats_reported:
                in_stats.print_report()
                out_stats.print_report()
                stats_reported = True

        sys.stdout.buffer.write(out)
        out_byte_count += len(out)

        if args.verbose and (pkt_count % 1000 == 0):
            print(
                f"pkts={pkt_count} dropped={drop_count} in_bytes={in_byte_count} out_bytes={out_byte_count} last_len={len(pkt)}",
                file=sys.stderr,
                flush=True,
            )


if __name__ == "__main__":
    main()
