#!/usr/bin/env python3
"""Isolated LDPC profiling with paced IQ replay; production code is untouched.

build OUT: compile instrumented AVX2 helpers from the pre-change revision
(--baseline-ref, default eb2e09d). "current" uses the working-tree source.
Variants: profile/baseline (unchanged), medium (16 lanes for 2..16 frames), single16
(16 lanes for 1..16 frames), packing (skip redundant zeroing). Append -plain
to build without profiling/capture for end-to-end timing.
run OUT MANIFEST --label NAME [--helper PATH] [--capture-every N]: replay complete
IQ files at their original rate and measure decoder + helper CPU, TS/BB hashes,
first TS and source elapsed time. Optional LLR capture samples complete batches.
replay OUT TRACE --variants profile,...: compare helpers on captured LLR batches
(unpaced kernel benchmark, explicitly separate from the live IQ measurements).
merge OUT: combine profile-CASE-PID-mcN-SIZE traces by case and frame type.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'third_party/ldpc/ldpc_tool.cc'


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Expected one source anchor: {old[:80]}')
    return text.replace(old, new)


PROFILE = r'''
#include <time.h>
static double bench_cpu() {
    struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
static FILE *bench_in, *bench_out;
static unsigned bench_every, bench_request;
static void bench_open(int modcod, bool sf) {
    const char *prefix=getenv("LINRAD_LDPC_CAPTURE");
    const char *every=getenv("LINRAD_LDPC_CAPTURE_EVERY");
    bench_every=every ? strtoul(every,nullptr,10) : 0;
    fprintf(stderr,"[ldpc-config] pid=%d modcod=%d short=%d capture_every=%u\n",
            (int)getpid(),modcod,sf,bench_every);
    if (!prefix || !bench_every) return;
    std::string name=std::string(prefix)+"-"+std::to_string(getpid())+"-mc"+
        std::to_string(modcod)+(sf ? "-short" : "-normal");
    bench_in=fopen((name+".llr").c_str(),"wb");
    bench_out=fopen((name+".decoded").c_str(),"wb");
    if (!bench_in || !bench_out) { perror("LLR capture"); exit(1); }
}
static bool bench_capture(uint32_t blocks, const int8_t *code, size_t bytes) {
    bool capture=bench_in && (bench_request%bench_every==0);
    ++bench_request;
    if (capture) {
        if (fwrite(&blocks,sizeof(blocks),1,bench_in)!=1 ||
            fwrite(code,1,bytes,bench_in)!=bytes || fflush(bench_in)) exit(1);
    }
    return capture;
}
'''


def instrument(text):
    text=replace(text,'template<typename V> struct DecoderRunner {',PROFILE+'\ntemplate<typename V> struct DecoderRunner {')
    text=replace(text,'        if (!simd) {','        if (!simd) {\n            double init_t=bench_cpu();')
    text=replace(text,'            if (!simd) fatal("allocate LDPC workspace");',
                 '            if (!simd) fatal("allocate LDPC workspace");\n            fprintf(stderr,"[ldpc-init] width=%d cpu=%.9f\\n",V::SIZE,bench_cpu()-init_t);')
    text=replace(text,'            memset(simd, 0, sizeof(V)*length);',
                 '            double pack_t=bench_cpu();\n            memset(simd, 0, sizeof(V)*length);')
    text=replace(text,'            decoder(simd, simd+data_length, TRIALS, count);',
                 '            double decode_t=bench_cpu();\n            int remaining=decoder(simd, simd+data_length, TRIALS, count);\n            double unpack_t=bench_cpu();')
    text=replace(text,'                for (int i=0; i<length; ++i) code[(j+n)*length+i] = simd[i].v[n];',
                 '''                for (int i=0; i<length; ++i) code[(j+n)*length+i] = simd[i].v[n];
            fprintf(stderr,"[ldpc-profile] frames=%d width=%d length=%d iterations=%d satisfied=%d pack=%.9f decode=%.9f unpack=%.9f\\n",
                    count,V::SIZE,length,remaining<0 ? TRIALS : TRIALS-remaining,remaining>=0,
                    decode_t-pack_t,unpack_t-decode_t,bench_cpu()-unpack_t);''')
    text=replace(text,'    DecoderRunner<simd_type> wide;',
                 '    bench_open(modcod,shortframes);\n    DecoderRunner<simd_type> wide;')
    text=replace(text,'        if (!read_full(code.data(),bytes,!framed)) break;',
                 '        if (!read_full(code.data(),bytes,!framed)) break;\n        bool capture=bench_capture(blocks,code.data(),bytes);')
    text=replace(text,'        size_t pos=0;',
                 '''        if (capture && (fwrite(code.data(),1,bytes,bench_out)!=bytes || fflush(bench_out))) exit(1);
        size_t pos=0;''')
    return text


def candidate(source, name):
    if name in ('medium', 'single16'):
        source=replace(source, '#include "testbench.hh"',
                       '#include "testbench.hh"\n#include "sse4_1.hh"')
        source=replace(source, '    DecoderRunner<simd_type> wide;',
                       '    DecoderRunner<SIMD<int8_t,16>> medium;\n    DecoderRunner<simd_type> wide;')
        old='''        if (blocks==1) narrow.run(code.data(),blocks,ldpc);
        else wide.run(code.data(),blocks,ldpc);'''
        if name=='medium':
            new='''        if (blocks==1) narrow.run(code.data(),blocks,ldpc);
        else if (blocks<=16) medium.run(code.data(),blocks,ldpc);
        else wide.run(code.data(),blocks,ldpc);'''
        else:
            new='''        if (blocks<=16) medium.run(code.data(),blocks,ldpc);
        else wide.run(code.data(),blocks,ldpc);'''
        return replace(source,old,new)
    if name=='packing':
        return replace(source,'            memset(simd, 0, sizeof(V)*length);',
                       '            if (count<V::SIZE) memset(simd, 0, sizeof(V)*length);')
    if name in ('profile','baseline','current'):return source
    raise ValueError('Unknown variant: '+name)


def build(out, variants, baseline_ref):
    original=(SOURCE.read_text() if baseline_ref=='current' else
              subprocess.check_output(['git','show',baseline_ref+':'+str(SOURCE.relative_to(ROOT))],cwd=ROOT,text=True))
    commands=[]
    for name in variants:
        plain=name.endswith('-plain')
        kind=name.removesuffix('-plain')
        # Instrument first; the probes surround unchanged packing/decoder calls.
        base=SOURCE.read_text() if kind=='current' else original
        source=candidate(base if plain else instrument(base),kind)
        path=out/(name+'.cc');path.write_text(source)
        cmd=['g++','-O3','-Wall','-Wextra','-std=gnu++17','-mavx2',
             '-Wno-unused-variable','-Wno-unused-parameter','-I'+str(SOURCE.parent),
             str(path),str(ROOT/'build/ldpc_tables.o'),'-o',str(out/name)]
        subprocess.run(cmd,check=True);commands.append(cmd)
    meta=dict(commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              source_sha256=digest(SOURCE),baseline_ref=baseline_ref,
              baseline_source_sha256=hashlib.sha256(original.encode()).hexdigest(),commands=commands)
    (out/('build-'+','.join(variants)+'.json')).write_text(json.dumps(meta,indent=2)+'\n')


def profile_summary(path):
    log=path.read_text()
    if '[ldpc-config]' not in log:
        return None  # An uninstrumented run has no LDPC timing/count evidence.
    rows=[]
    for line in log.splitlines():
        if line.startswith('[ldpc-profile]'):
            row={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)}
            rows.append(row)
    hist={}
    iterations={}
    for r in rows:
        key=f"{int(r['frames'])}/{int(r['width'])}"
        hist[key]=hist.get(key,0)+1
        key=str(int(r['iterations']));iterations[key]=iterations.get(key,0)+1
    return dict(batches=len(rows),frames=int(sum(r['frames'] for r in rows)),
                helper_starts=log.count('[ldpc-config]'),
                batch_frames_simd_width=hist,iterations=iterations,
                exhausted_batches=sum(not r['satisfied'] for r in rows),
                pack_cpu_s=sum(r['pack'] for r in rows),
                decode_cpu_s=sum(r['decode'] for r in rows),
                unpack_cpu_s=sum(r['unpack'] for r in rows),
                init_cpu_s=sum(map(float,re.findall(r'\[ldpc-init\].*cpu=([\d.]+)',log))))


def timed(command, destination):
    return ['/usr/bin/time','-f','%U %S %e %M','-o',str(destination),*command]


def cpu_read(path):
    user,system,wall,rss=map(float,path.read_text().strip().splitlines()[-1].split())
    return dict(cpu_s=user+system,user_s=user,system_s=system,elapsed_s=wall,rss_kib=int(rss))


def run_case(out, case, label, helper, capture_every, unpaced=False):
    prefix=out/(label+'-'+case['name'])
    file=lambda suffix: Path(str(prefix)+suffix)
    env={k:v for k,v in os.environ.items() if not k.startswith('LINRAD_')}
    if capture_every:
        env['LINRAD_LDPC_CAPTURE']=str(prefix)
        env['LINRAD_LDPC_CAPTURE_EVERY']=str(capture_every)
    feeder=[str(ROOT/'build/iq_file_to_stdout'),'--in',case['path'],
            '--sample-rate',str(case['rate']),'--input-format','s16',
            '--header-bytes',str(case.get('header_bytes',0))]
    if unpaced:
        # Stream exactly the same raw IQ, with no source clock or rate limiter.
        feeder=['/usr/bin/tail','--bytes=+'+str(case.get('header_bytes',0)+1),case['path']]
    command=[str(ROOT/'build/linrad_dvb'),'--decoder-worker=leansdr','--s16',
             '-f',str(case['rate']),'--sr',str(case['sr']),'--standard','DVB-S2',
             '--sampler','rrc','--rrc-steps','8','--rrc-rej','10',
             '--framesizes','3','--modcods',str(case['modcods']),
             '--ldpc-helper',helper,'--ldpc-batch',str(case['batch']),
             '--nhelpers',str(case['workers']),'-v']
    if case.get('fastlock'):command+=['--fastlock']
    with file('.log').open('wb') as log, file('.ts').open('wb') as ts, \
            file('.bb').open('wb') as bb, file('.status').open('wb') as status, \
            file('.source.log').open('wb') as source_log:
        command+=['--fd-bbf',str(bb.fileno()),'--fd-info',str(status.fileno())]
        start=time.monotonic();first=None;rx=None
        arrivals=[];ts_total=0
        source=subprocess.Popen(timed(feeder,file('.source.cpu')),stdout=subprocess.PIPE,stderr=source_log,env=env,start_new_session=True)
        try:
            rx=subprocess.Popen(timed(command,file('.cpu')),stdin=source.stdout,
                stdout=subprocess.PIPE,stderr=log,env=env,pass_fds=(bb.fileno(),status.fileno()),start_new_session=True)
            source.stdout.close()
            with selectors.DefaultSelector() as sel:
                sel.register(rx.stdout,selectors.EVENT_READ)
                while True:
                    if not sel.select(30):
                        print(label,case['name'],'waiting for TS',round(time.monotonic()-start,1),flush=True)
                        if time.monotonic()-start>case['duration_s']*3+60:raise TimeoutError(case['name'])
                        continue
                    data=os.read(rx.stdout.fileno(),65536)
                    if not data:break
                    arrival=time.monotonic()-start
                    if first is None:first=arrival
                    ts_total+=len(data);arrivals.append([ts_total,arrival])
                    ts.write(data)
            rc=rx.wait(timeout=30);sr=source.wait(timeout=30)
            elapsed=time.monotonic()-start
            if rc or sr:raise RuntimeError(f'{prefix}: decoder={rc}, feeder={sr}')
        finally:
            import signal
            for proc in (rx,source):
                if proc is not None and proc.poll() is None:
                    os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    text=file('.source.log').read_text()
    sent=re.search(r'end of file: (\d+) samples, ([\d.]+) s elapsed',text)
    source_cpu=cpu_read(file('.source.cpu'))
    if unpaced:
        samples=(Path(case['path']).stat().st_size-case.get('header_bytes',0))//4
        source_elapsed=source_cpu['elapsed_s']
    else:
        if not sent:raise RuntimeError('Incomplete source: '+text)
        samples=int(sent[1]);source_elapsed=float(sent[2])
    if samples!=case['samples']:raise RuntimeError('Unexpected source sample count')
    file('.arrivals.json').write_text(json.dumps(arrivals)+'\n')
    result=dict(case=case['name'],label=label,command=command,feeder=feeder,
        helper=helper,capture_every=capture_every,elapsed_s=elapsed,first_ts_s=first,
        last_ts_s=arrivals[-1][1] if arrivals else None,
        measurement='unpaced receiver capacity' if unpaced else 'original-rate paced replay',
        input_samples=samples,source_elapsed_s=source_elapsed,
        expected_duration_s=case['duration_s'],decoder_and_helpers=cpu_read(file('.cpu')),
        source=source_cpu,ts_bytes=file('.ts').stat().st_size,
        ts_sha256=digest(file('.ts')),bb_bytes=file('.bb').stat().st_size,bb_sha256=digest(file('.bb')),
        ldpc=profile_summary(file('.log')))
    file('.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('command','feeder')}),flush=True)
    return result


def replay(out, trace, variants):
    mc=int(re.search(r'-mc(\d+)-',trace.name)[1]);short='-short' in trace.name
    expected=trace.with_suffix('.decoded')
    for variant in variants:
        prefix=out/(trace.stem+'-'+variant)
        command=[str(out/variant),'--framed','--modcod',str(mc)]
        if short:command+=['--shortframes']
        with trace.open('rb') as data, prefix.with_suffix('.decoded').open('wb') as output, prefix.with_suffix('.log').open('wb') as log:
            subprocess.run(timed(command,prefix.with_suffix('.cpu')),stdin=data,stdout=output,stderr=log,check=True)
        result=dict(trace=str(trace),variant=variant,measurement='unpaced captured LLR batches',
                    cpu=cpu_read(prefix.with_suffix('.cpu')),ldpc=profile_summary(prefix.with_suffix('.log')),
                    decoded_sha256=digest(prefix.with_suffix('.decoded')),expected_sha256=digest(expected))
        result['identical']=result['decoded_sha256']==result['expected_sha256']
        prefix.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result),flush=True)
        if not result['identical']:
            raise RuntimeError('Decoded LLRs differ: '+str(prefix))


def merge(out):
    groups={}
    for path in out.glob('profile-*.llr'):
        match=re.fullmatch(r'profile-(.+)-(\d+)-mc(\d+)-(short|normal)\.llr',path.name)
        if match:
            groups.setdefault((match[1],match[3],match[4]),[]).append((int(match[2]),path))
    for (case,mc,size),entries in groups.items():
        target=out/f'captured-{case}-mc{mc}-{size}.llr'
        expected=target.with_suffix('.decoded')
        length=16200 if size=='short' else 64800
        frames=batches=0
        with target.open('wb') as dst,expected.open('wb') as decoded:
            for _,path in sorted(entries):
                data=path.read_bytes();pos=total=0
                while pos<len(data):
                    n=struct.unpack_from('=I',data,pos)[0]
                    if not 1<=n<=32:raise ValueError('Invalid batch in '+str(path))
                    pos+=4+n*length;total+=n*length;batches+=1;frames+=n
                response=path.with_suffix('.decoded').read_bytes()
                if pos!=len(data) or len(response)!=total:
                    raise ValueError('Incomplete captured batch in '+str(path))
                dst.write(data);decoded.write(response)
        meta=dict(case=case,modcod=int(mc),short=size=='short',batches=batches,frames=frames,
                  helper_processes=len(entries),input_sha256=digest(target),decoded_sha256=digest(expected))
        target.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n')
        print(json.dumps(meta),flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action',choices=('build','run','replay','merge'))
    p.add_argument('output',type=Path);p.add_argument('input',nargs='?',type=Path)
    p.add_argument('--variants',default='profile');p.add_argument('--label',default='profile')
    p.add_argument('--helper',default='builtin');p.add_argument('--capture-every',type=int,default=0)
    p.add_argument('--baseline-ref',default='eb2e09d')
    p.add_argument('--unpaced',action='store_true',help='Capacity test with raw IQ streamed without rate limiting; not live reception')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if a.capture_every<0:p.error('capture-every must be nonnegative')
    if a.action=='build':build(out,a.variants.split(','),a.baseline_ref);return
    if a.action=='merge':merge(out);return
    if a.input is None:p.error('input is required')
    if a.action=='replay':replay(out,a.input,a.variants.split(','));return
    for case in json.loads(a.input.read_text()):run_case(out,case,a.label,a.helper,a.capture_every,a.unpaced)


if __name__=='__main__':main()
