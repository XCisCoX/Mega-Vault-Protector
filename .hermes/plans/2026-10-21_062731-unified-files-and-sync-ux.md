# Unified Files, Website and Sync Experience — Implementation Plan

> **For Hermes:** Execute in bounded tasks with specification and code-quality review. Load the applicable repository, Android build, testing and security skills before implementation. Do not commit: the user reviews and commits each completed phase.

**Goal:** Make ciscox.ir, the Windows app and the Android app feel like one approachable personal file library: sign in, open your files, add something, and see it on your other devices without learning sync terminology.

**Architecture:** Preserve the local encrypted vaults and existing protocol-4 relay; evolve them behind compatible capability negotiation. Build a real browser file client, a shared key/unlock contract, and one durable sync coordinator per native client. Keep administrative configuration out of the normal file journey. The server never receives a content-decryption key or vault password.

**Tech stack:** Existing Python/SQLite relay, HTML/CSS/JavaScript website, Qt/C++ Windows app, Kotlin/Compose Android app. Evaluate a narrow C++/WebAssembly package-crypto bridge before promising browser compatibility; native package code currently uses Argon2, libsodium and zlib. Do not assume those browser build dependencies exist.

**Status:** Planning only. No deployment or new product-code edits authorized by this plan turn. Prior uncommitted work stays intact; assess it rather than treating it as production-ready.

---

## 1. What the inspection actually establishes

- The live public ciscox.ir page calls itself “MegaVault console” and foregrounds opaque packages, device tokens and hosting. This was read in the browser during planning; no account sign-in was performed during this turn.
- `sync_server/web/index.html` opens to Overview, Vaults, Devices, Account and Connect the app; Files is absent.
- `sync_server/tests/test_web.py::WebTests.test_the_ui_never_touches_records_or_blobs` explicitly forbids browser access to records/blobs. A MEGA-like website requires a deliberate product/API/security change, not a CSS refresh.
- `sync_server/README.md` documents encrypted content but readable names, paths, tags and sizes. Do not claim metadata confidentiality or MEGA-equivalent security. Decide metadata encryption separately; it is not delivered by hiding fields in the UI.
- Android `SyncScreen.kt` already uses `SyncV4Client` and `SyncV4Engine`. Earlier statements that its active screen was v3 were incorrect; `android/SYNC_V3.md` is historical and must not drive this design.
- Android setup asks for an account password and an independently typed package secret; a successful account login does not establish that a device can decrypt this library.
- Windows has existing sync clients, engine, state and UI tests. Reuse them; do not create another transport implementation.
- Existing Android auto-sync work is unverified end-to-end. `SyncV4Auto` and `SyncV4Run` are separate runners; Boolean guards do not provide mutual exclusion. Attaching without a saved link ends the periodic loop; adopting a link does not restart it. Coroutine cancellation does not necessarily stop blocking native or HTTP work. Lock, vault-switch and disconnect need a session-generation boundary.
- Current onboarding/empty-state text claiming files “never leave this phone” or always “stay on this phone” is false when sync/export/sharing is enabled. Replace with precise copy, not a stronger privacy promise.
- Existing data and account state must be preserved. A prior refusal on SSH port 22 is not proof of a firewall: record it as “deployment access unresolved; connection refused at last check.” Do not access a different server as a workaround.

## 2. Product contract: one understandable model

### Everyday words

| User sees | Meaning | Keep out of normal screens |
|---|---|---|
| Files | The contents of the selected library | Records, hashes, blobs, cursor |
| Add | Bring in photos, videos, music or documents; create a folder | Export, sync configuration, extensions |
| Sync across devices | Keep this library on the account and approved devices | Device token, package secret |
| Available offline | This device has a verified usable local copy | Local mapping id |
| Remove download | Free this device’s space without deleting the file elsewhere | Ambiguous Remove |
| Move to Trash | Remove across this synced library, with recovery | Silent permanent deletion |
| Encrypted backup | A portable .mvpk package, with a short explanation | Import/export package jargon |
| Connect a device | Approve another device to access the same library | Copy a token/vault id/key |

