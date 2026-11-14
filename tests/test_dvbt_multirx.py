#!/usr/bin/env python3
"""Config, byte-exact fanout, overload and shutdown tests; no live SDR used."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('multirx', ROOT/'tools/dvbt_multirx.py')
MULTIRX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MULTIRX)


class MultiRxTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.example = (ROOT/'examples/dvbt-multirx.ini').read_text()

    def config(self, text):
        path = self.folder/'rx.ini'
        path.write_text(text)
        return MULTIRX.load_config(path)

    def test_example_and_funcube(self):
        sdr, channels, limit = self.config(self.example)
        self.assertIn('144550000', sdr)
        self.assertEqual(len(channels), 2)
        self.assertEqual(limit, 4800000)
        for (name, command), offset, port in zip(channels, ('-25000', '25000'), ('10000', '10001')):
            self.assertEqual(command[command.index('--frequency-offset')+1], offset)
            self.assertIn('127.0.0.1:'+port, command)
            self.assertIn('--channel-filter', command)
            self.assertEqual(command[:2], ['taskset', '--cpu-list'])
            self.assertEqual(command[2], '2' if offset == '-25000' else '3')
        text = self.example.replace('source=rtlsdr', 'source=fcdpp').replace('sample_rate=2400000', 'sample_rate=192000').replace('args=driver=rtlsdr', 'args=')
        sdr, _, limit = self.config(text)
        self.assertIn('fcdpp', sdr)
        self.assertEqual(limit, 384000)

    def test_affinity_can_be_disabled(self):
        for value in ('auto', ''):
            text = self.example.replace('cpu_affinity=2', 'cpu_affinity='+value).replace('cpu_affinity=3', 'cpu_affinity='+value)
            _, channels, _ = self.config(text)
            self.assertTrue(all(command[0] != 'taskset' for _, command in channels))
        text = self.example.replace('cpu_affinity=2', '').replace('cpu_affinity=3', '')
        _, channels, _ = self.config(text)
        self.assertTrue(all(command[0] != 'taskset' for _, command in channels))

    def test_affinity_rejected_before_opening_hardware(self):
        unavailable = max(os.sched_getaffinity(0))+1
        commands = [('bad', ['taskset', '--cpu-list', str(unavailable), '/unused-decoder'])]
        with mock.patch.object(MULTIRX.subprocess, 'Popen') as spawn:
            with self.assertRaisesRegex(MULTIRX.ConfigError, 'unavailable'):
                MULTIRX.run(['/unused-sdr'], commands, 65536)
            spawn.assert_not_called()
            with mock.patch.object(MULTIRX.shutil, 'which', return_value=None):
                with self.assertRaisesRegex(MULTIRX.ConfigError, 'requires taskset'):
                    MULTIRX.run(['/unused-sdr'], commands, 65536)
            spawn.assert_not_called()

    def test_reject_invalid_config(self):
        for old, new in [('experimental=true', 'experimental=false'),
                         ('frequency=144575000', 'frequency=150000000'),
                         ('udp_port=10001', 'udp_port=10000'),
                         ('udp_port=10001', 'udp_port=65536'),
                         ('bandwidth=40000', 'bandwidth=250000'),
                         ('gi=auto', 'gi=bad'), ('gain=30', 'gain=nan'),
                         ('cpu_affinity=2', 'cpu_affinity=-1'),
                         ('cpu_affinity=2', 'cpu_affinity=2,3'),
                         ('cpu_affinity=2', 'cpu_affinity=nan'),
                         ('source=rtlsdr', 'source=unknown'),
                         ('udp_host=127.0.0.1', 'udp_host=0.0.0.0'),
                         ('queue_ms=500', 'queue_ms=100000'),
                         ('frequency=144550000', 'frequncy=144550000')]:
            with self.subTest(new=new), self.assertRaises((ValueError, MULTIRX.configparser.Error)):
                self.config(self.example.replace(old, new))

    def test_decoder_rejects_bad_offset_and_filter_mode(self):
        command = [str(ROOT/'build/linrad_dvb'), '--decoder-worker=dvbt', '--stdin',
                   '--sample-rate', '192000', '--sr', '40000']
        for options in (['--frequency-offset', 'nan'], ['--frequency-offset', '80000'],
                        ['--frequency-offset', '25k'], ['--channel-filter']):
            result = subprocess.run(command+options, input=b'', capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr)

    def launch_fake(self, mode, pinned=False):
        fake = self.folder/'fake.py'
        fake.write_text('''import json, os, signal, sys, threading, time
from pathlib import Path
mode, folder = sys.argv[1:3]
folder = Path(folder)
(folder/(mode+'.pid')).write_text(str(os.getpid()))
if mode == 'sdr':
    block = bytes(range(256))*256
    for n in range(16 if sys.argv[3] in ('normal', 'failed') else 200):
        os.write(1, block)
        time.sleep(.003)
    time.sleep(30)
elif mode == 'stalled':
    time.sleep(30)
elif mode == 'failed':
    time.sleep(.3)
    sys.exit(7)
else:
    affinity = {'main': sorted(os.sched_getaffinity(0))}
    def check_thread():
        affinity['thread'] = sorted(os.sched_getaffinity(0))
    thread = threading.Thread(target=check_thread)
    thread.start()
    thread.join()
    (folder/(mode+'.affinity.json')).write_text(json.dumps(affinity))
    target = (folder/(mode+'.iq')).open('wb', buffering=0)
    while True:
        data = os.read(0, 8192)
        if not data: break
        target.write(data)
''')
        driver = self.folder/'driver.py'
        driver.write_text(f'''import sys
sys.path.insert(0, {str(ROOT/'tools')!r})
import dvbt_multirx as m
cmd = [sys.executable, {str(fake)!r}]
channels = [('a',cmd+['a', {str(self.folder)!r}]),
            ('b',cmd+[{('b' if mode=='normal' else mode)!r}, {str(self.folder)!r}])]
if {pinned!r}:
    channels = [(name, ['taskset', '--cpu-list', str(cpu)]+command)
                for (name, command), cpu in zip(channels, sorted(m.os.sched_getaffinity(0))[:2])]
try:
    sys.exit(m.run(cmd+['sdr', {str(self.folder)!r}, {mode!r}], channels,
                  2000000 if {mode!r} in ('normal', 'failed') else 65536))
except Exception as e:
    print(e, file=sys.stderr)
    sys.exit(1)
''')
        return subprocess.Popen([sys.executable, str(driver)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def assert_children_stopped(self):
        for path in self.folder.glob('*.pid'):
            with self.assertRaises(ProcessLookupError, msg=str(path)):
                os.kill(int(path.read_text()), 0)

    def test_partial_startup_cleanup(self):
        driver = self.folder/'partial.py'
        pid_file = self.folder/'first.pid'
        driver.write_text(f"""import sys
