#!/usr/bin/env python3
from __future__ import annotations

import argparse
import select
import socket
import struct
import sys
import time

def clamp_int(x: int, lo: int, hi: int) -> int:
    return lo if x < lo else hi if x > hi else x

def calc_real_rate(main_clock_freq: int, requested_sample_rate: int) -> int:
    temp = main_clock_freq // requested_sample_rate
    temp = temp // 4
    if main_clock_freq > requested_sample_rate * (4 * temp + 2):
        temp += 1
    return main_clock_freq // (4 * temp)

def xchg(sock: socket.socket, msg: bytes, label: str = "") -> bytes:
    sock.sendall(msg)
    time.sleep(0.1)
    try:
        data = sock.recv(256)
    except socket.timeout:
        data = b""
    if label:
        print(label, file=sys.stderr)
    print("TX:", msg.hex(" "), file=sys.stderr)
    print("RX:", data.hex(" ") if data else "<timeout>", file=sys.stderr)
    return data

def req_main_clock(sock: socket.socket) -> int | None:
    rsp = xchg(sock, bytes([0x04, 0x20, 0xB0, 0x00]), "Request main clock")
    if len(rsp) >= 9 and rsp[0] == 0x09 and rsp[2] == 0xB0:
        return int.from_bytes(rsp[5:9], "little")
    return None

def set_single_channel_mode(sock: socket.socket, channel: int = 0) -> bytes:
    cmd = bytes([0x09, 0xE0, 0x02, 48, 0x00, channel & 0xFF, 0x00, 0x00, 0x00])
    return xchg(sock, cmd, "Set single-channel mode")

def set_sample_rate(sock: socket.socket, rate_hz: int) -> bytes:
    cmd = bytes([0x09, 0x00, 0xB8, 0x00, 0x00]) + struct.pack("<I", rate_hz)
    return xchg(sock, cmd, "Set sample rate")

def set_packet_size_long(sock: socket.socket) -> bytes:
    cmd = bytes([0x05, 0x00, 0xC4, 0x00, 0x00])
    return xchg(sock, cmd, "Set long UDP packets")

def set_frequency(sock: socket.socket, freq_hz: int, channel: int = 0) -> bytes:
    cmd = bytes([0x0A, 0x00, 0x20, 0x00, channel & 0xFF]) + struct.pack("<I", freq_hz) + b"\x00"
    return xchg(sock, cmd, "Set frequency")

def set_rf_gain(sock: socket.socket, gain_db: int, channel: int = 0) -> bytes:
    gain_db = clamp_int(gain_db, -10, 35)
    cmd = bytes([0x06, 0x00, 0x38, 0x00, channel & 0xFF, gain_db & 0xFF])
    return xchg(sock, cmd, "Set RF gain")

def start_stream(sock: socket.socket) -> bytes:
    cmd = bytes([0x08, 0x00, 0x18, 0x00, 0x80, 0x02, 0x00, 0x01])
    return xchg(sock, cmd, "Start stream")

def stop_stream(sock: socket.socket) -> bytes:
    cmd = bytes([0x08, 0x00, 0x18, 0x00, 0x80, 0x01, 0x00, 0x01])
    return xchg(sock, cmd, "Stop stream")

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.0.118")
    ap.add_argument("--port", type=int, default=50005)
    ap.add_argument("--freq", type=int, required=True)
    ap.add_argument("--rate", type=int, required=True)
    ap.add_argument("--gain", type=int, default=0)
    ap.add_argument("--channel", type=int, default=0, choices=[0, 2])
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.0)
    s.connect((args.ip, args.port))

    try:
        main_clock = req_main_clock(s) or 76_800_000
        real_rate = calc_real_rate(main_clock, args.rate)

        set_single_channel_mode(s, args.channel)
        set_sample_rate(s, args.rate)
        set_packet_size_long(s)
        set_frequency(s, args.freq, args.channel)
        set_rf_gain(s, args.gain, args.channel)

        time.sleep(0.2)
        start_stream(s)

        print(f"Freq        : {args.freq} Hz", file=sys.stderr)
        print(f"Rate req    : {args.rate} Hz", file=sys.stderr)
        print(f"Rate actual : {real_rate} Hz", file=sys.stderr)
        print(f"RF gain     : {clamp_int(args.gain, -10, 35)} dB", file=sys.stderr)
        print("Type q + Enter to stop", file=sys.stderr)

        while True:
            rlist, _, _ = select.select([sys.stdin], [], [], 0.25)
            if sys.stdin in rlist:
                line = sys.stdin.readline()
                if line and line.strip().lower() == "q":
                    break

        stop_stream(s)
        return 0

    finally:
        s.close()

if __name__ == "__main__":
    raise SystemExit(main())
