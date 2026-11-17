"""Regression tests; Python is only a test dependency, not used by the receiver/GUI."""
import configparser
import csv
import itertools
import json
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
EXAMPLE = ROOT / "linrad-dvb.example.ini"


def gain_s16le(data, gain=1.0):
    """Reference contract: round to nearest even and saturate signed 16-bit."""
    values = (max(-32768, min(32767, round(value * gain)))
              for (value,) in struct.iter_unpack("<h", data))
    return b"".join(struct.pack("<h", value) for value in values)


def s16le_iq_to_u8(data, scale=256.0, offset=128):
    """Reference contract for Linrad's configurable unsigned output scaling."""
    return bytes(max(0, min(255, round(offset + value * scale / 256.0)))
                 for (value,) in struct.iter_unpack("<h", data))


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
    def test_live_iq_record_toggle_preserves_full_band_s16(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = free_port()
            p = subprocess.Popen([RX, "--port", str(port), "--sample-rate", "250000",
                                  "--control-stdin", "--u8", "--filter-bandwidth", "40000"],
                                 cwd=tmp, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, bufsize=0)
            self.addCleanup(lambda: p.poll() is None and p.kill())

            def wait_log(marker):
                data = b""
                deadline = time.monotonic() + 4
                while marker not in data:
                    ready, _, _ = select.select([p.stderr], [], [], max(0, deadline - time.monotonic()))
                    self.assertTrue(ready, data.decode())
                    chunk = os.read(p.stderr.fileno(), 4096)
                    self.assertTrue(chunk, data.decode())
                    data += chunk
                return data

            def command(text, marker):
                p.stdin.write(text.encode() + b"\n")
                return wait_log(marker)

            payload = struct.pack("<8h", -32768, 32767, -12345, 6789, 1, -1, 0, 1234)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                def send():
                    sender.sendto(bytes(24) + payload, ("127.0.0.1", port))
                    read_exact(p.stdout, len(payload) // 2)

                try:
                    wait_log(b"[linrad] listening")
                    send()  # No recording before the toggle.
                    first = "linrad-dvb-20261101T000000.000001Z-144600000-250000.iq"
                    command(f"record 144600000 {first}", b"[iq-record] started")
                    send()
                    # Commands also work without incoming UDP packets.
                    command("record-stop", b"[iq-record] stopped")
                    self.assertEqual(Path(tmp, first).read_bytes(), payload)
                    metadata = json.loads(Path(tmp, first + ".json").read_text())
                    self.assertEqual(metadata["format"], "cs16_le")
                    self.assertEqual(metadata["sample_rate_hz"], 250000)
                    self.assertEqual(metadata["center_frequency_hz"], 144600000)
                    send()
                    self.assertEqual(Path(tmp, first).read_bytes(), payload)
                    command(f"record 144600000 {first}", b"[iq-record] error")
                    self.assertEqual(Path(tmp, first).read_bytes(), payload)
                    second = first.replace("000001Z", "000002Z")
                    command(f"record 144600000 {second}", b"[iq-record] started")
                    send()
                    p.terminate()
                    _, err = p.communicate(timeout=4)
                    self.assertEqual(p.returncode, 0, err.decode())
                    self.assertIn(b"[iq-record] stopped", err)
                    self.assertEqual(Path(tmp, second).read_bytes(), payload)
                finally:
                    if p.poll() is None:
                        p.kill()
                        p.communicate()

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
            ([], lambda x: gain_s16le(x)),
            (["--s16-gain", "0.5"], lambda x: gain_s16le(x, 0.5)),
            (["--s16-gain", "2.5"], lambda x: gain_s16le(x, 2.5)),
            (["--s16-gain", "-1"], lambda x: gain_s16le(x, -1)),
            (["--u8"], lambda x: s16le_iq_to_u8(x)),
            (["--u8", "--scale", "0.5"], lambda x: s16le_iq_to_u8(x, 0.5)),
            (["--u8", "--scale", "-128", "--offset", "120"],
             lambda x: s16le_iq_to_u8(x, -128, 120)),
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
        err = self.receive([bytes(24) + payload], [s16le_iq_to_u8(payload)], ["--u8", "--measure", "4"])
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
                output = s16le_iq_to_u8(payload)
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


class RtlSerialTests(unittest.TestCase):
    def test_serial_overrides_saved_indices_and_preserves_other_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(EXAMPLE)
            cfg["rtlsdr"]["args"] = "driver=rtlsdr, rtl=0, index=2, serial=1001, bufflen=8192"
            path = Path(tmp, "receiver.ini")
            with path.open("w") as out:
                cfg.write(out)
            for serial in ("1001", "1002", "1003", "001002"):
                result = subprocess.run([UI, "--config", path, "--print-pipeline", "--rtlsdr-serial", serial],
                                        capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 0, result.stderr)
                args = shlex.split(result.stdout.splitlines()[0])
                self.assertEqual(args[args.index("--source") + 1], "rtlsdr")
                self.assertEqual(args[args.index("--args") + 1], "driver=rtlsdr,bufflen=8192,serial=" + serial)
            self.assertIn("serial=1001", path.read_text())  # CLI does not overwrite the saved file.

    def test_serial_rejects_conflicting_source_and_malformed_value(self):
        for extra in (("--source", "hackrf", "--rtlsdr-serial", "1002"),
                      ("--iq-file", "unused.iq", "--rtlsdr-serial", "1002"),
                      ("--rtlsdr-serial", ""), ("--rtlsdr-serial", "1002,rtl=0")):
            result = subprocess.run([UI, "--config", EXAMPLE, "--print-pipeline", *extra], capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)


class PipelineTests(unittest.TestCase):
    def preview(self, standard, sr, fec, gi="1/32", scatter=False, config=None):
        args = [UI, "--config", str(config or EXAMPLE), "--print-pipeline", "--source", "linrad", "--standard", standard, "--sr", str(sr), "--fec", fec, "--gi", gi]
        if config is None:
            args += ["--renderer", "cvlc"]
        if scatter:
            args += ["--scatterfix"]
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
                    self.assertEqual(len(cmds), 3)
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
                    self.assertEqual(cmds[-1][-1], "fd://0")

    def test_s2_automatic_acquisition_and_processing(self):
        for rate in (35000, 66000, 125000, 250000, 333000, 500000, 1000000, 5000000):
            with self.subTest(rate=rate):
                decoder = self.preview('DVB-S2', rate, '1/2')[1]
                self.assertEqual('--fastlock' in decoder, rate <= 333000)
                self.assertEqual(decoder[decoder.index('--ldpc-batch')+1], '1' if rate <= 333000 else '32')
                self.assertEqual(decoder[decoder.index('--nhelpers')+1], '1')
        # The 5M 8PSK repeater uses the ordinary command line/configuration.
        p = subprocess.run([UI, '--config', EXAMPLE, '--print-pipeline', '--source', 'b210', '--standard', 'DVB-S2',
                            '--sr', '5000000', '--constellation', '8PSK', '--fec', '3/4'],
                           capture_output=True, text=True, timeout=3)
        self.assertEqual(p.returncode, 0, p.stderr)
        decoder = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")][1]
        self.assertNotIn('--fastlock', decoder)
        self.assertIn('0x4000', decoder)
        self.assertEqual(decoder[decoder.index('--ldpc-batch')+1], '32')

    def test_s2_automatic_defaults_and_manual_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for key in ('latency', 'acquisition'):
                cfg.remove_option('leandvb', key)
            self.write_config(cfg, path)
            decoder = self.preview('DVB-S2', 5000000, '3/4', config=path)[1]
            self.assertEqual(decoder[decoder.index('--ldpc-batch')+1], '32')
            self.assertNotIn('--fastlock', decoder)
            for rate, latency, acquisition, batch, fast in (
                    (125000, 'throughput', 'normal', '32', False),
                    (5000000, 'low', 'fast', '1', True)):
                cfg['leandvb'].update(latency=latency, acquisition=acquisition)
                self.write_config(cfg, path)
                decoder = self.preview('DVB-S2', rate, '1/2', config=path)[1]
                self.assertEqual(decoder[decoder.index('--ldpc-batch')+1], batch)
                self.assertEqual('--fastlock' in decoder, fast)
            cfg['leandvb'].update(ldpc_mode='bitflip', acquisition='auto')
            self.write_config(cfg, path)
            decoder = self.preview('DVB-S2', 125000, '1/2', config=path)[1]
            self.assertIn('--fastlock', decoder)
            self.assertNotIn('--ldpc-batch', decoder)

    def test_model_frequency_limits_and_converter_offsets(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for source, low, high in (("airspyhf", 64000000, 260000000),
                                      ("airspyr2", 24000000, 1700000000),
                                      ("rsp1a", 1000, 2000000000),
                                      ("rsp1b", 1000, 2000000000)):
                cfg[source] = {}  # Upgrade from a config without the new sources.
                for frequency, lo, valid in ((low, 0, True), (high, 0, True),
                        (low-1, 0, False), (high+1, 0, False),
                        (144000000, 293000000, True), (144000000, -100000000, True),
                        (high+1, -100000000, False)):
                    with self.subTest(source=source, frequency=frequency, lo=lo):
                        cfg[source].update(frequency=str(frequency), lo_offset=str(lo))
                        self.write_config(cfg, path)
                        result = subprocess.run([UI, "--config", path, "--source", source,
                            "--standard", "DVB-S2", "--sr", "125000", "--print-pipeline"],
                            capture_output=True, text=True, timeout=3)
                        self.assertEqual(result.returncode, 0 if valid else 2, result.stderr)
                        if valid:
                            command = shlex.split(next(line for line in result.stdout.splitlines() if line.startswith("'")))
                            self.assertEqual(command[command.index("--frequency")+1], str(frequency))
                            self.assertEqual(command[command.index("--channel-bandwidth")+1], "168750")
                        else:
                            self.assertIn("SDR/IF tuning", result.stderr)

    def test_symbol_rate_requires_usable_bandwidth_even_at_zero_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for source, rate, sr, offset, valid in (
                    ("sdriq", 196078, 333000, 0, False),
                    ("sdriq", 196078, 125000, 0, True),
                    ("sdriq", 111111, 125000, 0, False),
                    ("sdriq", 111111, 66000, 0, True),
                    ("sdriq", 196078, 125000, 10000, False),
                    ("airspyhf", 768000, 500000, 0, False),
                    ("airspyhf", 768000, 333000, 0, True),
                    ("rsp1b", 2000000, 2000000, 0, False),
                    ("rsp1b", 2000000, 1000000, 0, True),
                    ("rsp1b", 4000000, 2000000, 0, False),
                    ("airspyr2", 10000000, 5000000, 0, True),
                    ("airspyr2", 2500000, 2000000, 0, False),
                    ("airspyr2", 2500000, 1000000, 0, True),
                    ("airspyr2", 2500000, 1000000, 500000, False)):
                with self.subTest(source=source, rate=rate, sr=sr, offset=offset):
                    if source not in cfg:
                        cfg[source] = {}
                    cfg[source].update(sample_rate=str(rate), receiver_offset=str(offset))
                    self.write_config(cfg, path)
                    result = subprocess.run([UI, "--config", path, "--source", source,
                        "--standard", "DVB-S2", "--sr", str(sr), "--print-pipeline"],
                        capture_output=True, text=True, timeout=3)
                    self.assertEqual(result.returncode, 0 if valid else 2, result.stderr)
                    if not valid:
                        self.assertIn("Required:", result.stderr)
                        self.assertIn("usable:", result.stderr)

    def test_airspyr2_defaults_and_invalid_saved_rate(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg.remove_section('airspyr2')
            cfg['session'] = {'source':'airspyr2', 'standard':'DVB-S2', 'sr':'333000', 'fec':'1/2'}
            self.write_config(cfg, path)
            result = subprocess.run([UI, '--config', path, '--print-pipeline'],
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            command = shlex.split(next(line for line in result.stdout.splitlines() if line.startswith("'")))
            for key, value in (('--source','airspyr2'), ('--sample-rate','10000000'),
                               ('--frequency','437000000'), ('--args','driver=airspy'), ('--format','s16')):
                self.assertEqual(command[command.index(key)+1], value)
            cfg['airspyr2'] = {'sample_rate':'3000000'}
            self.write_config(cfg, path)
            result = subprocess.run([UI, '--config', path, '--print-pipeline'],
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn('Airspy R2: select sample rate', result.stderr)

    def test_direct_sdr_5ms(self):
        for source, fmt in (("hackrf", "--u8"), ("b210", "--s16"), ("lime", "--s16")):
            p = subprocess.run([UI, "--config", EXAMPLE, "--print-pipeline", "--source", source,
                                "--standard", "DVB-S2", "--sr", "5000000", "--fec", "3/4"],
                               capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            cmds = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
            self.assertIn(source, cmds[0])
            self.assertIn(fmt, cmds[1])
            self.assertEqual(cmds[1][cmds[1].index("-f") + 1], "10000000")
            self.assertIn("0x80", cmds[1])

    def test_lime_defaults_and_saved_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            del cfg['lime']  # Existing installations have no Lime section.
            self.write_config(cfg, path)
            p = subprocess.run([UI, '--config', path, '--source', 'lime', '--print-pipeline'],
                               capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            source = shlex.split(next(line for line in p.stdout.splitlines() if line.startswith("'")))
            self.assertEqual(source[source.index('--sample-rate')+1], '10000000')
            self.assertEqual(source[source.index('--antenna')+1], 'AUTO')
            self.assertEqual(source[source.index('--format')+1], 's16')
            cfg['session'] = {'source': 'lime', 'standard': 'DVB-T', 'sr': '40000', 'fec': '1/2'}
            cfg['lime'] = {'sample_rate': '100000', 'format': 'u8', 'args': 'serial=123',
                           'antenna': 'LNAW', 'filter_enabled': 'true', 'filter_bandwidth': '50000'}
            self.write_config(cfg, path)
            p = subprocess.run([UI, '--config', path, '--print-pipeline'],
                               capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            source, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
            self.assertIn('lime', source)
            self.assertIn('serial=123', source)
            self.assertIn('LNAW', source)
            self.assertIn('--filter-bandwidth', source)
            self.assertEqual(decoder[decoder.index('--sample-rate')+1], '100000')
            self.assertEqual(decoder[decoder.index('--input-format')+1], 'u8')
            cfg['lime']['sample_rate'] = '30720001'
            self.write_config(cfg, path)
            p = subprocess.run([UI, '--config', path, '--print-pipeline'], capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2)

    def test_lime_rejects_invalid_settings_before_hardware(self):
        for option, value in (('--sample-rate', '99999'), ('--sample-rate', '30720001'),
                              ('--sample-rate', '100000.5'), ('--frequency', '9999999'),
                              ('--frequency', '3500000001'), ('--gain', '-1'),
                              ('--gain', '74'), ('--gain', '30.5')):
            p = subprocess.run([ROOT / 'build/sdr_to_stdout', '--source', 'lime', option, value],
                               capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn(b'LimeSDR Mini:', p.stderr)

    def test_rtlsdr_pipeline_defaults_formats_and_saved_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for legacy in (False, True):
                if legacy:
                    del cfg["rtlsdr"]
                for standard, sr in (("DVB-T", "40000"), ("DVB-S", "125000"), ("DVB-S2", "333000")):
                    self.write_config(cfg, path)
                    p = subprocess.run([UI, "--config", str(path), "--source", "rtlsdr",
                                        "--standard", standard, "--sr", sr, "--print-pipeline"],
                                       capture_output=True, text=True, timeout=3)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    source, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
                    self.assertEqual(source[source.index("--sample-rate")+1], "2400000")
                    self.assertEqual(source[source.index("--args")+1], "driver=rtlsdr")
                    self.assertEqual(source[source.index("--format")+1], "u8")
                    self.assertIn("2400000", decoder)
                    self.assertIn("u8" if standard == "DVB-T" else "--u8", decoder)
            cfg["rtlsdr"] = {"sample_rate": "250000", "format": "s16",
                             "args": "driver=rtlsdr,serial=1007", "filter_enabled": "true",
                             "filter_bandwidth": "50000"}
            cfg["session"] = {"source": "rtlsdr", "standard": "DVB-T", "sr": "40000", "fec": "1/2"}
            self.write_config(cfg, path)
            p = subprocess.run([UI, "--config", str(path), "--print-pipeline"],
                               capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            source, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
            self.assertIn("rtlsdr", source)
            self.assertIn("driver=rtlsdr,serial=1007", source)
            self.assertIn("--filter-bandwidth", source)
            self.assertEqual(decoder[decoder.index("--sample-rate")+1], "250000")
            self.assertEqual(decoder[decoder.index("--input-format")+1], "s16")

    def test_rtlsdr_rejects_invalid_settings_before_opening_hardware(self):
        helper = ROOT / "build/sdr_to_stdout"
        for option, value in (("--sample-rate", "225000"), ("--sample-rate", "300001"),
                              ("--sample-rate", "900000"), ("--sample-rate", "3200001"),
                              ("--sample-rate", "250000.5"), ("--frequency", "23999999"),
                              ("--frequency", "1764000001"), ("--gain", "-1"), ("--gain", "50")):
            p = subprocess.run([helper, "--source", "rtlsdr", "--sample-rate", "2400000",
                                option, value], capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn(b"RTL-SDR:", p.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg["rtlsdr"]["sample_rate"] = "500000"
            self.write_config(cfg, path)
            p = subprocess.run([UI, "--config", str(path), "--source", "rtlsdr", "--print-pipeline"],
                               capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2)
            self.assertIn(b"RTL-SDR sample rate", p.stderr)

    def test_receiver_offset_pipeline_and_bandwidth_limits(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg['fcdpp'].update(frequency='144550000', receiver_offset='50000')
            cfg['session'] = {'source':'fcdpp', 'standard':'DVB-T', 'sr':'40000', 'fec':'1/2'}
            for offset in ('0', '50000', '-50000'):
                cfg['fcdpp']['receiver_offset'] = offset
                self.write_config(cfg, path)
                p = subprocess.run([UI, '--config', str(path), '--print-pipeline'], capture_output=True, text=True, timeout=3)
                self.assertEqual(p.returncode, 0, p.stderr)
                source, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
                self.assertEqual(source[source.index('--frequency')+1], '144550000')
                self.assertIn('192000', decoder)
                if offset == '0':
                    self.assertNotIn('--receiver-offset', source)
                    self.assertEqual(source[source.index('--channel-bandwidth')+1], '40000')
                else:
                    self.assertEqual(source[source.index('--receiver-offset')+1], offset)
                    self.assertEqual(source[source.index('--channel-bandwidth')+1], '40000')
            cfg['fcdpp']['receiver_offset'] = '50000'
            self.write_config(cfg, path)
            p = subprocess.run([UI, '--config', str(path), '--print-pipeline', '--standard','DVB-S','--sr','125000'],
                               capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 2)
            self.assertIn('Receiver offset + DVB bandwidth', p.stderr)
            cfg['fcdpp'].update(filter_enabled='true', filter_bandwidth='150000')
            self.write_config(cfg, path)
            p = subprocess.run([UI, '--config', str(path), '--print-pipeline'], capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 2)
            self.assertIn('Receiver offset + IQ filter', p.stderr)
            cfg['fcdpp'].update(receiver_offset='nan', filter_enabled='false')
            self.write_config(cfg, path)
            p = subprocess.run([UI, '--config', str(path), '--print-pipeline'], capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2)
        helper=ROOT/'build/sdr_to_stdout'
        for flags in (['--receiver-offset','96000'], ['--receiver-offset','50000','--channel-bandwidth','168750'],
                      ['--receiver-offset','50000','--filter-bandwidth','150000']):
            p = subprocess.run([helper,'--source','fcdpp','--sample-rate','192000',*flags],capture_output=True,timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn(b'Receiver offset',p.stderr)

    def test_funcube_fixed_rate_pipeline_and_legacy_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            cfg.remove_section("fcdpp")
            for fmt in (None, "s16", "u8"):
                if fmt:
                    cfg["fcdpp"] = {"format": fmt, "args": "lna=0,mixer=1"}
                self.write_config(cfg, path)
                for standard, sr in (("DVB-T", "40000"), ("DVB-S", "66000"), ("DVB-S2", "125000")):
                    p = subprocess.run([UI, "--config", str(path), "--source", "fcdpp",
                                        "--standard", standard, "--sr", sr, "--print-pipeline"],
                                       capture_output=True, text=True, timeout=3)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    source, decoder, _ = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
                    self.assertEqual(source[source.index("--sample-rate")+1], "192000")
                    self.assertEqual(source[source.index("--format")+1], fmt or "s16")
                    self.assertEqual(source[source.index("--gain")+1], "20")
                    self.assertEqual(source[source.index("--args")+1], "lna=0,mixer=1" if fmt else "lna=1,mixer=1")
                    self.assertEqual(decoder[decoder.index("--sample-rate" if standard == "DVB-T" else "-f")+1], "192000")
                    self.assertIn((fmt or "s16") if standard == "DVB-T" else "--" + (fmt or "s16"), decoder)
            cfg["session"] = {"source": "fcdpp", "standard": "DVB-T", "sr": "40000", "fec": "1/2"}
            self.write_config(cfg, path)
            p = subprocess.run([UI, "--config", str(path), "--print-pipeline"], capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("'--source' 'fcdpp'", p.stdout)
            cfg["fcdpp"]["sample_rate"] = "96000"
            self.write_config(cfg, path)
            p = subprocess.run([UI, "--config", str(path), "--print-pipeline"], capture_output=True, text=True, timeout=3)
            self.assertEqual(p.returncode, 2)
            self.assertIn("sample_rate", p.stderr)

    def test_funcube_rejects_unsupported_settings_before_hardware(self):
        base = [ROOT / "build/sdr_to_stdout", "--source", "fcdpp", "--sample-rate", "192000",
                "--frequency", "437000000", "--gain", "20"]
        for flag, value in (("--sample-rate", "96000"), ("--sample-rate", "196078"),
                            ("--sample-rate", "192000.1"), ("--gain", "-1"), ("--gain", "60"),
                            ("--gain", "20.5"), ("--frequency", "149999"), ("--frequency", "1900000001"),
                            ("--frequency", "240000001"), ("--frequency", "419999999")):
            p = subprocess.run([*base, flag, value], capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn(b"FUNcube Pro+", p.stderr)
        p = subprocess.run([*base, "--args", "lna=2"], capture_output=True, timeout=3)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn(b"unknown option", p.stderr)

    def test_sdriq_pipeline_and_legacy_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for legacy in (False, True):
                if legacy:
                    del cfg["sdriq"]
                for standard, sr in (("DVB-T", "35000"), ("DVB-T", "40000"), ("DVB-S", "35000"), ("DVB-S2", "66000")):
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
            self.assertTrue(b"SDR-IQ sample rate" in p.stderr or b"DVB bandwidth" in p.stderr, p.stderr)

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
            self.assertEqual(len(cmds), 3)
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
            # The default 125 ksym/s needs a higher SDR-IQ sample rate.
            cfg["sdriq"]["sample_rate"] = "196078"
            self.write_config(cfg, path)
            cmds = preview("--standard", "DVB-S")
            self.assertEqual(cmds[1][cmds[1].index("--sr")+1], "125000")

    def test_filter_keeps_decoder_rate_and_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.config(tmp)
            for source in ("linrad", "sdriq", "hackrf", "b210", "rtlsdr"):
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
                p = subprocess.run([UI, "--config", EXAMPLE, "--print-pipeline", "--standard", "DVB-S2",
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
            p = subprocess.run([UI, "--config", EXAMPLE, "--print-pipeline", *args], capture_output=True, timeout=3)
            self.assertNotEqual(p.returncode, 0)

    def test_invalid_selection(self):
        for args in (["--standard", "DVB-T", "--sr", "36000"], ["--fec", "1.2"],
                     ["--sr", "0"], ["--gi", "1/4"], ["--standard", "bogus"]):
            # File replay allows custom symbol rates; do not inherit that source.
            p = subprocess.run([UI, "--config", EXAMPLE, "--source", "linrad", "--print-pipeline", *args],
                               capture_output=True, timeout=3)
            self.assertNotEqual(p.returncode, 0)

    def config(self, tmp):
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT / "linrad-dvb.example.ini")
        # Each test chooses its own session, independent of the example startup mode.
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
            cfg["leandvb"]["backend"] = "external"
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
            low_latency = ["--network-caching=300", "--clock-jitter=0",
                           "--clock-synchro=0", "--drop-late-frames", "--skip-frames"]
            cfg["cvlc"]["args"] = " ".join(low_latency)
            for renderer in ("cvlc", "vlc"):
                cfg["session"] = {"renderer": renderer}
                self.write_config(cfg, path)
                direct = self.preview("DVB-S", 125000, "1/2", config=path)[-1]
                self.assertEqual(direct[1:], low_latency + ["fd://0"])
                scatter = self.preview("DVB-S", 125000, "1/2", scatter=True, config=path)[-1]
                self.assertEqual(scatter[1:], low_latency + ["fd://0"])
            cfg["session"] = {"renderer": "custom"}
            cfg["paths"]["custom_renderer"] = "players/my renderer"
            cfg["renderer"] = {"args": '--input={input} "literal $(touch NO_FILE)"',
                               "output_file": "capture with spaces.ts"}
            for scatter in (False, True):
                self.write_config(cfg, path)
                commands = self.preview("DVB-S", 125000, "1/2", scatter=scatter, config=path)
                self.assertEqual(commands[-1], [str(Path(tmp, "players/my renderer")),
                    "--input=-", "literal $(touch NO_FILE)"])
                self.assertFalse(Path(tmp, "NO_FILE").exists())
            cfg["session"]["renderer"] = "mpegts"
            self.write_config(cfg, path)
            # File output needs no external renderer; Scatterfix is an embedded branch.
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
                    "-autoexit", "-f", "mpegts", "pipe:0"])
                self.assertEqual(len(commands), 3)
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
        cfg["leandvb"]["backend"] = "external"
        cfg["paths"]["leandvb"] = "./decoder"
        cfg["paths"]["rbdvbt_rx"] = "./decoder"
        cfg["paths"]["cvlc"] = "./player"
        cfg["paths"]["scatterfix"] = "./scatter"
        cfg["linrad"]["port"] = str(free_port())
        cfg["scatterfix"] = {"udp_port": str(free_port())}
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
                self.assertIn(b"Choose a new filename", err)
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
                self.assertIn(b"exit code 17", err)
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
    def test_dab_gui_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.fixture(tmp)
            source = Path(tmp, "fake-dab-source")
            source.write_text(r"""#!/usr/bin/env python3
import os,time,sys,select
from pathlib import Path
Path(__file__+'.pid').write_text(str(os.getpid()))
directory=Path(sys.argv[sys.argv.index('--dab-state-dir')+1])
written=0
commands=b''
while True:
 if select.select([0],[],[],0)[0]:
  commands+=os.read(0,4096)
  while b'\n' in commands:
   line,commands=commands.split(b'\n',1)
   parts=line.split()
   if len(parts)==3 and parts[0]==b'dab':
    temporary=directory/'tuned.tmp'
    temporary.write_text(f'{int(parts[1])} {int(parts[2])} {written}\n')
    temporary.replace(directory/'tuned')
 written+=os.write(1,bytes(65536))
 time.sleep(.01)
""")
            source.chmod(0o755)
            cfg["paths"]["sdr"] = str(source)
            self.write_config(cfg, path)
            display = free_port()
            env = dict(os.environ, GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{display}",
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE="1")
            daemon = subprocess.Popen(["broadwayd", "--address", "127.0.0.1", "--port", str(free_port()),
                                       f":{display}"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                p = subprocess.run([ROOT / "build/dab_gui_smoke", path], env=env,
                                   capture_output=True, timeout=15)
                self.assertEqual(p.returncode, 0, p.stderr.decode())
                self.assertNotIn(b"CRITICAL", p.stderr)
            finally:
                daemon.terminate()
                daemon.communicate(timeout=3)
                for pidfile in Path(tmp).glob("*.pid"):
                    try:
                        os.kill(int(pidfile.read_text()), signal.SIGTERM)
                    except ProcessLookupError:
                        pass

    @unittest.skipUnless(shutil.which("broadwayd"), "GTK Broadway daemon not installed")
    def test_receiver_presets(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, path = self.fixture(tmp)
            display = free_port()
            env = dict(os.environ, GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{display}",
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE="1", G_DEBUG="fatal-criticals")
            daemon = subprocess.Popen(["broadwayd", "--address", "127.0.0.1", "--port",
                                       str(free_port()), f":{display}"], env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                result = subprocess.run([ROOT / "build/gui_smoke", "--test-presets", path],
                                        env=env, capture_output=True, timeout=25)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertIn(b"manual-change checks passed", result.stdout)
            finally:
                daemon.terminate()
                daemon.communicate(timeout=3)

    @unittest.skipUnless(shutil.which("broadwayd"), "GTK Broadway daemon not installed")
    def test_gui_controls_stop_restart_and_close(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, path = self.fixture(tmp)
            # gui_smoke asserts this simulated receiver's address explicitly.
            cfg["afedri"]["ip"] = "192.168.0.118"
            self.write_config(cfg, path)
            from test_ts_info import Stream
            ts = Stream()
            Path(tmp, "information.ts").write_bytes(ts.pat(((1, 0x1000), (2, 0x1001))) + ts.sdt() +
                ts.pmt() + ts.eit() + ts.eit(number=1, title=b"Next event") + ts.teletext() + ts.teletext())
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
                    with Path(tmp, "reception-log.csv").open(newline="") as log:
                        rows = list(csv.DictReader(log))
                    self.assertEqual(len(rows), 2)
                    self.assertEqual(rows[0]["frequency_hz"], "436125000")
                    self.assertEqual(rows[0]["mode"], "DVB-T2")
                    self.assertEqual(rows[0]["service_name"], 'Test, "TV" é')
                    self.assertEqual(rows[0]["provider_name"], "Provider, one")
                    self.assertEqual(rows[0]["tii"], "")
                    self.assertEqual(rows[0]["source"], "b210")
                    self.assertEqual(rows[0]["symbol_rate_baud"], "")
                    self.assertEqual(rows[0]["bandwidth_hz"], "8000000")
                    self.assertEqual(rows[0]["fec"], "2/3")
                    self.assertEqual(rows[0]["my_callsign"], "PE1ITR/P")
                    self.assertEqual(rows[0]["my_locator"], "JO21QK")
                    self.assertRegex(rows[0]["timestamp_utc"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z$")
                    self.assertEqual(rows[1]["tii"], "12:03; 21:07")
                    self.assertEqual(rows[1]["provider_name"], "Provider\nTwo")
                    from test_reception_log import read_adif
                    exported = read_adif(Path(tmp, "all.adi"))
                    selected = read_adif(Path(tmp, "selected.adi"))
                    self.assertEqual(len(exported), 2)
                    self.assertEqual(len(selected), 1)
                    self.assertEqual(selected[0]["APP_LINRADDVB_TII"], "12:03; 21:07")
                    self.assertEqual(exported[1]["MODE"], "ATV")
                    self.assertEqual(exported[1]["STATION_CALLSIGN"], "PE1ITR/P")
                    self.assertEqual(exported[1]["MY_GRIDSQUARE"], "JO21QK")
                    self.assertEqual(exported[1]["APP_LINRADDVB_MODE"], "DVB-T2")
                    self.assertEqual(exported[1]["APP_VERON_ATV_POINTS"], "4321")
                    self.assertEqual(exported[1]["APP_VERON_ATV_DIRECTION"], "ONE-WAY")
                    self.assertIn("BW (Hz): 8000000; FEC: 2/3", exported[1]["COMMENT"])
                    self.assert_stopped(tmp, int(cfg["linrad"]["port"]))
                finally:
                    self.cleanup_process(p, tmp)
            finally:
                daemon.terminate()
                daemon.communicate(timeout=3)


@unittest.skipUnless(shutil.which("broadwayd") and shutil.which("ffmpeg") and shutil.which("ffprobe"),
                     "Video smoke test needs Broadway and FFmpeg")
class VideoTests(unittest.TestCase):
    def test_live_external_switch_failure_and_stalled_player(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "switch.ts")
            subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                            "color=c=green:s=320x240:r=25", "-f", "lavfi", "-i",
                            "anullsrc=r=48000:cl=stereo", "-t", "120", "-c:v", "mpeg2video",
                            "-c:a", "mp2", "-b:a", "64k", "-g", "12",
                            "-program", "program_num=101:st=0:st=1",
                            "-muxrate", "1000000", "-f", "mpegts", str(path)],
                           check=True, capture_output=True, timeout=20)
            player = Path(tmp, "fake player")
            player.write_text("""#!/usr/bin/python3
import json, os, signal, sys, time
from pathlib import Path
root=Path(__file__).parent
mode=(root/'mode').read_text()
if mode in ('blocked','stop'):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
(root/(mode+'.json')).write_text(json.dumps(sys.argv[1:]))
(root/(mode+'.pid')).write_text(str(os.getpid()))
if mode=='failure':
    sys.exit(1)
if mode in ('blocked','stop'):
    time.sleep(60)
if mode=='first':
    time.sleep(0.4)  # Simulate initialization while HD data keeps arriving.
with (root/(mode+'.ts')).open('wb') as out:
    while True:
        data=os.read(0,65536)
        if not data: break
        out.write(data); out.flush()
""")
            player.chmod(0o755)
            display = free_port()
            env = dict(os.environ, GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{display}",
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE="1")
            daemon = subprocess.Popen(["broadwayd", "--address", "127.0.0.1", "--port", str(free_port()),
                                       f":{display}"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                result = subprocess.run([ROOT / "build/video_smoke", "--external", path, tmp],
                                        env=env, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertNotIn(b"CRITICAL", result.stderr)
                captured = Path(tmp, "first.ts").read_bytes()
                self.assertGreater(len(captured), 250000)
                self.assertTrue(captured in path.read_bytes(),
                                "External startup buffer lost/reordered TS bytes")
                for mode, program, volume in (("first",101,"0"),("service",202,"0"),
                                              ("muted",202,"100"),("return",101,"100")):
                    argv = json.loads(Path(tmp, mode+".json").read_text())
                    self.assertEqual(argv[argv.index("-hwaccel")+1], "cuda")
                    self.assertEqual(argv[argv.index("-vulkan_params")+1], "device=NVIDIA test GPU")
                    self.assertEqual(argv[argv.index("-vst")+1], f"p:{program}:v:0")
                    self.assertEqual(argv[argv.index("-ast")+1], f"p:{program}:a:0")
                    self.assertEqual(argv[argv.index("-volume")+1], volume)
                self.assertGreater(sum(p.stat().st_size for p in Path(tmp).glob("*.ts")
                                       if p.name != "switch.ts"), 188)
            finally:
                # Also clean up deliberately stubborn fakes if an assertion fails.
                for pidfile in Path(tmp).glob("*.pid"):
                    try:
                        pid = int(pidfile.read_text())
                        command = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
                        if os.fsencode(player) in command:
                            os.kill(pid, signal.SIGKILL)
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                daemon.terminate(); daemon.communicate(timeout=3)

    def test_live_program_switch_keeps_audio_video_and_udp(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "programs.ts")
            subprocess.run(["ffmpeg", "-v", "error",
                            "-f", "lavfi", "-i", "color=c=green:s=320x240:r=25",
                            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                            "-f", "lavfi", "-i", "color=c=red:s=320x240:r=25",
                            "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
                            "-map", "0:v", "-map", "1:a", "-map", "2:v", "-map", "3:a",
                            "-program", "program_num=101:st=0:st=1:title=Green",
                            "-program", "program_num=202:st=2:st=3:title=Red",
                            "-t", "24", "-c:v", "mpeg2video", "-g", "12",
                            "-c:a", "mp2", "-b:a", "64k", "-muxrate", "1000000",
                            "-f", "mpegts", str(path)], check=True, capture_output=True, timeout=20)
            display = free_port()
            env = dict(os.environ, GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{display}",
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE="1")
            daemon = subprocess.Popen(["broadwayd", "--address", "127.0.0.1", "--port", str(free_port()),
                                       f":{display}"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                result = subprocess.run([ROOT / "build/video_smoke", "--programs", path],
                                        env=env, capture_output=True, timeout=25)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertNotIn(b"CRITICAL", result.stderr)
            finally:
                daemon.terminate(); daemon.communicate(timeout=3)

    def test_embedded_video_fixed_frame_and_udp(self):
        with tempfile.TemporaryDirectory() as tmp:
            inputs = []
            for i, (size, aspect) in enumerate((("320x240", "4:3"), ("1280x720", "16:9"),
                                               ("720x576", "4:3"), ("720x576", "16:9"))):
                path = Path(tmp, f"{i}.ts")
                subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                                f"color=c=green:s={size}:r=25", "-f", "lavfi", "-i",
                                "anullsrc=r=48000:cl=stereo", "-t", "4", "-c:v", "mpeg2video",
                                "-g", "12", "-aspect", aspect, "-c:a", "mp2", "-f", "mpegts", str(path)],
                               check=True, capture_output=True, timeout=15)
                inputs.append(path)
            display = free_port()
            env = dict(os.environ, GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{display}",
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE="1")
            daemon = subprocess.Popen(["broadwayd", "--address", "127.0.0.1", "--port", str(free_port()),
                                       f":{display}"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                output = Path(tmp, "forwarded.ts")
                try:
                    p = subprocess.run([ROOT / "build/video_smoke", *inputs, output],
                                       env=env, capture_output=True, timeout=35)
                except subprocess.TimeoutExpired as exc:
                    self.fail(f"Video smoke timed out: {(exc.stderr or b'').decode(errors='replace')}")
                self.assertEqual(p.returncode, 0, p.stderr.decode())
                self.assertNotIn(b"CRITICAL", p.stderr)
                probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type",
                                        "-of", "csv=p=0", str(output)], capture_output=True, text=True, timeout=5)
                self.assertEqual(probe.returncode, 0, probe.stderr)
                self.assertIn("video", probe.stdout)
                self.assertIn("audio", probe.stdout)
                # An independent player/decoder can actually decode the UDP payload.
                decode = subprocess.run(["ffmpeg", "-v", "error", "-i", str(output),
                                         "-frames:v", "1", "-f", "null", "-"], capture_output=True, timeout=5)
                self.assertEqual(decode.returncode, 0, decode.stderr.decode())
                # Four HEVC pictures, then an open but idle input: frame threads
                # must not retain all video until EOF as in the PE1ORG recording.
                sparse = Path(tmp, "sparse-hevc.ts")
                subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                                "color=c=green:s=320x240:r=2", "-frames:v", "4",
                                "-c:v", "libx265", "-x265-params",
                                "pools=1:frame-threads=1:keyint=1:bframes=0:log-level=error",
                                "-f", "mpegts", sparse], check=True, capture_output=True, timeout=15)
                p = subprocess.run([ROOT / "build/video_smoke", "--sparse", sparse],
                                   env=env, capture_output=True, timeout=10)
                self.assertEqual(p.returncode, 0, p.stderr.decode())
            finally:
                daemon.terminate()
                daemon.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
