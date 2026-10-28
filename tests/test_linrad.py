"""Regression tests; Python is only a test dependency, not used by the receiver/GUI."""
import configparser
import importlib.util
import itertools
import os
from pathlib import Path
import select
import shlex
import signal
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RX = ROOT / "build/linrad_udp_to_stdout"
UI = ROOT / "build/linrad_dvb"
spec = importlib.util.spec_from_file_location("reference", ROOT / "linrad_udp_to_stdout_v3.py")
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read_exact(stream, size, timeout=4):
    data = bytearray()
    until = time.monotonic() + timeout
    while len(data) < size:
        ready, _, _ = select.select([stream], [], [], max(0, until - time.monotonic()))
        if not ready:
            raise AssertionError(f"Timed out: received {len(data)} of {size} bytes")
        chunk = os.read(stream.fileno(), size - len(data))
        if not chunk:
            raise AssertionError("Unexpected EOF")
        data.extend(chunk)
    return bytes(data)


def wait_for(predicate, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Condition did not become true")


class ReceiverTests(unittest.TestCase):
    def receive(self, packets, expected, args=(), cwd=None):
        port = free_port()
        p = subprocess.Popen([RX, "--port", str(port), *args], cwd=cwd,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        prefix = b""
        until = time.monotonic() + 4
        while b"[linrad] listening" not in prefix:
            ready, _, _ = select.select([p.stderr], [], [], max(0, until - time.monotonic()))
            self.assertTrue(ready, "Receiver did not bind")
            chunk = os.read(p.stderr.fileno(), 4096)
            self.assertTrue(chunk, prefix.decode())
            prefix += chunk
        actual = b""
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for packet, out in zip(packets, expected):
                s.sendto(packet, ("127.0.0.1", port))
                if out:
                    actual += read_exact(p.stdout, len(out))
        if not any(expected):
            time.sleep(0.1)  # Allow recording-only packet processing.
        p.send_signal(signal.SIGTERM)
        rest, err = p.communicate(timeout=3)
        self.assertEqual(p.returncode, 0, (prefix + err).decode())
        self.assertEqual(actual + rest, b"".join(expected))
        return prefix + err

    def test_every_s16_value_matches_python(self):
        data = struct.pack("<65536h", *range(-32768, 32768))
        chunks = [data[i:i+4096] for i in range(0, len(data), 4096)]
        for args, convert in [
            ([], lambda x: reference.gain_s16le(x)),
            (["--s16-gain", "0.5"], lambda x: reference.gain_s16le(x, 0.5)),
            (["--s16-gain", "2.5"], lambda x: reference.gain_s16le(x, 2.5)),
            (["--s16-gain", "-1"], lambda x: reference.gain_s16le(x, -1)),
            (["--u8"], lambda x: reference.s16le_iq_to_u8(x)),
            (["--u8", "--scale", "0.5"], lambda x: reference.s16le_iq_to_u8(x, 0.5)),
            (["--u8", "--scale", "-128", "--offset", "120"],
             lambda x: reference.s16le_iq_to_u8(x, -128, 120)),
        ]:
            with self.subTest(args=args):
                self.receive([bytes(24) + x for x in chunks], [convert(x) for x in chunks], args)

    def test_packet_boundaries_and_measurement(self):
        payload = struct.pack("<4h", -32768, 32767, -1, 1)
        err = self.receive([b"short", bytes(24), bytes(24) + payload + b"x"],
                           [b"", b"", payload], ["--measure", "7", "--verbose"])
        self.assertIn(b"dropped=2", err)
        self.assertIn(b"[INPUT] type=s16 iq_samples=1", err)
        self.assertIn(b"[OUTPUT] type=s16 iq_samples=1", err)
        self.assertIn(b"mean = -32768.000", err)

    def test_u8_measurement(self):
        payload = struct.pack("<4h", -1, 1, -2, 2)
        err = self.receive([bytes(24) + payload], [reference.s16le_iq_to_u8(payload)], ["--u8", "--measure", "4"])
        self.assertIn(b"[INPUT] type=s16 iq_samples=1", err)
        self.assertIn(b"[OUTPUT] type=u8 iq_samples=2", err)

    def test_truncated_datagram_is_rejected(self):
        payload = struct.pack("<2h", 10, 20)
        err = self.receive([bytes(80), bytes(24) + payload], [b"", payload], ["--bufsize", "32"])
        self.assertIn(b"dropped=1", err)

    def test_recording_and_tee(self):
        payload = struct.pack("<4h", -100, 100, -20, 20)
        for tee in (False, True):
            with tempfile.TemporaryDirectory() as tmp, self.subTest(tee=tee):
                args = ["--record-iq", "--record-prefix", "capture", "--u8"]
                if tee:
                    args += ["--record-stdout"]
                output = reference.s16le_iq_to_u8(payload)
                self.receive([bytes(24) + payload], [output if tee else b""], args, tmp)
                files = list(Path(tmp, "recordings").glob("capture_*_u8.iq"))
                self.assertEqual(len(files), 1)
                self.assertEqual(files[0].read_bytes(), output)

    def test_explicit_recording_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = str(Path(tmp, "nested", "IQ files"))
            self.receive([bytes(24) + b"1234"], [b"1234"],
                         ["--record-iq", directory, "--record-stdout"])
            self.assertEqual(next(Path(directory).glob("*.iq")).read_bytes(), b"1234")

    def test_invalid_options(self):
        for args in (["--port", "0"], ["--skip", "-1"], ["--scale", "nan"],
                     ["--s16-gain", "inf"], ["--bufsize", "24"], ["--measure", "-1"],
                     ["--ip", "bogus"], ["--unknown"], ["--record-prefix", "../bad"]):
            with self.subTest(args=args):
                p = subprocess.run([RX, *args], capture_output=True, timeout=3)
                self.assertEqual(p.returncode, 2, p.stderr)


class PipelineTests(unittest.TestCase):
    def preview(self, standard, sr, fec, gi="1/32", scatter=False, config=None):
        args = [UI, "--print-pipeline", "--source", "linrad", "--standard", standard, "--sr", str(sr), "--fec", fec, "--gi", gi]
        if scatter:
            args += ["--scatterfix"]
        if config:
            args += ["--config", str(config)]
        p = subprocess.run(args, capture_output=True, text=True, cwd="/tmp", timeout=3)
        self.assertEqual(p.returncode, 0, p.stderr)
        return [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]

    def test_all_parameter_combinations(self):
        for standard in ("DVB-S", "DVB-S2", "DVB-T"):
            rates = (35000, 40000, 150000, 250000, 333000, 500000) if standard == "DVB-T" else (35000, 66000, 125000, 250000, 333000)
            guards = ("1/32", "1/16", "1/8", "auto") if standard == "DVB-T" else ("1/32",)
            code_rates = ("1/2", "2/3", "3/4", "5/6", "7/8") if standard == "DVB-T" else ("1/2", "2/3")
            for sr, fec, gi, scatter in itertools.product(rates, code_rates, guards, (False, True)):
                with self.subTest(standard=standard, sr=sr, fec=fec, gi=gi, scatter=scatter):
                    cmds = self.preview(standard, sr, fec, gi, scatter)
                    self.assertEqual(len(cmds), 4 if scatter else 3)
                    self.assertEqual(cmds[0][0], str(RX))
                    dec = cmds[1]
                    self.assertEqual(dec[dec.index("--sr") + 1], str(sr))
                    rate_flag = "--sample-rate" if standard == "DVB-T" else "-f"
                    self.assertEqual(dec[dec.index(rate_flag) + 1], "1010526")
                    flag = {"DVB-S": "--cr", "DVB-S2": "--modcods", "DVB-T": "--fec"}[standard]
                    expected = ("0x10" if fec == "1/2" else "0x40") if standard == "DVB-S2" else fec
                    self.assertEqual(dec[dec.index(flag) + 1], expected)
                    if standard == "DVB-T":
                        self.assertEqual(dec[0], str(UI))
                        self.assertIn("--decoder-worker=dvbt", dec)
                        self.assertEqual(dec[dec.index("--gi") + 1], gi)
                    if scatter:
                        self.assertEqual(cmds[2][-2:], ["--udp-output", "127.0.0.1:1235"])
                    self.assertEqual(cmds[-1][-1], "udp://@127.0.0.1:1235" if scatter else "fd://0")

    def test_direct_sdr_5ms(self):
        for source, fmt in (("hackrf", "--u8"), ("b210", "--s16")):
            p = subprocess.run([UI, "--print-pipeline", "--source", source,
                                "--standard", "DVB-S2", "--sr", "5000000", "--fec", "3/4"],
                               capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            cmds = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
            self.assertIn(source, cmds[0])
            self.assertIn(fmt, cmds[1])
            self.assertEqual(cmds[1][cmds[1].index("-f") + 1], "10000000")
            self.assertIn("0x80", cmds[1])

    def test_sdriq_pipeline_and_legacy_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for legacy in (False, True):
                if legacy:
                    del cfg["sdriq"]
                for standard, sr in (("DVB-T", "35000"), ("DVB-T", "40000"), ("DVB-S", "35000"), ("DVB-S2", "125000")):
                    self.write_config(cfg, path)
                    p = subprocess.run([UI, "--config", str(path), "--print-pipeline", "--source", "sdriq",
                                        "--standard", standard, "--sr", sr],
                                       capture_output=True, text=True, timeout=3)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    source, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
                    self.assertEqual(source[source.index("--sample-rate") + 1], "111111")
                    self.assertEqual(source[source.index("--frequency") + 1], "28000000")
                    self.assertIn("driver=rfspace,sdr-iq=/dev/ttyUSB0", source)
                    self.assertEqual(decoder[decoder.index("--sample-rate" if standard == "DVB-T" else "-f") + 1], "111111")
                    if standard == "DVB-T":
                        self.assertIn("--resample-to-dvbt-rate", decoder)
                        self.assertIn("s16", decoder)
                    else:
                        self.assertIn("--s16", decoder)

    def test_sdriq_rejects_unsupported_settings_before_hardware(self):
        base = [ROOT / "build/sdr_to_stdout", "--source", "sdriq", "--sample-rate", "111111",
                "--frequency", "28000000", "--gain", "10"]
        for flag, value in (("--sample-rate", "40000"), ("--sample-rate", "2000000"),
                            ("--gain", "5"), ("--gain", "-30"), ("--frequency", "436000000")):
            p = subprocess.run([*base, flag, value], capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn(b"SDR-IQ:", p.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg["sdriq"]["sample_rate"] = "40000"
            self.write_config(cfg, path)
            p = subprocess.run([UI, "--config", str(path), "--source", "sdriq", "--print-pipeline"],
                               capture_output=True, timeout=3)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn(b"SDR-IQ samplerate", p.stderr)

    def test_saved_session_and_cli_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg["session"] = {"source": "sdriq", "standard": "DVB-T", "sr": "35000",
                              "fec": "2/3", "gi": "1/16", "constellation": "QPSK", "scatterfix": "true"}
            cfg["sdriq"]["frequency"] = "28000100"
            cfg["sdriq"]["gain"] = "-10"
            self.write_config(cfg, path)
            def preview(*args):
                p = subprocess.run([UI, "--config", str(path), "--print-pipeline", *args],
                                   capture_output=True, text=True, timeout=3)
                self.assertEqual(p.returncode, 0, p.stderr)
                return [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
            cmds = preview()
            self.assertEqual(len(cmds), 4)
            self.assertIn("sdriq", cmds[0])
            self.assertIn("28000100", cmds[0])
            self.assertIn("-10", cmds[0])
            self.assertEqual(cmds[1][cmds[1].index("--sr")+1], "35000")
            self.assertEqual(cmds[1][cmds[1].index("--gi")+1], "1/16")
            cmds = preview("--source", "b210", "--standard", "DVB-S2", "--sr", "125000", "--fec", "1/2", "--no-scatterfix")
            self.assertEqual(len(cmds), 3)
            self.assertIn("b210", cmds[0])
            self.assertIn("0x10", cmds[1])
            # Changing standard alone must not retain an incompatible saved rate.
            cmds = preview("--standard", "DVB-S")
            self.assertEqual(cmds[1][cmds[1].index("--sr")+1], "125000")

    def test_filter_keeps_decoder_rate_and_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for source in ("linrad", "sdriq", "hackrf", "b210"):
                cfg[source]["filter_enabled"] = "true"
                cfg[source]["filter_bandwidth"] = "50000"
                self.write_config(cfg, path)
                p = subprocess.run([UI, "--config", str(path), "--source", source,
                                    "--standard", "DVB-T", "--sr", "35000", "--print-pipeline"],
                                   capture_output=True, text=True, timeout=3)
                self.assertEqual(p.returncode, 0, p.stderr)
                src, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
                self.assertEqual(src[src.index("--filter-bandwidth")+1], "50000")
                self.assertEqual(decoder[decoder.index("--sample-rate")+1], cfg[source]["sample_rate"])
                self.assertEqual(decoder[decoder.index("--input-format")+1], cfg[source]["format"])

    def test_constellation_modcod_filters(self):
        mappings = {
            "QPSK": {"1/4": 1, "1/3": 2, "2/5": 3, "1/2": 4, "3/5": 5,
                     "2/3": 6, "3/4": 7, "4/5": 8, "5/6": 9, "8/9": 10, "9/10": 11},
            "8PSK": {"3/5": 12, "2/3": 13, "3/4": 14, "5/6": 15, "8/9": 16, "9/10": 17},
        }
        for constellation, fecs in mappings.items():
            for fec, modcod in fecs.items():
                p = subprocess.run([UI, "--print-pipeline", "--standard", "DVB-S2",
                                    "--constellation", constellation, "--fec", fec],
                                   capture_output=True, text=True, timeout=3)
                self.assertEqual(p.returncode, 0, p.stderr)
                decoder = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")][1]
                self.assertEqual(int(decoder[decoder.index("--modcods") + 1], 0), 1 << modcod)
        for args in (["--standard", "DVB-S2", "--constellation", "8PSK", "--fec", "1/2"],
                     ["--standard", "DVB-S2", "--fec", "7/8"],
                     ["--standard", "DVB-T", "--fec", "4/5"],
                     ["--standard", "DVB-S2", "--constellation", "8PSK", "--fec", "4/5"],
                     ["--constellation", "8PSK"], ["--standard", "DVB-T", "--fec", "9/10"],
                     ["--standard", "DVB-S2", "--constellation", "16APSK"]):
            p = subprocess.run([UI, "--print-pipeline", *args], capture_output=True, timeout=3)
            self.assertNotEqual(p.returncode, 0)

    def test_invalid_selection(self):
        for args in (["--standard", "DVB-T", "--sr", "36000"], ["--fec", "1.2"],
                     ["--sr", "0"], ["--gi", "1/4"], ["--standard", "bogus"]):
            p = subprocess.run([UI, "--print-pipeline", *args], capture_output=True, timeout=3)
            self.assertNotEqual(p.returncode, 0)

    def config(self, tmp):
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT / "linrad-dvb.ini")
        # A saved user session is not a stable test fixture.
        cfg.remove_section("session")
        cfg["sdriq"].update(sample_rate="111111", frequency="28000000", gain="10",
                            lo_offset="0", tune_step="1000", format="s16",
                            filter_enabled="false", filter_bandwidth="50000")
        cfg["paths"]["linrad"] = str(RX)
        path = Path(tmp, "receiver.ini")
        return cfg, path

    def write_config(self, cfg, path):
        with path.open("w") as f:
            cfg.write(f)

    def test_config_quoting_and_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg["paths"]["leandvb"] = "programs/a decoder's name"
            cfg["linrad"]["format"] = "u8"
            cfg["linrad"]["sample_rate"] = "2000000"
            cfg["leandvb"]["dvbs_args"] = '--some-option "literal $(touch SHOULD_NOT_EXIST)"'
            self.write_config(cfg, path)
            source, decoder, _ = self.preview("DVB-S", 125000, "1/2", config=path)
            self.assertIn("--u8", source)
            self.assertIn("--u8", decoder)
            self.assertEqual(decoder[0], str(Path(tmp, "programs/a decoder's name")))
            self.assertIn("literal $(touch SHOULD_NOT_EXIST)", decoder)
            self.assertEqual(decoder[decoder.index("-f") + 1], "2000000")
            self.assertFalse(Path(tmp, "SHOULD_NOT_EXIST").exists())

    def test_reserved_options_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg["leandvb"]["dvbs_args"] = "--sr 333000"
            self.write_config(cfg, path)
            p = subprocess.run([UI, "--config", path, "--print-pipeline"], capture_output=True, timeout=3)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn(b"controlled", p.stderr)

    def test_renderer_selection_and_input_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg["session"] = {"renderer": "custom"}
            cfg["paths"]["custom_renderer"] = "players/my renderer"
            cfg["renderer"] = {"args": '--input={input} "literal $(touch NO_FILE)"',
                               "output_file": "capture with spaces.ts"}
            for scatter in (False, True):
                self.write_config(cfg, path)
                commands = self.preview("DVB-S", 125000, "1/2", scatter=scatter, config=path)
                self.assertEqual(commands[-1], [str(Path(tmp, "players/my renderer")),
                    "--input=udp://127.0.0.1:1235" if scatter else "--input=-", "literal $(touch NO_FILE)"])
                self.assertFalse(Path(tmp, "NO_FILE").exists())
            cfg["session"]["renderer"] = "mpegts"
            self.write_config(cfg, path)
            # Direct file output excludes both the renderer and Scatterfix.
            self.assertEqual(len(self.preview("DVB-S", 125000, "1/2", scatter=True, config=path)), 2)
            self.assertFalse(Path(tmp, "capture with spaces.ts").exists())  # Preview has no side effects.
            cfg["session"]["renderer"] = "ffplay"
            # Older INIs have neither the program path nor an ffplay section.
            cfg["paths"].pop("ffplay", None)
            cfg.remove_section("ffplay")
            for scatter in (False, True):
                self.write_config(cfg, path)
                commands = self.preview("DVB-S", 125000, "1/2", scatter=scatter, config=path)
                self.assertEqual(commands[-1], ["ffplay", "-hide_banner", "-loglevel", "warning",
                    "-autoexit", "-f", "mpegts", "udp://127.0.0.1:1235" if scatter else "pipe:0"])
                self.assertEqual(len(commands), 4 if scatter else 3)
            cfg["paths"]["ffplay"] = "players/my ffplay"
            cfg["ffplay"] = {"args": '-window_title "DVB ontvangst" -f mpegts'}
            self.write_config(cfg, path)
            self.assertEqual(self.preview("DVB-T", 40000, "1/2", config=path)[-1],
                [str(Path(tmp, "players/my ffplay")), "-window_title", "DVB ontvangst", "-f", "mpegts", "pipe:0"])


class LifecycleTests(PipelineTests):
    # The base's pure parameter tests need only run once.
    test_constellation_modcod_filters = None
    test_all_parameter_combinations = None
    test_invalid_selection = None
    test_config_quoting_and_format = None
    test_reserved_options_rejected = None
    test_renderer_selection_and_input_arguments = None

    def fixture(self, tmp):
        cfg, path = self.config(tmp)
        for role in ("decoder", "player", "scatter"):
            target = Path(tmp, role)
            shutil.copyfile(ROOT / "tests/fake_stage.py", target)
            target.chmod(0o755)
        cfg["paths"]["leandvb"] = "./decoder"
        cfg["paths"]["rbdvbt_rx"] = "./decoder"
        cfg["paths"]["cvlc"] = "./player"
        cfg["paths"]["scatterfix"] = "./scatter"
        cfg["linrad"]["port"] = str(free_port())
        cfg["scatterfix"]["udp_port"] = str(free_port())
        for section, key in (("leandvb", "dvbs_args"), ("leandvb", "dvbs2_args"),
                             ("rbdvbt_rx", "args"), ("cvlc", "args"), ("scatterfix", "args")):
            cfg[section][key] = ""
        self.write_config(cfg, path)
        return cfg, path

    def launch(self, path, scatter=False):
        command = [UI, "--config", path, "--run"]
        if scatter:
            command += ["--scatterfix"]
        return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def cleanup_process(self, p, tmp):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        p.stderr.close()
        for f in Path(tmp).glob("*.pid"):
            try:
                os.kill(int(f.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass

    @staticmethod
    def alive(pid):
        try:
            return Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0] != "Z"
        except FileNotFoundError:
            return False

    def assert_stopped(self, tmp, port):
        for f in Path(tmp).glob("*.pid"):
            wait_for(lambda: not self.alive(int(f.read_text())))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind(("127.0.0.1", port))

    def send_until_received(self, tmp, port, p):
        expected = bytes(range(16))  # Four complete s16 IQ pairs.
        player_out = Path(tmp, "player.out")
        def received():
            self.assertIsNone(p.poll(), "Pipeline exited before receiving")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.sendto(bytes(24) + expected, ("127.0.0.1", port))
            return player_out.exists() and expected in player_out.read_bytes()
        try:
            wait_for(received)
        except AssertionError:
            sizes = {f.name: f.read_bytes()[:100] for f in Path(tmp).glob("*.out")}
            p.terminate()
            _, err = p.communicate(timeout=5)
            self.fail(f"No data at player: {sizes}; stderr={err.decode()}")

    def test_start_stop_data_and_descendants(self):
        for scatter in (False, True):
            with tempfile.TemporaryDirectory() as tmp, self.subTest(scatter=scatter):
                cfg, path = self.fixture(tmp)
                Path(tmp, "stubborn").touch()
                p = self.launch(path, scatter)
                try:
                    wait_for(lambda: Path(tmp, "descendant.pid").exists())
                    self.send_until_received(tmp, int(cfg["linrad"]["port"]), p)
                    self.assertEqual(Path(tmp, "decoder.cwd").read_text(), tmp)
                    p.send_signal(signal.SIGINT)
                    _, err = p.communicate(timeout=5)
                    self.assertEqual(p.returncode, 0, err.decode())
                    self.assert_stopped(tmp, int(cfg["linrad"]["port"]))
                finally:
                    self.cleanup_process(p, tmp)

    def test_direct_mpegts_file_and_existing_file_protection(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.fixture(tmp)
            cfg["session"] = {"renderer": "mpegts", "scatterfix": "true"}
            cfg["renderer"] = {"output_file": "capture with spaces.ts"}
            self.write_config(cfg, path)
            output = Path(tmp, "capture with spaces.ts")
            expected = bytes(range(16))
            p = self.launch(path)
            try:
                wait_for(lambda: Path(tmp, "decoder.ready").exists())
                def received():
                    self.assertIsNone(p.poll())
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                        sock.sendto(bytes(24) + expected, ("127.0.0.1", int(cfg["linrad"]["port"])))
                    return output.exists() and expected in output.read_bytes()
                wait_for(received)
                p.send_signal(signal.SIGINT)
                _, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 0, err.decode())
                self.assert_stopped(tmp, int(cfg["linrad"]["port"]))
                self.assertFalse(Path(tmp, "player.pid").exists())
                self.assertFalse(Path(tmp, "scatter.pid").exists())
                contents = output.read_bytes()
                self.assertEqual(contents, expected * (len(contents) // len(expected)))
            finally:
                self.cleanup_process(p, tmp)
            p = self.launch(path)
            try:
                _, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 1, err.decode())
                self.assertIn(b"Kies een nieuwe bestandsnaam", err)
                self.assertEqual(output.read_bytes(), contents)
            finally:
                self.cleanup_process(p, tmp)

    def test_decoder_failure_stops_entire_pipeline(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.fixture(tmp)
            p = self.launch(path, True)
            try:
                self.send_until_received(tmp, int(cfg["linrad"]["port"]), p)
                Path(tmp, "fail").touch()
                _, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 1, err.decode())
                self.assertIn(b"exitcode 17", err)
                self.assert_stopped(tmp, int(cfg["linrad"]["port"]))
            finally:
                self.cleanup_process(p, tmp)

    def test_bind_failure_stops_downstream(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as occupied:
            cfg, path = self.fixture(tmp)
            occupied.bind(("127.0.0.1", int(cfg["linrad"]["port"])))
            p = self.launch(path)
            try:
                _, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 1)
                self.assertIn(b"bind:", err)
                for f in Path(tmp).glob("*.pid"):
                    self.assertFalse(self.alive(int(f.read_text())))
            finally:
                self.cleanup_process(p, tmp)

    def test_spawn_failure_cleans_partial_pipeline(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.fixture(tmp)
            Path(tmp, "decoder").write_text("#!/no/such/interpreter\n")
            p = self.launch(path, True)
            try:
                _, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 1, err.decode())
                self.assert_stopped(tmp, int(cfg["linrad"]["port"]))
            finally:
                self.cleanup_process(p, tmp)

    @unittest.skipUnless(shutil.which("broadwayd"), "GTK Broadway daemon not installed")
    def test_gui_controls_stop_restart_and_close(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.fixture(tmp)
            # A private headless GTK display; never connects to the user's desktop.
            display = free_port()
            env = dict(os.environ, GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{display}",
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE="1",
                       LINRAD_GUI_SCREENSHOT=str(ROOT / "build/gui-smoke.png"))
            daemon = subprocess.Popen(["broadwayd", "--address", "127.0.0.1", "--port", str(free_port()),
                                       f":{display}"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                self.assertIsNone(daemon.poll(), daemon.stderr.read().decode() if daemon.poll() is not None else "")
                p = subprocess.Popen([ROOT / "build/gui_smoke", path], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                try:
                    _, err = p.communicate(timeout=12)
                    self.assertEqual(p.returncode, 0, err.decode())
                    self.assertNotIn(b"CRITICAL", err)
                    self.assert_stopped(tmp, int(cfg["linrad"]["port"]))
                finally:
                    self.cleanup_process(p, tmp)
            finally:
                daemon.terminate()
                daemon.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
