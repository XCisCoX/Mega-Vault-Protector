"""Storage: one SQLite database plus a ``blobs/`` tree, under the data directory.

Nothing here knows about HTTP; callers hand in already-validated values.  A
``Problem`` is the one exception: it carries an HTTP status and a JSON body for
a condition the transport should report (stale version, missing vault, quota).
Everything else is an ordinary exception the handler turns into a 5xx.

Secrets are stored hashed and compared with ``hmac.compare_digest``: password
digests live in ``accounts``, session and device tokens are *looked up* by their
SHA-256 so the plaintext token exists only in the client's cookie or keychain.
"""
from contextlib import contextmanager
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import time

PROTOCOL = 4
CHUNK_BYTES = 1 << 20
HEX64 = re.compile(r'[0-9a-f]{64}')
BLOB_NAME = re.compile(r'[0-9a-f]{64}\.mvpk')
VAULT_ID = re.compile(r'[0-9a-f]{32}')
RECORD_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}')

# scrypt is the password KDF here (stdlib only, no Argon2 dependency).  16 MiB
# of memory per guess, which is the property that matters; 32 bytes out.
SCRYPT = dict(n=16384, r=8, p=1, dklen=32, maxmem=64 * 1024 * 1024)
SESSION_SECONDS = 14 * 24 * 3600
OPERATION_TTL_MS = 24 * 3600 * 1000

SCHEMA = (
    '''CREATE TABLE IF NOT EXISTS accounts(
        id TEXT PRIMARY KEY, email TEXT NOT NULL UNIQUE, salt BLOB NOT NULL,
        digest BLOB NOT NULL, created INTEGER NOT NULL)''',
    '''CREATE TABLE IF NOT EXISTS sessions(
        id TEXT PRIMARY KEY, account TEXT NOT NULL, expires INTEGER NOT NULL)''',
    '''CREATE TABLE IF NOT EXISTS tokens(
        id TEXT PRIMARY KEY, account TEXT NOT NULL, label TEXT NOT NULL,
        created INTEGER NOT NULL, revoked INTEGER NOT NULL)''',
    '''CREATE TABLE IF NOT EXISTS vaults(
        id TEXT PRIMARY KEY, account TEXT NOT NULL, name TEXT NOT NULL,
        created INTEGER NOT NULL, seq INTEGER NOT NULL)''',
    '''CREATE TABLE IF NOT EXISTS records(
        vault TEXT NOT NULL, id TEXT NOT NULL, version INTEGER NOT NULL,
        name TEXT NOT NULL, path TEXT NOT NULL, tags TEXT NOT NULL,
        package TEXT, size INTEGER NOT NULL, deleted INTEGER NOT NULL,
        updated INTEGER NOT NULL, seq INTEGER NOT NULL,
        PRIMARY KEY(vault, id))''',
    'CREATE INDEX IF NOT EXISTS records_cursor ON records(vault, seq)',
    '''CREATE TABLE IF NOT EXISTS operations(
        vault TEXT NOT NULL, operation TEXT NOT NULL, record TEXT NOT NULL,
        status INTEGER NOT NULL, result TEXT NOT NULL, created INTEGER NOT NULL,
        PRIMARY KEY(vault, operation))''',
    'CREATE INDEX IF NOT EXISTS operations_age ON operations(vault, created)',
    '''CREATE TABLE IF NOT EXISTS blobs(
        vault TEXT NOT NULL, name TEXT NOT NULL, size INTEGER NOT NULL,
        offset INTEGER NOT NULL, complete INTEGER NOT NULL,
        PRIMARY KEY(vault, name))''',
)


class Problem(Exception):
    """An HTTP-shaped failure: status plus a JSON body."""

    def __init__(self, status, value):
        super().__init__(value)
        self.status, self.value = status, value


def now_ms():
    return int(time.time() * 1000)


def digest(token):
    """The stored form of a bearer token or session cookie."""
    return hashlib.sha256(token.encode('utf-8')).hexdigest()


def derive(password, salt):
    return hashlib.scrypt(password.encode('utf-8'), salt=salt, **SCRYPT)


def sync_directory(path):
    """Persist directory entries on POSIX; Windows has no directory fsync."""
    if os.name != 'nt':
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def public_record(row):
    return {'id': row['id'], 'version': row['version'], 'name': row['name'],
            'path': row['path'], 'tags': json.loads(row['tags']),
            'package': row['package'] or None, 'size': row['size'],
            'deleted': bool(row['deleted']), 'updated': row['updated']}


