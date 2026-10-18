"""mvpk sync server: accounts, vaults, versioned records and encrypted packages.

Protocol 4.  The server never holds a key, a file name or a byte of plaintext:
a package is named by the SHA-256 of its own ciphertext and a record is a small
metadata row that the client wrote.  See ``docs/sync-mega-plan.md``.

    python -m sync_server --data DIR serve
    python -m sync_server --data DIR account create owner@example.com
"""
import argparse
import getpass
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import ssl
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

from .store import (CHUNK_BYTES, HEX64, PROTOCOL, RECORD_ID, SESSION_SECONDS, VAULT_ID,
                    Problem, Store, digest)

COOKIE = 'mv_session'
CSP = ("default-src 'self'; img-src 'self'; style-src 'self'; script-src 'self'; "
       "connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
DIGITS = re.compile(r'[0-9]{1,19}')
EMAIL = re.compile(r'[^@\s]{1,64}@[^@\s.]{1,63}(\.[^@\s.]{1,63})+')
OPERATION = re.compile(r'[A-Za-z0-9._:-]{1,128}')
STATIC = {
    '/': ('index.html', 'text/html; charset=utf-8'),
    '/index.html': ('index.html', 'text/html; charset=utf-8'),
    '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
    '/style.css': ('style.css', 'text/css; charset=utf-8'),
    '/icon.svg': ('icon.svg', 'image/svg+xml'),
}


def integer(value):
    """Strict non-negative integer, or None.  Rejects booleans, signs, spaces."""
    return int(value) if isinstance(value, str) and DIGITS.fullmatch(value) else None


class Server(ThreadingHTTPServer):
    """Bounded workers; one slow client cannot spawn threads without limit."""

    daemon_threads = True
    request_queue_size = 32

    def __init__(self, address, store, args, context, static):
        self.store = store
        self.context = context
        self.static = static
        self.quota_bytes = args.quota_bytes
        self.max_file_bytes = args.max_file_bytes
        self.request_timeout = args.request_timeout
        self.slots = threading.BoundedSemaphore(args.workers)
        super().__init__(address, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        def expire():
            try:
                request.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        timer = threading.Timer(self.request_timeout, expire)
        timer.daemon = True
        request.settimeout(self.request_timeout)
        try:
            if self.context:
                request = self.context.wrap_socket(request, server_side=True, do_handshake_on_connect=False)
            timer.start()
            if self.context:
                request.do_handshake()
            self.finish_request(request, address)
        except (OSError, ValueError):
            pass
        finally:
            timer.cancel()
            self.shutdown_request(request)
            self.slots.release()

    def handle_error(self, request, client_address):
        # Deliberately omit exception, path and header material from logs.
        pass


class Handler(BaseHTTPRequestHandler):
    server_version = 'mvpk-sync/4'
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def version_string(self):
        return self.server_version  # do not advertise the interpreter version

    @property
    def store(self):
        return self.server.store

    # -- plumbing ---------------------------------------------------------

    def reply(self, status, value, headers=()):
        payload = json.dumps(value, separators=(',', ':')).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.send_header('Cache-Control', 'no-store')
        for name, header in headers:
            self.send_header(name, header)
        if self.close_connection:
            self.send_header('Connection', 'close')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(payload)

    def empty(self, status):
        self.send_response(status)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def read_chunk(self, limit):
        if self.headers.get('Transfer-Encoding') or len(self.headers.get_all('Content-Length', [])) != 1:
            self.close_connection = True
            raise Problem(400, {'error': 'one Content-Length required'})
        length = integer(self.headers['Content-Length'])
        if length is None:
            raise Problem(400, {'error': 'invalid Content-Length'})
        if length > limit:
            self.close_connection = True
            raise Problem(413, {'error': 'chunk too large'})
        data = self.rfile.read(length)
        if len(data) != length:
            self.close_connection = True
            raise Problem(400, {'error': 'incomplete body'})
        return data

    def json_body(self, limit=65536):
        kind = self.headers.get('Content-Type', '').split(';')[0].strip().lower()
        if kind != 'application/json':
            # A form or text/plain body cannot be sent cross-site without a
            # preflight, so this also closes the CSRF door on cookie writes.
            raise Problem(415, {'error': 'application/json required'})
        data = self.read_chunk(limit)
        try:
            value = json.loads(data)
        except ValueError:
            raise Problem(400, {'error': 'invalid JSON'})
        if not isinstance(value, dict):
            raise Problem(400, {'error': 'JSON object required'})
        return value

    def secure(self):
        return bool(self.server.context) or self.headers.get('X-Forwarded-Proto', '').lower() == 'https'

    def cookies(self):
        jar = {}
        for header in self.headers.get_all('Cookie', []):
            for part in header.split(';'):
                name, _, value = part.strip().partition('=')
                if name:
                    jar[name] = value
        return jar

    def cookie_header(self, token, age):
        return '%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d%s' % (
            COOKIE, token, age, '; Secure' if self.secure() else '')

    def only(self, *methods):
        if self.command not in methods:
            raise Problem(405, {'error': 'method not allowed'})

    def same_origin(self):
        """Reject a cookie-authenticated write aimed at us from another site."""
        origin = self.headers.get('Origin')
        if origin is not None and urlsplit(origin).netloc != self.headers.get('Host', ''):
            raise Problem(403, {'error': 'cross-origin request refused'})

    def authenticate(self, bearer_only=False):
        """The account id behind a device token or a web session cookie."""
        header = self.headers.get('Authorization', '')
        if header.startswith('Bearer '):
            account = self.store.token_account(header[7:].strip())
            if account:
                return account
            raise Problem(401, {'error': 'unauthorized'})
        if bearer_only:
            raise Problem(401, {'error': 'device token required'})
        token = self.cookies().get(COOKIE)
        account = self.store.session_account(token) if token else None
        if account:
            return account
        raise Problem(401, {'error': 'unauthorized'})

    # -- routes -----------------------------------------------------------

    def dispatch(self):
        url = urlsplit(self.path)
        path, query = url.path, parse_qs(url.query, keep_blank_values=True)
        if path == '/healthz':
            self.only('GET', 'HEAD')
            return self.reply(200, {'ok': True, 'protocol': PROTOCOL})
        if path.startswith('/auth/'):
            return self.auth_route(path[len('/auth/'):])
        if path in STATIC:
            return self.static(path)
        if path == '/v1/vaults' or path.startswith('/v1/vaults/'):
            return self.vault_route(path, query)
        raise Problem(404, {'error': 'not found'})

    def static(self, path):
        self.only('GET', 'HEAD')
        name, kind = STATIC[path]
        try:
            data = (self.server.static / name).read_bytes()
        except OSError:
            raise Problem(404, {'error': 'not found'})
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        if kind.startswith('text/html'):
            self.send_header('Content-Security-Policy', CSP)
            self.send_header('X-Frame-Options', 'DENY')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    # -- accounts and sessions --------------------------------------------

    def auth_route(self, tail):
        if tail == 'register':
            self.only('POST')
            body = self.json_body()
            email, password = body.get('email'), body.get('password')
            if not isinstance(email, str) or len(email) > 190 or not EMAIL.fullmatch(email):
                raise Problem(400, {'error': 'a valid email address is required'})
            if not isinstance(password, str) or not 10 <= len(password) <= 200:
                raise Problem(400, {'error': 'password must be 10 to 200 characters'})
            return self.reply(201, {'account': self.store.create_account(email.lower(), password)})

        if tail == 'login':
            self.only('POST')
            body = self.json_body()
            email, password = body.get('email'), body.get('password')
            if not isinstance(email, str) or not isinstance(password, str):
                raise Problem(400, {'error': 'email and password required'})
            account = self.store.check_password(email[:190], password[:200])
            if account is None:
                raise Problem(401, {'error': 'invalid email or password'})
            token = self.store.start_session(account['id'])
            return self.reply(200, {'account': account},
                              [('Set-Cookie', self.cookie_header(token, SESSION_SECONDS))])

        if tail == 'logout':
            self.only('POST')
            self.same_origin()
            token = self.cookies().get(COOKIE)
            if token:
                self.store.end_session(token)
            return self.reply(200, {'ok': True}, [('Set-Cookie', self.cookie_header('', 0))])

        if tail == 'session':
            self.only('GET', 'HEAD')
            return self.reply(200, {'account': self.store.account(self.authenticate())})

        if tail == 'token' or tail.startswith('token/'):
            return self.token_route(tail)
        raise Problem(404, {'error': 'not found'})

    def token_route(self, tail):
        # Issuing a device token is a website action, so the cookie does it.
        account = self.authenticate()
        if tail == 'token':
            if self.command in ('GET', 'HEAD'):
                return self.reply(200, {'tokens': self.store.tokens(account)})
            self.only('GET', 'HEAD', 'POST')
            if self.command == 'POST':
                self.same_origin()
                label = self.json_body().get('label', 'device')
                if not isinstance(label, str) or not 0 < len(label) <= 60 \
                        or any(ord(c) < 32 for c in label):
                    raise Problem(400, {'error': 'invalid label'})
                token = self.store.issue_token(account, label)
                # The only time the token itself leaves the server.
                return self.reply(201, {'token': token, 'id': digest(token), 'label': label})
        identifier = tail[len('token/'):]
        if not HEX64.fullmatch(identifier):
            raise Problem(404, {'error': 'no such token'})
        self.only('DELETE')
        self.same_origin()
        self.store.revoke_token(account, identifier)
        return self.reply(200, {'ok': True})

    # -- vaults, records and blobs ----------------------------------------

    def vault_route(self, path, query):
        parts = [part for part in path[len('/v1/vaults'):].split('/') if part]
        # Records and packages belong to devices, which hold a bearer token.
        # The website has no vault key, so its cookie is not accepted for them.
        device_only = len(parts) > 1 and parts[1] in ('records', 'blobs')
        account = self.authenticate(bearer_only=device_only)
        if not parts:
            self.only('GET', 'HEAD', 'POST')
            if self.command in ('GET', 'HEAD'):
                return self.reply(200, {
                    'vaults': self.store.vaults(account),
                    'quota': {'bytes': self.store.usage(account), 'limit': self.server.quota_bytes}})
            if self.command == 'POST':
                self.same_origin()
                vault = self.store.create_vault(account, self.vault_name(self.json_body()))
                return self.reply(201, {'vault': vault})

        if not parts or not VAULT_ID.fullmatch(parts[0]) or self.store.vault(account, parts[0]) is None:
            # Unknown and someone else's vault are the same answer on purpose.
            raise Problem(404, {'error': 'no such vault'})
        vault = parts[0]
        if device_only:
            if parts[1] == 'blobs':
                return self.blob_route(vault, account, parts, query)
            return self.record_route(vault, parts, query)
        if len(parts) == 1:
            self.only('GET', 'HEAD', 'PATCH', 'DELETE')
            if self.command in ('GET', 'HEAD'):
                return self.reply(200, {'vault': self.store.vault(account, vault)})
            if self.command == 'PATCH':
                self.same_origin()
                name = self.vault_name(self.json_body())
                return self.reply(200, {'vault': self.store.rename_vault(account, vault, name)})
            if self.command == 'DELETE':
                self.same_origin()
                self.store.delete_vault(account, vault)
                return self.empty(204)
        raise Problem(404, {'error': 'not found'})

    def vault_name(self, body):
        name = body.get('name')
        if not isinstance(name, str) or not name.strip() or len(name) > 120 \
                or any(ord(c) < 32 for c in name):
            raise Problem(400, {'error': 'invalid vault name'})
        return name.strip()

    def record_route(self, vault, parts, query):
        if len(parts) == 2:
            self.only('GET', 'HEAD')
            since = integer((query.get('since') or ['0'])[0])
            limit = integer((query.get('limit') or ['500'])[0])
            if since is None or not limit:
                raise Problem(400, {'error': 'invalid since or limit'})
            records, cursor = self.store.records(vault, since, min(limit, 1000))
            return self.reply(200, {'records': records, 'cursor': cursor})
        if len(parts) < 3 or not RECORD_ID.fullmatch(parts[2]):
            raise Problem(404, {'error': 'no such record'})
        record = parts[2]
        if len(parts) == 3:
            self.only('GET', 'HEAD', 'PUT', 'DELETE')
            if self.command in ('GET', 'HEAD'):
                found = self.store.record(vault, record)
                if found is None:
                    raise Problem(404, {'error': 'no such record'})
                return self.reply(200, {'record': found})
            if self.command == 'PUT':
                return self.put_record(vault, record)
            if self.command == 'DELETE':
                purge = (query.get('purge') or ['0'])[0] in ('1', 'true', 'yes')
                if self.store.tombstone(vault, record, purge) is None:
                    return self.empty(204)
                return self.reply(200, {'record': self.store.record(vault, record)})
        if len(parts) == 4 and parts[3] == 'restore':
            self.only('POST')
            return self.reply(200, {'record': self.store.restore(vault, record)})
        raise Problem(404, {'error': 'not found'})

    def put_record(self, vault, record):
        body = self.json_body()
        operation = body.get('operation')
        if not isinstance(operation, str) or not OPERATION.fullmatch(operation):
            raise Problem(400, {'error': 'operation id required'})
        base = body.get('base_version')
        if not isinstance(base, int) or isinstance(base, bool) or not 0 <= base <= 2**62:
            raise Problem(400, {'error': 'base_version required'})
        status, payload = self.store.write_record(vault, record, base, self.record_fields(body), operation)
        self.reply(status, payload)

    def record_fields(self, body):
        name = body.get('name')
        if not isinstance(name, str) or not name.strip() or len(name) > 255 \
                or any(ord(c) < 32 for c in name):
            raise Problem(400, {'error': 'invalid name'})
        path = body.get('path') or ''
        if not isinstance(path, str) or len(path) > 1024 or any(ord(c) < 32 for c in path):
            raise Problem(400, {'error': 'invalid path'})
        tags = body.get('tags') or []
        if not isinstance(tags, list) or len(tags) > 32 \
                or not all(isinstance(tag, str) and 0 < len(tag) <= 64 for tag in tags):
            raise Problem(400, {'error': 'invalid tags'})
        package = body.get('package') or None
        if package is not None and (not isinstance(package, str) or not HEX64.fullmatch(package)):
            raise Problem(400, {'error': 'invalid package'})
        size = body.get('size', 0)
        if not isinstance(size, int) or isinstance(size, bool) or not 0 <= size <= self.server.max_file_bytes:
            raise Problem(400, {'error': 'invalid size'})
        return {'name': name, 'path': path, 'tags': tags, 'package': package, 'size': size}

    def blob_route(self, vault, account, parts, query):
        if len(parts) != 3 or not HEX64.fullmatch(parts[2]):
            raise Problem(404, {'error': 'not found'})
        name = parts[2] + '.mvpk'
        if self.command == 'PUT':
            return self.put_blob(vault, account, name, query)
        if self.command in ('GET', 'HEAD'):
            return self.send_blob(vault, name, self.command == 'HEAD')
        self.only('DELETE')
        self.store.delete_blob(vault, name)
        self.empty(204)

    def put_blob(self, vault, account, name, query):
        data = self.read_chunk(CHUNK_BYTES)
        total = integer(self.headers.get('X-Total-Size'))
        offsets = query.get('offset', ['0'])
        offset = integer(offsets[0]) if len(offsets) == 1 else None
        if total is None or offset is None:
            raise Problem(400, {'error': 'X-Total-Size and ?offset=N required'})
        status, payload = self.store.put_blob_chunk(
            vault, name, data, total, offset, account,
            self.server.max_file_bytes, self.server.quota_bytes)
        self.reply(status, payload)

    def send_blob(self, vault, name, head):
        row = self.store.blob(vault, name)
        if row is None:
            raise Problem(404, {'error': 'not found'})
        size = row['size']
        if head:
            self.send_response(200)
            self.send_header('Content-Length', str(size))
            self.send_header('X-Offset', str(row['offset']))
            self.send_header('X-Complete', 'true' if row['complete'] else 'false')
            self.end_headers()
            return
        if not row['complete']:
            raise Problem(404, {'error': 'not found'})
        start, end, status = 0, size - 1, 200
        value = self.headers.get('Range')
        if value:
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', value)
            if not match:
                raise Problem(416, {'error': 'invalid range'})
            start, end = int(match[1]), int(match[2]) if match[2] else size - 1
            if start >= size or end < start or end >= size:
                raise Problem(416, {'error': 'invalid range'})
            status = 206
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(end - start + 1))
        self.send_header('ETag', '"%s"' % name)
        self.send_header('Accept-Ranges', 'bytes')
        if status == 206:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.end_headers()
        remaining = end - start + 1
        with self.store.blob_path(vault, name).open('rb') as f:
            f.seek(start)
            while remaining > 0:
                block = f.read(min(65536, remaining))
                if not block:
                    break
                self.wfile.write(block)
                remaining -= len(block)

    # -- entry point ------------------------------------------------------

    def run_request(self):
        try:
            self.dispatch()
        except Problem as e:
            self.reply(e.status, e.value)
        except (ValueError, TypeError, KeyError):
            self.reply(400, {'error': 'invalid request'})
        except sqlite3.Error:
            self.reply(503, {'error': 'storage unavailable'})
        except OSError:
            pass

    do_GET = do_PUT = do_HEAD = do_DELETE = do_POST = do_PATCH = run_request


# -- command line ---------------------------------------------------------

def read_password(stdin):
    if stdin:
        return sys.stdin.readline().rstrip('\r\n')
    first = getpass.getpass('password: ')
    if getpass.getpass('repeat: ') != first:
        raise SystemExit('passwords do not match')
    return first


def main():
    if os.name != 'nt':
        os.umask(0o077)
    parser = argparse.ArgumentParser(prog='sync_server',
                                     description='Protocol 4 encrypted vault sync server.')
    parser.add_argument('--data', required=True, metavar='DIR', help='data directory (SQLite + blobs)')
    sub = parser.add_subparsers(dest='command', required=True)

    serve = sub.add_parser('serve', help='serve one data directory')
    serve.add_argument('--host', default='127.0.0.1')
    serve.add_argument('--port', type=int, default=8766)
    serve.add_argument('--cert', metavar='PEM', help='TLS certificate chain')
    serve.add_argument('--key', metavar='PEM', help='TLS private key')
    serve.add_argument('--quota-bytes', type=int, default=10 * 1024**3, help='per account')
    serve.add_argument('--max-file-bytes', type=int, default=1024**3)
    serve.add_argument('--workers', type=int, default=16)
    serve.add_argument('--request-timeout', type=float, default=30)
    serve.add_argument('--web', metavar='DIR', help='static web UI directory (default: sync_web/)')
    serve.add_argument('--gc-hours', type=float, default=24.0,
                       help='sweep unfetched packages and abandoned uploads older than this at '
                            'startup (0 disables the sweep)')

    account = sub.add_parser('account', help='operator account commands')
    account.add_argument('action', choices=('create',))
    account.add_argument('email', nargs='?')
    account.add_argument('--password-stdin', action='store_true', help='read the password from stdin')

    vault = sub.add_parser('vault', help="list an account's vaults")
    vault.add_argument('action', choices=('list',))
    vault.add_argument('email')

    token = sub.add_parser('token', help='issue a device token')
    token.add_argument('action', choices=('issue',))
    token.add_argument('email')
    token.add_argument('--label', default='cli')

    gc = sub.add_parser('gc', help='remove unreferenced packages and abandoned uploads')
    gc.add_argument('--older-than-hours', type=float, default=24.0,
                    help='only touch rows older than this (default: 24)')
    gc.add_argument('--vault', help='limit the sweep to one vault id')
    gc.add_argument('--dry-run', action='store_true', help='report what would be freed')
    args = parser.parse_args()

    if args.command == 'serve':
        if not 0 < args.max_file_bytes <= 1024**3:
            parser.error('--max-file-bytes must be between 1 and 1 GiB')
        if not 0 < args.quota_bytes <= 2**63 - 1:
            parser.error('--quota-bytes out of range')
        if not 1 <= args.workers <= 256 or not 0 < args.request_timeout <= 300:
            parser.error('invalid workers or request timeout')
        if bool(args.cert) != bool(args.key):
            parser.error('TLS requires both --cert and --key')
        loopback = args.host in ('localhost', '::1')
        try:
            loopback = loopback or ipaddress.ip_address(args.host).is_loopback
        except ValueError:
            pass
        if not loopback and not args.cert:
            parser.error('non-loopback binding requires --cert and --key (TLS)')
        context = None
        if args.cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(args.cert, args.key)
        static = Path(args.web) if args.web else Path(__file__).resolve().parent.parent / 'sync_web'
        try:
            store = Store(args.data)
        except (ValueError, OSError, sqlite3.Error) as e:
            parser.error(str(e))
        if args.gc_hours > 0:
            try:
                swept = store.collect_garbage(args.gc_hours * 3600)
                if swept['packages'] or swept['partials'] or swept['dangling']:
                    print('startup sweep: %(packages)d package(s), %(partials)d abandoned upload(s), '
                          '%(dangling)d dangling row(s), %(bytes)d byte(s) freed' % swept)
            except (OSError, sqlite3.Error) as e:
                print('startup sweep failed: %s' % e, file=sys.stderr)
        with Server((args.host, args.port), store, args, context, static) as server:
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
        return

    # Operator commands run against the same directory the server uses.
    try:
        store = Store(args.data, recover=False)
    except (ValueError, OSError, sqlite3.Error) as e:
        parser.error(str(e))
    if args.command == 'gc':
        if args.older_than_hours < 0:
            parser.error('--older-than-hours cannot be negative')
        swept = store.collect_garbage(args.older_than_hours * 3600, args.vault, args.dry_run)
        print(f"{'dry run' if args.dry_run else 'swept'}: {swept['packages']} package(s), "
              f"{swept['partials']} abandoned upload(s), {swept['dangling']} dangling row(s), "
              f"{swept['bytes']} byte(s) {'would be freed' if args.dry_run else 'freed'}")
        return
    if not args.email:
        parser.error('an email address is required')
    found = store.account_by_email(args.email)
    if args.command == 'account' and args.action == 'create':
        if found:
            parser.error('that email is already registered')
        created = store.create_account(args.email, read_password(args.password_stdin))
        print('account: ' + created['id'])
        print('email: ' + created['email'])
    elif not found:
        parser.error('no such account')
    elif args.command == 'vault':
        for row in store.vaults(found['id']):
            print('%s\t%s\t%d\t%d' % (row['id'], row['name'], row['bytes'], row['records']))
    else:
        token = store.issue_token(found['id'], args.label)
        print('token: ' + token)
        print('id: ' + digest(token))
        print('label: ' + args.label)
        print('give this to the device now; it is not recoverable', file=sys.stderr)


if __name__ == '__main__':
    main()
