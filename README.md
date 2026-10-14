<div align="center">

<img src="docs/MVP-logo.png" alt="Mega Vault Protect" width="360" />

# Mega Vault Protect

<p align="center">
  <img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=600&size=20&pause=1000&color=5B7CFA&center=true&vCenter=true&width=620&lines=A+private+library+for+your+files.;Encrypted+on+this+device.;Video%2C+pictures%2C+music%2C+and+PDFs." alt="Typing SVG" />
</p>

[![C++20](https://img.shields.io/badge/Core-C%2B%2B%2020-536fe8?style=for-the-badge&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![Qt 5](https://img.shields.io/badge/UI-Qt%205-41cd52?style=for-the-badge&logo=qt&logoColor=white)](https://www.qt.io/)
[![Kotlin](https://img.shields.io/badge/Android-Kotlin%20%2F%20Compose-7F52FF?style=for-the-badge&logo=kotlin&logoColor=white)](https://kotlinlang.org/)
[![SQLCipher](https://img.shields.io/badge/Database-SQLCipher-65a30d?style=for-the-badge&logo=sqlite&logoColor=white)](https://www.zetetic.net/sqlcipher/)
[![Argon2id](https://img.shields.io/badge/KDF-Argon2id-8b5cf6?style=for-the-badge)](https://en.wikipedia.org/wiki/Argon2)
[![libsodium](https://img.shields.io/badge/Crypto-libsodium-4f8ef7?style=for-the-badge)](https://doc.libsodium.org/)
[![FFmpeg](https://img.shields.io/badge/Media-FFmpeg-d63031?style=for-the-badge&logo=ffmpeg&logoColor=white)](https://ffmpeg.org/)
[![CMake](https://img.shields.io/badge/Build-CMake-064f8c?style=for-the-badge&logo=cmake&logoColor=white)](https://cmake.org/)

</div>

---
<div align="center">

[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20Android-informational?style=flat-square)]() &nbsp;
[![Status](https://img.shields.io/badge/Status-Active-brightgreen?style=flat-square)]() &nbsp;
[![PRs](https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square)]()

**[What it is](#what-it-is)** · **[Library](#library)** · **[Phone and PC](#phone-and-pc)** · **[Security model](#security-model)** · **[Downloads](#downloads)** · **[Build & run](#build--run)**

</div>

Mega Vault Protect is a private library for the files on your computer or phone. The vault is locked with a password you choose. Every imported file is stored as an authenticated encrypted package on that device. The same C++ core runs the Windows and Linux app and the Android app.

The filename is not the type. The app reads the file header. It uses the extension only when the header is not a known type. A RAR named `.mp4` stays a RAR.

## What it is

Import accepts any file, one at a time or a whole folder. Dropping files on the Windows library does the same.

| What you imported | What the app does |
|---|---|
| Video | Plays it from the encrypted package. Playback reads only the chunks it needs. |
| Picture | Opens it in the viewer. Drag moves the picture. The wheel or a pinch zooms toward the pointer. |
| Music | Opens the music player. On Android it keeps playing from the notification. |
| PDF | Opens the page viewer. Pinch or the wheel zooms. Double-tap switches between fit and a closer view. |
| Text | `.txt`, `.json`, notes, and other plain-text files open in a reader in the same window. |
| Anything else | Stays in the library. The tile shows a stable color for that type, a label such as `[ Rar ]`, and the size. Opening it shows **this format currently not supported**. The file is not deleted and is not sent to another app. |

Video, pictures, and music are decrypted in memory for viewing. On Android, the PDF viewer writes the open document into the app cache because the system PDF renderer needs a file. That cache file is the open document, not the vault copy.

## Library

Folders are names inside the vault. The encrypted packages stay where the vault put them. Removing a folder deletes the files and folders inside it, after a confirmation when anything is there.

**New record** creates a text item in the current folder: a title plus any number of name/value pairs — an API key, a password, an e-mail address, a note. Values can be marked secret; a secret stays masked until you reveal or copy it, and a copied secret is cleared from the clipboard after 30 seconds. The app works out how to present each value (a URL, an e-mail address, a password, a number, or a longer text) from the value itself, so there is no type to pick. A record is encrypted and stored exactly like an imported file, so it sits in folders, carries tags, and is found by search. Like every other item, it is recognised by its content, not by its name.

The library has three views: large icons, a list, and a details table. Search matches the file name or a tag. Tags are edited in Settings. The vault locks after 5 minutes without use. Changing the password re-wraps keys and re-encrypts thumbnails. It does not re-encrypt the packages. Removing an item deletes its package.

On Android, Back walks up one folder. At the library root, a second press within two seconds leaves the app. The video player starts fullscreen, fades the title, seeks ±5 seconds on a double tap, and plays at 2× while a side is held. The timeline shows the buffered region.

## Phone and PC

Both devices have to be on the same network.

**Open the PC vault on the phone.** Share on the PC shows a QR code. The code contains the address and the fingerprint of that session’s TLS certificate. The phone checks the certificate before it sends the vault password. Locking the PC window does not stop sharing. Stopping share does.

**Swap libraries.** From the phone, Swap with PC asks the PC to accept. After that, each side can browse the other’s library and copy items into its own. Neither side can rename, move, or delete the other’s files.

Copies wait in a queue and run one at a time, because the vault writes one package at a time. You can leave the other library, open your own, and add more copies while a transfer is running. **Transfers** lists what is copying and what is waiting. Cancel removes a waiting item immediately, and stops the item that is currently copying.

### Optional self-hosted sync

The self-hosted sync design keeps a local vault on each device and transfers
end-to-end encrypted vault packages through an optional relay. Pairing uses a
separate device credential; the server must never receive your vault password
or the shared package recovery secret. Selected files remain usable locally
when the relay is offline. Interrupted transfers resume, and conflicting edits
are preserved rather than silently overwritten.

See [setup, pairing, security and backups](docs/self-hosted-sync.md), the
[architecture decision](docs/sync-architecture.md), and the
[protocol contract](docs/sync-protocol.md). This replaces the old trusted-server
`MegaVaultCloud` workflow, not PC/LAN sharing. Development-build verification
and the documented MVP limits still matter; server tests alone do not prove
native client UI behavior.

## Screenshots

The pictures below are the library and the video player. Pictures, music, PDFs, and phone sharing are in the app and are not in these shots.

### Windows

| | |
|---|---|
| **Icons** — thumbnail grid, search, and sort. | **List** — name, size, and cover. |
| ![Windows icons](docs/screenshots/windows-icons.png) | ![Windows list](docs/screenshots/windows-list.png) |
| **Details** — size, duration, resolution, codec, tags, imported time. | **Player** — playback of an imported video, with previous/next, seek, volume, and fullscreen. |
| ![Windows details](docs/screenshots/windows-details.png) | ![Windows player](docs/screenshots/windows-player.png) |

### Android

<p align="center">
  <img src="docs/screenshots/android-icons.png" alt="Android icons" width="240" />
  <img src="docs/screenshots/android-list.png" alt="Android list" width="240" />
  <img src="docs/screenshots/android-player.png" alt="Android player" width="240" />
</p>

## Downloads

Each [GitHub Release](https://github.com/XCisCoX/Mega-Vault-Protector/releases) has three files:

| File | How to run it |
|---|---|
| `MegaVaultProtect-windows-x64.zip` | Unzip and run `MegaVaultProtect.exe`. |
| `MegaVaultProtect-linux-x64.tar.gz` | Extract and run `./MegaVaultProtect.sh`. Needs glibc 2.39 or newer (Ubuntu 24.04). |
| `MegaVaultProtect-android-arm64.apk` | Install on an arm64 phone running Android 8 or newer. |

Create another release from Actions → **Release** → Run workflow, or push a tag such as `v0.4.0`. The Android package is debug-signed so it can be installed directly. If a later release is signed with a different debug key, uninstall the old one before installing the new APK.

## Security model

- The master key comes from the password with **Argon2id** (8 MiB–1 GiB memory, 1–20 iterations, 1–16 lanes).
- Domain-separated subkeys (libsodium `crypto_kdf`) cover the database, the password verifier, file-key wrapping, and thumbnails.
- The vault database is **SQLCipher** (4096-byte pages, HMAC-SHA512, `secure_delete ON`).
- Each imported file is a `.vvp` package: a random 256-bit file key, wrapped under the vault key, split into chunks, and authenticated per chunk. A seek verifies the chunks it reads.
- Creating a vault is crash-consistent: the database is written to a temporary file, flushed, renamed into place, and only then is the ready marker written.
- Key material is held in libsodium guarded memory and wiped on release. Wiping is best effort.

## Platforms

- **Linux x64** — Ubuntu 24.04 (GCC 13, Qt 5.15, distro FFmpeg 6.1 / SQLCipher / Argon2 / libsodium).
- **Windows x64** — MSVC 2022, Qt 5.12.12, pinned vcpkg dependencies.
- **Android (arm64-v8a)** — Kotlin and Jetpack Compose, with the same core through JNI. CI builds it in a pinned NDK container.

Platform-specific code is the file I/O (`_WIN32` vs POSIX), dependency discovery (vcpkg vs pkg-config), the JNI bridge, and the phone and desktop UI. The Release workflow builds all three and runs the test suites on Windows and Linux before it publishes.

## Build & run

### Linux (Ubuntu 24.04)

```text
sudo apt install qtbase5-dev qtmultimedia5-dev libqt5multimedia5-plugins \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libsqlcipher-dev libargon2-dev libsodium-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good

cmake --preset linux-debug            # or linux-release
cmake --build --preset linux-debug -j "$(nproc)"
ctest --preset linux-debug --output-on-failure   # all 7 suites, headless

out/build/linux-debug/qt-app/MegaVaultProtect    # run
```

Audio output needs Qt 5 Multimedia's GStreamer backend (`libqt5multimedia5-plugins` + base/good plugins).

### Windows

From a Visual Studio developer shell with Qt 5.12.12 (msvc2017_64) and the vcpkg manifest installed:

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-debug --target VideoVaultCoreTests VideoVaultApp
ctest --preset vs2022-x64-debug
```

The vcpkg FFmpeg port installs stub import libraries. Re-run `scripts/regenerate-ffmpeg-importlibs.sh` after a FFmpeg reinstall. FFmpeg 7.x headers have no `extern "C"` guards, so the sources wrap every include.

### Android

The phone app is Kotlin and Jetpack Compose. The core is compiled for arm64-v8a and exposed through `android/app/src/main/cpp/jni_bridge.cpp`. The vault is in the app sandbox. Import uses the system picker (`*/*`, plus gallery and music shortcuts). Video and music play through the streaming reader.

```text
# Cross-compile core + dependencies, then build the APK (in a pinned NDK container):
docker run --rm --user root -e HOST_UID="$(id -u)" -v "$PWD":/repo \
  saschpe/android-ndk:36.1-jdk25.0.3_9-ndk30.0.14904198-cmake3.31.6 \
  bash scripts/android/build-android-core.sh
docker run --rm --user root -e HOST_UID="$(id -u)" -v "$PWD":/repo \
  saschpe/android-ndk:36.1-jdk25.0.3_9-ndk30.0.14904198-cmake3.31.6 \
  bash out/android-arm64/build-android-app.sh

adb install -r out/android-arm64/MegaVaultProtect-native-debug.apk
```

Full procedure: [docs/ANDROID-BUILD.md](docs/ANDROID-BUILD.md). CI builds the same APK on every push (`.github/workflows/android-build.yml`).

### Tests

`ctest` runs nine headless suites. Windows and Linux release builds run the same list before a release is published.

| Suite | What it checks |
|---|---|
| `VideoVaultCore.Smoke` | Create and open a vault. A wrong password is rejected. |
| `VideoVaultCore.Gallery` | Import, list, and export round-trip. |
| `VideoVaultCore.Admin` | Password change re-keys the vault. Removal deletes the package. |
| `VideoVaultCore.Reader` | Streaming reads, seek, and tamper detection on a `.vvp` package. |
| `VideoVaultCore.Media` | Probe, thumbnail after import, and thumbnail survival across a password change. |
| `VideoVault.Playback` | Headless decode through the encrypted reader: duration, frames, seek, and audio. No temp file. |
| `VideoVaultCore.Tags` | Add, filter, rename, and remove tags. |
| `VideoVaultCore.Records` | Create a record from memory, read it back, replace its content in place (same id, folders and tags kept, old package gone), and reject bad names or unknown targets. |
| `VideoVaultRecords.Format` | The record JSON container: marker at byte 0, escaping round-trip, non-record content rejected, and the inferred value kinds. |

```text
ctest --preset linux-debug --output-on-failure
ctest --preset vs2022-x64-debug
```

## Project layout

```text
core/        C++20 library: crypto, SQLCipher, .vvp packages, streaming
             reader, FFmpeg probing, thumbnails, tags, folders, text records
qt-app/      Qt 5 app: library, video, pictures, PDF, music, records, share, transfers
android/     Kotlin + Jetpack Compose app over the same core
tests/       Headless ctest suites
scripts/     Android cross-build and Windows FFmpeg import libraries
third_party/ vcpkg overlay ports (Windows)
docs/        ANDROID-BUILD.md, screenshots, logo
.github/     Release workflow (Windows zip, Linux tarball, Android APK)
```

## Tech stack

C++20 · Qt 5 (Widgets, Multimedia) · Kotlin + Jetpack Compose · JNI · Argon2id · libsodium (XChaCha20-Poly1305, crypto_kdf) · SQLCipher · FFmpeg · CMake · Gradle (Android) · vcpkg (Windows) / pkg-config (Linux)
