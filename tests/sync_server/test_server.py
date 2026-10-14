"""Real socket integration tests; no HTTP or storage mocks."""
import concurrent.futures
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]

class ServerTests(unittest.TestCase):
    def setUp(self):
        scratch = Path.home() / 'AppData' / 'Local' / 'hermes' / 'cache' / 'scratch' if os.name == 'nt' else Path(os.environ.get('TMPDIR', tempfile.gettempdir()))
        scratch.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=scratch)
        self.data = Path(self.tmp.name) / 'data'
        self.proc = None
        self.start()

    def tearDown(self):
        self.stop()
        self.tmp.cleanup()

    def cli(self, *args):
        try:
            return subprocess.run([sys.executable, '-m', 'sync_server', '--data', str(self.data), *args], cwd=ROOT, capture_output=True, text=True, timeout=3)
        except subprocess.TimeoutExpired:
            self.fail('CLI did not terminate: ' + args[0])

    def start(self, *args):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.proc = subprocess.Popen([sys.executable, '-m', 'sync_server', '--data', str(self.data), 'serve', '--port', str(self.port), *args], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(100):
            if self.proc.poll() is not None:
                break
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        out, err = self.proc.communicate(timeout=3)
        self.fail('server did not start: ' + err.decode())

    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.terminate()
            self.proc.communicate(timeout=5)
            self.proc = None

    def request(self, method, path, body=None, token=None, headers=None, tls=False):
        h = dict(headers or {})
        if token:
            h['Authorization'] = 'Bearer ' + token
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            h['Content-Type'] = 'application/json'
        conn = (http.client.HTTPSConnection('127.0.0.1', self.port, timeout=3, context=self.tls_context) if tls else http.client.HTTPConnection('127.0.0.1', self.port, timeout=3))
        try:
            conn.request(method, path, body=body, headers=h)
            r = conn.getresponse()
            data = r.read()
            result = json.loads(data) if data and r.getheader('Content-Type', '').startswith('application/json') else data
            return r.status, dict(r.getheaders()), result
        finally:
            conn.close()

    def pair(self, name='laptop'):
        result = self.cli('pair-code')
        self.assertEqual(result.returncode, 0, result.stderr)
        code = result.stdout.strip()
        status, _, value = self.request('POST', '/v2/pair', {'code': code, 'name': name})
        self.assertEqual(status, 200, value)
        return code, value

    def test_health_real_socket(self):
        self.assertEqual(self.request('GET', '/healthz')[2], {'ok': True, 'protocol': 2})

    def test_quotas_oversized_bodies_and_strict_payload(self):
        self.stop()
        self.start('--quota-bytes', '20', '--max-blob-bytes', '15', '--max-objects', '1')
        _, device = self.pair()
        token = device['token']
        self.assertEqual(self.upload(b'x'*16, token)[0], 413)
        self.assertEqual(self.upload(b'abcdefghij', token)[0], 200)
        self.assertEqual(self.upload(b'klmnopqrst', token, total=15, hash_value='f'*64)[0], 507)
        self.assertEqual(self.upload(b'x'*1048577, token)[0], 413)
        self.assertEqual(self.request('POST', '/v2/pair', b'x'*65537)[0], 413)
        value = {'base_revision': 0, 'blob': '', 'size': 0, 'deleted': True, 'operation_id': 'a'*32}
        self.assertEqual(self.request('POST', '/v2/objects/' + 'a'*32, dict(value, base_revision=True), token)[0], 400)
        self.assertEqual(self.request('POST', '/v2/objects/' + 'a'*32, dict(value, password='secret'), token)[0], 400)
        self.assertEqual(self.request('POST', '/v2/objects/' + 'a'*32, value, token)[0], 200)
        self.assertEqual(self.request('POST', '/v2/objects/' + 'b'*32, dict(value, operation_id='b'*32), token)[0], 507)

    def test_pair_expiry_and_failed_attempt_rate_limit(self):
        code = self.cli('pair-code').stdout.strip()
        with sqlite3.connect(self.data / 'sync.sqlite3') as db:
            db.execute('UPDATE codes SET expires=0')
        db.close()
        self.assertEqual(self.request('POST', '/v2/pair', {'code': code, 'name': 'expired'})[0], 401)
        statuses = [self.request('POST', '/v2/pair', {'code': '0'*64, 'name': 'bad'})[0] for _ in range(15)]
        self.assertIn(429, statuses)

    def test_secure_bind_policy_and_verified_tls(self):
        self.stop()
        result = self.cli('serve', '--host', '0.0.0.0', '--port', '0')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('TLS', result.stderr)
        result = self.cli('serve', '--host', '0.0.0.0', '--port', '0', '--proxy-mode')
        self.assertNotEqual(result.returncode, 0)
        cert, key = Path(self.tmp.name) / 'cert.pem', Path(self.tmp.name) / 'key.pem'
        result = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(cert), '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=IP:127.0.0.1,DNS:localhost'], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.tls_context = ssl.create_default_context(cafile=str(cert))
        self.start('--cert', str(cert), '--key', str(key))
        self.assertEqual(self.request('GET', '/healthz', tls=True)[2]['protocol'], 2)
        code = self.cli('pair-code').stdout.strip()
        status, _, value = self.request('POST', '/v2/pair', {'code': code, 'name': 'TLS device'}, tls=True)
        self.assertEqual(status, 200)
        self.assertEqual(self.request('GET', '/v2/objects', token=value['token'], tls=True)[2], {'objects': []})
        content = b'MVPK HTTPS payload'
        h = hashlib.sha256(content).hexdigest()
        status, _, result = self.request('PUT', '/v2/blobs/' + h + '?offset=0', content, value['token'], {'X-Blob-Size': str(len(content))}, tls=True)
        self.assertEqual((status, result['complete']), (200, True))
        self.assertEqual(self.request('GET', '/v2/blobs/' + h, token=value['token'], tls=True)[2], content)

    def test_connection_slots_and_absolute_request_timeout(self):
        self.stop()
        self.start('--workers', '1', '--request-timeout', '2')
        time.sleep(.1)
        with socket.create_connection(('127.0.0.1', self.port), timeout=2) as slow:
            slow.sendall(b'POST /v2/pair HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n{')
            time.sleep(.1)
            with socket.create_connection(('127.0.0.1', self.port), timeout=2) as excess:
                excess.sendall(b'GET /healthz HTTP/1.0\r\n\r\n')
                try:
                    self.assertEqual(excess.recv(4096), b'')
                except ConnectionError:
                    pass
            time.sleep(2.2)
            self.assertEqual(self.request('GET', '/healthz')[0], 200)

    def test_live_backup_restore_includes_history_and_partial_uploads(self):
        _, device = self.pair()
        token = device['token']
        content = b'MVPK backed up ciphertext'
        h = hashlib.sha256(content).hexdigest()
        self.assertEqual(self.upload(content, token)[0], 200)
        value = {'base_revision': 0, 'blob': h, 'size': len(content), 'deleted': False, 'operation_id': 'a'*32}
        path = '/v2/objects/' + 'a'*32
        original = self.request('POST', path, value, token)[2]
        partial = b'MVPK unfinished'
        hp = hashlib.sha256(partial).hexdigest()
        self.assertEqual(self.upload(partial[:5], token, total=len(partial), hash_value=hp)[0], 200)
        backup = Path(self.tmp.name) / 'backup'
        result = self.cli('backup', str(backup))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(self.cli('backup', str(backup)).returncode, 0)
        self.stop()
        self.data = backup
        self.start()
        self.assertEqual(self.request('GET', '/v2/objects', token=token)[2]['objects'], [original])
        self.assertEqual(self.request('GET', '/v2/blobs/' + h, token=token)[2], content)
        self.assertEqual(self.request('HEAD', '/v2/blobs/' + hp, token=token)[1]['X-Upload-Offset'], '5')
        self.assertTrue(self.upload(partial[5:], token, 5, len(partial), hp)[2]['complete'])
        self.assertEqual(self.request('POST', path, value, token)[2], original)

    def test_schema_version_migration_and_newer_schema_refusal(self):
        self.stop()
        with sqlite3.connect(self.data / 'sync.sqlite3') as db:
            db.execute('PRAGMA user_version=0')
        db.close()
        self.assertEqual(self.cli('devices').returncode, 0)
        with sqlite3.connect(self.data / 'sync.sqlite3') as db:
            self.assertEqual(db.execute('PRAGMA user_version').fetchone()[0], 1)
            db.execute('PRAGMA user_version=999')
        db.close()
        self.assertNotEqual(self.cli('devices').returncode, 0)

    def test_crash_window_recovery_and_unacknowledged_tail(self):
        _, device = self.pair()
        token = device['token']
        content = b'MVPK crash-resumable bytes'
        h = hashlib.sha256(content).hexdigest()
        self.assertEqual(self.upload(content[:5], token, total=len(content), hash_value=h)[0], 200)
        self.stop()
        partial = self.data / 'blobs' / (h + '.part')
        with partial.open('ab') as f:
            f.write(b'unacknowledged tail')
        self.start()
        self.assertEqual(partial.stat().st_size, 5)
        self.assertTrue(self.upload(content[5:], token, 5, len(content), h)[2]['complete'])
        self.stop()
        # Model atomic rename before SQLite commit of the final append.
        with sqlite3.connect(self.data / 'sync.sqlite3') as db:
            db.execute('UPDATE blobs SET complete=0,offset=5 WHERE hash=?', (h,))
        db.close()
        orphan = self.data / 'blobs' / ('f'*64 + '.part')
        orphan.write_bytes(b'orphan interrupted first append')
        self.start()
        self.assertFalse(orphan.exists())
        self.assertEqual(self.request('HEAD', '/v2/blobs/' + h, token=token)[1]['X-Blob-Complete'], 'true')
        self.assertEqual(self.request('GET', '/v2/blobs/' + h, token=token)[2], content)

    def test_pairing_rejects_unknown_sensitive_fields_and_logs_no_secrets(self):
        code = self.cli('pair-code').stdout.strip()
        sentinel = 'never-log-vault-password'
        self.assertEqual(self.request('POST', '/v2/pair', {'code': code, 'name': 'label', 'password': sentinel})[0], 400)
        status, _, device = self.request('POST', '/v2/pair', {'code': code, 'name': 'label'})
        self.assertEqual(status, 200)
        self.request('GET', '/v2/objects?secret=' + sentinel, token=device['token'])
        self.proc.terminate()
        out, err = self.proc.communicate(timeout=5)
        self.proc = None
        for secret in (code, device['token'], sentinel):
            self.assertNotIn(secret.encode(), out + err)

    def test_revocation_blocks_inflight_upload_before_durable_write(self):
        _, device = self.pair()
        data = b'MVPK revoked upload'
        h = hashlib.sha256(data).hexdigest()
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as sock:
            request = f'PUT /v2/blobs/{h}?offset=0 HTTP/1.0\r\nAuthorization: Bearer {device["token"]}\r\nX-Blob-Size: {len(data)}\r\nContent-Length: {len(data)}\r\n\r\n'
            sock.sendall(request.encode())
            time.sleep(.15)
            self.assertEqual(self.cli('revoke', device['device_id']).returncode, 0)
            sock.sendall(data)
            response = http.client.HTTPResponse(sock)
            response.begin()
            self.assertEqual(response.status, 401)
            response.read()
        self.assertFalse((self.data / 'blobs' / h).exists())

    def test_interrupted_network_body_never_advances_offset(self):
        _, device = self.pair()
        content = b'MVPK durable chunks'
        h = hashlib.sha256(content).hexdigest()
        self.upload(content[:4], device['token'], total=len(content), hash_value=h)
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as sock:
            request = f'PUT /v2/blobs/{h}?offset=4 HTTP/1.0\r\nAuthorization: Bearer {device["token"]}\r\nX-Blob-Size: {len(content)}\r\nContent-Length: {len(content)-4}\r\n\r\n'
            sock.sendall(request.encode() + content[4:7])
        time.sleep(.1)
        self.assertEqual(self.request('HEAD', '/v2/blobs/' + h, token=device['token'])[1]['X-Upload-Offset'], '4')
        self.assertTrue(self.upload(content[4:], device['token'], 4, len(content), h)[2]['complete'])

    def test_concurrent_append_and_device_scoped_operation_ids(self):
        _, first = self.pair('one')
        _, second = self.pair('two')
        content = b'MVPK simultaneous append'
        h = hashlib.sha256(content).hexdigest()
        def append(device):
            return self.upload(content[:5], device['token'], total=len(content), hash_value=h)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(append, [first, second]))
        self.assertEqual(sorted(r[0] for r in results), [200, 409])
        self.assertTrue(self.upload(content[5:], first['token'], 5, len(content), h)[2]['complete'])
        value = {'base_revision': 0, 'blob': h, 'size': len(content), 'deleted': False, 'operation_id': 'a'*32}
        self.assertEqual(self.request('POST', '/v2/objects/' + 'a'*32, value, first['token'])[0], 200)
        self.assertEqual(self.request('POST', '/v2/objects/' + 'b'*32, value, second['token'])[0], 200)
        records = self.request('GET', '/v2/objects', token=first['token'])[2]['objects']
        self.assertEqual([r['sequence'] for r in records], sorted(r['sequence'] for r in records))
        self.assertEqual(len(records), 2)

    def test_unbounded_integer_and_empty_blob_count_are_rejected(self):
        _, device = self.pair()
        token = device['token']
        value = {'base_revision': 2**100, 'blob': '', 'size': 0, 'deleted': True, 'operation_id': 'a'*32}
        self.assertEqual(self.request('POST', '/v2/objects/' + 'a'*32, value, token)[0], 400)
        self.stop()
        self.start('--max-blobs', '1')
        self.assertTrue(self.upload(b'', token)[2]['complete'])
        self.assertEqual(self.upload(b'x', token)[0], 507)

    def test_pair_authentication_hash_storage_and_revocation(self):
        self.assertEqual(self.request('GET', '/v2/devices')[0], 401)
        code, device = self.pair()
        self.assertEqual(len(device['token']), 64)
        self.assertEqual(len(device['device_id']), 32)
        self.assertEqual(self.request('GET', '/v2/devices', token=device['token'])[2]['devices'][0]['name'], 'laptop')
        self.assertEqual(self.request('POST', '/v2/pair', {'code': code, 'name': 'reuse'})[0], 401)
        for file in self.data.glob('*'):
            if file.is_file():
                self.assertNotIn(device['token'].encode(), file.read_bytes())
                self.assertNotIn(code.encode(), file.read_bytes())
        self.assertEqual(self.cli('revoke', device['device_id']).returncode, 0)
        self.assertEqual(self.request('GET', '/v2/devices', token=device['token'])[0], 401)
        listed = json.loads(self.cli('devices').stdout)
        self.assertTrue(listed['devices'][0]['revoked'])

    def upload(self, content, token, offset=0, total=None, hash_value=None):
        hash_value = hash_value or hashlib.sha256(content).hexdigest()
        return self.request('PUT', '/v2/blobs/' + hash_value + '?offset=' + str(offset), content, token, {'X-Blob-Size': str(total if total is not None else len(content)), 'Content-Type': 'application/octet-stream'})

    def test_transactional_conflicts_idempotency_and_immutable_history(self):
        _, device = self.pair()
        token = device['token']
        content = b'MVPK opaque'
        h = hashlib.sha256(content).hexdigest()
        path = '/v2/objects/' + 'a'*32
        value = {'base_revision': 0, 'blob': h, 'size': len(content), 'deleted': False, 'operation_id': 'b'*32}
        self.assertEqual(self.request('GET', '/v2/objects', token=token)[2], {'objects': []})
        self.assertEqual(self.request('POST', path, value, token)[0], 422)
        self.assertEqual(self.upload(content, token)[0], 200)
        status, _, original = self.request('POST', path, value, token)
        self.assertEqual(status, 200)
        self.assertEqual(original['revision'], 1)
        self.assertEqual(self.request('POST', path, value, token)[2], original)
        self.assertEqual(self.request('POST', path, dict(value, size=999), token)[0], 409)
        def commit(op):
            return self.request('POST', path, dict(value, base_revision=1, operation_id=op*32), token)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(commit, ['c', 'd']))
        self.assertEqual(sorted(r[0] for r in results), [200, 409])
        current = self.request('GET', '/v2/objects', token=token)[2]['objects'][0]
        self.assertEqual(current['revision'], 2)
        self.assertEqual([r[2]['current'] for r in results if r[0] == 409], [current])
        tombstone = dict(value, base_revision=2, blob='', size=0, deleted=True, operation_id='e'*32)
        self.assertEqual(self.request('POST', path, tombstone, token)[2]['revision'], 3)
        history = self.request('GET', path + '/history', token=token)[2]['versions']
        self.assertEqual(history[0], original)
        self.assertEqual([r['revision'] for r in history], [1, 2, 3])
        self.assertTrue(history[-1]['deleted'])
        self.stop()
        self.start()
        self.assertEqual(self.request('POST', path, value, token)[2], original)

    def test_durable_partial_resume_hash_validation_and_ranges(self):
        _, device = self.pair()
        token = device['token']
        content = b'MVPK opaque encrypted bytes' * 100
        h = hashlib.sha256(content).hexdigest()
        self.assertEqual(self.request('HEAD', '/v2/blobs/' + h, token=token)[0], 404)
        self.assertEqual(self.upload(content[:40], token, total=len(content), hash_value=h)[2], {'offset': 40, 'complete': False})
        self.assertEqual(self.request('GET', '/v2/blobs/' + h, token=token)[0], 404)
        self.assertEqual(self.upload(content[:40], token, total=len(content), hash_value=h)[2], {'offset': 40})
        self.stop()
        self.start()
        status, headers, _ = self.request('HEAD', '/v2/blobs/' + h, token=token)
        self.assertEqual((status, headers['X-Upload-Offset'], headers['Content-Length'], headers['X-Blob-Complete']), (200, '40', str(len(content)), 'false'))
        self.assertEqual(self.upload(content[40:], token, 40, len(content), h)[2], {'offset': len(content), 'complete': True})
        self.assertEqual(self.upload(content, token)[2]['complete'], True)
        self.assertEqual(self.request('GET', '/v2/blobs/' + h, token=token)[2], content)
        status, headers, data = self.request('GET', '/v2/blobs/' + h, token=token, headers={'Range': 'bytes=3-12'})
        self.assertEqual((status, data, headers['Content-Range']), (206, content[3:13], 'bytes 3-12/' + str(len(content))))
        self.assertEqual(self.request('GET', '/v2/blobs/' + h, token=token, headers={'Range': 'bytes=999999-'})[0], 416)
        self.assertEqual(self.upload(b'corrupt', token, hash_value='0'*64)[0], 422)
        self.assertEqual(self.request('HEAD', '/v2/blobs/' + '0'*64, token=token)[0], 404)

if __name__ == '__main__':
    unittest.main()
