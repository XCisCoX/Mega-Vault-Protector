# Opaque encrypted sync server v2

The protocol is frozen in `docs/sync-protocol.md`. The relay stores opaque bytes; it does not parse MVPK, encrypt/decrypt packages, accept vault passwords, or distribute the recovery secret. Use Python 3.12+; no Python packages are required.

## Direct execution

From the repository root:

```sh
python -m sync_server --data ./sync-data serve --host 127.0.0.1 --port 8766
python -m sync_server --data ./sync-data pair-code
python -m sync_server --data ./sync-data devices
python -m sync_server --data ./sync-data revoke DEVICE_ID
python -m sync_server --data ./sync-data backup ./sync-backup
```

The backup destination must not exist and must be outside the data directory. Pairing codes are printed only by the administrative CLI, are single-use, expire after 600 seconds, and are stored only as SHA-256 hashes. Device tokens are independently random 256-bit credentials, also stored only as hashes. Pairing failures are limited to ten per source IP per sixty-second window, persisted across restarts. Behind Caddy this intentionally becomes a shared limit because the server does not trust forwarded IP headers.

Loopback plaintext is for development or a same-host TLS proxy only. A direct public bind requires both certificate and key:

```sh
python -m sync_server --data ./sync-data serve --host 0.0.0.0 --port 8766 --cert /private/cert.pem --key /private/key.pem
```

Direct TLS requires TLS 1.2 or newer. Clients must validate the certificate; never disable verification to make pairing work. Keep private keys outside this repository. Existing directories must be private to the service account; POSIX-created files use a restrictive umask, but Windows deployments require appropriate NTFS ACLs.

## Supplied TLS proxy deployment

Files are directly in `deployment/`, not `deployment/sync/`.

Copy `.env.example` to `.env`, set your real DNS hostname and ACME email, then run from the repository root:

```sh
docker compose --env-file deployment/.env -f deployment/compose.yaml up --build -d
docker compose --env-file deployment/.env -f deployment/compose.yaml exec server python -m sync_server --data /data pair-code
docker compose --env-file deployment/.env -f deployment/compose.yaml exec server python -m sync_server --data /data devices
docker compose --env-file deployment/.env -f deployment/compose.yaml exec server python -m sync_server --data /data backup /backups/snapshot-001
```

Only Caddy publishes host ports. The non-root relay uses a private internal network, read-only root filesystem, dropped capabilities, explicit process/memory bounds, and persistent data and backup volumes. Caddy has a separate network for ACME access. Allow inbound TCP 80/443 and point DNS at the host. Do not add a relay `ports:` mapping or attach untrusted containers to its private network.

Plaintext non-loopback proxy mode requires both `--proxy-mode` and `MV_SYNC_PROXY_MODE=1`. This is an explicit operator acknowledgement, not network isolation enforced by Python. Never enable it outside a genuinely private TLS-proxy deployment. Do not enable proxy access/debug logs that include credentials or sensitive URLs. The Python server intentionally emits no request logs, bodies, authorization headers, labels, or pairing secrets.

Image tags are major/minor templates, not reproducible digest locks: pin approved Python and Caddy image digests in your deployment. Docker and Caddy were unavailable on the implementation host, so image build, Compose startup and ACME issuance have **not** been executed there. Deployment security invariants are covered by static tests; direct server HTTPS is covered by actual TLS socket tests.

## Limits and durability

`serve --help` lists configurable limits. Defaults:

- 10 GiB blob storage reservation, including the **entire declared size** of partial uploads, not only their current offsets;
- 1 GiB per blob; 1 MiB maximum append body; 64 KiB maximum control JSON;
- 10,000 latest objects, including tombstones; 100,000 blob records, including partials and empty blobs;
- 16 active connections and a 30-second absolute request deadline, including TLS handshake and transfer. Excess connections are closed rather than queued in extra threads;
- five-second SQLite lock timeout. Storage contention returns retryable 503 where possible.

Changing limits never truncates the catalog or history. Quota failures are 507; oversized bodies/blobs are 413. Append reservation prevents partially uploaded data from bypassing quota. There is no automatic history or abandoned-upload GC: use monitoring and increase approved quotas rather than deleting files manually. Metadata/history storage is additional to the blob-byte quota.

Every acknowledged append is file-flushed before SQLite FULL-synchronous offset commit. Complete blobs are SHA-256 checked and renamed atomically; POSIX directory entries are flushed. Startup truncates unacknowledged append tails, resolves the rename-before-commit window, and removes unreferenced interrupted first writes. Missing acknowledged data fails startup rather than manufacturing replacement bytes. SQLite writer transactions serialize append offsets, quota reservations, pairing-code consumption, revocation, object CAS, operation-id replay and history insertion.

A backup holds a writer lock while producing a SQLite snapshot and copying all complete blobs plus acknowledged partial bytes. Complete copies are hash-verified and the database is integrity-checked before a new backup directory is atomically published. Large backups pause writes; clients may need to retry 503/timeouts. Backups contain device hashes and revocation state; restoring an old backup can undo later revocations, so reapply them before network exposure. Certificates and recovery secrets are not backed up by the relay.

Restore by stopping the service and pointing `--data` at a clean backup directory. Schema version 0 is initialized/migrated transactionally to version 1; a newer schema is refused. This is a local-filesystem SQLite deployment, not an NFS/distributed cluster service. Windows flush/rename paths are exercised, but neither physical power-loss guarantees nor POSIX directory-fsync behavior were tested on this Windows host.

## Tests

```sh
python -m unittest discover -s tests/sync_server -v
```

Tests spawn actual server subprocesses and use real TCP sockets, SQLite, filesystem storage and administrative CLI commands. TLS tests generate a temporary certificate using `openssl` and validate it with a real client trust store. Coverage includes resumable restart, disconnected request bodies, modeled filesystem crash windows, simultaneous appends/CAS conflicts, idempotency across restarts/device scopes, tombstones/history, revocation (including an upload already in flight), backup/restore, schema refusal, failed-pair rate limits, corrupted hashes, quota/oversize rejection, connection bounds/deadlines and secret-free logs.

Tests use opaque synthetic bytes rather than generating encrypted packages: MVPK encryption/import interoperability belongs to the native client/core tests. The server intentionally treats any byte sequence with a matching digest as opaque data. Real hardware power cuts, production load, Caddy/ACME/container execution and client UI integration remain outside this server test run.