class Store:
    """Accounts, vaults, records and the content-addressed blob tree."""

    def __init__(self, directory, recover=True):
        self.root = Path(directory)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.database = self.root / 'sync.sqlite3'
        with self.session() as db:
            version = db.execute('PRAGMA user_version').fetchone()[0]
            if version > PROTOCOL:
                raise ValueError('data directory was written by a newer server')
            if version and version < PROTOCOL:
                raise ValueError('data directory uses protocol %d; this server needs %d -- '
                                 'start from an empty directory' % (version, PROTOCOL))
            db.execute('PRAGMA journal_mode=WAL')
        self.blobs = self.root / 'blobs'
        self.blobs.mkdir(exist_ok=True, mode=0o700)
        with self.session(write=True) as db:
            for statement in SCHEMA:
                db.execute(statement)
            db.execute('PRAGMA user_version=%d' % PROTOCOL)
            db.execute('DELETE FROM sessions WHERE expires<?', (now_ms(),))
        if recover:
            self.recover()

    @contextmanager
    def session(self, write=False):
        """One connection; ``write`` holds the single-writer lock for the block."""
        db = sqlite3.connect(self.database, timeout=10, isolation_level=None)
        db.row_factory = sqlite3.Row
        try:
            db.execute('PRAGMA synchronous=FULL')
            if write:
                db.execute('BEGIN IMMEDIATE')
            yield db
            if write:
                db.execute('COMMIT')
        except BaseException:
            if write and db.in_transaction:
                db.execute('ROLLBACK')
            raise
        finally:
            db.close()

    def recover(self):
        """Trim unacknowledged tails and drop packages no row references."""
        with self.session(write=True) as db:
            rows = db.execute('SELECT vault,name,size,offset,complete FROM blobs').fetchall()
            known = set()
            for row in rows:
                vault, name = row['vault'], row['name']
                directory = self.blobs / vault
                final, part = directory / name, directory / (name + '.part')
                known.add((vault, name))
                if row['complete']:
                    if not final.is_file() or final.stat().st_size != row['size']:
                        raise ValueError('complete package missing or wrong size: ' + name)
                    part.unlink(missing_ok=True)
                    continue
                if final.is_file() and final.stat().st_size == row['size']:
                    with final.open('rb') as f:
                        if hashlib.file_digest(f, 'sha256').hexdigest() == name.removesuffix('.mvpk'):
                            db.execute('UPDATE blobs SET offset=size,complete=1 WHERE vault=? AND name=?',
                                       (vault, name))
                            part.unlink(missing_ok=True)
                            continue
                actual = part.stat().st_size if part.is_file() else 0
                if actual > row['offset']:
                    # Bytes written after the last acknowledged chunk: drop them.
                    with part.open('r+b') as f:
                        f.truncate(row['offset'])
                        f.flush()
                        os.fsync(f.fileno())
                elif actual < row['offset']:
                    # Durable bytes are gone; let the client start over.
                    db.execute('UPDATE blobs SET offset=? WHERE vault=? AND name=?', (actual, vault, name))
            for directory in self.blobs.iterdir():
                if not directory.is_dir():
                    continue
                for path in directory.iterdir():
                    if (directory.name, path.name.removesuffix('.part')) not in known \
                            and BLOB_NAME.fullmatch(path.name.removesuffix('.part')):
                        path.unlink()

    # -- accounts ---------------------------------------------------------

    def create_account(self, email, password):
        salt = secrets.token_bytes(16)
        value = (secrets.token_hex(16), email.lower(), salt, derive(password, salt), now_ms())
        with self.session(write=True) as db:
            try:
                db.execute('INSERT INTO accounts(id,email,salt,digest,created) VALUES(?,?,?,?,?)', value)
            except sqlite3.IntegrityError:
                raise Problem(409, {'error': 'that email is already registered'})
        return self.account(value[0])

    def account(self, account_id):
        with self.session() as db:
            row = db.execute('SELECT id,email,created FROM accounts WHERE id=?', (account_id,)).fetchone()
        return dict(row) if row else None

    def account_by_email(self, email):
        with self.session() as db:
            row = db.execute('SELECT id,email,created FROM accounts WHERE email=?',
                             (email.lower(),)).fetchone()
        return dict(row) if row else None

    def check_password(self, email, password):
        """The account for this email and password, or None.  Always one KDF."""
        with self.session() as db:
            row = db.execute('SELECT id,email,created,salt,digest FROM accounts WHERE email=?',
                             (email.lower(),)).fetchone()
        if row is None:
            # Burn the same work so a missing email is not the fast path.
            derive(password, b'\x00' * 16)
            return None
        return dict(id=row['id'], email=row['email'], created=row['created']) \
            if hmac.compare_digest(derive(password, row['salt']), row['digest']) else None

    # -- web sessions -----------------------------------------------------

    def start_session(self, account_id):
        token = secrets.token_urlsafe(32)
        with self.session(write=True) as db:
            db.execute('INSERT INTO sessions(id,account,expires) VALUES(?,?,?)',
                       (digest(token), account_id, now_ms() + SESSION_SECONDS * 1000))
        return token

    def session_account(self, token):
        if len(token) > 200:
            return None
        with self.session() as db:
            row = db.execute('SELECT account,expires FROM sessions WHERE id=?',
                             (digest(token),)).fetchone()
        if row is None or row['expires'] < now_ms():
            if row is not None:
                with self.session(write=True) as db:
                    db.execute('DELETE FROM sessions WHERE id=?', (digest(token),))
            return None
        return row['account']

    def end_session(self, token):
        with self.session(write=True) as db:
            db.execute('DELETE FROM sessions WHERE id=?', (digest(token),))

    # -- device tokens ----------------------------------------------------

    def issue_token(self, account_id, label):
        token = secrets.token_hex(32)
        created = now_ms()
        with self.session(write=True) as db:
            db.execute('INSERT INTO tokens(id,account,label,created,revoked) VALUES(?,?,?,?,0)',
                       (digest(token), account_id, label, created))
        return token

    def token_account(self, token):
        """The account a live device token belongs to."""
        if not HEX64.fullmatch(token):
            return None
        with self.session() as db:
            row = db.execute('SELECT account,revoked FROM tokens WHERE id=?',
                             (digest(token),)).fetchone()
        return row['account'] if row is not None and not row['revoked'] else None

    def tokens(self, account_id):
        with self.session() as db:
            rows = db.execute('SELECT id,label,created,revoked FROM tokens WHERE account=? '
                              'ORDER BY created', (account_id,)).fetchall()
        return [dict(row) for row in rows]

    def revoke_token(self, account_id, identifier):
        with self.session(write=True) as db:
            changed = db.execute('UPDATE tokens SET revoked=1 WHERE id=? AND account=?',
                                 (identifier, account_id)).rowcount
        if not changed:
            raise Problem(404, {'error': 'no such token'})

    # -- vaults -----------------------------------------------------------

    def create_vault(self, account_id, name):
        identifier = secrets.token_hex(16)
        with self.session(write=True) as db:
            db.execute('INSERT INTO vaults(id,account,name,created,seq) VALUES(?,?,?,?,0)',
                       (identifier, account_id, name, now_ms()))
        (self.blobs / identifier).mkdir(exist_ok=True, mode=0o700)
        return self.vault(account_id, identifier)

    def vault(self, account_id, vault_id):
        """A vault row, but only if this account owns it."""
        with self.session() as db:
            row = db.execute('SELECT id,account,name,created,seq FROM vaults WHERE id=? AND account=?',
                             (vault_id, account_id)).fetchone()
        return dict(row) if row else None

    def vaults(self, account_id):
        with self.session() as db:
            rows = db.execute(
                '''SELECT v.id, v.name, v.created,
                          (SELECT COALESCE(SUM(size),0) FROM blobs WHERE vault=v.id) AS bytes,
                          (SELECT COUNT(*) FROM records WHERE vault=v.id AND deleted=0) AS records
                   FROM vaults v WHERE v.account=? ORDER BY v.created''', (account_id,)).fetchall()
        return [dict(row) for row in rows]

    def usage(self, account_id):
        with self.session() as db:
            return db.execute(
                '''SELECT COALESCE(SUM(b.size),0) FROM blobs b JOIN vaults v ON v.id=b.vault
                   WHERE v.account=?''', (account_id,)).fetchone()[0]

    def rename_vault(self, account_id, vault_id, name):
        with self.session(write=True) as db:
            if db.execute('UPDATE vaults SET name=? WHERE id=? AND account=?',
                          (name, vault_id, account_id)).rowcount != 1:
                raise Problem(404, {'error': 'no such vault'})
        return self.vault(account_id, vault_id)

    def delete_vault(self, account_id, vault_id):
        with self.session(write=True) as db:
            if db.execute('DELETE FROM vaults WHERE id=? AND account=?',
                          (vault_id, account_id)).rowcount != 1:
                raise Problem(404, {'error': 'no such vault'})
            for table in ('records', 'operations', 'blobs'):
                db.execute('DELETE FROM %s WHERE vault=?' % table, (vault_id,))
        directory = self.blobs / vault_id
        if directory.is_dir():
            for path in directory.iterdir():
                path.unlink()
            directory.rmdir()

    # -- records ----------------------------------------------------------

    def records(self, vault_id, since=0, limit=500):
        """Records changed after ``since``, oldest change first, plus a cursor."""
        with self.session() as db:
            rows = db.execute('SELECT * FROM records WHERE vault=? AND seq>? ORDER BY seq LIMIT ?',
                              (vault_id, since, limit + 1)).fetchall()
            high = db.execute('SELECT seq FROM vaults WHERE id=?', (vault_id,)).fetchone()['seq']
        more = len(rows) > limit
        rows = rows[:limit]
        cursor = rows[-1]['seq'] if more else high
        return [public_record(row) for row in rows], cursor

    def record(self, vault_id, record_id):
        with self.session() as db:
            row = db.execute('SELECT * FROM records WHERE vault=? AND id=?', (vault_id, record_id)).fetchone()
        return public_record(row) if row else None

    def write_record(self, vault_id, record_id, base, fields, operation):
        """Create or update with an optimistic version check and a replay key.

        Returns ``(status, payload)``.  A repeated ``operation`` returns the
        first call's stored answer and changes nothing; a stale ``base`` raises
        409 carrying the current record so the client can make a conflict copy.
        """
        with self.session(write=True) as db:
            if db.execute('SELECT 1 FROM vaults WHERE id=?', (vault_id,)).fetchone() is None:
                raise Problem(404, {'error': 'no such vault'})
            done = db.execute('SELECT status,result FROM operations WHERE vault=? AND operation=?',
                              (vault_id, operation)).fetchone()
            if done:
                return done['status'], json.loads(done['result'])
            row = db.execute('SELECT * FROM records WHERE vault=? AND id=?',
                             (vault_id, record_id)).fetchone()
            if row is None:
                if base != 0:
                    raise Problem(409, {'error': 'version conflict', 'record': None})
                version, status = 1, 201
            else:
                if base != row['version']:
                    raise Problem(409, {'error': 'version conflict', 'record': public_record(row)})
                version, status = base + 1, 200
            db.execute('UPDATE vaults SET seq=seq+1 WHERE id=?', (vault_id,))
            seq = db.execute('SELECT seq FROM vaults WHERE id=?', (vault_id,)).fetchone()['seq']
            updated = now_ms()
            tags = json.dumps(fields['tags'], separators=(',', ':'))
            if row is None:
                db.execute('''INSERT INTO records(vault,id,version,name,path,tags,package,size,deleted,updated,seq)
                              VALUES(?,?,?,?,?,?,?,?,0,?,?)''',
                           (vault_id, record_id, version, fields['name'], fields['path'], tags,
                            fields['package'], fields['size'], updated, seq))
            else:
                db.execute('''UPDATE records SET version=?,name=?,path=?,tags=?,package=?,size=?,
                              deleted=0,updated=?,seq=? WHERE vault=? AND id=?''',
                           (version, fields['name'], fields['path'], tags, fields['package'],
                            fields['size'], updated, seq, vault_id, record_id))
            payload = {'record': {'id': record_id, 'version': version, **fields,
                                  'deleted': False, 'updated': updated}}
            db.execute('''INSERT INTO operations(vault,operation,record,status,result,created)
                          VALUES(?,?,?,?,?,?)''',
                       (vault_id, operation, record_id, status, json.dumps(payload), updated))
            db.execute('DELETE FROM operations WHERE vault=? AND created<?', (vault_id, updated - OPERATION_TTL_MS))
            return status, payload

    def tombstone(self, vault_id, record_id, purge=False):
        """Flag a record deleted (reversible debris) or remove it for good."""
        with self.session(write=True) as db:
            if db.execute('SELECT 1 FROM vaults WHERE id=?', (vault_id,)).fetchone() is None:
                raise Problem(404, {'error': 'no such vault'})
            row = db.execute('SELECT * FROM records WHERE vault=? AND id=?',
                             (vault_id, record_id)).fetchone()
            if row is None:
                raise Problem(404, {'error': 'no such record'})
            if purge:
                db.execute('DELETE FROM records WHERE vault=? AND id=?', (vault_id, record_id))
                db.execute('DELETE FROM operations WHERE vault=? AND record=?', (vault_id, record_id))
                return None
            if row['deleted']:
                return public_record(row)
            db.execute('UPDATE vaults SET seq=seq+1 WHERE id=?', (vault_id,))
            seq = db.execute('SELECT seq FROM vaults WHERE id=?', (vault_id,)).fetchone()['seq']
            db.execute('UPDATE records SET deleted=1,version=version+1,updated=?,seq=? WHERE vault=? AND id=?',
                       (now_ms(), seq, vault_id, record_id))
            row = db.execute('SELECT * FROM records WHERE vault=? AND id=?', (vault_id, record_id)).fetchone()
        return public_record(row)

    def restore(self, vault_id, record_id):
        with self.session(write=True) as db:
            if db.execute('SELECT 1 FROM vaults WHERE id=?', (vault_id,)).fetchone() is None:
                raise Problem(404, {'error': 'no such vault'})
            row = db.execute('SELECT * FROM records WHERE vault=? AND id=?',
                             (vault_id, record_id)).fetchone()
            if row is None:
                raise Problem(404, {'error': 'no such record'})
            if not row['deleted']:
                return public_record(row)
            db.execute('UPDATE vaults SET seq=seq+1 WHERE id=?', (vault_id,))
            seq = db.execute('SELECT seq FROM vaults WHERE id=?', (vault_id,)).fetchone()['seq']
            db.execute('UPDATE records SET deleted=0,version=version+1,updated=?,seq=? WHERE vault=? AND id=?',
                       (now_ms(), seq, vault_id, record_id))
            row = db.execute('SELECT * FROM records WHERE vault=? AND id=?', (vault_id, record_id)).fetchone()
        return public_record(row)

    # -- blobs ------------------------------------------------------------

    def blob_path(self, vault_id, name):
        return self.blobs / vault_id / name

    def blob(self, vault_id, name):
        with self.session() as db:
            row = db.execute('SELECT size,offset,complete FROM blobs WHERE vault=? AND name=?',
                             (vault_id, name)).fetchone()
        return dict(row) if row else None

    def put_blob_chunk(self, vault_id, name, data, total, offset, account_id, max_bytes, quota):
        """Append one chunk of a content-addressed package; returns (status, body)."""
        if total > max_bytes:
            raise Problem(413, {'error': 'package too large'})
        final, part = self.blob_path(vault_id, name), self.blob_path(vault_id, name + '.part')
        with self.session(write=True) as db:
            if db.execute('SELECT 1 FROM vaults WHERE id=?', (vault_id,)).fetchone() is None:
                raise Problem(404, {'error': 'no such vault'})
            row = db.execute('SELECT size,offset,complete FROM blobs WHERE vault=? AND name=?',
                             (vault_id, name)).fetchone()
            if row and row['size'] != total:
                raise Problem(409, {'offset': row['offset']})
            if row and row['complete']:
                return 200, {'offset': row['size'], 'complete': True}
            position = row['offset'] if row else 0
            if offset != position:
                raise Problem(409, {'offset': position})
            if position + len(data) > total:
                raise Problem(400, {'error': 'chunk exceeds declared size'})
            created = row is None
            if created:
                used = db.execute(
                    '''SELECT COALESCE(SUM(b.size),0) FROM blobs b JOIN vaults v ON v.id=b.vault
                       WHERE v.account=?''', (account_id,)).fetchone()[0]
                if used + total > quota:
                    raise Problem(507, {'error': 'quota exceeded'})
                db.execute('INSERT INTO blobs(vault,name,size,offset,complete) VALUES(?,?,?,0,0)',
                           (vault_id, name, total))
            with part.open('r+b' if part.exists() else 'w+b') as f:
                f.truncate(position)
                f.seek(position)
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            sync_directory(part.parent)
            position += len(data)
            complete = position == total
            if complete:
                with part.open('rb') as f:
                    rejected = hashlib.file_digest(f, 'sha256').hexdigest() + '.mvpk' != name
            else:
                rejected = False
            if rejected:
                part.unlink()
                db.execute('DELETE FROM blobs WHERE vault=? AND name=?', (vault_id, name))
            else:
                if complete:
                    os.replace(part, final)
                    sync_directory(part.parent)
                db.execute('UPDATE blobs SET offset=?,complete=? WHERE vault=? AND name=?',
                           (position, int(complete), vault_id, name))
        if rejected:
            raise Problem(422, {'error': 'hash mismatch'})
        return (201 if created else 200), {'offset': position, 'complete': complete}

    def delete_blob(self, vault_id, name):
        with self.session(write=True) as db:
            if db.execute('DELETE FROM blobs WHERE vault=? AND name=?', (vault_id, name)).rowcount != 1:
                raise Problem(404, {'error': 'not found'})
        for path in (self.blob_path(vault_id, name), self.blob_path(vault_id, name + '.part')):
            path.unlink(missing_ok=True)
        sync_directory(self.blobs / vault_id)
