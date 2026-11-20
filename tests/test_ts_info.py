#!/usr/bin/env python3
"""Synthetic DVB SI and real Hamming/parity-coded DVB Teletext PES fixtures."""
import collections
import datetime
import json
import os
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

    def sdt(self, service=1, provider=b'TEST', name=b'PE1ITR A'):
        value = b'\x01' + bytes([len(provider)]) + provider + bytes([len(name)]) + name
        desc = bytes([0x48, len(value)]) + value
        return self.table(0x11, section(0x42, 42, b'\0\1\xff' + struct.pack('>HBH', service, 3, 0x8000 | len(desc)) + desc))

    def pmt(self, teletext=True, service=1, pid=0x1000, ttx_pid=0x200, video=0x1b, audio=None):
        desc = b'\x56\x05eng\x09\x00' if teletext else b''
        body = struct.pack('>HH', 0xe100, 0xf000)
        if video is not None:
            body += bytes([video])+b'\xe1\0\xf0\0'
        if audio is not None:
            body += bytes([audio])+b'\xe1\x01\xf0\0'
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

    def teletext(self, page=0x100, message='CLASSIC TELETEXT', pid=0x200, rows=None, national=0, flags=0, unit_id=2, field=1):
        # Hamming(8,4), odd parity, and DVB transmission bit order.
        ham = [0x15, 0x02, 0x49, 0x5e, 0x64, 0x73, 0x38, 0x2f,
               0xd0, 0xc7, 0x8c, 0x9b, 0xa1, 0xb6, 0xfd, 0xea]
        def address(row):
            a = (row << 3) | ((page >> 8) & 7)
            return bytes([ham[a & 15], ham[a >> 4]])
        def parity(value):
            return value | (0x80 if value.bit_count() % 2 == 0 else 0)
        def header(number):
            fields = [number & 15, (number >> 4) & 15, 0, 8, 0, 0, flags, national << 1]
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
            if rows is not None:
                text = rows.get(row, b" ")
            lines.append(address(row) + bytes(parity(x) for x in text.ljust(40)[:40]))
        lines.append(header(page+1))  # terminates previous page
        lines += [header(page+1)] * ((-len(lines)) % 3)
        output = b''
        for at in range(0, len(lines), 3):
            data = b'\x10'
            for i, line in enumerate(lines[at:at+3]):
                assert len(line) == 42, (len(line), line)
                rev = bytes(int(f'{x:08b}'[::-1], 2) for x in line)
                data += bytes([unit_id, 44, 0xc0 | (field << 5) | (7+i), 0xe4]) + rev
            pes = b'\0\0\1\xbd\0\xb2\x84\0\x24' + b'\xff' * 36 + data
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

    def frames(self, rows, **options):
        t=Stream()
        data=t.pat()+t.pmt()+t.teletext(rows=rows, **options)+t.teletext(rows=rows, **options)
        with tempfile.TemporaryDirectory() as tmp:
            source=pathlib.Path(tmp, 'page.ts'); source.write_bytes(data)
            target=pathlib.Path(tmp, 'page.ppm')
            result=subprocess.run([PROBE, source], env=dict(os.environ, TELETEXT_PPM=str(target)),
                                  check=True, capture_output=True, text=True)
            info=json.loads(result.stdout)
            self.assertEqual((info['width'], info['height']), (480, 250))
            frames=[]
            for suffix in ('', '.off', '.reveal', '.reveal-off'):
                header, size, maximum, pixels=pathlib.Path(str(target)+suffix).read_bytes().split(b'\n',3)
                self.assertEqual((header,size,maximum), (b'P6',b'480 250',b'255'))
                self.assertEqual(len(pixels),480*250*3)
                frames.append(pixels)
            return frames

    @staticmethod
    def cell(frame, row, col, width=1, height=1):
        return b''.join(frame[((row*10+y)*480+col*12)*3:((row*10+y)*480+(col+width)*12)*3]
                        for y in range(height*10))

    def test_conceal_and_flash_are_independent(self):
        on, off, reveal, reveal_off=self.frames({1:b'\x01\x08X\x09Y\x18Z', 2:b'Z'})
        black=bytes(12*10*3)
        self.assertNotEqual(self.cell(on,1,2),black)
        self.assertEqual(self.cell(off,1,2),black)
        self.assertEqual(self.cell(on,1,4),self.cell(off,1,4))
        self.assertNotEqual(self.cell(off,1,4),black)
        self.assertEqual(self.cell(on,1,6),black)
        self.assertNotEqual(self.cell(reveal,1,6),black)
        self.assertEqual(self.cell(reveal,1,6),self.cell(reveal_off,1,6))
        self.assertEqual(self.cell(off,1,2),self.cell(reveal_off,1,2))
        self.assertNotEqual(self.cell(on,2,0),black)  # attributes reset on each row

    def test_mosaic_hold_separation_and_background_timing(self):
        on, *_=self.frames({1:b'\x11\x7f\x1e\x12\x1f ', 2:b'\x11\x7f\x1a\x7f',
                            3:b'\x01\x1d \x1c '})
        red=bytes((255,0,0))*120; green=bytes((0,255,0))*120; black=bytes(360)
        for col in (1,2,3): self.assertEqual(self.cell(on,1,col),red)
        self.assertEqual(self.cell(on,1,4),green)  # release is set-after
        self.assertEqual(self.cell(on,1,5),black)
        self.assertEqual(self.cell(on,2,1),red)
        separated=self.cell(on,2,3)
        self.assertNotEqual(separated,red)
        self.assertIn(bytes((255,0,0)),separated)
        self.assertEqual(self.cell(on,3,1),red)  # new background is set-at
        self.assertEqual(self.cell(on,3,3),black)

    def test_double_dimensions_and_broadcast_bottom_row(self):
        on, *_=self.frames({1:b' A', 3:b'\x0dA', 6:b'\x0eA', 9:b'\x0fA', 24:b'BOTTOM ROW'})
        normal=self.cell(on,1,1)
        pixels=[normal[y*36:(y+1)*36] for y in range(10)]
        self.assertEqual(self.cell(on,3,1,height=2),b''.join(row*2 for row in pixels))
        wide=[b''.join(row[x:x+3]*2 for x in range(0,36,3)) for row in pixels]
        self.assertEqual(self.cell(on,6,1,width=2),b''.join(wide))
        self.assertEqual(self.cell(on,9,1,width=2,height=2),b''.join(row*2 for row in wide))
        self.assertNotEqual(self.cell(on,24,0,width=10),bytes(3600))

    def test_national_charset_and_header_suppression(self):
        english, *_=self.frames({1:b'#[]'}, national=0)
        german, *_=self.frames({1:b'#[]'}, national=4)
        self.assertNotEqual(self.cell(english,1,0,width=3), self.cell(german,1,0,width=3))
        suppressed, *_=self.frames({1:b'TEXT'}, flags=1)  # C7 suppress header
        self.assertEqual(self.cell(suppressed,0,0,width=40),bytes(14400))
        self.assertNotEqual(self.cell(suppressed,1,0,width=4),bytes(1440))
        inhibited, *_=self.frames({1:b'TEXT'},flags=8)  # C10 inhibit display
        self.assertEqual(inhibited,bytes(480*250*3))

    def test_dvb_subtitle_units_and_second_field(self):
        normal=self.frames({1:b'TEXT'})
        subtitle=self.frames({1:b'TEXT'},unit_id=3,field=0)
        self.assertEqual(normal,subtitle)

    def test_dvb_multits_pes_stuffing_duplicates_and_loss(self):
        t=Stream()
        original=t.teletext()+t.teletext()
        packets=[original[i:i+188] for i in range(0,len(original),188)]
        multi=[]
        for i in range(0,len(packets),2):
            # Six text units and one stuffing unit: a PES spanning two TS packets.
            header=bytearray(packets[i][4:49]); header[4:6]=struct.pack('>H',362)
            payload=header+b'\x1f'+packets[i][50:]+packets[i+1][50:]+b'\xff\x2c'+b'\xff'*44
            self.assertEqual(len(payload),368)
            first=t.packet(0x200,payload[:184]); second=t.packet(0x200,payload[184:],False)
            multi.append(first+first+second)  # legal retransmission of an identical TS packet
        result,=self.run_streams(t.pat()+t.pmt()+b''.join(multi))
        self.assertEqual((result['width'],result['height'],result['colors']), (480,250,15))
        # An incomplete PES must not leak into the next one after a continuity gap.
        t=Stream(); initial=t.pat()+t.pmt()
        header=bytearray(packets[0][4:49]); header[4:6]=struct.pack('>H',362)
        partial=t.packet(0x200,(header+b'\x10'+packets[0][50:])[:184])
        t.cc[0x200]=(t.cc[0x200]+1)%16
        before,after=self.run_streams(initial+partial,t.teletext()+t.teletext())
        self.assertFalse(before['teletext_received'])
        self.assertEqual(after['width'],480)

    def test_invalid_dvb_payload_does_not_publish_a_page(self):
        # Mutate every PES so none of its headers/rows can contribute a page.
        for offset, value in ((4+45,0x20),(4+47,43),(4+49,0),(4+8,35)):
            with self.subTest(offset=offset):
                t=Stream(); page=bytearray(t.teletext()+t.teletext())
                for start in range(0,len(page),188): page[start+offset]=value
                initial, recovered=self.run_streams(t.pat()+t.pmt()+page,
                                                     t.teletext()+t.teletext())
                self.assertFalse(initial['teletext_received'])
                self.assertEqual(recovered['width'],480)

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

    def test_service_provider_survives_pat_and_gap_but_not_service_removal(self):
        t = Stream()
        result = self.run_streams(
            t.pat()+t.sdt(provider=b'0201', name=b'PE1ORG'),
            t.pat(version=1), '--gap', t.pat(version=2),
            t.sdt(provider=b'\x15Caf\xc3\xa9'), t.sdt(provider=b''),
            t.pat(((2, 0x1001),), version=3), '--reset')
        for i in range(4):
            self.assertEqual(result[i]['services'][0]['provider'], '0201')
            self.assertEqual(result[i]['services'][0]['name'], 'PE1ORG')
        self.assertEqual(result[4]['services'][0]['provider'], 'Café')
        self.assertEqual(result[5]['services'][0]['provider'], '')
        self.assertEqual(result[6]['services'][0]['provider'], '')
        self.assertEqual(result[7]['services'], [])

    def test_codecs_follow_selected_pmt_and_clear_on_track_removal(self):
        t = Stream()
        result = self.run_streams(
            t.pat(((1, 0x1000), (2, 0x1001)))+t.pmt(video=0x24, audio=0x0f),
            '--gap', t.pmt(video=0x1b, audio=None)+t.packet(0x1fff, b''),
            '--select=2', t.pmt(False, 2, 0x1001, video=0x02, audio=0x03),
            t.pmt(False, 2, 0x1001, video=None, audio=0x11), '--reset')
        for i in (0, 1):
            self.assertEqual(result[i]['video_codec'], 'H.265/HEVC')
            self.assertEqual(result[i]['audio_codec'], 'AAC')
        self.assertEqual(result[2]['video_codec'], 'H.264/AVC')
        self.assertEqual(result[2]['audio_codec'], '')
        for i in (3, 6):
            self.assertEqual(result[i]['video_codec'], '')
            self.assertEqual(result[i]['audio_codec'], '')
        self.assertEqual(result[4]['video_codec'], 'MPEG-2')
        self.assertEqual(result[4]['audio_codec'], 'MPEG-1 audio')
        self.assertEqual(result[5]['video_codec'], '')
        self.assertEqual(result[5]['audio_codec'], 'AAC (LATM)')

    def test_pmt_view_tracks_data_and_service_switch(self):
        t = Stream()
        def entry(kind, pid, descriptors=b''):
            return struct.pack('>BHH', kind, 0xe000 | pid, 0xf000 | len(descriptors)) + descriptors
        body = struct.pack('>HH', 0xe100, 0xf000)
        body += entry(0x0f, 0x101, b'\x0a\x04nld\x00') + entry(0x0f, 0x102)
        body += entry(0x0b, 0x150, b'\x66\x02\x00\x07') + entry(0xfe, 0x155)
        initial, malformed, switched = self.run_streams(
            t.pat(((1, 0x1000), (2, 0x1001))) + t.table(0x1000, section(2, 1, body, version=3)),
            t.table(0x1000, section(2, 1, b'\xe1\x00\xf0\x02\x66\xff')),
            '--select=2')
        for field in ('PMT PID 0x1000', 'Version 3', 'PCR PID 0x0100', 'PID 0x0101',
                      'nld', 'PID 0x0102', 'DSM-CC', '0x0007 (object carousel)', 'Type 0xFE'):
            self.assertIn(field, initial['pmt_text'])
        self.assertEqual(initial['pmt_text'].count('AAC'), 2)
        self.assertEqual(initial['pmt_text'], malformed['pmt_text'])
        self.assertNotIn('DSM-CC', switched['pmt_text'])

    def test_malformed_pmt_does_not_replace_codecs(self):
        t = Stream()
        # Valid CRC, but the last ES claims a descriptor that is not present.
        body = b'\xe1\0\xf0\0\x1b\xe1\0\xf0\0\x03\xe1\x01\xf0\x05'
        result = self.run_streams(t.pat()+t.pmt(video=0x24, audio=0x0f),
                                 t.table(0x1000, section(2, 1, body)))
        self.assertEqual(result[1]['video_codec'], 'H.265/HEVC')
        self.assertEqual(result[1]['audio_codec'], 'AAC')

    def test_page_selection_decimal_bcd_boundaries_and_waiting(self):
        t=Stream()
        initial=t.pat()+t.pmt()+t.eit()+t.teletext()+t.teletext()
        for number in (101, 199, 200, 888, 899, 100):
            with self.subTest(page=number):
                bcd=int(str(number),16)
                before, waiting, wrong, received, invalid, gap, reset=self.run_streams(
                    initial, '--page='+str(number),
                    t.teletext(0x234)+t.teletext(0x234),
                    t.teletext(bcd, message='CHOSEN PAGE')+t.teletext(bcd, message='CHOSEN PAGE'),
                    '--page=900', '--gap', '--reset')
                self.assertEqual(waiting['page_number'], number)
                if number!=100:
                    self.assertNotIn('page_hash', waiting)
                    self.assertNotIn('page_hash', wrong)
                    self.assertFalse(waiting['teletext_received'])
                self.assertEqual(waiting['events'], before['events'])
                self.assertIn('page_hash', received)
                self.assertEqual(received['page_number'], number)
                self.assertEqual(received['page_hash'], invalid['page_hash'])
                self.assertEqual(received['page_hash'], gap['page_hash'])
                self.assertEqual(reset['page_number'],100)
        result=self.run_streams(initial,'--page=99','--page=-1')
        self.assertEqual([s['page_number'] for s in result],[100,100,100])

    def test_classic_teletext_page_filter_updates_gap_and_removal(self):
        t=Stream()
        result=self.run_streams(t.pat()+t.pmt()+t.teletext(0x101),
            t.teletext()+t.teletext(), t.teletext(message='UPDATED PAGE')+t.teletext(message='UPDATED PAGE'),
            '--gap', t.pmt(False)+t.packet(0x1fff, b''))
        self.assertTrue(result[0]['decoder'], 'Test requires libzvbi')
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
