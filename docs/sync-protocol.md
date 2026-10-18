# Sync protocol 3 — wire contract

> **Historical: protocol 3.** The shipped relay (`sync_server/`) and the desktop
> client speak **protocol 4** (accounts, vaults, versioned records). This document
> describes the earlier protocol 3 hash bucket, which the Android client still
> implements. Authoritative v4 references: `sync_server/README.md`,
> `sync_server/deployment/README.md`, `docs/sync-mega-plan.md`.

The relay is an encrypted hash bucket: a flat set of opaque `.mvpk` packages,
each named by the SHA-256 of its own bytes, guarded by one library key. It
parses nothing inside a package. File names, folder paths, tags and content
live inside the ciphertext, encrypted by the client with the same library key,
so the server can store, list, serve and delete packages without learning what
they are.

- Transport: HTTPS to the public edge, HTTP/1.1 to the client. Bodies are
  `application/octet-stream` for packages and small JSON objects for control
  replies. No cookies, no sessions, no query-string credentials.
- A package name is exactly 64 lowercase hex characters followed by `.mvpk`,
  and must equal `sha256(<the bytes served under it>)`.
- The server holds `sha256(library key)` and nothing else. The key itself is
  never stored server-side, so nothing in a stolen data directory can be
  decrypted from it.

## Authentication

```
Authorization: Bearer <64 lowercase hex characters: the library key>
```

Required on every route except `GET /healthz`. A missing, malformed or wrong
value is `401` with `{"error":"unauthorized"}`; the comparison is constant-time
over the SHA-256 of the presented value. Clients present the key itself, so the
channel must be TLS — the deployment terminates it at Caddy and refuses to
expose the relay's plaintext listener.

There is no per-device identity: no pairing code, no device token, no session,
no revocation list. Any holder of the key can list, upload, download and delete
anything, and the server cannot attribute a request to a device. Access is
withdrawn by rotating the key (see below), never per device.

## Endpoints

| Method | Path | Auth | Success |
| --- | --- | --- | --- |
| GET | `/healthz` | no | `200 {"ok":true,"protocol":3,"files":N,"bytes":M}` |
| GET | `/v3/files` | yes | `200 {"files":[{"name":…,"size":…,"seq":…}]}` |
| PUT | `/v3/files/<name>?offset=N` | yes | `201` first chunk, `200` later chunks or already complete |
| HEAD | `/v3/files/<name>` | yes | `200`, size and durable offset in headers |
| GET | `/v3/files/<name>` | yes | `200` bytes, `206` for a range |
| DELETE | `/v3/files/<name>` | yes | `204` |

A method other than the one listed, on `/v3/files` or on a package path, is
`405 {"error":"method not allowed"}`. An unknown path, a path segment that is
not a valid package name, or any unexpected route gets `404`, with the body
`{"error":"not found"}`.

### GET /healthz

Unauthenticated liveness and version probe:

```json
{"ok":true,"protocol":3,"files":4,"bytes":18342}
```

`files` counts complete packages, `bytes` their summed size; unfinished uploads
are excluded from both. A client must treat any `protocol` other than `3` as
incompatible, and a 404 here as "not a sync relay" (the protocol 2 server
answered this route too, so check the number, not the path).

### GET /v3/files

```json
{"files":[{"name":"2f8a…e1.mvpk","size":4096,"seq":7},
          {"name":"9b31…04.mvpk","size":1024,"seq":6}]}
```

Newest first (`seq` descending, assigned when the name is first created),
complete packages only. One response, no pagination: a client with a large
bucket gets the whole list. This is how a device discovers packages it has not
imported yet; names carry no meaning, so the only way to learn what a package
is, is to download it and decrypt it locally.

### PUT /v3/files/<name>?offset=N

Uploads are chunked so an interrupted transfer resumes instead of restarting.

Headers:

- `Content-Length` — the chunk length, exactly one such header, no
  `Transfer-Encoding`. Required.
- `X-Total-Size` — the full package size. Required.

Rules:

- The body is one chunk of at most 1 MiB.
- `offset` is the byte position this chunk starts at; it defaults to `0` when
  the query is absent. It must equal the durable offset the server already
  holds — `0` for a name it has never seen — otherwise `409` with
  `{"offset":<durable offset>}`; continue from there.
- `X-Total-Size` must equal the size recorded for that name; a mismatch is also
  `409` with the durable offset.
- `201` when the name is created, `200` for every later chunk. The body is
  `{"offset":<new durable offset>,"complete":true|false}`.
- Re-sending a finished package returns `200 {"offset":<size>,"complete":true}`
  without writing anything. Identical bytes therefore cost one upload; two
  exports of the same plaintext are *different* packages, because the package
  format randomises its encryption, so a re-export is a new name, not an
  update.
- When the last byte of the declared size arrives, the server hashes the
  result. Bytes that do not hash to the name are discarded with `422` and
  `{"error":"hash mismatch"}`; the name disappears from the listing.
- `413` for a chunk over 1 MiB or a declared size over `--max-file-bytes`;
  `507 {"error":"quota exceeded"}` when the declared size would exceed
  `--quota-bytes`; `400` for a missing or invalid header, a chunk that would
  overrun the declared size, or anything other than one `Content-Length`.
