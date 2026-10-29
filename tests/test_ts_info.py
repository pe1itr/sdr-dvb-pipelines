#!/usr/bin/env python3
"""Synthetic DVB SI and real Hamming/parity-coded DVB Teletext PES fixtures."""
import collections
import datetime
import json
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/ts_info_probe'


def crc(data):
    result = 0xffffffff
    for value in data:
        result ^= value << 24
        for _ in range(8):
            result = ((result << 1) ^ (0x04c11db7 if result & 0x80000000 else 0)) & 0xffffffff
    return result


def section(table, ident, body, version=0, number=0, last=0):
    data = bytes([table]) + struct.pack('>H', 0xb000 | (9 + len(body)))
    data += struct.pack('>HBBB', ident, 0xc1 | (version << 1), number, last) + body
    return data + struct.pack('>I', crc(data))


class Stream:
    def __init__(self):
        self.cc = collections.defaultdict(int)

    def packet(self, pid, data, start=True):
        assert len(data) <= 184
        result = bytes([0x47, (pid >> 8) | (0x40 if start else 0), pid & 255,
                        0x10 | self.cc[pid]]) + data + b'\xff' * (184-len(data))
        self.cc[pid] = (self.cc[pid]+1) % 16
        return result

    def table(self, pid, data):
        data = b'\0' + data
        return b''.join(self.packet(pid, data[i:i+184], i == 0) for i in range(0, len(data), 184))

    def pat(self, services=((1, 0x1000),), version=0):
        return self.table(0, section(0, 42, b''.join(struct.pack('>HH', sid, 0xe000 | pid)
                                                   for sid, pid in services), version))

    def sdt(self):
        desc = b'\x48\x0f\x01\x04TEST\x08PE1ITR A'
        return self.table(0x11, section(0x42, 42, b'\0\1\xff' + struct.pack('>HBH', 1, 3, 0x8000 | len(desc)) + desc))

    def pmt(self, teletext=True, service=1, pid=0x1000, ttx_pid=0x200):
        desc = b'\x56\x05eng\x09\x00' if teletext else b''
        body = struct.pack('>HH', 0xe100, 0xf000)
        body += b'\x1b\xe1\0\xf0\0'
        if teletext:
            body += bytes([6]) + struct.pack('>HH', 0xe000 | ttx_pid, 0xf000 | len(desc)) + desc
        return self.table(pid, section(2, service, body))

    def eit(self, title=b'\x15Evening test', text=b'\x15Description', service=1, number=0, version=0,
            extended=None, other=False, empty=False, timestamp=None, duration=b'\x00\x30\x00'):
        desc = b'eng' + bytes([len(title)]) + title + bytes([len(text)]) + text
        descriptors = bytes([0x4d, len(desc)]) + desc
        if extended:
            for index, text in extended:
                value = bytes([(index << 4) | (len(extended)-1)]) + b'eng\0' + bytes([len(text)]) + text
                descriptors += bytes([0x4e, len(value)]) + value
        if timestamp is None:
            timestamp = struct.pack('>H', (datetime.date(2026, 10, 29)-datetime.date(1858, 11, 17)).days) + b'\x18\x30\x00'
        event = struct.pack('>H', 77) + timestamp + duration + struct.pack('>H', 0x8000 | len(descriptors)) + descriptors
        body = struct.pack('>HHBB', 42, 1, 1, 0x4e) + (b'' if empty else event)
        return self.table(0x12, section(0x4f if other else 0x4e, service, body, version, number, 1))

    def teletext(self, page=0x100, message='CLASSIC TELETEXT', pid=0x200):
        # Hamming(8,4), odd parity, and DVB transmission bit order.
        ham = [0x15, 0x02, 0x49, 0x5e, 0x64, 0x73, 0x38, 0x2f,
               0xd0, 0xc7, 0x8c, 0x9b, 0xa1, 0xb6, 0xfd, 0xea]
        def address(row):
            a = (row << 3) | ((page >> 8) & 7)
            return bytes([ham[a & 15], ham[a >> 4]])
        def parity(value):
            return value | (0x80 if value.bit_count() % 2 == 0 else 0)
        def header(number):
            fields = [number & 15, (number >> 4) & 15, 0, 8, 0, 0, 0, 0]
            return address(0) + bytes(ham[x] for x in fields) + bytes(parity(x) for x in b'PE1ITR   TELETEXT 100   18:30:00 '.ljust(32)[:32])
        lines = [header(page)]
        for row in range(1, 25):
            if row == 1:
                text = b'\x01 RED  \x02 GREEN  \x04 BLUE  \x03 YELLOW'
            elif row == 3:
                text = b'\x0d' + message.encode('ascii')  # double height
            elif row == 6:
                text = b'\x11' + bytes([0x7f]) * 12 + b'\x12' + bytes([0x7f]) * 12
            else:
                text = b' '
            lines.append(address(row) + bytes(parity(x) for x in text.ljust(40)[:40]))
        lines.append(header(page+1))  # terminates previous page
        lines += [header(page+1)] * ((-len(lines)) % 3)
        output = b''
        for at in range(0, len(lines), 3):
            data = b'\x10'
            for i, line in enumerate(lines[at:at+3]):
                assert len(line) == 42, (len(line), line)
                rev = bytes(int(f'{x:08b}'[::-1], 2) for x in line)
                data += bytes([2, 44, 0x27+i, 0xe4]) + rev
            pes = b'\0\0\1\xbd\0\xb2\x80\0\x24' + b'\xff' * 36 + data
            assert len(pes) == 184
            output += self.packet(pid, pes)
        return output


