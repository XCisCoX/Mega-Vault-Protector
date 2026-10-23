"""The console's account and device actions: password, sessions, tokens, delete.

These routes exist for the web UI, so each one is exercised over a real socket
with a real cookie, and the database is read back where the effect matters.
"""
from pathlib import Path
import secrets
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import PASSWORD, ServerCase, package


class ConsoleAccountTests(ServerCase):
    def setUp(self):
        super().setUp()
        self.jar, self.account = self.user()

    def login_again(self):
        cookie, _ = self.login(self.account['email'])
        return cookie

    def test_password_change_requires_the_current_password(self):
        status, _, value = self.request('POST', '/auth/password', {
            'current_password': 'not-the-password', 'new_password': 'a-long-new-password'},
            cookie=self.jar)
        self.assertEqual(status, 403, value)
        self.assertIn('current password', value['error'])

        status, _, value = self.request('POST', '/auth/password', {
            'current_password': PASSWORD, 'new_password': 'short'}, cookie=self.jar)
        self.assertEqual(status, 400, value)

        status, _, value = self.request('POST', '/auth/password', {
            'current_password': PASSWORD, 'new_password': PASSWORD}, cookie=self.jar)
        self.assertEqual(status, 400, value)

        # Nothing changed: the original password still opens a session.
        status, _, _ = self.request('POST', '/auth/login',
                                    {'email': self.account['email'], 'password': PASSWORD})
        self.assertEqual(status, 200)

    def test_password_change_keeps_this_session_and_ends_the_others(self):
        other = self.login_again()
        fresh = 'another-long-password'
        status, _, value = self.request('POST', '/auth/password', {
            'current_password': PASSWORD, 'new_password': fresh}, cookie=self.jar)
        self.assertEqual(status, 200, value)
        self.assertEqual(value['sessions_ended'], 1)

        self.assertEqual(self.request('GET', '/auth/session', cookie=self.jar)[0], 200)
        self.assertEqual(self.request('GET', '/auth/session', cookie=other)[0], 401)
        self.assertEqual(self.request('POST', '/auth/login',
                                      {'email': self.account['email'], 'password': PASSWORD})[0], 401)
        self.assertEqual(self.request('POST', '/auth/login',
                                      {'email': self.account['email'], 'password': fresh})[0], 200)

        # The stored credential is a scrypt digest of the new password only.
        stored = self.rows('SELECT salt,digest FROM accounts WHERE id=?', (self.account['id'],))[0]
        self.assertNotIn(fresh.encode(), bytes(stored['digest']))
        self.assertEqual(len(stored['digest']), 32)

    def test_sign_out_everywhere_ends_every_session(self):
        other = self.login_again()
        status, headers, value = self.request('DELETE', '/auth/sessions', cookie=self.jar)
        self.assertEqual(status, 200, value)
        self.assertEqual(value['sessions_ended'], 2)
        self.assertIn('max-age=0', headers['set-cookie'].lower())
        self.assertEqual(self.request('GET', '/auth/session', cookie=self.jar)[0], 401)
        self.assertEqual(self.request('GET', '/auth/session', cookie=other)[0], 401)
        self.assertEqual(self.rows('SELECT * FROM sessions WHERE account=?',
                                   (self.account['id'],)), [])

    def test_token_can_be_renamed_but_not_someone_elses(self):
        issued = self.token_for(self.jar, 'Laptop')
        status, _, value = self.request('PATCH', '/auth/token/' + issued['id'],
                                        {'label': 'Kitchen laptop'}, cookie=self.jar)
        self.assertEqual(status, 200, value)
        self.assertEqual(value['token']['label'], 'Kitchen laptop')
        labels = {token['id']: token['label'] for token in
                  self.request('GET', '/auth/token', cookie=self.jar)[2]['tokens']}
        self.assertEqual(labels[issued['id']], 'Kitchen laptop')

        # The device token still works after its label changes.
        status, _, _ = self.request('GET', '/v1/vaults',
                                    headers={'Authorization': 'Bearer ' + issued['token']})
        self.assertEqual(status, 200)

        status, _, value = self.request('PATCH', '/auth/token/' + 'ab' * 32,
                                        {'label': 'nope'}, cookie=self.jar)
        self.assertEqual(status, 404, value)
        status, _, value = self.request('PATCH', '/auth/token/' + issued['id'],
                                        {'label': ''}, cookie=self.jar)
        self.assertEqual(status, 400, value)

    def test_cross_origin_requests_are_refused_on_the_new_routes(self):
        for method, path, body in (('POST', '/auth/password',
                                    {'current_password': PASSWORD, 'new_password': 'x' * 12}),
                                   ('DELETE', '/auth/sessions', None),
                                   ('DELETE', '/auth/account', {'password': PASSWORD})):
            status, _, value = self.request(method, path, body, cookie=self.jar,
                                            headers={'Origin': 'https://evil.example'})
            self.assertEqual(status, 403, (path, value))

    def test_deleting_the_account_removes_vaults_records_and_packages(self):
        vault = self.vault(self.jar, 'Family')['id']
        token = self.token_for(self.jar)['token']
        content = b'MVPK' + secrets.token_bytes(2048)
        address = package(content)
        self.put_blob(token, vault, content)
        self.put_record(token, vault, 'rec-1', 0, package=address, size=len(content))
        blob = self.data / 'blobs' / vault / (address + '.mvpk')
        self.assertTrue(blob.is_file())

        status, _, value = self.request('DELETE', '/auth/account',
                                        {'password': 'wrong-password'}, cookie=self.jar)
        self.assertEqual(status, 403, value)

        status, headers, value = self.request('DELETE', '/auth/account',
                                              {'password': PASSWORD}, cookie=self.jar)
        self.assertEqual(status, 200, value)
        self.assertEqual(value['vaults_removed'], 1)
        self.assertIn('max-age=0', headers['set-cookie'].lower())

        self.assertEqual(self.rows('SELECT * FROM accounts'), [])
        self.assertEqual(self.rows('SELECT * FROM vaults'), [])
        self.assertEqual(self.rows('SELECT * FROM records'), [])
        self.assertEqual(self.rows('SELECT * FROM blobs'), [])
        self.assertEqual(self.rows('SELECT * FROM tokens'), [])
        self.assertFalse(blob.exists(), 'the package outlived its account')
        self.assertFalse((self.data / 'blobs' / vault).exists())

        # The old device token and the old cookie are both useless now.
        self.assertEqual(self.request('GET', '/v1/vaults',
                                      headers={'Authorization': 'Bearer ' + token})[0], 401)
        self.assertEqual(self.request('GET', '/auth/session', cookie=self.jar)[0], 401)
        # The address can be registered again.
        self.assertEqual(self.request('POST', '/auth/register',
                                      {'email': self.account['email'],
                                       'password': 'brand-new-password'})[0], 201)


    def test_registration_can_be_closed_for_a_public_host(self):
        """A deployed relay must not accept self-service signup."""
        self.assertEqual(self.request('GET', '/healthz')[2]['registration'], 'open')
        self.stop()
        self.start('--registration', 'closed')
        self.assertEqual(self.request('GET', '/healthz')[2]['registration'], 'closed')
        status, _, value = self.request('POST', '/auth/register',
                                        {'email': 'stranger@example.com',
                                         'password': 'long-enough-password'})
        self.assertEqual(status, 403, value)
        self.assertIn('does not accept new accounts', value['error'])
        self.assertEqual(self.rows("SELECT * FROM accounts WHERE email='stranger@example.com'"), [])
        # The existing account still signs in: only signup is closed.
        status, _, _ = self.request('POST', '/auth/login',
                                    {'email': self.account['email'], 'password': PASSWORD})
        self.assertEqual(status, 200)


if __name__ == '__main__':
    unittest.main()
