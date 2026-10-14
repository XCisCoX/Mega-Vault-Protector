"""Opaque sync relay. No package decoding or vault secrets enter this module."""
import argparse
from contextlib import contextmanager
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import socket
import ssl
import time
import threading
from urllib.parse import urlsplit, parse_qs
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()

def valid(value, length):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{%d}' % length, value) is not None

def sync_directory(path):
    """Persist directory entries on POSIX; Windows has no directory fsync."""
    if os.name != 'nt':
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

class Store:
    def __init__(self, directory):
        self.root = Path(directory)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.database = self.root / 'sync.sqlite3'
        self.blobs = self.root / 'blobs'
        self.blobs.mkdir(exist_ok=True, mode=0o700)
        with self.connect() as db:
            version = db.execute('PRAGMA user_version').fetchone()[0]
            if version > 1:
                raise ValueError('database schema is newer than this server')
            db.execute('PRAGMA journal_mode=WAL')
            db.executescript('''
                BEGIN IMMEDIATE;
                CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT UNIQUE NOT NULL, revoked INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS codes(hash TEXT PRIMARY KEY, expires REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS pair_limits(ip TEXT PRIMARY KEY, start REAL NOT NULL, failures INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS blobs(hash TEXT PRIMARY KEY, size INTEGER NOT NULL, offset INTEGER NOT NULL, complete INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS versions(sequence INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL, revision INTEGER NOT NULL, blob TEXT NOT NULL, size INTEGER NOT NULL, deleted INTEGER NOT NULL, UNIQUE(id,revision));
                CREATE TABLE IF NOT EXISTS operations(device TEXT NOT NULL, operation TEXT NOT NULL, payload TEXT NOT NULL, result TEXT NOT NULL, PRIMARY KEY(device,operation));
                PRAGMA user_version=1;
                COMMIT;
            ''')

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.database, timeout=5)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA synchronous=FULL')
        try:
            with db:
                yield db
        finally:
            db.close()

    def devices(self):
        with self.connect() as db:
            return {'devices': [{'id': r['id'], 'name': r['name'], 'revoked': bool(r['revoked'])} for r in db.execute('SELECT * FROM devices ORDER BY id')]}

    def recover(self):
        """Reconcile filesystem writes interrupted before metadata commit."""
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            rows = db.execute('SELECT * FROM blobs').fetchall()
            known = set()
            for row in rows:
                h = row['hash']
                final, partial = self.blobs / h, self.blobs / (h + '.part')
                known.update((h, h + '.part'))
                if row['complete']:
                    if not final.is_file() or final.stat().st_size != row['size']:
                        raise ValueError('completed blob missing or wrong size')
                    if partial.exists():
                        partial.unlink()
                elif final.exists():
                    with final.open('rb') as f:
                        good = hashlib.file_digest(f, 'sha256').hexdigest() == h
                    if not good or final.stat().st_size != row['size']:
                        raise ValueError('interrupted finalize digest mismatch')
                    db.execute('UPDATE blobs SET complete=1,offset=size WHERE hash=?', (h,))
                    if partial.exists():
                        partial.unlink()
                elif partial.exists():
                    if partial.stat().st_size < row['offset']:
                        raise ValueError('durable partial data missing')
                    with partial.open('r+b') as f:
                        f.truncate(row['offset'])
                        f.flush()
                        os.fsync(f.fileno())
                elif row['offset']:
                    raise ValueError('durable partial data missing')
            for path in self.blobs.iterdir():
                if path.name not in known and re.fullmatch(r'[0-9a-f]{64}(\.part)?', path.name):
                    path.unlink()

    def backup(self, destination):
        target = Path(destination).resolve()
        if target.exists() or target == self.root.resolve() or self.root.resolve() in target.parents:
            raise ValueError('backup destination must be new and outside data directory')
        target.parent.mkdir(parents=True, exist_ok=True)
        stage = target.with_name(target.name + '.staging-' + secrets.token_hex(8))
        stage.mkdir(mode=0o700)
        (stage / 'blobs').mkdir(mode=0o700)
        try:
            with self.connect() as lock:
                # Hold writer lock throughout DB snapshot + mutable partial copy.
                # Copy via a separate read connection to avoid backup's own
                # write-transaction busy loop. Immutable files are never GC'd.
                lock.execute('BEGIN IMMEDIATE')
                with self.connect() as source:
                    dest = sqlite3.connect(stage / 'sync.sqlite3')
                    try:
                        source.backup(dest)
                        rows = dest.execute('SELECT hash,size,offset,complete FROM blobs').fetchall()
                        for h, size, offset, complete in rows:
                            path = self.blobs / (h if complete else h + '.part')
                            if not path.exists() and not complete:
                                path = self.blobs / h
                            output = stage / 'blobs' / (h if complete else h + '.part')
                            remaining = size if complete else offset
                            with path.open('rb') as src, output.open('wb') as dst:
                                while remaining:
                                    chunk = src.read(min(65536, remaining))
                                    if not chunk:
                                        raise ValueError('backup blob is incomplete')
                                    dst.write(chunk)
                                    remaining -= len(chunk)
                                dst.flush()
                                os.fsync(dst.fileno())
                            if complete:
                                with output.open('rb') as f:
                                    if hashlib.file_digest(f, 'sha256').hexdigest() != h:
                                        raise ValueError('backup blob digest mismatch')
                        if dest.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                            raise ValueError('backup database failed integrity check')
                        dest.execute('PRAGMA journal_mode=DELETE')
                    finally:
                        dest.close()
            sync_directory(stage / 'blobs')
            sync_directory(stage)
            os.rename(stage, target)
            sync_directory(target.parent)
        except BaseException:
            if stage.exists():
                shutil.rmtree(stage)
            raise

