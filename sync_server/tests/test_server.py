"""Transport: health, unknown routes, TLS, binding refusals, and silence in logs."""
import http.client
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import PASSWORD, ROOT, ServerCase, package


class TransportTests(ServerCase):
    def test_health_is_open_and_versioned(self):
        status, _, value = self.request('GET', '/healthz')
        self.assertEqual((status, value), (200, {'ok': True, 'protocol': 4, 'registration': 'open'}))
        self.assertEqual(self.request('POST', '/healthz', {})[0], 405)

    def test_every_data_route_needs_authentication(self):
        cookie, _ = self.user()
        vault = self.vault(cookie)['id']
        open_paths = [
            ('GET', '/v1/vaults'), ('POST', '/v1/vaults'), ('GET', '/v1/vaults/' + vault),
            ('PATCH', '/v1/vaults/' + vault), ('DELETE', '/v1/vaults/' + vault),
            ('GET', '/v1/vaults/%s/records' % vault), ('PUT', '/v1/vaults/%s/records/r1' % vault),
            ('GET', '/v1/vaults/%s/records/r1' % vault), ('DELETE', '/v1/vaults/%s/records/r1' % vault),
            ('PUT', '/v1/vaults/%s/blobs/%s?offset=0' % (vault, package(b'x'))),
            ('HEAD', '/v1/vaults/%s/blobs/%s' % (vault, package(b'x'))),
            ('GET', '/auth/token'), ('POST', '/auth/token'), ('GET', '/auth/session'),
        ]
        for method, path in open_paths:
            body = {} if method in ('PUT', 'POST', 'PATCH') else None
            status, _, _ = self.request(method, path, body)
            self.assertEqual(status, 401, (method, path))
        for token in ('f' * 64, 'not-a-token', 'F' * 64, 'a' * 63):
            self.assertEqual(self.request('GET', '/v1/vaults', token=token)[0], 401, token)

    def test_unknown_routes_and_methods(self):
        cookie, _ = self.user()
        vault = self.vault(cookie)['id']
        token = self.token_for(cookie)['token']
        self.assertEqual(self.request('GET', '/v1')[0], 404)
        self.assertEqual(self.request('GET', '/v1/vaults/' + 'z' * 32, cookie=cookie)[0], 404)
        self.assertEqual(self.request('GET', '/v1/vaults/%s/records/r1/nope' % vault, token=token)[0], 404)
        self.assertEqual(self.request('POST', '/v1/vaults/%s/records' % vault, {}, token=token)[0], 405)
        self.assertEqual(self.request('PUT', '/v1/vaults/%s/blobs/' % vault, b'x', token=token)[0], 404)
        self.assertEqual(self.request('GET', '/v3/files', token=token)[0], 404, 'the v3 bucket is gone')

    def test_non_loopback_binding_requires_tls(self):
        self.stop()
        result = self.cli('serve', '--host', '0.0.0.0', '--port', '0')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('TLS', result.stderr)
        result = self.cli('serve', '--port', '0', '--cert', 'only-a-cert.pem')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('both', result.stderr)

    def test_tls_serves_the_same_protocol_with_secure_cookies(self):
        cert, key = self.certificate()
        self.stop()
        self.start('--cert', str(cert), '--key', str(key))
        self.assertEqual(self.request('GET', '/healthz', tls=True)[2]['protocol'], 4)
        email = self.new_email()
        self.assertEqual(self.request('POST', '/auth/register', {'email': email, 'password': PASSWORD},
                                      tls=True)[0], 201)
        status, headers, _ = self.request('POST', '/auth/login', {'email': email, 'password': PASSWORD},
                                          tls=True)
        self.assertEqual(status, 200)
        self.assertIn('Secure', headers['set-cookie'])
        self.assertIn('HttpOnly', headers['set-cookie'])
        cookie = self.cookie(headers)
        self.assertEqual(self.request('GET', '/v1/vaults', cookie=cookie, tls=True)[0], 200)
        self.assertEqual(self.request('GET', '/', tls=True)[0], 200)

    def test_nothing_secret_is_logged_or_stored_in_the_clear(self):
        cookie, account = self.user()
        token = self.token_for(cookie)['token']
        vault = self.vault(cookie)['id']
        content = b'ciphertext-with-a-sentinel-9f2c'
        self.put_blob(token, vault, content)
        sentinel = 'query-sentinel-13ab'
        self.request('GET', '/v1/vaults?leak=' + sentinel, token=token)
        self.proc.terminate()
        out, err = self.proc.communicate(timeout=20)
        self.proc = None
        logs = out + err
        for secret in (token, PASSWORD, sentinel, content.decode()):
            self.assertNotIn(secret.encode(), logs, 'a secret reached the log')
        for path in self.data.rglob('*'):
            if path.is_file():
                blob = path.read_bytes()
                self.assertNotIn(token.encode(), blob, 'a device token was stored in the clear: ' + path.name)
                self.assertNotIn(PASSWORD.encode(), blob, 'a password was stored in the clear: ' + path.name)

    def test_a_data_directory_from_an_older_protocol_is_refused(self):
        old = Path(self.tmp.name) / 'old'
        old.mkdir()
        db = sqlite3.connect(old / 'sync.sqlite3')
        db.execute('PRAGMA user_version=3')
        db.execute('CREATE TABLE files(name TEXT PRIMARY KEY, size INTEGER)')
        db.commit()
        db.close()
        result = subprocess.run([sys.executable, '-m', 'sync_server', '--data', str(old), 'account',
                                 'create', 'x@example.com', '--password-stdin'],
                                cwd=ROOT, capture_output=True, text=True, input='a long password\n', timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('protocol 3', result.stderr)
        self.assertFalse((old / 'blobs').exists(), 'refusing must not touch the old directory')
        db = sqlite3.connect(old / 'sync.sqlite3')
        try:
            self.assertEqual(db.execute('PRAGMA user_version').fetchone()[0], 3)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM sqlite_master WHERE name='files'").fetchone()[0], 1)
        finally:
            db.close()


    def test_a_rejected_body_does_not_poison_the_connection(self):
        """A 4xx that never reads the body must not corrupt the next request.

        Without draining or closing, the leftover bytes are parsed as the next
        request line and the client gets a 501 from the HTTP layer instead of
        the error we sent.
        """
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        try:
            conn.request('PATCH', '/auth/login',
                         body=b'{"email":"stranger@example.com"}',
                         headers={'Content-Type': 'application/json'})
            first = conn.getresponse()
            first.read()
            self.assertEqual(first.status, 405)
            conn.request('GET', '/healthz')
            second = conn.getresponse()
            payload = json.loads(second.read())
            self.assertEqual(second.status, 200, payload)
            self.assertEqual(payload['protocol'], 4)
        finally:
            conn.close()


if __name__ == '__main__':
    unittest.main()
