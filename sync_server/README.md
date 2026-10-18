# sync_server — protocol 4: accounts, vaults, versioned records, encrypted packages

A single Python process that stores opaque, already-encrypted `.mvpk` packages
plus the small metadata rows that describe them. It never holds a key, a file
name, a tag, or a plaintext byte, so it cannot read a vault even if it wanted
to: the client encrypts, the client names the package by the SHA-256 of its own
ciphertext, and the vault password stays on the device.

Requirements: Python 3.12+ and the standard library only. No third-party
packages, no crypto library, no ORM, no framework. Two modules, `store.py`
(SQLite + `blobs/`) and `__main__.py` (HTTP + CLI), roughly 1,100 lines together
on purpose.

## Run it locally

```sh
# 1. Create the owner account. The password is read from the terminal, hashed
#    with scrypt and a per-account salt, and never written anywhere.
python -m sync_server --data ./sync-data account create owner@example.com

# 2. Serve (loopback only by default).
python -m sync_server --data ./sync-data serve

# 3. Open the website, create a vault, issue a device token.
#    http://127.0.0.1:8766/
```

The site at `/` is the only client-side code: register, log in, list, create,
rename and delete vaults, see usage, issue and revoke device tokens, and a
"connect the app" panel that shows this server's URL. It has no vault key, so it
cannot list, read or decrypt files — by construction, not by policy.

Then point a device at the server URL with the token:

```sh
TOKEN=...        # the 64 hex characters shown once when you issued it
VAULT=...        # the opaque vault id, e.g. from `vault list`
NAME=$(sha256sum item.mvpk | cut -d' ' -f1)

curl -fsS -X PUT "http://127.0.0.1:8766/v1/vaults/$VAULT/blobs/$NAME?offset=0" \
  -H "Authorization: Bearer $TOKEN" \
  -H "X-Total-Size: $(stat -c%s item.mvpk)" \
  --data-binary @item.mvpk
```

### Password hashing

Passwords are hashed with **scrypt** from `hashlib` (`n=16384, r=8, p=1`, 32
bytes out, 16 random salt bytes per account, 16 MiB per guess), not Argon2id as
the design note originally said. That is a deliberate stdlib-only trade: Argon2
would mean shipping a C dependency into a container that currently has none.
Session cookies and device tokens are 32 random bytes from `secrets`; the
server stores only their SHA-256, and comparison of secret material goes through
`hmac.compare_digest`.

## Endpoints

| Method | Path | Auth | Notes |
|---|---|---|---|
| GET | `/healthz` | none | `{ok:true, protocol:4}` |
| POST | `/auth/register` | none | `{email, password}` → `201 {account}`; 409 if taken |
| POST | `/auth/login` | none | sets `mv_session` (HttpOnly, SameSite=Lax, 14 days) |
| POST | `/auth/logout` | cookie | ends that session only |
| GET | `/auth/session` | cookie/bearer | `{account}`; 401 when signed out |
| GET/POST | `/auth/token` | cookie | list / issue a device token; the token is returned **once** |
| DELETE | `/auth/token/<id>` | cookie | revoke; the device gets 401 from then on |
| GET | `/v1/vaults` | cookie or bearer | `{vaults:[{id,name,created,bytes,records}], quota:{bytes,limit}}` |
| POST | `/v1/vaults` | cookie or bearer | `{name}` → `201 {vault}` |
| GET | `/v1/vaults/<id>` | cookie or bearer | one vault |
| PATCH | `/v1/vaults/<id>` | cookie or bearer | `{name}` rename |
| DELETE | `/v1/vaults/<id>` | cookie or bearer | removes records, rows and packages |
| GET | `/v1/vaults/<id>/records?since=&limit=` | **bearer** | `{records:[...], cursor}` |
| PUT | `/v1/vaults/<id>/records/<record>` | **bearer** | `{base_version, operation, name, path, tags, package, size}` |
| GET | `/v1/vaults/<id>/records/<record>` | **bearer** | `{record}` |
| POST | `/v1/vaults/<id>/records/<record>/restore` | **bearer** | clears a tombstone |
| DELETE | `/v1/vaults/<id>/records/<record>` | **bearer** | tombstone; `?purge=1` is permanent |
| PUT | `/v1/vaults/<id>/blobs/<64hex>?offset=N` | **bearer** | body = one chunk, `X-Total-Size` = full size |
| HEAD | `/v1/vaults/<id>/blobs/<64hex>` | **bearer** | `Content-Length` = declared size, `X-Offset` = durable bytes |
| GET | `/v1/vaults/<id>/blobs/<64hex>` | **bearer** | bytes; `Range` supported; `ETag` = the package name |
| DELETE | `/v1/vaults/<id>/blobs/<64hex>` | **bearer** | `204`, immediate |
| GET | `/`, `/app.js`, `/style.css`, `/icon.svg` | none | the static site |

