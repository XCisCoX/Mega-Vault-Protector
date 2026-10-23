# Contributing to Mega Vault Protect

Contributions are welcome: bug fixes, tests, documentation, accessibility improvements, and focused features. You do not need to understand the entire codebase to help.

Mega Vault Protect stores private files in an encrypted vault and provides Windows, Linux, and Android interfaces over a shared C++ core. Changes should protect users' data, preserve vault compatibility, and keep common tasks understandable.

## Before you start

Read the [README](README.md) for the application's behavior, security model, and build instructions. Search [existing issues](https://github.com/XCisCoX/Mega-Vault-Protector/issues) and [pull requests](https://github.com/XCisCoX/Mega-Vault-Protector/pulls) before starting work.

Small fixes and documentation improvements can go directly to a pull request. For substantial features, new dependencies, storage-format changes, or changes to cryptography or sharing, open an issue first to discuss the problem and proposed approach. This helps avoid work that conflicts with the project's direction.

Keep discussions respectful and focused on the work. Explain disagreements with evidence and make room for contributors with different experience levels.

## Reporting bugs

[Open an issue](https://github.com/XCisCoX/Mega-Vault-Protector/issues/new) with:

- A clear description of the affected task.
- Steps to reproduce the problem.
- Expected and actual behavior.
- The release version or commit, operating system, and device architecture.
- Relevant logs or error messages, with sensitive information removed.
- A small sample file or test case when it is safe and useful.

Screenshots and recordings are optional. Use disposable vaults and sample data. Never upload passwords, encryption keys, private files, secret record values, or sharing-session QR codes.

For accessibility barriers, also describe your input method or assistive technology and the control or screen involved. See [ACCESSIBILITY.md](ACCESSIBILITY.md) for reporting guidance.

## Security reports

Do not post exploitable vulnerability details, working exploits, or sensitive vault data in public issues or pull requests.

If the repository's **Security** tab offers **Report a vulnerability**, use that private reporting option. Otherwise, check the maintainer's [GitHub profile](https://github.com/XCisCoX) for a published private contact method. If neither is available, open a minimal issue asking for a private reporting channel without disclosing technical details.

A useful private report describes the affected version, impact, reproduction steps using synthetic data, and any suggested mitigation. No fixed acknowledgement or resolution deadline is promised.

## Development setup

Fork the repository, clone your fork, and create a focused branch from the current default branch:

```sh
git clone https://github.com/YOUR_USERNAME/Mega-Vault-Protector.git
cd Mega-Vault-Protector
git switch -c fix/describe-your-change
```

Use a separate development vault. Back up any data you cannot afford to lose before testing changes to storage, imports, deletion, password changes, or migrations.

### Linux

The documented build environment is Ubuntu 24.04 with GCC 13 and Qt 5.15. Install the dependencies listed in the README, then run:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug -j "$(nproc)"
ctest --preset linux-debug --output-on-failure

out/build/linux-debug/qt-app/MegaVaultProtect
```

Use a CMake version that supports the repository's preset schema. Audio playback also needs the Qt Multimedia GStreamer backend and the plugins listed in the README.

### Windows

Use a Visual Studio 2022 developer shell, Qt 5.12.12 for MSVC2017 x64, and the project's vcpkg dependencies.

The Windows configure preset contains machine-specific paths for Qt, vcpkg, the Windows SDK, and dependency directories. Adapt them locally to your installation, preferably through an untracked `CMakeUserPresets.json` with a preset that inherits from `vs2022-x64`. Do not commit your personal paths. If you use a custom preset, substitute its names in the commands below.

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure
```

The README documents the FFmpeg import-library workaround. Check it if a dependency reinstall causes Windows linking failures.

### Android

The Android application uses Kotlin and Jetpack Compose with the shared core through JNI. The documented target is `arm64-v8a` on Android 8 or newer.

Follow [docs/ANDROID-BUILD.md](docs/ANDROID-BUILD.md) for the native dependencies, pinned NDK container, APK build, and installation. Test UI and JNI changes on a compatible device or emulator, and record the Android version and environment used. Building an APK alone does not verify runtime behavior.

## Finding the right place to work

| Directory | Responsibility |
| --- | --- |
| `core/` | Encryption, vault database, encrypted packages, streaming, metadata, tags, folders, and records |
| `qt-app/` | Windows and Linux interfaces, viewers, playback, record editor, text notes, LAN sharing and transfers, cloud server and browser, and updater |
| `android/` | Android interface, JNI integration, cloud client, updater, and platform behavior |
| `tests/` | Headless automated test suites |
| `scripts/` | Build and dependency tooling, release-version selection, and Linux packaging |
| `third_party/` | Dependency overlays |
| `docs/` | Architecture, Android build and signing, cloud TLS setup, screenshots, and project assets |
| `.github/` | CI and release workflows |

## Implementation expectations

Match the surrounding code's naming, formatting, and error-handling conventions. Keep changes focused and avoid unrelated formatting or refactoring. Prefer clear ownership and explicit failure handling over clever abstractions.

- Keep shared vault behavior in the core where practical; keep platform-specific behavior in the appropriate interface or bridge.
- Do not block the UI with expensive file, crypto, decoding, or network work.
- Check cancellation, object lifetime, and thread boundaries when changing asynchronous work.
- Handle malformed files, unsupported formats, and failed operations without silently losing user data.
- Explain new dependencies, including why existing dependencies are insufficient and which platforms are affected.
- Do not commit generated builds, APKs, local vaults, credentials, signing keys, or machine-specific configuration.

### Protect privacy and compatibility

Use the existing cryptographic libraries and established project primitives. Do not add custom cryptography or weaken password derivation, authentication, certificate verification, or key handling to simplify a feature.

Changes to package formats, database layouts, key handling, or password changes must explain compatibility with existing vaults, failure behavior, and any migration or recovery requirements. Add tests for the affected behavior.

Avoid introducing plaintext files, logs, or caches containing private content. If a platform API requires a temporary plaintext file, document its purpose, location, lifetime, cleanup, and security implications. Existing exceptions are not permission to create additional plaintext copies elsewhere.

The desktop note-save path is an existing exception, not an in-memory-only save: `MainWindow::saveOpenText` in `qt-app/src/main_window.cpp` writes the UTF-8 body to `<system temporary directory>/mvp-notes/<sanitized note filename>` so it can call the core's file-import API. The temporary root comes from `std::filesystem::temp_directory_path()` (not a fixed `/tmp` path), and `noteFileName` sanitizes the title. The plaintext file exists during writing and import. The code attempts ordinary deletion after import returns, including an import-error result, and after a short write; it does not check removal errors. A crash or exception before removal, or failed removal, can leave plaintext behind. Deletion is not secure erasure; the filename and body are outside vault encryption while staged. See [SECURITY.md](SECURITY.md) for the exposure and cleanup warning, and the README for the separate Android PDF cache exception.

For LAN sharing changes, preserve certificate verification and the permission boundaries described in the README. LAN remote browsing and copying must not accidentally grant permission to rename, move, or delete another device's files. Cloud access has a different trust boundary: `MegaVaultCloud` unlocks the vault on the server and retains its password in process memory, and authenticated cloud clients can delete shared files and folders. Do not apply LAN read-only assumptions to cloud operations; see [docs/cloud-ssl.md](docs/cloud-ssl.md).

### Accessibility

User-facing changes should support meaningful control labels, visible keyboard focus, predictable navigation, readable scaled text, and understandable errors. Provide alternatives when an action relies on a gesture or precise pointer movement.

Follow [ACCESSIBILITY.md](ACCESSIBILITY.md). Include the checks performed and any known gaps in the pull request; do not claim screen reader support or accessibility conformance based only on a successful build.

## Testing your change

Run the relevant build and automated tests for your platform. The README lists suites covering vault access, imports and exports, password changes, removal, encrypted streaming, media, playback, tags, and records.

For behavior changes and bug fixes, add focused regression coverage where practical. Tests should verify observable outcomes and meaningful failure cases. Documentation-only changes generally need a review of links, commands, and accuracy rather than new code tests.

Use manual checks for behavior that headless tests cannot cover, such as playback controls, dialogs, focus, text scaling, Android navigation, and sharing between devices. Test cancellation and failure paths when they are affected.

If you change the shared core, consider Windows, Linux, and Android compatibility. You do not need access to every platform to submit a contribution: state what you tested and what remains untested. Review the available CI results and investigate failures related to your change. The documented release workflow runs Windows and Linux test suites; the Android workflow builds the APK. These checks do not replace manual interface testing.

For performance changes, provide reproducible measurements with the workload, hardware, build configuration, and before/after results. Speed improvements must preserve correctness and privacy.

## Submitting a pull request

Keep one main purpose per pull request and use a title that describes the resulting change. Reference the relevant issue when one exists.

Your description should include:

- **Problem:** the concrete bug or user need.
- **Change:** what now behaves differently and why.
- **Validation:** commands, manual checks, platforms, and results.
- **Risks:** compatibility changes, security implications, and known limitations, when relevant.

For UI changes, screenshots or short recordings with sample data can help reviewers. Explain accessibility checks alongside visual changes. Update documentation when behavior, setup, dependencies, or supported environments change.

Before requesting review:

- Check the diff for accidental files, secrets, and unrelated changes.
- Run the relevant checks and report failures or checks you could not run.
- Ensure tests and documentation match the implementation.
- Explain any remaining concerns rather than hiding them.

Draft pull requests are welcome for work in progress. Maintainers may request revisions or additional testing before merging. Review timing depends on availability and change complexity; there is no guaranteed turnaround.

## AI-assisted contributions

AI-assisted work is welcome, but the contributor remains responsible for its correctness, provenance, and maintainability. Read and understand every submitted change, verify APIs and dependencies, and run appropriate checks. Do not submit unreviewed generated code or invented test results.

Do not send private vault contents, passwords, keys, or confidential information to an AI service. Mention AI assistance when it helps reviewers understand how the change was produced or validated.

## Licensing

The repository includes the GNU Affero General Public License, version 3, in [LICENSE](LICENSE). Submit contributions under the project's applicable license terms. Only contribute material you have the right to share, preserve relevant copyright notices, and identify third-party code or assets and their licenses.

## Other ways to help

Clear bug reports, reproduction cases, documentation corrections, translations, accessibility testing, and thoughtful reviews all help the project. If you are unsure where to start, describe your interests and available platform in an issue and ask for a suitable task.
