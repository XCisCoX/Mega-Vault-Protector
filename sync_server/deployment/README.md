# Self-hosted sync relay — protocol 4

This directory deploys the whole server: a Caddy TLS edge, the protocol 4 relay,
one persistent data directory. The relay stores opaque, already-encrypted
`.mvpk` packages named by the SHA-256 of their own bytes, plus the small
metadata rows that describe them (record id, name, folder path, tags, package,
size, version, deleted flag). It never holds a vault password and cannot decrypt
anything; the client encrypts, the client names the package, and the vault
password stays on the device.

Accounts, sessions, vaults and device tokens all live in the data directory, so
there is nothing to generate before the first start and no secret in the
container's argument list.

Contracts: [docs/sync-mega-plan.md](../docs/sync-mega-plan.md) for the design and
[sync_server/README.md](../sync_server/README.md) for the wire protocol. Pages
under `docs/` that still describe protocol 3 are historical; those two are the
current ones.

## What you need

- A hostname whose DNS record points at this host, plus inbound TCP 80/443 for
  the ACME challenge and for clients.
- Docker with Compose v2.

Python 3.12+ on the host is optional: the operator CLI can run inside the image
instead (step 5).

## 1. A certificate for the Caddy-to-relay hop

Public TLS is terminated by Caddy, with a certificate it obtains automatically
for `SYNC_DOMAIN`. The hop between Caddy and the relay is also TLS, because
`serve` refuses to expose a plaintext listener on a non-loopback address — that
is a server invariant, not an operator switch:

```text
$ python -m sync_server --data ./sync-data serve --host 0.0.0.0
sync_server: error: non-loopback binding requires --cert and --key (TLS)
```

So generate a throwaway self-signed pair. No client ever sees it; it only
protects the internal container network:

```sh
mkdir -p sync_server/deployment/tls
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -subj "/CN=server" -addext "subjectAltName=DNS:server" \
  -keyout sync_server/deployment/tls/relay-key.pem -out sync_server/deployment/tls/relay-cert.pem
chmod 600 sync_server/deployment/tls/relay-key.pem
```

Keep both files out of version control (`sync_server/deployment/tls/.gitignore` ignores
`*.pem`). The relay runs as uid `10001`, so on a Linux host make the pair
readable by that account — `sudo chown 10001:10001 sync_server/deployment/tls/relay-*.pem` —
while Docker Desktop handles ownership for you. Regenerate whenever you like and
restart the relay to pick up a new pair; Caddy does not verify this certificate
(see the note in `Caddyfile`), so nothing has to be distributed to clients. If
the pair is missing or unreadable the relay exits at startup with a
`FileNotFoundError` or `PermissionError` and compose restarts it until you fix
that.

## 2. Configure `.env`

```sh
cp sync_server/deployment/.env.example sync_server/deployment/.env
```

Fill in:

- `SYNC_DOMAIN` — the public hostname Caddy serves and obtains a certificate for.
- `ACME_EMAIL` — ACME contact address.

Both are required: compose refuses to interpolate the file while either is
empty, so a typo stops `up` instead of starting a relay nothing can reach. Do
this before any `docker compose run` — compose parses the whole file even when
it only runs one command.

`SYNC_QUOTA_BYTES` and `SYNC_MAX_FILE_BYTES` are optional; leave them alone to
accept the defaults in the table below. There is no secret in this file —
whoever can read it learns your hostname, and nothing else.

## 3. Start

```sh
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml up --build -d
```

Only Caddy publishes host ports. The relay publishes none, sits on an
`internal: true` network, runs as the non-root uid `10001` with a read-only root
filesystem, `cap_drop: [ALL]`, `no-new-privileges`, a pid limit of 128 and a
256 MiB memory limit, and keeps its data in the named volume
`megavault-sync-data` mounted at `/data`. Caddy starts only after the relay
answers its health check, and that check insists on `protocol: 4`, so an older
server answering on the same volume is caught before the edge goes live.

Caddy proxies `/v1/*` (vaults, records, blobs), `/auth/*` (register, login,
logout, sessions, tokens), `/healthz` and the static site to the same upstream.
No access log is configured, at either tier.

## 4. Verify the stack

