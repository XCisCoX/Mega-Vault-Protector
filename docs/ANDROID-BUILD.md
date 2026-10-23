# Building the Android app

The Android app is a **native Kotlin/Jetpack Compose application** that reuses
the Qt-independent C++ core ([`core/`](../core/)) through a JNI bridge
([`jni_bridge.cpp`](../android/app/src/main/cpp/jni_bridge.cpp), built as
`libmvpcore.so`). It shares vault concepts and dark styling with the desktop
application, not 1:1 screen or feature parity: the desktop record editor has no
Android screen.

Android includes vault browsing, video and music playback, image/PDF/text
readers, PC sharing and transfers, a cloud client, and an in-app updater.
The default vault location is the private sandbox (`filesDir`);
`VaultLocation.kt` also offers app-specific external storage directories.
Local video and music playback stream decrypted ranges through JNI into
Media3 rather than writing a complete plaintext playback file. This is not a
blanket no-plaintext-on-disk guarantee: PDF viewing, import staging, cloud
copies, and explicit export/restore paths need separate treatment (see below).

## Artifacts

| Artifact | Path |
|---|---|
| Local debug APK (debug-signed, arm64-v8a) | `android/app/build/outputs/apk/debug/app-debug.apk` |
| Local release APK (with signing variables configured) | `android/app/build/outputs/apk/release/app-release.apk` |
| Local release APK (without a keystore; unsigned) | `android/app/build/outputs/apk/release/app-release-unsigned.apk` |
| Cross-built dependency archives | `out/android-arm64/prefix/lib/*.a` |
| Native core/test build tree | `out/android-arm64/build/` |
| Native dependency step log | `out/android-arm64/step.log` |
| Actions artifact container | `MegaVaultProtect-android-arm64` (contains `app-release.apk` or fallback `app-debug.apk`) |
| Published release/beta asset | `MegaVaultProtect-android-arm64.apk` (release-signed) |

APK and native-library sizes depend on the build; no size measurements are
provided here. The Actions summary reports the produced APK's byte size.

## Prerequisites

* JDK **21**, matching CI (the Java/Kotlin bytecode target is 17).
* Android SDK command-line tools and real SDK packages: platform **36**,
  build-tools **35.0.0**, NDK **30.0.14904198**, CMake **3.22.1**, and
  platform-tools (`adb`). These match the app's Gradle configuration and CI;
  CMake 3.22.1 is the SDK package installed by CI.
* For the native dependency script: Bash, curl, tar, GNU make, a host C
  compiler, Perl, CMake, and Ninja. `qemu-aarch64` is optional locally; CI
  installs `qemu-user`.
* An arm64-v8a phone running Android 8.0+/API 26+ for device testing.

The checked-in Gradle settings use Aliyun Maven mirrors with official
fallbacks, and the wrapper uses Huawei's Gradle mirror. Source archives come
from GitHub codeload. Downloads and caches still require reachable endpoints;
the repository does not supply a complete offline SDK or dependency bundle.

## Build steps

Run from the repository root with JDK 21 selected through `JAVA_HOME` and
`PATH`. Set `ANDROID_HOME` to your installed SDK; the path below is an example.
The commands use a Bash host shell and `sdkmanager` from the SDK command-line
tools. Review and accept the SDK licenses when prompted.

```bash
export ANDROID_HOME="$HOME/Android/Sdk" # replace with your SDK location
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
sdkmanager --licenses
sdkmanager --install "platform-tools" "platforms;android-36" \
  "build-tools;35.0.0" "ndk;30.0.14904198" "cmake;3.22.1"
export ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/30.0.14904198"
export PATH="$ANDROID_HOME/cmake/3.22.1/bin:$PATH"

# Cross-build dependencies and the native core/test executables.
bash scripts/android/build-android-core.sh

# Use an absolute prefix: Gradle's fallback is /repo/out/android-arm64/prefix.
export MVP_ANDROID_PREFIX="$PWD/out/android-arm64/prefix"
cd android
./gradlew --no-daemon assembleDebug
cd ..
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

The tracked native script builds OpenSSL 3.0.16, Argon2 20190702, libsodium
1.0.18-RELEASE, SQLCipher v4.5.6, zlib 1.3.1, and FFmpeg n7.1.1. It reuses
existing installed dependency archives and recreates the core build tree.
When `qemu-aarch64` is available it attempts its listed test executables;
missing Android `linker64` produces explicit skips, other failures abort.
Without QEMU the binaries are built but not executed. This is not device testing.

For a signed release, configure the signing variables described below, then
run `./gradlew --no-daemon assembleRelease` from `android/` with the same SDK
and prefix environment. Without `MVP_KEYSTORE`, this task produces an unsigned
release APK, not a publishable update. Use `gradlew.bat` in a native Windows
shell. `android/local.properties` is machine-specific and gitignored; if used,
set `sdk.dir` yourself. No tracked app-build helper writes it for you.

Docker is optional for the native dependency step. The script documents this
alternative (run from the repository root):

```bash
docker run --rm --user root -e HOST_UID="$(id -u)" \
  -v "$PWD":/repo -w /repo \
  saschpe/android-ndk:36.1-jdk25.0.3_9-ndk30.0.14904198-cmake3.31.6 \
  bash scripts/android/build-android-core.sh
