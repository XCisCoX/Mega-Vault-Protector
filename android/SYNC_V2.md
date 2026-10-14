# Android encrypted sync v2

## User flow

1. Create/open an ordinary **local** vault. Its password is never used for sync.
2. In the local gallery choose **Add → Encrypted sync**.
3. Enter an HTTPS origin, an optional lowercase certificate SHA-256 pin, a non-sensitive device label and a one-use server administration pairing code. Enter the existing shared 64-lowercase-hex recovery key; generate a key only for a **new** shared library. Save/share that key out-of-band before pairing.
4. Filter/select local files or folder paths and choose **Stage selected encrypted packages**. This is a snapshot upload, not an automatic watch of every local file.
5. Refresh the opaque catalog and select object IDs to download. Names are available only in the ordinary local gallery after native MVPK import. Download selections persist. No download-all default.
6. **Retry pending transfers now** resets backoff. A 401 stops automatic network retries until **Re-pair**. Re-pair preserves recovery key, queued bytes and mappings.
7. If native import deduplicates, or an import was interrupted, the native bridge cannot return existing file IDs. Retry the cached import successfully first if needed, then explicitly link the verified existing local file. Uploads pause while that receipt is unresolved. Never choose an unrelated file.

Legacy cloud-password login is no longer reachable from LoginScreen. Existing PC QR/password share and PC browsing are separate and retained; their TLS pinning no longer modifies process-wide HTTPS defaults.

## Storage / safety

- Calls the existing `CoreBridge.nativeExportPackage(path, key, ids, paths, dirs, false)` for one file, and `nativeImportPackage(path, key, 0L)` into ordinary local folders. The latter returns **count**, not IDs. A unique new ID is linked only when native reports exactly one addition. A zero-count dedup never links an unrelated concurrent addition.
- Recovery key, device token, manifest, mappings, metadata fingerprints and pending queue records are AES-GCM sealed with a per-vault Android Keystore AES key in private `sync_v2` preferences. Pending MVPK and ranged-download partials are already encrypted and live under `noBackupFilesDir/sync-v2/`.
- The namespace combines canonical local root and core's stable 24-byte `vault.ready` identity marker. Recreating a vault at the same path does not reuse old local-ID mappings. Moving the vault to another root requires separate setup; old queue/state is preserved, not automatically migrated.
- `allowBackup=false` remains set; Android 12+ cloud/device-transfer extraction explicitly excludes `sync_v2.xml`. No-backup files and non-exportable Keystore keys are not a portable secret backup.
- TLS uses platform CA validation and normal hostname verification. The optional pin is **additional**, not a trust-all/self-signed override. Install an appropriate trusted CA or use a publicly trusted certificate. HTTP is permitted only for loopback development.
- Pending package bytes, object ID, base revision and operation ID stay stable on retry/restart. Upload queries HEAD and uses <=1 MiB PUTs. Download uses <=1 MiB ranges and durable writes, verifies full size/SHA-256 before native import, and does not advance the imported revision until successful import/mapping.
- CAS conflicts durably preserve the original request in the queue entry and reuse its encrypted bytes under a fresh object/operation ID. Never overwrite using the server's current revision. Imported updates preserve old local copies; unchanged retained versions are suppressed from echo upload.
- Remote tombstones are notices/explicit acknowledgements only. No local media is removed, and missing local IDs are never automatically translated to server deletes. Monotonic observed/acknowledged revisions reject rollback, even for unselected objects.
- Backoff is persisted, exponentially bounded to 1–60 seconds plus up to 999 ms jitter. Work is serialized and coroutine/lifecycle-bound to the unlocked sync screen. Leaving/backgrounding cancels retries/staging between files and transfers between chunks. A blocking HTTP request can take its configured timeout, and a native export/import already in progress is not cancellable through the existing JNI bridge; it may finish before cancellation is observed. No foreground sync service is added.
- Network and the **sync cache** contain encrypted MVPK only. Native import may transiently decrypt **locally** according to the core's temporary-directory policy. This is not a claim of zero plaintext ever.

## Deliberate MVP limits

- No tag sync (the existing MVPK format carries names/folders, not tags), binary deltas, text merge, automatic delete propagation, recovery-key rotation/re-encryption workflow, server switching/state reset, queue cancellation, or device revocation/history UI. The server CLI remains responsible for administration. Mistyped initial recovery keys can be corrected only before staged/acknowledged work exists.
- Manual snapshot upload selection; no automatic new-file watcher. Retries/resumption run while the sync screen is visible and the local vault is unlocked, not as a permanent background service. Reopen the screen and Retry after restart.
- Legacy `packageSegment` portability normalization is used for exported names/path segments. Existing immutable local IDs, size/import time, full relative path and sorted tags form the local change signature; the encrypted package hash is separately persisted with it. Core media bytes are immutable for an ID under the currently exposed Android bridge.
- Lists show at most 200 matching entries at once with explicit counts and path/ID filters; the full <=10000-object manifest is retained, not silently truncated.
- Dedup mapping needs explicit user verification because JNI has no imported-ID result. Import failures and interrupted receipts require a successful cached-import retry before confirmation. Both originals and encrypted cache are retained during uncertainty.

## Verification

Standalone Kotlin tests require JDK 17+, the Kotlin 2.1.20 distribution (including coroutines), and `org.json:json:20240303` as `json.jar`. `--tools` points to a scratch directory containing `jdk*/`, `kotlinc/`, and that jar:

```text
python android/sync-tests/run.py --tools <scratch-tools> --real-server
python android/sync-tests/test_integration.py
```

The five Kotlin programs exercise real loopback HTTP/TLS, durable staging/restart, cancellation, offset/range resumption and lost responses, CAS/idempotent replay, mapping/retained-copy echo suppression, dedup races, interrupted/failed import guards, conservative tombstones, 401 stopping, digest validation, strict JSON and rollback. `--real-server` additionally starts the repository's actual relay on a disposable loopback port and exercises two-device pairing, >2 MiB upload/download, CAS keep-both and history/devices endpoints. Test payloads and native ports are **opaque fixtures, not real MVPK/JNI crypto**. Four Python tests are static integration guards, not instrumentation.

All app Kotlin/Compose and Android resources were compile-validated with `:app:compileDebugKotlin` using portable JDK/Gradle and SDK 36/build-tools 35 in scratch. A scratch-only Gradle init script sets `androidComponents.finalizeDsl { externalNativeBuild.cmake.path = null }` solely for Kotlin/resource compilation, because this machine has no licensed/installed NDK 30.0.14904198/native dependency prefix. The unchanged normal Gradle build is blocked on that NDK. No APK/native link, emulator/device interaction, Android Keystore execution, PC-share interaction, or genuine JNI/MVPK end-to-end test was performed here. Compiled UI wiring is **not** claimed to be device-tested.
