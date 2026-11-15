#!/usr/bin/env python3
"""Unpaced captured-LLR experiment, separate from paced full-receiver runs.

Run only with live receivers stopped. GPU results require real hardware.
No production batching policy is changed by these hypothetical batch sizes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('--frames',type=int,default=128)
    p.add_argument('--cpu-helper',type=Path,required=True,help='Instrumented current production CPU helper from ldpc_experiment.py build')
    p.add_argument('--gpu-timing',action='store_true',help='Enable Vulkan device timestamps (adds measurement overhead)')
    a=p.parse_args()
    if a.frames<32 or a.frames%32: p.error('frames must be a positive multiple of 32')
    a.output.mkdir(parents=True,exist_ok=True)
    words=[]
    with a.trace.open('rb') as f:
        while len(words)<a.frames:
            header=f.read(4)
            if len(header)!=4: raise ValueError('Incomplete trace')
            n=struct.unpack('=I',header)[0]
            if not 1<=n<=32: raise ValueError('Invalid batch')
            for _ in range(n):
                word=f.read(64800)
                if len(word)!=64800: raise ValueError('Truncated frame')
                words.append(word)
    words=words[:a.frames]
    rows=[]
    for batch in (1,2,4,8,16,32):
        data=b''.join(struct.pack('=I',batch)+b''.join(words[i:i+batch])
                      for i in range(0,len(words),batch))
        reference=None
        reference_totals=None
        for backend in ('cpu-reference','gpu','production-cpu'):
            prefix=a.output/f'{backend}-batch{batch}'
            cmd=([str(a.cpu_helper.resolve())] if backend=='production-cpu'
                 else [str(ROOT/'build/dvbs2_gpu_helper')])
            cmd+=['--framed','--modcod','14']
            if backend=='cpu-reference': cmd+=['--backend','cpu-reference']
            env=dict(os.environ,LINRAD_S2_LDPC_PROFILE='1')
            env.pop('DECT2_NOGPU',None)
            if a.gpu_timing: env['DECT2_GPU_TIMING']='1'
            result=subprocess.run(['/usr/bin/time','-f','%U %S %e %M','-o',str(prefix.with_suffix('.cpu')),*cmd],
                                  input=data,capture_output=True,timeout=120,env=env)
            prefix.with_suffix('.log').write_bytes(result.stderr)
            if result.returncode: raise RuntimeError(result.stderr.decode(errors='replace'))
            if len(result.stdout)!=len(words)*64800: raise ValueError('Wrong output length')
            if backend=='cpu-reference': reference=result.stdout
            hard=bytes(x>=128 for x in result.stdout)
            ref=bytes(x>=128 for x in reference)
            differences=sum(x!=y for x,y in zip(hard,ref))
            if backend=='gpu' and differences: raise ValueError('GPU differs from matching FP32 CPU reference')
            user,system,wall,rss=map(float,prefix.with_suffix('.cpu').read_text().split())
            log=result.stderr.decode()
            latencies=list(map(float,re.findall(r'\[s2-ldpc\].*host_ms=([\d.]+)',log)))
            total=re.search(r'frames=(\d+) converged=(\d+) iterations=(\d+) host_decode_ms=([\d.]+)',log)
            row=dict(backend=backend,batch=batch,frames=len(words),process_cpu_s=user+system,
                     process_wall_s=wall,hard_differences_from_fp32_cpu=differences,
                     output_sha256=hashlib.sha256(result.stdout).hexdigest())
            if total:
                totals=tuple(map(int,total.group(1,2,3)))
                if backend=='cpu-reference': reference_totals=totals
                if backend=='gpu' and totals!=reference_totals:
                    raise ValueError('GPU convergence/iteration counts differ from FP32 CPU')
                row.update(converged=int(total[2]),iterations=int(total[3]),warm_host_decode_ms=float(total[4]),
                           median_batch_ms=statistics.median(latencies),max_batch_ms=max(latencies))
                if backend=='gpu' and a.gpu_timing:
                    before,after=log.split('S2 GPU helper: device=',1)
                    pattern=r'timing cumulative batches=(\d+) words=(\d+) device_ms=([\d.]+) host_ms=([\d.]+)'
                    initial=re.findall(pattern,before)
                    final=re.findall(pattern,after)
                    if not initial or not final: raise ValueError('Missing Vulkan device timestamps')
                    begin=list(map(float,initial[-1]));end=list(map(float,final[-1]))
                    row['device_timing_delta']=dict(batches=int(end[0]-begin[0]),frames=int(end[1]-begin[1]),
                                                    device_ms=end[2]-begin[2],host_ms=end[3]-begin[3])
            else:
                probes=[dict((k,float(v)) for k,v in re.findall(r'(\w+)=([\d.]+)',line))
                        for line in log.splitlines() if line.startswith('[ldpc-profile]')]
                if not probes: raise ValueError('Production CPU helper must include profiling')
                row['warm_pack_decode_unpack_cpu_ms']=1000*sum(r['pack']+r['decode']+r['unpack'] for r in probes)
                row['exhausted_batches']=sum(not r['satisfied'] for r in probes)
            rows.append(row)
            print(json.dumps(row),flush=True)
    report=dict(trace=str(a.trace),selected_input_sha256=hashlib.sha256(b''.join(words)).hexdigest(),
                selection='first complete recorded LLR frames; preserves actual nonzero codewords',
                scope='unpaced component test; GPU initialization excluded only from warm metrics; not live reception',rows=rows)
    (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__': main()
