# Secret lifecycle audit

What this document is: the evidence behind the changes. For every sensitive
value it records where the value originates, the copies that exist, who owns
them, how long they live, how they are destroyed, and what an attacker with
process-memory access gets in each vault state.

Severity labels: **C**ritical, **H**igh, **M**edium, **L**ow.

---

## 1. Vault password (desktop, Qt)

**Path:** `QLineEdit` → `takeSecretFromLineEdit` → `core::Secret` →
`Argon2id` → master key.

| Stage | Representation | Owner | Lifetime |
| --- | --- | --- | --- |
| Typed text | `QString` inside `QLineEdit` | Qt widget | until the field is cleared |
| Field read | one `QString` copy | `takeSecretFromLineEdit` | microseconds; wiped with `wipeString` |
| Encoded | UTF-8 in a `SensitiveBuffer` (`Secret`) | `std::shared_ptr<core::Secret>` handed to the worker | until key derivation finishes |
| KDF input | `string_view` over that buffer | `derive_master_key` | one call |

**Before:** `std::make_shared<std::string>(passwordText.toUtf8().constData())`
in `MainWindow::beginCreate`, `MainWindow::beginOpen`,
`MainWindow::beginChangePassword` and `SettingsDialog::beginPasswordChange`.
That is three uncontrolled copies (the `QString`, the temporary `QByteArray`
from `toUtf8()`, and the `std::string`), all kept alive for the whole Argon2id
run and wiped — best effort — only at the end of the worker lambda. The password
field itself stayed filled for that whole time.

**After:** the field is read once, the widget is cleared immediately
(`QLineEdit::clear()` before the KDF starts), the UTF-16 text is encoded
directly into libsodium secure memory, and the buffer is wiped by the `Secret`
destructor on every exit path — success, failure, exception, or a cancelled
worker. Finding **H-1** (password retained in plain heap for the duration of the
unlock) is closed; the measurable statement is in
`MEMORY_HARDENING_RESULTS.md` (§ "Password retention").

**Residual (framework):** `QLineEdit` keeps its own `QString` copy which Qt frees
when the text is replaced; `QTextDocument`-backed widgets are worse. The
application can clear the widget and wipe its own copy, but it cannot zero a
buffer Qt has already freed. See `REMAINING_RISKS.md` R-2.

## 2. Vault password (Android)

**Path:** Compose state → JNI → `secret_from_jstring` → `Secret` → Argon2id.

`nativeCreateVault`, `nativeOpenVault`, `nativeChangePassword`,
`nativeExportPackage` and `nativeImportPackage` previously did
`const std::string p = jstring_to_string(env, password);` — a
`std::string` with the password, alive for the whole JNI call.

Now `secret_from_jstring` takes `GetStringChars` (UTF-16) and encodes straight
into `Secret`; no `std::string` exists anywhere in the path. The Kotlin caller
clears the field and drops its state reference before starting the work
(`LoginScreen`, `SetupScreen`, `SettingsDialog`).

**Residual (framework):** a Java `String` cannot be overwritten. ART may have
copied it (interning, JIT, GC compaction) and those copies are outside the
application's control. This is documented, not fixed; claiming otherwise would
be false.

## 3. Master key

| Property | Value |
| --- | --- |
| Origin | `internal::derive_master_key` (Argon2id, salt + parameters from `vault.meta`) |
| Storage | `Vault::Impl::master_key_` — `internal::SensitiveBuffer` (sodium_malloc, mlock attempted) |
| Copies | exactly one; `SensitiveBuffer` is move-only and copying is deleted |
| Lifetime | the unlocked session |
| Destruction | `Vault::lock()` → `impl_.reset()` → `SensitiveBuffer::release()`: `sodium_memzero`, `sodium_munlock`, `sodium_free` |
| Post-lock | absent — measured 0 occurrences in the process after `lock()` |