class InformationTests(unittest.TestCase):
    def run_streams(self, *actions):
        with tempfile.TemporaryDirectory() as tmp:
            args = [str(PROBE)]
            for i, action in enumerate(actions):
                if isinstance(action, str):
                    args.append(action)
                else:
                    path = pathlib.Path(tmp, f'{i}.ts'); path.write_bytes(action); args.append(str(path))
            result = subprocess.run(args, check=True, capture_output=True, text=True, timeout=15)
            return [json.loads(line) for line in result.stdout.splitlines()]

    def test_background_relay_toggles_reset_and_overflow(self):
        t=Stream()
        with tempfile.TemporaryDirectory() as tmp:
            source=pathlib.Path(tmp, 'source.ts')
            source.write_bytes(t.pat()+t.pmt()+t.eit()+t.teletext()+t.teletext())
            subprocess.run([ROOT / 'build/ts_info_relay', source, pathlib.Path(tmp, 'output.ts')],
                           check=True, capture_output=True, timeout=15)

    def test_eit_and_extended_text(self):
        t = Stream()
        result, = self.run_streams(t.pat()+t.sdt()+t.pmt()+t.eit(title='\x15Café <live>'.encode(),
            extended=[(1, b'\x15second'), (0, b'\x15first ')])+t.eit(number=1, title=b'\x15Next'))
        self.assertEqual(result['services'][0]['name'], 'PE1ITR A')
        self.assertEqual(result['events'][0]['title'], 'Café <live>')
        self.assertEqual(result['events'][0]['text'], 'Description\n\nfirst second')
        self.assertEqual(result['events'][1]['title'], 'Next')
        self.assertEqual(result['events'][0]['duration'], 1800)
        self.assertEqual(result['events'][0]['start'], int(datetime.datetime(2026, 10, 29, 18, 30, tzinfo=datetime.timezone.utc).timestamp()))

    def test_filter_crc_version_and_reset(self):
        t = Stream()
        initial = t.pat()+t.pmt()+t.eit()+t.eit(number=1)
        wrong = t.eit(service=2)+t.eit(other=True)
        bad = bytearray(t.eit(title=b'\x15Bad')); bad[30] ^= 1
        results = self.run_streams(initial, wrong+bad, t.eit(title=b'\x15Updated', version=1), '--reset')
        self.assertEqual(results[0]['events'], results[1]['events'])
        self.assertEqual(results[2]['events'][0]['title'], 'Updated')
        self.assertFalse(results[2]['events'][1]['valid'])
        self.assertFalse(results[3]['eit_received'])
        self.assertFalse(results[3]['services'])

    def test_text_encodings_and_unknown_time(self):
        t = Stream()
        results = self.run_streams(t.pat()+t.eit(title=b'Caf\xc2e', text=b'A\x86bold\x87\x8aB', timestamp=b'\xff'*5, duration=b'\xff'*3),
                                  t.eit(title=b'\x11'+ 'Test Ω'.encode('utf-16-be')))
        self.assertEqual(results[0]['events'][0]['title'], 'Café')
        self.assertEqual(results[0]['events'][0]['text'], 'Abold\nB')
        self.assertEqual(results[0]['events'][0]['start'], -1)
        self.assertEqual(results[0]['events'][0]['duration'], -1)
        self.assertEqual(results[1]['events'][0]['title'], 'Test Ω')

    def test_fragment_loss_duplicate_and_recovery(self):
        t = Stream()
        initial=t.pat()+t.eit(text=b'\x15'+b'X'*220)
        # Duplicate first packet is legal; a continuity gap must discard an incomplete section.
        long=t.eit(text=b'\x15'+b'Y'*220)
        lost=t.eit(text=b'\x15'+b'Z'*220)
        result=self.run_streams(initial, long[:188]+long, lost[188:], t.eit(text=b'\x15Recovered'))
        self.assertEqual(result[0]['events'][0]['text'], 'X'*220)
        self.assertEqual(result[1]['events'][0]['text'], 'Y'*220)
        self.assertEqual(result[2]['events'][0]['text'], 'Y'*220)
        self.assertEqual(result[3]['events'][0]['text'], 'Recovered')

    def test_sync_recovery_and_section_pointer(self):
        t=Stream()
        initial=t.pat()+t.eit(text=b'\x15'+b'X'*220)
        # Split a section across packets, then finish it using pointer_field and
        # append a complete next section in the very same payload.
        long=t.eit(text=b'\x15'+b'Y'*220)
        next_table=t.eit(number=1, title=b'After pointer')
        tail=long[188+4:].rstrip(b'\xff')
        next_section=next_table[5:].rstrip(b'\xff')
        combined=bytes([len(tail)])+tail+next_section
        self.assertLessEqual(len(combined), 184)
        # Use the original second packet's continuity count.
        packet=bytes([0x47, 0x40, 0x12, long[191]])+combined+b'\xff'*(184-len(combined))
        # Force actual loss of byte alignment; subsequent sections must recover.
        broken=t.eit(text=b'\x15'+b'Z'*220)
        broken=broken[:100]+b'noise'+broken[100:]
        result=self.run_streams(b'junk\x47bad prefix'+initial, long[:188]+packet,
            broken+t.pat()+t.eit(title=b'Recovered'))
        self.assertEqual(result[0]['events'][0]['text'], 'X'*220)
        self.assertEqual(result[1]['events'][0]['text'], 'Y'*220)
        self.assertEqual(result[1]['events'][1]['title'], 'After pointer')
        self.assertEqual(result[2]['events'][0]['title'], 'Recovered')

    def test_services_and_pmt_changes(self):
        t = Stream()
        result = self.run_streams(t.pat(((1, 0x1000), (2, 0x1001)))+t.pmt()+t.eit(), '--select=2',
            t.pmt(False, 2, 0x1001)+t.eit(service=2, title=b'\x15Service two'), t.pat(((3, 0x1002),), version=1))
        self.assertEqual(result[1]['selected'], 2)
        self.assertFalse(result[1]['eit_received'])
        self.assertFalse(result[2]['teletext'])
        self.assertEqual(result[2]['events'][0]['title'], 'Service two')
        self.assertEqual(result[3]['selected'], 3)
        self.assertFalse(result[3]['events'][0]['valid'])

    def test_classic_teletext_page_filter_updates_gap_and_removal(self):
        t=Stream()
        result=self.run_streams(t.pat()+t.pmt()+t.teletext(0x101),
            t.teletext()+t.teletext(), t.teletext(message='UPDATED PAGE')+t.teletext(message='UPDATED PAGE'),
            '--gap', t.pmt(False)+t.packet(0x1fff, b''))
        self.assertTrue(result[0]['decoder'], 'Test requires FFmpeg with libzvbi')
        self.assertTrue(result[0]['teletext'])
        self.assertFalse(result[0]['teletext_received'])
        self.assertGreater(result[1]['width'], 400)
        self.assertGreaterEqual(result[1]['height'], 250)
        self.assertEqual(result[1]['colors'], 15)
        self.assertNotEqual(result[1]['page_hash'], result[2]['page_hash'])
        self.assertEqual(result[2]['page_hash'], result[3]['page_hash'])
        self.assertFalse(result[4]['teletext'])
        self.assertFalse(result[4]['teletext_received'])


if __name__ == '__main__':
    unittest.main()
