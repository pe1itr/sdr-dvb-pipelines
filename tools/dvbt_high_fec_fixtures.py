#!/usr/bin/env python3
"""Generate file-only DVB-T IQ with the independent datv-transmitter test encoder.

No radio is opened. Requires NumPy and a prebuilt datv-iq fixture binary.
The fixture embeds deterministic counter TS payloads and PAT/PMT. All output
is signed 16-bit IQ; the supplied manifest uses actual encoder sample rates.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import numpy as np


def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def generate(encoder,out,bw,fec,duration):
    a,b=map(int,fec.split('/'));rate=math.ceil(bw*(32/7)/250000)*250000
    name=f'{bw//1000}k-fec{a}{b}-{duration}s'
    samples=duration*rate
    # QPSK, 1512 payload carriers, 2K, GI 1/32; allow startup/pipeline tail.
    bitrate=(bw*8/7)/(2048*33/32)*1512*2*a/b*188/204
    packets=math.ceil(duration*bitrate/(188*8))+600
    command=[str(encoder),'--dvbt',{'1/2':'3','3/4':'6','5/6':'8','7/8':'10'}[fec],'32',str(packets),str(bw)]
    path=out/(name+'.iq');written=clipped=0;peak=0.0
    with path.open('wb') as dst:
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            while data:=process.stdout.read(65536*8):
                x=np.frombuffer(data,dtype='<f4').reshape(-1,2)
                x=x[:max(0,samples-written)]
                if not len(x):continue
                if not np.isfinite(x).all():raise ValueError('Nonfinite IQ')
                peak=max(peak,float(np.max(np.abs(x))))
                y=np.rint(x*32768)
                clipped+=int(np.count_nonzero((y>32767)|(y<-32768)))
                dst.write(np.clip(y,-32768,32767).astype('<i2').tobytes())
                written+=len(x)
            stderr=process.stderr.read().decode(errors='replace')
            if process.wait():raise RuntimeError(stderr)
        finally:
            if process.poll() is None:process.kill();process.wait()
    assert written==samples,(name,written,samples)
    row=dict(name=name,path=str(path),format='s16',rate=rate,bandwidth=bw,fec=fec,gi='1/32',
             nominal_duration_s=duration,input_samples=samples,sha256=digest(path),
             generator_command=command,generator_stderr=stderr,clipped_components=clipped,peak_float=peak,
             expected_clean=True,ts_pattern='datv-iq counter/PAT/PMT')
    print(name,rate,'samples/s;',written,'samples; clipped',clipped,flush=True)
    return row


def impair(out,case):
    rng=np.random.default_rng(20261106)
    source=Path(case['path']);target=out/'500k-fec78-fading-20s.iq'
    total=case['rate']*20;power=0.0;count=0
    with source.open('rb') as f:
        while data:=f.read(65536*4):
            x=np.frombuffer(data,dtype='<i2').astype(np.float64)/32768
            power+=float(np.dot(x,x));count+=len(x)//2
    sigma=np.sqrt(power/count/10**(14/10)/2)
    done=clipped=0
    with source.open('rb') as src,target.open('wb') as dst:
        while done<total:
            data=src.read(min(65536,total-done)*4)
            if not data:raise ValueError('Too short')
            x=np.frombuffer(data,dtype='<i2').reshape(-1,2).astype(np.float64)/32768
            t=(np.arange(len(x))+done)/case['rate'];gain=np.ones(len(x))
            for start in (5,12):gain[(t>=start)&(t<start+.35)]=.1
            z=(x[:,0]+1j*x[:,1])*gain*np.exp(1j*2*np.pi*(.5*t*t))
            noise=rng.standard_normal((len(x),2))*sigma
            y=np.rint((np.column_stack((z.real,z.imag))+noise)*32768)
            clipped+=int(np.count_nonzero((y>32767)|(y<-32768)))
            dst.write(np.clip(y,-32768,32767).astype('<i2').tobytes());done+=len(x)
    row={k:v for k,v in case.items() if k not in ('generator_command','generator_stderr','peak_float')}
    row.update(name='500k-fec78-fading-20s',path=str(target),sha256=digest(target),expected_clean=False,
               clipped_components=clipped,impairment=dict(seed=20261106,noise_db=14,
                fade_gain=.1,fade_intervals_s=[[5,5.35],[12,12.35]],drift_hz_per_s=1))
    print(row['name'],'clipped',clipped,flush=True)
    return row


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('encoder',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();encoder=a.encoder.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    cases=[generate(encoder,out,bw,fec,20) for bw in (250000,500000) for fec in ('3/4','5/6','7/8')]
    cases.append(generate(encoder,out,500000,'1/2',20))
    cases.append(generate(encoder,out,500000,'7/8',60))
    cases.append(impair(out,cases[5]))
    (out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n')
    (out/'generation.json').write_text(json.dumps(dict(encoder=str(encoder),encoder_sha256=digest(encoder),numpy=np.__version__,cases=cases),indent=2)+'\n')


if __name__=='__main__':main()
