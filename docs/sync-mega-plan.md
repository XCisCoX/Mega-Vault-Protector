# MEGA-shaped sync — design and plan

Reference behaviour: MEGA (mega.nz). End-to-end encrypted blobs, per-file
objects, conflicts become copies, deletes become recoverable debris, transfers
are lazy and resumable. Applied to this vault, whose one non-file artifact is
the SQLCipher index.

## Hard rule that shapes everything

The vault database is **local and never uploaded**. One device writes its own
DB; two devices cannot clobber each other's rows because they never share the
file. What syncs is:

1. **Packages** — encrypted `.mvpk`, named by the SHA-256 of their own bytes.
   Already implemented; unchanged.
2. **Records** — a small metadata row per item: record id, name, folder path,
   tags, package hash, version, deleted flag. This is the only thing whose
   divergence needs a rule, and the rule is "both survive".

## Accounts and web

- `POST /auth/register` {email, password} → creates account. Passwords stored
  as Argon2id hashes; no plaintext, no password reset that reveals anything.
- `POST /auth/login` → session cookie (httpOnly, SameSite=Lax) for the website;
  `POST /auth/token` → bearer token for desktop/Android clients.
- A **vault** belongs to an account and has a display name and an opaque vault
  id. The website lists vaults, shows storage used, lets the owner rename or
  delete one, and rotates client tokens. It never decrypts anything: it holds
  no vault key material, ever.
- The **vault password is never sent**. The account password decides *which*
  vault you attach to; the vault password decides whether the local DB opens.
  Signing in with the account but not knowing the vault password leaves you
  with an unreadable DB — that is the intended property.

## Records and versions

`GET /v1/vaults/<id>/records?since=<cursor>` → `{records:[...], cursor}`.
Each record: `{id, version, name, path, tags, package, size, deleted, updated}`.

Write path is optimistic with a version check:

- `PUT /v1/vaults/<id>/records/<record id>` with `{base_version, name, path,
  tags, package, size, operation}`.
- `base_version` equal to the stored version → version +1, accepted.
- `base_version` stale → **409 with the current record**. The client does what
  MEGA does: does not overwrite, does not merge — it re-issues its own copy as
  a **new record id** with a name suffix `(conflict copy)` and uploads that.
  Both items exist; the user can delete either.
- Idempotent by `operation` id so a retry after a lost response is free.

Deletes set `deleted` on the record (a tombstone that also serves as the debris
entry). The UI shows "Removed on another device — restore or delete" rather
than silently dropping local data. Restore clears the flag. A separate
`DELETE /v1/vaults/<id>/records/<id>?purge=1` is the explicit permanent one.

## Blobs

Unchanged from the v3 bucket: `PUT /v1/vaults/<id>/blobs/<64hex>` with
`X-Total-Size` and `?offset=`, `HEAD` for the durable offset, ranged `GET`,
`DELETE`. Blobs are content-addressed, so a conflict copy that shares content
costs nothing.

## Lazy client behaviour

1. Sign in with the account; choose the vault folder.
2. Fetch the record cursor and the record list (small, fast). The local DB is
   the cache: records already present and matching are not touched.
3. Anything missing locally is queued for download and imported as it lands.
   Anything local and not yet on the server is queued for upload.
4. The UI is usable immediately: files already local play/open; queued items
   show a pending marker; the transfer bar reports counts and bytes.
5. Progress is durable — a queue entry survives restart, and a partial download
   resumes from its offset (ranged GET).
6. Deletes propagate as tombstones (deferred, reversible), never as a silent
   remote wipe.

## Endpoints (v1, versioned separately from the old v2/v3 labels)

| Method | Path |
|---|---|
| POST | `/auth/register`, `/auth/login`, `/auth/logout`, `/auth/token` |
| GET | `/v1/vaults` (web + clients), `POST /v1/vaults` |
| GET | `/v1/vaults/<id>/records?since=` |
| PUT | `/v1/vaults/<id>/records/<record>` |
| POST | `/v1/vaults/<id>/records/<record>/restore` |
| DELETE | `/v1/vaults/<id>/records/<record>` (tombstone; `?purge=1` permanent) |
| PUT/HEAD/GET/DELETE | `/v1/vaults/<id>/blobs/<64hex>` |
| GET | `/healthz` (no auth) |

All client routes require `Authorization: Bearer <token>` and are scoped to the
caller's vault; a token for one vault cannot read another.

## Website scope (deliberately small)

Register, log in, list vaults, create a vault, show usage, rename, delete,
issue/revoke a device token, show "how to connect" with the app. No file
browsing, no sharing links, no team features — those would need the vault key
on the server, which this design forbids.

## Phases

1. **Server + web**: accounts, sessions, tokens, vaults, records with version
   checks and conflict 409s, tombstones/restore, blob routes, SQLite schema,
   and a static web UI for the five screens above. Tests: auth, cross-vault
   isolation, record version conflict returns the current row, idempotent
   operation replay, tombstone/restore/purge, blob resume, quota.
2. **Desktop**: replace the current Sync dialog with "Sign in" → vault list →
   "Choose folder" → lazy sync loop + transfer panel. Keep the ledger, extend
   it with record ids and versions.
3. **Android**: same flow with the account token in Keystore.
4. **Conflict UX**: name suffix `(conflict copy)`, pending markers, debris list.
5. **Verify**: two devices, same account, same vault; import on A appears on B;
   simultaneous rename on both produces two items, neither lost; delete on A
   shows as debris on B and restores; kill mid-download and resume; wrong
   account password rejected; token for vault A cannot read vault B.

## Explicit non-goals

Server-side decryption, file sharing links, team roles, revision history,
binary deltas, and automatic destructive deletes. Conflicts are copies; deletes
are reversible. That is the MEGA model, and it is what keeps this safe without
a write lease.
