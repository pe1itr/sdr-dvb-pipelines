"""USB DSP, TCI transport, and DVB isolation regressions (no RF hardware)."""
import array
import errno
import base64
import ctypes
import mmap
import hashlib
import math
import os
from pathlib import Path
import socket
import shutil
import struct
import subprocess
import tempfile
import time
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]


def amplitude(audio, hz, rate=48000):
    # Coherent one-second measurement after filter/AGC startup.
    audio = audio[-48000:]
    re = sum(v * math.cos(2 * math.pi * hz * i / rate) for i, v in enumerate(audio))
    im = sum(v * math.sin(2 * math.pi * hz * i / rate) for i, v in enumerate(audio))
    return 2 * math.hypot(re, im) / len(audio)


def packet_iq(directory, baud=9600):
    original = Path(directory, 'packet.wav')
    message = f'N0CALL>APDW:PCM {baud} external modem regression\n'.encode()
    subprocess.run(['gen_packets', '-B', str(baud), '-r', '48000', '-a', '100',
                    '-o', str(original), '-'], input=message,
                   capture_output=True, timeout=10, check=True)
    with wave.open(str(original), 'rb') as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 48000)
        samples = struct.unpack('<' + 'h' * wav.getnframes(), wav.readframes(wav.getnframes()))
    # Real AFSK/G3RUH waveform with +/-2.5 / +/-3 kHz deviation. Carrier before
    # and after the packet lets FIR/resampler startup and tail settle.
    phase = 0.0
    deviation = 2500 if baud == 1200 else 3000
    iq = bytearray()
    for sample in (*([0] * 4800), *samples, *([0] * 4800)):
        phase += 2 * math.pi * deviation * (sample / 32768) / 48000
        iq.extend(struct.pack('<hh', round(12000 * math.cos(phase)),
                              round(12000 * math.sin(phase))))
    return iq, message


