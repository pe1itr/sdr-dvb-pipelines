#!/usr/bin/env python3
"""Isolated DVB-T experiments; never changes the production decoder/build.

Build the normal project first. This compiles copies of probe_constellation.c
with the same -O3 flags and links the existing embedded DVB-T objects. A tiny
entry point calls the real dvbt_backend_main, without GTK or a video player.
The baseline includes the same diagnostic CPU counters as the candidates.

build OUT: build baseline, rotations, buffers, combined. Optional --variants
cached (exact reuse of rotation/FFT results), alloc-profile (FEC allocation cost),
current (the working-tree decoder). --baseline-ref defaults to the pre-change
revision 1069f21; other linked backend objects always come from build/.
impair OUT MANIFEST: generate deterministic noise/fades/drift on a u8 recording
(requires NumPy); writes controlled.json for the run command.
run OUT MANIFEST [--repeat N]: replay every input at its original live rate
using the project's iq_file_to_stdout; write TS, logs and MANIFEST-results.json.
Manifest: list of {name, path, format, rate, bandwidth, fec, gi, offset_s,
duration_s}. Optional offset_s/duration_s extract a fixed input segment once.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'components/dvbt/src/probe_constellation.c'
VARIANTS = ('baseline', 'rotations', 'buffers', 'combined', 'cached', 'alloc-profile', 'current')


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Expected one source anchor, found {text.count(old)}: {old[:90]}')
    return text.replace(old, new)


ROTATION_HELPER = r'''
/* Experiment: exact anchor every 64 samples; table for offsets in the block.
 * Double arithmetic bounds recurrence error before conversion back to float.
 * Not claimed bit-identical: compare recovered TS on the same recordings. */
static void bench_rotate_fft(const complexf_t *samples, fftwf_complex *out,
                             size_t n, size_t base, double rate, double cfo)
{
    double cs[64], sn[64];
    double step = -2.0 * M_PI * cfo / rate;
    for (unsigned j=0; j<64; ++j) { cs[j]=cos(step*j); sn[j]=sin(step*j); }
    for (size_t k=0; k<n; k+=64) {
        double phase=-2.0*M_PI*cfo*((double)(base+k)/rate);
        double a=cos(phase), b=sin(phase);
        size_t count=n-k < 64 ? n-k : 64;
        for (size_t j=0; j<count; ++j) {
            double c=a*cs[j]-b*sn[j], s=a*sn[j]+b*cs[j];
            complexf_t v=samples[base+k+j];
            out[k+j][0]=(float)(v.re*c-v.im*s);
            out[k+j][1]=(float)(v.re*s+v.im*c);
        }
    }
}
'''


def rotations(text):
    text = replace_once(text, 'static uint32_t guard_samples(', ROTATION_HELPER + '\nstatic uint32_t guard_samples(')
    pattern = re.compile(r'for \((?:uint32_t )?k = 0; k < (\w+)( && k < nfft)?; \+\+k\) \{\s*'
                         r'double t = \(double\)\(base \+ k\) / \(double\)cfg->sample_rate_hz;\s*'
                         r'complexf_t v = c_rotate\(samples\[base \+ k\], -2\.0 \* M_PI \* cfo_hz \* t\);\s*'
                         r'fft_in\[k\]\[0\] = v.re;\s*fft_in\[k\]\[1\] = v.im;\s*}')
    def loop_replacement(match):
        length = f'({match[1]} < nfft ? {match[1]} : nfft)' if match[2] else match[1]
        return f'bench_rotate_fft(samples, fft_in, {length}, base, cfg->sample_rate_hz, cfo_hz);'
    text, count = pattern.subn(loop_replacement, text)
    if count != 8:
        raise ValueError(f'Unexpected FFT rotation loop count: {count}')
    return payload_cache(text)


def payload_cache(text):
    # The payload is derotated twice: once for power, again for demapping.
    # Keep the first result in the existing per-symbol soft-symbol workspace.
    start = text.index('static int write_dvbt2k_qpsk_constellation(')
    end = text.index('static int write_constellation(', start)
    section = text[start:end]
    section = replace_once(section, 'power_sum += c_abs2(z);', 'rx_sio_soft[di] = z;\n            power_sum += c_abs2(z);')
    section = replace_once(section, 'z = c_rotate(z, -(phase_intercept + phase_slope * x));\n            if (normalizer > 0.0)',
                           'z = rx_sio_soft[di];\n            if (normalizer > 0.0)')
    # Remove the now-unused coordinate from the second payload loop only.
    split = section.index('if (data_count > 0 && power_sum > 0.0)')
    tail = replace_once(section[split:], '            double x;\n', '')
    tail = replace_once(tail, '            x = (double)((int)logical_k - 852);\n', '')
    section = section[:split] + tail
    return text[:start] + section + text[end:]


def cached(text):
    """Exact alternative: reuse identical rotation/FFT work across hypotheses."""
    start=text.index('static int scan_dvbt2k_pilot_alignment(')
    end=text.index('static int write_dvbt2k_qpsk_constellation(',start)
    section=text[start:end]
    section=replace_once(section,'    double best_lock = -1.0;', '''    complexf_t *cache=malloc((size_t)scan_symbols * RBDVBT_DVBT_2K_FFT_SIZE * sizeof(*cache));
    if (!cache) return -1;
    double best_lock = -1.0;''')
    a=section.index('                    for (uint32_t k = 0;')
    b=section.index('                    if (estimate_dvbt2k_pilot_phase',a)
    block=section[a:b]
    section=section[:a]+'''                    if (conjugate==conjugate_first && symbol_phase==phase_first && bin_shift==bin_first) {
'''+block+'''                        memcpy(cache+(size_t)s*RBDVBT_DVBT_2K_FFT_SIZE, shifted, RBDVBT_DVBT_2K_FFT_SIZE*sizeof(*cache));
                    } else {
                        memcpy(shifted, cache+(size_t)s*RBDVBT_DVBT_2K_FFT_SIZE, RBDVBT_DVBT_2K_FFT_SIZE*sizeof(*cache));
                    }
'''+section[b:]
    section=replace_once(section,'    if (best_lock < 0.0)', '    free(cache);\n    if (best_lock < 0.0)')
    return payload_cache(text[:start]+section+text[end:])


BUFFER_DECL = r'''
/* Experiment: these scratch buffers are owned solely by the OFDM thread. */
static complexf_t *bench_input, *bench_merged;
static size_t bench_input_cap, bench_merged_cap;
static void bench_release_samples(complexf_t *p);
static complexf_t *bench_reserve(complexf_t **p, size_t *cap, size_t n)
{
    if (n>*cap) {
        complexf_t *next=realloc(*p,n*sizeof(*next));
        if (!next) return NULL;
        *p=next; *cap=n;
    }
    return *p;
}
'''


def buffers(text):
    text = replace_once(text, 'static int read_probe_samples(', BUFFER_DECL+'\nstatic int read_probe_samples(')
    text = replace_once(text, 'complexf_t *samples = calloc((size_t)sample_limit, sizeof(*samples));',
                         'complexf_t *samples = cfg->live_mode ? bench_reserve(&bench_input, &bench_input_cap, (size_t)sample_limit) : calloc((size_t)sample_limit, sizeof(*samples));')
    text = replace_once(text, 'static live_resampler_state_t live_resampler_state;', '''static live_resampler_state_t live_resampler_state;
static void bench_release_samples(complexf_t *p) {
    if (p != bench_input && p != bench_merged && p != live_resampler_state.out) free(p);
}''')
    text = replace_once(text, '''    out = malloc(out_n * sizeof(*out));
    if (out == NULL) {
        fprintf(stderr, "failed to allocate live resample output buffer\\n");
        return -1;
    }
    memcpy(out, st->out, out_n * sizeof(*out));''', '    out = st->out; /* Borrow until the next resample; no output copy. */')
    text = replace_once(text, 'merged = calloc(prefix + *count, sizeof(*merged));',
                         'merged = bench_reserve(&bench_merged, &bench_merged_cap, prefix + *count);')
    # All remaining callers own non-scratch pointers; the ownership check keeps
    # those normal frees intact. No FEC queue pointers borrow these buffers.
    text = text.replace('free(samples);', 'bench_release_samples(samples);')
    text = text.replace('free(*samples);', 'bench_release_samples(*samples);')
    return text


def allocation_profile(text):
    """Measure FEC job allocation/copy/free cost before considering a pool."""
    helper = '''static double bench_thread_seconds(void) {
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
'''
    text = replace_once(text, 'static void live_decode_free_job(', helper+'\nstatic void live_decode_free_job(')
    text = replace_once(text, '''    free(job->symbols);
    free(job->model_symbols);
    free(job);
}''', '''    double bench_t0=bench_thread_seconds();
    free(job->symbols);
    free(job->model_symbols);
    free(job);
    fprintf(stderr,"[bench-job-free] cpu=%.9f\\n",bench_thread_seconds()-bench_t0);
}''')
    text = replace_once(text, '    job = calloc(1, sizeof(*job));',
                         '    double bench_t0=bench_thread_seconds();\n    job = calloc(1, sizeof(*job));')
    text = replace_once(text, '    job->symbol_cell_count = symbol_cell_count;',
                         '    fprintf(stderr,"[bench-job-allocate-copy] cpu=%.9f\\n",bench_thread_seconds()-bench_t0);\n    job->symbol_cell_count = symbol_cell_count;')
    return text


def build(out, selected, baseline_ref):
    out.mkdir(parents=True, exist_ok=True)
    entry = out / 'entry.c'
    entry.write_text('''#include "dvbt_backend.h"
#include <sys/resource.h>
#include <time.h>
#include <stdio.h>
int main(int argc, char **argv) {
 int rc=dvbt_backend_main(argc,argv);
 struct rusage r; getrusage(RUSAGE_SELF,&r);
 struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
 fprintf(stderr,"[bench-process] user=%.6f system=%.6f rss_kib=%ld ofdm_cpu=%.6f\\n",r.ru_utime.tv_sec+r.ru_utime.tv_usec*1e-6,r.ru_stime.tv_sec+r.ru_stime.tv_usec*1e-6,r.ru_maxrss,t.tv_sec+t.tv_nsec*1e-9);
 return rc;
}
''')
    current = SOURCE.read_text()
    original = (subprocess.check_output(['git', 'show', baseline_ref+':'+str(SOURCE.relative_to(ROOT))],
                                       cwd=ROOT, text=True) if baseline_ref else current)
    objects = sorted((ROOT / 'build/components/dvbt').glob('*.o'))
    objects = [str(p) for p in objects if p.name != 'probe_constellation.o']
    if len(objects) != 9:
        raise ValueError('Build project first; expected nine other DVB-T objects')
    variants = {'baseline': lambda: original, 'rotations': lambda: rotations(original),
                'buffers': lambda: buffers(original), 'combined': lambda: buffers(rotations(original)),
                'alloc-profile': lambda: allocation_profile(original), 'cached': lambda: cached(original),
                'current': lambda: current}
    commands = []
    for name, transform in variants.items():
        if name not in selected: continue
        source = transform()
        source = replace_once(source, '    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);', '''    fprintf(stderr,"[bench-iq] dropped=%llu max_samples=%zu capacity=%zu\\n",(unsigned long long)live_iq_ring.overrun_samples,live_iq_ring.max_count,live_iq_ring.cap);
    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);''')
        source = replace_once(source, '    free(worker_dibits);\n    return NULL;', '''    free(worker_dibits);
    struct timespec bench_t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &bench_t);
    fprintf(stderr,"[bench-fec] cpu=%.6f\\n", bench_t.tv_sec + bench_t.tv_nsec*1e-9);
    return NULL;''')
        path = out / (name + '.c')
        path.write_text(source)
        command = ['cc', '-O2', '-Wall', '-Wextra', '-std=c11', '-O3',
                   '-I'+str(ROOT/'components/dvbt/include'), '-I'+str(ROOT/'src'),
                   '-DLINRAD_EMBEDDED_DVBT', '-pthread', str(path), str(entry),
                   *objects, '-lfftw3f', '-lm', '-o', str(out/name)]
        subprocess.run(command, check=True)
        commands.append(command)
    meta = {'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
            'baseline_ref': baseline_ref, 'baseline_source_sha256': hashlib.sha256(original.encode()).hexdigest(),
            'commands': commands, 'variants': selected}
    (out/'build.json').write_text(json.dumps(meta, indent=2)+'\n')


def prepare(out, case):
    source = Path(case['path'])
    if 'duration_s' not in case and not case.get('offset_s'):
        return source
    width = 2 if case['format']=='u8' else 4
    offset = round(case.get('offset_s', 0)*case['rate'])*width
    count = (round(case['duration_s']*case['rate'])*width if 'duration_s' in case
             else source.stat().st_size-offset)
    if offset < 0 or count <= 0:
        raise ValueError('Recording segment must have a nonnegative offset and positive length')
    target = out / (case['name']+'.iq')
    signature = {'source':str(source.resolve()), 'size':source.stat().st_size,
                 'mtime_ns':source.stat().st_mtime_ns, 'offset_s':case.get('offset_s',0),
                 'count':count, 'rate':case['rate'], 'format':case['format']}
    metadata = target.with_suffix('.segment.json')
    if not target.exists() or not metadata.exists() or json.loads(metadata.read_text()) != signature:
        with source.open('rb') as f:
            f.seek(offset)
            data = f.read(count)
        if len(data)!=count:
            raise ValueError('Recording too short: '+str(source))
        target.write_bytes(data)
        metadata.write_text(json.dumps(signature,indent=2)+'\n')
    return target


def impair(out, manifest):
    import numpy as np
    case=json.loads(manifest.read_text())[0]
    if case['format']!='u8': raise ValueError('impair expects a u8 reference recording')
    source=prepare(out,case)
    x=np.fromfile(source,dtype=np.uint8).reshape(-1,2)
    z=((x[:,0].astype(np.float32)-128)+1j*(x[:,1].astype(np.float32)-128))/128
    rng=np.random.default_rng(20261105)
    t=np.arange(len(z))/case['rate']
    sigma=np.sqrt(float(np.mean(abs(z)**2))/10**(14/10)/2)
    gain=np.ones(len(z),dtype=np.float32)
    for a,b in ((3,3.35),(6,6.35)): gain[(t>=a)&(t<b)]=.1
    z=z*gain*np.exp(1j*2*np.pi*(3*t*t/2))
    z+=sigma*(rng.standard_normal(len(z))+1j*rng.standard_normal(len(z)))
    raw=np.column_stack((z.real,z.imag))*32767
    meta={'seed':20261105, 'noise_db':14, 'drift_hz_per_s':3, 'fade_gain':.1,
          'fade_intervals_s':[[3,3.35],[6,6.35]], 'clipped_components':int(np.count_nonzero(abs(raw)>32767)),
          'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()}
    target=out/'controlled-fade.iq'
    np.clip(np.rint(raw),-32768,32767).astype('<i2').tofile(target)
    case={k:v for k,v in case.items() if k not in ('offset_s','duration_s')}
    case.update(name='250k-controlled-fade',path=str(target),format='s16')
    (out/'controlled.json').write_text(json.dumps([case],indent=2)+'\n')
    (out/'controlled-generation.json').write_text(json.dumps(meta,indent=2)+'\n')


def run_one(out, logs, case, variant, repeat):
    iq = prepare(out, case)
    prefix = logs / f"{case['name']}-{repeat}-{variant}"
    feeder = [str(ROOT/'build/iq_file_to_stdout'), '--in', str(iq),
              '--input-format', case['format'], '--sample-rate', str(case['rate'])]
    command = [str(out/variant), '--stdin', '--live', '--probe-constellation',
               '--resample-to-dvbt-rate', '--input-format', case['format'],
               '--sample-rate', str(case['rate']), '--sr', str(case['bandwidth']),
               '--gi', case.get('gi','1/32'), '--fec', case['fec'],
               '--ts-out', '-', '--loglevel', 'info']
    env = {k:v for k,v in os.environ.items() if not k.startswith('LINRAD_')}
    first = None
    with prefix.with_suffix('.log').open('wb') as log, prefix.with_suffix('.ts').open('wb') as ts, prefix.with_suffix('.source.log').open('wb') as source_log:
        start = time.monotonic()
        source = subprocess.Popen(feeder, stdout=subprocess.PIPE, stderr=source_log, env=env)
        rx = None
        try:
            rx = subprocess.Popen(command, stdin=source.stdout, stdout=subprocess.PIPE, stderr=log, env=env)
            source.stdout.close()
            with selectors.DefaultSelector() as selector:
                selector.register(rx.stdout, selectors.EVENT_READ)
                while True:
                    if not selector.select(60):
                        raise TimeoutError('No TS/EOF for 60 seconds')
                    data = os.read(rx.stdout.fileno(), 65536)
                    if not data: break
                    if first is None: first=time.monotonic()-start
                    ts.write(data)
            code=rx.wait(timeout=15)
            source_code=source.wait(timeout=15)
            elapsed=time.monotonic()-start
        finally:
            for proc in (rx,source):
                if proc is not None and proc.poll() is None: proc.kill(); proc.wait()
    log = prefix.with_suffix('.log').read_text()
    match = re.search(r'\[bench-process\] user=([\d.]+) system=([\d.]+) rss_kib=(\d+) ofdm_cpu=([\d.]+)',log)
    if not match or code or source_code:
        raise RuntimeError(f'Failed benchmark: {prefix}; receiver={code}, source={source_code}')
    data = prefix.with_suffix('.ts').read_bytes()
    counts = lambda pattern: sum(map(int,re.findall(pattern,log)))
    if not re.search(r'\[bench-iq\] dropped=\d+',log):
        raise RuntimeError('Rebuild experiment binaries to read the total IQ loss counter')
    expected = iq.stat().st_size // (2 if case['format']=='u8' else 4)
    result = {'case':case['name'], 'variant':variant, 'repeat':repeat,
              'cpu_s':float(match[1])+float(match[2]), 'ofdm_cpu_s':float(match[4]),
              'rss_kib':int(match[3]), 'elapsed_s':elapsed, 'first_ts_s':first,
              'fec_cpu_s':sum(map(float,re.findall(r'\[bench-fec\] cpu=([\d.]+)',log))),
              'ts_bytes':len(data), 'ts_sha256':hashlib.sha256(data).hexdigest(),
              'ts_packet_aligned':len(data)%188==0,
              'iq_dropped':counts(r'\[bench-iq\] dropped=(\d+)'),
              'input_samples':expected,
              'processed_input_samples':counts(r'\[resampler\] in=(\d+)'),
              'fec_dropped':counts(r'backlog full dropping incoming_symbols=(\d+)'),
              'rs_uncorrectable':counts(r'\[outer\][^\n]*rs_uncorrectable=(\d+)'),
              'queue_max_symbols':max([0]+list(map(int,re.findall(r'queued_inner[^\n]*queued_symbols=(\d+)',log)))),
              'command':command, 'feeder':feeder, 'log_prefix':str(prefix)}
    print(json.dumps({k:v for k,v in result.items() if k not in ('command','feeder')}),flush=True)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action',choices=('build','run','impair'))
    parser.add_argument('output',type=Path)
    parser.add_argument('manifest',nargs='?',type=Path)
    parser.add_argument('--repeat',type=int,default=3)
    parser.add_argument('--variants',default='baseline,rotations,buffers,combined')
    parser.add_argument('--baseline-ref',default='1069f21',
                        help='Git revision for baseline/prototypes (default: 1069f21); current uses the working tree')
    args=parser.parse_args()
    out=args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    variants=args.variants.split(',')
    if any(v not in VARIANTS for v in variants): parser.error('unknown variant')
    if args.action=='build': build(out,variants,args.baseline_ref); return
    if args.manifest is None: parser.error(args.action+' requires a manifest')
    if args.action=='impair': impair(out,args.manifest); return
    cases=json.loads(args.manifest.read_text())
    if args.repeat<1: parser.error('repeat must be positive')
    for case in cases:
        if case['format'] not in ('u8','s16'): parser.error('format must be u8 or s16')
        if case['rate']<=0: parser.error('rate must be positive')
        if Path(case['name']).name!=case['name']: parser.error('case name must be a filename')
    logs=out/(args.manifest.stem+'-logs')
    logs.mkdir(exist_ok=True)
    results=[]
    for case in cases:
        for repeat in range(args.repeat):
            # Reverse every other pass to reduce ordering/temperature bias.
            for variant in variants if repeat%2==0 else list(reversed(variants)):
                results.append(run_one(out,logs,case,variant,repeat))
                (out/(args.manifest.stem+'-results.json')).write_text(json.dumps(results,indent=2)+'\n')


if __name__=='__main__': main()
