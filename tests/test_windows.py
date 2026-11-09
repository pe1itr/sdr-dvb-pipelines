#!/usr/bin/env python3
"""Exercise the packaged native executables, including binary pipes and LDPC.

Run on the Linux cross-build host with Wine and the existing leandvbtx fixture.
No receiver hardware is opened.
"""
import configparser
import json
import os
from pathlib import Path
import random
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(os.getenv("LINRAD_WINDOWS_PACKAGE", ROOT / "build/linrad_dvb-windows-x64")).resolve()
ENV = dict(os.environ, LINRAD_DIAGNOSTIC_LOG="1", WINEDEBUG="-all", WINEPREFIX=str(ROOT / "build/wine-windows"))


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
    def test_iq_output_backpressure(self):
        for name in ("test_iq_output", "test_iq_output_backpressure"):
            with self.subTest(program=name):
                p = run(name, timeout=15)
                self.assertEqual(p.returncode, 0, (p.stderr+p.stdout).decode(errors="replace"))

    def test_sdr_receive_diagnostics(self):
        for name, args in [("test_sdr_rx_status", []), ("test_iq_monitor_async", []),
                           ("windows_gui_smoke", ["--test-sdr-status"])]:
            with self.subTest(program=name):
                p = run(name, *args)
                self.assertEqual(p.returncode, 0, (p.stderr+p.stdout).decode(errors="replace"))

    def test_dvbt2_ldpc(self):
        p = run("test_dvbt2_ldpc", timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace") + p.stdout.decode(errors="replace"))
        self.assertIn(b"1008 cases identical", p.stdout)

    def test_dvbt2_clock_correct(self):
        checksums = []
        for generic in ("1", "0"):
            with patch.dict(ENV, {"DECT2_CLOCK_GENERIC": generic}):
                p = run("test_dvbt2_clock_correct")
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
            line = next(line for line in p.stdout.splitlines() if line.startswith(b"clock hash="))
            checksums.append(line)
        self.assertEqual(checksums[0], checksums[1])

    def test_dvbt2_stage_profile(self):
        p = run("test_dvbt2_stage_prof")
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace") + p.stdout.decode(errors="replace"))

    def test_dvbt2_gap_clock(self):
        p = run("test_dvbt2_gap_clock", timeout=40)
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace") + p.stdout.decode(errors="replace"))

    def test_dvbt2_pipeline(self):
        p = run("test_dvbt2_pipeline", timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_default_config_created_once_and_preserved(self):
        personal_b210 = (PACKAGE / "uhd-images/kintex7").exists()
        with tempfile.TemporaryDirectory(prefix="Windows config é ", dir=ROOT / "build") as tmp:
            isolated = Path(tmp)
            for source in PACKAGE.iterdir():
                if source.suffix.lower() in (".exe", ".dll"):
                    os.link(source, isolated / source.name)
            example = isolated / "linrad-dvb.example.ini"
            shutil.copy2(PACKAGE / example.name, example)
            config = isolated / "linrad-dvb.ini"
            with patch(__name__ + ".PACKAGE", isolated):
                p = run("linrad_dvb", "--source", "b210", "--standard", "DVB-T2", "--print-pipeline")
                self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
                self.assertEqual(config.read_bytes(), example.read_bytes())
                settings = configparser.ConfigParser(interpolation=None)
                settings.read(config)
                if personal_b210:
                    self.assertEqual(settings["b210"]["args"], "type=b200")
                settings["b210"]["args"] = "type=b200,serial=keep-me"
                settings["status"].update(enabled="true", bind="0.0.0.0", port="8877")
                with config.open("w") as f:
                    settings.write(f)
                saved = config.read_bytes()
                example.write_text("[changed-template]\nvalue=must-not-replace-settings\n")
                p = run("linrad_dvb", "--source", "b210", "--standard", "DVB-T2", "--print-pipeline")
                self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
                self.assertIn(b"type=b200,serial=keep-me", p.stdout)
                self.assertEqual(config.read_bytes(), saved)
                missing = isolated / "explicit-missing.ini"
                p = run("linrad_dvb", "-c", windows_path(missing), "--print-pipeline")
                self.assertNotEqual(p.returncode, 0)
                self.assertFalse(missing.exists())

    def test_snapshot_contention(self):
        with tempfile.TemporaryDirectory(prefix="Windows snapshot é 日 ", dir=ROOT / "build") as tmp:
            p = run("windows_snapshot", windows_path(tmp))
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_pluto_backend(self):
        p = run("test_pluto_rx")
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_lime_backend(self):
        p = run("test_lime_rx")
        self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_dab_input_profiles(self):
        with tempfile.TemporaryDirectory(prefix="Windows DAB profiles ", dir=ROOT / "build") as tmp:
            for source, rate in (("pluto", 4096000), ("b210", 4096000), ("rtlsdr", 2048000), ("hackrf", 8000000)):
                with self.subTest(source=source):
                    cfg = configparser.ConfigParser(interpolation=None)
                    cfg.read(PACKAGE / "linrad-dvb.example.ini")
                    cfg[source].update(sample_rate=str(rate), receiver_offset="0", lo_offset="0", filter_enabled="false")
                    cfg["dab"].update(channel="11C", profile="weak")
                    config = Path(tmp) / "receiver.ini"
                    with config.open("w") as f:
                        cfg.write(f)
                    p = run("linrad_dvb", "-c", windows_path(config), "--source", source,
                            "--standard", "DAB", "--print-pipeline")
                    self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
                    self.assertIn(b"dab_worker.exe", p.stdout)
                    self.assertIn(b"'--channel' '11C'", p.stdout)
                    self.assertEqual(p.stdout.count(f"'--sample-rate' '{rate}'".encode()), 2)

    @unittest.skipUnless(os.getenv("LINRAD_WINDOWS_GUI_TEST"), "Set LINRAD_WINDOWS_GUI_TEST=1 for audio checks")
    def test_media_plugins_and_audio(self):
        with tempfile.TemporaryDirectory(prefix="Windows media ", dir=ROOT / "build") as tmp:
            p = run("windows_media", windows_path(tmp), timeout=90)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    @unittest.skipUnless(os.getenv("LINRAD_WINDOWS_GUI_TEST"), "Set LINRAD_WINDOWS_GUI_TEST=1 for GUI checks")
    def test_dab_gui_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="Windows DAB GUI ", dir=ROOT / "build") as tmp:
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(PACKAGE / "linrad-dvb.example.ini")
            cfg["paths"]["sdr"] = windows_path(PACKAGE / "dab_fake_source.exe").replace("\\", "/")
            cfg["paths"]["dab"] = windows_path(PACKAGE / "dab_worker.exe").replace("\\", "/")
            cfg["b210"]["sample_rate"] = "4096000"
            config = Path(tmp) / "receiver.ini"
            with config.open("w") as f:
                cfg.write(f)
            p = run("dab_gui_smoke", windows_path(config), timeout=90)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
            self.assertNotIn(b"CRITICAL", p.stderr)

    def test_dab_core(self):
        for name in "dx_msc dx_pad stream_acquisition libdxdabsdr_tii_synthetic fic_identity_accumulator channel_filter mot_slideshow dynamic_label dab_plus_radio".split():
            with self.subTest(name=name):
                p = run("dab_test_" + name, timeout=180)
                self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))

    def test_dab_worker_and_pipeline(self):
        with tempfile.TemporaryDirectory(prefix="Windows DAB é 日 ", dir=ROOT / "build") as tmp:
            tmp = Path(tmp)
            iq = tmp / "silence.iq"
            iq.write_bytes(bytes(196608 * 4 * 3))
            p = run("dab_worker", "--state-dir", windows_path(tmp), "--frequency", "5700000123",
                    "--no-audio", data=iq.read_bytes(), timeout=90)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
            state = json.loads((tmp / "state.json").read_text())
            self.assertEqual(state["radio_available"], 1)
            self.assertEqual(state["frequency"], 5700000123)
            self.assertEqual(state["input_samples"], 196608 * 3)
            self.assertFalse(state["error"])
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(PACKAGE / "linrad-dvb.example.ini")
            cfg["file"].update(path=windows_path(iq).replace("\\", "/"), sample_rate="2048000", input_format="s16", header_bytes="0")
            for key, name in [("iq_file", "iq_file_to_stdout"), ("dab", "dab_worker")]:
                cfg["paths"][key] = windows_path(PACKAGE / (name + ".exe")).replace("\\", "/")
            cfg["dab"]["channel"] = "9C"
            config = tmp / "receiver.ini"
            with config.open("w") as f:
                cfg.write(f)
            for mode in ("--print-pipeline", "--headless"):
                p = run("linrad_dvb", "-c", windows_path(config), "--source", "file",
                        "--standard", "DAB", mode, timeout=90)
                self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
                if mode == "--print-pipeline":
                    self.assertIn(b"dab_worker.exe", p.stdout)

    def test_dab_recorded_audio(self):
        directory = Path(os.environ.get("DAB_FIXTURE_DIR", ROOT / "tests/fixtures/dab"))
        fixture = directory / "live-tii-phase-11c-4s.s16le.iq"
        if not fixture.exists():
            self.skipTest("Set DAB_FIXTURE_DIR to the recorded DAB fixtures")
        with tempfile.TemporaryDirectory(prefix="Windows DAB audio ", dir=ROOT / "build") as tmp:
            p = run("dab_worker", "--state-dir", windows_path(tmp), "--channel", "11C",
                    "--no-audio", data=fixture.read_bytes(), timeout=180)
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
            state = json.loads((Path(tmp) / "state.json").read_text())
            self.assertGreater(state["audio_frames"], 0, state)
            self.assertGreater(len(state["services"]), 0, state)
            self.assertFalse(state["error"])

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
