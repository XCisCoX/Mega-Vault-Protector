"""Exercise the actual native client against a disposable real Python relay.
Usage: python qt-app/tests/run_sync_live.py <VideoVaultSyncEngineTests executable>
The executable uses disposable test keys; no real credentials are read or logged.
"""
import argparse
from contextlib import closing
import hashlib
import os
from pathlib import Path
import secrets
import socket
import ssl
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--tls', action='store_true', help='test self-signed TLS rejection, wrong pin rejection and exact pin trust; requires openssl')
    parser.add_argument('--scratch', default=os.environ.get('HERMES_SCRATCH_DIR', str(Path.home() / 'AppData/Local/hermes/cache/scratch') if os.name == 'nt' else None))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='native-sync-live-', dir=args.scratch) as data:
        command = [sys.executable, '-m', 'sync_server', '--data', data]
        codes = [subprocess.check_output(command + ['pair-code'], cwd=root, text=True).strip() for _ in range(2)]
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        tls_args = []
        context = None
        env = os.environ.copy()
        env.pop('MV_SYNC_TEST_PIN', None)
        if args.tls:
            cert, key = Path(data) / 'cert.pem', Path(data) / 'key.pem'
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1', '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
            env['MV_SYNC_TEST_PIN'] = hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert.read_text()).encode('latin1') if isinstance(ssl.PEM_cert_to_DER_cert(cert.read_text()), str) else ssl.PEM_cert_to_DER_cert(cert.read_text())).hexdigest()
            context = ssl.create_default_context(cafile=str(cert))
            tls_args = ['--cert', str(cert), '--key', str(key)]
        server = subprocess.Popen(command + ['serve', '--host', '127.0.0.1', '--port', str(port)] + tls_args, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            url = f'{"https" if args.tls else "http"}://127.0.0.1:{port}'
            for _ in range(100):
                try:
                    with urllib.request.urlopen(url + '/healthz', timeout=1, context=context) as response:
                        if response.status == 200:
                            break
                except OSError:
                    if server.poll() is not None:
                        raise RuntimeError('relay failed to start')
                    time.sleep(0.05)
            else:
                raise RuntimeError('relay startup timeout')
            result = subprocess.run([str(Path(args.executable).resolve()), url, codes[0], secrets.token_hex(32), codes[1]], cwd=root, env=env, capture_output=True, text=True, timeout=120)
            print(result.stdout, end='')
            if result.returncode:
                print(result.stderr, file=sys.stderr)
                return result.returncode
            with closing(sqlite3.connect(Path(data) / 'sync.sqlite3')) as db:
                devices = db.execute('SELECT COUNT(*) FROM devices').fetchone()[0]
                versions = db.execute('SELECT COUNT(*) FROM versions').fetchone()[0]
                objects = db.execute('SELECT COUNT(DISTINCT id) FROM versions').fetchone()[0]
                assert devices == 2, devices
                assert objects == 2, objects
                assert versions == 6, versions
            print('relay read-back verified: two devices, two opaque objects, six immutable revisions including a tombstone')
            return 0
        finally:
            server.terminate()
            server.communicate(timeout=10)


if __name__ == '__main__':
    raise SystemExit(main())
