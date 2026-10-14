# Sync architecture decision (issue #3)

## Why the old cloud server does not meet the goal

The former `qt-app/src/cloud_server.cpp` opens a server-side vault using a password received at `/v1/login`, retains that password and vault keys for the process lifetime, and returns decrypted content. Every password holder has deletion authority. It is therefore a trusted-server remote-access service, not end-to-end encrypted sync.

The old implementation has no upload or revision protocol, no independently revocable persistent device credentials, and no durable offline write queue. Its server certificate changes at each restart. The desktop browser downloads all files into a plaintext cache keyed only by numeric item ID (not server identity), and a failed item stays in a permanent failed set. These properties make disconnected daily use, restart recovery, large libraries, multiple servers and safe concurrent edits unreliable.

The Android legacy cloud mode similarly uses server-side decryption and plaintext cached media. The existing PC/LAN sharing feature is separate: it intentionally trusts the PC hosting an unlocked vault. It is not the new sync security model and is not being removed.

## Chosen model

The user explicitly chose end-to-end encryption. A single-user, optional, self-hosted service stores opaque encrypted MVPK packages and revision metadata. It has no dependency on the C++ vault core, FFmpeg, SQLCipher or Qt. Python's standard library provides the small server, SQLite stores catalog/device/idempotency state, and immutable files hold encrypted blobs. Use a TLS reverse proxy for internet-facing deployment, or the server's certificate/key options on a private network.

Clients continue to use their normal local vaults and existing viewers. They export one selected item into an MVPK encrypted using a separate random shared recovery secret, upload the encrypted package, download other revisions, authenticate/decrypt on the device, and import into their own independently encrypted vault. A server pairing code authorizes a device; it does not contain, distribute or recover the decryption secret. Pairing never sends a vault password.

This reuses the project's existing authenticated streaming package format rather than introducing a second cryptosystem or copying a live encrypted SQLite database between devices. Copying whole vault directories while they are open would not safely merge independently edited databases.

## Practical synchronization contract

* Selection is opt-in. Do not upload or download an entire private library merely because a device was paired.
* A stable object ID links revisions; local vault database IDs are not shared across devices.
* Stage encrypted bytes plus a pending record durably before starting a network write. Reuse the exact package, object ID and operation ID after interruption.
* Query the stored upload offset before resuming, transfer bounded blocks, verify SHA-256 and size before import, and only mark a revision seen after import succeeds.
* Commit against the last acknowledged revision. A stale base produces a conflict, not a last-writer-wins overwrite. Publish a separate conflict copy while preserving both originals and immutable history.
* Keep local changes during an outage. Retry with bounded backoff; distinguish authentication failures, network failures, conflicts and pending work. The word "Synced" must not hide an error or queued work.
* Revocation blocks future API access. It cannot erase plaintext or shared secrets already copied by that device.
* Preserve local files on remote deletion rather than silently deleting an offline edited copy. Deliberate destructive cross-device propagation is deferred.

See [sync-protocol.md](sync-protocol.md) for the endpoint and persistence contract. Setup, recovery and backups are documented in [self-hosted-sync.md](self-hosted-sync.md).

## Threat model and recovery

A storage administrator or someone who steals a server backup can see device labels, sizes, transfer timing, random object identifiers and revision relationships. They cannot decrypt properly exported MVPK content, names or directory paths without the separate recovery secret. A compromised paired device can decrypt its selected files and can make authorized changes. A malicious server can withhold data or replay history; remembered decreasing revision numbers can be detected locally, but the MVP has no global rollback/transparency proof.

Loss of the recovery secret is not reset by the server administrator. Recover from a paired device or the user's separately stored recovery copy. If the secret is lost everywhere, retained encrypted packages cannot be decrypted. After device compromise, revoke its token and establish a new shared secret and new encrypted packages for future data. Old ciphertext remains decryptable with the old secret; do not promise retroactive revocation.

Each local vault retains its own vault password. Changing that password does not change the sync recovery secret or revoke another paired device.

MVPK import currently uses a temporary plaintext staging file inside the existing core import path before re-encrypting into the destination vault. This is not a persistent sync/media cache. Normal cleanup removes staging files, but process crashes, storage snapshots and filesystem recovery may retain them. The implementation and deployment guide must identify the staging location accurately; OS permissions and full-disk encryption matter. Server E2EE does not make a compromised client safe.

## Deliberate MVP limits

Whole-file encrypted packages are resumable; changing a file requires a new encrypted package, not a binary delta. There is no real-time editing, automatic text merge, multitenancy or team roles. The initial package format carries content, names and directory paths, but not tags or empty-folder-only changes. Do not claim complete metadata parity. Remote history is retained without automatic pruning, so operators must monitor disk use and back up the service. Client sync while the app is closed is not implied by a UI retry timer.

## Verification contract

Server tests must exercise real requests for pairing, revocation, resumed transfer/restart, hashes, limits, CAS/idempotency/history and consistent backup. Native tests must exercise queue persistence, error/retry state and the actual MVPK export/import boundary. The critical end-to-end scenario is two independent local vaults syncing a file without revealing it to the relay, followed by an offline edit, reconnect and concurrent conflict. A successful build is not a substitute for this scenario or for native UI testing.