Every `/v1` route is scoped to the authenticated account. A token for account A
asking for account B's vault gets `404` — the same answer as a vault that does
not exist, so ids cannot be probed. Records and blobs require a bearer token and
refuse the website cookie: the browser has no vault key, so it has no business
on those routes, and refusing the cookie removes any cross-site request angle.
Writes that do take the cookie (vault create/rename/delete, token issue/revoke,
logout) require `Content-Type: application/json` and, when `Origin` is present,
a matching `Host`.

## Records and versions

A record is `{id, version, name, path, tags, package, size, deleted, updated}`.

`PUT` is optimistic. `base_version` must equal the stored version, or `0` to
create: matching → `version + 1` and `200`, creating → `version 1` and `201`.

- **Stale `base_version`** → `409 {"error":"version conflict","record":{...}}`
  carrying the *current* record. The server never merges and never overwrites.
  The client does what MEGA does: it re-issues its own copy as a **new record
  id** (name suffix `(conflict copy)`) and uploads that. Both items exist and
  the user deletes whichever they do not want.
- **Unknown record with `base_version` 0** → created. Unknown record with any
  other base → `409` with `record: null`, which the client treats the same way.
- **`operation`** is an idempotency key. Repeating it (same vault) replays the
  first answer byte for byte and changes nothing, so a retry after a lost
  response is free. Keys are kept for 24 hours.
- **`since`/`cursor`**: every write bumps a per-vault sequence. `GET
  .../records?since=N` returns only rows changed after `N`, oldest change first,
  plus a cursor to store. A short page (`limit`, default 500, max 1000) stops at
  the last row it returned, so a client can walk a big vault in pages.
- **Deletes are tombstones**: `deleted:true`, version bumped, and the row stays
  in the list so other devices can show "removed on another device — restore or
  delete". A stale writer gets a `409` instead of quietly resurrecting it.
  `POST .../restore` clears the flag. `DELETE ?purge=1` removes the row for good
  (and forgets its operation keys).

A tombstone keeps travelling because it is still listed; a purged record is
simply missing on the next full reconciliation.

## Blobs

Unchanged mechanics from the v3 bucket, now under a vault:

- The package is named by the SHA-256 of its own bytes; `?offset=` plus
  `X-Total-Size` make an upload resumable, and a chunk that would overflow the
  declared size, or arrive at the wrong offset, is refused. Offset mismatch →
  `409 {"offset":N}` with the durable offset to resume from.
- The last chunk is verified against the name; a mismatch → `422` and the
  partial is discarded.
- `HEAD` reports the durable offset of an interrupted upload, so a client
  resumes with a ranged `GET` or a continued `PUT`.
- Content addressing means a conflict copy that shares content costs nothing.
- Limits: 1 GiB per package, 1 MiB per PUT chunk, 10 GiB per **account**
  (declared sizes, including partial uploads, count against it). Over quota →
  `507`.
- Blobs are per vault: `blobs/<vault id>/<64hex>.mvpk`. Deleting one vault's
  copy never touches another vault's copy of the same content.

## Operator CLI

```sh
python -m sync_server --data DIR account create owner@example.com
python -m sync_server --data DIR vault list owner@example.com
python -m sync_server --data DIR token issue owner@example.com --label Laptop
```

`account create` prompts twice for the password (or `--password-stdin` for
scripting) and prints `account:` and `email:`. `vault list` prints
`<vault id>\t<name>\t<bytes>\t<records>` per vault. `token issue` prints
`token:`, `id:` and `label:`, and warns on stderr that the token is shown once.

