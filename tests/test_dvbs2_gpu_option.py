#!/usr/bin/env python3
"""S2 GPU opt-in, mode isolation and exact production CPU fallback.

--gpu also tests real hardware nonconvergence retry. Stop live receivers first.
"""
import configparser
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
ENV = dict(os.environ, DECT2_NOGPU='1')

def run(binary, options, data=b'', env=ENV):
    return subprocess.run([str(BUILD/binary), *options], input=data,
                          capture_output=True, timeout=60, env=env)

with tempfile.TemporaryDirectory(dir=BUILD) as directory:
    path=Path(directory)/'settings.ini'
    cfg=configparser.ConfigParser(interpolation=None)
    cfg.read(ROOT/'linrad-dvb.example.ini')
    cfg['leandvb']['helpers']='3'
    for choice in (None, 'true', 'false'):
        if choice is None: cfg['leandvb'].pop('gpu_ldpc',None)
        else: cfg['leandvb']['gpu_ldpc']=choice
        with path.open('w') as f: cfg.write(f)
        original=path.read_bytes()
        for mode in ('DVB-S2','DVB-S','DVB-S2'):
            p=run('linrad_dvb',['-c',str(path),'--standard',mode,'--print-pipeline'])
            assert p.returncode==0,p.stderr
            gpu=choice=='true' and mode=='DVB-S2'
            assert (b"'builtin-gpu'" in p.stdout)==gpu,p.stdout
            if mode=='DVB-S2':
                assert (b"'--nhelpers' '1'" if gpu else b"'--nhelpers' '3'") in p.stdout
            assert path.read_bytes()==original,'Active INI changed'

rng=random.Random(14064800)
for mc,short in ((14,False),(4,False),(4,True)):
    length=16200 if short else 64800
    options=['--framed','--modcod',str(mc)]+(['--shortframes'] if short else [])
    # Mixed data, full batch, short EOF tail and consecutive requests.
    noisy=bytes(rng.randrange(256) for _ in range(length))
    data=struct.pack('=I',32)+bytes([32])*length*31+noisy
    data+=struct.pack('=I',1)+noisy
    expected=run('ldpc_tool',options,data)
    actual=run('ldpc_tool_gpu',options,data)
    assert expected.returncode==actual.returncode==0,(expected.stderr,actual.stderr)
    assert actual.stdout==expected.stdout,(mc,short)
    assert b'cpu_frames=33' in actual.stderr,actual.stderr
    if mc!=14: assert b'GpuLdpc:' not in actual.stderr
    if '--device-failure' in sys.argv and mc==14:
        failure=run('test_dvbs2_gpu_device_failure',options,data)
        assert failure.returncode==0,failure.stderr
        assert failure.stdout==expected.stdout,'Device failure did not retry original LLRs'
        assert failure.stderr.count(b'[s2-device-failure-test]')==1,failure.stderr
        assert b'gpu_frames=0 cpu_frames=33' in failure.stderr,failure.stderr
        print('Device failure retains original input and disables further GPU calls')
    if '--gpu' in sys.argv and mc==14:
        env=dict(os.environ);env.pop('DECT2_NOGPU',None)
        hardware=run('ldpc_tool_gpu',options,data,env)
        assert hardware.returncode==0,hardware.stderr
        # Clean codewords on GPU; nonconverged noisy input retried on CPU.
        assert hardware.stdout==expected.stdout,hardware.stderr
        assert b'gpu_frames=31 cpu_frames=2' in hardware.stderr,hardware.stderr
        assert b'GPU unavailable' not in hardware.stderr
        print('Real GPU clean-word decoding and nonconverged CPU retry passed')
print('S2 GPU defaults, mode isolation, INI preservation, unsupported modes, 32-frame batches and EOF CPU fallback passed')
