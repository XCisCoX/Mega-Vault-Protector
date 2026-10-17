"""Run the native protocol 4 sync client against a throwaway real sync_server.

    python qt-app/tests/run_sync_live.py <VideoVaultSyncEngineTests executable>

The harness provisions everything through the server's own CLI -- an account,
one device token, two vaults -- serves the data directory on a free loopback
port, hands the native test the URL, the token and the account credentials, and
then reads the server's SQLite database back to prove what is actually on disk.
Nothing is mocked and no real credentials are used or printed.
"""

import argparse
import hashlib
import os
from contextlib import closing
from pathlib import Path
import re
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

TOKEN = re.compile(r'^token:\s*([0-9a-f]{64})\s*$', re.MULTILINE)
ACCOUNT = re.compile(r'^account:\s*([0-9a-f]{32})\s*$', re.MULTILINE)
EMAIL = 'owner@example.com'
PASSWORD = 'account-password-for-the-native-test'


def server(root, data, *argv, stdin=None):
    """Run one sync_server CLI command and return its stdout."""
    return subprocess.run(
        [sys.executable, '-m', 'sync_server', '--data', str(data), *argv],
        cwd=root, input=stdin, capture_output=True, text=True, check=True).stdout


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def call(url, token, method, path, body=None):
    request = urllib.request.Request(
        url + path, method=method,
        data=None if body is None else body.encode('utf-8'),
        headers={'Authorization': 'Bearer ' + token,
                 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=10) as response:
        return response.status, response.read().decode('utf-8')


def wait_for_health(url, process, timeout=20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url + '/healthz', timeout=1) as response:
                if response.status == 200:
                    return
        except OSError:
            if process.poll() is not None:
                raise RuntimeError('the server exited during startup')
            time.sleep(0.05)
    raise RuntimeError('the server did not answer /healthz in time')


def provision(root, data):
    """Create the account and one device token with the operator CLI."""
    created = server(root, data, 'account', 'create', EMAIL, '--password-stdin',
                     stdin=PASSWORD + '\n')
    account = ACCOUNT.search(created)
    vaults = server(root, data, 'token', 'issue', EMAIL, '--label', 'native-test')
    token = TOKEN.search(vaults)
    if account is None or token is None:
        raise RuntimeError('the CLI printed no usable account or token')
    return account.group(1), token.group(1)


def verify(data, vault_ids):
    """Prove the server holds complete, correctly named opaque packages."""
    data = Path(data)
    with closing(sqlite3.connect(data / 'sync.sqlite3')) as db:
        db.row_factory = sqlite3.Row
        vaults = db.execute('SELECT id, name FROM vaults ORDER BY created').fetchall()
        records = db.execute(
            'SELECT vault, id, version, name, package, size, deleted FROM records').fetchall()
        blobs = {(row['vault'], row['name']): row for row in
                 db.execute('SELECT vault, name, size, offset, complete FROM blobs')}
        tokens = db.execute('SELECT id, revoked FROM tokens').fetchall()

    assert [row['id'] for row in vaults] == vault_ids, \
        f'the account holds {[row["id"] for row in vaults]}, expected the two the harness made'
    assert records, 'no records were written'
    assert tokens and all(row['revoked'] == 0 for row in tokens), 'the device token was revoked'

    complete = 0
    for row in records:
        if row['package'] is None:
            continue
        key = (row['vault'], row['package'] + '.mvpk')
        stored = blobs.get(key)
        assert stored is not None, f'record {row["id"]} points at a package the server does not have'
        assert stored['complete'] == 1 and stored['offset'] == stored['size'], \
            f'{key[1]} is not complete'
        assert stored['size'] == row['size'], f'{key[1]} size does not match the record'
        body = (data / 'blobs' / row['vault'] / key[1]).read_bytes()
        assert len(body) == stored['size'], f'{key[1]} is the wrong length on disk'
        assert hashlib.sha256(body).hexdigest() + '.mvpk' == key[1], \
            f'{key[1]} is not named by the sha256 of its own bytes'
        assert body[:4] == b'MVPK', f'{key[1]} is not a MegaVault package'
        complete += 1

    leftovers = [str(path) for path in (data / 'blobs').rglob('*.part')]
    assert not leftovers, f'partial uploads left behind: {leftovers}'
    deleted = sum(1 for row in records if row['deleted'])
    return len(records), complete, deleted, len(blobs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--scratch', default=os.environ.get('TMPDIR') or None,
                        help='parent directory for the temporary server data')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='native-sync-live-', dir=args.scratch) as data:
        account, token = provision(root, data)
        print(f'account {account} created through the CLI; one device token issued '
              f'(stored server-side only as a sha256)')
        port = free_port()
        process = subprocess.Popen(
            [sys.executable, '-m', 'sync_server', '--data', str(data), 'serve',
             '--host', '127.0.0.1', '--port', str(port)],
            cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            url = f'http://127.0.0.1:{port}'
            wait_for_health(url, process)
            vault_ids = []
            for name in ('Vault A', 'Vault B'):
                status, body = call(url, token, 'POST', '/v1/vaults',
                                    '{"name":"%s"}' % name)
                assert status == 201, f'vault create answered {status}: {body}'
                vault_ids.append(re.search(r'"id":"([0-9a-f]{32})"', body).group(1))
            print(f'serving {url}; vaults {vault_ids[0]} and {vault_ids[1]} created with the token')

            result = subprocess.run(
                [str(Path(args.executable).resolve()), url, token, EMAIL, PASSWORD,
                 vault_ids[0], vault_ids[1]],
                cwd=root, capture_output=True, text=True, timeout=600)
            print(result.stdout, end='')
            if result.returncode:
                print(result.stderr, file=sys.stderr, end='')
                return result.returncode
            records, packages, deleted, rows = verify(data, vault_ids)
            print(f'server read-back verified: {records} records, {packages} complete packages, '
                  f'{rows} blob rows, {deleted} tombstone(s), each package named by the sha256 '
                  f'of its own bytes, no partial uploads')
            return 0
        finally:
            process.terminate()
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=10)


if __name__ == '__main__':
    raise SystemExit(main())
