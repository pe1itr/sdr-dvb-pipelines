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
        cfg['usb']['tci_port'] = '41023'
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

    def test_dsmcc_messages(self):
        status = self.get()[1]['dsmcc']
        path = status['href']
        cursor = status['latest_cursor']
        expected = dict(messages=[], latest_cursor=cursor, gap=False)
        self.assertEqual(self.get(path), (200, expected))
        self.assertEqual(self.get(f'{path}?after={cursor}'), (200, expected))
        self.assertTrue(self.get(path + '?after=1')[1]['gap'])
        for query in ('after=', 'after=-1', 'after=1.2', 'after=1&after=2', 'since=1'):
            self.assertEqual(self.get(path + '?' + query)[0], 400)
        self.assertEqual(self.get(path, 'POST')[0], 405)

    def test_offline_api_reference(self):
        code, spec = self.get('/openapi.json')
        self.assertEqual(code, 200)
        self.assertEqual(spec, json.loads((ROOT / 'docs/api/openapi.json').read_text()))
        self.assertEqual(spec['openapi'], '3.0.3')
        self.assertEqual(self.get('/')[0], 404)
        self.assertNotIn('/', spec['paths'])
        raw = self.raw(b'GET /docs HTTP/1.1\r\nHost: localhost\r\n\r\n')
        headers, body = raw.split(b'\r\n\r\n', 1)
        self.assertIn(b'200 OK', headers)
        self.assertIn(b'Content-Type: text/html; charset=utf-8', headers)
        self.assertIn(f'Content-Length: {len(body)}'.encode(), headers)
        self.assertIn(b'href="/openapi.json"', body)
        self.assertIn(b'ws://127.0.0.1:41023', body)
        self.assertNotIn(b'{{tci_port}}', body)
        for route in spec['paths']:
            self.assertIn(f'<code>{route}</code>'.encode(), body)
        self.assertIn(b'PUT /control/fec/1/2', body)
        for path in ('/docs', '/openapi.json'):
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
            cfg['session']['renderer'] = 'embedded'
            cfg['linrad']['port'] = str(free_port())
            cfg['linrad']['sample_rate'] = '12000000'
            config = Path(tmp) / 'test.ini'
            with config.open('w') as out:
                cfg.write(out)
            Path(str(config) + '.presets.ini').write_text(
                '[hdmi]\nmonitor=1\n[preset2]\nlabel=Test RTL\nsettings=[session]\\nsource=rtlsdr\\n'
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

        self.assertEqual((state['hdmi_monitor'], state['hdmi_state']), (1, 'No Signal'))
        put('hdmi/off')
        self.assertEqual(self.get()[1]['hdmi_state'], 'Off')
        self.assertFalse(state['network_info_visible'])
        for action in ('on', 'on', 'off', 'off'):
            put('network-info/' + action)
            self.assertEqual(self.get()[1]['network_info_visible'], action == 'on')
        put('network-info/invalid', 400)
        put('osd/left', 409)
        put('osd/x', 400)
        put('hdmi/9999', 400)
        put('hdmi/1')
        self.assertEqual(self.get()[1]['hdmi_state'], 'No Signal')
        for side in ('right', 'left', 'off'):
            put('osd/' + side)
            self.assertEqual(self.get()[1]['hdmi_osd'], side)
        put('hdmi/off')
        self.assertTrue(state['control_enabled'])
        self.assertEqual(state['presets'][1], {'slot': 2, 'label': 'Test RTL', 'saved': True})
        self.assertEqual(self.get('/control/start')[0], 405)
        put('fmatv/framestack/on', 409)
        put('mode/fm-atv')
        self.assertFalse(self.get()[1]['fmatv_framestack_enabled'])
        put('fmatv/framestack/invalid', 400)
        for action in ('on', 'on', 'off', 'off'):
            put('fmatv/framestack/' + action)
            self.assertEqual(self.get()[1]['fmatv_framestack_enabled'], action == 'on')
        for mode, bandwidth in [('DAB', 1536000), ('DVB-T2', 8000000)]:
            put('mode/' + mode.lower())
            state = self.get()[1]
            self.assertEqual((state['standard'], state['rate'], state['configured_fec']),
                             (mode, bandwidth, 'auto'))
        put('gpu-ldpc/on')
        self.assertTrue(self.get()[1]['configured_gpu_ldpc'])
        put('mode/DvB-s2')
        self.assertFalse(self.get()[1]['configured_gpu_ldpc'])
        put('gpu-ldpc/ON')
        put('gpu-ldpc/invalid', 400)
        self.assertTrue(self.get()[1]['configured_gpu_ldpc'])
        put('gpu-ldpc/off')
        self.assertFalse(self.get()[1]['configured_gpu_ldpc'])
        put('mode/dvb-t2')
        self.assertTrue(self.get()[1]['configured_gpu_ldpc'])
        put('gpu-ldpc/off')
        self.assertFalse(self.get()[1]['configured_gpu_ldpc'])
        put('mode/dvb-s2')
        put('fec/1/2')
        put('constellation/8psk')
        state = self.get()[1]
        self.assertEqual((state['configured_constellation'], state['configured_fec']), ('8PSK', '2/3'))
        put('fec/1/2', 400)
        put('fec/3/5')
        put('constellation/QpSk')
        put('constellation/16apsk', 400)
        state = self.get()[1]
        self.assertEqual((state['configured_constellation'], state['configured_fec']), ('QPSK', '3/5'))
        put('mode/dvb-s')
        put('constellation/8psk', 409)
        put('gpu-ldpc/on', 409)
        put('fec/3/5', 400)
        put('mode/dvb-t')
        put('mode/dvb-x', 400)
        put('rate/250000')
        put('rate/123456', 400)
        put('fec/7/8')
        state = self.get()[1]
        self.assertEqual((state['standard'], state['rate'], state['configured_fec']),
                         ('DVB-T', 250000, '7/8'))
        put('teletext/off')
        self.assertFalse(self.get()[1]['teletext_visible'])
        put('tsscatterfix/invalid', 400)
        put('eit/on')
        self.assertTrue(self.get()[1]['eit_overlay_enabled'])
        put('eit/off')
        put('eit/invalid', 400)
        put('teletext/on', 409)
        put('program/0', 400)
        put('program/65536', 400)
        put('program/101', 409)
        self.assertEqual(self.get()[1]['programs'], [])
        self.assertIsNone(self.get()[1]['selected_program'])
        put('teletext/101', 409)
        put('preset/1', 404)
        put('start')
        self.assertTrue(self.get()[1]['running'])
        for action in ('on', 'on', 'off', 'off'):
            put('tsscatterfix/' + action)
            self.assertEqual(self.get()[1]['tsscatterfix_enabled'], action == 'on')
        put('tsscatterfix/toggle', 400)
        self.assertFalse(self.get()[1]['tsscatterfix_enabled'])
        for action, enabled in [('on', True), ('on', True), ('toggle', False), ('toggle', True), ('off', False)]:
            put('eit/' + action)
            self.assertEqual(self.get()[1]['eit_overlay_enabled'], enabled)
        for page in (100, 899, 101):
            put(f'teletext/{page}')
            state = self.get()[1]
            self.assertEqual(state['teletext_page'], page)
            self.assertFalse(state['teletext_page_received'])
        self.assertTrue(state['teletext_visible'])
        for command, visible in [('off', False), ('off', False), ('on', True), ('on', True)]:
            put('teletext/' + command)
            state = self.get()[1]
            self.assertEqual(state['teletext_visible'], visible)
            self.assertEqual(state['teletext_page'], 101)
            self.assertTrue(state['running'])
        self.assertEqual(self.get()[1]['teletext_color_links'], {})
        put('teletext/color/red', 409)
        put('teletext/color/purple', 400)
        for invalid in ('99', '900', '10A', '0101'):
            put('teletext/' + invalid, 400)
        self.assertEqual(self.get()[1]['teletext_page'], 101)
        put('hdmi/1')
        put('teletext/123')
        state = self.get()[1]
        self.assertTrue(state['hdmi_teletext_visible'])
        self.assertEqual(state['teletext_page'], 123)
        put('teletext/off')
        self.assertFalse(self.get()[1]['teletext_visible'])
        put('teletext/on')
        put('hdmi/off')
        self.assertFalse(self.get()[1]['hdmi_teletext_visible'])
        put('gpu-ldpc/on', 409)
        put('rate/40000', 409)
        put('constellation/qpsk', 409)
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

        put('frequency/5A', 400)
        put('mode/DAB')
        put('frequency/175000000')
        self.assertEqual(self.get()[1]['configured_frequency_hz'], 175000000)
        put('frequency/5A')
        self.assertEqual(self.get()[1]['configured_frequency_hz'], 174928000)
        put('frequency/13f')
        put('frequency/5E', 400)
        self.assertEqual(self.get()[1]['configured_frequency_hz'], 239200000)

    def wait_stopped(self):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if self.get()[1]['state'] == 'stopped':
                return
            time.sleep(.05)
        self.fail('Receiver did not stop')


if __name__ == '__main__':
    unittest.main()
