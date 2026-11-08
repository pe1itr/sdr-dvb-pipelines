"""Exercise the real receiver's HTTP diagnostics without SDR hardware."""
import configparser
import http.client
import json
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
        self.assertEqual(state['standard'], 'DAB')
        self.assertEqual(state['state'], 'running')
        self.assertTrue(state['telemetry_fresh'])
        self.assertGreaterEqual(state['telemetry_age_ms'], 0)
        self.assertIn('Pipeline running', state['recent_log'])
        self.assertIn('error', state['dab'])
        self.assertEqual(self.get('/missing')[0], 404)
        self.assertEqual(self.get(method='POST')[0], 405)
        # Internal DAB status now travels through a pipe, never through state.json.
        children = subprocess.check_output(['ps', '--ppid', str(self.process.pid), '-o', 'args='], text=True)
        import shlex
        worker = next(shlex.split(line) for line in children.splitlines() if '--state-dir' in line)
        directory = Path(worker[worker.index('--state-dir') + 1])
        self.assertIn('--status-stdout', worker)
        self.assertFalse((directory / 'state.json').exists())

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


if __name__ == '__main__':
    unittest.main()
