# Optional self-hosted encrypted sync

> **Historical: protocol 3.** The shipped relay (`sync_server/`) and the desktop
> client speak **protocol 4** (accounts, vaults, versioned records). This document
> describes the earlier protocol 3 hash bucket, which the Android client still
> implements. Authoritative v4 references: `sync_server/README.md`,
> `sync_server/deployment/README.md`, `docs/sync-mega-plan.md`.

The relay moves end-to-end encrypted vault packages between devices that each
keep their own local vault. It is optional: normal local use, export/import and
PC/LAN sharing work without it. It replaces the old trusted-server
`MegaVaultCloud` workflow, not the local vault.

The design is in [sync-architecture.md](sync-architecture.md), the wire
contract and threat model in [sync-protocol.md](sync-protocol.md), and the
container deployment in [../sync_server/deployment/README.md](../sync_server/deployment/README.md).

## Before you start

1. Back up each local vault with the app's existing backup procedure, closed and
   locked, before testing sync on it.
2. Use a dedicated data directory for the relay. It never needs a vault, and it
   must never be pointed at one.
3. Have an HTTPS URL the devices can reach. A publicly trusted certificate (the
   Caddy deployment) needs nothing on the devices; a private certificate must be
   installed in their trust stores. Plain `http://127.0.0.1` is acceptable only
   when the client and server are the same machine.
4. Generate the library key once and put it in a password manager. It is 64
   lowercase hex characters. It is the only credential in the system — there is
   no pairing code, no device list and no per-device token — and it is also the
   secret that decrypts the packages, so anyone who has it and can reach the
   bucket can read, upload and delete everything in it.
5. Do not put the key in a URL, a screenshot, a shared shell history, a commit
   or an issue.

The server never stores the key and cannot print it again. It keeps only
`sha256(key)`, which is what `serve` is given and what every request is checked
against. Losing the key means the packages in the bucket can never be
decrypted again.

## Run it directly

Python 3.12+ and the standard library; no third-party packages. From the
repository root:

```sh
# 1. Print a new key once. --data is required but not created or read here.
python -m sync_server --data ./sync-data key
#   library key: 3f1c…c1   <- to every device, then into your password manager
#   key sha256:  9a40…77   <- to the server, safe to store

# 2. Serve. Loopback only by default, on 127.0.0.1:8766.
python -m sync_server --data ./sync-data serve --key-sha256 9a40…77

# 3. Check it.
curl -s http://127.0.0.1:8766/healthz
#   {"ok":true,"protocol":3,"files":0,"bytes":0}
```

If you would rather not paste the digest into a command line,
`serve --key-file /path/library.key` reads the key itself at startup and hashes
it. That file is the secret, so it must be private to the service account; the
server never writes it into the data directory or a log.

A plaintext listener is only accepted on a loopback address. Any other `--host`
requires TLS in the server:

```sh
python -m sync_server --data ./sync-data serve --host 0.0.0.0 --port 8766 \
  --cert /private/path/cert.pem --key /private/path/private-key.pem
```

`--cert` takes the certificate chain and `--key` its private key — not the
library key. TLS 1.2 or newer. Prefer the Caddy deployment: terminating TLS in
Python means you own renewal, and the plain port must never be exposed.

Limits are flags on `serve`: `--quota-bytes` (default 10 GiB for the whole data
directory, counting the declared size of unfinished uploads), `--max-file-bytes`
(default and hard maximum 1 GiB per package), `--workers` (default 16),
`--request-timeout` (default 30 s). The protocol document lists the exact
ranges and the status code for each failure.

## Point a device at it

Unlock the local vault, open its sync settings, and enter two things:

- the server URL, for example `https://sync.example.com`
- the library key from the step above

Nothing is exchanged beyond that: no pairing code to type on the other device,
no device name registered with the server, no token to revoke. Every device with
the URL and the key reads and writes the same bucket. Publish the items you
choose, and let each device import the packages it has not seen yet; file names
are encrypted, so a device learns what a package is only after downloading and
decrypting it locally. Keep the vault unlocked while export and import run, and
watch the status instead of assuming a configured server means synchronized
files.

## Backups and restore