```sh
curl -fsS https://sync.example.com/healthz
# {"ok":true,"protocol":4}
```

`protocol` must be `4`. A 404 here means something other than this server is
answering; a different protocol number means an old deployment is still
listening. Then load `https://sync.example.com/` — the sign-in page coming up is
what proves the image carries the static UI. A 404 on `/` while `/healthz` works
means `sync_server/web/` did not make it into the image.

`/healthz` is the only unauthenticated route; everything under `/v1` answers
`401` without a token.

The relay writes nothing per request (no paths, no query strings, no headers, no
bodies, no errors), so startup failures are the only thing in
`docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml logs server`.

## 5. First run: one account, one vault, one token per device

### 5a. Create the account

Three equivalent ways. Pick one; the account is the same object in all of them.

**The container CLI** (no Python on the host):

```sh
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml \
  run --rm -it server account create owner@example.com
# password:        <- typed twice, not echoed
# account: 6f2c…   <- the account id
# email: owner@example.com
```

This runs against the same volume the relay uses and takes effect immediately;
the relay does not need restarting.

For scripting, avoid the prompt with `--password-stdin` and `-T`:

```sh
printf '%s\n' "$ACCOUNT_PASSWORD" | docker compose --env-file deployment/.env \
  -f deployment/compose.yaml run --rm -T server account create owner@example.com --password-stdin
```

**The host CLI**, if Python 3.12+ is installed and you would rather not involve
Docker for this:

```sh
python -m sync_server --data ./sync-data account create owner@example.com
```

Point `--data` at the directory the container uses if you want the account to be
visible to the relay; with the compose file below that is the named volume, so
prefer one of the container forms.

**The website**: open `https://sync.example.com/`, press **No account? Create
one**, and fill in **Create account**. Same account, same password rules (at
least 10 characters).

The password is hashed with scrypt and a per-account salt; neither the password
nor a reverser is stored, and there is no reset that reveals anything. The
account password decides *which* vaults you own — it does not decrypt a vault.
The vault password is typed on the device and never sent here.

### 5b. Sign in and create a vault

On the website: **Sign in**, then under **Your vaults** type a name and press
**Create vault**. Or with the API:

```sh
curl -fsS -c jar -X POST https://sync.example.com/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"owner@example.com","password":"…"}'

curl -fsS -b jar -X POST https://sync.example.com/v1/vaults \
  -H 'Content-Type: application/json' -d '{"name":"Archive"}'
# {"vault":{"id":"<32 hex>","name":"Archive",…}}
```

The vault id is opaque. Devices list vaults after signing in, so you rarely type
it; the CLI can print it: `python -m sync_server --data DIR vault list
owner@example.com` (tab-separated `id`, name, bytes, records).

### 5c. Issue a device token for each app

One token per device, so one revocation never disturbs the others.

On the website, under **Devices**: type a label (for example `Laptop`) and press
**Issue token**. The 64-character token is displayed **once** — copy it straight
into that device. Or from the CLI:

```sh
python -m sync_server --data ./sync-data token issue owner@example.com --label Laptop
# token: 3f1c…      <- shown once, give it to that device
# id: 9a40…         <- sha256 of the token; this is what revocation uses
# label: Laptop
```

The server stores only `sha256(token)`. A token is scoped to the account that
issued it and can read and write that account's vaults — records and packages
require a bearer token, and the website cookie is refused on those routes.

### 5d. Point the app at the relay

On the device, open **Settings → Sync** and enter:

- **Server URL**: `https://sync.example.com`
- **Token**: that device's token from step 5c

Then pick the local vault folder. MegaVault asks for the **vault password**
locally; it is never transmitted. Repeat 5c–5d for every app: desktop, Android,
a second laptop.

## 6. Devices: adding, losing, revoking a token

- **Add**: step 5c. Every device gets its own token; a second app on the same
  account is a second token, not a copy of the first.
- **Lose a device**: revoke its token. On the website under **Devices** press
  **Revoke** next to it, or:

  ```sh
  curl -fsS -b jar -X DELETE https://sync.example.com/auth/token/<id>
  ```

  The device's next request is a `401`; nothing needs restarting and the other
  devices are untouched. The revoked device still has its local files — they are
  its own copy — but it can no longer exchange anything with the relay.
