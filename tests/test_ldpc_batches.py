#!/usr/bin/env python3
"""Check all DVB-S2 LDPC tables/MODCOD aliases and SIMD batch transitions.

Compare every decoded LLR byte with the old batch policy, then independently
check known codewords and parity outcomes. No IQ, SDR or network access.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
# Independent DVB-S2 MODCOD-to-FEC mapping, normal then short frames.
TABLES=(
    [None]+[f'B{i}' for i in range(1,12)]+['B5','B6','B7','B9','B10','B11',
        'B6','B7','B8','B9','B10','B11','B7','B8','B8','B10','B11']+[None]*3,
    [None]+[f'C{i}' for i in range(1,11)]+[None,'C5','C6','C7','C9','C10',
        None,'C6','C7','C8','C9','C10',None,'C7','C8','C8','C10',None]+[None]*3)


def execute(command, data=b'', env=None):
    return subprocess.run(command,input=data,capture_output=True,env=env,timeout=180)


def checked(command, data=b'', env=None):
    p=execute(command,data,env)
    if p.returncode:raise AssertionError(f'{command}: {p.stderr.decode(errors="replace")}')
    return p.stdout


def fixture(frames, n):
    rng=random.Random(8821+n)
    result=bytearray();expected=[];batches=[]
    # Repeated grow/shrink transitions in one persistent process exercise
    # reuse, zeroing of inactive lanes, and the final one-frame EOF tail.
    sequence=[(1,'clean'),(1,'repair'),(1,'noise'),(32,'mixed'),(1,'clean'),
              (16,'mixed'),(2,'mixed'),(17,'mixed'),(15,'mixed'),(31,'mixed'),
              (1,'repair'),(1,'clean')]
    for count,pattern in sequence:
        result+=struct.pack('=I',count);batches.append(count)
        for lane in range(count):
            clean=frames[lane*n:(lane+1)*n]
            kind=('clean','noise','repair','extreme')[lane%4] if pattern=='mixed' else pattern
            damaged=bytearray(clean)
            if kind=='repair':
                # Known sparse errors in both data and parity, at low reliability.
                for bit in (0,n-1):damaged[bit]=8 if clean[bit]>=128 else 248
            elif kind=='noise':
                damaged=bytearray(rng.choice((0,1,2,3,253,254,255,128,127)) for _ in range(n))
            elif kind=='extreme':
                damaged=bytearray(128 if bit>=128 else 127 for bit in clean)
            result+=damaged
            expected.append(None if kind=='noise' else clean)
    return bytes(result),expected,batches


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,default=ROOT/'build/ldpc_tool')
    parser.add_argument('--reference',type=Path,help='Optional preserved old helper; otherwise use the reference-policy fixture')
    parser.add_argument('--portable',action='store_true')
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    env=os.environ.copy();env.pop('LINRAD_LDPC_PORTABLE',None)
    if args.portable:env['LINRAD_LDPC_PORTABLE']='1'
    version=checked([str(args.candidate),'--version'],env=env).decode().strip()
    avx='SIMD=32' in version
    gen=ROOT/'build/ldpc_fixture'
    ref=ROOT/('build/ldpc_fixture_avx2' if avx else 'build/ldpc_fixture')
    rows=[];seen={};start=time.monotonic()
    for short,tables in enumerate(TABLES):
        assert len(tables)==32
        n=16200 if short else 64800
        for mc,table in enumerate(tables):
            command=[str(args.candidate),'--framed','--modcod',str(mc)]
            if short:command+=['--shortframes']
            if table is None:
                p=execute(command,env=env)
                assert p.returncode!=0,(short,mc,'reserved MODCOD accepted')
                continue
            frames=checked([str(gen),'generate',table])
            assert len(frames)==32*n
            if table not in seen:
                data,expected,batches=fixture(frames,n)
                reference=([str(args.reference),*command[1:]] if args.reference
                           else [str(ref),'reference',table])
                before=checked(reference,data,env)
                after=checked(command,data,env)
                assert before==after,f'{table}: decoded LLR bytes differ'
                assert len(after)==len(expected)*n
                outcomes=checked([str(gen),'verify',table],after)
                assert len(outcomes)==len(expected)
                failed=good=0
                for i,clean in enumerate(expected):
                    decoded=after[i*n:(i+1)*n]
                    if clean is None:
                        assert outcomes[i]==0,f'{table}: noise fixture did not exercise failure'
                        failed+=1
                    else:
                        assert outcomes[i]==1,f'{table}: clean/repairable frame failed parity'
                        assert all((a<128)==(b<128) for a,b in zip(decoded,clean)),f'{table}: wrong hard bits/order'
                        good+=1
                seen[table]=dict(table=table,modcod=mc,short=bool(short),batches=batches,
                    frames=len(expected),valid=good,failed=failed,decoded_sha256=hashlib.sha256(after).hexdigest())
                print(f'{table}: {len(expected)} frames, exact LLRs; {good} valid / {failed} failed',flush=True)
            # Every modulation alias must select the right graph. A nonzero
            # encoded codeword would not pass unchanged against the wrong graph.
            clean=frames[:n]
            decoded=checked(command,struct.pack('=I',1)+clean,env)
            assert decoded==clean,(short,mc,table,'MODCOD mapping mismatch')
            rows.append(dict(modcod=mc,short=bool(short),table=table))
    # Framing must reject invalid counts and partial headers/bodies, and accept
    # EOF exactly between records. Repeated valid frames precede a bad tail.
    cmd=[str(args.candidate),'--framed','--modcod','4']
    assert checked(cmd,env=env)==b''
    valid=struct.pack('=I',1)+bytes([40])*64800
    for bad in [struct.pack('=I',n) for n in (0,33,0xffffffff)]+[b'\1\0',struct.pack('=I',1)+b'x']:
        assert execute(cmd,bad,env).returncode!=0
        assert execute(cmd,valid+bad,env).returncode!=0
    report=dict(candidate=str(args.candidate),version=version,portable=args.portable,
                reference=str(args.reference or ref),tables=list(seen.values()),modcod_mappings=rows,
                reserved_mappings=64-len(rows),invalid_records=10,elapsed_s=time.monotonic()-start)
    assert len(seen)==21 and len(rows)==52
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f'LDPC: all 21 tables, 52 supported MODCOD/frame-size mappings, batch sizes 1/2/15/16/17/31/32 and malformed input passed',flush=True)


if __name__=='__main__':main()
