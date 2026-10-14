# Optional self-hosted encrypted sync

This service relays encrypted vault packages between independently encrypted local vaults. It is optional: normal local use, export/import and PC/LAN sharing continue to work without it. It replaces the old trusted-server `MegaVaultCloud` workflow, not the local vault.

Read [sync-architecture.md](sync-architecture.md) for the design and threat model and [sync-protocol.md](sync-protocol.md) for the API.

## Before enabling it

1. Back up each existing local vault before testing a new sync build.
2. Use a dedicated server data directory, not a vault directory. The server has no reason to open a real vault or receive its password.
3. Establish an HTTPS URL reachable by your devices. Keep the TLS certificate stable across restarts. A publicly trusted certificate avoids manually sharing a fingerprint; a private certificate requires explicit client pinning/trust.
4. Generate a new recovery secret on the first client and save it in your password manager. It is a random 32-byte value displayed as 64 hexadecimal characters, not your vault password.
5. Give subsequent devices that same recovery secret through a channel you trust, separate from the pairing code. Anyone with the secret and a copy of the encrypted packages can decrypt them.

Do not publish screenshots of pairing codes, device credentials or recovery secrets. Do not put them in URLs, command-line arguments, Git, logs or a public issue.

## Run directly

From the repository root with Python 3.12 or newer:

```sh
python -m sync_server --data ./sync-data serve --host 127.0.0.1 --port 8766
```

This loopback-only plaintext listener is for a same-host TLS reverse proxy, not for phone connections over public plaintext. A separate terminal administers the same data directory:

```sh
python -m sync_server --data ./sync-data pair-code
python -m sync_server --data ./sync-data devices
python -m sync_server --data ./sync-data revoke DEVICE_ID
```

A pairing code expires after ten minutes and can pair one device once. Generate a new one for each device. Revocation blocks that device's future requests; another device's token is not affected.

To serve TLS directly using a certificate whose private key is stored outside this repository:

```sh
python -m sync_server --data ./sync-data serve --host 0.0.0.0 --port 8766 \
  --cert /private/path/cert.pem --key /private/path/key.pem
```

Do not disable certificate verification as a troubleshooting shortcut. Use the actual HTTPS hostname on the certificate, or explicitly pin the certificate where supported. After an intended certificate/pin change, update clients deliberately.

The server is a single-tenant small-service MVP, not a hardened general-purpose public web platform. For internet-facing use place it behind the supplied TLS proxy; apply OS updates, firewall rules, proxy request/time limits and storage monitoring. Only the proxy should have a published network port.

## Docker deployment

The deployment files are in `deployment/sync/`. Use the supplied compose/Caddy configuration so TLS terminates at Caddy and the plaintext relay stays on a private container network. The persistent data volume must survive container recreation. Generate pairing codes and make backups with the server administration CLI inside the relay container; do not bake credentials into images.

Review the actual compose service names and required hostname settings in that directory before running it. Native execution is useful when Docker is unavailable; Docker is not required for the server or its tests.

## Pair and use clients

Unlock your normal local vault first, then open its **Sync** controls. Configure the HTTPS server URL, an optional explicit certificate fingerprint for a private certificate, a fresh pairing code, a non-sensitive device label and the separate recovery secret. The pairing request transmits only the pairing code and label. The recovery secret is used locally to encrypt/decrypt MVPK packages.

Choose the local items you want to publish and the remote objects you want to fetch. File names are encrypted, so a new remote object cannot be named by the server before the client downloads and decrypts it. Do not enable download-all merely to discover one file in a large library. Imported files are stored in your local vault and opened with its normal viewers, including while offline.

Keep the vault unlocked while sync is doing local export/import. Locking or leaving the app can pause work; a retry timer in an open app is not a promise of OS-level background execution. A staged upload remains encrypted and can resume later without starting again from zero. Watch the status rather than treating a login as proof that files are synchronized. Use **Retry** after correcting a network/storage problem. A revoked device must be paired again with a new code.

Concurrent edits are kept as separate copies rather than overwriting either edit. Remote history is retained. Remote deletion is conservative in this MVP: preserve your local copy and review the notice; do not expect an offline-edited file to be silently removed.

## Backups and restore

```sh
python -m sync_server --data ./sync-data backup /private/path/sync-backup
```

A backup must include a consistent server database and all referenced immutable encrypted blobs. Copying only the database loses the encrypted content. Keep the recovery secret separately; server backups intentionally cannot recover it. Also back up each local vault's complete root, using the existing local vault backup procedure with the app closed/locked.

To restore the service, stop it, preserve the current data directory, restore the backup into a clean directory, and point `--data` at that directory. Restore TLS configuration separately. Do not combine a database from one backup with unrelated blobs from another. Use a temporary port and disposable client before making a restored service authoritative. Device credentials in an old backup can predate revocations: reapply revoked-device records or revoke them again before reopening network access.

The database schema is versioned. Do not point an older server at a newer schema. Back up before upgrading and retain the old executable/image and data backup for rollback. The MVP retains history and partial uploads rather than promising automated retention; monitor disk space and request a documented garbage-collection policy before deleting blobs by hand.

## Security and cache limits

Transport uses TLS, and MVPK content, file names and folder paths are encrypted before upload. The relay stores device-token and pairing-code hashes rather than usable credentials. It still sees sizes, timing, device labels, object IDs and revision relationships. A malicious relay can withhold/replay old data; this is not a transparency-verified distributed system.

Each device has the shared package secret and its own vault password. Revocation cannot erase already downloaded files or a copied shared secret. After compromise, revoke the device and establish a new secret for newly encrypted packages; historical packages remain decryptable with the old secret. If all copies of the secret are lost, no server password reset can decrypt the relay's packages.

The persistent sync transfer cache contains encrypted packages. The existing MVPK core import path temporarily decrypts a file into a staging file before importing it into the local vault. Normal cleanup removes it, but crashes/filesystem snapshots may retain plaintext. Protect the client OS account, use full-disk encryption and avoid untrusted shared temporary storage. This transient import staging is different from the old plaintext media cache, which is not the new sync design.

Names/folder paths and contents travel in MVPK. Tags, empty-folder-only changes, binary deltas, real-time collaboration, automatic text merges and automatic destructive deletion propagation are not promised by this MVP. Large edits require a new encrypted package, although interrupted network transfers resume. History occupies real disk space; it is not a substitute for independent backups.

## Tests

```sh
python -m unittest discover -s tests/sync_server -v
```

Native package integrity tests are registered as `VideoVaultCore.SyncPackages` in CTest. Native sync state tests and end-to-end client checks must also pass before treating a development build as daily-use-ready. Server unit/integration tests alone do not establish that an Android or desktop UI has been tested on a device.