These commands are the break-glass path: they run against the same data
directory the server uses, so anyone who can run them can already read the
database. They therefore do not ask for the account password.

### Reclaiming space

Nothing is deleted implicitly: a record edit uploads a new package and a purge
removes the row, so packages no record points at — and uploads a dead client
started but never finished — stay on disk and keep counting against the quota.
One sweep clears both:

```sh
# what a sweep would take, without touching anything
python -m sync_server --data DIR gc --dry-run
# take it (default: only rows older than 24 hours)
python -m sync_server --data DIR gc --older-than-hours 24
```

The age margin is the safety rule, not a preference: a client uploads the blob
before it writes the record that points at it, and another device may still be
holding an older record for a package a newer edit replaced. `serve` runs the
same sweep at startup (`--gc-hours`, default 24, `0` disables it), so a relay
that is restarted periodically never accumulates garbage.

## Schema (what to back up)

`sync.sqlite3` plus `blobs/`. The tables:

| table | holds |
|---|---|
| `accounts` | `id, email, salt, digest, created` (scrypt digest, never the password) |
| `sessions` | `id` = SHA-256 of the cookie token, `account, expires` |
| `tokens` | `id` = SHA-256 of the bearer token, `account, label, created, revoked` |
| `vaults` | `id, account, name, created, seq` (seq is the record cursor) |
| `records` | `vault, id, version, name, path, tags, package, size, deleted, updated, seq` |
| `operations` | `vault, operation, record, status, result, created` (idempotency, 24 h) |
| `blobs` | `vault, name, size, offset, complete` |

A consistent backup is a copy of the directory while the server is stopped, or
`sqlite3 sync.sqlite3 ".backup snapshot.sqlite3"` plus a copy of `blobs/`.
Restoring is copying it back. Do not copy `blobs/` without its database:
completed packages would no longer be listed. On startup the server trims
unacknowledged upload tails, adopts packages that finished just before a crash,
and drops files no row references; a missing or truncated *completed* package
stops startup rather than serving a lie.

The schema is part of the protocol: if you change a table, bump `PROTOCOL`, so a
directory written by an older build is refused with a message instead of being
misread.

## Deployment notes

**TLS.** The server binds `127.0.0.1` and refuses a non-loopback bind unless
`--cert` and `--key` are given (TLS 1.2+). Either terminate TLS in the server
itself, or run it behind a proxy that speaks TLS to it — but never expose the
plain port. Cookies are marked `Secure` when the request arrived over TLS or
carries `X-Forwarded-Proto: https`.

**Logs.** The server writes nothing per request: no paths, no query strings, no
headers, no bodies, no errors, and the `Server` header carries only
`mvpk-sync/4`. Point your supervisor at stderr for startup failures only.

**Rotate a device.** Revoke its token on the website (`Devices` → `Revoke`) or
`DELETE /auth/token/<id>`; the device's next request is a 401. Nothing needs
restarting.

**Quota** is per account (`--quota-bytes`, default 10 GiB). Lowering it does not
delete anything; further uploads are refused with `507` until usage drops.

**Upgrading from protocol 3.** A v3 data directory (one `files` table, one
library key) is refused with a clear message rather than half-migrated: v3
packages have no owner, so there is no account to attach them to. Start from an
empty directory and let devices upload again — packages are content-addressed,
so a client that still has them pays only the bandwidth. The library-key flags
(`key`, `--key-sha256`, `--key-file`) are gone; accounts and tokens replace them.

## Tests

```sh
python -W error -m unittest discover -s tests/sync_server -v
```

43 tests. They spawn the real server on a real socket against a throwaway data
directory and drive it with `http.client`: register/login (wrong password
rejected), session-cookie web flow, token issue/revoke, cross-account and
cross-vault isolation, record version conflicts carrying the current row,
idempotent replay, tombstone/restore/purge, cursor paging, concurrent writers,
blob put/head/ranged get/resume-after-kill/delete, hash mismatch, quota, TLS, the
non-loopback refusal, and that no secret reaches a log or a plaintext column.
Nothing is mocked.
