#!/usr/bin/env python3
from __future__ import annotations

import argparse
import select
import socket
import struct
import sys
import time
from typing import Optional

# AFE822x / AFEDRI protocol notes used here:
# - Single-channel mode selection is done with HID-over-TCP command 48.
# - AGC get/set are HID-over-TCP commands 71 / 69.
# - Frequency and RF gain use SDR-IP compatible TCP commands.
# - For AFEDRI-compatible RF gain coding, Linrad's afedri.c uses:
#       code = 8 * ((gain_db + 10) // 3) + 4    for gain_db <= 35
#       code = 124                               for gain_db > 35
#   with the practical RF-only range on AFE822x being -10..35 dB.


def clamp_int(x: int, lo: int, hi: int) -> int:
    return lo if x < lo else hi if x > hi else x


class Logger:
    def __init__(self, enabled: bool = True) -> None:
        self.enabled = enabled

    def log(self, *parts: object) -> None:
        if self.enabled:
            print(*parts, file=sys.stderr, flush=True)


LOG = Logger(True)


def calc_real_rate(main_clock_freq: int, requested_sample_rate: int) -> int:
    temp = main_clock_freq // requested_sample_rate
    temp = temp // 4
    if main_clock_freq > requested_sample_rate * (4 * temp + 2):
        temp += 1
    return main_clock_freq // (4 * temp)


def hexs(b: bytes) -> str:
    return b.hex(" ") if b else "<timeout>"


def xchg(sock: socket.socket, msg: bytes, label: str = "", pause_s: float = 0.10) -> bytes:
    sock.sendall(msg)
    time.sleep(pause_s)
    try:
        data = sock.recv(256)
    except socket.timeout:
        data = b""
    if label:
        LOG.log(f"[{label}]")
    LOG.log("TX:", hexs(msg))
    LOG.log("RX:", hexs(data))
    return data


def ack_ok(rsp: bytes, *, tcp_header_lsb: int, tcp_header_msb: int, report_id: int = 0x06, ack_cmd: int = 28, orig_cmd: Optional[int] = None) -> bool:
    if len(rsp) < 4:
        return False
    if rsp[0] != tcp_header_lsb or rsp[1] != tcp_header_msb:
        return False
    if rsp[2] != report_id or rsp[3] != ack_cmd:
        return False
    if orig_cmd is not None and (len(rsp) < 5 or rsp[4] != orig_cmd):
        return False
    return True


def req_main_clock(sock: socket.socket) -> Optional[int]:
    rsp = xchg(sock, bytes([0x04, 0x20, 0xB0, 0x00]), "Request main clock")
    if len(rsp) >= 9 and rsp[0] == 0x09 and rsp[2] == 0xB0:
        return int.from_bytes(rsp[5:9], "little")
    return None


def get_sdr_id_index(sock: socket.socket) -> Optional[int]:
    # HID-over-TCP: 0x09 0xE0 + [0x02, 51, 0,0,0,0,0]
    rsp = xchg(sock, bytes([0x09, 0xE0, 0x02, 51, 0x00, 0x00, 0x00, 0x00, 0x00]), "Get SDR ID index")
    if len(rsp) >= 5 and rsp[0] == 0x09 and rsp[1] == 0xE0 and rsp[2] == 0x06 and rsp[3] == 51:
        return rsp[4]
    return None


def set_single_channel_mode(sock: socket.socket, channel: int = 0) -> bytes:
    cmd = bytes([0x09, 0xE0, 0x02, 48, 0x00, channel & 0xFF, 0x00, 0x00, 0x00])
    return xchg(sock, cmd, f"Set single-channel mode (DDC {channel})")


def get_agc_state(sock: socket.socket) -> Optional[int]:
    # HID-over-TCP command 71
    rsp = xchg(sock, bytes([0x09, 0xE0, 0x02, 71, 0x00, 0x00, 0x00, 0x00, 0x00]), "Get AGC state")
    if len(rsp) >= 5 and rsp[0] == 0x09 and rsp[1] == 0xE0 and rsp[2] == 0x06 and rsp[3] == 71:
        return rsp[4]
    return None


def set_agc_state(sock: socket.socket, mask: int) -> bytes:
    # HID-over-TCP command 69
    cmd = bytes([0x09, 0xE0, 0x02, 69, mask & 0xFF, 0x00, 0x00, 0x00, 0x00])
    return xchg(sock, cmd, f"Set AGC state mask 0x{mask:02x}")


def agc_mask_for_channel(channel: int) -> int:
    # Doc bit0=DDC1 (channel 0), bit1=DDC2 (channel 2)
    return 0x01 if channel == 0 else 0x02


def describe_agc_mask(mask: int) -> str:
    states = []
    states.append(f"DDC1={'on' if (mask & 0x01) else 'off'}")
    states.append(f"DDC2={'on' if (mask & 0x02) else 'off'}")
    if mask & 0x04:
        states.append("DDC3=on")
    if mask & 0x08:
        states.append("DDC4=on")
    return ", ".join(states)


def set_sample_rate(sock: socket.socket, rate_hz: int) -> bytes:
    cmd = bytes([0x09, 0x00, 0xB8, 0x00, 0x00]) + struct.pack("<I", rate_hz)
    return xchg(sock, cmd, f"Set sample rate {rate_hz} Hz")


def set_packet_size_long(sock: socket.socket) -> bytes:
    cmd = bytes([0x05, 0x00, 0xC4, 0x00, 0x00])
    return xchg(sock, cmd, "Set long UDP packets")


