#!/usr/bin/env python3
"""Exercise the packaged native executables, including binary pipes and LDPC.

Run on the Linux cross-build host with Wine and the existing leandvbtx fixture.
No receiver hardware is opened.
"""
import configparser
import os
from pathlib import Path
import random
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(os.getenv("LINRAD_WINDOWS_PACKAGE", ROOT / "build/linrad_dvb-windows-x64")).resolve()
ENV = dict(os.environ, WINEDEBUG="-all", WINEPREFIX=str(ROOT / "build/wine-windows"))


def windows_path(path):
    return "Z:" + str(Path(path).resolve()).replace("/", "\\")


def run(name, *args, data=None, timeout=40):
    # A Unix pipe inherited through Wine is not a Windows named pipe and cannot
    # use SetNamedPipeHandleState. Test native inter-process pipes through the
    # application supervisor below; use disk handles at the Wine boundary.
    with tempfile.TemporaryFile() as source, tempfile.TemporaryFile() as output:
        if data:
            source.write(data)
        source.seek(0)
        p = subprocess.run(["wine", str(PACKAGE / (name + ".exe")), *map(str, args)],
                           stdin=source, stdout=output, stderr=subprocess.PIPE, timeout=timeout, env=ENV)
        output.seek(0)
        p.stdout = output.read()
        return p


class WindowsTests(unittest.TestCase):
    def test_thread_local_cleanup(self):
        p = run("windows_tls")
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_dvbt2_fec_and_l1(self):
        for name in ["test_dvbt2_fec", "test_dvbt2_l1"]:
            p = run(name, timeout=90)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_udp_input(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender, tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as log:
            sender.bind(("127.0.0.1", 0))
            # Reserve a separate receive port, then hand it to the executable.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            payload = bytes(range(256)) * 4
            p = subprocess.Popen(["wine", str(PACKAGE / "linrad_udp_to_stdout.exe"), "--ip", "127.0.0.1",
                                  "--port", str(port), "--skip", "24", "--record-stdout", "--measure", "256"],
                                 stdin=subprocess.DEVNULL, stdout=output, stderr=log, env=ENV)
            try:
                deadline = time.monotonic() + 40
                while p.poll() is None and time.monotonic() < deadline:
                    sender.sendto(bytes(24) + payload, ("127.0.0.1", port))
                    time.sleep(.05)
                    if os.fstat(output.fileno()).st_size >= len(payload):
                        break
                log.seek(0)
                self.assertIsNone(p.poll(), log.read().decode(errors="replace"))
                output.seek(0)
                self.assertEqual(output.read(len(payload)), payload)
            finally:
                if p.poll() is None:
                    p.kill(); p.wait()

    def test_help_and_pipeline(self):
        for program in ["linrad_dvb", "sdr_to_stdout", "iq_file_to_stdout", "linrad_udp_to_stdout", "ldpc_tool"]:
            with self.subTest(program=program):
                p = run(program, "--help")
                self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
        for standard in ["DVB-S", "DVB-S2", "DVB-T"]:
            p = run("linrad_dvb", "--print-pipeline", "--standard", standard)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
            self.assertIn(b"--decoder-worker=", p.stdout)

    def test_binary_replay(self):
        with tempfile.TemporaryDirectory(prefix="Windows IQ ", dir=ROOT / "build") as tmp:
            path = Path(tmp) / "all bytes.iq"
            data = bytes(range(256)) * 64
            path.write_bytes(b"header" + data)
            for fmt in ["s16", "u8"]:
                p = run("iq_file_to_stdout", "--in", windows_path(path), "--sample-rate", "100000",
                        "--input-format", fmt, "--header-bytes", "6")
                self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
                self.assertEqual(p.stdout, data)

    def test_dvbs2_and_pipeline(self):
        rng = random.Random(41)
        ts = b"".join(bytes([0x47, 1, 0, 0x10 + i % 16]) + struct.pack(">I", i) + rng.randbytes(180)
                      for i in range(1000))
        tx = subprocess.run([str(ROOT / "build/leandvbtx"), "--standard", "DVB-S2", "-f", "4",
                             "--rrc-rej", "30", "--s16", "--modcod", "4", "--pilots"],
                            input=ts, capture_output=True, check=True)
        p = run("linrad_dvb", "--decoder-worker=leansdr", "--standard", "DVB-S2", "--s16",
                "-f", "500000", "--sr", "125000", "--sampler", "rrc", "--rrc-rej", "30",
                "--modcods", "16", "--framesizes", "1", "--ldpc-helper", "builtin",
                "--nhelpers", "2", "--ldpc-batch", "2", data=tx.stdout, timeout=90)
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
        self.assertGreater(len(p.stdout), len(ts) * .65, p.stderr.decode(errors="replace"))
        self.assertEqual(len(p.stdout) % 188, 0)
        for offset in range(0, len(p.stdout), 188):
            packet = p.stdout[offset:offset+188]
            index = struct.unpack(">I", packet[4:8])[0]
            self.assertEqual(packet, ts[index*188:(index+1)*188])
        with tempfile.TemporaryDirectory(prefix="Windows pipeline ", dir=ROOT / "build") as tmp:
            tmp = Path(tmp)
            iq, output, config = tmp / "input.iq", tmp / "output.ts", tmp / "receiver.ini"
            iq.write_bytes(tx.stdout)
            settings = configparser.ConfigParser(interpolation=None)
            settings.read(PACKAGE / "linrad-dvb.example.ini")
            for key, executable in [("sdr", "sdr_to_stdout"), ("linrad", "linrad_udp_to_stdout"), ("iq_file", "iq_file_to_stdout")]:
                settings["paths"][key] = windows_path(PACKAGE / (executable + ".exe")).replace("\\", "/")
            settings["file"].update(path=windows_path(iq).replace("\\", "/"), sample_rate="500000", input_format="s16", header_bytes="0")
            settings["renderer"]["output_file"] = windows_path(output).replace("\\", "/")
            with config.open("w") as stream:
                settings.write(stream)
            p = run("linrad_dvb", "-c", windows_path(config), "--headless", "--source", "file",
                    "--standard", "DVB-S2", "--sr", "125000", "--fec", "1/2", "--renderer", "mpegts", timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
            self.assertGreater(output.stat().st_size, len(ts) * .65, p.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
