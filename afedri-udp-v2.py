#!/usr/bin/env python3
from __future__ import annotations

import argparse
import signal
import socket
import struct
import sys
import time


AFEDRI_PACKET_SIZE = 1028
AFEDRI_HEADER_SIZE = 4
AFEDRI_PAYLOAD_SIZE = AFEDRI_PACKET_SIZE - AFEDRI_HEADER_SIZE

MAGIC0 = 0x04
MAGIC1 = 0x84

DEFAULT_WRITEBUF_SIZE = 64 * 1024


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Receive AFEDRI UDP IQ and write raw s16 IQ payload to stdout."
    )

    ap.add_argument("--bind", default="0.0.0.0", help="Bind address")
    ap.add_argument("--port", type=int, default=50005, help="UDP port")
    ap.add_argument("--source-ip", default=None, help="Optional expected source IP")

    ap.add_argument(
        "--rcvbuf",
        type=int,
        default=4 * 1024 * 1024,
        help="Requested UDP receive buffer size in bytes",
    )

    ap.add_argument(
        "--writebuf",
        type=int,
        default=DEFAULT_WRITEBUF_SIZE,
        help="Stdout write buffer size in bytes",
    )

    ap.add_argument(
        "--stats-every",
        type=int,
        default=0,
        help="Print packet stats every N packets to stderr (0=off)",
    )

    ap.add_argument(
        "--insert-zero-on-loss",
        action="store_true",
        help="Insert zero IQ payloads for missing UDP packets to preserve stream timing",
    )

    ap.add_argument(
        "--max-zero-insert",
        type=int,
        default=50,
        help="Maximum missing packets to fill with zeros after one sequence jump",
    )

    return ap.parse_args()


def flush_writebuf(out, writebuf: bytearray) -> bool:
    if not writebuf:
        return True

    try:
        out.write(writebuf)
        writebuf.clear()
        return True
    except BrokenPipeError:
        return False


def append_output(out, writebuf: bytearray, data: bytes, limit: int) -> bool:
    writebuf.extend(data)

    if len(writebuf) >= limit:
        return flush_writebuf(out, writebuf)

    return True


def main() -> int:
    args = parse_args()

    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.rcvbuf)
    sock.bind((args.bind, args.port))

    actual_rcvbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)

    out = sys.stdout.buffer
    writebuf = bytearray()

    pkt_count = 0
    bad_len = 0
    bad_magic = 0
    wrong_source = 0
    lost = 0
    zero_inserted = 0
    stdout_writes = 0

    last_seq = None
    start_time = time.monotonic()
    last_report_time = start_time

    zero_payload = b"\x00" * AFEDRI_PAYLOAD_SIZE

    print(
        f"Listening on {args.bind}:{args.port}, "
        f"SO_RCVBUF={actual_rcvbuf}, writebuf={args.writebuf}",
        file=sys.stderr,
        flush=True,
    )

    try:
        while True:
            pkt, addr = sock.recvfrom(4096)

            if args.source_ip is not None and addr[0] != args.source_ip:
                wrong_source += 1
                continue

            if len(pkt) != AFEDRI_PACKET_SIZE:
                bad_len += 1
                continue

            if pkt[0] != MAGIC0 or pkt[1] != MAGIC1:
                bad_magic += 1
                continue

            seq = struct.unpack_from("<H", pkt, 2)[0]

            if last_seq is not None:
                expected = (last_seq + 1) & 0xFFFF
                if seq != expected:
                    delta = (seq - expected) & 0xFFFF

                    if delta > 0:
                        lost += delta

                        if args.insert_zero_on_loss:
                            fill = min(delta, args.max_zero_insert)
                            for _ in range(fill):
                                before = len(writebuf)
                                if not append_output(out, writebuf, zero_payload, args.writebuf):
                                    return 0
                                if before + AFEDRI_PAYLOAD_SIZE >= args.writebuf:
                                    stdout_writes += 1
                            zero_inserted += fill

            last_seq = seq
            pkt_count += 1

            before = len(writebuf)
            if not append_output(out, writebuf, pkt[AFEDRI_HEADER_SIZE:], args.writebuf):
                return 0
            if before + AFEDRI_PAYLOAD_SIZE >= args.writebuf:
                stdout_writes += 1

            if args.stats_every and (pkt_count % args.stats_every == 0):
                now = time.monotonic()
                elapsed = now - start_time
                interval = now - last_report_time
                last_report_time = now

                pps = pkt_count / elapsed if elapsed > 0 else 0.0

                print(
                    "packets={} lost={} zero_inserted={} bad_len={} bad_magic={} "
                    "wrong_source={} last_seq={} pps={:.1f} interval={:.2f}s "
                    "stdout_writes={} pending_out={}".format(
                        pkt_count,
                        lost,
                        zero_inserted,
                        bad_len,
                        bad_magic,
                        wrong_source,
                        last_seq,
                        pps,
                        interval,
                        stdout_writes,
                        len(writebuf),
                    ),
                    file=sys.stderr,
                    flush=True,
                )

    except KeyboardInterrupt:
        flush_writebuf(out, writebuf)

        elapsed = time.monotonic() - start_time
        pps = pkt_count / elapsed if elapsed > 0 else 0.0

        print(
            "Stopped. packets={} lost={} zero_inserted={} bad_len={} bad_magic={} "
            "wrong_source={} pps={:.1f} stdout_writes={}".format(
                pkt_count,
                lost,
                zero_inserted,
                bad_len,
                bad_magic,
                wrong_source,
                pps,
                stdout_writes,
            ),
            file=sys.stderr,
            flush=True,
        )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
