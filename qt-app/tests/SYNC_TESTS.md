# Native sync v2 tests

The production client is `MegaVaultSync`, linked into `VideoVaultApp`. It uses
only the unlocked local vault for gallery/playback. `Sync` in the unlocked
library opens setup; legacy remote vault-password login is disabled and
`MegaVaultCloud` is now opt-in (`MVP_BUILD_LEGACY_CLOUD`, default OFF).

## Small headless entrypoint

No Widgets, Multimedia, FFmpeg, SQLCipher or vault is needed for protocol and
ledger tests. Qt 5.12+ and libsodium are sufficient (Qt 6 is a helper-test-only
fallback; the production application still targets Qt 5).

```sh
cmake -S qt-app/tests/sync-headless -B <scratch>/sync-headless -DCMAKE_PREFIX_PATH=<matched-qt-prefix>
cmake --build <scratch>/sync-headless
ctest --test-dir <scratch>/sync-headless --output-on-failure
```

Missing libsodium explicitly skips the encrypted-state target with a CMake
warning. Do not report that as a full state test run. Match the compiler,
Qt libraries and runtime DLLs; avoid mixing MSVCRT and UCRT distributions.

## Core-bound native tests

Configure the normal repository with `MVP_BUILD_TESTS=ON`, then build:

```sh
cmake --build <build> --target VideoVaultSyncProtocolTests VideoVaultSyncStateTests VideoVaultSyncEngineTests VideoVaultSyncDialogTests
ctest --test-dir <build>/qt-app -R '^sync_' --output-on-failure
```

The dialog test uses `QT_QPA_PLATFORM=offscreen`, a real temporary vault,
masked recovery entry and the real local-key-load button. These targets do
not require building the complete application's Windows WinRT music/PDF
integration. Native engine tests cover folder/tag fingerprints and ledger
lock contention/retry. State tests verify encrypted durable queue roundtrip,
wrong-key rejection and authenticated corruption rejection.

## Actual relay integration (no fabricated responses)

From repository root, with native Qt runtime DLLs on PATH:

```sh
python qt-app/tests/run_sync_live.py <build>/qt-app/VideoVaultSyncEngineTests[.exe]
python qt-app/tests/run_sync_live.py <build>/qt-app/VideoVaultSyncEngineTests[.exe] --tls
```

`--tls` needs `openssl`. The harness creates disposable pairing codes, keys,
certificates, vaults and relay data; starts the actual `sync_server`; terminates
it on completion; then verifies exact SQLite object/history/device totals.
The TLS variant first proves ordinary validation rejects its self-signed
certificate and a wrong certificate pin is rejected, then performs the same
workflow with the exact pin. No real credentials are requested or logged.

The native executable exercises actual MVPK export/authentication/content
hashing; independent local passwords on two devices; encrypted queue reload
with unchanged operation ID; a pre-existing partial upload; a pre-existing
ranged encrypted download; dedup import with zero callbacks resolved by
bounded plaintext content hashing in memory; no echo; bidirectional selected
local files without download-all; conflict copies and retained originals;
remote delete notices and explicit non-destructive acknowledgment; HTTP 401
recovery guidance; immutable history; device listing; and Pause/Retry.

## Deliberate safety boundaries

* Sync queue and download cache contain encrypted MVPK only. The core MVPK
  importer has transient plaintext staging; cleanup is best effort, not secure
  erasure. The client does not claim zero plaintext ever reaches disk.
* MVPK carries names and paths, not tags. Tags and empty folders are not synced.
* Core content dedup can retain an existing local name/folder rather than apply
  remote metadata. Name collisions retain copies. This is labeled in setup;
  the client does not delete an original to force a metadata merge.
* Remote tombstones never automatically remove local files. Local deletion
  does not automatically create a remote tombstone.
* Pending packages and conflict archive bytes are retained, not garbage
  collected. Back up the displayed private directory and original recovery key.
* Re-pairing authenticates a device; it does not rotate the recovery key.