A backup is a copy of the data directory — `sync.sqlite3` plus `blobs/` — taken
while the server is stopped, or a `sqlite3 sync.sqlite3 ".backup snapshot.sqlite3"`
plus a copy of `blobs/`. Do not copy `blobs/` without its database: finished
packages would no longer be listed, and a database row whose file is missing
stops the server at startup. The compose deployment has copy-paste commands in
[../sync_server/deployment/README.md](../sync_server/deployment/README.md).

The backup does **not** contain the library key, and no backup can recover it.
Store the key separately, and also keep normal backups of each local vault.
Restoring the relay is copying the directory back into an empty one and
starting `serve --data` against it. Never mix the database from one backup with
blobs from another.

The data directory records `PRAGMA user_version = 3`. A directory written by a
newer server refuses to open rather than being reinterpreted, so keep the
previous executable or image and its data backup for rollback.

## Pruning

Nothing is deleted automatically: not superseded packages, not abandoned
uploads, not packages no device references. To reclaim space, list and delete
by name:

```sh
export LIBRARY_KEY=...   # the 64-character key
curl -s -H "Authorization: Bearer $LIBRARY_KEY" http://127.0.0.1:8766/v3/files
curl -fsS -X DELETE -H "Authorization: Bearer $LIBRARY_KEY" \
  http://127.0.0.1:8766/v3/files/2f8a…e1.mvpk
```

A device that still holds the item uploads it again on its next pass, so prune
after the devices have dropped it. A `DELETE` of an unknown name answers `404`.

## Rotating the key

```sh
python -m sync_server --data ./sync-data key   # new key, new digest
```

Restart `serve` with the new `--key-sha256` and give every device the new key.
The old key stops authenticating immediately.

Rotation is **not retroactive**. Packages already in `blobs/` were encrypted
under the old key and stay readable by anyone who kept it; rotation only changes
who may reach the bucket. If the old key leaked, rotate *and* delete the old
packages — or start from an empty data directory — then re-export from a device
you still trust. There is no per-device revocation, so a lost or compromised
device is handled the same way: new key, new packages, old ciphertext removed.

## Honest limits

- **No history.** Editing an item creates a new package under a new name. The
  previous package stays in the bucket until you delete it, and nothing tells
  you it is stale.
- **No delta.** A one-byte change uploads the whole package again. Transfers
  resume after an interruption; they are not incremental.
- **Deletes are manual.** The server has `DELETE` and no propagation: other
  devices are not told, keep their local copy, and re-upload it if they still
  hold the item.
- **No per-device credentials.** One key for every device means no revocation,
  no attribution and no "this device only" access.
- **No merge.** Two devices that export the same item produce two independent
  packages; resolving them is a user decision.
- **No background sync.** Work happens while the app is open and the vault is
  unlocked. A retry timer in an open window is not an OS-level service.
- **Size and quota.** 1 GiB per package, 10 GiB per data directory by default;
  a large library needs a bigger `--quota-bytes` and the disk to match, and a
  single item over 1 GiB cannot be synced at all.

## Security notes

The relay sees ciphertext, package sizes, how many packages there are, and the
timing of transfers. It sees the package names, which are SHA-256 digests of
ciphertext: they reveal when two packages are byte-identical, nothing about
their contents. It does not see file names, folder paths, tags, contents, your
vault password (which never leaves the device) or the library key. It holds
only the key's digest.

A malicious or compromised relay can withhold packages, serve an older set,
delete them, or refuse service. Verifying a download's size and SHA-256 against
its name proves it is the exact package that was uploaded, but nothing proves
that the set it shows you is complete or current. There is no transparency log.

Each local vault keeps its own password; changing it does not change the
library key, and the sync key does not unlock the vault.

Import is not plaintext-free on the client. The package is authenticated first,
then each file is staged as plaintext at `<vault-root>/.import-staging/` (owner-only
on POSIX, inheriting the vault directory's ACL on Windows) before it is
re-encrypted into the vault. Normal cleanup removes it; a crash, a filesystem
snapshot or a backup taken at the wrong moment can retain it. Treat the vault
directory as confidential storage and use full-disk encryption. Sync encryption
does not make a compromised client safe.

## Tests

```sh
python -W error -m unittest discover -s tests/sync_server -v
```

They start real servers on real sockets — including TLS, resumed uploads after a
kill, quota, hash mismatch and restart recovery — against throwaway data
directories. Client-side suites live with the clients; neither replaces trying
two real vaults against your own relay.
