"""Exercise native AFEDRI against a loopback TCP/UDP receiver, without hardware."""
import configparser
import os
import math
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RX = ROOT / "build/sdr_to_stdout"
START = bytes.fromhex("08 00 18 00 80 02 00 01")
STOP = bytes.fromhex("08 00 18 00 80 01 00 01")
PAYLOAD = struct.pack("<8h", -32768, 32767, -12345, 6789, 1, -1, 0, 1234) * 64


class Receiver:
    def __init__(self, failure=None):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen()
        self.listener.settimeout(8)
        self.sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.failure = failure
        self.delay_frequency = 0
        self.in_frequency = threading.Event()
        self.commands = []
        self.times = []
        self.errors = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def send(self, seq, payload=PAYLOAD, magic=b"\x04\x84"):
        self.sender.sendto(magic + struct.pack("<H", seq) + payload, ("127.0.0.1", self.port))

    def serve(self):
        try:
            with self.listener.accept()[0] as conn:
                conn.settimeout(8)
                self.conn = conn
                while True:
                    header = conn.recv(1)
                    if not header:
                        break
                    cmd = header
                    while len(cmd) < header[0]:
                        chunk = conn.recv(header[0] - len(cmd))
                        if not chunk:
                            raise AssertionError("truncated command")
                        cmd += chunk
                    self.commands.append(cmd)
                    self.times.append(time.monotonic())
                    if cmd == b"\x04\x20\x01\x00":
                        reply = bytes.fromhex("0b 00 01 00") + b"AFEDRI\0"
                    elif cmd[:4] == bytes.fromhex("09 e0 02 55"):
                        word = (76800000 >> (16 * cmd[4])) & 65535
                        reply = bytes.fromhex("09 e0 06 55") + struct.pack("<H", word) + bytes(3)
                    elif cmd[:4] == bytes.fromhex("09 e0 02 30"):
                        reply = bytes.fromhex("09 e0 06 1c 30 00 00 00 00")
                    else:
                        reply = cmd
                    if cmd == START:
                        if self.failure == "start":
                            reply = STOP
                        elif self.failure == "malformed":
                            reply = b"\x01\x00"
                        elif self.failure == "silent":
                            continue
                        elif self.failure != "no-iq":
                            # Arrive BEFORE the TCP acknowledgement: UDP must be bound already.
                            self.send(65535)
                    if cmd[:4] == bytes.fromhex("0a 00 20 00") and self.delay_frequency:
                        self.in_frequency.set()
                        time.sleep(self.delay_frequency)
                    # Fragment every response, including the 2-byte header.
                    conn.sendall(reply[:1])
                    time.sleep(.002)
                    conn.sendall(reply[1:3])
                    time.sleep(.002)
                    conn.sendall(reply[3:])
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:
            self.errors.append(exc)

    def close(self):
        self.thread.join(3)
        self.listener.close()
        self.sender.close()
        if self.thread.is_alive():
            self.conn.close()
            raise AssertionError("control connection did not close")
        if self.errors:
            raise self.errors[0]


