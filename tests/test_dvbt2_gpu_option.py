#!/usr/bin/env python3
"""Check opt-in, old-INI defaults, persistence and unavailable-GPU fallback.

Optional --windows runs the newly built Windows executable with Wine.
Default tests use no receiver hardware or GPU computation (DECT2_NOGPU=1).
--probe checks the GPU self-test; --hardware-tests runs the full GPU comparison
with local live sources stopped.
"""
import configparser
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
windows = '--windows' in sys.argv
command = (['wine', str(ROOT/'build/windows/linrad_dvb.exe')] if windows
           else [str(ROOT/'build/linrad_dvb')])
env = dict(os.environ, DECT2_NOGPU='1', WINEDEBUG='-all',
           WINEPREFIX=str(ROOT/'build/wine-windows'),
           WINEPATH='Z:'+str(ROOT/'build/linrad_dvb-windows-x64').replace('/', '\\'))

def run(*args):
    # Native Windows stdin must be a disk handle at the Wine boundary.
    with tempfile.TemporaryFile() as source:
        return subprocess.run(command+list(args), stdin=source, capture_output=True,
                              timeout=45, env=env, text=True)

if '--hardware-tests' in sys.argv:
    # Run this component test with live local receivers stopped.
    active=subprocess.run(['ps','-C','sdr_to_stdout,iq_file_to_stdout,linrad_udp_to_stdout,dvbt2_worker,dab_worker',
                           '-o','pid=,comm='],capture_output=True,text=True)
    assert not active.stdout.strip(),'Stop live local sources before GPU testing: '+active.stdout
    env.pop('DECT2_NOGPU')
    env['DXVK_LOG_LEVEL']='error'
    test_command=(['wine',str(ROOT/'build/windows/test_dvbt2_gpu.exe')] if windows
                  else [str(ROOT/'build/test_dvbt2_gpu')])
    result=subprocess.run(test_command,env=env,timeout=180)
    sys.exit(result.returncode)

if '--probe' in sys.argv:
    env.pop('DECT2_NOGPU')
    p=run('--decoder-worker=dvbt2','--gpu-check')
    print(p.stderr,end='',flush=True)
    assert p.returncode in (0,1),p.stderr
    print('GPU probe: '+('available' if p.returncode==0 else 'unavailable; hardware correctness test pending'))
    sys.exit(0)

with tempfile.TemporaryDirectory(dir=ROOT/'build') as directory:
    path=Path(directory)/'settings.ini'
    cfg=configparser.ConfigParser(interpolation=None)
    cfg.read(ROOT/'linrad-dvbt2-eindhoven.example.ini')
    name='Z:'+str(path).replace('/', '\\') if windows else str(path)
    for value,expected in [(None,'cpu'),('true','gpu'),('false','cpu')]:
        if value is None: cfg['dvbt2'].pop('gpu_ldpc',None)
        else: cfg['dvbt2']['gpu_ldpc']=value
        with path.open('w') as f: cfg.write(f)
        original=path.read_bytes()
        for standard in ('DVB-T2','DVB-S2','DVB-T2'):
            p=run('-c',name,'--standard',standard,'--print-pipeline')
            assert p.returncode==0,p.stderr
            if standard=='DVB-T2': assert f"'--ldpc' '{expected}'" in p.stdout,p.stdout
            else: assert "'--ldpc'" not in p.stdout,p.stdout
            assert path.read_bytes()==original,'Opening settings overwrote active INI'

for choice in ('cpu','gpu'):
    p=run('--decoder-worker=dvbt2','--sample-rate','10000000','--ldpc',choice)
    assert p.returncode==0,p.stderr
    assert f'requested={choice} active=cpu' in p.stderr,p.stderr
    assert not p.stdout
    if choice=='cpu': assert 'device=not initialized' in p.stderr,p.stderr
p=run('--decoder-worker=dvbt2','--sample-rate','10000000','--ldpc','invalid')
assert p.returncode==2 and not p.stdout,p.stderr
p=run('--decoder-worker=dvbt2','--gpu-check')
assert p.returncode==1 and 'CPU fallback' in p.stderr and not p.stdout,p.stderr
print(('Windows Wine' if windows else 'Linux')+': LDPC option, defaults, mode isolation, INI preservation and unavailable fallback passed')

if windows and '--components' in sys.argv:
    for name,args in [('test_dvbt2_gpu_fallback',[]),('test_dvbt2_pipeline',[]),
                      ('test_dvbt2_gap_clock',[]),('windows_gui_smoke',['--test-sdr-status'])]:
        result=subprocess.run(['wine',str(ROOT/'build/windows'/f'{name}.exe'),*args],
                              capture_output=True,text=True,env=env,timeout=60)
        assert result.returncode==0,(name,result.stdout,result.stderr)
        print(name+': '+result.stdout.strip(),flush=True)
    # Deliberately disabled GPU must report a skipped hardware test, not pass.
    result=subprocess.run(['wine',str(ROOT/'build/windows/test_dvbt2_gpu.exe')],
                          capture_output=True,text=True,env=env,timeout=45)
    assert result.returncode==77,(result.returncode,result.stdout,result.stderr)
    print(result.stdout.strip(),flush=True)