“Library” remains the named encrypted collection, but do not demand a new collection if an existing default can be used. Keep a library switcher if multiple libraries exist. Existing local-only libraries remain local-only until the user chooses to connect them.

### Navigation

- **Website:** Files (default), Photos and Music as filters/views, Trash; a small persistent Transfers tray. Devices and Settings are secondary. Storage usage is quiet supporting information, not the home screen.
- **Windows:** library title/switcher, search, a labelled primary **+ Add**, and labelled **More**. More contains Music, Sync status, Devices, Settings, Lock, updates and a separate Encrypted backups group. Preserve keyboard shortcuts and selected-file actions.
- **Android:** title/switcher, **+ Add**, **More** with accessible names and at least 48dp targets; search and view/filter controls in the second row. A scrollable bottom sheet for Add; a separate More sheet. Do not squeeze everything into one toolbar row.
- **Add everywhere:** Photos & videos, Music, Choose files, New folder; Notes/Recording only where already supported. Use platform pickers. No sync, export, account settings or extension administration here.
- **More → Encrypted backups:** Restore a backup (.mvpk), Create a backup (.mvpk), a one-sentence explanation. This is manual portability, not the primary syncing workflow.
- Use the same conceptual hierarchy, not identical pixel layouts on every screen.

### Visual and accessibility rules

- Establish tokens for typography, spacing, surfaces, selection, focus and status; implement equivalent Qt, Compose and CSS mappings.
- Calm neutral surfaces; one strong primary action; avoid black placeholder squares and excessive blur behind small text.
- Do not rely on colour alone for status. Use icon + short text.
- Normal text contrast >=4.5:1, large text >=3:1; meaningful focus rings and keyboard order.
- Test Light, Dark and System separately, plus 200% text scaling, 320px browser width, narrow desktop windows, long/non-Latin names and right-to-left text.
- Loading, empty, filtered-empty, offline, locked, permission-denied and error are distinct states. Never present a spinner forever or a blank grid with no explanation.

## 3. The user journeys to build

### A. Website first visit

1. Simple sign-in with the real registration policy; do not show a broken sign-up CTA when registration is closed.
2. Existing user lands in Files for the last library; a new user is guided to create their first library.
3. If contents are locked, offer **Unlock files** or **Approve from another device**. Account access and content unlock remain separate security steps, without cryptographic jargon.
4. Show Add files, a drop area and one sentence on the empty state. A locked library is not an empty library.
5. Choose a file or drop it → visible encryption/upload progress → usable file row → download/preview works in the browser.
6. Reload/reopen does not lose completed transfers or pretend an incomplete file is ready.

### B. Connect an existing phone or PC library

1. More → Sync across devices.
2. Sign in; the service URL defaults to this product’s host, with Change server under Advanced. Never hardcode user credentials.
3. Select or create a named library, showing existing contents before merging. Explicitly confirm whether this uploads the whole local library; do not silently bulk-upload private files.
4. Prove access to the existing library key before importing/uploading; reject a wrong key before transfer work.
5. Show a short initial-sync summary and enter the normal Files screen. Subsequent changes synchronize automatically.

### C. Add another device

1. Signed-in user chooses Connect a device; device name is human readable.
2. QR starts a short-lived pairing request; it must not contain a permanent bearer token or a plaintext library key.
3. Already-unlocked device explicitly approves the requesting device with a verification step.
4. Transfer the key using an audited authenticated key-transfer design, with expiry, single use, replay prevention and device revocation. Do not invent a custom cryptographic protocol to make pairing look simple.
5. The new device opens the same Files view; no URL, token, hash or vault id entry in the normal path.
6. Manual recovery-key import remains an escape hatch when no approved device is available.

### D. Offline and deletion

- Offline edits queue durably and reconcile on reconnect; status says “Waiting for connection” rather than “Sync failed.”
- Phone defaults to metadata-first for new remote items; explicit Available offline controls bulk storage use. Existing local items stay local. Opening a remote item fetches it with visible progress and cancellation.
- Trash is recoverable and syncs to all clients. Remove download affects only this device and cannot become a remote delete.
- Authentication expired → Sign in again; storage full → Manage storage; wrong unlock secret → Unlock files; each error has an actionable next step.

