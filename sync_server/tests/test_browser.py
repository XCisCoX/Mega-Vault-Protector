"""Browser file API: real subprocess, real cookies and real TCP sockets."""
import http.client
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import ServerCase, package


class BrowserTests(ServerCase):
    def setUp(self):
        super().setUp()
        self.jar, self.account = self.user()
        self.device = self.token_for(self.jar)['token']
        self.vid = self.vault(self.jar, 'Browser files')['id']

    def test_session_exposes_stable_session_bound_csrf_without_credentials(self):
        status, headers, value = self.request('GET', '/auth/session', cookie=self.jar)
        self.assertEqual(status, 200, value)
        self.assertEqual(set(value), {'account', 'csrf'})
        self.assertEqual(value['account'], self.account)
        self.assertRegex(value['csrf'], r'^[0-9a-f]{64}$')
        self.assertEqual(headers['cache-control'], 'no-store')
        self.assertNotIn(self.jar, str(value))
        self.assertNotIn(self.device, str(value))
        self.assertEqual(self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf'], value['csrf'])
        another, _ = self.login(self.account['email'])
        self.assertNotEqual(self.request('GET', '/auth/session', cookie=another)[2]['csrf'], value['csrf'])
        for credentials in ({}, {'token': self.device}, {'cookie': 'invalid-session', 'token': self.device}):
            self.assertEqual(self.request('GET', '/auth/session', **credentials)[0], 401)

    def test_browser_record_write_requires_cookie_and_session_csrf(self):
        path = '/browser/vaults/%s/records/r1' % self.vid
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        body = {'base_version': 0, 'operation': 'browser-create', 'name': 'notes.txt'}
        for credentials in ({}, {'token': self.device}, {'cookie': 'invalid', 'token': self.device}):
            self.assertEqual(self.request('PUT', path, body, headers={'X-CSRF-Token': csrf},
                                          **credentials)[0], 401)
        for headers in ({}, {'X-CSRF-Token': ''}, {'X-CSRF-Token': 'wrong'}):
            self.assertEqual(self.request('PUT', path, body, cookie=self.jar, headers=headers)[0], 403)
        self.assertEqual(self.records(self.device, self.vid)['records'], [])
        status, _, value = self.request('PUT', path, body, cookie=self.jar,
                                        headers={'X-CSRF-Token': csrf})
        self.assertEqual(status, 201, value)
        self.assertEqual(value['record']['name'], 'notes.txt')
        status, _, readback = self.request('GET', path, cookie=self.jar)
        self.assertEqual((status, readback), (200, value))
        self.assertEqual(self.records(self.device, self.vid)['records'], [value['record']])
        self.assertEqual(self.request('GET', path, token=self.device)[0], 401)
        self.assertEqual(self.request('GET', path.replace('/browser/', '/v1/'), cookie=self.jar)[0], 401)

    def test_browser_refuses_cross_origin_and_fetch_metadata(self):
        path = '/browser/vaults/%s/records/r1' % self.vid
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        body = {'base_version': 0, 'operation': 'origin-create', 'name': 'safe.txt'}
        host = '127.0.0.1:%d' % self.port
        for extra in ({'Origin': 'https://evil.example'}, {'Origin': 'https://' + host},
                      {'Origin': 'null'}, {'Origin': 'http://user@' + host},
                      {'Origin': 'http://' + host + '/not-an-origin'},
                      {'Sec-Fetch-Site': 'cross-site'}, {'Sec-Fetch-Site': 'same-site'}):
            with self.subTest(extra=extra):
                headers = {'X-CSRF-Token': csrf, **extra}
                self.assertEqual(self.request('PUT', path, body, cookie=self.jar, headers=headers)[0], 403)
        for path in ('/auth/session', '/browser/vaults/%s/records' % self.vid):
            self.assertEqual(self.request('GET', path, cookie=self.jar,
                                          headers={'Sec-Fetch-Site': 'cross-site'})[0], 403)
        self.assertEqual(self.records(self.device, self.vid)['records'], [])
        status, _, value = self.request('PUT', '/browser/vaults/%s/records/r1' % self.vid, body,
                                        cookie=self.jar, headers={'X-CSRF-Token': csrf,
                                        'Origin': 'http://' + host, 'Sec-Fetch-Site': 'same-origin'})
        self.assertEqual(status, 201, value)

    def test_browser_ciphertext_blob_resumes_and_downloads_with_ranges(self):
        content = bytes(range(256)) * 8192
        address = package(content)
        path = '/browser/vaults/%s/blobs/%s' % (self.vid, address)
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        headers = {'X-CSRF-Token': csrf, 'X-Total-Size': str(len(content)),
                   'Content-Type': 'application/octet-stream'}
        self.assertEqual(self.request('PUT', path, b'plaintext form body', cookie=self.jar,
                                      headers={**headers, 'Content-Type': 'text/plain'})[0], 415)
        self.assertEqual(self.request('PUT', path, content[:1048576], cookie=self.jar, headers=headers)[0], 201)
        status, head, _ = self.request('HEAD', path, cookie=self.jar)
        self.assertEqual((status, head['x-offset'], head['x-complete']), (200, '1048576', 'false'))
        self.assertEqual(self.request('GET', path, cookie=self.jar)[0], 404)
        self.assertEqual(self.request('PUT', path, b'x', cookie=self.jar, headers=headers)[2], {'offset': 1048576})
        self.stop()
        self.start()
        status, head, _ = self.request('HEAD', path, cookie=self.jar)
        self.assertEqual((status, head['x-offset']), (200, '1048576'))
        status, _, value = self.request('PUT', path + '?offset=1048576', content[1048576:],
                                        cookie=self.jar, headers=headers)
        self.assertEqual((status, value), (200, {'offset': len(content), 'complete': True}))
        status, received_headers, received = self.request('GET', path, cookie=self.jar)
        self.assertEqual((status, received), (200, content))
        self.assertEqual(package(received), address)
        self.assertEqual(received_headers['content-type'], 'application/octet-stream')
        status, ranged_headers, ranged = self.request('GET', path, cookie=self.jar,
                                                      headers={'Range': 'bytes=17-100'})
        self.assertEqual((status, ranged), (206, content[17:101]))
        self.assertEqual(ranged_headers['content-range'], 'bytes 17-100/%d' % len(content))
        self.assertEqual(self.request('GET', path, cookie=self.jar, headers={'Range': 'bytes=100-'})[2],
                         content[100:])
        self.assertEqual(self.request('GET', path, cookie=self.jar, headers={'Range': 'bytes=3-1'})[0], 416)
        self.assertEqual(self.request('GET', path.replace('/browser/', '/v1/'), token=self.device)[2], content)
        self.assertEqual(self.request('GET', path.replace('/browser/', '/v1/'), cookie=self.jar)[0], 401)
        self.assertEqual(self.request('GET', path, token=self.device)[0], 401)
        for method in ('GET', 'HEAD', 'PUT', 'DELETE'):
            self.assertEqual(self.request(method, path, token=self.device, headers=headers)[0], 401)
        self.assertEqual(self.request('DELETE', path, cookie=self.jar, headers={'X-CSRF-Token': csrf})[0], 204)
        self.assertEqual(self.request('GET', path, cookie=self.jar)[0], 404)

    def test_browser_capabilities_are_session_only_and_match_configured_limits(self):
        self.stop()
        self.start('--max-file-bytes', '1234567')
        path = '/browser/capabilities'
        for credentials in ({}, {'token': self.device}, {'cookie': 'invalid'}):
            self.assertEqual(self.request('GET', path, **credentials)[0], 401)
        status, headers, value = self.request('GET', path, cookie=self.jar)
        self.assertEqual((status, value), (200, {'protocol': 4, 'max_file_bytes': 1234567,
                                               'chunk_bytes': 1048576}))
        self.assertEqual(headers['cache-control'], 'no-store')
        self.assertEqual(self.request('GET', path, cookie=self.jar,
                                      headers={'Sec-Fetch-Site': 'cross-site'})[0], 403)
        self.assertEqual(self.request('POST', path, cookie=self.jar)[0], 405)

    def test_self_hosted_crypto_assets_have_explicit_routes_and_minimal_csp(self):
        self.stop()
        web = Path(self.tmp.name) / 'web'
        (web / 'vendor').mkdir(parents=True)
        (web / 'index.html').write_text('<!doctype html><title>fixture</title>', encoding='utf-8')
        assets = ('files-model.js', 'package-crypto.js', 'vendor/sodium.js', 'vendor/sodium-wrappers.js')
        for name in assets:
            (web / name).write_text('// fixture asset: ' + name, encoding='utf-8')
        (web / 'vendor' / 'secret.js').write_text('not public', encoding='utf-8')
        self.start('--web', str(web))
        for name in assets:
            status, headers, body = self.request('GET', '/' + name)
            self.assertEqual(status, 200, name)
            self.assertEqual(body.decode(), '// fixture asset: ' + name)
            self.assertEqual(headers['content-type'], 'text/javascript; charset=utf-8')
            self.assertEqual(headers['x-content-type-options'], 'nosniff')
            self.assertEqual(self.request('HEAD', '/' + name)[0], 200)
            self.assertEqual(self.request('POST', '/' + name)[0], 405)
        for path in ('/vendor/secret.js', '/vendor/../__main__.py', '/vendor/%2e%2e/__main__.py',
                     '/vendor/sodium.js/extra', '/vendor/', '/package-crypto.js.map'):
            self.assertEqual(self.request('GET', path)[0], 404, path)
        csp = self.request('GET', '/')[1]['content-security-policy']
        directives = dict((row.strip().split()[0], row.strip().split()[1:]) for row in csp.split(';'))
        self.assertEqual(directives['script-src'], ["'self'", "'wasm-unsafe-eval'"])
        self.assertEqual(directives['img-src'], ["'self'", 'blob:'])
        self.assertEqual(directives['media-src'], ["'self'", 'blob:'])
        self.assertEqual(directives['object-src'], ["'none'"])
        self.assertNotIn("'unsafe-inline'", csp)
        self.assertNotIn("'unsafe-eval'", csp)
        self.assertNotIn('https:', csp)

    def test_cookie_account_is_authoritative_even_with_another_accounts_bearer(self):
        other_cookie, other_account = self.user()
        other_device = self.token_for(other_cookie)['token']
        csrf = self.request('GET', '/auth/session', cookie=other_cookie)[2]['csrf']
        own_csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        blob = b'opaque ciphertext'
        address = package(blob)
        self.assertEqual(self.put_blob(self.device, self.vid, blob)[0], 201)
        self.assertEqual(self.put_record(self.device, self.vid, 'r1', 0, name='private')[0], 201)
        base = '/browser/vaults/' + self.vid
        for method, suffix, body in (
                ('GET', '/records', None), ('GET', '/records/r1', None),
                ('PUT', '/records/r1', {'base_version': 1, 'operation': 'other', 'name': 'stolen'}),
                ('DELETE', '/records/r1', None), ('POST', '/records/r1/restore', None),
                ('GET', '/blobs/' + address, None), ('HEAD', '/blobs/' + address, None),
                ('PUT', '/blobs/' + address, blob), ('DELETE', '/blobs/' + address, None)):
            with self.subTest(method=method, suffix=suffix):
                status, _, _ = self.request(method, base + suffix, body, cookie=other_cookie,
                    token=self.device, headers={'X-CSRF-Token': csrf,
                    'Content-Type': 'application/octet-stream' if isinstance(body, bytes) else 'application/json',
                    'X-Total-Size': str(len(blob))})
                self.assertEqual(status, 404)
        # A bearer for B must not override cookie A, even when it is valid.
        self.assertEqual(self.request('GET', base + '/records/r1', cookie=self.jar,
                                      token=other_device)[2]['record']['name'], 'private')
        self.assertEqual(self.request('GET', '/auth/session', cookie=self.jar,
                                      token=other_device)[2]['account'], self.account)
        self.assertNotEqual(other_account['id'], self.account['id'])
        # A CSRF value from another live session cannot authorize this cookie.
        self.assertEqual(self.request('DELETE', base + '/records/r1', cookie=self.jar,
                                      headers={'X-CSRF-Token': csrf})[0], 403)
        self.assertEqual(self.request('DELETE', base + '/records/r1', cookie=self.jar,
                                      headers={'X-CSRF-Token': own_csrf})[0], 200)
        self.assertEqual(self.request('GET', base + '/blobs/' + address, cookie=self.jar)[2], blob)

    def test_browser_vault_and_resource_identifiers_are_exact(self):
        for identifier in (self.vid.upper(), self.vid + '0', self.vid[:-1],
                           '0' * 32, '%2e%2e', '../' + self.vid, '/' + self.vid,
                           self.vid + '/', '%31' + self.vid[1:]):
            with self.subTest(identifier=identifier):
                self.assertEqual(self.request('GET', '/browser/vaults/' + identifier + '/records',
                                              cookie=self.jar)[0], 404)
        for suffix in ('records/', 'records//r1', 'records/%72', 'records/../r1',
                       'blobs/' + 'A' * 64, 'blobs/' + 'a' * 65, 'blobs/' + 'a' * 64 + '/',
                       'upload', 'decrypt', 'encrypt', 'files'):
            self.assertEqual(self.request('GET', '/browser/vaults/' + self.vid + '/' + suffix,
                                          cookie=self.jar)[0], 404, suffix)
        self.assertEqual(self.request('GET', '/browser/vaults/%s/records' % self.vid,
                                      cookie=self.jar)[0], 200)

    def test_all_browser_mutations_require_csrf_before_any_state_change(self):
        content = b'opaque encrypted package bytes'
        address = package(content)
        self.assertEqual(self.put_blob(self.device, self.vid, content)[0], 201)
        self.assertEqual(self.put_record(self.device, self.vid, 'r1', 0, name='stay')[0], 201)
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        base = '/browser/vaults/' + self.vid
        for method, suffix, body in (
                ('PUT', '/records/r1', {'base_version': 1, 'operation': 'blocked', 'name': 'changed'}),
                ('DELETE', '/records/r1', None), ('DELETE', '/records/r1?purge=1', None),
                ('POST', '/records/r1/restore', None),
                ('PUT', '/blobs/' + address, content), ('DELETE', '/blobs/' + address, None)):
            for protections in ({}, {'X-CSRF-Token': ''}, {'X-CSRF-Token': 'wrong'},
                                {'X-CSRF-Token': csrf, 'Origin': 'https://evil.example'},
                                {'X-CSRF-Token': csrf, 'Sec-Fetch-Site': 'cross-site'}):
                with self.subTest(method=method, suffix=suffix, protections=protections):
                    self.assertEqual(self.request(method, base + suffix, body, cookie=self.jar,
                        headers={'X-Total-Size': str(len(content)), **protections,
                        'Content-Type': 'application/octet-stream' if isinstance(body, bytes)
                        else 'application/json'})[0], 403)
        self.assertEqual(self.records(self.device, self.vid)['records'][0]['name'], 'stay')
        self.assertEqual(self.records(self.device, self.vid)['records'][0]['version'], 1)
        self.assertEqual(self.request('GET', base + '/blobs/' + address, cookie=self.jar)[2], content)

    def test_browser_record_metadata_cursor_conflict_trash_restore_and_purge(self):
        base = '/browser/vaults/%s/records' % self.vid
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        headers = {'X-CSRF-Token': csrf}
        body = {'base_version': 0, 'operation': 'browser-op1', 'name': 'report.pdf',
                'path': 'Work/Reports', 'tags': ['finance'], 'size': 123, 'package': 'a' * 64}
        status, _, value = self.request('PUT', base + '/r1', body, cookie=self.jar, headers=headers)
        self.assertEqual(status, 201, value)
        self.assertEqual((value['record']['path'], value['record']['tags'], value['record']['package']),
                         ('Work/Reports', ['finance'], 'a' * 64))
        self.assertEqual(self.request('PUT', base + '/r1', body, cookie=self.jar, headers=headers)[2], value)
        status, _, page = self.request('GET', base + '?limit=1', cookie=self.jar)
        self.assertEqual((status, page['records'], page['cursor']), (200, [value['record']], 1))
        self.assertEqual(self.request('GET', base + '?since=1', cookie=self.jar)[2]['records'], [])
        edited = {**body, 'base_version': 1, 'operation': 'browser-op2', 'name': 'renamed.pdf'}
        status, _, value = self.request('PUT', base + '/r1', edited, cookie=self.jar, headers=headers)
        self.assertEqual((status, value['record']['version']), (200, 2))
        stale = {**edited, 'operation': 'browser-stale'}
        status, _, value = self.request('PUT', base + '/r1', stale, cookie=self.jar, headers=headers)
        self.assertEqual((status, value['record']['name']), (409, 'renamed.pdf'))
        status, _, value = self.request('DELETE', base + '/r1', cookie=self.jar, headers=headers)
        self.assertEqual((status, value['record']['deleted'], value['record']['version']), (200, True, 3))
        tombstones = self.request('GET', base + '?since=2', cookie=self.jar)[2]
        self.assertEqual(tombstones['records'], [value['record']])
        status, _, value = self.request('POST', base + '/r1/restore', cookie=self.jar, headers=headers)
        self.assertEqual((status, value['record']['deleted'], value['record']['version']), (200, False, 4))
        self.assertEqual(self.records(self.device, self.vid)['records'], [value['record']])
        self.assertEqual(self.request('DELETE', base + '/r1?purge=1', cookie=self.jar, headers=headers)[0], 204)
        self.assertEqual(self.request('GET', base + '/r1', cookie=self.jar)[0], 404)
        self.assertEqual(self.request('GET', base, cookie=self.jar)[2]['records'], [])

    def test_corrupt_ciphertext_is_rejected_by_its_content_hash_and_can_be_retried(self):
        content = b'MVPK' + bytes(range(256)) * 10
        address = package(content)
        path = '/browser/vaults/%s/blobs/%s' % (self.vid, address)
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        headers = {'X-CSRF-Token': csrf, 'Content-Type': 'application/octet-stream',
                   'X-Total-Size': str(len(content))}
        damaged = content[:-1] + bytes([content[-1] ^ 1])
        status, _, value = self.request('PUT', path, damaged, cookie=self.jar, headers=headers)
        self.assertEqual((status, value), (422, {'error': 'hash mismatch'}))
        self.assertEqual(self.request('HEAD', path, cookie=self.jar)[0], 404)
        self.assertEqual(self.rows('SELECT * FROM blobs WHERE vault=?', (self.vid,)), [])
        self.assertFalse((self.data / 'blobs' / self.vid / (address + '.mvpk.part')).exists())
        self.assertEqual(self.request('PUT', path, content, cookie=self.jar, headers=headers)[0], 201)
        self.assertEqual(self.request('GET', path, cookie=self.jar)[2], content)

    def test_logout_immediately_revokes_browser_file_access(self):
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        self.assertEqual(self.request('POST', '/auth/logout', cookie=self.jar)[0], 200)
        for method, path, body in (
                ('GET', '/browser/capabilities', None),
                ('GET', '/browser/vaults/%s/records' % self.vid, None),
                ('PUT', '/browser/vaults/%s/records/r1' % self.vid,
                 {'name': 'no', 'base_version': 0, 'operation': 'ended'})):
            self.assertEqual(self.request(method, path, body, cookie=self.jar,
                                          headers={'X-CSRF-Token': csrf})[0], 401)
        self.assertEqual(self.records(self.device, self.vid)['records'], [])

    def test_duplicate_security_headers_are_refused(self):
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        path = '/browser/vaults/%s/records/r1' % self.vid
        origin = 'http://127.0.0.1:%d' % self.port
        for name, values in (('X-CSRF-Token', (csrf, csrf)),
                             ('Origin', (origin, 'https://evil.example')),
                             ('Sec-Fetch-Site', ('same-origin', 'cross-site'))):
            with self.subTest(header=name):
                conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=20)
                try:
                    conn.putrequest('DELETE', path)
                    conn.putheader('Cookie', 'mv_session=' + self.jar)
                    if name != 'X-CSRF-Token':
                        conn.putheader('X-CSRF-Token', csrf)
                    for value in values:
                        conn.putheader(name, value)
                    conn.endheaders()
                    response = conn.getresponse()
                    self.assertEqual(response.status, 403, response.read())
                finally:
                    conn.close()

    def test_browser_blob_limits_reuse_the_native_account_quota(self):
        self.stop()
        self.start('--quota-bytes', '10', '--max-file-bytes', '20')
        csrf = self.request('GET', '/auth/session', cookie=self.jar)[2]['csrf']
        path = '/browser/vaults/%s/blobs/%s' % (self.vid, package(b'ciphertext'))
        headers = {'X-CSRF-Token': csrf, 'Content-Type': 'application/octet-stream', 'X-Total-Size': '21'}
        self.assertEqual(self.request('PUT', path, b'x', cookie=self.jar, headers=headers)[0], 413)
        self.assertEqual(self.request('PUT', path, b'x', cookie=self.jar,
                                      headers={**headers, 'X-Total-Size': '11'})[0], 507)
        status, _, value = self.request('PUT', path, b'ciphertext', cookie=self.jar,
                                        headers={**headers, 'X-Total-Size': '10'})
        self.assertEqual((status, value), (201, {'offset': 10, 'complete': True}))
        self.assertEqual(self.vaults(self.jar)['quota']['bytes'], 10)
        self.assertEqual(self.put_blob(self.device, self.vid, b'other native ciphertext')[0], 413)
        self.assertEqual(self.put_blob(self.device, self.vid, b'x')[0], 507)
        self.assertEqual(self.request('DELETE', path, cookie=self.jar,
                                      headers={'X-CSRF-Token': csrf})[0], 204)
        self.assertEqual(self.put_blob(self.device, self.vid, b'x')[0], 201)

    def test_shipped_browser_crypto_and_model_scripts_are_fetchable(self):
        for path in ('/files-model.js', '/package-crypto.js', '/vendor/sodium.js', '/vendor/sodium-wrappers.js'):
            status, headers, body = self.request('GET', path)
            self.assertEqual(status, 200, path)
            self.assertEqual(headers['content-type'], 'text/javascript; charset=utf-8')
            self.assertTrue(body, path)


if __name__ == '__main__':
    unittest.main()
