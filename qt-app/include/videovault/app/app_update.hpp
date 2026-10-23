#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>

namespace videovault::app {

// A newer GitHub release than the copy that is installed. The phone carries the
// same struct for the same API (android/.../AppUpdate.kt).
struct AppRelease {
    QString version;   // tag without its leading v, e.g. "2.0.10"
    QUrl url;          // asset to download
    qint64 size{0};    // asset size in bytes; 0 when GitHub did not report one
};

// Version of the running build ("2.0.9"), the number CMake stamped into it.
[[nodiscard]] QString installedVersion();

// Same ordering as the phone: three dot-separated numbers, and a build without
// a beta suffix sorts above the same numbers with one.
[[nodiscard]] bool isNewerVersion(const QString& latest, const QString& current);

// The file this platform installs from the release, empty when there is none.
[[nodiscard]] QString appUpdateAssetName();

// True when applyRelease can replace this installation in place.
[[nodiscard]] bool appUpdateSupported();

// Pure parser for either a /releases/latest object or a /releases array.
// Selects the highest eligible version with the exact platform asset name.
// Drafts are ignored; prerelease flags and beta suffixes require includeBeta.
// False + empty error means no newer eligible release. Malformed JSON/API
// errors or newer releases lacking a trusted package return a nonempty error.
// Every call clears release first; error may be null.
bool selectReleaseFromJson(const QByteArray& body, const QString& current,
    bool includeBeta, AppRelease* release, QString* error);

// Everything below blocks, so call it from a worker thread.

// Fetches a newer release, with the same output/error contract as the parser.
// By default only /releases/latest is queried; includeBeta queries the first
// 100 /releases entries and considers stable and beta packages together.
bool fetchLatestRelease(const QString& current, AppRelease* release, QString* error,
    bool includeBeta = false);

// Stores the asset at `destination`. `progress` receives (received, total),
// where total is 0 when the server does not report a length; returning false
// from it cancels the transfer.
bool downloadRelease(const AppRelease& release, const QString& destination,
    const std::function<bool(qint64, qint64)>& progress, QString* error);

// Unpacks `package` and points `payload` at the folder that holds the new
// executable, ready for applyRelease.
bool stageRelease(const QString& package, QString* payload, QString* error);

// Starts the helper that copies `payload` over the installed files once this
// process has exited, then launches the app again. The caller quits afterwards.
bool applyRelease(const AppRelease& release, const QString& payload, QString* error);

// Folder that holds the package and the helper of one version.
[[nodiscard]] QString updateFolder(const QString& version);

} // namespace videovault::app