## 4. Implementation work packages and gates

Work packages below are ordered by dependency. Split each into test-first, minimal implementation and review tasks. A phase is complete only when its acceptance gate passes, not when its files compile.

### Phase 0 — Safety baseline and real journey inventory

**Files to inspect:** `docs/sync-mega-plan.md`, `docs/sync-protocol.md`, `android/SYNC_V3.md`, `sync_server/README.md`, both native sync implementations, `core/src/share_package.cpp`, `core/src/internal/package.hpp`.

- Capture current git status and preserve all uncommitted changes. Inventory partial toolbar tests/edits left by the interrupted Windows task.
- Establish a separate staging server/data directory and isolated phone/PC test vaults. Never reset app data, overwrite real libraries, uninstall the app, or use production files as fixtures.
- Obtain supported deployment access for exactly the ciscox.ir host; no credential files in the repository, scripts or plan. Use secure credential entry; rotate plaintext credentials previously stored by automation before production release.
- Inventory create, sign in, unlock, link, upload, download, delete, restore and revoke behavior with fixtures. Establish which paths currently work across devices; do not trust earlier summary claims.
- Run the relay/native baseline suites and record failures before edits.
- Verified interrupted Windows work: `qt-app/CMakeLists.txt` registers an offscreen `library_commands` test; `qt-app/tests/gallery_performance.cpp` adds expectations for the proposed Add/More design. Production toolbar code is unchanged, so this is unfinished test-first scaffolding, not a delivered feature. Reconcile or complete it when Phase 6 begins; do not claim the baseline is green with this test present.
- Verified native baseline blocker: `qt-app/src/main_window.cpp:1370` calls `QSize::grownBy`, which does not compile against pinned Qt 5.12.12 (MSVC C2039 in `mvp-toolbar-red.log`). Resolve with a supported size/margins calculation and a focused regression test before native verification. No Windows build success was obtained by the interrupted task.

**Gate:** reproducible baseline + backups/restore rehearsal + explicit test data and deployment target. No data migration before this gate.

### Phase 1 — Agree the complete experience with clickable designs

**Create:** `docs/ux/unified-experience.md`, `docs/ux/state-and-copy-contract.md`, `docs/ux/prototype/` (standalone prototype only).

- Draw the same first-use, existing-user and offline paths for browser, desktop and phone.
- Prototype six key states: sign in, locked Files, empty Files, populated Files, Transfers, Connect device; include Add and More sheets/menus.
- Define what happens on every primary button; show realistic fixture file names but clearly label prototype data.
- Use short labels and supporting explanations; remove “console”, “token”, “package secret”, “derive key” and protocol numbers from ordinary use.
- Usability check: someone unfamiliar with the product adds a photo, finds it, connects another device and recovers a trashed item without verbal coaching.

**Gate:** the user approves navigation and the setup flow. A beautiful static dashboard is not approval of a working file app.

### Phase 2 — Browser crypto/key-compatibility feasibility gate

**Existing files:** `core/src/share_package.cpp`, `core/src/internal/package.hpp`, `core/src/internal/crypto.hpp`, `core/include/videovault/core/vault.hpp`.
**Proposed new files:** `tools/web-crypto/CMakeLists.txt`, `tools/web-crypto/bridge.cpp`, `sync_server/web/crypto-worker.js`, `tests/web_crypto_compatibility.py`.

- Generate non-secret native .mvpk fixtures, including compressed/uncompressed files, Unicode paths, empty files and multi-chunk packages.
- Prove a narrow browser-compatible bridge can decrypt a native fixture and encrypt one the native core imports identically. Bound memory, KDF input parameters, decompression output, worker time and message sizes.
- Verify wrong passwords, corruption, truncation and tampered headers fail closed; preview is never shown before authentication succeeds.
- Keep key material in a worker/in-memory session; no permanent tokens/keys in localStorage, URLs, analytics or logs. Clear on lock/sign out.
- Measure real browser memory/time on a phone before choosing whole-package buffering. Large-file browser limits need explicit UI handling, not a fake universal promise.
- Reuse existing native crypto, not a second independently interpreted file format. If WASM is unsuitable, stop at this gate and document an alternative design for review.

