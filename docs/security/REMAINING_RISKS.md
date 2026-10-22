# Remaining risks

What this hardening does not fix, stated plainly. Nothing in this document is
hypothetical hand-waving: each item names where it lives and what an attacker
gains from it.

---

## What an Attacker Can Still Recover

### 1. While the vault is unlocked (the easy case)

Everything. A process-memory dump gives the master key, the live SQLCipher key,
the decrypted gallery (names, folders, tags, durations, thumbnails), the
currently playing content and, in a shared session, the phone's link key. This is
**expected and unavoidable**: an unlocked vault is a vault whose contents are
being displayed.

The hardening still helps here: the password is not in the process (it never was,
after derivation), fewer plaintext copies exist, and media chunks are not copied
per read. But an attacker who dumps at this moment does not need the password.

### 2. After a privacy screen (the deliberately half-open case)

Identical to case 1, with two differences: the user knowingly chose it, and the
screen says "the vault is NOT locked" with the name of the device that still has
access. Nothing is wiped, sharing keeps working, and a dump taken in this state
is as good as a dump taken while the gallery was open.

This is not a regression — it is the *documented* shape of the feature. Before
this work it was the silent behaviour of the ordinary Lock button; now it is an
explicit choice, it is never taken by the automatic idle lock, and it ends by
itself when the peer disconnects.

### 3. After a hard lock (the case that was fixed)

Verified by `tests/security_memory.cpp` on Windows:

- no live copy of the 32-byte master key exists (0 occurrences),
- no live copy of the password exists outside the caller's own `Secret`
  (0 after that is wiped too),
- no decrypted media chunk from the reader cache exists (0),
- a locked vault refuses metadata, content and reader operations,
- a reader that was already open refuses to decrypt (session revoked).

What still exists after a hard lock:

| Residual | Why it is there | Can an attacker use it? |
| --- | --- | --- |
| Freed heap bytes from *before* the lock | allocator reuse, `std::string`/`std::vector` growth, Qt/ART internals, SQLCipher page cache, FFmpeg buffers | Only ciphertext-adjacent fragments or already-shown metadata; **no key material** (the key buffers are zeroed before release). A careful dump might still recover a filename that was displayed earlier. |
| Pagefile / swap | `mlock` is attempted; when the OS refuses it (`memory_locked()` is false) the page may have been written to disk | On this host `mlock` succeeded; on a host that refuses, key bytes may reach the pagefile. The UI can report this through `Vault::secure_memory_available()`. |
| Crash dumps / hibernation | OS-level, written while unlocked | A WER minidump or `hiberfil.sys` captured while unlocked contains the same material as case 1. Not mitigable from inside the process. |
| The vault database itself | SQLCipher, unchanged | Requires the password; not a memory issue. |
| Sync server metadata | server-side SQLite, plaintext names/paths/tags | See below (L-2). |
| Plaintext exports / restores | the user asked for a plaintext copy | By definition readable; deletion is not secure erasure on SSD/COW filesystems. |

---

## Residual findings

### R-1 (Medium) — Qt and Kotlin make copies the application cannot wipe

`QString` is implicitly shared: `QLineEdit::text()` can return a reference to the
widget's own buffer, `toUtf8()` allocates a temporary, and Qt's internal
`QTextDocument`/clipboard paths may copy again. Kotlin `String` is immutable;
`textField` state, ART's string table and JIT constants can all hold a password
outside the application's reach.

Mitigated: the application reads the field once, clears the widget before the
KDF, encodes into locked memory, and wipes every buffer it owns. Not fixable:
historical copies that Qt/ART created and freed. A future improvement is a
custom input widget that accumulates keystrokes directly into a
`SensitiveBuffer`, which removes the `QString` stage entirely for Qt; it was out
of scope here because it changes the input experience (IME support, undo,
accessibility) and would need its own UI testing.

### R-2 (Medium) — `mlock` can be refused