Transient copies of the *derivation output* exist inside libsodium/Argon2 during
the call. Argon2id clears its working memory block on success
(`ARGON2_CLEAR_MEMORY` default). Finding **C-1** ("the master key is the single
most valuable object in the process") is addressed by ownership, not by hoping.

## 4. Subkeys and file keys

| Value | Storage | Lifetime |
| --- | --- | --- |
| database key / verifier key / wrapping key / thumbnail key | `SensitiveBuffer` from `derive_subkey`, per operation | one operation |
| per-file key (unwrapped from `video_keys`) | `PackageReader::file_key_` (`SensitiveBuffer`) | one reader |

The unwrapped key used to be copied out of a `std::vector` and the vector was
"wiped" with `std::fill` (`open_package_reader`). `std::fill` on a buffer that
is freed immediately afterwards is removable by the optimizer. It is now
`internal::wipe_bytes` → `sodium_memzero` (**M-1**, fixed).

## 5. Decrypted content paths

| Path | Plaintext copies | Lifetime | Wiped |
| --- | --- | --- | --- |
| `PackageReader::chunk` | LRU cache, 4 × chunk | until evicted / reader destroyed | **yes** — `release_slot` → `sodium_memzero` before the vector is released |
| `PackageReader::read` | one copy per chunk returned **by value** | one call | it was never wiped; the copy is gone — `chunk()` now returns a pointer into the cache (**M-2**, fixed) |
| `read_package_content` | one vector for the whole file + one per chunk | one call | per-chunk copy now `sodium_memzero`ed after the insert |
| `Vault::read_video_bytes` / `read_video_range` | returned `std::vector` | caller's | caller's responsibility (documented) |
| Android `g_plain` | up to 48 MiB of decrypted media | while unlocked | `wipe_plain` now uses `sodium_memzero` (**H-2**, fixed) |

**H-2 detail:** the old `wipe_plain` used `std::fill` and then
`vector::clear()` + `shrink_to_fit()`. The dead-store elimination risk is the
smaller half of the bug: `g_plain.erase()` in `forget_plain` released the buffer
*before* it was wiped, so the zeroing wrote into memory that had already been
returned to the allocator. Both order and primitive are now correct.

## 6. Decrypted metadata

`VideoInfo` / `FolderInfo` / `MediaInfo` are ordinary `std::string` values: the
gallery is showing them, so they are expected in memory while unlocked. What
matters is the lifetime:

- `MainWindow::lockVault()` clears the gallery cache (thumbs, media, kinds,
  rows, tags) and closes the player, music, record and text windows before the
  vault is locked.
- After `Vault::lock()`, `list_videos()` / `media_info()` / `thumbnail()` all
  return a locked error: there is no path that regenerates metadata without the
  password.
- Measured: the decrypted *content* marker is 0 occurrences after the reader is
  closed, and 0 after lock.

Finding **M-3**: the text-record editor kept the decrypted note in a
`QTextDocument` behind the lock screen; `closeText()` is now called first in
`lockVault()` and `record_window` is destroyed, which was already the intent —
the audit confirmed the ordering is correct.

## 7. Sync secrets

| Value | Before | After |
| --- | --- | --- |
| `.mvpk` package password (`Keys::package`, a 64-char hex `QString`) | `fill('\0')` then `clear()` — `fill` detaches first, leaving the old buffer intact on the heap | `wipeString` (detach, `sodium_memzero`, clear), plus `Keys::~Keys()` so every copy dies with its scope (**H-3**, fixed) |
| state key / legacy state key (`QByteArray`) | `sodium_memzero` — correct | `wipeByteArray` (same, one helper) |
| vault password inside `deriveKeys` | `vaultPassword.toUtf8()` — a second `QByteArray` copy of the vault password | byte-level `deriveKeys(..., std::string_view, ...)`; no copy |
| password change reseal | two `QString::fromStdString(...)` round trips through `std::string` | `resealForPasswordChange(dir, const Secret&, const Secret&)` (**H-4**, fixed) |
| device token | `SyncState::token` in memory; `clearToken()` on sign-out | unchanged; it is a bearer credential, not a key, and is already sealed at rest |

Compatibility: none of this changes the on-disk state format or the package
key, so existing sync state and already-uploaded `.mvpk` packages keep working.
Password change still re-seals `state.enc` and keeps the package key, exactly as
before.

## 8. Sharing secrets

`ShareServer::State` holds `tokenHex`, `linkTokenHex`, `linkKeyHex` and a
`std::shared_ptr<core::Vault>`. `ShareServer::stop()` wipes the three hex
secrets and calls `vault->lock()`. Before this work, `MainWindow::lockVault()`
returned early when a peer was linked, so the share server kept a live, unlocked
vault *and* the lock screen claimed the library was locked. That is finding
**C-2**, and it is the reason Hard Lock exists — see `HARD_LOCK_DESIGN.md`.

## 9. Android lifecycle

| Concern | State |
| --- | --- |
| Screenshot / Recents preview of decrypted content | `FLAG_SECURE` set in `MainActivity.onCreate` (was absent) — **M-4**, fixed |
| `g_plain` cleared on lock | yes, `nativeLock` → `clear_plain` (now really zeroed) |
| `onDestroy()` used for cleanup | no; lock is explicit (`lockNow`) and the idle path uses `awayTooLong()` |
| Playback service surviving lock | `lockNow` stops `MusicPlaybackService` when `MusicHub.active` |
| Activity recreation | `AppSession` keeps only the *identity* of the vault, never keys; a recreated activity re-attaches to the native session, which is the same locked/unlocked state |
| Share/peer session across lock | now requires an explicit choice; the screen says the vault is still unlocked |

## 10. Temporary files, caches, logs

| Item | Finding |
| --- | --- |
| Desktop text records | written through `import_bytes`; no plaintext temp file (already correct) |
| Android URI imports | `nativeImportFile`/`nativeImportBytes` stream into the package; no staging copy |
| Thumbnails | stored encrypted in the vault database; plaintext only while displayed |
| PDF page rendering | rendered in memory into a JPEG, then stored encrypted |
| Exports / package writes | written to the user-chosen destination by definition; the plaintext never touches a temp file |
| Restore | writes plaintext to the destination the user picked — by design, and the UI says so |
| Logs | no password, key or token is logged; SQLCipher's own error lines contain no key material |

**L-1:** deletion of an exported/restored plaintext file is not secure erasure on
an SSD or a copy-on-write filesystem. Documented, not "fixed" — nothing in the
application can fix it.

## 11. Findings summary

| ID | Severity | Finding | Status |
| --- | --- | --- | --- |
| C-1 | Critical | Master key had no explicit wipe guarantee in paths that dropped the vault | Fixed: single owner, wiped on lock, measured |
| C-2 | Critical | Lock screen left the vault unlocked and shareable | Fixed: Hard Lock default + explicit privacy screen |
| H-1 | High | Password in `std::string`/`QString` for the whole unlock | Fixed |
| H-2 | High | Android `g_plain` wipe was removable and ran after `erase` | Fixed |
| H-3 | High | Sync package key wiped with `QString::fill` (detaches, leaves the copy) | Fixed |
| H-4 | High | Password change materialised both passwords as `QString`/`std::string` | Fixed |
| M-1 | Medium | Unwrapped file key "wiped" with `std::fill` | Fixed |
| M-2 | Medium | `PackageReader::chunk` returned a per-read plaintext copy that was never wiped | Fixed (no copy) |
| M-3 | Medium | Text editor / record window lifetime vs lock | Verified correct, ordering preserved |
| M-4 | Medium | No `FLAG_SECURE` on Android | Fixed |
| M-5 | Medium | Readers created outside the vault mutex (media probe) outlived a lock with a live file key | Fixed: `SessionGuard` revocation |
| M-6 | Medium | UTF-16 → UTF-8 password encoding truncated the tail when fewer than 4 bytes of slack remained (introduced and fixed during this work; caught by `settings_page`) | Fixed + pinned by `tests/security_memory.cpp` §0 |
| L-1 | Low | Plaintext exports/restores are not securely erased | Documented |
| L-2 | Low | Sync server stores names/paths/tags as plaintext metadata | Documented, not changed (protocol work) |