class DemodTests(unittest.TestCase):
    def test_usb_filter_resampling_and_alias_rejection(self):
        for rate, offset, u8 in ((8138, 0, 0), (158730, 12345, 0),
                                (2000000, -125000, 1), (10000000, 456789, 0)):
            with self.subTest(rate=rate, offset=offset, u8=u8):
                p = subprocess.run([ROOT / 'build/usb_demod_probe', str(rate), str(offset), str(u8)],
                                   capture_output=True, timeout=30, check=True)
                audio = array.array('f', p.stdout)
                self.assertGreater(len(audio), 56000)
                self.assertLess(len(audio), 57601)
                self.assertTrue(all(math.isfinite(x) and abs(x) <= .951 for x in audio))
                wanted = amplitude(audio, 1000)
                self.assertGreater(wanted, .05)
                for rejected in (1800, 5000, 2300):
                    self.assertLess(amplitude(audio, rejected), wanted * .01,
                                    f'{rejected} Hz rejection below 40 dB at {rate} samples/s')

    def test_fm_voice_and_am_rejection(self):
        for rate, offset, u8 in ((12500, 0, 0), (48000, 10000, 0),
                                (158730, -25000, 0), (2000000, 125000, 1)):
            with self.subTest(rate=rate, offset=offset, u8=u8):
                p = subprocess.run([ROOT / 'build/usb_demod_probe', str(rate), str(offset), str(u8), 'fm'],
                                   capture_output=True, timeout=30, check=True)
                audio = array.array('f', p.stdout)
                self.assertGreater(len(audio), 56000)
                self.assertTrue(all(math.isfinite(x) and abs(x) <= .951 for x in audio))
                wanted = amplitude(audio, 1000)
                self.assertGreater(wanted, .15)
                self.assertLess(wanted, .6)
                for unwanted in (1800, 2000):
                    self.assertLess(amplitude(audio, unwanted), wanted * .03)

    def test_fm_voice_lowpass_and_silence(self):
        for tone in (0, -1, 6000):
            p = subprocess.run([ROOT / 'build/usb_demod_probe', '48000', '0', '0', 'fm', str(tone)],
                               capture_output=True, timeout=10, check=True)
            audio = array.array('f', p.stdout)[-48000:]
            self.assertLess(math.sqrt(sum(x*x for x in audio)/len(audio)), .01)
        for rate, offset in ((8138, 0), (48000, 20000), (48000, -20000)):
            p = subprocess.run([ROOT / 'build/usb_demod_probe', str(rate), str(offset), '0', 'fm'],
                               capture_output=True)
            self.assertNotEqual(p.returncode, 0)

    def test_fm_data_flat_response_dc_and_limits(self):
        # Constant +/-2.5 kHz deviation must retain its level at low frequencies
        # and above the speech passband, for both sample formats and resampling.
        for rate, offset, u8 in ((24000, 0, 0), (48000, 5000, 0),
                                (158730, -25000, 0), (2000000, 125000, 1)):
            for tone in (50, 1000, 4800, 5000, 0, -1):
                with self.subTest(rate=rate, tone=tone, u8=u8):
                    p = subprocess.run([ROOT / 'build/usb_demod_probe', str(rate),
                                        str(offset), str(u8), 'fm-data', str(tone)],
                                       capture_output=True, timeout=30, check=True)
                    audio = array.array('f', p.stdout)[-48000:]
                    self.assertEqual(len(audio), 48000)
                    self.assertTrue(all(math.isfinite(x) and abs(x) <= .951 for x in audio))
                    if tone > 0:
                        self.assertAlmostEqual(amplitude(audio, tone), .125, delta=.0125)
                        self.assertLess(amplitude(audio, 1800), .004)
                    elif tone == 0:
                        # A carrier 500 Hz off tune produces a constant level:
                        # no hidden DC blocker / speech high-pass in DATA.
                        self.assertAlmostEqual(sum(audio)/len(audio), .025, delta=.001)
                    else:
                        self.assertLess(max(abs(x) for x in audio), 1e-6)
        for rate, offset in ((12500, 0), (48000, 15000), (48000, -15000)):
            p = subprocess.run([ROOT / 'build/usb_demod_probe', str(rate), str(offset), '0', 'fm-data'],
                               capture_output=True)
            self.assertNotEqual(p.returncode, 0)

    @unittest.skipUnless(shutil.which('gen_packets') and shutil.which('atest'),
                         'optional Dire Wolf packet test tools not installed')
    def test_fm_data_9600_packet_decoded_by_external_modem(self):
        with tempfile.TemporaryDirectory() as tmp:
            received = Path(tmp, 'received.wav')
            iq, message = packet_iq(tmp)
            p = subprocess.run([ROOT / 'build/usb_demod_probe', 'fm-data-iq'], input=iq,
                               capture_output=True, timeout=10, check=True)
            audio = array.array('f', p.stdout)
            with wave.open(str(received), 'wb') as wav:
                wav.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
                wav.writeframes(b''.join(struct.pack('<h', round(x * 32767)) for x in audio))
            result = subprocess.run(['atest', '-B', '9600', '-F', '0', '-L', '1', str(received)],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode())
            self.assertIn(message.strip(), result.stdout)

    def test_invalid_band_is_rejected(self):
        for rate, offset in ((7000, 0), (48000, 23000), (48000, -25000)):
            p = subprocess.run([ROOT / 'build/usb_demod_probe', str(rate), str(offset), '0'],
                               capture_output=True)
            self.assertNotEqual(p.returncode, 0)


class IsolationTests(unittest.TestCase):
    def test_full_usb_queue_preserves_dvb_bytes_and_realtime(self):
        class Block(ctypes.Structure):
            _fields_ = [('rate', ctypes.c_double), ('sequence', ctypes.c_uint64),
                        ('time_us', ctypes.c_int64), ('samples', ctypes.c_uint),
                        ('u8', ctypes.c_uint), ('iq', ctypes.c_ubyte * (8192 * 4))]

        class Tap(ctypes.Structure):
            _fields_ = [('magic', ctypes.c_uint), ('enabled', ctypes.c_uint),
                        ('write_index', ctypes.c_uint), ('read_index', ctypes.c_uint),
                        ('dropped', ctypes.c_uint), ('blocks', Block * 128)]

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'tap')
            with path.open('w+b') as f:
                f.truncate(ctypes.sizeof(Tap))
                with mmap.mmap(f.fileno(), ctypes.sizeof(Tap)) as memory:
                    tap = Tap.from_buffer(memory)
                    tap.magic = 0x55534231
                    tap.enabled = 1
                    raw = bytes(range(256)) * 1500  # two seconds at 48 kHz s16
                    iq = Path(tmp, 'input.iq')
                    iq.write_bytes(raw)
                    args = [ROOT / 'build/iq_file_to_stdout', '--in', iq,
                            '--input-format', 's16', '--sample-rate', '48000']
                    start = time.monotonic()
                    p = subprocess.run(args, env=dict(os.environ, LINRAD_USB_TAP=str(path)),
                                       capture_output=True, check=True, timeout=4)
                    self.assertEqual(p.stdout, raw)
                    self.assertLess(time.monotonic() - start, 3)
                    self.assertEqual(tap.write_index, 128)
                    self.assertGreater(tap.dropped, 0)
                    self.assertEqual(tap.blocks[0].rate, 48000)
                    # A separate DVB filter does not affect the USB tap.
                    tap.write_index = 0
                    p = subprocess.run(args + ['--filter-bandwidth', '10000'],
                                       env=dict(os.environ, LINRAD_USB_TAP=str(path)),
                                       capture_output=True, check=True, timeout=4)
                    self.assertNotEqual(p.stdout, raw)
                    n = tap.blocks[0].samples * 4
                    self.assertEqual(bytes(tap.blocks[0].iq[:n]), raw[:n])
                    del tap