`sodium_mlock` fails when `RLIMIT_MEMLOCK` is exhausted or the kernel is
hardened. The bytes are still zeroed on release, but they may have been swapped.
The failure is now observable (`SensitiveBuffer::memory_locked()`,
`Secret::memory_locked()`, `Vault::secure_memory_available()`) instead of being
silently ignored. The desktop UI does not yet surface it; that is a small,
worthwhile follow-up (a line in Settings → Security).

### R-3 (Low) — page protection is not used

`sodium_mprotect_noaccess()` would make key pages unreadable between operations,
but the master key is read by every operation and every worker thread, so a
transition window would have to be synchronised across all of them. Getting that
wrong turns a security feature into a rare crash or a deadlock. It was
deliberately not added; the ownership/wipe model gives most of the benefit at a
fraction of the risk. Revisit only with a design that serialises access through
one guarded call site.

### R-4 (Low) — the SQLCipher key schedule lives inside SQLCipher

`sqlite3_key` copies the database key into the library's own memory. Closing the
connection (`Vault::lock()` → `Database::close()`) releases it, and
`PRAGMA cipher_memory_security = ON` is set, but the library's internal wipe is
outside this codebase's control. A dump taken *while unlocked* can contain it.

### R-5 (Low) — Android plaintext cache policy is a trade-off

`g_plain` keeps up to 48 MiB of decrypted short-media in RAM to make short-clip
looping cheap. It is zeroed on lock and on eviction, but while unlocked it is the
largest single plaintext object in the process. Reducing the cap or removing the
cache would cost playback smoothness for small files; the current policy is
documented rather than changed. If a stricter profile is wanted, the constant is
`kPlainTotalCap` in `jni_bridge.cpp`.

### R-6 (Low) — desktop gallery cache clears but does not zero

`galleryCache_` holds decrypted thumbnails (`QPixmap`) and metadata strings. On
lock it is cleared, so the entries are released — but `QPixmap` data lives in
driver/shared memory that the application cannot zeroise, and the strings are
freed unzeroed. Clearing is what prevents them being *redrawn* on the next
vault; it is not erasure.

### L-2 (Low, by design) — sync server stores plaintext metadata

The account-based sync server keeps filenames, paths and tags as plaintext in its
own SQLite database, and `.mvpk` content is encrypted client-side. This is a
metadata-confidentiality gap on the server, **not** evidence that local vault
encryption failed. Fixing it needs an encrypted-metadata protocol version with
its own compatibility story for existing records; that was explicitly out of
scope for a memory-hardening change and is not started. Nothing in this work
makes it worse.

### L-3 (Low) — package passwords for `.mvpk` exports

An exported package is protected by the password the user typed for it. That
password is now handled through `Secret` and wiped, but the `.mvpk` itself is a
portable encrypted artefact: whoever holds it can attack the password offline.
Argon2id parameters for packages are chosen by the creator (`kPackageKdf`), not
by this hardening.

---

## Test coverage gaps

| Gap | Why | How to close it |
| --- | --- | --- |
| Linux never executed here | no Linux build host in this environment | run `ctest -R VideoVaultCore.SecurityMemory` on Linux; the `/proc/self/mem` branch is compiled but unproven |
| Android never executed here | no NDK device loop on this host | cross-build, install, verify `FLAG_SECURE`, lock dialog, `g_plain` zeroisation via `dumpsys meminfo` |
| Sync/sharing stages G | the desktop sync engine needs a live relay to exercise | run the existing `sync_engine` live test with a relay, plus a memory scan before/after lock |
| Stage I (cancel/exception) | partially covered (wrong password, refused package password) | add a cancelled-import case once the import cancellation hook exists |
| Sanitizers | MSVC Debug CRT catches heap misuse, but no ASan run | build with `-fsanitize=address,undefined` on Linux and re-run the suite |

## What is deliberately not claimed

- Not "the password never appears in a memory dump". The test asserts that the
  *vault* does not retain it; Qt, the C++ runtime and the OS may.
- Not "keys are unrecoverable by a privileged attacker". They are unrecoverable
  *after a hard lock* by an attacker who dumps the process afterwards.
- Not "zeroised disk data". Only process-owned buffers are zeroed.
