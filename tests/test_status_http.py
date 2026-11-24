"""Exercise the real receiver's HTTP diagnostics without SDR hardware."""
import configparser
import http.client
import json
import os
import shutil
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT / 'linrad-dvb.example.ini')
        for key, value in cfg['paths'].items():
            if value.startswith('build/'):
                cfg['paths'][key] = str(ROOT / value)
        cfg['linrad']['sample_rate'] = '2048000'
        cfg['linrad']['port'] = str(free_port())
        cfg['dab'] = {'audio_device': 'null'}
        self.port = free_port()
        cfg['status'] = {'enabled': 'true', 'bind': '127.0.0.1', 'port': str(self.port)}
        self.config = Path(self.tmp.name) / 'config.ini'
        with self.config.open('w') as file:
            cfg.write(file)
        self.log = tempfile.TemporaryFile()
        self.addCleanup(self.log.close)
        self.process = subprocess.Popen([ROOT / 'build/linrad_dvb', '-c', self.config,
            '--run', '--source', 'linrad', '--standard', 'DAB'], stdout=self.log, stderr=self.log)
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0)
                self.fail(self.log.read().decode(errors='replace'))
            try:
                if self.get()[1].get('dab'):
                    return
            except OSError:
                pass
            time.sleep(.05)
        self.fail('Receiver did not publish DAB status')

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def get(self, path='/status', method='GET'):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        try:
            conn.request(method, path)
            response = conn.getresponse()
            self.assertEqual(response.getheader('Cache-Control'), 'no-store')
            body = response.read()
            self.assertEqual(len(body), int(response.getheader('Content-Length')))
            return response.status, json.loads(body)
        finally:
            conn.close()

    def raw(self, request):
        with socket.create_connection(('127.0.0.1', self.port), timeout=5) as sock:
            sock.sendall(request)
            response = b''
            while chunk := sock.recv(8192):
                response += chunk
            return response

    def test_status_and_routes(self):
        code, state = self.get()
        self.assertEqual(code, 200)
        self.assertEqual(state['schema_version'], 1)
        build = state['build']
        version = subprocess.check_output([ROOT / 'build/linrad_dvb', '--version'], text=True)
        self.assertIn('Linrad DVB ' + build['version'], version)
        self.assertIn('Build ID: ' + build['id'], version)
        self.assertIn('Git commit: ' + build['git_commit'], version)
        self.assertIn('Source state: ' + build['source_state'], version)
        self.assertIn('Compiled: ' + build['compiled'], version)
        self.assertEqual(state['standard'], 'DAB')
        self.assertEqual(state['state'], 'running')
        self.assertTrue(state['telemetry_fresh'])
        self.assertGreaterEqual(state['telemetry_age_ms'], 0)
        self.assertIn('Pipeline running', state['recent_log'])
        self.assertIn('error', state['dab'])
        code, constellation = self.get('/constellation')
        self.assertEqual(code, 200)
        self.assertFalse(constellation['supported'])
        self.assertFalse(constellation['fresh'])
        self.assertIsNone(constellation['received_unix_ms'])
        self.assertEqual(constellation['points'], [])
        self.assertEqual(self.get('/constellation', 'POST')[0], 405)
        self.assertFalse(state['control_enabled'])
        self.assertEqual(self.get('/control/stop', 'PUT')[0], 403)
        self.assertEqual(self.get('/missing')[0], 404)
        self.assertEqual(self.get(method='POST')[0], 405)
        # Internal DAB status now travels through a pipe, never through state.json.
        children = subprocess.check_output(['ps', '--ppid', str(self.process.pid), '-o', 'args='], text=True)
        import shlex
        worker = next(shlex.split(line) for line in children.splitlines() if '--state-dir' in line)
        directory = Path(worker[worker.index('--state-dir') + 1])
        self.assertIn('--status-stdout', worker)
        self.assertFalse((directory / 'state.json').exists())

    def test_offline_api_reference(self):
        code, spec = self.get('/openapi.json')
        self.assertEqual(code, 200)
        self.assertEqual(spec, json.loads((ROOT / 'docs/api/openapi.json').read_text()))
        self.assertEqual(spec['openapi'], '3.0.3')
        for path in ('/', '/docs'):
            raw = self.raw(f'GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
            headers, body = raw.split(b'\r\n\r\n', 1)
            self.assertIn(b'200 OK', headers)
            self.assertIn(b'Content-Type: text/html; charset=utf-8', headers)
            self.assertIn(f'Content-Length: {len(body)}'.encode(), headers)
            self.assertIn(b'href="/openapi.json"', body)
            for route in spec['paths']:
                self.assertIn(f'<code>{route}</code>'.encode(), body)
            self.assertIn(b'PUT /control/fec/1/2', body)
        for path in ('/', '/docs', '/openapi.json'):
            self.assertEqual(self.get(path, 'PUT')[0], 405)

    def test_malformed_and_large_requests(self):
        self.assertIn(b'400 Bad Request', self.raw(b'GET /status HTTP/1.1\x00\r\n\r\n'))
        self.assertIn(b'400 Bad Request', self.raw(b'GET /status WHAT\r\n\r\n'))
        self.assertIn(b'431 Request Header Fields Too Large', self.raw(b'X' * 8192))
        self.assertEqual(self.get()[0], 200)

    def test_slow_client_does_not_block_updates(self):
        with socket.create_connection(('127.0.0.1', self.port), timeout=6) as slow:
            slow.sendall(b'GET /status HTTP/1.1\r\nHost:')
            first = self.get()[1]['updated_unix_ms']
            time.sleep(.25)
            self.assertGreater(self.get()[1]['updated_unix_ms'], first)
            self.assertEqual(slow.recv(1), b'')  # Total request deadline.
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as partial:
            partial.sendall(b'GET /sta')
            time.sleep(.05)
            partial.sendall(b'tus HTTP/1.1\r\n\r\n')
            self.assertIn(b'200 OK', partial.recv(8192))

    def test_bind_conflict_is_reported(self):
        result = subprocess.run([ROOT / 'build/linrad_dvb', '-c', self.config, '--run',
            '--source', 'linrad', '--standard', 'DAB'], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'Cannot start status endpoint', result.stderr)


@unittest.skipUnless(shutil.which('broadwayd'), 'requires GTK Broadway display')
class ReceiverControlTests(unittest.TestCase):
    get = StatusTests.get
    stop = StatusTests.stop

    def test_desktop_control(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryFile() as log:
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(ROOT / 'linrad-dvb.example.ini')
            for key, value in cfg['paths'].items():
                if value.startswith('build/'):
                    cfg['paths'][key] = str(ROOT / value)
            self.port = free_port()
            cfg['status'].update(enabled='true', control='true', port=str(self.port))
            cfg['session']['renderer'] = 'udp'
            cfg['linrad']['port'] = str(free_port())
            config = Path(tmp) / 'test.ini'
            with config.open('w') as out:
                cfg.write(out)
            Path(str(config) + '.presets.ini').write_text(
                '[preset2]\nlabel=Test RTL\nsettings=[session]\\nsource=rtlsdr\\n'
                'standard=DVB-T\\nsr=250000\\nfec=1/2\\nrenderer=udp\\n\n')
            display = f':{free_port()}'
            env = dict(os.environ, GDK_BACKEND='broadway', BROADWAY_DISPLAY=display,
                       XDG_RUNTIME_DIR=tmp, NO_AT_BRIDGE='1')
            daemon = subprocess.Popen(['broadwayd', '--port', str(free_port()), display],
                                      env=env, stdout=log, stderr=log)
            try:
                time.sleep(.2)
                self.process = subprocess.Popen([ROOT / 'build/linrad_dvb', '-c', config],
                                                env=env, stdout=log, stderr=log)
                try:
                    self.exercise_controls(log)
                finally:
                    self.stop()
            finally:
                daemon.terminate()
                daemon.wait(timeout=5)

    def exercise_controls(self, log):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                log.seek(0)
                self.fail(log.read().decode(errors='replace'))
            try:
                state = self.get()[1]
                break
            except OSError:
                time.sleep(.05)
        else:
            self.fail('Control endpoint did not start')

        def put(command, expected=200):
            code, body = self.get('/control/' + command, 'PUT')
            self.assertEqual(code, expected, body)

        self.assertTrue(state['control_enabled'])
        self.assertEqual(state['presets'][1], {'slot': 2, 'label': 'Test RTL', 'saved': True})
        self.assertEqual(self.get('/control/start')[0], 405)
        put('mode/DVB-S2')
        put('fec/3/5')
        put('mode/DVB-S')
        put('fec/3/5', 400)
        put('mode/DVB-T')
        put('rate/250000')
        put('rate/123456', 400)
        put('fec/7/8')
        state = self.get()[1]
        self.assertEqual((state['standard'], state['rate'], state['configured_fec']),
                         ('DVB-T', 250000, '7/8'))
        put('preset/1', 404)
        put('start')
        self.assertTrue(self.get()[1]['running'])
        put('rate/40000', 409)
        put('stop', 202)
        self.wait_stopped()
        put('start')
        put('preset/2', 202)
        self.wait_stopped()
        self.assertEqual(self.get()[1]['active_preset'], 2)
        put('frequency/144600000')
        put('frequency/12x', 400)
        self.assertEqual(self.get()[1]['configured_frequency_hz'], 144600000)
        self.assertEqual(self.get()[1]['active_preset'], 0)

    def wait_stopped(self):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if self.get()[1]['state'] == 'stopped':
                return
            time.sleep(.05)
        self.fail('Receiver did not stop')


if __name__ == '__main__':
    unittest.main()
