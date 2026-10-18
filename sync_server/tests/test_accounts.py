"""Accounts, web sessions and device tokens, over real sockets."""
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import PASSWORD, ServerCase


class AccountTests(ServerCase):
    def test_register_creates_one_account_and_rejects_repeats(self):
        email = self.new_email()
        status, _, value = self.request('POST', '/auth/register', {'email': email, 'password': PASSWORD})
        self.assertEqual(status, 201, value)
        self.assertEqual(value['account']['email'], email)
        self.assertNotIn('password', value['account'])
        status, _, value = self.request('POST', '/auth/register', {'email': email.upper(), 'password': PASSWORD})
        self.assertEqual(status, 409, 'email is unique regardless of case')
        for bad in ({'email': 'not-an-email', 'password': PASSWORD},
                    {'email': 'a@b', 'password': PASSWORD},
                    {'email': self.new_email(), 'password': 'short'},
                    {'email': self.new_email()},
                    {}):
            self.assertEqual(self.request('POST', '/auth/register', bad)[0], 400, bad)
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM accounts')[0]['n'], 1)

    def test_password_is_salted_scrypt_and_never_stored(self):
        self.register(password='identical password one')
        self.register(password='identical password one')
        stored = self.rows('SELECT email,salt,digest FROM accounts')
        self.assertEqual(len(stored), 2)
        self.assertNotEqual(stored[0]['salt'], stored[1]['salt'], 'each account needs its own salt')
        self.assertNotEqual(stored[0]['digest'], stored[1]['digest'], 'same password must not collide')
        for row in stored:
            self.assertEqual(len(row['salt']), 16)
            self.assertEqual(len(row['digest']), 32)
            expected = hashlib.scrypt(b'identical password one', salt=row['salt'], n=16384, r=8, p=1, dklen=32)
            self.assertEqual(row['digest'], expected, 'stored value must be scrypt(password, salt)')
        for path in self.data.rglob('*'):
            if path.is_file():
                self.assertNotIn(b'identical password one', path.read_bytes(),
                                 'plaintext password on disk: ' + path.name)

    def test_login_wrong_password_and_unknown_email_are_rejected(self):
        account, _ = self.register()
        status, headers, value = self.request('POST', '/auth/login',
                                              {'email': account['email'], 'password': 'wrong password here'})
        self.assertEqual((status, value), (401, {'error': 'invalid email or password'}))
        self.assertNotIn('set-cookie', headers)
        self.assertEqual(self.request('POST', '/auth/login',
                                      {'email': self.new_email(), 'password': PASSWORD})[0], 401)
        self.assertEqual(self.request('POST', '/auth/login', {'email': account['email']})[0], 400)
        self.assertEqual(self.request('POST', '/auth/login', {}, headers={'Content-Type': 'text/plain'})[0], 415)

    def test_session_cookie_is_httponly_lax_and_authorises_web_routes(self):
        account, password = self.register()
        status, headers, value = self.request('POST', '/auth/login',
                                              {'email': account['email'], 'password': password})
        self.assertEqual(status, 200)
        raw = headers['set-cookie']
        self.assertIn('HttpOnly', raw)
        self.assertIn('SameSite=Lax', raw)
        self.assertIn('Path=/', raw)
        self.assertNotIn('Secure', raw, 'plain HTTP on loopback must not set Secure or the browser drops it')
        cookie = self.cookie(headers)
        self.assertGreaterEqual(len(cookie), 32)
        status, _, value = self.request('GET', '/auth/session', cookie=cookie)
        self.assertEqual((status, value['account']['email']), (200, account['email']))
        status, _, value = self.request('GET', '/v1/vaults', cookie=cookie)
        self.assertEqual((status, value['vaults']), (200, []))
        self.assertEqual(self.request('GET', '/v1/vaults')[0], 401)
        self.assertEqual(self.request('GET', '/v1/vaults', cookie='f' * 43)[0], 401)
        self.assertEqual(self.request('GET', '/v1/vaults', cookie=cookie + 'x')[0], 401)
        # the cookie token itself is not in the database, only its digest
        stored = [row['id'] for row in self.rows('SELECT id FROM sessions')]
        self.assertEqual(stored, [hashlib.sha256(cookie.encode()).hexdigest()])
        self.assertNotIn(cookie, str(stored))

    def test_logout_ends_only_that_session(self):
        cookie, account = self.user()
        other, _ = self.login(account['email'])
        self.assertEqual(self.request('POST', '/auth/logout', {}, cookie=cookie)[0], 200)
        self.assertEqual(self.request('GET', '/v1/vaults', cookie=cookie)[0], 401)
        self.assertEqual(self.request('GET', '/v1/vaults', cookie=other)[0], 200)
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM sessions')[0]['n'], 1)

    def test_device_token_is_issued_hashed_and_revocable(self):
        cookie, _ = self.user()
        issued = self.token_for(cookie, 'Laptop')
        token = issued['token']
        self.assertRegex(token, r'^[0-9a-f]{64}$')
        self.assertEqual(issued['id'], hashlib.sha256(token.encode()).hexdigest())
        listed = self.request('GET', '/auth/token', cookie=cookie)[2]['tokens']
        self.assertEqual([(row['id'], row['label'], row['revoked']) for row in listed],
                         [(issued['id'], 'Laptop', 0)])
        self.assertNotIn(token, str(listed), 'the token must never be listed again')
        self.assertEqual(self.request('GET', '/auth/token', token=token)[0], 200)
        status, _, value = self.request('POST', '/v1/vaults', {'name': 'From a device'}, token=token)
        self.assertEqual(status, 201, value)
        self.assertEqual(self.request('GET', '/v1/vaults', token=token)[2]['vaults'][0]['name'], 'From a device')
        self.assertEqual(self.request('DELETE', '/auth/token/' + issued['id'], cookie=cookie)[0], 200)
        self.assertEqual(self.request('GET', '/v1/vaults', token=token)[0], 401, 'revoked token must stop working')
        self.assertEqual(self.request('GET', '/auth/token', token=token)[0], 401)
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM tokens WHERE revoked=1')[0]['n'], 1)
        self.assertEqual(self.request('DELETE', '/auth/token/' + 'a' * 64, cookie=cookie)[0], 404)
        self.assertEqual(self.request('GET', '/auth/token', cookie=cookie)[2]['tokens'][0]['revoked'], 1)

    def test_token_issue_needs_a_login(self):
        self.assertEqual(self.request('POST', '/auth/token', {'label': 'x'})[0], 401)
        cookie, _ = self.user()
        self.assertEqual(self.request('POST', '/auth/token', {'label': 'x' * 61}, cookie=cookie)[0], 400)
        self.assertEqual(self.request('POST', '/auth/token', {}, headers={'Origin': 'https://evil.example',
                                                                          'Cookie': 'mv_session=' + cookie})[0], 403)

    def test_operator_cli_creates_accounts_lists_vaults_and_issues_tokens(self):
        email = self.new_email()
        created = self.cli('account', 'create', email, '--password-stdin', stdin='cli password 123\n')
        self.assertEqual(created.returncode, 0, created.stderr)
        values = dict(line.split(': ', 1) for line in created.stdout.strip().splitlines())
        self.assertEqual(values['email'], email)
        self.assertNotIn('cli password 123', created.stdout + created.stderr)
        again = self.cli('account', 'create', email, '--password-stdin', stdin='cli password 123\n')
        self.assertNotEqual(again.returncode, 0)

        cookie, _ = self.login(email, 'cli password 123')
        vault = self.vault(cookie, 'CLI vault')
        vaults = self.cli('vault', 'list', email)
        self.assertEqual(vaults.returncode, 0, vaults.stderr)
        self.assertIn('%s\tCLI vault\t0\t0' % vault['id'], vaults.stdout)

        issued = self.cli('token', 'issue', email, '--label', 'cli-device')
        self.assertEqual(issued.returncode, 0, issued.stderr)
        token = dict(line.split(': ', 1) for line in issued.stdout.strip().splitlines())['token']
        status, _, value = self.request('GET', '/v1/vaults', token=token)
        self.assertEqual(status, 200, value)
        self.assertEqual([row['name'] for row in value['vaults']], ['CLI vault'])
        self.assertIn(issued.stdout.strip().splitlines()[0].split(': ')[1], issued.stdout)

    def test_one_account_cannot_reach_another(self):
        first_cookie, first = self.user()
        second_cookie, second = self.user()
        vault = self.vault(first_cookie, 'Private')
        second_token = self.token_for(second_cookie)['token']
        self.assertEqual(self.vaults(second_cookie)['vaults'], [])
        self.assertEqual(self.request('GET', '/v1/vaults/' + vault['id'], token=second_token)[0], 404)
        self.assertEqual(self.request('GET', '/v1/vaults/%s/records' % vault['id'], token=second_token)[0], 404)
        self.assertEqual(self.request('PATCH', '/v1/vaults/' + vault['id'], {'name': 'mine now'},
                                      token=second_token)[0], 404)
        self.assertEqual(self.request('DELETE', '/v1/vaults/' + vault['id'], token=second_token)[0], 404)
        self.assertEqual(self.put_blob(second_token, vault['id'], b'trespass')[0], 404)
        self.assertEqual(self.request('GET', '/v1/vaults/' + vault['id'], cookie=first_cookie)[2]['vault']['name'],
                         'Private')


if __name__ == '__main__':
    unittest.main()
