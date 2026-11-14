#!/usr/bin/env python3
"""Experimental Linux headless DVB-T repeater: one SDR, several UDP outputs.

Only the shared IQ fanout runs in Python. Mixing, anti-alias resampling, DVB-T
and UDP TS output run in independent native decoder workers. No shell/FFmpeg.
"""
import argparse
import configparser
import ipaddress
import math
import os
from pathlib import Path
import selectors
import shlex
import shutil
import signal
import subprocess
import sys
import time


class ConfigError(ValueError):
    pass


def load_config(path):
    path = Path(path).resolve()
    cfg = configparser.ConfigParser(interpolation=None)
    with path.open() as stream:
        cfg.read_file(stream)
    allowed = {'sdr', 'paths', 'multirx'}
    for name in cfg.sections():
        if name not in allowed and not name.startswith('channel:'):
            raise ConfigError(f'Unknown section [{name}]')
    keys = {
        'sdr': {'source', 'frequency', 'sample_rate', 'gain', 'args'},
        'paths': {'sdr', 'decoder'},
        'multirx': {'experimental', 'queue_ms'},
    }
    channel_keys = {'frequency', 'bandwidth', 'fec', 'gi', 'udp_host', 'udp_port', 'cpu_affinity'}
    for name in cfg.sections():
        unknown = set(cfg[name]) - (channel_keys if name.startswith('channel:') else keys[name])
        if unknown:
            raise ConfigError(f'Unknown keys in [{name}]: {sorted(unknown)}')
    if not cfg.getboolean('multirx', 'experimental', fallback=False):
        raise ConfigError('Set [multirx] experimental=true to enable this mode')
    source = cfg.get('sdr', 'source')
    if source not in ('rtlsdr', 'fcdpp'):
        raise ConfigError('This experiment supports source=rtlsdr or fcdpp (FUNcube Pro+)')
    rate = cfg.getint('sdr', 'sample_rate')
    center = cfg.getint('sdr', 'frequency')
    gain = cfg.getfloat('sdr', 'gain', fallback=30)
    if not math.isfinite(gain):
        raise ConfigError('gain must be finite')
    if source == 'rtlsdr':
        if not (225001 <= rate <= 300000 or 900001 <= rate <= 3200000):
            raise ConfigError('Unsupported RTL-SDR sample_rate')
        if not 24000000 <= center <= 1764000000 or not 0 <= gain <= 49.6:
            raise ConfigError('RTL-SDR frequency/gain out of range')
    else:
        if rate != 192000:
            raise ConfigError('FUNcube Pro+ requires sample_rate=192000')
        if not (150000 <= center <= 240000000 or 420000000 <= center <= 1900000000) or not 0 <= gain <= 59:
            raise ConfigError('FUNcube Pro+ frequency/gain out of range')
    queue_ms = cfg.getint('multirx', 'queue_ms', fallback=500)
    if not 100 <= queue_ms <= 2000:
        raise ConfigError('queue_ms must be 100..2000')

    def program(key, default):
        value = cfg.get('paths', key, fallback=default)
        # Bare executable names use PATH; relative paths use the INI directory.
        return str((path.parent / value).resolve()) if '/' in value else value

    sdr = [program('sdr', '../build/sdr_to_stdout'), '--source', source,
           '--sample-rate', str(rate), '--frequency', str(center), '--gain', str(gain),
           '--format', 's16']
    device_args = cfg.get('sdr', 'args', fallback='driver=rtlsdr' if source == 'rtlsdr' else '')
    if device_args:
        sdr += ['--args', device_args]
    channels = []
    destinations = set()
    for name in cfg.sections():
        if not name.startswith('channel:'):
            continue
        if not name.removeprefix('channel:').strip():
            raise ConfigError('Channels require a name after channel:')
        frequency = cfg.getint(name, 'frequency')
        bw = cfg.getint(name, 'bandwidth', fallback=40000)
        # Keep the initial experiment narrow; additional modes need validation.
        if bw != 40000:
            raise ConfigError(f'[{name}] experimental bandwidth must be 40000')
        offset = frequency - center
        if abs(offset) + bw / 2 > rate * .475:
            raise ConfigError(f'[{name}] channel falls outside shared SDR passband')
        fec = cfg.get(name, 'fec', fallback='1/2')
        gi = cfg.get(name, 'gi', fallback='auto')
        if fec not in ('auto', '1/2', '2/3', '3/4', '5/6', '7/8') or gi not in ('auto', '1/32', '1/16', '1/8'):
            raise ConfigError(f'[{name}] unsupported FEC or GI')
        host = str(ipaddress.IPv4Address(cfg.get(name, 'udp_host', fallback='127.0.0.1')))
        if host in ('0.0.0.0', '255.255.255.255'):
            raise ConfigError(f'[{name}] use a destination IPv4 address')
        port = cfg.getint(name, 'udp_port')
        if not 1 <= port <= 65535 or (host, port) in destinations:
            raise ConfigError(f'[{name}] UDP port invalid or destination duplicated')
        destinations.add((host, port))
        command = [program('decoder', '../build/linrad_dvb'), '--decoder-worker=dvbt',
                   '--stdin', '--input-format', 's16', '--sample-rate', str(rate),
                   '--frequency-offset', str(offset), '--sr', str(bw), '--fec', fec,
                   '--gi', gi, '--live', '--resample-to-dvbt-rate', '--channel-filter', '--afc',
                   '--live-symbols', '64', '--probe-symbols', '64',
                   '--udp-ts', f'{host}:{port}', '--loglevel', 'info']
        affinity = cfg.get(name, 'cpu_affinity', fallback='').strip()
        if affinity and affinity != 'auto':
            if not affinity.isascii() or not affinity.isdecimal():
                raise ConfigError(f'[{name}] cpu_affinity must be one nonnegative CPU ID, auto or empty')
            # Set affinity BEFORE exec so every decoder thread inherits it.
            # Avoid pinning the supervisor, SDR helper or a running GUI.
            command = ['taskset', '--cpu-list', str(int(affinity))] + command
        channels.append((name, command))
    if not 1 <= len(channels) <= 8:
        raise ConfigError('Configure 1..8 [channel:NAME] sections')
    return sdr, channels, max(65536, rate * 4 * queue_ms // 1000)


def run(sdr_command, channels, queue_bytes):
    if sys.platform != 'linux':
        raise ConfigError('The experimental supervisor currently requires Linux')
    # Check the target machine at runtime, before starting any process/hardware.
    # --print-pipeline remains usable when preparing a config for another host.
    for name, command in channels:
        if command[:2] == ['taskset', '--cpu-list']:
            if not shutil.which('taskset'):
                raise ConfigError('cpu_affinity requires taskset (util-linux)')
            cpu = int(command[2])
            allowed = os.sched_getaffinity(0)
            if cpu not in allowed:
                raise ConfigError(f'[{name}] CPU {cpu} is unavailable; allowed CPU IDs: {sorted(allowed)}')
    stopping = False

    def stop(_signum, _frame):
        nonlocal stopping
        stopping = True

    handlers = {sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    processes = []
    selector = selectors.DefaultSelector()
    log_buffers = {}
    writers = []
    started = time.monotonic()
    try:
        env = os.environ.copy()
        # Do not send unrelated decoder telemetry into a running GUI session.
        env.pop('LINRAD_DECODER_MONITOR_PORT', None)

        def spawn(name, command, **kwargs):
            proc = subprocess.Popen(command, stderr=subprocess.PIPE, env=env,
                                    start_new_session=True, bufsize=0, **kwargs)
            processes.append((name, proc))
            os.set_blocking(proc.stderr.fileno(), False)
            selector.register(proc.stderr, selectors.EVENT_READ, ('log', name))
            log_buffers[name] = bytearray()
            return proc

        # Start all consumers before opening the hardware, exactly once.
        for name, command in channels:
            proc = spawn(name, command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
            os.set_blocking(proc.stdin.fileno(), False)
            writers.append((name, proc.stdin, bytearray()))
        source = spawn('sdr', sdr_command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
        os.set_blocking(source.stdout.fileno(), False)
        selector.register(source.stdout, selectors.EVENT_READ, ('iq', None))
        print('[multirx] experimental: shared SDR; independent native DVB-T/UDP workers', file=sys.stderr)
        for name, command in channels:
            print(f'[{name}] {shlex.join(command)}', file=sys.stderr)
        while not stopping:
            for name, proc in processes:
                status = proc.poll()
                if status is not None:
                    raise RuntimeError(f'{name} exited ({status}); stopping shared receiver')
            for key, _events in selector.select(.2):
                kind, info = key.data
                if kind == 'log':
                    data = os.read(key.fd, 8192)
                    buf = log_buffers[info]
                    buf.extend(data)
                    while b'\n' in buf:
                        line, _, rest = buf.partition(b'\n')
                        buf[:] = rest
                        print(f'[{info}] {line.decode(errors="replace")}', file=sys.stderr)
                    if len(buf) > 16384 or (not data and buf):
                        print(f'[{info}] {buf.decode(errors="replace")}', file=sys.stderr)
                        buf.clear()
                    if not data:
                        selector.unregister(key.fileobj)
                elif kind == 'iq':
                    data = os.read(key.fd, 65536)
                    if not data:
                        raise RuntimeError('SDR IQ stream ended; stopping shared receiver')
                    for name, pipe, pending in writers:
                        if len(pending) + len(data) > queue_bytes:
                            raise RuntimeError(f'{name}: IQ fanout queue exceeded {queue_bytes} bytes; '
                                               'decoder too slow; stopping rather than silently dropping IQ')
                        empty = not pending
                        pending.extend(data)
                        if empty:
                            selector.register(pipe, selectors.EVENT_WRITE, ('write', (name, pending)))
                else:
                    name, pending = info
                    try:
                        sent = os.write(key.fd, pending)
                    except BlockingIOError:
                        continue
                    except BrokenPipeError as error:
                        raise RuntimeError(f'{name}: decoder IQ pipe closed; stopping shared receiver') from error
                    del pending[:sent]
                    if not pending:
                        selector.unregister(key.fileobj)
        return 0
    finally:
        # Signals reach each complete child process group. Never leave the SDR
        # locked after a decoder failure, partial startup or service shutdown.
        for _name, proc in processes:
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + 3
        for _name, proc in processes:
            try:
                proc.wait(timeout=max(.01, deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                if stream:
                    stream.close()
        selector.close()
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
        print(f'[multirx] stopped after {time.monotonic()-started:.1f}s', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--print-pipeline', action='store_true', help='Validate and show commands without opening SDR/network')
    args = parser.parse_args()
    try:
        sdr, channels, queue_bytes = load_config(args.config)
        if args.print_pipeline:
            print('shared SDR:', shlex.join(sdr))
            for name, command in channels:
                print(f'{name}:', shlex.join(command))
            print(f'Bounded IQ fanout: {queue_bytes} bytes per channel')
            return 0
        return run(sdr, channels, queue_bytes)
    except (OSError, ValueError, configparser.Error, RuntimeError) as error:
        print(f'[multirx] ERROR: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
