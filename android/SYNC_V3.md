# Android encrypted sync (protocol 3)

The Android client is a protocol 3 hash-bucket client, like the desktop app
(`qt-app/src/sync_engine.cpp`) and the relay in `sync_server/`. One library key
unlocks one bucket; the bucket is a flat set of opaque `.mvpk` packages, each
named by the SHA-256 of its own bytes. The server stores, lists, serves and
deletes packages and never learns a file name, a folder path or a plaintext
byte.

This replaces the protocol 2 client entirely. Gone: pairing codes, the device
token and its Keystore pairing secret, revocation, revisions/CAS, conflict
copies, tombstones and the background retry loop.

## User flow

1. Create or open an ordinary **local** vault. Its password is never used for
   sync.
2. Choose **Add → Encrypted sync** in the local gallery.
3. Enter the server URL and the 64-lowercase-hex library key, then press
   **Sync**. That is the whole setup.
4. Check the local items to share (filter by path, first 200 shown). Press
   **Sync** to export the checked items that changed, upload them and download
   every package this device has never imported.
5. The package list shows opaque names, their size, and whether this device
   already has them. Imported items appear in the ordinary local gallery.
6. **Delete the selected package from the server…** asks for confirmation and
   deletes exactly that one package for every device. Local media is never
   removed, and nothing is deleted automatically.

A wrong key shows **“The server rejected this library key.”** once. The screen
does not retry: `SyncEngine` performs no retries and `SyncScreen` has no polling
loop, so another request only happens if the user presses Sync again.

## What one pass does

```
GET /healthz                      require {"protocol":3}
GET /v3/files                     authenticate; this is the key check
adopt ledger scope                reset mappings only after the key is accepted
for each checked item that changed:
    native export -> one .mvpk under the library key
    name = sha256(bytes) + ".mvpk"
    HEAD /v3/files/<name>         X-Complete=true -> already present, send nothing
                                  else resume from X-Offset
    PUT  /v3/files/<name>?offset=N  <=1 MiB chunks, X-Total-Size = full size
    ledger[local id] = name       (+ source fingerprint and path)
GET /v3/files                     list again, including what was just uploaded
for each listed name not in the local ledger:
    HEAD /v3/files/<name>         an unfinished upload is skipped, never imported
    GET  with Range               resume from the staged partial, <=1 MiB per request
    verify size and sha256 == name before anything else
    native import
    ledger known names += name
```

Uploads mirror the desktop's naming rule: the package is named by the hash of
its own bytes, and a `200` answer for the first chunk (after the HEAD probe)
means the server already owned a complete package under that name, so the
transfer stops and is counted as “already on the server”. `201` means this
request created the row. A `409` reports the server's durable offset and the
upload continues from there (bounded to four corrections, then it fails).

Downloads are verified completely before the JNI import: size and SHA-256 must
match the name. A mismatch deletes the partial and is reported; the original
local files are untouched. A package whose `HEAD` says `X-Complete: false` is
left alone and counted as still uploading by another device.

The ledger is the only local knowledge about the bucket:

- `files`: local item id → package name, a fingerprint of the item's local
  metadata, and its path. Equal fingerprint + equal name ⇒ **unchanged**, so the
  item is neither re-exported nor re-uploaded (a re-export would be a new
  package, because the package format randomises its encryption).
- `seen`: package names this device has already imported. The listing carries no
  other signal, so this is what prevents re-importing a known package.
- `selected`: the checked local ids.
- `server` and `key`: the saved settings. The ledger scope is reset only after
  the server has accepted a *different* key or server URL, because those
  mappings then belong to another bucket.

Deleting a package removes it from the bucket and forgets its mapping on the
device that asked. There is no delete propagation in either direction: other
devices are not told, and a device that does not have the mapping will upload
the item again on its next pass. The protocol has no tombstones by design.

## Storage and safety

- `AndroidSyncStore` keeps the ledger in the `sync_v3` private SharedPreferences,
  AES-GCM sealed with a non-exportable per-vault Android Keystore key (GCM tag,
  namespace as AAD). `allowBackup=false`, and Android 12+ cloud backup and
  device transfer both exclude `sync_v3.xml` (and the obsolete `sync_v2.xml`).
- Exported packages (`out-*.mvpk`), resumed downloads (`dl-<name>`) and the
  ledger directory live under `noBackupFilesDir/sync-v3/<namespace>/`, where the
  namespace combines the canonical vault root and core's stable 24-byte
  `vault.ready` identity marker. Packages and partials are ciphertext already.
- The library key is never logged and never written in plaintext: it is only in
  memory and inside the AES-GCM-sealed ledger, and the server only ever holds
  `sha256(key)` (it receives the key itself as `Authorization: Bearer …`, so the
  transport must be TLS). On first use after the upgrade, the protocol 2
  preferences, `sync-v2` directory and old Keystore alias for this vault are
  removed.
- TLS uses the platform CA store and normal hostname verification, captured
  once per client so a permissive process-wide default (the PC-share path)
  cannot weaken sync. Plain HTTP is accepted only for `127.0.0.1`, `localhost`
  or `[::1]`. There is no certificate pin and no trust-all option.
- The native export is `CoreBridge.nativeExportPackage(path, key, ids, paths,
  dirs, false)`; the import is `CoreBridge.nativeImportPackage(path, key, 0L)`.
  The import returns a count, not local IDs, so imported items are not added to
  the id→package ledger. Re-sharing an item that arrived by sync therefore
  exports it again (a new package); the desktop has the same limitation when no
  local mapping is available.
- Sync is manual and screen-bound. There is no foreground service, no watcher,
  no background retry, and leaving the screen cancels the UI job between
  requests; a blocking HTTP call or an in-progress native export/import cannot be
  interrupted through the existing JNI bridge.

