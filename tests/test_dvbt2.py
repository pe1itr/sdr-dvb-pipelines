"""Broadcast worker: fixed-clock input, exact TS contents, PLP and overload.

Synthetic signals are regression fixtures, not evidence of Digitenne reception.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', default='build')
args = parser.parse_args()
BUILD = ROOT / args.build_dir
WORKER = [str(BUILD / 'linrad_dvb'), '--decoder-worker=dvbt2']


def check_packets(data):
    assert len(data) % 188 == 0
    ids = []
    for start in range(0, len(data), 188):
        pkt = data[start:start+188]
        assert pkt[:3] == b'\x47\x01\x00', pkt[:8]
        count = int.from_bytes(pkt[4:8], 'little')
        assert pkt[3] == 0x10 | (count & 15)
        assert pkt[8:] == bytes((count * 31 + j * 17) & 255 for j in range(8, 188))
        ids.append(count)
    assert len(ids) > 1000, len(ids)
    assert all(b > a for a, b in zip(ids, ids[1:])), 'out of order TS'
    # Once acquisition has settled, all generated payload packets must survive.
    assert ids[-1] - ids[1000] + 1 == len(ids) - 1000, 'TS gaps after acquisition'
    return len(ids)


def live(path, fmt, plp):
    with tempfile.TemporaryFile() as source_log, tempfile.TemporaryFile() as output:
        source = subprocess.Popen([sys.executable, str(ROOT/'dev/fixed_iq_source.py'),
            '--in', str(path), '--sample-rate', '10000000', '--input-format', fmt],
            stdout=subprocess.PIPE, stderr=source_log)
        try:
            result = subprocess.run(WORKER + ['--sample-rate', '10000000', '--input-format', fmt,
                '--plp', str(plp)], stdin=source.stdout, stdout=output, stderr=subprocess.PIPE, timeout=30)
            source.stdout.close()
            assert source.wait(timeout=5) == 0
        finally:
            if source.poll() is None:
                source.kill(); source.wait()
        source_log.seek(0); log = source_log.read().decode()
        report = json.loads(re.search(r'\[fixed-iq\] (.*)', log)[1])
        assert report['dropped_samples'] == 0, report
        assert report['elapsed_s'] < report['nominal_duration_s'] + .2, report
        assert result.returncode == 0, result.stderr.decode()
        final = result.stderr.decode().splitlines()[-1]
        assert 'input_dropped=0 fec_frames_dropped=0' in final, final
        output.seek(0)
        return output.read(), final


for options in ([], ['--sample-rate','40000'], ['--sample-rate','nan'],
                ['--sample-rate','10000000','--bandwidth','40000'],
                ['--sample-rate','10000000','--plp','256'],
                ['--sample-rate','10000000','--input-format','invalid']):
    p = subprocess.run(WORKER+options, input=b'', capture_output=True)
    assert p.returncode == 2 and not p.stdout
for fmt, data, rc in [('s16', b'\0'*7, 1), ('u8', b'\x80'*9, 1), ('cf32', b'\0'*16, 0)]:
    p = subprocess.run(WORKER+['--sample-rate','10000000','--input-format',fmt], input=data,capture_output=True)
    assert p.returncode == rc and not p.stdout, p.stderr

for source in ('b210','hackrf'):
    p = subprocess.run([BUILD/'linrad_dvb','-c', ROOT/'linrad-dvbt2-eindhoven.example.ini',
        '--source',source,'--plp','7','--print-pipeline'],capture_output=True,text=True)
    assert p.returncode == 0, p.stderr
    assert "'570000000'" in p.stdout and "'--decoder-worker=dvbt2'" in p.stdout
    assert "'--plp' '7'" in p.stdout and "'--bandwidth' '8000000'" in p.stdout

with tempfile.TemporaryDirectory(prefix='linrad-t2-') as temp:
    path = Path(temp)/'fixture.iq'
    for profile, fmt, plp in [('8k','s16',-1), ('32k','u8',7)]:
        subprocess.run([BUILD/'dvbt2_fixture',path,profile,fmt],check=True,timeout=60)
        data, final = live(path,fmt,plp)
        count = check_packets(data)
        print(f'{profile} {fmt}: {count} exact TS packets; {final}', flush=True)
    # A requested PLP absent from L1 must not silently decode another PLP.
    data, final = live(path,'u8',8)
    assert not data, 'wrong PLP produced TS'
    print('Absent PLP: no transport stream', flush=True)
    # Burst input intentionally exceeds live throughput: bounded loss is explicit.
    with path.open('rb') as src:
        p = subprocess.run(WORKER+['--sample-rate','10000000','--input-format','u8'],
            stdin=src,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=30)
    assert p.returncode == 0, p.stderr
    loss = int(re.findall(rb'input_dropped=(\d+)',p.stderr)[-1])
    assert loss > 0, 'burst did not exercise bounded queue overflow'
    print(f'Burst overload: {loss} dropped input samples reported', flush=True)
print('DVB-T2 worker tests passed')