- The quota counts the **declared** size as soon as a name is created, so a
  half-finished upload reserves its whole footprint.

A resumed upload: `HEAD` for the durable offset, then `PUT` from there. There
is no server-side session, so this works from a different device, process or
day.

### HEAD /v3/files/<name>

```
200
Content-Length: 4096          # declared full size
X-Offset: 1024                # durable bytes held
X-Complete: false
```

Works for unfinished uploads, which is what makes resume possible. `404` for a
name the server has never seen. No body.

### GET /v3/files/<name>

Complete packages only; an unfinished upload is `404`, so a client can never
import a truncated package.

- `200`, `Content-Type: application/octet-stream`, `Accept-Ranges: bytes`,
  `ETag: "<name>"`.
- `Range: bytes=a-b` or `bytes=a-` → `206` with
  `Content-Range: bytes a-b/size`. An unparsable or unsatisfiable range is
  `416 {"error":"invalid range"}`.
- Because the name is the hash of the bytes, a client can verify a download
  completely and locally before decrypting or importing it. Do that: size and
  digest are the only integrity the protocol offers.

### DELETE /v3/files/<name>

`204`, and the row and bytes are gone, including a partial upload under that
name. `404` if the name is unknown. Deletion is immediate, local and manual:
there are no tombstones, no history and no propagation, so the server does not
tell other devices that something was removed, and a device that still holds
the item will upload it again on its next pass.

## Limits

| Limit | Value |
| --- | --- |
| Package size | 1 GiB hard maximum (`--max-file-bytes`, 1 B…1 GiB, default 1 GiB) |
| Bytes in one data directory | default 10 GiB (`--quota-bytes`, up to 2⁶³−1) |
| `PUT` body | 1 MiB |
| Concurrent requests | default 16, 1…256 (`--workers`); excess connections are closed, not queued |
| Request deadline | default 30 s, up to 300 s (`--request-timeout`), covering the whole body |

Exceeding the package size or chunk size is `413`, the quota is `507`, and a
storage failure is `503 {"error":"storage unavailable"}`.

## Storage and durability

One data directory holds the whole database:

```
<data>/sync.sqlite3        WAL mode, synchronous=FULL
<data>/blobs/<name>        complete packages
<data>/blobs/<name>.part   in-progress upload
```

One table: `files(name PRIMARY KEY, size, offset, complete, seq)`, with
`PRAGMA user_version = 3`. A directory written by a newer protocol refuses to
open, so downgrading means restoring an older copy rather than pointing an old
binary at new data.

Per chunk the server writes, flushes and `fsync`s the file, `fsync`s the
`blobs` directory, and only then commits the new offset — an acknowledged
offset is always durable. On the final chunk it hash-checks, atomically renames
`.part` to the final name and commits `complete`. One writer transaction at a
time serialises offset updates and quota reservations.

On startup it recovers: tails written after the last acknowledged chunk are
truncated, a complete file whose digest matches is adopted, blobs no row
references are removed, and a `complete` row whose file is missing or the wrong
size stops the server instead of inventing bytes.

Backup is therefore a copy of that directory, taken while the server is stopped
(or as a `sqlite3 .backup` snapshot plus a copy of `blobs/`). Restore is
copying it back into an empty directory. The library key is not in the backup
and cannot be recovered from it.

## Threat model

What the relay, the host and anyone who steals a backup can see:

- the ciphertext of each package, its size, the number of packages, and the
  timing and volume of transfers;
- the package names, which are SHA-256 digests of ciphertext: they reveal that
  two packages are byte-identical, and nothing about their contents;
- connection metadata such as source addresses (the server itself logs no
  request, path, header, body or error detail).

What it cannot see: file names, folder paths, tags, any plaintext byte, the
vault password (which never leaves the device), or the library key. It holds
only `sha256(library key)`, so a stolen data directory does not hand anyone the
key and cannot be decrypted by the operator.

What a malicious relay can still do: withhold packages, serve an older set,
delete, corrupt and deny service. The hash-to-name binding means it cannot
substitute different bytes under an existing name without a preimage, so a
client that verifies size and digest is protected against tampering, but not
against a package that is simply missing or rolled back. There is no
transparency log and no global proof of freshness; that is a deliberate
non-goal, not a hidden property.

Key rotation is **not retroactive**. Starting the server with a new digest
changes who may authenticate; packages already in the bucket stay encrypted
under the old key and remain readable by anyone who kept it. Revoking a device
means rotating the key *and* deleting the old ciphertext, then re-exporting
from a trusted device. Likewise, losing the key everywhere means the packages
in the bucket can never be decrypted, and no server-side reset exists.

## Client requirements

- Check `GET /healthz` once per session and require `protocol: 3`.
- Export one item to one package and `PUT` it under the digest of its bytes;
  keep the exact staged bytes so a retry re-uses them (a re-export is a new
  name).
- Before importing a download, verify its size and SHA-256 against the name,
  and never import a partial.
- Remember which names this device already imported (a local ledger); the
  listing gives no other signal, and absence from it is not a delete request.
- Delete only on an explicit user action, one name at a time.

Running a relay, backing it up, recovering and rotating the key:
[self-hosted-sync.md](self-hosted-sync.md). The TLS container deployment:
[../sync_server/deployment/README.md](../sync_server/deployment/README.md).