## Deliberate limits

- No tag sync (the MVPK format carries names and folders, not tags).
- No binary deltas, text merge, conflict copies or multi-version history.
- No delete propagation, tombstones, or per-device revocation — rotating the
  library key on the server is the only revocation, and it is not retroactive
  (see `docs/sync-protocol.md`).
- No automatic upload of new local files: the checked set is a snapshot.
- Lists show the first 200 matching local files and the first 200 packages;
  counts are shown and the filter narrows them.
- Server switching keeps the old KeyStore-sealed ledger file until the first
  successful pass under the new settings, which then resets the mappings.

## Verification

Commands run in this repository on Windows (JDK 17, Kotlin 2.1.20 compiler,
`org.json:json:20240303`), with the SDK/Gradle scratch setup described below.
These are real invocations and their real output.

### Standalone Kotlin programs, real sockets (`--real-server`)

```
$ python android/sync-tests/run.py --tools C:/Users/parvizi/AppData/Local/hermes/cache/scratch/android-sync-tools --real-server
SyncEngine: export/PUT naming, unchanged skip, restart, import-once, wrong key (no retry), protocol and explicit delete passed
SyncHttp: bearer key, HEAD/offset headers, PUT, Range, 401 and validation passed
SyncPolicy: package names, endpoints, namespace and folder paths passed
SyncTls: self-signed relay refused, platform CA trust isolated from global defaults
SyncTransfer: upload resume (HEAD and 409), 200-at-offset-0, lost answer, download resume, digest rejection, incomplete and invalid listing passed
Actual protocol 3 relay interop: Sync complete: 1 uploaded, 0 already on the server, 0 unchanged, 0 imported. / 2.0 MiB chunked upload / range download / import once / wrong-key 401 once / explicit delete and re-share passed (opaque fixtures; JNI not exercised)
5 standalone Kotlin test programs passed
```

What those programs cover: package naming and endpoint rules; bearer auth; HEAD
`Content-Length`/`X-Offset`/`X-Complete`; 1 MiB PUT chunks with `X-Total-Size`;
Range `206` + `Content-Range`; upload resume from both `X-Offset` and a `409`;
`200`-at-offset-0 as already present; a lost PUT answer followed by cleanup and a
fresh attempt; download resume from a staged partial; full-hash rejection before
import; incomplete packages skipped; invalid listings rejected; unchanged
items never re-exported; a known package never re-imported; a restart reusing
the ledger; one wrong-key rejection with no automatic retry and an untouched
ledger; protocol ≠ 3 refused; explicit delete only. `--real-server` starts the
repository's actual `sync_server` (protocol 3, real SQLite/blob data directory)
on a disposable loopback port and repeats upload, 2.0 MiB chunked transfer,
range download, import-once, wrong-key 401 and delete against it.

The relay tree changed after that run. At validation time `sync_server` was the
protocol 3 relay (`GET /v3/files`, `serve --key-sha256`, the `key` helper). It
has since been replaced in the working tree by a protocol 4 design
(`PROTOCOL = 4`, `/v1/vaults`, `account`/`vault`/`token` commands), which is not
the contract this client implements. A fresh `--real-server` run therefore stops
at provisioning the relay, before any Kotlin client code runs:

```
$ python android/sync-tests/run.py --tools <scratch-tools> --real-server
5 standalone Kotlin test programs passed
--real-server needs the protocol 3 sync_server: the one with the `key` command and /v3/files.
The sync_server in this working tree answered differently:
usage: sync_server [-h] --data DIR {serve,account,vault,token} ...
sync_server: error: argument command: invalid choice: 'key' (choose from 'serve', 'account', 'vault', 'token')
```

Without `--real-server` the five JVM programs still compile and pass. The
interop output above was produced against the protocol 3 relay before that
rewrite; it is reported as run, not re-run against protocol 4.

### Static integration guards

```
$ python android/sync-tests/test_integration.py
.......
----------------------------------------------------------------------
Ran 7 tests in 0.010s

OK
```

They assert: `allowBackup=false` and both sync preference files excluded from
cloud backup and device transfer; the client is a `/v3/files` hash bucket with
HEAD/PUT/GET/DELETE, `X-Total-Size`, `X-Offset`, `X-Complete` and Range; no
protocol 2 identity/revision/CAS/pairing code remains; the screen has exactly
one Sync action and no delay/polling loop; the ledger keys (`seen`, `remote`,
`source`) and the real JNI export/import are used; the legacy cloud login stays
unreachable and the PC-share path cannot change process-wide TLS defaults.

### Kotlin compile

```
$ cd android && gradle --offline --init-script <scratch>/kotlin-only.init.gradle :app:compileDebugKotlin
> Task :app:compileDebugKotlin

BUILD SUCCESSFUL in 42s
16 actionable tasks: 1 executed, 15 up-to-date
```

The scratch-only init script sets
`androidComponents.finalizeDsl { externalNativeBuild.cmake.path = null }` so
Kotlin and resources compile without the NDK. This machine has no NDK
30.0.14904198, so the normal Gradle build still stops at the native configure
step; the compile above is Kotlin/Compose only, not a native build or APK.

### Not verified here

- No emulator or physical device was available: no runtime UI check, no
  Android Keystore execution, and no confirmation that `HttpURLConnection`
  header parsing behaves identically on Android.
- No APK, no native link, no JNI end-to-end: every test package is an opaque
  byte fixture, not a real MVPK, so export/import through `libmvpcore.so` and
  the actual library key are **not** exercised.
- No interaction with the deployed Caddy/TLS relay, and no on-device backup /
  device-transfer extraction test.
- The `android/` client compiles, its protocol logic is exercised on a JVM
  against real sockets and against the real relay, but the phone app as a whole
  is not claimed to be device-tested.
