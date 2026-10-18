"""Shared harness: every test drives a real server process over a real socket."""
import hashlib
import http.client
import itertools
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
PASSWORD = 'correct horse battery staple'
_counter = itertools.count(1)


def scratch_dir():
    if os.name == 'nt':
        base = Path.home() / 'AppData' / 'Local' / 'hermes' / 'cache' / 'scratch'
    else:
        base = Path(os.environ.get('TMPDIR', tempfile.gettempdir()))
    base.mkdir(parents=True, exist_ok=True)
    return base


def package(content):
    """The content address a client uploads this ciphertext under."""
    return hashlib.sha256(content).hexdigest()


class ServerCase(unittest.TestCase):
    """Temp data directory, one spawned server, no mocks anywhere."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=scratch_dir())
        self.data = Path(self.tmp.name) / 'data'
        self.proc = None
        self.port = None
        self.tls_context = None
        self.start()

    def tearDown(self):
        self.stop()
        self.tmp.cleanup()

    # -- process ----------------------------------------------------------

    def cli(self, *args, stdin=''):
        try:
            return subprocess.run([sys.executable, '-m', 'sync_server', '--data', str(self.data), *args],
                                  cwd=ROOT, capture_output=True, text=True, input=stdin, timeout=60)
        except subprocess.TimeoutExpired:
            self.fail('CLI did not terminate: ' + args[0])

    def free_port(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            return sock.getsockname()[1]

    def launch(self, *args):
        """Run serve with *args on a free port; return once it accepts a socket."""
        self.port = self.free_port()
        self.proc = subprocess.Popen([sys.executable, '-m', 'sync_server', '--data', str(self.data),
                                      'serve', '--port', str(self.port), *args],
                                     cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(300):
            if self.proc.poll() is not None:
                break
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=.2):
                    return
            except OSError:
                time.sleep(.03)
        self.fail('server did not start: ' + self.proc.communicate(timeout=10)[1].decode())

    def start(self, *args):
        self.launch(*args)

    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.terminate()
            self.proc.communicate(timeout=20)
            self.proc = None

    # -- raw requests -----------------------------------------------------

    def request(self, method, path, body=None, token=None, cookie=None, headers=None,
                tls=False, timeout=20):
        """One request on its own connection; JSON in, JSON out unless bytes."""
        h = dict(headers or {})
        payload = None
        if isinstance(body, (bytes, bytearray)):
            payload = bytes(body)
        elif body is not None:
            payload = json.dumps(body).encode()
            h.setdefault('Content-Type', 'application/json')
        if token:
            h['Authorization'] = 'Bearer ' + token
        if cookie:
            h['Cookie'] = 'mv_session=' + cookie
        if tls:
            conn = http.client.HTTPSConnection('127.0.0.1', self.port, timeout=timeout, context=self.tls_context)
        else:
            conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)
        try:
            conn.request(method, path, body=payload, headers=h)
            response = conn.getresponse()
            data = response.read()
            kind = response.getheader('Content-Type', '')
            parsed = json.loads(data) if data and kind.startswith('application/json') else data
            return response.status, {k.lower(): v for k, v in response.getheaders()}, parsed
        finally:
            conn.close()

    # -- accounts ---------------------------------------------------------

    def new_email(self):
        return 'user%d-%s@example.com' % (next(_counter), secrets.token_hex(3))

    def register(self, email=None, password=PASSWORD):
        email = email or self.new_email()
        status, _, value = self.request('POST', '/auth/register', {'email': email, 'password': password})
        self.assertEqual(status, 201, value)
        return value['account'], password

    def login(self, email, password=PASSWORD):
        status, headers, value = self.request('POST', '/auth/login', {'email': email, 'password': password})
        self.assertEqual(status, 200, value)
        return self.cookie(headers), value['account']

    def cookie(self, headers):
        raw = headers.get('set-cookie', '')
        self.assertTrue(raw, 'no session cookie was set')
        return raw.split(';', 1)[0].split('=', 1)[1]

    def user(self):
        """A registered, logged-in account: (cookie, account)."""
        account, password = self.register()
        cookie, _ = self.login(account['email'], password)
        return cookie, account

    def token_for(self, cookie, label='test-device'):
        status, _, value = self.request('POST', '/auth/token', {'label': label}, cookie=cookie)
        self.assertEqual(status, 201, value)
        return value

    # -- vaults, records, blobs -------------------------------------------

    def vault(self, cookie, name='Vault'):
        status, _, value = self.request('POST', '/v1/vaults', {'name': name}, cookie=cookie)
        self.assertEqual(status, 201, value)
        return value['vault']

    def vaults(self, cookie):
        status, _, value = self.request('GET', '/v1/vaults', cookie=cookie)
        self.assertEqual(status, 200, value)
        return value

    def put_record(self, token, vault, rid, base, operation=None, **fields):
        body = {'base_version': base, 'operation': operation or 'op-' + secrets.token_hex(8),
                'name': fields.pop('name', rid), 'path': fields.pop('path', ''),
                'tags': fields.pop('tags', []), 'package': fields.pop('package', None),
                'size': fields.pop('size', 0)}
        body.update(fields)
        return self.request('PUT', '/v1/vaults/%s/records/%s' % (vault, rid), body, token=token)

    def records(self, token, vault, since=0, limit=None):
        path = '/v1/vaults/%s/records?since=%d' % (vault, since)
        if limit:
            path += '&limit=%d' % limit
        status, _, value = self.request('GET', path, token=token)
        self.assertEqual(status, 200, value)
        return value

    def blob_path(self, vault, name):
        return '/v1/vaults/%s/blobs/%s' % (vault, name)

    def put_blob(self, token, vault, content, name=None, offset=0, total=None, tls=False):
        name = name or package(content)
        total = len(content) if total is None else total
        return self.request('PUT', self.blob_path(vault, name) + '?offset=%d' % offset, content, token=token,
                            headers={'X-Total-Size': str(total), 'Content-Type': 'application/octet-stream'},
                            tls=tls)

    # -- database ---------------------------------------------------------

    def rows(self, sql, args=()):
        import sqlite3
        db = sqlite3.connect(self.data / 'sync.sqlite3')
        try:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(sql, args)]
        finally:
            db.close()

    def certificate(self):
        """A throwaway certificate for the loopback host, or skip."""
        if not shutil.which('openssl'):
            self.skipTest('openssl not available to mint a throwaway certificate')
        cert, key = Path(self.tmp.name) / 'cert.pem', Path(self.tmp.name) / 'key.pem'
        result = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                                 '-out', str(cert), '-days', '1', '-subj', '/CN=localhost',
                                 '-addext', 'subjectAltName=IP:127.0.0.1,DNS:localhost'],
                                capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.tls_context = ssl.create_default_context(cafile=str(cert))
        return cert, key
