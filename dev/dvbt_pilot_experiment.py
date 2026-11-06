#!/usr/bin/env python3
"""Profile the current DVB-T frontend and compare a fixed carrier-role table.

Isolated builds; no change to tracking cadence or signal arithmetic. Uses the
fixed-clock source (overflow drops samples, never slows the source). Build the
project first. Other backend objects are linked from build/ and hashed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from statistics import median

import dvbt_frontend_experiment as frontend
from dvbt_viterbi_experiment import live_detail

ROOT = frontend.ROOT
MODEL = ROOT / 'components/dvbt/src/dvbt_2k_model.c'
PROFILE_FUNCTIONS = (
    'resample_sinc_ratio_live', 'find_symbol_start_grdvbt_ml',
    'find_symbol_start_near_local_grdvbt_ml', 'estimate_dvbt2k_pilot_phase',
    'scan_dvbt2k_pilot_alignment', 'write_dvbt2k_qpsk_constellation',
)


def signal_summary(log):
    """Signal decisions, independent of wall-clock timing and thread ordering."""
    snr = list(map(float, re.findall(r'\[dvbt2k\][^\n]*snr=([-\d.]+)dB', log)))
    return dict(
        decoder_snr_db=dict(values=snr, minimum=min(snr) if snr else None,
            median=median(snr) if snr else None, maximum=max(snr) if snr else None),
        signal_decision_sha256={tag: hashlib.sha256('\n'.join(
            re.findall(r'\['+tag+r'\][^\n]*', log)).encode()).hexdigest()
            for tag in ('sync', 'frontend-cont', 'dvbt2k', 'outer')},
        acquisition_chunks=len(re.findall(r'\[sync\][^\n]*mode=acquire', log)),
        tracking_chunks=len(re.findall(r'\[sync\][^\n]*mode=track', log)),
        weak_hold_chunks=len(re.findall(r'holding weak live chunk', log)),
        frontend_resets=len(re.findall(r'reason=frontend-discontinuity', log)),
        outer_acquisitions=len(re.findall(r'\[outer-state\] acquired ', log)),
        # Per-FEC-job TS reports help distinguish initial acquisition from
        # recovering useful output after an impairment. These are not RF SNR.
        ts_job_packets=list(map(int, re.findall(r'\[ts\] packets=(\d+)', log))))


def role_table(source):
    source = frontend.replace_once(source, 'static int initialized;',
        'static uint8_t carrier_roles_mod4[4][RBDVBT_DVBT_2K_ACTIVE_CARRIERS];\nstatic int initialized;')
    start = source.index('rbdvbt_dvbt_carrier_role_t rbdvbt_dvbt_2k_carrier_role(')
    end = source.index('\nint rbdvbt_dvbt_2k_data_carrier(', start)
    original = source[start:end].replace('rbdvbt_dvbt_2k_carrier_role(', 'classify_carrier(')
    source = source[:start] + '''rbdvbt_dvbt_carrier_role_t rbdvbt_dvbt_2k_carrier_role(uint32_t symbol_index,
                                                        uint32_t logical_k)
{
    if (logical_k >= RBDVBT_DVBT_2K_ACTIVE_CARRIERS) {
        return RBDVBT_DVBT_CARRIER_UNUSED;
    }
    rbdvbt_dvbt_2k_model_init();
    return (rbdvbt_dvbt_carrier_role_t)carrier_roles_mod4[symbol_index % 4u][logical_k];
}
''' + source[end:]
    source = frontend.replace_once(source, 'static void build_data_carriers(void)',
        'static ' + original + '\nstatic void build_data_carriers(void)')
    source = frontend.replace_once(source, '    initialized = 1;', '''    for (uint32_t phase = 0; phase < 4u; ++phase) {
        for (uint32_t k = 0; k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++k) {
            carrier_roles_mod4[phase][k] = (uint8_t)classify_carrier(phase, k);
        }
    }
    initialized = 1;''')
    return source


def profile(source):
    helper = '''static double pilot_bench_cpu[7];
static double pilot_bench_now(void) {
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void pilot_bench_fft(fftwf_plan p) {
    double start = pilot_bench_now();
    fftwf_execute(p);
    pilot_bench_cpu[6] += pilot_bench_now() - start;
}
#define fftwf_execute pilot_bench_fft
'''
    source = frontend.replace_once(source, 'static complexf_t c_rotate(', helper + '\nstatic complexf_t c_rotate(')
    for index, name in enumerate(PROFILE_FUNCTIONS):
        match = re.search(r'static [\w]+ ' + name + r'\([^;]*?\)\n\{', source)
        if not match:
            raise ValueError(name)
        start = match.end()
        # Function closing braces in this source are at column zero.
        end = source.index('\n}', start)
        body = source[start:end]
        body = re.sub(r'\breturn ([^;]+);',
            lambda m: '{ pilot_bench_cpu[%d] += pilot_bench_now() - pilot_bench_start; return %s; }' % (index, m[1]), body)
        source = source[:start] + '\n    double pilot_bench_start = pilot_bench_now();' + body + source[end:]
    report = '\n'.join('    fprintf(stderr, "[pilot-profile] %s=%%.9f\\n", pilot_bench_cpu[%d]);' % (name, i)
        for i, name in enumerate((*PROFILE_FUNCTIONS, 'fft')))
    return frontend.replace_once(source, '    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);',
        report + '\n    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);')


def build(out, ref):
    frontend.build(out, ['baseline'], ref)
    meta = json.loads((out/'build.json').read_text())
    baseline = (out/'baseline.c').read_text()
    original = subprocess.check_output(['git', 'show', ref + ':' + str(MODEL.relative_to(ROOT))], cwd=ROOT, text=True)
    (out/'model-baseline.c').write_text(original)
    (out/'model-roles.c').write_text(role_table(original))
    base_command = meta['commands'][0]
    # Pin the model too, so later production integration cannot change baseline.
    model_object = str(ROOT/'build/components/dvbt/dvbt_2k_model.o')
    for variant in ('baseline', 'profile', 'roles', 'roles-profile'):
        path = out/(variant + '.c')
        path.write_text(profile(baseline) if variant.endswith('profile') else baseline)
        model = out/('model-roles.c' if variant.startswith('roles') else 'model-baseline.c')
        command = [str(path) if x == str(out/'baseline.c') else
                   str(out/variant) if x == str(out/'baseline') else
                   str(model) if x == model_object else x for x in base_command]
        subprocess.run(command, check=True)
        meta['commands'].append(command)
    meta['linked_object_sha256'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((ROOT/'build/components/dvbt').glob('*.o'))
        if p.name not in ('probe_constellation.o', 'dvbt_2k_model.o')}
    meta['model_sha256'] = hashlib.sha256(original.encode()).hexdigest()
    meta['variants'] = ['baseline', 'profile', 'roles', 'roles-profile']
    meta['binary_sha256'] = {name: hashlib.sha256((out/name).read_bytes()).hexdigest()
        for name in ('baseline', 'profile', 'roles', 'roles-profile')}
    (out/'build.json').write_text(json.dumps(meta, indent=2)+'\n')


def run(out, manifest, variants, repeat):
    logs = out/(manifest.stem+'-logs')
    logs.mkdir(exist_ok=True)
    results = []
    for case in json.loads(manifest.read_text()):
        for index in range(case.get('repeat', repeat)):
            for variant in variants if index % 2 == 0 else list(reversed(variants)):
                row = frontend.run_one(out, logs, case, variant, index,
                    [sys.executable, str(ROOT/'dev/fixed_iq_source.py')])
                live_detail(row, case)
                assert row['processed_input_samples'] == row['input_samples']
                assert not row['iq_dropped'] and not row['fec_dropped']
                log = Path(row['log_prefix']+'.log').read_text()
                row.update(signal_summary(log))
                row['profile_inclusive_cpu_s'] = {k: float(v) for k, v in
                    re.findall(r'\[pilot-profile\] (\w+)=([\d.]+)', log)}
                results.append(row)
                (out/(manifest.stem+'-results.json')).write_text(json.dumps(results, indent=2)+'\n')
                peers = [r for r in results if r['case'] == row['case']]
                assert len({r['ts_sha256'] for r in peers}) == 1, 'Different TS'
                assert len({r['rs_uncorrectable'] for r in peers}) == 1, 'Different RS failures'
                assert all(r['signal_decision_sha256'] == row['signal_decision_sha256'] for r in peers), 'Different tracking/decoding decisions'


def check(out):
    """Compare every carrier role/data index with the pinned original model."""
    header = (ROOT/'components/dvbt/include/dvbt_2k_model.h').read_text()
    reference_header = '#include "dvbt_2k_model.h"\n' + header[header.index('void rbdvbt_dvbt_2k_model_init'):header.rindex('#endif')]
    reference_header = reference_header.replace('rbdvbt_dvbt_2k_', 'reference_')
    (out/'reference.h').write_text(reference_header)
    reference = (out/'model-baseline.c').read_text().replace('"dvbt_2k_model.h"', '"reference.h"')
    (out/'reference.c').write_text(reference.replace('rbdvbt_dvbt_2k_', 'reference_'))
    test = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "dvbt_2k_model.h"
#include "reference.h"
int main(void) {
    /* Exercise role lookup before explicit initialization, then all 4 phases,
     * full frame/superframe boundaries and uint32_t symbol-index wraparound. */
    assert(rbdvbt_dvbt_2k_carrier_role(UINT32_MAX, 1704) == reference_carrier_role(UINT32_MAX, 1704));
    unsigned roles = 0, data = 0;
    for (unsigned s = 0; s < 548; ++s) {
        uint32_t symbol = s < 544 ? s : UINT32_MAX-(s-544);
        for (uint32_t k = 0; k < 2050; ++k) {
            assert(rbdvbt_dvbt_2k_carrier_role(symbol,k) == reference_carrier_role(symbol,k));
            assert(rbdvbt_dvbt_2k_pilot_sign(k) == reference_pilot_sign(k));
            assert(rbdvbt_dvbt_2k_logical_to_fft_bin(k) == reference_logical_to_fft_bin(k));
            ++roles;
        }
        for (uint32_t i = 0; i < 1514; ++i) {
            uint32_t a = UINT32_MAX, b = UINT32_MAX;
            assert(rbdvbt_dvbt_2k_data_carrier(symbol,i,&a) == reference_data_carrier(symbol,i,&b));
            assert(a == b); ++data;
        }
        assert(rbdvbt_dvbt_2k_data_carrier(symbol,0,NULL) == -1);
        assert(rbdvbt_dvbt_2k_carrier_role(symbol,UINT32_MAX) == RBDVBT_DVBT_CARRIER_UNUSED);
    }
    for (uint32_t i = 0; i < 1514; ++i) {
        uint32_t a = UINT32_MAX, b = UINT32_MAX;
        assert(rbdvbt_dvbt_2k_symbol_interleaver(i,&a) == reference_symbol_interleaver(i,&b));
        assert(a == b);
        assert(rbdvbt_dvbt_2k_symbol_deinterleaver(i,&a) == reference_symbol_deinterleaver(i,&b));
        assert(a == b);
    }
    printf("Exact original-model comparison: %u roles, %u data indices; bounds and interleavers OK\n",roles,data);
    return 0;
}
'''
    (out/'check.c').write_text(test)
    checks = []
    for label, model in (('prototype', out/'model-roles.c'), ('current', MODEL)):
        binary = out/('check-'+label)
        command = ['cc', '-O3', '-Wall', '-Wextra', '-std=c11',
            '-I'+str(ROOT/'components/dvbt/include'), str(out/'check.c'),
            str(out/'reference.c'), str(model), '-o', str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.check_output([str(binary)], text=True)
        print(label + ': ' + result, end='')
        checks.append(dict(command=command, result=result, variant=label,
            model_sha256=hashlib.sha256(model.read_bytes()).hexdigest()))
    (out/'check.json').write_text(json.dumps(checks, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'check', 'run'))
    parser.add_argument('output', type=Path)
    parser.add_argument('manifest', nargs='?', type=Path)
    parser.add_argument('--baseline-ref', default='d45f7c8')
    parser.add_argument('--variants', default='baseline,roles')
    parser.add_argument('--repeat', type=int, default=2)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.action == 'build':
        build(out, args.baseline_ref)
    elif args.action == 'check':
        check(out)
    elif args.manifest is None:
        parser.error('run requires a manifest')
    else:
        run(out, args.manifest, args.variants.split(','), args.repeat)


if __name__ == '__main__':
    main()
