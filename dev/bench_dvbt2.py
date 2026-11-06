#!/usr/bin/env python3
"""Measure the actual DVB-T2 worker with a fixed-clock, bounded IQ source.

No radio access. Record CPU seconds, peak RSS, source/decoder losses and the
TS hash so faster runs cannot hide dropped input or different decoded output.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path, required=True, help='standalone dvbt2_worker executable')
    parser.add_argument('--iq', type=Path, required=True)
    parser.add_argument('--sample-rate', type=int, default=10000000)
    parser.add_argument('--input-format', choices=('s16', 'u8'), default='s16')
    parser.add_argument('--output', type=Path, required=True, help='new result prefix; existing results are preserved')
    parser.add_argument('--profile', action='store_true')
    args = parser.parse_args()
    if args.sample_rate <= 0:
        parser.error('sample rate must be positive')
    nominal_duration = args.iq.stat().st_size / (4 if args.input_format == 's16' else 2) / args.sample_rate
    prefix = args.output.resolve(); prefix.parent.mkdir(parents=True, exist_ok=True)
    paths = {s: Path(str(prefix) + '.' + s) for s in ('ts', 'decoder.log', 'source.log', 'json')}
    if any(p.exists() for p in paths.values()):
        parser.error('output prefix already exists')
    env = os.environ.copy()
    env.pop('DECT2_NOPACE', None)  # never replace live drops/retry deadlines with backpressure
    env.pop('LINRAD_DECODER_MONITOR_PORT', None)
    env['DECT2_STAGE_PROF'] = '1' if args.profile else '0'
    with paths['ts'].open('xb') as ts, paths['decoder.log'].open('xb') as log, paths['source.log'].open('xb') as source_log:
        source = subprocess.Popen([sys.executable, str(ROOT/'dev/fixed_iq_source.py'), '--in', str(args.iq.resolve()),
            '--sample-rate', str(args.sample_rate), '--input-format', args.input_format], stdout=subprocess.PIPE, stderr=source_log)
        worker = None
        try:
            start = time.monotonic()
            worker = subprocess.Popen([str(args.decoder.resolve()), '--sample-rate', str(args.sample_rate),
                '--input-format', args.input_format], stdin=source.stdout, stdout=ts, stderr=log, env=env)
            source.stdout.close()
            # wait4 returns resource use for this worker only, not source/Python.
            deadline = start + nominal_duration + 20
            while True:
                pid, status, usage = os.wait4(worker.pid, os.WNOHANG)
                if pid:
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError('DVB-T2 worker exceeded recording duration plus 20 seconds')
                time.sleep(.05)
            worker.returncode = os.waitstatus_to_exitcode(status)
            elapsed = time.monotonic() - start
            source_status = source.wait(timeout=5)
        finally:
            for proc in (worker, source):
                if proc is not None and proc.poll() is None:
                    proc.kill(); proc.wait()
    decoder_log = paths['decoder.log'].read_text()
    source_log = paths['source.log'].read_text()
    final = re.search(r'DVB-T2 final: (.*)', decoder_log)
    if worker.returncode or source_status or not final:
        raise RuntimeError(f'decoder={worker.returncode} source={source_status}\n{decoder_log}\n{source_log}')
    fields = dict(re.findall(r'(\w+)=([^ ]+)', final[1]))
    good, bad = map(int, fields['BCH'].split('/'))
    clock = json.loads(re.search(r'\[fixed-iq\] (.*)', source_log)[1])
    digest = hashlib.sha256()
    with paths['ts'].open('rb') as f:
        for block in iter(lambda: f.read(1048576), b''): digest.update(block)
    result = dict(decoder=str(args.decoder.resolve()), iq=str(args.iq.resolve()),
        profile=args.profile, elapsed_s=elapsed, cpu_s=usage.ru_utime + usage.ru_stime,
        cpu_cores=(usage.ru_utime + usage.ru_stime)/clock['nominal_duration_s'], peak_rss_kib=usage.ru_maxrss,
        good_blocks=good, bad_blocks=bad, packets=int(fields['TS']), ts_sha256=digest.hexdigest(),
        input_dropped=int(fields['input_dropped']), fec_frames_dropped=int(fields['fec_frames_dropped']), source=clock,
        stages={name: dict(cpu_s=float(sec), calls=int(n)) for name, sec, n in
            re.findall(r'DVB-T2 profile: (.*?) cpu=([\d.]+) calls=(\d+)', decoder_log)})
    paths['json'].write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