from pathlib import Path
sys.path.insert(0, {str(ROOT/'tools')!r})
import dvbt_multirx as m
original_spawn = m.subprocess.Popen
def recorded_spawn(*args, **kwargs):
    proc = original_spawn(*args, **kwargs)
    Path({str(pid_file)!r}).write_text(str(proc.pid))
    return proc
m.subprocess.Popen = recorded_spawn
m.run(['/unused-source'], [('first', [sys.executable, '-c', 'import time;time.sleep(30)']),
    ('missing', ['/nonexistent/multirx-worker'])], 65536)
""")
        result = subprocess.run([sys.executable, str(driver)], capture_output=True, timeout=6)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'No such file', result.stderr)
        self.assertTrue(pid_file.exists())
        self.assert_children_stopped()

    def test_identical_fanout_and_signal_cleanup(self):
        proc = self.launch_fake('normal')
        expected = bytes(range(256))*256*16
        try:
            deadline = time.monotonic()+8
            while time.monotonic()<deadline:
                files = [self.folder/(name+'.iq') for name in ('a', 'b')]
                if all(p.exists() and p.stat().st_size == len(expected) for p in files):
                    break
                if proc.poll() is not None:
                    self.fail(proc.communicate()[1].decode())
                time.sleep(.02)
            for path in files:
                self.assertEqual(path.read_bytes(), expected)
            proc.send_signal(signal.SIGTERM)
            _, error = proc.communicate(timeout=6)
            self.assertEqual(proc.returncode, 0, error)
            self.assert_children_stopped()
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=6)

    def test_affinity_is_inherited_by_decoder_threads(self):
        cpus = sorted(os.sched_getaffinity(0))[:2]
        if len(cpus) < 2:
            self.skipTest('Two allowed CPUs needed for two distinct decoder masks')
        proc = self.launch_fake('normal', pinned=True)
        try:
            deadline = time.monotonic()+8
            files = [self.folder/(name+'.affinity.json') for name in ('a', 'b')]
            while time.monotonic() < deadline:
                if all(path.exists() and path.stat().st_size for path in files):
                    break
                if proc.poll() is not None:
                    self.fail(proc.communicate()[1].decode())
                time.sleep(.02)
            for path, cpu in zip(files, cpus):
                self.assertEqual(json.loads(path.read_text()), {'main': [cpu], 'thread': [cpu]})
            # The shared supervisor and SDR retain the original allowed mask.
            self.assertEqual(os.sched_getaffinity(proc.pid), os.sched_getaffinity(0))
            sdr_pid = int((self.folder/'sdr.pid').read_text())
            self.assertEqual(os.sched_getaffinity(sdr_pid), os.sched_getaffinity(0))
            proc.send_signal(signal.SIGTERM)
            _, error = proc.communicate(timeout=6)
            self.assertEqual(proc.returncode, 0, error)
            self.assert_children_stopped()
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=6)

    def test_slow_or_dead_decoder_stops_and_reaps_all(self):
        for mode in ('stalled', 'failed'):
            # Failed decoder needs a larger queue to observe its exit code.
            proc = self.launch_fake(mode)
            try:
                _, error = proc.communicate(timeout=8)
                self.assertNotEqual(proc.returncode, 0)
                if mode == 'stalled':
                    self.assertIn(b'queue exceeded', error)
                else:
                    self.assertTrue(b'exited (7)' in error or b'b: decoder IQ pipe closed' in error, error)
                self.assert_children_stopped()
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.communicate(timeout=6)


if __name__ == '__main__':
    unittest.main(verbosity=2)