class WebSocket:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=2)
        self.buf = b''
        key = base64.b64encode(b'0123456789abcdef')
        self.s.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                       b'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ' + key + b'\r\n\r\n')
        while b'\r\n\r\n' not in self.buf:
            self.buf += self.s.recv(4096)
        header, self.buf = self.buf.split(b'\r\n\r\n', 1)
        expected = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
        assert b'101 Switching Protocols' in header and expected in header

    def close(self):
        self.s.close()

    def read(self, count):
        while len(self.buf) < count:
            data = self.s.recv(65536)
            if not data:
                raise EOFError()
            self.buf += data
        data, self.buf = self.buf[:count], self.buf[count:]
        return data

    def receive(self):
        a, b = self.read(2)
        assert not b & 128
        size = b & 127
        if size == 126:
            size, = struct.unpack('!H', self.read(2))
        elif size == 127:
            size, = struct.unpack('!Q', self.read(8))
        return a & 15, self.read(size)

    def send(self, message, opcode=1, fin=True):
        data = message.encode() if isinstance(message, str) else message
        mask = b'1234'
        header = bytes([(128 if fin else 0) | opcode])
        header += bytes([128 | len(data)]) if len(data) < 126 else b'\xfe' + struct.pack('!H', len(data))
        self.s.sendall(header + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(data)))

    def until_text(self, wanted):
        for _ in range(150):
            kind, data = self.receive()
            if kind == 1 and wanted in data:
                return data
        raise AssertionError(f'Missing TCI reply {wanted!r}')

    def audio(self):
        for _ in range(150):
            kind, data = self.receive()
            if kind == 2:
                return struct.unpack('<16I', data[:64]), data[64:]
        raise AssertionError('No audio frame')


