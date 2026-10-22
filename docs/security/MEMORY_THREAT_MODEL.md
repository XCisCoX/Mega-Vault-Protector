# Memory threat model — Mega-Vault-Protector

Scope: plaintext passwords, encryption keys, decrypted metadata and decrypted
file content inside the running process, on Windows and Linux desktop (C++20 +
Qt) and on Android (Kotlin/Compose + JNI + the same C++ core).

This document states what the hardening is expected to defend against, and what
it explicitly does not.

## 1. Assets

| Asset | Where it lives | Lifetime that matters |
| --- | --- | --- |
| Vault password (text the user typed) | UI field → capture buffer → Argon2id input | milliseconds; must not outlive the unlock |
| Master key (32 B, Argon2id output) | `Vault::Impl::master_key_` | the whole unlocked session |
| Subkeys (database, verifier, package-wrapping, thumbnail) | derived per operation into `SensitiveBuffer` | one operation |
| Per-file keys (32 B, unwrapped from the database) | `PackageReader::file_key_` | one reader |
| SQLCipher key + SQLCipher page cache | inside the SQLite/SQLCipher library | the open connection |
| Decrypted metadata (names, tags, folder names) | `VideoInfo` / `FolderInfo` copies in the UI | per view refresh |
| Decrypted content (media chunks, thumbnails, text records) | `PackageReader` chunk cache, `std::vector` results, Android `g_plain` | per read / per playback |
| Sync secrets (package key, device token, state keys) | `sync::Keys`, `SyncState` | the unlocked session |
| Share session secrets (link token, link key) | `ShareServer::State` | the share session |

## 2. Adversaries in scope

**A1. Same-user process-memory reader after the fact.** The user (or someone
with the user's account) dumps the process *after* locking the vault, or opens a
crash dump / hibernation file written after locking. This is the adversary the
mission's acceptance criteria target, and the one the design can measurably
defeat.

**A2. Same-user process-memory reader while unlocked.** Can read everything the
process can read. Nothing here defeats A2; the goal is only to shorten the time
windows and remove copies that are not needed at all.

**A3. Same-user background process while the app is "locked" on screen.** This is
the case the old behaviour got wrong: the lock screen hid the library while
sharing kept the vault unlocked. A3 is now defeated by making the ordinary Lock
action a hard lock (see `HARD_LOCK_DESIGN.md`).

**A4. Other app on Android reading app-private storage / backups.** Addressed by
keeping plaintext out of app-private files where possible and by the existing
`FLAG_SECURE`/backup rules; see `REMAINING_RISKS.md`.

## 3. Out of scope (cannot be defended by this design)

- A privileged attacker (root/SYSTEM, kernel debugger, hypervisor) reading or
  modifying the live process. `mlock` and page protection do not stop this.
- An attacker who can inject code into the process before it locks.
- Cold-boot and DMA attacks on DRAM.
- Compromised toolchain, libsodium, SQLCipher, FFmpeg or OS crypto.
- The user's own password manager, clipboard managers, or screen recorders
  capturing what the user themselves typed or displayed.

Absolute memory confidentiality is not claimed anywhere in this work.

## 4. Trust boundaries

1. **UI toolkit boundary.** Qt `QString` is implicitly shared and `QLineEdit`
   owns an internal copy; Kotlin `String` is immutable and cannot be
   overwritten. The application can only shorten how long *its own* references
   live and wipe the buffers it owns. Historical copies created and freed inside
   Qt/ART are outside the application's control and are documented as residual.
2. **Core boundary.** `Secret` / `SensitiveBuffer` own secret bytes in
   libsodium secure memory. Nothing else in the core holds a password; keys are
   `SensitiveBuffer` from derivation to destruction.
3. **Library boundary.** SQLCipher keeps its own key schedule and page cache;
   `PRAGMA cipher_memory_security = ON` is already set, and closing the
   connection (`Vault::lock()` → `Database::close()`) releases it. FFmpeg's
   internal buffers are released when the reader is destroyed.
4. **Async boundary.** Any object handed to a worker must either be revoked on
   lock (`SessionGuard`) or be derived-only data. `Vault::lock_generation()`
   lets callers reject results produced in a previous session.

## 5. Why the readable strings in a dump are not (necessarily) a leak

The storage identifier `f4/51/f45117…c8a5.vvp` is the SHA-256 of the encrypted
package, and `Exil 2.flac` beside it is a decrypted display name from the vault
database. While the vault is unlocked those names are *expected* to be present:
the gallery is showing them. The security question is not "do they exist" but
"do they still exist, and are they still usable, after the user locked the
vault".

## 6. Resulting invariants

The implementation is built to guarantee these, and
`../tests/security_memory.cpp` asserts them:

- I1. After a completed hard lock, no live copy of the master key exists.
- I2. After a hard lock, every capability issued during the unlock is revoked:
  readers refuse to decrypt, a locked vault refuses metadata and content calls.
- I3. Decrypted content cached by the core is zeroised when released, not merely
  freed.
- I4. A password captured by the UI is not retained by the application after the
  key derivation completes.
- I5. A stale asynchronous result cannot reopen or keep using a locked vault.

Residual exposure that survives I1–I5 (freed heap, pagefile, framework copies) is
enumerated in `REMAINING_RISKS.md`.
