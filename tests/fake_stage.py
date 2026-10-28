#!/usr/bin/env python3
"""Local pipe/UDP stand-in for lifecycle tests; no actual DVB/VLC is started."""
import os
from pathlib import Path
import select
import signal
import socket
import sys
import time

directory = Path(__file__).resolve().parent
role = Path(sys.argv[0]).name
(directory / (role + ".pid")).write_text(str(os.getpid()))
(directory / (role + ".cwd")).write_text(str(Path.cwd()))
if role == "decoder" and (directory / "stubborn").exists():
    pid = os.fork()
    if pid == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        (directory / "descendant.pid").write_text(str(os.getpid()))
        while True:
            signal.pause()

udp = None
if role == "player" and sys.argv[-1].startswith("udp://@"):
    host, port = sys.argv[-1][7:].rsplit(":", 1)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind((host, int(port)))
if role == "scatter":
    host, port = sys.argv[sys.argv.index("--udp-output") + 1].rsplit(":", 1)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    destination = (host, int(port))
(directory / (role + ".ready")).touch()
with (directory / (role + ".out")).open("wb", buffering=0) as output:
    while True:
        if role == "decoder" and (directory / "fail").exists():
            sys.exit(17)
        source = udp if role == "player" and udp else sys.stdin.buffer
        ready, _, _ = select.select([source], [], [], 0.05)
        if not ready:
            continue
        data = udp.recv(65535) if role == "player" and udp else os.read(0, 4096)
        if not data:
            sys.exit(0)
        output.write(data)
        if role == "decoder":
            os.write(1, data)
        elif role == "scatter":
            udp.sendto(data, destination)
