#!/usr/bin/env python3
"""Isolated DVB-T Viterbi research; never replaces production sources/binaries.

build OUT: baseline, AVX2 prototype, and a per-step scalar/AVX2 checker.
The baseline source is pinned to pre-experiment commit 3b9077b; Git history is
required. Use --baseline-ref to select another compatible source revision.
test OUT: independent encoder regression and noisy/erased soft-value tests.
kernel OUT: repeated unpaced Viterbi benchmark (not a live CPU claim).
run OUT MANIFEST: original-rate IQ replays, alternating baseline/candidate order.
Requires a normal project build and an AVX2 host. Live replay uses the shared
frontend experiment harness; reports add exact Viterbi thread CPU counters.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from statistics import median
import dvbt_frontend_experiment as frontend

ROOT=frontend.ROOT
SOURCE=frontend.SOURCE
replace=frontend.replace_once


def transform(source, variant):
    if variant=='baseline':return source
    helper=(ROOT/'dev/viterbi_experiment_avx2.h').read_text()
    source=replace(source,'static void survivor_row_set(',helper+'\nstatic void survivor_row_set(')
    source=replace(source,'    viterbi_tables_ready = 1;', '''    for (unsigned s=0;s<64;++s) {
        bench_exp0[s]=viterbi_expected_table[s][0];
        bench_exp1[s]=viterbi_expected_table[s][1];
    }
    viterbi_tables_ready = 1;''')
    start=source.index('        for (uint32_t next_state = 0; next_state < VITERBI_STATE_COUNT; ++next_state) {',source.index('static int live_viterbi_stream_decode_bytes('))
    end=source.index('        if (step_best >= inf)',start)
    scalar=source[start:end]
    vector='''        bench_acs_avx2(old_metrics,new_metrics,pair->cost,history_row,&step_best,&best_state);
'''
    if variant=='check':
        vector=scalar+'''        float reference_metrics[64];
        uint8_t reference_history[8];
        float reference_best=step_best;
        int reference_state=best_state;
        memcpy(reference_metrics,new_metrics,sizeof(reference_metrics));
        memcpy(reference_history,history_row,sizeof(reference_history));
'''+vector+'''        if (memcmp(reference_metrics,new_metrics,sizeof(reference_metrics)) ||
            memcmp(reference_history,history_row,sizeof(reference_history)) ||
            memcmp(&reference_best,&step_best,sizeof(float)) || reference_state!=best_state) {
            fprintf(stderr,"Viterbi per-step mismatch at %llu\\n",(unsigned long long)live_viterbi_stream.steps);
            abort();
        }
'''
    return source[:start]+vector+source[end:]


def instrument(source):
    source=replace(source,'static int live_viterbi_stream_decode_bytes(','''static double bench_viterbi_cpu;
static unsigned long long bench_viterbi_pairs;
static double bench_thread_cpu(void) {
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
static int live_viterbi_stream_decode_bytes(''')
    a=source.index('static int live_viterbi_stream_decode_bytes(')
    b=source.index('static size_t dvbt_depuncture_pair_count(',a)
    section=source[a:b]
    section=replace(section,'    const float inf = 1.0e30f;', '    double bench_start=bench_thread_cpu();\n    const float inf = 1.0e30f;')
    section=replace(section,'    out = live_viterbi_stream.output;', '    bench_viterbi_pairs+=pair_count;\n    out = live_viterbi_stream.output;')
    section=replace(section,'done:\n    return rc;', 'done:\n    bench_viterbi_cpu+=bench_thread_cpu()-bench_start;\n    return rc;')
    source=source[:a]+section+source[b:]
    source=replace(source,'    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);','''    fprintf(stderr,"[bench-iq] dropped=%llu max_samples=%zu capacity=%zu\\n",(unsigned long long)live_iq_ring.overrun_samples,live_iq_ring.max_count,live_iq_ring.cap);
    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);''')
    return replace(source,'    free(worker_dibits);\n    return NULL;','''    free(worker_dibits);
    fprintf(stderr,"[bench-fec] cpu=%.6f\\n",bench_thread_cpu());
    fprintf(stderr,"[bench-viterbi] cpu=%.9f pairs=%llu\\n",bench_viterbi_cpu,bench_viterbi_pairs);
    return NULL;''')


def build(out, baseline_ref):
    source=subprocess.check_output(['git','show',baseline_ref+':'+str(SOURCE.relative_to(ROOT))],cwd=ROOT,text=True)
    objects=[str(p) for p in sorted((ROOT/'build/components/dvbt').glob('*.o')) if p.name!='probe_constellation.o']
    if len(objects)!=9:raise ValueError('Build the project first')
    entry=out/'entry.c'
    entry.write_text('''#include "dvbt_backend.h"
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>
int main(int argc,char **argv) {
 int rc=dvbt_backend_main(argc,argv);
 struct rusage r; getrusage(RUSAGE_SELF,&r);
 struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
 fprintf(stderr,"[bench-process] user=%.6f system=%.6f rss_kib=%ld ofdm_cpu=%.6f\\n",r.ru_utime.tv_sec+r.ru_utime.tv_usec*1e-6,r.ru_stime.tv_sec+r.ru_stime.tv_usec*1e-6,r.ru_maxrss,t.tv_sec+t.tv_nsec*1e-9);
 return rc;
}
''')
    flags=['cc','-O2','-Wall','-Wextra','-std=c11','-O3','-I'+str(ROOT/'components/dvbt/include'),'-I'+str(ROOT/'src'),'-DLINRAD_EMBEDDED_DVBT','-pthread']
    commands=[]
    for variant in ('baseline','avx2','check'):
        path=out/(variant+'.c');path.write_text(instrument(transform(source,variant)))
        cmd=[*flags,str(path),str(entry),*objects,'-lfftw3f','-lm','-o',str(out/variant)]
        subprocess.run(cmd,check=True);commands.append(cmd)
    fixture=subprocess.check_output(['git','show',baseline_ref+':tests/test_dvbt_stream.c'],cwd=ROOT,text=True)
    fixture=replace(fixture,'#include "../components/dvbt/src/probe_constellation.c"','#include "check.c"')
    fixture=replace(fixture,'    puts("DVB-T streaming', '''    /* Floating-point soft values, ties, erasures, bursts and reset/chunk tails.
     * check.c compares all 64 metrics, survivor bits and best-state decisions
     * against the original loop at EVERY trellis step. */
    qpsk_dibit_t noisy[1512*7];
    uint32_t rng=91821;
    for (unsigned rate=0;rate<5;++rate) {
      for (unsigned mode=0;mode<3;++mode) {
        live_viterbi_stream_reset();
        for (unsigned chunk=0;chunk<9;++chunk) {
          size_t n=1512*(1+chunk%7);
          for (size_t i=0;i<n;++i) {
            float *costs[]={noisy[i].x_cost,noisy[i].y_cost};
            for(unsigned xy=0;xy<2;++xy) for(unsigned bit=0;bit<2;++bit) {
              rng=1664525u*rng+1013904223u;
              costs[xy][bit]=mode==0 ? 0.0f : (float)(rng>>8)/16777216.0f*(mode==1 ? 1.0f : 10000.0f);
              if (chunk==4 && i<n/2) costs[xy][bit]=0.0f;
            }
          }
          uint8_t *bytes;size_t count;
          assert(!live_viterbi_stream_decode_bytes(rates[rate],noisy,n,chunk!=0,&bytes,&count,NULL,NULL,NULL));
        }
      }
    }
    puts("DVB-T streaming''')
    path=out/'test.c';path.write_text(fixture)
    cmd=[*flags,str(path),*objects,'-lfftw3f','-lm','-o',str(out/'test')]
    subprocess.run(cmd,check=True);commands.append(cmd)
    meta=dict(commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              baseline_ref=baseline_ref,source_sha256=hashlib.sha256(source.encode()).hexdigest(),commands=commands,
              linked_object_sha256={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in objects})
    (out/'build.json').write_text(json.dumps(meta,indent=2)+'\n')


def live_detail(row, case):
    """Distinguish keeping up with live input from silently slowed playback."""
    log=Path(row['log_prefix']+'.log').read_text()
    source_log=Path(row['log_prefix']+'.source.log').read_text()
    sent=re.search(r'end of file: (\d+) samples, ([\d.]+) s elapsed',source_log)
    if not sent:raise RuntimeError('Missing source completion counter')
    nominal=row['input_samples']/case['rate']
    row.update(source_samples=int(sent[1]),source_elapsed_s=float(sent[2]),nominal_duration_s=nominal)
    row['source_slowdown_s']=row['source_elapsed_s']-nominal
    queued=list(map(int,re.findall(r'queued_inner[^\n]*queued_symbols=(\d+)',log)))
    section=max(1,len(queued)//3)
    row['queue_first_third_median_symbols']=median(queued[:section]) if queued else None
    row['queue_last_third_median_symbols']=median(queued[-section:]) if queued else None
    row['queue_capacity_symbols']=1024
    gi=1/int(case.get('gi','1/32').split('/')[1])
    symbol_s=2048*(1+gi)/(case['bandwidth']*8/7)
    jobs=re.findall(r'worker_done seq=(\d+) processed_symbols=(\d+) job_symbols=(\d+).*?elapsed=([\d.]+)s status=(\w+)',log)
    ratios=[float(t)/(int(n)*symbol_s) for _,_,n,t,status in jobs if status=='ok' and int(n)]
    row['worker_jobs']=len(jobs)
    row['worker_status_counts']={s:sum(j[-1]==s for j in jobs) for s in set(j[-1] for j in jobs)}
    row['worker_max_elapsed_to_signal_ratio']=max(ratios,default=None)
    row['worker_median_elapsed_to_signal_ratio']=median(ratios) if ratios else None
    row['kept_source_rate']=row['source_elapsed_s']<=nominal*1.01+.05
    fixed=re.search(r'^\[fixed-iq\] (.+)$',source_log,re.M)
    if fixed:
        row['fixed_clock_source']=json.loads(fixed[1])
        assert not row['fixed_clock_source']['dropped_samples'],('Source FIFO overflow',case['name'])
    assert row['source_samples']==row['input_samples']
    assert row['kept_source_rate'],('Playback slowed',case['name'],row['source_slowdown_s'])
    if case.get('ts_pattern')=='datv-iq counter/PAT/PMT':
        data=Path(row['log_prefix']+'.ts').read_bytes();payload=bad=0
        for start in range(0,len(data),188):
            ts=data[start:start+188]
            assert len(ts)==188 and ts[0]==0x47
            pid=((ts[1]&31)<<8)|ts[2]
            if pid==256:
                index=(ts[4]-4)&255
                valid=ts[3]==(0x10|(index&15)) and ts[4:]==bytes((index+j)&255 for j in range(4,188))
                payload+=1;bad+=not valid
            else:assert pid in (0,100),('Unexpected PID',pid)
        row.update(known_payload_packets=payload,bad_payload_packets=bad)
        assert payload>100 and not bad
    if case.get('expected_clean'):
        assert not row['rs_uncorrectable'],('Uncorrectable blocks in clean IQ',case['name'])
        assert all(s=='ok' for s in row['worker_status_counts'])


def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action',choices=('build','test','kernel','run'))
    p.add_argument('output',type=Path);p.add_argument('manifest',nargs='?',type=Path)
    p.add_argument('--repeat',type=int,default=2)
    p.add_argument('--pairs',type=int,default=1000000)
    p.add_argument('--baseline-ref',default='3b9077b')
    p.add_argument('--sdr-live',action='store_true',help='Fixed-clock source with a 100 ms FIFO; overflow loses samples instead of slowing playback')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if a.repeat<1 or a.pairs<1:p.error('repeat and pairs must be positive')
    if a.action=='build':build(out,a.baseline_ref);return
    if a.action=='test':
        r=subprocess.run([str(out/'test')],text=True,capture_output=True,check=True)
        (out/'test.json').write_text(json.dumps(dict(passed=True,stdout=r.stdout,stderr=r.stderr),indent=2)+'\n')
        print(r.stdout);return
    if a.action=='kernel':
        rows=[]
        for rep in range(a.repeat):
            for variant in ('baseline','avx2') if rep%2==0 else ('avx2','baseline'):
                r=subprocess.run([str(out/variant),'--bench-viterbi-pairs',str(a.pairs)],text=True,capture_output=True,check=True)
                (out/f'kernel-{rep}-{variant}.log').write_text(r.stderr)
                seconds=float(re.search(r'total_raw=([\d.]+)s',r.stderr)[1])
                row=dict(variant=variant,repeat=rep,pairs=a.pairs,raw_wall_s=seconds)
                rows.append(row);print(row,flush=True)
        (out/'kernel.json').write_text(json.dumps(rows,indent=2)+'\n');return
    if not a.manifest:p.error('run needs a manifest')
    cases=json.loads(a.manifest.read_text());logs=out/(a.manifest.stem+'-logs');logs.mkdir(exist_ok=True)
    for case in cases:
        if Path(case['name']).name!=case['name']:p.error('case name must be a filename')
        if case['format'] not in ('u8','s16') or case['rate']<=0:p.error('invalid input format/rate')
    rows=[]
    for case in cases:
        for rep in range(case.get('repeat',a.repeat)):
            pair=[]
            for variant in ('baseline','avx2') if rep%2==0 else ('avx2','baseline'):
                feeder=[sys.executable,str(ROOT/'dev/fixed_iq_source.py')] if a.sdr_live else None
                row=frontend.run_one(out,logs,case,variant,rep,feeder_override=feeder)
                log=Path(row['log_prefix']+'.log').read_text()
                m=re.search(r'\[bench-viterbi\] cpu=([\d.]+) pairs=(\d+)',log)
                if not m:raise RuntimeError('Missing Viterbi CPU counters')
                row.update(viterbi_cpu_s=float(m[1]),viterbi_pairs=int(m[2]))
                live_detail(row,case)
                rows.append(row);pair.append(row)
                (out/(a.manifest.stem+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
                assert row['iq_dropped']==0 and row['fec_dropped']==0
                assert row['processed_input_samples']==row['input_samples']
            for field in ('ts_bytes','ts_sha256','rs_uncorrectable','viterbi_pairs'):
                assert pair[0][field]==pair[1][field],(case['name'],field)


if __name__=='__main__':main()