class Problem(Exception):
    def __init__(self, status, value):
        self.status, self.value = status, value

class Server(ThreadingHTTPServer):
    """Reject excess connections without allocating unbounded threads."""
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, address, store, args, context=None):
        self.store = store
        self.slots = threading.BoundedSemaphore(args.workers)
        self.request_timeout = args.request_timeout
        self.context = context
        self.quota_bytes = args.quota_bytes
        self.max_blob_bytes = args.max_blob_bytes
        self.max_objects = args.max_objects
        self.max_blobs = args.max_blobs
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
        # Deliberately omit exception/request material from server logs.
        pass

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, value):
        payload = json.dumps(value, separators=(',', ':')).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(payload)

    def read_bytes(self, limit):
        if self.headers.get('Transfer-Encoding') or len(self.headers.get_all('Content-Length', [])) != 1:
            raise Problem(400, {'error': 'one Content-Length required'})
        raw = self.headers['Content-Length']
        if not re.fullmatch('[0-9]+', raw):
            raise ValueError()
        length = int(raw)
        if length > limit:
            raise Problem(413, {'error': 'body too large'})
        data = self.rfile.read(length)
        if len(data) != length:
            raise Problem(400, {'error': 'incomplete body'})
        return data

    def body(self):
        value = json.loads(self.read_bytes(65536))
        if not isinstance(value, dict):
            raise ValueError()
        return value

    def reply_records(self, key, rows):
        """Stream manifests/history without materializing unbounded history."""
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(('{' + json.dumps(key) + ':[').encode())
        first = True
        for row in rows:
            if not first:
                self.wfile.write(b',')
            self.wfile.write(json.dumps(self.record(row), separators=(',', ':')).encode())
            first = False
        self.wfile.write(b']}')

    def auth(self):
        header = self.headers.get('Authorization', '')
        token = header[7:] if header.startswith('Bearer ') else ''
        if not valid(token, 64):
            raise Problem(401, {'error': 'unauthorized'})
        with self.server.store.connect() as db:
            device = db.execute('SELECT id FROM devices WHERE token_hash=? AND revoked=0', (digest(token),)).fetchone()
        if device is None:
            raise Problem(401, {'error': 'unauthorized'})
        return device['id']

    def dispatch(self):
        if self.command == 'GET' and self.path == '/healthz':
            return self.reply(200, {'ok': True, 'protocol': 2})
        if self.command == 'POST' and self.path == '/v2/pair':
            value = self.body()
            if set(value) != {'code', 'name'}:
                raise ValueError()
            code, name = value.get('code'), value.get('name')
            if not valid(code, 64) or not isinstance(name, str) or not 1 <= len(name) <= 128 or any(ord(c) < 32 for c in name):
                raise Problem(400, {'error': 'invalid pairing request'})
            with self.server.store.connect() as db:
                db.execute('BEGIN IMMEDIATE')
                ip = self.client_address[0]
                now = time.time()
                db.execute('DELETE FROM pair_limits WHERE start<?', (now-60,))
                limit = db.execute('SELECT failures FROM pair_limits WHERE ip=?', (ip,)).fetchone()
                if limit and limit['failures'] >= 10:
                    raise Problem(429, {'error': 'pairing rate limit'})
                result = db.execute('DELETE FROM codes WHERE hash=? AND expires>?', (digest(code), time.time()))
                if result.rowcount != 1:
                    db.execute('INSERT INTO pair_limits VALUES(?,?,1) ON CONFLICT(ip) DO UPDATE SET failures=failures+1', (ip, now))
                    db.commit()
                    raise Problem(401, {'error': 'invalid or expired pairing code'})
                token, device = secrets.token_hex(32), secrets.token_hex(16)
                db.execute('INSERT INTO devices(id,name,token_hash) VALUES(?,?,?)', (device, name, digest(token)))
            return self.reply(200, {'device_id': device, 'token': token})
        device = self.auth()
        url = urlsplit(self.path)
        if self.command == 'GET' and url.path == '/v2/objects':
            with self.server.store.connect() as db:
                rows = db.execute('SELECT * FROM versions WHERE sequence IN (SELECT MAX(sequence) FROM versions GROUP BY id) ORDER BY sequence')
                return self.reply_records('objects', rows)
        object_match = re.fullmatch('/v2/objects/([0-9a-f]{32})(/history)?', url.path)
        if object_match:
            oid = object_match[1]
            if self.command == 'GET' and object_match[2]:
                with self.server.store.connect() as db:
                    rows = db.execute('SELECT * FROM versions WHERE id=? ORDER BY revision', (oid,))
                    return self.reply_records('versions', rows)
            if self.command == 'POST' and not object_match[2]:
                return self.commit(oid, device)
        match = re.fullmatch('/v2/blobs/([0-9a-f]{64})', url.path)
        if match:
            return self.blob(match[1], parse_qs(url.query), device)
        if self.command == 'GET' and self.path == '/v2/devices':
            return self.reply(200, self.server.store.devices())
        raise Problem(404, {'error': 'not found'})

    @staticmethod
    def record(row):
        result = dict(row)
        result['deleted'] = bool(result['deleted'])
        return result

    def commit(self, oid, device):
        value = self.body()
        if set(value) != {'base_revision', 'blob', 'size', 'deleted', 'operation_id'}:
            raise ValueError()
        base, size, deleted, blob, op = (value[k] for k in ('base_revision', 'size', 'deleted', 'blob', 'operation_id'))
        if type(base) is not int or type(size) is not int or not 0 <= base < 2**63-1 or not 0 <= size < 2**63 or type(deleted) is not bool or not valid(op, 32):
            raise ValueError()
        if (deleted and (blob != '' or size != 0)) or (not deleted and not valid(blob, 64)):
            raise ValueError()
        payload = json.dumps({'id': oid, **value}, sort_keys=True, separators=(',', ':'))
        with self.server.store.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            # Revocation is serialized with commits, including replays.
            if not db.execute('SELECT 1 FROM devices WHERE id=? AND revoked=0', (device,)).fetchone():
                raise Problem(401, {'error': 'unauthorized'})
            replay = db.execute('SELECT * FROM operations WHERE device=? AND operation=?', (device, op)).fetchone()
            if replay:
                if replay['payload'] != payload:
                    raise Problem(409, {'error': 'operation id reused with different payload'})
                return self.reply(200, json.loads(replay['result']))
            current = db.execute('SELECT * FROM versions WHERE id=? ORDER BY revision DESC LIMIT 1', (oid,)).fetchone()
            if current is None and db.execute('SELECT COUNT(DISTINCT id) FROM versions').fetchone()[0] >= self.server.max_objects:
                raise Problem(507, {'error': 'catalog quota exceeded'})
            if base != (current['revision'] if current else 0):
                raise Problem(409, {'current': self.record(current) if current else None})
            if not deleted:
                stored = db.execute('SELECT * FROM blobs WHERE hash=? AND complete=1', (blob,)).fetchone()
                if stored is None or stored['size'] != size:
                    raise Problem(422, {'error': 'complete matching blob required'})
            db.execute('INSERT INTO versions(id,revision,blob,size,deleted) VALUES(?,?,?,?,?)', (oid, base+1, blob, size, int(deleted)))
            result = self.record(db.execute('SELECT * FROM versions WHERE id=? AND revision=?', (oid, base+1)).fetchone())
            db.execute('INSERT INTO operations VALUES(?,?,?,?)', (device, op, payload, json.dumps(result)))
        return self.reply(200, result)

    def blob(self, h, query, device):
        store = self.server.store
        complete_path, partial = store.blobs / h, store.blobs / (h + '.part')
        data = self.read_bytes(1048576) if self.command == 'PUT' else None
        with store.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            if not db.execute('SELECT 1 FROM devices WHERE id=? AND revoked=0', (device,)).fetchone():
                raise Problem(401, {'error': 'unauthorized'})
            row = db.execute('SELECT * FROM blobs WHERE hash=?', (h,)).fetchone()
            # Rename may have completed before a crashed transaction committed.
            if row and not row['complete'] and complete_path.exists():
                with complete_path.open('rb') as f:
                    good = hashlib.file_digest(f, 'sha256').hexdigest() == h
                if good and complete_path.stat().st_size == row['size']:
                    db.execute('UPDATE blobs SET complete=1,offset=size WHERE hash=?', (h,))
                    row = db.execute('SELECT * FROM blobs WHERE hash=?', (h,)).fetchone()
            if self.command == 'PUT':
                total = int(self.headers.get('X-Blob-Size', '-1'))
                offsets = query.get('offset', [])
                if len(offsets) != 1 or not re.fullmatch('[0-9]+', offsets[0]) or total < 0:
                    raise ValueError()
                offset = int(offsets[0])
                if total > self.server.max_blob_bytes:
                    raise Problem(413, {'error': 'blob too large'})
                if row and total != row['size']:
                    raise Problem(409, {'offset': row['offset']})
                if row and row['complete']:
                    db.commit()
                    return self.reply(200, {'offset': total, 'complete': True})
                actual = row['offset'] if row else 0
                if offset != actual:
                    raise Problem(409, {'offset': actual})
                if actual + len(data) > total or (not data and total != 0):
                    raise Problem(400, {'error': 'invalid chunk size'})
                if row is None:
                    if db.execute('SELECT COUNT(*) FROM blobs').fetchone()[0] >= self.server.max_blobs:
                        raise Problem(507, {'error': 'blob count quota exceeded'})
                    used = db.execute('SELECT COALESCE(SUM(size),0) FROM blobs').fetchone()[0]
                    if used + total > self.server.quota_bytes:
                        raise Problem(507, {'error': 'storage quota exceeded'})
                    db.execute('INSERT INTO blobs VALUES(?,?,0,0)', (h, total))
                with partial.open('r+b' if partial.exists() else 'w+b') as f:
                    f.truncate(actual)
                    f.seek(actual)
                    f.write(data)
                    f.flush()
                    os.fsync(f.fileno())
                sync_directory(store.blobs)
                actual += len(data)
                complete = actual == total
                if complete:
                    with partial.open('rb') as f:
                        good = hashlib.file_digest(f, 'sha256').hexdigest() == h
                    if not good:
                        partial.unlink()
                        db.execute('DELETE FROM blobs WHERE hash=?', (h,))
                        db.commit()
                        raise Problem(422, {'error': 'digest mismatch'})
                    os.replace(partial, complete_path)
                    sync_directory(store.blobs)
                db.execute('UPDATE blobs SET offset=?,complete=? WHERE hash=?', (actual, int(complete), h))
                db.commit()
                return self.reply(200, {'offset': actual, 'complete': complete})
            if row is None or (self.command == 'GET' and not row['complete']):
                raise Problem(404, {'error': 'blob not found'})
            if self.command == 'HEAD':
                db.commit()
                self.send_response(200)
                self.send_header('Content-Length', str(row['size']))
                self.send_header('X-Upload-Offset', str(row['offset']))
                self.send_header('X-Blob-Complete', str(bool(row['complete'])).lower())
                self.end_headers()
                return
            if self.command != 'GET':
                raise Problem(405, {'error': 'method not allowed'})
            size = row['size']
        start, end, status = 0, size - 1, 200
        value = self.headers.get('Range')
        if value:
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', value)
            if not match:
                raise Problem(416, {'error': 'invalid range'})
            start, end = int(match[1]), int(match[2]) if match[2] else size-1
            if start >= size or end < start or end >= size:
                raise Problem(416, {'error': 'invalid range'})
            status = 206
        with complete_path.open('rb') as f:
            self.send_response(status)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(end-start+1))
            if status == 206:
                self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
            self.end_headers()
            f.seek(start)
            remaining = end-start+1
            while remaining:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

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

    do_GET = do_POST = do_PUT = do_HEAD = run_request

