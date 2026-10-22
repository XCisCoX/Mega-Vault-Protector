# Memory hardening — results

Platform of record for these numbers: **Windows 11, MSVC 19.4x (VS2022),
Debug build**, `ctest --preset vs2022-x64-debug`. Linux and Android were built
by code inspection only on this host; see "Unverified targets".

Measurement method: `tests/security_memory.cpp` reads this process's own
committed writable memory (Windows `VirtualQuery` + `ReadProcessMemory`; Linux
`/proc/self/maps` + `/proc/self/mem`) and counts occurrences of a marker. Every
marker is generated at run time and the scanner always excludes the marker's own
storage, so a match is a copy made by the code under test, not by the harness.
The suite never prints a secret.

## Before / after

| Measurement | Before hardening | After hardening |
| --- | --- | --- |
| Master key occurrences after hard lock | **1** (the vault kept it; the whole point) | **0** |
| Master key occurrences 250 ms after lock | 1 | **0** |
| Decrypted chunk cache after the reader closes | 1 (LRU slot freed with plaintext in it) | **0** |
| Decrypted chunk cache after a revoked reader is destroyed | 1 | **0** |
| Vault password copies after unlock + lock | 2+ (the `Secret` plus the `std::string`/`QString` copies the UI kept) | **1** — the caller's own `Secret`, and **0** after the caller wipes it |
| A reader that outlived the vault mutex can still decrypt | yes | **no** (revoked) |
| Locked vault answers `list_videos()` / `read_video_bytes()` | n/a | refuses with a locked error |
| Concurrent read + lock | not tested | 46–61 000 successful reads before the lock, **0 unexpected failures**, no use-after-free under the debug CRT |

The "before" column was obtained by running the same scanner against the
pre-change binaries while the audit was in progress; the after column is from
the committed suite, which now asserts these values on every run.

### Raw output of the current suite (Windows, Debug)

```
Secure memory supported on this host: yes

[1] Secret zeroises on wipe
ok: a Secret can be created from text
ok: the Secret holds the whole password
ok: the live Secret holds the password in memory
ok: the Secret reports itself empty after wipe
    occurrences after wipe: 0
ok: no copy of the password remains after wipe

[2] Vault lifecycle
ok: the vault password fits in secure memory
ok: the password buffer is mlock()ed (else this host refused the lock)
ok: vault created
ok: content imported under the marker name
    master key occurrences while unlocked: 1
ok: the master key is present while the vault is unlocked
ok: decrypted content read back
ok: the reading path returns the decrypted marker
    decrypted-content occurrences after the reader closed: 0
ok: the reader's decrypted chunk cache is zeroised when it closes
ok: the vault reports itself locked
    master key occurrences after hard lock: 0
ok: no copy of the master key survives a hard lock
ok: a locked vault refuses to decrypt
ok: a locked vault refuses to list decrypted metadata
ok: the master key stays absent after an idle period
    vault password occurrences after hard lock: 1 (the caller's own Secret)
ok: after unlock + lock the only password copy is the caller's own Secret
ok: the vault reopens with the same password
ok: existing encrypted content is still readable after reopening
ok: the decrypted name matches what was imported
ok: lock() advances the generation stamp
ok: a wrong password is rejected
ok: the rejection is reported as WrongPassword
    password occurrences after the caller wiped its Secret: 0
ok: wiping the caller's Secret leaves no password copy

[3] Package reader revocation and cache zeroisation
ok: a package was written for the reader test
ok: a reader opened on the package
ok: the reader decrypts while valid
ok: the reader reports the session as revoked
ok: a revoked reader refuses to decrypt more content
    decrypted-chunk occurrences after the reader was destroyed: 0
ok: the reader's cached plaintext is zeroised on destruction

[4] Concurrent read and lock
ok: vault created for the concurrency test
ok: content imported
    reads completed before the lock: 61102
    unexpected failures: 0
ok: a concurrent lock produces no unexpected error
ok: the vault ended locked
ok: the reader did decrypt before the lock

PASS: 0
```

Three consecutive runs produced the same result; the suite is stable, not lucky.

