#!/usr/bin/env python3
"""Compare two decoders on identical weak IQ; optional full local recordings.

Requires numpy, build/leandvbtx, and a preserved pre-change linrad_dvb binary.
No SDR, network sockets, video renderer, or realtime pacing is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from test_leansdr import ROOT, transport, weak_iq


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def decode(binary, source, offset, args, output, label, timeout):
    stem = output/label
    ts_path, bb_path = stem.with_suffix('.ts'), stem.with_suffix('.bb')
    start = time.monotonic()
    with source.open('rb') as iq, ts_path.open('wb') as ts, \
            bb_path.open('wb') as bb, stem.with_suffix('.log').open('wb') as log:
        iq.seek(offset)
        command = [str(binary), '--decoder-worker=leansdr', *args]
        if 'DVB-S2' in args:
            command += ['--fd-bbf', str(bb.fileno())]
        subprocess.run(command, stdin=iq, stdout=ts, stderr=log,
                       pass_fds=(bb.fileno(),), check=True, timeout=timeout)
    return dict(ts_sha256=digest(ts_path), bb_sha256=digest(bb_path),
                ts_bytes=ts_path.stat().st_size, bb_bytes=bb_path.stat().st_size,
                seconds=round(time.monotonic()-start, 3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--candidate', type=Path, default=ROOT/'build/linrad_dvb')
    parser.add_argument('--output', type=Path, default=ROOT/'build/diagnostics/weak-signals')
    parser.add_argument('--recordings', action='store_true', help='Also decode full fbn2.raw, org.raw and h.raw')
    parser.add_argument('--recordings-only', action='store_true')
    parser.add_argument('--batch', type=int, choices=(1, 32), default=1)
    parser.add_argument('--seeds', type=int, nargs='+', default=[41, 42])
    parser.add_argument('--timeout', type=int, default=300)
    opts = parser.parse_args()
    opts.reference = opts.reference.resolve()
    opts.candidate = opts.candidate.resolve()
    opts.output.mkdir(parents=True, exist_ok=True)
    report = dict(reference=str(opts.reference), reference_sha256=digest(opts.reference),
                  candidate=str(opts.candidate), candidate_sha256=digest(opts.candidate),
                  helper_sha256=digest(ROOT/'build/ldpc_tool'),
                  batch=opts.batch, cases=[])
    report_path = opts.output/'results.json'
    common = ['--s16', '--sampler', 'rrc', '--rrc-steps', '8']
    soft = ['--ldpc-helper', str(ROOT/'build/ldpc_tool'), '--nhelpers', '1',
            '--ldpc-batch', str(opts.batch)]

    def compare(name, source, offset, args, expected=None, **metadata):
        print(f'{name}: ', end='', flush=True)
        row = dict(name=name, source=str(source), offset=offset, args=args, **metadata)
        for label, binary in [('before', opts.reference), ('after', opts.candidate)]:
            row[label] = decode(binary, source, offset, args, opts.output,
                                name+'-'+label, opts.timeout)
        row['identical'] = all(row['before'][key] == row['after'][key]
                               for key in ('ts_sha256', 'bb_sha256', 'ts_bytes', 'bb_bytes'))
        data = (opts.output/(name+'-after.ts')).read_bytes()
        if len(data)%188:
            raise AssertionError(f'{name}: partial TS packet')
        if expected is not None:
            # Losing lock may lose packets; every recovered packet must still
            # match the transmitted payload and remain in strict input order.
            previous = -1
            valid = damaged = 0
            for pos in range(0, len(data), 188):
                packet = data[pos:pos+188]
                index = int.from_bytes(packet[4:8], 'big')
                if packet != expected[188*index:188*(index+1)]:
                    # Legacy DVB-S can emit RS-damaged packets during loss of
                    # lock. Count them; the A/B bytes must still match exactly.
                    if 'DVB-S2' in args:
                        raise AssertionError(f'{name}: damaged packet at {pos//188}')
                    damaged += 1
                    continue
                if index <= previous:
                    raise AssertionError(f'{name}: reordered packet at {pos//188}')
                previous = index
                valid += 1
            row.update(valid_packets=valid, damaged_packets=damaged)
        report['cases'].append(row)
        report_path.write_text(json.dumps(report, indent=2)+'\n')
        print(f"{len(data)//188} TS packets; " + ('identical' if row['identical'] else 'DIFFERENT'), flush=True)
        return row

    if not opts.recordings_only:
        ts = transport(900)
        # Main contest rate: thorough sweeps. Secondary rates: boundary,
        # healthy reception and fading. All are QPSK.
        modes = [('s2-nf12-125k', 'DVB-S2', '1/2', 4, False, True, 125000),
                 ('s2-nf23-125k', 'DVB-S2', '2/3', 6, False, True, 125000),
                 ('s2-sf12-125k', 'DVB-S2', '1/2', 4, True, True, 125000),
                 ('s2-sf35-125k-no-pilots', 'DVB-S2', '3/5', 5, True, False, 125000),
                 ('s-12-125k', 'DVB-S', '1/2', 0, False, False, 125000),
                 ('s-23-125k', 'DVB-S', '2/3', 0, False, False, 125000)]
        for rate in (35000, 66000, 250000, 333000):
            short = rate in (66000, 333000)
            modes += [(f's2-{"sf" if short else "nf"}12-{rate}', 'DVB-S2', '1/2', 4, short, True, rate),
                      (f's-12-{rate}', 'DVB-S', '1/2', 0, False, False, rate)]
        source = opts.output/'synthetic.iq'
        for mode, standard, fec, mc, short, pilots, rate in modes:
            rows = []
            seeds = opts.seeds if rate == 125000 else opts.seeds[:1]
            levels = (-2, 0, 2, 4, 6, 10) if rate == 125000 else (2, 4, 10)
            for seed in seeds:
                for esn0, fading in [(x, False) for x in levels] + [(10, True)]:
                    name = f'{mode}-seed{seed}-' + ('fade' if fading else f'esn0-{esn0}')
                    source.write_bytes(weak_iq(ts, standard, fec, mc, short, pilots, seed, esn0, fading))
                    iq_sha = digest(source)
                    args = common + ['--standard', standard, '-f', str(4*rate), '--sr', str(rate), '--rrc-rej', '30']
                    if standard == 'DVB-S2':
                        args += ['--modcods', str(1 << mc), '--framesizes', '2' if short else '1'] + soft
                    else:
                        args += ['--cr', fec, '--viterbi', '--fastlock']
                    rows.append(compare(name, source, 0, args, expected=ts, seed=seed,
                                        esn0_db=esn0, fading=fading, iq_sha256=iq_sha))
            # An all-empty comparison proves nothing about weak reception.
            if not any(0 < r['valid_packets'] < len(ts)//188*.65 for r in rows):
                raise AssertionError(f'{mode}: sweep did not exercise partial reception')
            if not any(r['valid_packets'] > len(ts)//188*.65 for r in rows):
                raise AssertionError(f'{mode}: no healthy reception at upper noise levels')
        source.unlink()  # Generated working file only; outputs and hashes remain.

    if opts.recordings or opts.recordings_only:
        for name in ('fbn2', 'org', 'h'):
            source = ROOT/'recordings'/f'{name}.raw'
            metadata = json.loads(Path(str(source)+'.json').read_text())
            standard = metadata['standard']
            args = common + ['--standard', standard, '-f', str(metadata['sample_rate_hz']),
                             '--sr', str(metadata['symbol_rate_hz'])]
            if standard == 'DVB-S2':
                args += ['--modcods', '16', '--framesizes', '3'] + soft
            else:
                args += ['--cr', '1/2', '--fastlock', '--hq', '--inbuf', '262144']
            row = compare(name, source, metadata['header_bytes'], args, iq_sha256=digest(source))
            if name != 'h' and not row['after']['ts_bytes']:
                raise AssertionError(f'{name}: expected a recording with actual reception')

    failures = [r['name'] for r in report['cases'] if not r['identical']]
    print(f"{len(report['cases'])} comparisons; {len(failures)} differences; {report_path}")
    if failures:
        raise SystemExit('Decoder differences: '+', '.join(failures))


if __name__ == '__main__':
    main()