- **Re-enable a device**: issue a new token, paste it into `Settings → Sync`.
  The vault password does not change.

## 7. Backup and restore

A backup is a copy of the data directory: `sync.sqlite3` plus `blobs/`. Stop the
relay first so nothing is mid-write, then stream a tarball to the host (no bind
mount, so it behaves the same on Windows, macOS and Linux):

```sh
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml stop server
docker run --rm -v megavault-sync-data:/data:ro alpine \
  tar czf - -C /data . > sync-data-2026-10-15.tar.gz
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml start server
```

With SQLite tooling on the host, `sqlite3 sync.sqlite3 ".backup snapshot.sqlite3"`
plus a copy of `blobs/` is equally consistent and does not need the relay
stopped. Either way the database and `blobs/` must come from the same moment: a
package is served only when its row says complete and its size matches, and a
complete row whose file is missing makes the server refuse to start.

Restore into an emptied volume:

```sh
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml stop server
docker run --rm -i -v megavault-sync-data:/data alpine \
  sh -c 'rm -rf /data/* && tar xzf - -C /data && chown -R 10001:10001 /data' \
  < sync-data-2026-10-15.tar.gz
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml start server
```

The backup keeps the service recoverable; it does not make a vault readable.
Packages are encrypted on the devices and the vault password never reaches this
host, so a stolen backup is a set of sealed blobs.

## 8. Deleting a vault, purging a package

**Delete a vault** — on the website, press **Delete** next to it and confirm, or:

```sh
curl -fsS -b jar -X DELETE https://sync.example.com/v1/vaults/<32 hex id>
```

That is immediate and permanent for the server: the vault's records, rows and
packages are removed. The vault id never comes back, so a device still pointed at
it starts getting `404` and has to be re-pointed at a new vault. Devices keep
their local copies of the files — the confirm dialog says so — and a device that
still holds items for a *recreated* vault will upload them again.

**Records are tombstones by default.** `DELETE /v1/vaults/<id>/records/<record>`
marks the row deleted (version bumped) instead of dropping it, so other devices
can show "removed on another device — restore or delete", and `POST
.../restore` clears the flag. `?purge=1` is the explicit permanent delete.

**Packages** can be deleted by name with `DELETE
/v1/vaults/<id>/blobs/<64hex>`; it answers `204` and is immediate. A device that
still holds the item uploads it again on its next pass, so purge after the
devices have dropped it. Nothing is pruned by age, and a full quota refuses new
uploads with `507` instead of deleting anything.

## 9. Limits and failure modes

| Limit | Default | Flag |
| --- | --- | --- |
| Bytes per **account**, counting the declared size of unfinished uploads | 10 GiB | `--quota-bytes` |
| One package | 1 GiB (1 GiB is also the maximum accepted) | `--max-file-bytes` |
| One `PUT` body | 1 MiB | fixed |
| Concurrent requests | 16 (excess connections are closed, not queued) | `--workers` |
| Request deadline, including the body | 30 s | `--request-timeout` |

`507` means the quota is full, `413` an oversized package or chunk, `409` a stale
record version (the body carries the current row) or a stale upload offset (the
body carries the offset to resume from), `422` bytes that do not hash to their
name, `401` a missing, wrong or revoked token, `404` a vault or record that does
not belong to the caller, `503` a storage error.

Stopping the relay — for a backup, a restore, an upgrade or a vault deletion — is
a connection failure for every client until it is back. Clients keep their queued
work and retry on the next pass, so a short outage costs time, not data.

## 10. What the operator sees

The host, the disk and anyone who steals a backup see ciphertext, each package's
size, the number of packages, which account owns which vault, and transfer
timing. The package name is the SHA-256 of the ciphertext, so it reveals equality
(the same bytes always have the same name) but no file name, folder path, tag or
content. There is no plaintext column anywhere: account emails, vault names,
token hashes, record metadata and blob sizes are all this server ever learns.

It still cannot be made to prove it served the newest data: a malicious or
compromised relay can withhold, drop or replay packages, which is why clients
verify the hash of every downloaded package before importing it.

## 11. Upgrading to protocol 4

A data directory written by an older protocol is refused at startup with a clear
message rather than half-migrated, because packages from that era have no owner
to attach them to an account. Start from an empty directory:

```sh
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml down
docker volume rm megavault-sync-data
docker compose --env-file sync_server/deployment/.env -f sync_server/deployment/compose.yaml up --build -d
```

Then run step 5 again and let the devices upload. Packages are
content-addressed, so a device that still has them pays only the bandwidth, and
a mismatch would be caught by the hash check anyway.

## 12. Without Docker

`serve` alone is a complete server; Caddy is only there for the public
certificate.

```sh
# Same-host reverse proxy in front of a loopback, plaintext relay:
python -m sync_server --data ./sync-data serve            # 127.0.0.1:8766

# Or bound directly, with TLS (a non-loopback host requires it):
python -m sync_server --data ./sync-data serve \
  --host 0.0.0.0 --port 8766 \
  --cert /path/relay-cert.pem --key /path/relay-key.pem
```

The console is resolved as `sync_server/web/` inside the package, or
from `--web DIR`. Cookies are marked `Secure` when the request arrived over TLS
or carries `X-Forwarded-Proto: https`, so a plaintext proxy on another host would
hand out cookies over plain HTTP — keep that hop on loopback.

## 13. Tests

```sh
python -W error -m unittest discover -s tests/sync_server
```

43 tests. They start real servers on real sockets, including TLS, resumed uploads
after a kill, quota and hash-mismatch paths; `tests/sync_server/test_deployment.py`
pins the deployment invariants above (no published relay port, internal network,
read-only root, dropped capabilities, non-root user, `443` only at Caddy).

## 14. What was executed, and what needs a real Docker host

Docker, Caddy and a WSL distribution are all absent from the machine this
revision was written on, so the following were **not** run: `docker compose
config`, `docker compose up --build`, the image build, Caddy accepting this
`Caddyfile`, ACME issuance, and the container health check under a read-only root
as uid `10001`.

What *was* run against the real files:

- Compose interpolation replayed with compose's own semantics (`${V:-default}`,
  `${V:?message}`, `$$`), reading `sync_server/deployment/.env.example` as the env file: every
  token resolves, the quota and max-file defaults land as `10737418240` and
  `1073741824`, an empty environment stops on `SYNC_DOMAIN`, and no flag or
  environment variable from the previous CLI appears anywhere in the resolved
  document.
- Structural assertions on the resolved text: no `ports:` on the relay,
  `internal: true`, `read_only: true`, `cap_drop: [ALL]`,
  `no-new-privileges:true`, `user: "10001:10001"`, `pids_limit: 128`,
  `mem_limit: 256m`, the named volume on `/data`, `80:80`/`443:443` only at
  Caddy, and `depends_on: service_healthy`.
- The exact argv the container would receive — the image `ENTRYPOINT` plus the
  compose `command` — executed directly on Python 3.14, with two substitutions
  only: container-internal paths mapped to local ones, and port `8766` changed to
  `18766` because an unrelated protocol-2 server already held `8766` on this host.
- Against that live TLS listener bound to `0.0.0.0`: `/healthz` returned
  `{"ok":true,"protocol":4}`; `/`, `/app.js`, `/style.css` and `/icon.svg`
  returned `200` with the right content types (proving the `sync_server/web` copy
  resolves at the path the image creates); `POST /auth/register` created an
  account, `POST /auth/login` set a session cookie, `POST /v1/vaults` created a
  vault, `POST /auth/token` issued a device token, the bearer token listed the
  vault, and a blob `PUT`/`HEAD`/`GET` plus a record create/list round-tripped
  intact.
- The health-check `python -c` snippet from `compose.yaml`, run verbatim except
  for the port: exit `0` while the listener was up, non-zero once it was
  stopped.
- `serve --host 0.0.0.0` without `--cert`/`--key` still refused with
  `non-loopback binding requires --cert and --key (TLS)`.
- `python -W error -m unittest discover -s tests/sync_server`: 43 tests, OK.

The remaining risk is therefore the container layer itself: the image build, the
read-only root, uid `10001` writing the volume, and Caddy accepting the
`Caddyfile`. Run `docker compose --env-file deployment/.env -f
deployment/compose.yaml config` and then `up --build -d` on a Docker host, and
check `/healthz` through the public domain.