```

This builds native dependencies; it is not an APK helper. Build the app with
the host JDK/SDK and Gradle wrapper above. Do not substitute fake SDK tools,
rename SDK platforms, or relabel a different CMake installation.

## Building on a Windows host (no Docker, restricted network)

The native script contains Windows/MSYS support, not a guarantee of a tested
Windows build. It detects `windows-x86_64`, tries `mingw32-make` when `make`
is absent, and creates compiler wrappers that pass an explicit Android target
to clang. Its `win_path`/`posix_path` helpers distinguish native drive-letter
paths from shell/tar paths; OpenSSL receives a POSIX-form NDK path for its
compiler lookup. Supply absolute native paths for `MVP_ANDROID_PREFIX` and SDK
variables when invoking Windows Java/Gradle.

The script adds curl's `--ssl-no-revoke` on Windows for unreachable certificate
revocation servers. That disables revocation checking for those downloads;
it does not solve blocked SDK/Maven access. Install genuine SDK packages and
provide the required host tools rather than copying dummy executables into
the SDK. No network reachability or Windows build success is asserted here.

## CI (GitHub Actions)

The [Android workflow](../.github/workflows/android-build.yml) builds the
arm64-v8a APK on pushes to `main`, PRs targeting `main`, and manual
`workflow_dispatch`. The [release](../.github/workflows/release.yml) and
[beta](../.github/workflows/beta.yml) workflows call it with
`allow_unsigned: false` and publish its release APK under the asset name above.

What it does, in order: JDK 21 and the real Android SDK (platform 36,
build-tools 35.0.0, NDK 30.0.14904198 from the stable channel, CMake 3.22.1) via
`sdkmanager`; `scripts/android/build-android-core.sh` (with the QEMU skips and
failure behavior described above); then the Gradle wrapper with
`MVP_ANDROID_PREFIX` pointing at `out/android-arm64/prefix`. With the project
keystore it runs `assembleRelease`, passing `MVP_KEYSTORE`,
`MVP_KEYSTORE_PASSWORD`, `MVP_KEY_ALIAS`, and `MVP_KEY_PASSWORD`. Without that
secret it falls back to `assembleDebug` only if `allow_unsigned` permits it.

The workflow caches SDK packages, `out/android-arm64` (keyed on the native
script, `core/**`, and root `CMakeLists.txt`), and Gradle caches/wrapper files.
Its concurrency group cancels superseded runs.

Before uploading, the job checks for `lib/arm64-v8a/libmvpcore.so` and
`classes.dex`, then checks the workflow's explicit list of JNI exports with
`llvm-nm`. This is a packaging/symbol check, not exhaustive validation of every
Kotlin declaration or runtime behavior. For signed builds it then runs
`apksigner verify --print-certs` and fails unless the APK carries the release
certificate from the section below: a missing or rotated secret must not ship an
APK that no phone will install over the previous one.

The artifact is `MegaVaultProtect-android-arm64` (30-day retention). To build
without pushing: Actions → Android Build → Run workflow.

## Signing (Android releases)

Published releases use a stable signing certificate so they can replace
compatible installed copies. A local debug build is not a release update;
an installed copy signed with another key cannot be replaced by this key.
Version-code ordering also matters for updates.

Before the signing change on October 14, 2026, CI releases used changing
debug keys. Moving one of those installs to the stable release key needs a
one-time uninstall/reinstall; preserve and verify a separate copy of needed
vault data first, because uninstalling can remove the app's local vault.
Installs already signed with the stable project key should not be uninstalled
for routine updates.

The tracked signing configuration is:

* A PKCS#12 keystore supplied through `MVP_KEYSTORE` (use an absolute path),
  with `MVP_KEYSTORE_PASSWORD`, `MVP_KEY_ALIAS`, and `MVP_KEY_PASSWORD`.
  Keep keystores and passwords outside the repository; Gradle's comments
  describe a sibling `android-signing/` directory. Its actual contents and
  certificate validity are not established by the tracked source.
* Certificate SHA-256 (what CI pins in `android-build.yml` as
  `EXPECTED_SIGNER`):
  `ba94bf1cde8978456e21b5b35cc86382695816faea1a4ccbc9f7ce629da56bfe`
* Repository secrets: `ANDROID_KEYSTORE_BASE64` (the keystore, base64),
  `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_PASSWORD`, `ANDROID_KEY_ALIAS`.
* The workflow decodes the keystore into `$RUNNER_TEMP`, builds
  `assembleRelease`, and verifies the signature as described above. A fork pull
  request has no secrets and falls back to a debug build, which is why the
  release and beta workflows pass `allow_unsigned: false` — those two must fail
  rather than publish something uninstallable.

Do not replace the project key casually, commit it, or paste its credentials.
If an existing install has a different signer, first back up any vault data
before considering an uninstall/reinstall: uninstalling can remove app-private
data. Signing does not by itself guarantee a successful device installation.

## Project layout (Android)

* [`android/`](../android/): Gradle wrapper/settings and root plugin versions
  (AGP 8.13.0, Kotlin/Compose plugin 2.1.20).
* [`android/app/build.gradle.kts`](../android/app/build.gradle.kts): SDK/ABI,
  signing, Compose and Media3 dependencies, JNI CMake configuration.
* [`android/app/src/main/java/org/megavideoprotect/app/`](../android/app/src/main/java/org/megavideoprotect/app/):
  * App lifecycle and vault UI: `MainActivity.kt`, setup/login/vault screens,
    `SettingsDialog.kt`, `VaultLocation.kt`, `Models.kt`, `FileKinds.kt`.
  * Shared styling/components: `Theme.kt`, `Components.kt` (`MvpCard`,
    `MvpButton`, `MvpInput`, `MvpError`, title/description/combo helpers).
  * Native interface: `CoreBridge.kt` declares the JNI surface; results use
    JSON, byte arrays, and primitive values, without a fixed method count here.
  * Media/readers: `PlayerScreen.kt`, `VaultDataSource.kt`, `MusicScreen.kt`,
    `MusicPlaybackService.kt`, `ImageViewerScreen.kt`, `PdfScreen.kt`,
    `TextScreen.kt`.
  * Device import, packages and peer transfers: `DeviceLibrary.kt`,
    `FolderPicker.kt`, `Packages.kt`, `PeerLink.kt`, `PeerScreen.kt`,
    `QrScanScreen.kt`, `TransferHub.kt`.
  * Remote/cloud access: `RemoteVault.kt`, `RemoteHttpDataSource.kt`,
    `CloudFiles.kt`; server setup is documented in [cloud-ssl.md](cloud-ssl.md).
  * In-app updates: `AppUpdate.kt` checks GitHub releases, downloads an APK,
    and hands it to Android's package installer for system/user approval.
* [`android/app/src/main/cpp/`](../android/app/src/main/cpp/): `jni_bridge.cpp`
  wraps the vault and FFmpeg helpers; `CMakeLists.txt` compiles core sources
  and links the static dependency prefix into `libmvpcore.so`.
* [`android/app/src/main/AndroidManifest.xml`](../android/app/src/main/AndroidManifest.xml)
  and [`res/`](../android/app/src/main/res/): permissions, service/receiver
  declarations, theme, strings, and icons.

## Notes / limitations

* **Playback:** video uses Media3/ExoPlayer; native FFmpeg helpers also provide
  metadata, thumbnails and frame decoding. Music playback exists through
  `MusicPlayer` and the foreground `MusicPlaybackService`, with media-session
  and notification controls; closing the music page does not release the player.
* **Text:** `TextScreen.kt` reads UTF-8 through native range reads into memory
  with a 2 MiB cap. It is a reader, not the desktop record editor.
* **PDF plaintext cache:** `PdfScreen.kt` reads a local or remote PDF (up to
  80 MiB) into memory and writes `cacheDir/mvp-pdf-<id>.pdf` for Android's
  `PdfRenderer`, which needs a file descriptor. On normal screen disposal it
  closes the renderer/descriptor and attempts to delete the file. Cleanup is
  not secure erasure and is not guaranteed after a crash/process kill or a
  failure before the opened file is registered for disposal. Password-protected
  PDFs are rejected with a message directing the user to Windows.
* **Plaintext import staging:** `VaultScreen.kt`'s `importBatch` copies each
  picked URI into `cacheDir/import_<index>/<original-name>` before calling
  `nativeImportInto(path, folderId)`. Keeping the original filename preserves
  the vault display name. Its `finally` block attempts to delete both file and
  staging directory on success or failure. This requires temporary disk space
  for the plaintext source; it is not direct URI-to-encrypted-vault streaming,
  and interrupted-process cleanup is not guaranteed. Package import separately
  stages `cacheDir/import.mvpk` and attempts deletion in `finally`.
* **Cloud copies:** `CloudFiles.kt` downloads remote content through
  `RemoteVault.readRange` into `cacheDir/cloud/<id>.part`, then renames completed
  copies to `<id>`. These are plaintext local cache copies, not encrypted vault
  packages; the downloader does not implement per-view deletion. Do not apply
  the local streaming-playback guarantee to cloud synchronization.
* **Export/restore:** explicit exports can write plaintext to user-chosen
  destinations, with intermediate cache files on some paths. Treat these
  copies as outside the encrypted vault's protection.

These descriptions are based on tracked source and workflow configuration,
not a newly built APK or an on-device verification. Build output sizes and
runtime behavior must be checked on a configured Android build/device host.