class AfedriTests(unittest.TestCase):
    def launch(self, device, *args, cwd=None, env=None, stdout=subprocess.PIPE):
        p = subprocess.Popen([RX, "--source", "afedri", "--ip", "127.0.0.1", "--port", str(device.port),
                              "--sample-rate", "200000", "--frequency", "28000000", "--gain", "14",
                              "--control-stdin", *args], stdin=subprocess.PIPE, stdout=stdout,
                             stderr=subprocess.PIPE, bufsize=0, cwd=cwd, env=env)
        self.addCleanup(self.cleanup, p, device)
        return p

    @staticmethod
    def cleanup(p, device):
        if p.poll() is None:
            p.terminate()
        try:
            p.wait(4)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        for stream in (p.stdin, p.stdout, p.stderr):
            if stream is not None: stream.close()
        device.close()

    def read(self, stream, size, timeout=6):
        data = b""
        deadline = time.monotonic() + timeout
        while len(data) < size:
            self.assertTrue(select.select([stream], [], [], max(0, deadline-time.monotonic()))[0], data)
            chunk = os.read(stream.fileno(), size-len(data))
            self.assertTrue(chunk, data)
            data += chunk
        return data

    def wait_log(self, p, marker):
        log = b""
        deadline = time.monotonic() + 6
        while marker not in log:
            self.assertTrue(select.select([p.stderr], [], [], max(0, deadline-time.monotonic()))[0], log)
            chunk = os.read(p.stderr.fileno(), 4096)
            self.assertTrue(chunk, log)
            log += chunk
        return log

    def test_start_sequence_iq_loss_wrap_and_live_controls(self):
        device = Receiver()
        p = self.launch(device)
        self.assertEqual(self.read(p.stdout, 1024), PAYLOAD)
        log = self.wait_log(p, b"Receiving IQ:")
        self.assertIn(b"Online:", log)
        self.assertIn(b"Stream start confirmed", log)
        expected = [STOP, bytes.fromhex("09 e0 02 30 00 00 00 00 00"),
                    bytes.fromhex("09 00 b8 00 00") + struct.pack("<I", 200000),
                    bytes.fromhex("05 00 c4 00 00"),
                    bytes.fromhex("0a 00 20 00 00") + struct.pack("<I", 28000000) + b"\0",
                    bytes.fromhex("06 00 38 00 00 44"), START]
        self.assertEqual(device.commands[3:], expected)
        self.assertTrue(all(b-a >= .09 for a, b in zip(device.times, device.times[1:])))
        self.assertGreaterEqual(device.times[-1]-device.times[-2], .29)
        device.send(0)  # Wrap around, no loss.
        self.assertEqual(self.read(p.stdout, 1024), PAYLOAD)
        device.send(0)  # Duplicate.
        device.send(65535)  # Late packet.
        device.send(1, magic=b"xx")
        device.send(1, payload=b"short")
        device.send(2)  # Keep sample timing across one lost packet, like Linrad.
        self.assertEqual(self.read(p.stdout, 2048), bytes(1024) + PAYLOAD)
        p.stdin.write(b"28001000\ngain 17\n")
        self.wait_log(p, b"RX gain 17.0")
        self.assertIn(bytes.fromhex("06 00 38 00 00 4c"), device.commands)
        p.terminate()
        _, log = p.communicate(timeout=4)
        self.assertEqual(p.returncode, 0, log)
        self.assertIn(b"lost=1 stale=2 invalid=2", log)
        self.assertEqual(device.commands[-1], STOP)

    def test_u8_output_and_full_band_recording(self):
        with tempfile.TemporaryDirectory() as tmp:
            device = Receiver()
            p = self.launch(device, "--format", "u8", cwd=tmp)
            expected = bytes((x+32768)//256 for x in struct.unpack("<512h", PAYLOAD))
            self.assertEqual(self.read(p.stdout, 512), expected)
            base = str(Path(tmp)/"linrad-dvb-test.iq")
            p.stdin.write(b"record 436000000 linrad-dvb-test.iq\n")
            self.wait_log(p, b"[iq-record] started")
            device.send(0)
            self.assertEqual(self.read(p.stdout, 512), expected)
            p.stdin.write(b"record-stop\n")
            self.wait_log(p, b"[iq-record] stopped")
            self.assertEqual(Path(base).read_bytes(), PAYLOAD)

    def test_failed_start_and_missing_iq_stop_receiver(self):
        for failure, marker in (("start", b"rejected"), ("malformed", b"Protocol error"),
                                ("silent", b"timed out"), ("no-iq", b"No valid UDP IQ")):
            with self.subTest(failure=failure):
                device = Receiver(failure)
                p = self.launch(device)
                out, log = p.communicate(timeout=8)
                self.assertNotEqual(p.returncode, 0, log)
                self.assertEqual(out, b"")
                self.assertIn(marker, log)
                self.assertNotIn(b"Receiving IQ:", log)
                self.assertEqual(device.commands[-1], STOP)

    def test_actual_rate_mismatch_does_not_start_stream(self):
        device = Receiver()
        p = self.launch(device, "--sample-rate", "250000")
        _, log = p.communicate(timeout=4)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b"hardware rate 249350", log)
        self.assertNotIn(START, device.commands)

    def test_tcp_disconnect_is_reported(self):
        device = Receiver()
        p = self.launch(device)
        self.read(p.stdout, 1024)
        device.conn.shutdown(socket.SHUT_RDWR)
        _, log = p.communicate(timeout=4)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b"Control connection closed", log)

    def test_udp_keeps_draining_during_slow_tune_reply(self):
        device = Receiver()
        p = self.launch(device, "--sample-rate", "2400000")
        self.assertEqual(self.read(p.stdout, 1024), PAYLOAD)
        device.delay_frequency = .95
        p.stdin.write(b"28001000\n")
        self.assertTrue(device.in_frequency.wait(3))
        # More than a Linux 8 MiB UDP queue can hold (packet bookkeeping counts),
        # but less than one second of IQ. TCP settling must not stall UDP reads.
        count = 6500
        output = bytearray()
        def drain():
            while len(output) < count*1024:
                chunk = p.stdout.read(min(65536, count*1024-len(output)))
                if not chunk: break
                output.extend(chunk)
        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        deadline = time.monotonic()
        for base in range(0, count, 16):
            for seq in range(base, min(base+16, count)):
                device.send(seq)
            deadline += .0016
            time.sleep(max(0, deadline-time.monotonic()))
        reader.join(4)
        self.assertFalse(reader.is_alive(), f"Only {len(output)//1024}/{count} packets")
        self.assertEqual(output, PAYLOAD * count)
        p.terminate()
        _, log = p.communicate(timeout=4)
        self.assertEqual(p.returncode, 0, log)
        self.assertIn(b"lost=0 stale=0 invalid=0", log)
        self.assertIn(b"socket-drops=0 input-queue-drops=0", log)

    def test_large_sequence_jump_does_not_expand_silence(self):
        device = Receiver()
        p = self.launch(device)
        self.read(p.stdout, 1024)
        device.send(1000)
        self.assertEqual(self.read(p.stdout, 1024), PAYLOAD)
        p.terminate()
        _, log = p.communicate(timeout=4)
        self.assertIn(b"lost=1000", log)
        self.assertIn(b"zero-filled=0", log)

    def test_usb_audio_continuity_with_bursty_iq_and_filter(self):
        # Three seconds of 1 kHz USB tone, delivered in 100 ms network bursts.
        # This exercises the actual audio consumer, not just a dummy tap reader.
        rate, packets = 960000, 11250
        cycle = b"".join(struct.pack("<hh", round(10000*math.cos(2*math.pi*i/960)),
                                    round(10000*math.sin(2*math.pi*i/960))) for i in range(960))
        raw = cycle * 3000
        with socket.socket() as tcp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as pcm:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            tcp.close()
            pcm.bind(("127.0.0.1", 0)); pcm.settimeout(.1)
            audio = subprocess.Popen([ROOT/"build/afedri_audio_probe", str(rate), str(port),
                                      str(pcm.getsockname()[1])], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.addCleanup(lambda: audio.poll() is None and audio.kill())
            tap = audio.stdout.readline().decode().strip()
            self.assertTrue(tap)
            device = Receiver()
            p = self.launch(device, "--sample-rate", str(rate), "--filter-bandwidth", "50000",
                            env=dict(os.environ, LINRAD_USB_TAP=tap), stdout=subprocess.DEVNULL)
            self.wait_log(p, b"Receiving IQ:")
            output = bytearray(); done = threading.Event()
            def drain_pcm():
                while not done.is_set():
                    try: output.extend(pcm.recv(4096))
                    except socket.timeout: pass
            reader = threading.Thread(target=drain_pcm)
            reader.start()
            try:
                deadline = time.monotonic()
                for start in range(0, packets, 375):
                    for seq in range(start, start+375):
                        device.send(seq, raw[seq*1024:(seq+1)*1024])
                    deadline += .1
                    time.sleep(max(0, deadline-time.monotonic()))
                time.sleep(.15)
                p.terminate(); _, log = p.communicate(timeout=4)
                self.assertEqual(p.returncode, 0, log)
                self.assertIn(b"lost=0 stale=0 invalid=0", log)
                self.assertIn(b"socket-drops=0 input-queue-drops=0", log)
                audio.terminate(); _, status = audio.communicate(timeout=4)
                self.assertEqual(audio.returncode, 0, status)
                self.assertIn(b"0 monitor blocks dropped", status)
            finally:
                done.set(); reader.join(1)
            # FIR/resampler startup and 10-ms PCM packet rounding account for
            # <20 ms. A dropped 100-ms burst must fail this bound.
            self.assertGreaterEqual(len(output)//2, int(2.98*48000))
            self.assertLessEqual(len(output)//2, int(3.02*48000))
            samples = struct.unpack('<'+'h'*(len(output)//2), output)
            # Every 20-ms slice after startup retains the tone amplitude.
            for start in range(4800, len(samples)-960, 960):
                chunk = samples[start:start+960]
                amplitude = 2*abs(sum(x*complex(math.cos(2*math.pi*i/48), math.sin(2*math.pi*i/48))
                                      for i,x in enumerate(chunk)))/960/32768
                self.assertGreater(amplitude, .08)

    def test_gui_pipeline_and_legacy_config_defaults(self):
        cfg = configparser.ConfigParser()
        cfg.read(ROOT / "linrad-dvb.ini")
        cfg.remove_section("afedri")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"config.ini"
            with path.open("w") as f:
                cfg.write(f)
            p = subprocess.run([ROOT/"build/linrad_dvb", "--config", path, "--source", "afedri",
                                "--standard", "DVB-T", "--sr", "40000", "--print-pipeline"],
                               capture_output=True, text=True, timeout=4)
            self.assertEqual(p.returncode, 0, p.stderr)
            for part in ("'--source' 'afedri'", "'--ip' '192.168.0.118'", "'--port' '50005'",
                         "'--bind' '0.0.0.0'", "'--sample-rate' '1010526'", "'--gain' '14'"):
                self.assertIn(part, p.stdout)

    def test_bad_settings_fail_before_connect(self):
        for flag, value in (("--gain", "0"), ("--port", "0"), ("--port", "50005.5"),
                            ("--sample-rate", "47000"), ("--frequency", "36000000")):
            p = subprocess.run([RX, "--source", "afedri", "--sample-rate", "200000",
                                "--frequency", "28000000", "--gain", "14", flag, value],
                               capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertNotIn(b"Connecting", p.stderr)


if __name__ == "__main__":
    unittest.main()
