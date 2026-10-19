# Native sync protocol 4 tests

The client is `MegaVaultSync` (`sync_state.cpp`, `sync_client.cpp`,
`sync_engine.cpp`, `sync_page.cpp`), linked into `VideoVaultApp`. Sync in the
unlocked library is a **page** in the main window's content stack, not a
dialog: **sign in** (server URL, account email, account password) on one
centred card, then the account's vault rows, an inline **Choose folder**
control, one **Sync now** button, a quiet last-sync line and an inline
transfer bar, with tombstones under "Removed on another device" as rows whose
own actions are Restore and Delete permanently. There is no library key any
more and no flat bucket: the server holds accounts, vaults, versioned records,
tombstones and content-addressed packages, and the client speaks that protocol.

The vault password is *not* asked for on the page. It was already entered to
unlock the database, and the two secrets sync needs are derived from it while
the unlock worker still has it in hand:

* `package` — 64 lowercase hex, the `.mvpk` password. Derivation is device
  independent on purpose, so two devices of one vault derive the same value and
  can open each other's packages. It is never sent anywhere.
* `state` — 32 bytes, the XChaCha20-Poly1305 key for `state.enc`, keyed with this
  device's random salt. It protects the device token and the record map, so a
  copied state file is useless without the vault. The key is wiped on lock.

## What each test covers

| Target | Covers | Needs a server |
|---|---|---|
| `VideoVaultSyncStateTests` (`sync_state`) | key derivation (shared package password, per-device state key, stable salt), encrypted state round trip, the token is not on disk in the clear, wrong-key / tamper / truncation refusal, missing state, record bookkeeping, `(conflict copy)` naming, operation-id determinism, URL/email/record-id/package validation | no |
| `VideoVaultSyncEngineTests` (`sync_engine`) | sign in and token at rest, vault + folder choice (a foreign folder is refused), publish with base_version and an operation id, record cursor paging (client pages of three and an engine page limit of two), a second device receiving, tombstone then restore, version conflict producing a conflict copy with nothing lost, operation replay not duplicating, a full pass writing nothing when up to date, round trip in the other direction with a tag, a queued record waiting for its bytes and then resuming a partial download, delete permanently sparing a shared item, and a server read-back in the harness | yes — skipped (exit 77) without arguments |
| `VideoVaultSyncDialogTests` (`sync_page`) | the sync surface is a page, not a dialog (no `QDialog`, tab or checkbox anywhere in it), it opens on one centred sign-in card whose three fields are labelled by placeholders, exactly one masked password field and no vault-password or library-key field, an inline error line under the button instead of a popup, empty URL / malformed email / empty password refused locally, Choose folder refused for a foreign folder and shown by name with the path in the tooltip, Sync now gated until an account plus vault plus folder exist, a local item's pending state stated on the quiet line, the page interactive while a request runs, an unreachable server failing without hanging, Sync without sign-in saying to sign in | no (offscreen) |

`sync_engine` returns 77 (`SKIP_RETURN_CODE`) when it is started without a
server, so a plain `ctest` run states plainly that it did not run.

## Running them

```sh
export PATH=/c/Users/parvizi/AppData/Local/msys64/ucrt64/bin:$PATH   # Qt on PATH
python C:/Users/parvizi/AppData/Local/MegaVaultProtect/native-build/native_build.py \
    build --target VideoVaultSyncStateTests VideoVaultSyncEngineTests VideoVaultSyncDialogTests
python C:/Users/parvizi/AppData/Local/MegaVaultProtect/native-build/native_build.py \
    test --filter '^sync_'
```

The live integration test needs the real server, run from the repository root:

```sh
python qt-app/tests/run_sync_live.py \
    <build>/qt-app/VideoVaultSyncEngineTests[.exe]
```

The harness provisions everything through the server's own operator CLI —
`account create` (password on stdin), `token issue` — creates two vaults with
that token, serves the throwaway data directory on a free loopback port, and
passes the URL, token, account credentials and both vault ids to the native
executable. Afterwards it reads the server's SQLite database back: every blob
row is complete and correctly sized, every file is named by the SHA-256 of its
own bytes, every record's package exists, and no `.part` files are left. Nothing
is mocked and no real credentials are used or printed.

## Boundary this client keeps

* Records, not files, are the sync unit. A record is
  `{id, version, name, path, tags, package, size, deleted, updated}`; a package
  is still named by the SHA-256 of its own bytes, so a conflict copy that shares
  content costs no extra storage.
* **Both devices of one vault must use the same vault password**, because that
  password is what makes the packages readable to each other and the server
  holds no key. A different password is a different vault as far as the
  packages are concerned.
* Export is not deterministic — every export is sealed with a fresh nonce — so
  the state remembers two hashes per record: the server's package address and
  the local item's own package hash. "Unchanged" compares the local hash, and a
  rename or a tag change re-uses the package the server already has instead of
  uploading the content again.
* A pass publishes first, then pulls, then downloads the queue, then tombstones
  records whose local item has gone. Publishing first is what makes a
  simultaneous rename on two devices a 409 instead of a silent overwrite; the
  loser re-issues itself under a new record id with the `(conflict copy)`
  suffix, and a second copy is never minted on later passes.
* Tombstones keep the local item and stay listed as "removed on another
  device" until the user restores or purges them. Purging removes the local item
  only when no other record still claims it.
* A partial download stays in `staging/<package>.part` and resumes with a
  ranged `GET` from its length; the finished file must hash to the record's
  package before the core is allowed to import a byte.
* The account password is used once, for `/auth/login` → `/auth/token`; only the
  issued token is stored, and only inside the sealed state file.