**Gate:** real cross-client encrypted roundtrip with corruption tests. Without this, do not advertise browser upload/download/playback as implemented.

### Phase 3 — Safe browser API and key distribution

**Modify:** `sync_server/__main__.py`, `sync_server/store.py`, `sync_server/tests/test_web.py`, `test_accounts.py`, `test_records.py`, `test_blobs.py`.
**Create:** `sync_server/tests/test_pairing.py`, `sync_server/tests/test_browser_files.py`, `docs/ux/key-and-browser-security.md`.

- Specify content-key ownership, legacy package-secret verification and optional versioned key envelopes. Existing local vault passwords do not automatically become account passwords or a shared sync key.
- Support explicit legacy unlock; never silently replace existing package secrets. If migrating to random per-library keys, store only wrapped keys at the server, supply a recovery mechanism, and stage resumable migration with backward compatibility.
- Add capability negotiation so old clients continue to work and unsupported features have honest UI.
- Introduce an explicitly scoped browser-file session/API with account authorization, CSRF protection on writes, secure cookies, Origin checks and download/range validation. Do not simply remove bearer checks or inject a long-lived native device token into browser storage.
- Replace the old “website never touches files” test with intentional positive browser authorization tests and equally strong negative cross-account/cross-origin tests. Retain the native bearer contract.
- Add rate-limited, expiring, one-use pairing requests, explicit approval/revocation and audited encrypted key transfer. Revocation prevents future access; it cannot retract files already downloaded.
- Add read-only device activity/last-success metadata without logging filenames, secrets or file content. Keep operator diagnostics separate from consumer Files.

**Gate:** security review, legacy-client regression suite, unauthorized reads/writes rejected, incorrect unlock cannot link a device.

### Phase 4 — Make ciscox.ir a real file application

**Modify:** `sync_server/web/index.html`, `sync_server/web/style.css`, `sync_server/web/app.js`, static-asset/CSP routing in `sync_server/__main__.py`.
**Create as needed:** `sync_server/web/files.js`, `sync_server/web/transfers.js`, `sync_server/web/unlock.js`, browser worker assets; one declared browser-test setup rather than assuming Playwright is installed.

- Files becomes the landing page, with folder breadcrumbs, search, list/grid, sort, selection, keyboard navigation and accessible contextual actions.
- Add files/folder, drag/drop, rename, move, download, Trash and Restore call real APIs. For empty folder support, define how folder-only records survive sync instead of deriving every folder solely from file paths.
- Client-side encryption/decryption uses Phase 2; no plaintext upload proxy. Download returns the user’s file, not an unexplained .mvpk attachment.
- Provide safe preview support for the types actually tested. For unsupported formats, show a clear Download/Open in app action; never imply every codec works in every browser.
- Durable transfer queue checkpoints ciphertext safely; pause/resume/cancel and byte progress. No persistent plaintext caches by default.
- Devices page shows real names, last seen/status, Connect device and Revoke. No “Issue a token” as the normal action.
- Account/settings/usage and operator information move away from the home page. Deleting an entire library/account is segregated with an explicit destructive confirmation.

**Gate:** a new browser user can sign in, unlock, add, find, download, delete and restore real fixtures without a terminal or native app (except optional approved-device unlock).

### Phase 5 — Fix the sync foundation before calling it effortless

**Modify Android:** `SyncV4Auto.kt`, `SyncV4Run.kt`, `SyncV4Engine.kt`, `SyncV4Client.kt`, `SyncV4State.kt`, `SyncScreen.kt`, `MainActivity.kt`, mutation producers including `VaultScreen.kt`.
**Modify Windows:** `qt-app/src/sync_engine.cpp`, `sync_client.cpp`, `sync_state.cpp`, `sync_page.cpp`, their headers and `qt-app/tests/sync_engine.cpp`, `sync_state.cpp`, `sync_page.cpp`.