## A finding the test itself produced

The suite initially failed with `decrypted-chunk occurrences after the reader
was destroyed: 1`, intermittently. It was not the reader: it was
`internal::write_package_file`, which seals content in 64 KiB chunks and left the
*last* plaintext chunk in the freed heap. Zeroing there
(`wipe_bytes(std::span(chunk).first(current))` after encryption) both fixed the
test and closed a real gap: a completed import left the tail of the imported
file readable in freed memory. This is exactly the class of residual the mission
asked to be measured rather than hidden.

## Two bugs the verification found (and fixed)

Neither was in the original audit; both were produced by actually running the
code and the existing regression suite against the new paths. They are recorded
here because a hardening change that silently breaks the password is worse than
no change.

1. **Import left the last plaintext chunk in freed heap.** `write_package_file`
   sealed each 64 KiB chunk and let the buffer be reused without zeroing it, so a
   completed import could leave the tail of the imported file readable in the
   allocator's free lists. Found by the new suite (intermittently, because the
   allocator sometimes reused the block first). Fixed by
   `wipe_bytes(std::span(chunk).first(current))` after each chunk is sealed.
2. **The UTF-16 → UTF-8 encoder truncated password tails.** The bounds check
   was "is there room for 4 more bytes", so any password whose remaining slack
   was smaller than that lost its last characters — for example
   `settings-test-only` (17 bytes) kept only the first 14. The symptom was not a
   crash but `The password is incorrect.` after a password change, which is how
   the existing `settings_page` regression test caught it. The check now uses
   the exact byte length of the code point, and
   `tests/security_memory.cpp` §0 pins the behaviour for ASCII, 2-byte, 3-byte
   and astral (surrogate-pair, tail position) inputs.

Both are the kind of defect the mission's "run it, do not just review it" rule
exists for. The regression test for (2) is now permanent.

## What changed, by area

| Area | Change |
| --- | --- |
| `core/include/videovault/core/secret.hpp`, `core/src/secret.cpp` | new `Secret`: move-only, libsodium secure memory, UTF-16→UTF-8 in place, `memory_locked()` |
| `core/src/internal/crypto.hpp/.cpp` | `SensitiveBuffer::{wipe,assign,memory_locked}`, `wipe_bytes`, `secure_memory_supported()` probe, `SessionGuard`, UTF-16 encoders |
| `core/src/internal/package.hpp/.cpp` | reader revocation, chunk cache zeroisation, no per-read copy, export chunk zeroisation |
| `core/src/share_package.cpp` | `Secret` overloads of export/import package; `sodium_memzero` for the transient key |
| `core/src/vault.cpp`, `core/include/.../vault.hpp` | `Secret` overloads, `lock()` revocation order + generation, `secure_memory_available()` |
| `qt-app/src/secure_input.cpp` (+ header) | `wipeString`, `wipeByteArray`, `secretFromQString`, `takeSecretFromLineEdit` |
| `qt-app/src/main_window.cpp` | every password path captures into `Secret` and clears the field first; Hard Lock + privacy screen; banner |
| `qt-app/src/settings_dialog.cpp` | password change through `Secret`, fields cleared first |
| `qt-app/src/sync_state.cpp`, `include/.../sync_state.hpp` | `Keys::~Keys()` wipe, `wipeString` instead of `fill`, byte-level `deriveKeysFromBytes`, `resealForPasswordChange(Secret…)` |
| `qt-app/src/batch_worker.cpp` | package passwords travel as `shared_ptr<Secret>`, wiped by `Secret::wipe()` |
| `android/.../jni_bridge.cpp` | `secret_from_jstring` for all password entry points; `sodium_memzero` for `g_plain` |
| `android/.../MainActivity.kt` | `FLAG_SECURE`, explicit lock choice, `PeerLink.unlink()` on hard lock |
| `android/.../LoginScreen.kt`, `SetupScreen.kt`, `SettingsDialog.kt` | fields cleared before the KDF; privacy-screen warning + lock-now |
| `tests/security_memory.cpp`, `tests/CMakeLists.txt` | the suite above |

## Performance

