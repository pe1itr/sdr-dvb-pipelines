#!/usr/bin/env python3
from __future__ import annotations

import argparse
import signal
import socket
import struct
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description="Receive AFEDRI UDP IQ and write raw s16 IQ payload to stdout.")
    ap.add_argument("--bind", default="0.0.0.0", help="Bind address")
    ap.add_argument("--port", type=int, default=50005, help="UDP port")
    ap.add_argument("--source-ip", default=None, help="Optional expected source IP")
    ap.add_argument("--stats-every", type=int, default=0, help="Print packet stats every N packets to stderr (0=off)")
    args = ap.parse_args()

    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))

    out = sys.stdout.buffer
    pkt_count = 0
    lost = 0
    last_seq = None

    try:
        while True:
            pkt, addr = sock.recvfrom(2048)

            if args.source_ip and addr[0] != args.source_ip:
                continue

            if len(pkt) != 1028:
                continue

            if pkt[0] != 0x04 or pkt[1] != 0x84:
                continue

            seq = struct.unpack("<H", pkt[2:4])[0]

            if last_seq is not None:
                expected = (last_seq + 1) & 0xFFFF
                if seq != expected:
                    delta = (seq - expected) & 0xFFFF
                    lost += delta

            last_seq = seq
            pkt_count += 1

            out.write(pkt[4:])  # strip 4-byte UDP/IQ header
            out.flush()

            if args.stats_every and (pkt_count % args.stats_every == 0):
                print(
                    f"packets={pkt_count} lost={lost} last_seq={last_seq}",
                    file=sys.stderr,
                    flush=True,
                )

    except KeyboardInterrupt:
        print(f"Stopped. packets={pkt_count} lost={lost}", file=sys.stderr)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
    