- Consolidate manual/automatic operations into one serialized coordinator per open library. Protect state with proper concurrency primitives; UI state updates occur on the UI dispatcher.
- Persist the work queue and checkpoints. A process-local pending Boolean is not durable work and not sufficient mutual exclusion.
- Lock/disconnect/switch invalidates the session generation, cancels actual I/O where possible, and prevents late callbacks from importing into a different vault or writing old credentials back.
- Starting sync after initial setup must create the periodic/reconnect loop even if the app opened without a saved link.
- Ensure all mutation paths enqueue work, including tags, batch delete, folder operations and plugin imports. Queue after committed changes, not merely before starting an operation.
- Separate progress from ready/synced status: only authenticated verified imports/commits count as done. Carry byte totals, per-file errors, remaining items and cancellation.
- Bound parallel transfers while keeping ledger/index application serialized; avoid repeating Argon2/package export and hashing for every unchanged file.
- Preserve offline edits during pull; stale versions produce conflict copies rather than silent overwrite. Audit fingerprint refresh behavior against pending local edits.
- Reconcile Trash, explicit device-only eviction, purge and cursor checkpoints; do not advance a cursor past work that was not safely recorded/applied.
- Publish short actionable states: All changes saved / Uploading… / Waiting for connection / Sign in again / Not enough space.
- Define Android foreground-service/background scheduling policy with battery/network constraints. Do not promise a 90-second always-on timer while Android has suspended or killed the app.

**Gate:** app restart, network loss, lock mid-transfer, concurrent manual/automatic sync, wrong key, token revocation and conflict tests pass with no lost/duplicate files.

### Phase 6 — Apply the same Add/More UX to phone and Windows

**Android files:** `VaultScreen.kt`, `Components.kt`, `LoginScreen.kt`, `SetupScreen.kt`, `SyncScreen.kt`, `SettingsDialog.kt`, `DeviceLibrary.kt`, `Theme.kt`.
**Windows files:** `qt-app/src/main_window.cpp`, its header, `sync_page.cpp`, settings/onboarding components located during Phase 0, `qt-app/tests/gallery_performance.cpp` and focused navigation tests.

- Finish the requested primary + Add and secondary More menus on both platforms; do not lose any callback, shortcut, context action or enabled/remote-library condition.
- Add sheet is short and descriptive. Move backups, devices, sync setup, updates, settings, extensions and Lock out of it.
- Keep selected-file actions contextual and label export-as-plaintext versus encrypted backup distinctly. Lock remains reachable in one More menu plus existing shortcuts; do not accidentally hide it behind Advanced.
- Keep cloud/peer/local modes distinguishable and prevent unavailable Add actions in a read-only remote vault.
- Correct misleading privacy and recovery copy on setup, unlock and empty states. Account-password recovery is not file-key recovery.
- Prevent the empty state from showing over loading, filtered results or a connected remote library; one appropriate CTA per state.

**Gate:** no toolbar clipping, all prior functions discoverable, matching language across browser/native, platform accessibility checks and real native builds pass.

### Phase 7 — Staged migration, deployment and end-to-end proof

- Build/version server and clients; install compatible clients before enabling incompatible server features. Use a reversible feature flag for new key/browser/pairing paths, not wholesale deletion of the current implementation.
- Back up SQLite with a consistent backup method and preserve blobs, key envelopes and local link/ledger state. Rehearse restoration on staging.
- Add a real fixture on Android → observe on Windows and website; add on website → verify Android and Windows download; add on Windows → verify phone/browser. Compare original and decrypted hashes with tool output.
- Verify rename/move/tags, offline edit conflicts, recoverable Trash and restore propagate. Test Remove download never deletes the remote file.
- Test original problematic M4A as a separate verified playback acceptance case; do not claim it is solved by navigation changes.
- Prove encrypted transfer semantics with network/server inspection: no vault key/password/file plaintext received by relay, and no false metadata-encryption claims.
- Obtain deployment access for ciscox.ir, deploy to that exact host with backup and rollback, then read back health/capabilities/assets and repeat authenticated file operations. Local code is not proof of a deployed fix.
- User performs project GUI testing on their devices; automated headless/UI-harness checks are permitted but do not launch their installed GUI without permission.

