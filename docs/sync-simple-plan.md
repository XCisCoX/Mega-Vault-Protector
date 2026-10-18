# Simple sync — rewrite plan

> **Historical: protocol 3.** The shipped relay (`sync_server/`) and the desktop
> client speak **protocol 4** (accounts, vaults, versioned records). This document
> describes the earlier protocol 3 hash bucket, which the Android client still
> implements. Authoritative v4 references: `sync_server/README.md`,
> `deployment/README.md`, `docs/sync-mega-plan.md`.

Goal: replace the v2 relay (pairing, device tokens, CAS revisions, history,
tombstones, idempotency) with an encrypted hash-bucket. Same guarantee —
the server never sees names or contents — with a fraction of the moving parts.

## Design

The server is a folder of encrypted files behind one library key.

- Library key: one 32-byte secret, shown once, shared out of band. The server
  stores only its SHA-256 as the auth check; the key itself encrypts nothing on
  the server — it authenticates the client.
- Files are named by content hash: `<sha256-of-encrypted-bytes>.mvpk`. Same
  content therefore never collides and re-uploading is idempotent for free.
- Two devices agree because they see the same hash list. No revisions, no
  manifest, no merge.

### API (HTTP, JSON only for the listing)

| Method | Path | Purpose |
|---|---|---|
| GET | `/healthz` | `{ok:true, protocol:3, files:N, bytes:M}` |
| GET | `/v3/files` | `{files:[{name,size,seq}]}` newest `seq` first |
| PUT | `/v3/files/<64hex>.mvpk` | body = encrypted package; 201 created, 200 already present |
| GET | `/v3/files/<name>` | bytes, `Range` supported, `ETag` = name |
| DELETE | `/v3/files/<name>` | explicit, manual, one name per request; no propagation from clients |

Auth: `Authorization: Bearer <64hex library key>` on everything except health.
Wrong key → 401. No per-device identity, no revocation by design; rotating the
key means new packages.

Limits: 1 GiB per file, 10 GiB total, 1 MiB PUT chunks with a single
`X-Total-Size` header and an `?offset=N` query, resumable by HEAD (`X-Offset`).

Storage: one SQLite table `files(name PRIMARY KEY, size, offset, complete, seq)`
plus `blobs/<name>`; `.part` for in-progress uploads. Backup = copy the directory.

## Client behaviour

1. Export each selected item to one `.mvpk` (existing core API, existing
   recovery/secret handling reused as the library key).
2. PUT it under its hash. If 200, it was already there — skip.
3. Poll or refresh: GET the list, download any name not in the local ledger,
   verify hash, import, record it.
4. A local ledger (SQLite or the existing encrypted state file) maps
   `local item id -> package hash` so unchanged items are never re-uploaded and
   imported packages are never re-imported.
5. Delete is explicit in the UI, never automatic.

No queue retry machinery beyond "try again next pass": a failed upload simply
stays unsent because its hash is not recorded.

## What gets deleted

- `sync_server/__main__.py`: pair/pair-code/devices/revoke, codes, device
  tokens, `versions`, `operations`, CAS, history, manifest, tombstones.
- Qt: `SyncSession::manifest/receive/publish/conflict`, `moveItemsToFolder`
  conflict copies, `acceptRevision`, rollback detection, `cut`/paste sync
  plumbing that only existed to carry operation ids.
- Android: `SyncEngine` revision/CAS/idempotency paths, the pairing screen's
  code flow (one key + server URL is the whole setup).
- Protocol doc, tests and deployment docs shrink to match.

## Phases

1. **Server** — new `sync_server/` in the v3 shape above, with tests: auth,
   put/list/get/range/delete, idempotent re-put, resume, quota, corrupt hash.
2. **Qt client** — setup dialog becomes URL + key; upload/refresh/import loop;
   ledger; explicit delete. Remove the v2 engine paths.
3. **Android client** — same three actions; drop pairing codes.
4. **Cut over** — delete v2 endpoints and docs; the app refuses a v2 server with
   "this server is too old".
5. **Verify** — two independent vaults, one package each way, re-upload no-op,
   resume after kill, wrong key rejected, 100-file listing, delete propagation
   stays manual.

## Explicit non-goals

Revision history, per-device revocation, conflict merging, delta encoding,
background OS service, and automatic deletes. Simple means these are absent,
not hidden behind options.