def main():
    if os.name != 'nt':
        os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', required=True)
    sub = parser.add_subparsers(dest='command', required=True)
    serve = sub.add_parser('serve')
    serve.add_argument('--host', default='127.0.0.1')
    serve.add_argument('--port', type=int, default=8766)
    serve.add_argument('--quota-bytes', type=int, default=10*1024**3)
    serve.add_argument('--max-blob-bytes', type=int, default=1024**3)
    serve.add_argument('--max-objects', type=int, default=10000)
    serve.add_argument('--max-blobs', type=int, default=100000)
    serve.add_argument('--workers', type=int, default=16)
    serve.add_argument('--request-timeout', type=float, default=30)
    serve.add_argument('--cert')
    serve.add_argument('--key')
    serve.add_argument('--proxy-mode', action='store_true', help='private container network only; requires MV_SYNC_PROXY_MODE=1')
    sub.add_parser('pair-code')
    sub.add_parser('devices')
    revoke = sub.add_parser('revoke')
    revoke.add_argument('device_id')
    backup = sub.add_parser('backup')
    backup.add_argument('destination')
    args = parser.parse_args()
    try:
        store = Store(args.data)
    except (ValueError, OSError, sqlite3.Error) as e:
        parser.error(str(e))
    if args.command == 'pair-code':
        code = secrets.token_hex(32)
        with store.connect() as db:
            db.execute('DELETE FROM codes WHERE expires<=?', (time.time(),))
            db.execute('INSERT INTO codes VALUES(?,?)', (digest(code), time.time()+600))
        print(code)
    elif args.command == 'devices':
        print(json.dumps(store.devices()))
    elif args.command == 'revoke':
        if not valid(args.device_id, 32):
            parser.error('invalid device id')
        with store.connect() as db:
            if db.execute('UPDATE devices SET revoked=1 WHERE id=?', (args.device_id,)).rowcount != 1:
                parser.error('unknown device')
    elif args.command == 'backup':
        try:
            store.backup(args.destination)
        except (ValueError, OSError, sqlite3.Error) as e:
            parser.error(str(e))
    else:
        if not 0 < args.quota_bytes < 2**63 or not 0 < args.max_blob_bytes < 2**63 or not 1 <= args.max_objects <= 10000 or not 1 <= args.max_blobs <= 1000000 or not 1 <= args.workers <= 256 or not 0 < args.request_timeout <= 300:
            parser.error('invalid resource limits')
        if bool(args.cert) != bool(args.key):
            parser.error('TLS requires both --cert and --key')
        if args.proxy_mode and (os.environ.get('MV_SYNC_PROXY_MODE') != '1' or args.cert):
            parser.error('proxy mode requires explicit MV_SYNC_PROXY_MODE=1 and no direct TLS')
        try:
            loopback = ipaddress.ip_address(args.host).is_loopback
        except ValueError:
            loopback = False
        if not loopback and not args.cert and not args.proxy_mode:
            parser.error('non-loopback binding requires TLS certificate and key')
        context = None
        if args.cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(args.cert, args.key)
        try:
            store.recover()
        except (ValueError, OSError, sqlite3.Error) as e:
            parser.error(str(e))
        with Server((args.host, args.port), store, args, context) as server:
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass

if __name__ == '__main__':
    main()