**Gate:** documented three-device/browser roundtrip, user usability pass, and rollback validation. Only then say “ready.”

## 5. Verification commands and scenarios

Run from repo root, against disposable fixtures, not the live account:

- Relay: `python -m unittest discover -s sync_server/tests -p "test_*.py" -v` (first confirm the test support’s environment/toolchain requirements).
- Windows: build through the existing MSVC developer-environment wrapper and `cmake --preset vs2022-x64`; execute `ctest --preset vs2022-x64-debug --output-on-failure`. Inspect discovered suites; do not repeat historical test counts as current facts.
- Android: `bash ~/android-dev/build-android-local.sh apk`; in the synchronized build mirror run Gradle `testDebugUnitTest` with the same SDK/JDK/prefix environment. Add scheduler/state unit tests and native fixture integration tests, not just source-string assertions.
- Browser: introduce declared pinned browser-test dependencies; test actual staging app at mobile/desktop widths, both themes, keyboard-only and screen-reader semantics. Add/delete/restore assertions must read back persisted state.
- Inspect final APK contents, install without clearing app data only when the user’s phone is unlocked, and read package update metadata back. A successful build alone does not verify pairing or sync.
- Every phase records actual commands/results/blockers. No invented outputs, fabricated production files or placeholder “synced” statuses.

### Beginner usability release checklist

- [ ] First click after sign-in is obvious; Files is home.
- [ ] Add a photo without understanding vault layout, tokens, hashes or protocols.
- [ ] See where the file is, whether it is uploaded and whether it works offline.
- [ ] Connect another device with approval, no token/id/key typing in the normal flow.
- [ ] Use the website for actual files, not just account administration.
- [ ] Understand locked vs empty vs offline, and know the next action in each.
- [ ] Recover an accidental delete without admin/server work.
- [ ] No false claim that syncing files never leave the phone.
- [ ] No permanent-key exposure through QR, URLs, logs or browser storage.
- [ ] No lost edits or duplicates after restart, interruption or conflict.

## 6. Risks and explicit scope decisions

- **Browser crypto is substantial work:** native .mvpk compatibility plus memory-safe large-file handling is a prerequisite, not a final cosmetic task.
- **Recovery must stay honest:** account reset cannot unlock encrypted files unless an explicitly designed recovery mechanism exists. Never store a plaintext master key to simplify onboarding.
- **Metadata privacy differs today:** readable filenames/paths/tags are part of v4. Full encrypted-metadata search/migration is a separate versioned security project; do not casually claim it is already private.
- **Existing libraries may use different secrets:** do not merge incompatible libraries by name or assume their local passwords match. Verify key access and preserve originals before migration.
- **Durability matters more than animation:** prioritize tested queues, cancellation, key verification and deletion semantics before visual motion.
- **Production access is unresolved:** useful local builds/prototypes can proceed, but production deployment needs the correct host’s access and rollback.
- **No broad rewrite by default:** preserve working native crypto, local databases, transport and APIs where possible. Extend compatibly, add targeted boundaries, and retain rollback flags.
- **No commit/push without the user:** leave reviewed phases uncommitted.

## 7. Honest evaluation and completion reporting

Current subjective beginner usability: approximately **4/10 overall**; the public website is an operator-facing console rather than a useful file destination. This is an assessment, not a measured usability study. Toolbar/copy fixes improve individual screens but cannot justify an 8–10 rating while setup, browser files and sync trust remain unresolved.

Target: **8/10 demonstrated usability**, earned by uncoached completion of the checklist across phone, PC and website. Report each phase as designed, implemented, built, tested or deployed separately. Do not describe the whole product as fixed based only on a successful APK build.
