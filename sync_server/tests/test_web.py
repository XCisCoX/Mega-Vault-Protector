"""The static web UI and the cookie-authenticated routes it drives."""
from html.parser import HTMLParser
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import PASSWORD, ServerCase


class WebTests(ServerCase):
    def test_the_page_is_served_from_the_same_process_with_a_strict_policy(self):
        status, headers, body = self.request('GET', '/')
        self.assertEqual(status, 200)
        self.assertTrue(headers['content-type'].startswith('text/html'))
        self.assertIn("frame-ancestors 'none'", headers['content-security-policy'])
        self.assertNotIn("unsafe-inline", headers['content-security-policy'])
        self.assertEqual(headers['x-frame-options'], 'DENY')
        self.assertEqual(headers['x-content-type-options'], 'nosniff')
        html = body.decode()
        class Page(HTMLParser):
            def __init__(self):
                super().__init__()
                self.elements = []

            def handle_starttag(self, tag, attrs):
                self.elements.append((tag, dict(attrs)))

        page = Page()
        page.feed(html)
        ids = {attrs['id']: (tag, attrs) for tag, attrs in page.elements if 'id' in attrs}
        for marker in ('register-form', 'login-form', 'token-form', 'server-url',
                       'library-select', 'create-library', 'file-picker', 'file-list',
                       'lock-library', 'preview-dialog', 'transfer-tray'):
            self.assertIn(marker, ids)
        self.assertIn('hidden', ids['app'][1], 'the file shell starts signed out')
        self.assertEqual(ids['file-picker'][1]['type'], 'file')
        self.assertEqual(ids['preview-dialog'][0], 'dialog')
        scripts = [attrs for tag, attrs in page.elements if tag == 'script']
        self.assertEqual([attrs.get('src') for attrs in scripts], [
            '/vendor/sodium.js', '/vendor/sodium-wrappers.js', '/package-crypto.js',
            '/files-model.js', '/app.js'])
        self.assertTrue(all('defer' in attrs for attrs in scripts))
        self.assertNotIn('<script>', html, 'no inline script may sneak past the CSP')
        for _, attrs in page.elements:
            self.assertFalse(any(name.lower().startswith('on') for name in attrs),
                             'inline event handlers violate the CSP')
        for asset, kind in (('/app.js', 'text/javascript'), ('/style.css', 'text/css'), ('/icon.svg', 'image/svg')):
            status, headers, body = self.request('GET', asset)
            self.assertEqual(status, 200, asset)
            self.assertTrue(headers['content-type'].startswith(kind), asset)
            self.assertTrue(body)

    def test_unknown_paths_and_methods_on_the_static_site(self):
        for path in ('/nope', '/../sync_server/__main__.py', '/index.html/../sync.sqlite3', '/v3/files'):
            self.assertEqual(self.request('GET', path)[0], 404, path)
        self.assertEqual(self.request('POST', '/')[0], 405)
        self.assertEqual(self.request('GET', '/healthz')[2], {'ok': True, 'protocol': 4, 'registration': 'open'})

    def test_file_ui_uses_session_browser_routes_and_only_persists_theme(self):
        script = self.request('GET', '/app.js')[2].decode()
        for route in ('/v1/vaults', '/auth/token', '/browser/vaults/', '/records', '/blobs/'):
            self.assertIn(route, script)
        for forbidden in ('/v3/files', 'Authorization', 'sessionStorage', 'indexedDB',
                          'document.cookie', 'innerHTML', 'outerHTML', 'insertAdjacentHTML'):
            self.assertNotIn(forbidden, script)
        # Appearance may persist, never a password, token, decrypted file or record.
        accesses = re.findall(r'\blocalStorage\s*\.\s*(\w+)\s*\(([^)]*)\)', script)
        self.assertEqual(accesses, [('setItem', "'mvp-theme',e.target.value"),
                                    ('getItem', "'mvp-theme'")])
        self.assertEqual(len(re.findall(r'\blocalStorage\b', script)), len(accesses),
                         'no unreviewed storage API or bracket access')
        for asset in ('/files-model.js', '/package-crypto.js'):
            code = self.request('GET', asset)[2].decode()
            for forbidden in ('localStorage', 'sessionStorage', 'indexedDB'):
                self.assertNotIn(forbidden, code, asset)

    def test_the_whole_web_flow_over_the_session_cookie(self):
        email = self.new_email()
        # register from the form, then log in
        self.assertEqual(self.request('POST', '/auth/register', {'email': email, 'password': PASSWORD})[0], 201)
        status, headers, value = self.request('POST', '/auth/login', {'email': email, 'password': PASSWORD})
        self.assertEqual(status, 200, value)
        cookie = self.cookie(headers)
        self.assertEqual(self.request('GET', '/auth/session', cookie=cookie)[2]['account']['email'], email)

        # create, list
        vault = self.vault(cookie, 'Website vault')
        self.assertEqual(self.vaults(cookie)['vaults'][0]['id'], vault['id'])
        self.assertEqual(self.vaults(cookie)['quota'], {'bytes': 0, 'limit': 10 * 1024**3})

        # rename
        status, _, value = self.request('PATCH', '/v1/vaults/' + vault['id'], {'name': 'Renamed'}, cookie=cookie)
        self.assertEqual((status, value['vault']['name']), (200, 'Renamed'))
        self.assertEqual(self.request('PATCH', '/v1/vaults/' + vault['id'], {'name': '  '}, cookie=cookie)[0], 400)

        # issue a token, then revoke it from the list
        issued = self.token_for(cookie, 'Pixel')
        self.assertEqual(self.request('GET', '/v1/vaults', token=issued['token'])[0], 200)
        listed = self.request('GET', '/auth/token', cookie=cookie)[2]['tokens']
        self.assertEqual([(row['id'], row['label']) for row in listed], [(issued['id'], 'Pixel')])
        self.assertEqual(self.request('DELETE', '/auth/token/' + issued['id'], cookie=cookie)[0], 200)
        self.assertEqual(self.request('GET', '/v1/vaults', token=issued['token'])[0], 401)
        self.assertEqual(self.request('GET', '/auth/token', cookie=cookie)[2]['tokens'][0]['revoked'], 1)

        # delete the vault, then log out
        self.assertEqual(self.request('DELETE', '/v1/vaults/' + vault['id'], cookie=cookie)[0], 204)
        self.assertEqual(self.vaults(cookie)['vaults'], [])
        self.assertEqual(self.request('POST', '/auth/logout', {}, cookie=cookie)[0], 200)
        self.assertEqual(self.request('GET', '/auth/session', cookie=cookie)[0], 401)
        self.assertEqual(self.request('GET', '/', cookie=cookie)[0], 200, 'the page itself stays public')

    def test_cookie_writes_are_closed_to_other_origins_and_plain_bodies(self):
        cookie, _ = self.user()
        evil = {'Origin': 'https://evil.example', 'Cookie': 'mv_session=' + cookie}
        self.assertEqual(self.request('POST', '/v1/vaults', {'name': 'x'}, headers=evil)[0], 403)
        self.assertEqual(self.request('DELETE', '/auth/token/' + 'a' * 64, headers=evil)[0], 403)
        self.assertEqual(self.request('POST', '/auth/logout', {}, headers=evil)[0], 403)
        self.assertEqual(self.vaults(cookie)['vaults'], [])
        # a form-shaped body cannot be sent cross-site without a preflight
        status, _, _ = self.request('POST', '/v1/vaults', b'name=x',
                                    headers={'Content-Type': 'application/x-www-form-urlencoded',
                                             'Cookie': 'mv_session=' + cookie})
        self.assertEqual(status, 415)
        # same-origin writes still work
        same = {'Origin': 'http://127.0.0.1:%d' % self.port, 'Cookie': 'mv_session=' + cookie}
        self.assertEqual(self.request('POST', '/v1/vaults', {'name': 'fine'}, headers=same)[0], 201)


if __name__ == '__main__':
    unittest.main()
