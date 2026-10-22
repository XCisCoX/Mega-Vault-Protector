# Hard lock design

## The problem it fixes

Before this work, the ordinary Lock action was not a lock:

```cpp
// MainWindow::lockVault(), before
if (peerLinked()) {
    ... showLogin(root);
    return;          // <-- vault_ stays open, keys stay in RAM
}
```

If a phone was sharing the vault, pressing Lock showed the login screen and left
`Vault::Impl` fully alive: master key, open SQLCipher connection, and a share
server holding a `shared_ptr<core::Vault>` that could decrypt on demand. The
same shape existed on Android (`lockNow()` set `shareHoldsVault = true` and
returned without calling `nativeLock`). Anyone who dumped the process after
"locking" got a working vault.

## The model

Two operations, named honestly:

| Operation | What it does | When it is used |
| --- | --- | --- |
| **Hard lock** | revokes the session, closes the database, wipes keys, clears caches, stops sharing and sync | the ordinary Lock action, and the idle auto-lock — always |
| **Privacy screen** | hides the library; the vault stays unlocked and sharing keeps working | only after the user explicitly picks it in the Lock dialog, and the screen says so |

Hard lock is the default and the only outcome of the Lock button when nothing is
shared. A privacy screen is never entered silently.

### Core: what a hard lock does

`Vault::lock()`:

1. takes the vault mutex (so no operation is mid-decrypt),
2. `impl_->session_->revoke()` — flips the `SessionGuard` shared with every
   `PackageReader` handed out during this unlock,
3. `impl_.reset()` — destroys `master_key_` (`sodium_memzero` + `sodium_free`)
   and the `Database` (SQLCipher connection closed, page cache released),
4. `lock_generation_ += 1` — asynchronous callers can tell their result belongs
   to a session that ended,
5. publishes `unlocked_ = false`.

Order matters: revocation before key destruction means an object that already
left the mutex (a media probe, a share worker) cannot start another decryption
while the keys are being destroyed.

### Revoking capabilities, not just keys

`PackageReader` is the one object that legitimately outlives the vault mutex:
`Vault::media_info()` deliberately opens a reader inside the lock and probes
outside it, so a duration column does not block playback behind FFmpeg.

Such a reader now carries a `std::shared_ptr<internal::SessionGuard>`:

```cpp
Result<bool> PackageReader::seek(uint64_t offset) {
    if (revoked()) { clear_cache(); return VaultError{AuthenticationFailed, ...}; }
    ...
}
```

`read()`, `seek()` and the internal `chunk()` all check the guard. On detection
they also zeroise the cached plaintext. A reader that outlives a hard lock
therefore cannot decrypt another byte; its already-unwrapped file key dies when
the reader is destroyed, which is the next thing that happens to it.

### Rejecting stale results

`Vault::lock_generation()` gives the UI a cheap stamp. Any asynchronous task that
captured a generation and finds it changed must discard its result instead of
repopulating a view for a vault that no longer exists. The desktop gallery
already invalidates by `galleryGeneration_`; the vault generation is the
authoritative one and is advanced by every `lock()`.

## Desktop flow (`MainWindow::lockVault`)

1. `closeText()` — a decrypted note must be saved or dropped before locking.
2. If a phone is linked: `InWindowSheet` with
   * **Lock and stop sharing** (primary, the default),
   * **Hide screen, keep sharing** (labelled with the consequence),
   * **Cancel** — nothing happens, the vault stays as it was.
3. Common teardown: close the settings page, sync engine, plugin/vault-bound
   windows, peer browser and approval sheet, player, music, record and text
   windows; clear the gallery cache; bump the gallery generation.
4. If the user chose to keep sharing → `enterPrivacyScreen()`: the vault is
   untouched and the login page shows a red banner naming who still has access,
   plus a "Lock the vault now and stop sharing" button.
5. Otherwise → hard lock: `shareServer_->setLocalVault(nullptr)` (drops its
   handle), `ShareServer::stop()` (wipes link token/key, `vault->lock()`),
   `vault_->lock()`, `vault_.reset()`.

The idle auto-lock sets `interactiveLock_ = false` and therefore never shows the
dialog: an unattended machine always hard-locks.

`peerChanged()` closes the loop: when the phone disconnects while a privacy
screen is up, the vault is locked immediately — there is nothing left to share.

## Android flow

`MainActivity::requestLock()` shows an `AlertDialog` when `PeerLink.linked()`
("Lock and stop sharing" / "Hide screen, keep sharing"). The hard path calls
`PeerLink.unlink()`, disconnects a remote vault, calls `CoreBridge.nativeLock()`
(which zeroises `g_plain` and locks the core vault) and stops music playback.
The privacy path sets `shareHoldsVault = true`; `LoginScreen` then renders the
same warning and a "Lock the vault now" button, and the existing
`LaunchedEffect(PeerLink.pcName)` finishes the lock when the PC goes away.

`FLAG_SECURE` is set for every screen, so the lock screen, the gallery and the
players never appear in screenshots or the Recents thumbnail.

## Policy for playback

- A hard lock stops playback: `lockVault` closes the player/music windows and,
  on Android, stops `MusicPlaybackService` when `MusicHub.active`. This is
  intentional — a lock that leaves a decryption pipeline running is not a lock.
- A privacy screen keeps playback and sharing alive and says so.
- Backgrounding the app is not a lock: the idle timer (`VaultLocation.idleLockMs`)
  decides, and when it fires it takes the hard-lock path.

## What a hard lock guarantees — and what it does not

Guaranteed (asserted by `tests/security_memory.cpp`):

- no live copy of the master key remains (measured 0 occurrences),
- readers cannot decrypt after revocation,
- decrypted chunk caches are zeroed, not just freed,
- a locked vault refuses metadata and content calls,
- concurrent reads cannot outlive the lock.

Not guaranteed:

- bytes already released by the allocator, by Qt/ART, by SQLCipher or by FFmpeg
  before the lock — see `REMAINING_RISKS.md`,
- protection against a privileged attacker who can read the process at will,
- erasure of plaintext the user exported or restored to disk.