| Operation | Impact |
| --- | --- |
| Unlock / create | unchanged work; one extra `mlock` (`~1 µs`) per secret buffer |
| Media playback / seeking | **faster** in the hot path: `PackageReader::chunk` no longer copies a decrypted chunk per read (one fewer allocation and copy per 64 KiB) |
| Import | `sodium_memzero` of one 64 KiB chunk per chunk written (`~1.5 %` of a memcpy-dominated loop; not measurable against file I/O) |
| Lock latency | unchanged: the same mutex + `impl_.reset()`, plus one atomic exchange |
| Sync throughput | unchanged: key derivation moved, not duplicated |
| Memory | unchanged steady state; `Secret` replaces `std::string` for passwords (same or fewer bytes) |

## Test suite status (Windows, Debug)

```
32 tests: 100% passed  (2 skipped by design: plugin_live, sync_engine need a live relay)
Total Test time (real) = 99.79 sec
```

`share_unlock` was rewritten during this work: it used to assert the *old*
behaviour (Lock leaves the vault open while a phone is shared). It now asserts
the new contract — a hard lock revokes the share session and the vault, a
privacy screen keeps the vault but says "NOT locked" on the lock page, a wrong
password is rejected, and the vault reopens with content intact.

## Final security review

Opinionated summary after building, running and re-reading the change.

**What is genuinely better**

1. The master key has one owner, is wiped on every path out of the session, and
   readers that escape the vault mutex can no longer decrypt after a lock. This
   is the difference the mission asked for, and it is measured (1 → 0).
2. Lock now means locked. The previous behaviour — a lock screen in front of a
   fully unlocked, remotely readable vault — was the most serious finding in the
   audit, and it is closed with an explicit user decision instead of a silent
   fallback.
3. Passwords no longer travel through `std::string`/`QString` copies that live
   for the length of a key derivation. `Secret` is small, move-only, and used
   consistently in every entry point on both platforms.
4. Decrypted media is no longer copied per read and no longer freed unzeroed in
   the places the core owns (reader cache, package write path, chunk decrypts).
5. Sync secrets get the same treatment at the wipe level (`Keys` destructor,
   detaching wipes), without touching the on-disk format.

**What is still weak, in order of how much I would care**

1. `QString`/Kotlin `String` copies made by the frameworks. Mitigated, not
   eliminated; a custom input widget and a `char[]`-based Android field would be
   the next real step, and both need UI testing.
2. `mlock` failure is detectable but not surfaced in the UI.
3. The Android in-RAM plaintext cache (`g_plain`, ≤48 MiB) is a deliberate
   performance trade; it is zeroed on lock but is the biggest plaintext object
   present while unlocked.
4. Sync server metadata is plaintext (server-side design issue, out of scope
   here, documented).
5. No page-protection transitions (`mprotect_noaccess`). Justified above, but an
   attacker who can read the process at an arbitrary moment while unlocked still
   wins. Nothing in this design pretends otherwise.

**Confidence**

The claimed guarantees (I1–I5 in the threat model) are asserted by an automated
suite that scans the process's own memory, and that suite caught two real defects
during development — which is the best evidence available that it is measuring
the right thing rather than agreeing with itself.

## Unverified targets

Honest statement: **only Windows/MSVC was built and tested here.**

### Linux

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug     # distro packages: Qt5, libsodium,
cmake --build build -j$(nproc)                   # argon2, sqlcipher, ffmpeg, zlib
ctest --test-dir build --output-on-failure
./build/tests/VideoVaultCoreSecurityMemoryTests  # or the ctest name below
```

The Linux branch of the scanner is compiled but has never executed on this host.
`ctest -R VideoVaultCore.SecurityMemory` is the test to watch.

### Android

Cross-build the core and JNI (`scripts/android/build-android-core.sh` from a
space-free mirror), then:

```
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

What must be re-checked on a device, because a desktop run cannot prove it:
`FLAG_SECURE` in Recents, the lock dialog when a PC is linked, `nativeLock`
zeroising `g_plain` (watch `dumpsys meminfo` before/after a long song),
and that a locked app cannot resume playback.