def set_frequency(sock: socket.socket, freq_hz: int, channel: int = 0) -> bytes:
    cmd = bytes([0x0A, 0x00, 0x20, 0x00, channel & 0xFF]) + struct.pack("<I", freq_hz) + b"\x00"
    return xchg(sock, cmd, f"Set frequency {freq_hz} Hz on channel field {channel}")


def encode_afedri_rf_gain(gain_db: int) -> int:
    gain_db = clamp_int(gain_db, -10, 35)
    # Linrad afedri.c for net AFEDRI: 8*((gain+10)/3)+4 using integer division.
    return 8 * ((gain_db + 10) // 3) + 4


def set_rf_gain(sock: socket.socket, gain_db: int, channel: int = 0) -> bytes:
    gain_db = clamp_int(gain_db, -10, 35)
    gain_code = encode_afedri_rf_gain(gain_db)
    cmd = bytes([0x06, 0x00, 0x38, 0x00, channel & 0xFF, gain_code & 0xFF])
    return xchg(sock, cmd, f"Set RF gain {gain_db} dB (code 0x{gain_code:02x})")


def start_stream(sock: socket.socket) -> bytes:
    cmd = bytes([0x08, 0x00, 0x18, 0x00, 0x80, 0x02, 0x00, 0x01])
    return xchg(sock, cmd, "Start stream")


def stop_stream(sock: socket.socket) -> bytes:
    cmd = bytes([0x08, 0x00, 0x18, 0x00, 0x80, 0x01, 0x00, 0x01])
    return xchg(sock, cmd, "Stop stream")


def check_rsp_ok(label: str, rsp: bytes, kind: str) -> None:
    if rsp:
        return
    LOG.log(f"WARN: no response to {label} ({kind})")


def main() -> int:
    ap = argparse.ArgumentParser(description="AFEDRI/AFE822x control with corrected RF gain coding, AGC control, and detailed reply logging.")
    ap.add_argument("--ip", default="192.168.0.118")
    ap.add_argument("--port", type=int, default=50005)
    ap.add_argument("--freq", type=int, required=True, help="Center frequency in Hz")
    ap.add_argument("--rate", type=int, required=True, help="Requested sample rate in Hz")
    ap.add_argument("--gain", type=int, default=0, help="Requested RF gain in dB (-10..35)")
    ap.add_argument("--channel", type=int, default=0, choices=[0, 2], help="DDC to use in single-channel mode: 0=DDC1, 2=DDC2")
    ap.add_argument("--freq-channel-field", type=int, default=0, choices=[0, 2], help="Channel field used in SDR-IP frequency command; for single-channel mode the doc says 0")
    ap.add_argument("--gain-channel-field", type=int, default=0, choices=[0, 2], help="Channel field used in SDR-IP RF gain command; for single-channel mode the doc says 0")
    ap.add_argument("--agc", choices=["keep", "on", "off"], default="keep", help="Read AGC state, optionally force it on/off for the selected DDC")
    ap.add_argument("--stop-first", action="store_true", help="Send stop-stream before programming")
    ap.add_argument("--verbose", action="store_true", help="Keep verbose TX/RX logging enabled")
    args = ap.parse_args()

    LOG.enabled = True if args.verbose else True  # default on; keep easy to inspect

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.0)
    s.connect((args.ip, args.port))

    try:
        sdr_id = get_sdr_id_index(s)
        if sdr_id is not None:
            LOG.log(f"SDR ID index : {sdr_id}")

        main_clock = req_main_clock(s) or 76_800_000
        real_rate = calc_real_rate(main_clock, args.rate)

        agc_before = get_agc_state(s)
        if agc_before is not None:
            LOG.log(f"AGC before   : 0x{agc_before:02x} ({describe_agc_mask(agc_before)})")
        else:
            LOG.log("AGC before   : <unsupported or no response>")

        if args.stop_first:
            stop_stream(s)

        set_single_channel_mode(s, args.channel)
        set_sample_rate(s, args.rate)
        set_packet_size_long(s)
        set_frequency(s, args.freq, args.freq_channel_field)

        if args.agc != "keep":
            current = agc_before if agc_before is not None else 0x00
            bit = agc_mask_for_channel(args.channel)
            if args.agc == "on":
                desired = current | bit
            else:
                desired = current & ~bit
            set_agc_state(s, desired)
            time.sleep(0.1)

        agc_after = get_agc_state(s)
        if agc_after is not None:
            LOG.log(f"AGC active   : 0x{agc_after:02x} ({describe_agc_mask(agc_after)})")
        else:
            LOG.log("AGC active   : <unsupported or no response>")

        set_rf_gain(s, args.gain, args.gain_channel_field)

        time.sleep(0.2)
        start_stream(s)

        gain_code = encode_afedri_rf_gain(args.gain)
        print(f"Freq               : {args.freq} Hz", file=sys.stderr)
        print(f"Rate req           : {args.rate} Hz", file=sys.stderr)
        print(f"Rate actual        : {real_rate} Hz", file=sys.stderr)
        print(f"Single-channel DDC : {args.channel}", file=sys.stderr)
        print(f"Freq ch-field      : {args.freq_channel_field}", file=sys.stderr)
        print(f"Gain ch-field      : {args.gain_channel_field}", file=sys.stderr)
        print(f"RF gain req        : {clamp_int(args.gain, -10, 35)} dB", file=sys.stderr)
        print(f"RF gain code       : 0x{gain_code:02x}", file=sys.stderr)
        if agc_after is not None:
            print(f"AGC state          : 0x{agc_after:02x} ({describe_agc_mask(agc_after)})", file=sys.stderr)
        print("Type q + Enter to stop", file=sys.stderr)
        sys.stderr.flush()

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