class TciTests(unittest.TestCase):
    def setUp(self):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            self.port = s.getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', 0))
        self.udp.settimeout(2)
        self.addCleanup(self.udp.close)
        self.process = subprocess.Popen([ROOT / 'build/usb_receiver_probe', str(self.port),
                                         str(self.udp.getsockname()[1])],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(self.process.stdout.readline(), b'ready\n')
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.process.terminate()
        _, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, err.decode())

    def connect(self):
        c = WebSocket(self.port)
        self.clients.append(c)
        self.assertIn(b'receive_only:true;', c.until_text(b'protocol:ExpertSDR3,1.9.0;'))
        c.until_text(b'ready;')
        return c

    def command(self, command):
        self.process.stdin.write(command)
        self.process.stdin.flush()
        self.assertEqual(self.process.stdout.readline(), command + b'\n')

    def udp_audio(self, packets=100):
        audio = []
        for _ in range(packets):
            packet = self.udp.recv(4096)
            self.assertEqual(len(packet), 960)  # 10 ms, raw s16le, no header.
            audio.extend(x / 32767 for x in struct.unpack('<480h', packet))
        return audio

    def assert_udp_stopped(self):
        # Drop datagrams already in the kernel queue before the disable ack.
        self.udp.setblocking(False)
        try:
            while True:
                self.udp.recv(4096)
        except BlockingIOError:
            pass
        self.udp.settimeout(.15)
        with self.assertRaises(socket.timeout):
            self.udp.recv(4096)
        self.udp.settimeout(2)

    def test_pcm_udp_enable_mute_modes_and_restart(self):
        self.assert_udp_stopped()  # Default off, even while monitor runs.
        self.command(b'v')  # Invalid addresses / ports must not enable it.
        c = self.connect()
        for mode in (b'u', b'f', b'd'):
            self.command(mode)
            self.command(b'e')
            self.command(b'm')
            self.command(b'v')  # Invalid updates preserve working output.
            # Discard startup, then measure exactly one second of mono audio.
            self.udp_audio(20)
            values = self.udp_audio()
            self.assertGreater(amplitude(values, 1000), .08)
            self.assertLess(max(abs(x) for x in values), .951)
            if mode == b'd':
                self.assertAlmostEqual(amplitude(values, 1000), .125, delta=.005)
            c.send('audio_start:0;')
            self.assertEqual(c.audio()[0][1], 48000)
            c.send('audio_stop:0;')
            c.until_text(b'audio_stop:0;')
            self.command(b'x')
            self.assert_udp_stopped()
            c.send('vfo:0,0;')
            c.until_text(b'vfo:0,0,144600000;')
        self.command(b'e')
        self.command(b's')
        self.assert_udp_stopped()
        self.command(b'r')  # Armed PCM resumes with monitor, still muted.
        self.udp_audio(20)
        self.assertGreater(amplitude(self.udp_audio(), 1000), .08)

    def test_unread_and_absent_pcm_consumer_do_not_block_tci(self):
        self.command(b'e')
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        c = self.connect()
        c.send('audio_start:0;')
        for absent in (False, True):
            if absent:
                self.udp.close()
            until = time.monotonic() + 1.5
            count = 0
            while time.monotonic() < until:
                c.audio()
                count += 1
            self.assertGreater(count, 20)
            self.assertIsNone(self.process.poll())

    @unittest.skipUnless(shutil.which('gen_packets') and shutil.which('direwolf'),
                         'optional Dire Wolf tools not installed')
    def test_packet_through_live_pcm_udp_to_direwolf(self):
        for baud, mode in ((1200, b'f'), (9600, b'd')):
            with self.subTest(baud=baud):
                self.check_packet_to_direwolf(baud, mode)

    def check_packet_to_direwolf(self, baud, mode):
        self.process.terminate()
        self.process.communicate(timeout=5)
        with tempfile.TemporaryDirectory() as tmp:
            iq, message = packet_iq(tmp, baud)
            path = Path(tmp, 'packet.iq')
            path.write_bytes(iq)
            config = Path(tmp, 'direwolf.conf')
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
                reserve.bind(('127.0.0.1', 0))
                port = reserve.getsockname()[1]
            config.write_text((ROOT / 'direwolf-pcm.conf').read_text().replace('UDP:7355', f'UDP:{port}'))
            with Path(tmp, 'direwolf.log').open('w+b') as log:
                modem = subprocess.Popen(['direwolf', '-t', '0', '-c', str(config),
                                          '-B', str(baud)],
                                         stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 5
                    while True:
                        self.assertIsNone(modem.poll())
                        try:
                            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as check:
                                check.bind(('127.0.0.1', port))
                        except OSError as exc:
                            if exc.errno == errno.EADDRINUSE:
                                break
                            raise
                        self.assertLess(time.monotonic(), deadline, 'Dire Wolf did not bind its UDP input')
                        time.sleep(.02)
                    self.process = subprocess.Popen([ROOT / 'build/usb_receiver_probe', str(self.port),
                                                     str(port), str(path)], stdin=subprocess.PIPE,
                                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    self.assertEqual(self.process.stdout.readline(), b'ready\n')
                    for command in (mode, b'e', b'm', b'i'):
                        self.command(command)
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        log.seek(0)
                        output = log.read()
                        if message.strip() in output:
                            break
                        time.sleep(.05)
                    self.assertIn(message.strip(), output, output.decode(errors='replace'))
                finally:
                    modem.terminate()
                    modem.wait(timeout=5)

    def test_audio_frequency_negotiation_fragmentation_and_receive_only(self):
        c = self.connect()
        c.send('AUDIO_START:0;')
        h, payload = c.audio()
        self.assertEqual(h[:8], (0, 48000, 3, 0, 0, 2048, 1, 1))
        self.assertEqual(h[8:], (0,) * 8)
        self.assertEqual(len(payload), 8192)
        values = struct.unpack('<2048f', payload)
        self.assertGreater(max(abs(v) for v in values), .05)
        self.assertTrue(all(math.isfinite(v) and abs(v) <= .951 for v in values))
        c.send('audio_stop:0;')
        c.until_text(b'audio_stop:0;')
        c.send('audio_samplerate:24000;audio_stream_channels:2;audio_stream_sample_type:int16;'
               'audio_stream_samples:512;audio_start:0;')
        h, payload = c.audio()
        self.assertEqual(h[:8], (0, 24000, 0, 0, 0, 512, 1, 2))
        self.assertEqual(len(payload), 2048)
        values = struct.unpack('<1024h', payload)
        self.assertEqual(values[::2], values[1::2])
        c.send('audio_stop:0;')
        c.until_text(b'audio_stop:0;')
        c.send('vfo:0,0,144600', fin=False)
        c.send(b'ping', opcode=9)
        c.send('500;', opcode=0)
        c.until_text(b'vfo:0,0,144600500;')
        c.send('vfo:0,0,145000000;trx:0,true,tci;modulation:0,LSB;')
        c.until_text(b'vfo:0,0,144600500;')  # outside captured spectrum rejected
        c.until_text(b'trx:0,false;')
        c.until_text(b'modulation:0,USB;')
        other = self.connect()
        other.send('vfo:0,0;')
        other.until_text(b'vfo:0,0,144600500;')
        other.send('audio_start:0;')
        shifted = []
        for _ in range(12):
            h, payload = other.audio()
            self.assertEqual(h[1], 48000)
            shifted.extend(struct.unpack('<2048f', payload))
        shifted = shifted[-24000:]
        self.assertGreater(amplitude(shifted, 500), .05)
        self.assertLess(amplitude(shifted, 1000), .005)

    def test_live_fm_switch_and_speaker_mute_keep_tci_audio(self):
        c = self.connect()
        c.send('audio_start:0;')
        c.audio()
        for mode, text, band in ((b'f', b'NFM', b'-6250,6250'), (b'd', b'NFM', b'-10000,10000'),
                                 (b'u', b'USB', b'0,3000'),
                                 (b'f', b'NFM', b'-6250,6250')):
            self.process.stdin.write(mode)
            self.process.stdin.flush()
            reply = c.until_text(b'modulation:0,' + text + b';')
            self.assertIn(b'rx_filter_band:0,' + band + b';', reply)
            self.process.stdin.write(b'm')
            self.process.stdin.flush()
            # A full second of audio while speaker mute is active, on the same
            # WebSocket connection and without another audio_start command.
            values = []
            for _ in range(25):
                header, payload = c.audio()
                self.assertEqual(header[:8], (0, 48000, 3, 0, 0, 2048, 1, 1))
                values.extend(struct.unpack('<2048f', payload))
            self.assertGreater(amplitude(values, 1000), .05)
            self.process.stdin.write(b'a')
            self.process.stdin.flush()
        c.send('vfo:0,0,144620000;')
        c.until_text(b'vfo:0,0,144600000;')  # FM +/-6.25 kHz must fit.
        c.send('modulation:0;')
        c.until_text(b'modulation:0,NFM;')

    def test_fm_data_requires_48k_on_switch_and_negotiation(self):
        c = self.connect()
        c.send('audio_samplerate:12000;audio_start:0;')
        self.assertEqual(c.audio()[0][1], 12000)
        self.process.stdin.write(b'd')
        self.process.stdin.flush()
        c.until_text(b'audio_samplerate:48000;')
        c.until_text(b'rx_filter_band:0,-10000,10000;')
        self.assertEqual(c.audio()[0][1], 48000)
        for rate in (12000, 24000):
            c.send(f'audio_samplerate:{rate};')
            c.until_text(b'audio_samplerate:48000;')
            self.assertEqual(c.audio()[0][1], 48000)
        # New connections must also see the DATA filter and 48 kHz audio.
        other = self.connect()
        other.send('rx_filter_band:0;audio_start:0;')
        other.until_text(b'rx_filter_band:0,-10000,10000;')
        self.assertEqual(other.audio()[0][1], 48000)
        self.process.stdin.write(b'f')
        self.process.stdin.flush()
        c.until_text(b'rx_filter_band:0,-6250,6250;')
        c.send('audio_samplerate:24000;')
        c.until_text(b'audio_samplerate:24000;')
        self.assertEqual(c.audio()[0][1], 24000)

    def test_busy_port_and_malformed_client_do_not_stop_receiver(self):
        p = subprocess.run([ROOT / 'build/usb_receiver_probe', str(self.port)],
                           capture_output=True, timeout=5)
        self.assertNotEqual(p.returncode, 0)
        c = self.connect()
        c.s.sendall(b'\x81\x01x')  # client frame without required masking
        with self.assertRaises(EOFError):
            while True:
                c.receive()
        good = self.connect()
        good.send('audio_start:0;')
        self.assertEqual(good.audio()[0][6], 1)
        self.assertIsNone(self.process.poll())

    def test_slow_client_does_not_delay_other_client(self):
        slow = self.connect()
        slow.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow.send('audio_stream_channels:2;audio_start:0;')
        good = self.connect()
        good.send('audio_start:0;')
        until = time.monotonic() + 3
        count = 0
        while time.monotonic() < until:
            good.audio()
            count += 1
        self.assertGreater(count, 45)


if __name__ == '__main__':
    unittest.main(verbosity=2)
