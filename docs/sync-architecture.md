# Sync architecture: an encrypted hash bucket

> **Historical: protocol 3.** The shipped relay (`sync_server/`) and the desktop
> client speak **protocol 4** (accounts, vaults, versioned records). This document
> describes the earlier protocol 3 hash bucket, which the Android client still
> implements. Authoritative v4 references: `sync_server/README.md`,
> `deployment/README.md`, `docs/sync-mega-plan.md`.

## Two shapes that did not fit

The app already had a trusted-server remote-access mode
(`qt-app/src/cloud_server.cpp`, `/v1/login`): the server opens the vault with a
password it receives, keeps the keys, and returns decrypted content. Any
password holder can read and delete everything, so it is not end-to-end
encrypted sync.

The first attempt at end-to-end sync was a general-purpose replication
protocol — pairing codes, device tokens, object revisions, compare-and-swap
commits, immutable history, tombstones and idempotency operation ids. It kept
the plaintext away from the server, but every one of those mechanisms was a
small state machine that could lose, duplicate or mis-order data, and all of
them existed to serve multi-device conflict resolution that a single user with
a handful of devices does not need.

## The decision

Sync is a directory of opaque, already-encrypted packages, each named by the
SHA-256 of its own bytes, behind a single library key:

```
<data>/blobs/<sha256-of-ciphertext>.mvpk
```

A device exports one selected item into one `.mvpk` package, uploads it under
its hash, and periodically lists the bucket. Any name it has not imported yet
is new to it: download, verify the digest, decrypt locally with the library
key, import into the local vault. Nothing else is exchanged.

The server stores bytes, a size and a name. It does not parse packages,
encrypt, decrypt, accept vault passwords, or hold the library key — only
`sha256(library key)`, which authenticates requests.

Why this is enough:

- **Content addressing replaces revisions.** The same bytes always have the
  same name, so a re-upload is a no-op (`200`, nothing written) and there is no
  manifest to keep in step, no base revision to compare, no conflict response.
- **Two devices agree because they see the same names.** No merge algorithm,
  no operation log, no idempotency tokens.
- **No per-device state.** One key replaces pairing codes, device tokens and a
  revocation table. The cost is that there is also no per-device revocation.
- **Storage is a directory.** One SQLite table plus `blobs/`: backup is a copy,
  recovery is copying back, and there is no server-side schema to migrate
  between protocol versions beyond a version number.

## What that costs, deliberately

- **No history.** Editing an item produces a new package with a new name. The
  old package stays until someone deletes it, so history is an accident of
  manual pruning, not a feature.
- **No delta.** A one-byte change re-uploads the whole package. Transfers are
  resumable, but not incremental.
- **No deletion propagation.** `DELETE` removes a package; devices are not
  told, and a device that still holds the item uploads it again on its next
  pass.
- **No conflict resolution.** Two devices that export the same item produce two
  independent packages. The protocol has no opinion about which is "current".
- **No revocation by device.** Possession of the key is the only identity.
  Losing a device means rotating the key and removing the old ciphertext.
- **Losing the key loses the data.** The operator can reset nothing.

Each of these is absent on purpose rather than hidden behind an option. See
[sync-simple-plan.md](sync-simple-plan.md) for the rewrite that produced this
design.

## Reusing the existing package format

Clients keep their normal local vaults and viewers. Export and import use the
project's existing authenticated MVPK package format, with the library key as
the package secret, so no second cryptosystem is introduced and no live
encrypted vault database is copied between devices. Copying a whole vault
directory while it is open could never merge two independently edited
databases safely.

Because the same secret both encrypts packages and authenticates to the relay,
the relay's operator sees the digest of the very secret that would decrypt the
bucket. That is acceptable only because the digest is not invertible and the
key never leaves the devices; it is also why a stolen data directory is not a
decryption key.

## Boundaries

- Selection is opt-in: a device uploads what the user picked and downloads what
  it has not imported. There is no "download everything on connect".
- A device keeps a local ledger of imported names so an unchanged item is never
  re-exported and an imported package is never re-imported.
- Import stages each file as plaintext inside the destination vault
  (`<vault-root>/.import-staging/`) before re-encrypting it. That is a real
  plaintext window on the client, and it is documented as one.
- No real-time collaboration, no background service while the app is closed, no
  multitenancy, no team roles.

## Where the details live

- [sync-protocol.md](sync-protocol.md) — endpoints, status codes, limits,
  storage and durability, and the full threat model.
- [self-hosted-sync.md](self-hosted-sync.md) — setup with a URL and a key,
  backup, recovery, rotation and the operational limits.
- [../deployment/README.md](../deployment/README.md) — the container
  deployment with TLS at Caddy.
