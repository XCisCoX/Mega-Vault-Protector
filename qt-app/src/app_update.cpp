#include "videovault/app/app_update.hpp"

#include "videovault/core/version.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

// The phone reads the same releases feed (android/.../AppUpdate.kt). This build
// talks to it directly instead of through Qt's network stack: the shipped
// Windows package has no OpenSSL DLL for Qt 5, so Windows uses WinHTTP — the
// same reason the LAN sharing code carries its own SChannel channel.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#define MVP_UPDATE_WINHTTP 1
#elif defined(__linux__) && !defined(__ANDROID__)
#include <QSslSocket>
#define MVP_UPDATE_SOCKET 1
#endif

namespace videovault::app {
namespace {

// Where the releases feed lives. A renamed repository answers with a redirect,
// which the fetch below follows.
const char* const kLatestReleaseUrl =
    "https://api.github.com/repos/XCisCoX/Mega-Vault-Protector/releases/latest";

constexpr int kRedirectLimit = 5;
constexpr int kConnectTimeoutMs = 8000;
constexpr int kReadTimeoutMs = 30000;
constexpr int kUnpackTimeoutMs = 120000;
constexpr qint64 kApiLimit = 1024 * 1024;    // the release JSON is a few KB
constexpr int kTransferBufferBytes = 64 * 1024;

struct HttpReply {
    int status{0};
    QByteArray body;        // set when no file sink was given
    QUrl location;          // redirect target, when the status is a redirect
    QString error;
};

[[nodiscard]] bool isRedirect(const int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// --- the network ------------------------------------------------------------

#if defined(MVP_UPDATE_WINHTTP)

struct HttpHandleCloser {
    void operator()(void* handle) const {
        if (handle != nullptr) {
            WinHttpCloseHandle(static_cast<HINTERNET>(handle));
        }
    }
};

using HttpHandle = std::unique_ptr<void, HttpHandleCloser>;

[[nodiscard]] QString winHttpError(const wchar_t* step) {
    return QStringLiteral("%1 failed (Windows error %2).")
        .arg(QString::fromWCharArray(step))
        .arg(GetLastError());
}

bool httpsFetch(const QUrl& url, const std::function<bool(qint64, qint64)>& progress,
    QFile* sink, HttpReply* reply) {
    if (!url.isValid() || url.host().isEmpty()
        || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        reply->error = QStringLiteral("GitHub sent an address this build cannot open.");
        return false;
    }
    QString target = url.path(QUrl::FullyEncoded);
    if (target.isEmpty()) {
        target = QStringLiteral("/");
    }
    if (!url.query().isEmpty()) {
        target += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    }

    HttpHandle session(WinHttpOpen(L"MegaVaultProtect", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        reply->error = winHttpError(L"WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session.get(), kConnectTimeoutMs, kConnectTimeoutMs, kReadTimeoutMs, kReadTimeoutMs);
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2)
    // Pin the floor: GitHub refuses anything below TLS 1.2, and asking for it
    // keeps an old machine from negotiating something weaker.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
#endif

    const std::wstring host = url.host().toStdWString();
    HttpHandle connection(WinHttpConnect(session.get(), host.c_str(),
        static_cast<INTERNET_PORT>(url.port(443)), 0));
    if (!connection) {
        reply->error = winHttpError(L"WinHttpConnect");
        return false;
    }

    // WinHTTP verifies the server certificate against the Windows store and
    // checks the host name; the ignore flags that would relax that are never
    // set here.
    const std::wstring wideTarget = target.toStdWString();
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", wideTarget.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        reply->error = winHttpError(L"WinHttpOpenRequest");
        return false;
    }
    WinHttpAddRequestHeaders(request.get(), L"User-Agent: MegaVaultProtect",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(request.get(), L"Accept: */*",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.get(), nullptr)) {
        reply->error = winHttpError(L"WinHttpSendRequest");
        return false;
    }

    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX);
    reply->status = static_cast<int>(status);

    if (isRedirect(reply->status)) {
        // The size is asked for first: WinHttpQueryHeaders reports the needed
        // length through ERROR_INSUFFICIENT_BUFFER, and asking twice keeps this
        // out of guessing how long a host name may be.
        DWORD needed = 0;
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr, &needed, WINHTTP_NO_HEADER_INDEX);
        if (needed > sizeof(wchar_t)) {
            std::vector<wchar_t> location(needed / sizeof(wchar_t) + 1, L'\0');
            DWORD bytes = needed;
            if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                    location.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
                reply->location = QUrl(QString::fromWCharArray(location.data()));
            }
        }
        return true;
    }

    DWORD length = 0;
    DWORD lengthBytes = sizeof(length);
    qint64 total = 0;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthBytes, WINHTTP_NO_HEADER_INDEX)) {
        total = static_cast<qint64>(length);
    }

    qint64 received = 0;
    QByteArray chunk;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0) {
            break;
        }
        chunk.resize(static_cast<int>(std::min<DWORD>(available, kTransferBufferBytes)));
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read)
            || read == 0) {
            break;
        }
        const QByteArray bytes(chunk.constData(), static_cast<int>(read));
        if (sink != nullptr) {
            if (sink->write(bytes) != bytes.size()) {
                reply->error = QStringLiteral("The update file could not be written to disk.");
                return false;
            }
        } else if (reply->body.size() + bytes.size() > kApiLimit) {
            reply->error = QStringLiteral("GitHub sent a reply too large for this build to read.");
            return false;
        } else {
            reply->body += bytes;
        }
        received += bytes.size();
        if (progress != nullptr && !progress(received, total)) {
            return false;
        }
    }
    return true;
}

#elif defined(MVP_UPDATE_SOCKET)

// Buffered reads over the TLS socket, so the body can be followed with the
// Content-Length, with chunked framing, or until the peer closes.
class TlsStream {
public:
    bool connectTo(const QString& host, const quint16 port, QString* error) {
        if (!QSslSocket::supportsSsl()) {
            *error = QStringLiteral("This system has no SSL library for Qt to use.");
            return false;
        }
        socket_.setPeerVerifyMode(QSslSocket::VerifyPeer);
        socket_.connectToHostEncrypted(host, port);
        if (!socket_.waitForEncrypted(kConnectTimeoutMs)) {
            *error = socket_.errorString().isEmpty()
                ? QStringLiteral("Could not reach GitHub.")
                : QStringLiteral("Could not reach GitHub: %1").arg(socket_.errorString());
            return false;
        }
        return true;
    }

    bool write(const QByteArray& bytes) {
        qint64 sent = 0;
        while (sent < bytes.size()) {
            const qint64 wrote = socket_.write(bytes.constData() + sent, bytes.size() - sent);
            if (wrote <= 0) {
                return false;
            }
            sent += wrote;
            if (!socket_.waitForBytesWritten(kReadTimeoutMs)) {
                return false;
            }
        }
        return true;
    }

    // False once the peer closed or the read timed out with nothing to hand
    // back; buffered bytes stay readable through `drain`.
    bool pull() {
        if (socket_.bytesAvailable() == 0 && !socket_.waitForReadyRead(kReadTimeoutMs)) {
            return false;
        }
        const QByteArray chunk = socket_.readAll();
        if (chunk.isEmpty()) {
            return false;
        }
        buffer_ += chunk;
        return true;
    }

    // Hands back up to `max` bytes, at least one unless the peer is done.
    bool drain(const int max, QByteArray* out) {
        if (buffer_.isEmpty() && !pull()) {
            return false;
        }
        const int count = std::min<int>(max, buffer_.size());
        out->append(buffer_.left(count));
        buffer_.remove(0, count);
        return true;
    }

    // One CRLF-terminated line, without its terminator.
    bool line(QByteArray* out) {
        int end = buffer_.indexOf("\r\n");
        while (end < 0) {
            if (!pull()) {
                return false;
            }
            end = buffer_.indexOf("\r\n");
        }
        *out = buffer_.left(end);
        buffer_.remove(0, end + 2);
        return true;
    }

    void close() { socket_.disconnectFromHost(); }

private:
    QSslSocket socket_;
    QByteArray buffer_;
};

bool readBody(TlsStream* stream, const bool chunked, const qint64 length,
    const std::function<bool(qint64, qint64)>& progress, QFile* sink, QByteArray* body) {
    qint64 done = 0;
    const auto deliver = [&](const QByteArray& bytes) {
        if (sink != nullptr) {
            sink->write(bytes);
        } else {
            *body += bytes;
        }
        done += bytes.size();
        return progress == nullptr || progress(done, chunked ? 0 : length);
    };
    if (chunked) {
        while (true) {
            QByteArray sizeLine;
            if (!stream->line(&sizeLine)) {
                return false;
            }
            const int extension = sizeLine.indexOf(';');
            if (extension >= 0) {
                sizeLine.truncate(extension);
            }
            bool ok = false;
            const qint64 size = sizeLine.trimmed().toLongLong(&ok, 16);
            if (!ok) {
                return false;
            }
            if (size == 0) {
                QByteArray trailer;
                while (stream->line(&trailer) && !trailer.isEmpty()) {
                    // Discard the trailer headers that close the body.
                }
                return true;
            }
            QByteArray data;
            if (!stream->drain(static_cast<int>(size), &data)) {
                return false;
            }
            if (!deliver(data)) {
                return false;
            }
            QByteArray terminator;
            if (!stream->line(&terminator)) {
                return false;
            }
        }
    }
    if (length >= 0) {
        qint64 remaining = length;
        QByteArray chunk;
        while (remaining > 0) {
            chunk.clear();
            if (!stream->drain(static_cast<int>(std::min<qint64>(remaining, kTransferBufferBytes)), &chunk)) {
                return false;
            }
            remaining -= chunk.size();
            if (!deliver(chunk)) {
                return false;
            }
        }
        return true;
    }
    // No length and no framing: the peer closes when the body ends.
    QByteArray chunk;
    while (true) {
        chunk.clear();
        if (!stream->drain(kTransferBufferBytes, &chunk)) {
            return true;
        }
        if (!deliver(chunk)) {
            return false;
        }
    }
}

bool httpsFetch(const QUrl& url, const std::function<bool(qint64, qint64)>& progress,
    QFile* sink, HttpReply* reply) {
    if (!url.isValid() || url.host().isEmpty()
        || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        reply->error = QStringLiteral("GitHub sent an address this build cannot open.");
        return false;
    }
    TlsStream stream;
    if (!stream.connectTo(url.host(), static_cast<quint16>(url.port(443)), &reply->error)) {
        return false;
    }
    QString target = url.path(QUrl::FullyEncoded);
    if (target.isEmpty()) {
        target = QStringLiteral("/");
    }
    if (!url.query().isEmpty()) {
        target += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    }
    QByteArray request;
    request += "GET " + target.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: " + url.host().toUtf8() + "\r\n";
    request += "User-Agent: MegaVaultProtect\r\n";
    request += "Accept: */*\r\n";
    request += "Connection: close\r\n\r\n";
    if (!stream.write(request)) {
        reply->error = QStringLiteral("Could not send the request to GitHub.");
        return false;
    }

    QByteArray statusLine;
    if (!stream.line(&statusLine)) {
        reply->error = QStringLiteral("GitHub closed the connection without answering.");
        return false;
    }
    const QList<QByteArray> parts = statusLine.split(' ');
    if (parts.size() < 2) {
        reply->error = QStringLiteral("GitHub sent an answer this build cannot read.");
        return false;
    }
    reply->status = parts.at(1).toInt();

    bool chunked = false;
    qint64 length = -1;
    QByteArray line;
    while (stream.line(&line) && !line.isEmpty()) {
        const int colon = line.indexOf(':');
        if (colon < 0) {
            continue;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            length = value.toLongLong();
        } else if (name == "transfer-encoding" && value.toLower().contains("chunked")) {
            chunked = true;
        } else if (name == "location") {
            reply->location = QUrl(QString::fromUtf8(value));
        }
    }
    if (isRedirect(reply->status)) {
        stream.close();
        return true;
    }
    if (!readBody(&stream, chunked, length, progress, sink, &reply->body)) {
        if (reply->error.isEmpty()) {
            reply->error = QStringLiteral("The connection to GitHub ended before the last byte.");
        }
        return false;
    }
    stream.close();
    return true;
}

#else

bool httpsFetch(const QUrl&, const std::function<bool(qint64, qint64)>&, QFile*, HttpReply* reply) {
    reply->error = QStringLiteral("This build cannot install updates.");
    return false;
}

#endif

// Follows GitHub's redirect from api.github.com to the asset host, starting the
// file over each time so a partial body from the first host is not kept.
bool httpGet(const QUrl& url, const std::function<bool(qint64, qint64)>& progress,
    QFile* sink, HttpReply* reply) {
    QUrl target = url;
    for (int hop = 0; hop <= kRedirectLimit; ++hop) {
        HttpReply current;
        if (!httpsFetch(target, progress, sink, &current)) {
            *reply = current;
            return false;
        }
        if (isRedirect(current.status) && !current.location.isEmpty()) {
            const QUrl next = current.location.isRelative()
                ? target.resolved(current.location)
                : current.location;
            if (!next.isValid() || next.host().isEmpty()) {
                reply->status = current.status;
                reply->error = QStringLiteral("GitHub sent an address this build cannot open.");
                return false;
            }
            if (sink != nullptr) {
                sink->seek(0);
                sink->resize(0);
            }
            target = next;
            continue;
        }
        *reply = current;
        return true;
    }
    reply->error = QStringLiteral("GitHub redirected the request too many times.");
    return false;
}

// --- version handling -------------------------------------------------------

// The parts the phone compares: the three numbers before a dash, plus the beta
// counter. A build without a beta suffix sorts above one with it.
[[nodiscard]] std::array<int, 4> versionParts(const QString& text) {
    QString head = text;
    const int dash = head.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        head.truncate(dash);
    }
    const QStringList numbers = head.split(QLatin1Char('.'));
    const QRegularExpressionMatch beta = QRegularExpression(QStringLiteral("beta\\.(\\d+)")).match(text);
    return { numbers.value(0).toInt(), numbers.value(1).toInt(), numbers.value(2).toInt(),
        beta.hasMatch() ? beta.captured(1).toInt() : 99 };
}

[[nodiscard]] QString withoutLeadingV(const QString& tag) {
    return tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
}

// The suffix the release assets carry on this platform, used when the exact
// name is not in the release. Empty when the platform has no package.
[[nodiscard]] QString assetSuffix() {
#if defined(MVP_UPDATE_WINHTTP)
    return QStringLiteral(".zip");
#elif defined(MVP_UPDATE_SOCKET)
    return QStringLiteral(".tar.gz");
#else
    return {};
#endif
}

[[nodiscard]] bool isReleaseAsset(const QString& name) {
    const QString wanted = appUpdateAssetName();
    return !wanted.isEmpty() && name == wanted;
}

// --- unpacking --------------------------------------------------------------

[[nodiscard]] QString sanitized(const QString& version) {
    QString clean;
    for (const QChar character : version) {
        if (character.isLetterOrNumber() || character == QLatin1Char('.')
            || character == QLatin1Char('-') || character == QLatin1Char('_')) {
            clean += character;
        }
    }
    return clean.isEmpty() ? QStringLiteral("unknown") : clean;
}

// The folder inside the unpacked archive that holds the executable. The Windows
// zip carries the files at its root, the Linux tarball keeps them inside
// MegaVaultProtect/.
[[nodiscard]] QString payloadRoot(const QString& staging, const QString& executable) {
    if (QFileInfo::exists(staging + QLatin1Char('/') + executable)) {
        return staging;
    }
    QDir level(staging);
    const QFileInfoList first = level.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& candidate : first) {
        if (QFileInfo::exists(candidate.absoluteFilePath() + QLatin1Char('/') + executable)) {
            return candidate.absoluteFilePath();
        }
        QDir deeper(candidate.absoluteFilePath());
        const QFileInfoList second = deeper.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& inner : second) {
            if (QFileInfo::exists(inner.absoluteFilePath() + QLatin1Char('/') + executable)) {
                return inner.absoluteFilePath();
            }
        }
    }
    return {};
}

bool unpack(const QString& package, const QString& staging, QString* error) {
#if defined(_WIN32)
    // tar.exe ships with Windows 10 1803 and later and reads zip archives; the
    // system copy is used so a tar from PATH (git-bash, for instance) that
    // expects different arguments cannot break the unpack.
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    const QString tar = systemRoot.isEmpty()
        ? QStringLiteral("tar.exe")
        : systemRoot + QStringLiteral("/System32/tar.exe");
    const QStringList arguments{ QStringLiteral("-xf"), package, QStringLiteral("-C"), staging };
#else
    const QString tar = QFileInfo::exists(QStringLiteral("/usr/bin/tar"))
        ? QStringLiteral("/usr/bin/tar")
        : QStringLiteral("tar");
    const QStringList arguments{ QStringLiteral("-xzf"), package, QStringLiteral("-C"), staging };
#endif
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(tar, arguments);
    if (!process.waitForStarted(kConnectTimeoutMs)) {
        *error = QStringLiteral("The unpacking tool could not be started.");
        return false;
    }
    if (!process.waitForFinished(kUnpackTimeoutMs) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(process.readAll()).trimmed();
        *error = detail.isEmpty()
            ? QStringLiteral("The downloaded update could not be unpacked.")
            : QStringLiteral("The downloaded update could not be unpacked: %1").arg(detail);
        return false;
    }
    return true;
}

// --- replacing the installation ---------------------------------------------

[[nodiscard]] QString escapedForShell(const QString& text) {
    QString escaped = text;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return escaped;
}

[[nodiscard]] QString escapedForPowerShell(const QString& text) {
    QString escaped = text;
    escaped.replace(QLatin1String("'"), QLatin1String("''"));
    return escaped;
}

bool writeTextFile(const QString& path, const QString& text, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        *error = QStringLiteral("The update folder is not writable: %1").arg(path);
        return false;
    }
    file.write(text.toUtf8());
    file.close();
    return true;
}

#if defined(_WIN32)

// The running executable and its DLLs stay locked until this process is gone,
// which is what the helper waits for.
[[nodiscard]] QString helperScript(const QString& folder, const QString& payload,
    const QString& install, const QString& executable) {
    const QString log = escapedForPowerShell(folder + QStringLiteral("/apply-update.log"));
    return QStringLiteral(
        "# Replaces MegaVaultProtect with the unpacked release, then starts it again.\n"
        "$ErrorActionPreference = 'Stop'\n"
        "$log = '%1'\n"
        "try {\n"
        "    while (Get-Process -Id %2 -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 400 }\n"
        "    Start-Sleep -Milliseconds 500\n"
        "    Copy-Item -Path '%3\\*' -Destination '%4' -Recurse -Force\n"
        "    'Installed over %4' | Out-File -FilePath $log -Encoding utf8\n"
        "} catch {\n"
        "    $_ | Out-File -FilePath $log -Encoding utf8\n"
        "} finally {\n"
        "    Start-Process -FilePath '%4\\%5'\n"
        "}\n")
        .arg(log, QString::number(QCoreApplication::applicationPid()),
            escapedForPowerShell(payload), escapedForPowerShell(install),
            escapedForPowerShell(executable));
}

#else

[[nodiscard]] QString helperScript(const QString& folder, const QString& payload,
    const QString& install, const QString& executable) {
    return QStringLiteral(
        "#!/bin/sh\n"
        "# Replaces MegaVaultProtect with the unpacked release, then starts it again.\n"
        "folder='%1'\n"
        "for _ in $(seq 1 600); do\n"
        "    kill -0 %2 2>/dev/null || break\n"
        "    sleep 1\n"
        "done\n"
        "cp -a '%3/.' '%4/' >\"$folder/apply-update.log\" 2>&1\n"
        "chmod +x '%4/%5' 2>/dev/null\n"
        "if [ -x '%4/MegaVaultProtect.sh' ]; then\n"
        "    nohup '%4/MegaVaultProtect.sh' >/dev/null 2>&1 &\n"
        "else\n"
        "    nohup '%4/%5' >/dev/null 2>&1 &\n"
        "fi\n"
        "exit 0\n")
        .arg(escapedForShell(folder), QString::number(QCoreApplication::applicationPid()),
            escapedForShell(payload), escapedForShell(install),
            escapedForShell(executable));
}

#endif

} // namespace

QString installedVersion() {
    const QString reported = QCoreApplication::applicationVersion();
    if (!reported.isEmpty()) {
        return reported;
    }
    const std::string_view version = videovault::core::version();
    return QString::fromLatin1(version.data(), static_cast<int>(version.size()));
}

bool isNewerVersion(const QString& latest, const QString& current) {
    const std::array<int, 4> left = versionParts(latest);
    const std::array<int, 4> right = versionParts(current);
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return left[index] > right[index];
        }
    }
    return false;
}

QString appUpdateAssetName() {
#if defined(MVP_UPDATE_WINHTTP)
    return QStringLiteral("MegaVaultProtect-windows-x64.zip");
#elif defined(MVP_UPDATE_SOCKET)
    return QStringLiteral("MegaVaultProtect-linux-x64.tar.gz");
#else
    return {};
#endif
}

bool appUpdateSupported() {
#if defined(MVP_UPDATE_WINHTTP) || defined(MVP_UPDATE_SOCKET)
    return true;
#else
    return false;
#endif
}

QString updateFolder(const QString& version) {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return base + QStringLiteral("/update/") + sanitized(version);
}

bool fetchLatestRelease(const QString& current, AppRelease* release, QString* error) {
    release->version.clear();
    release->url = QUrl();
    release->size = 0;
    if (!appUpdateSupported()) {
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    HttpReply reply;
    if (!httpGet(QUrl(QString::fromLatin1(kLatestReleaseUrl)), nullptr, nullptr, &reply)) {
        if (error != nullptr) {
            *error = reply.error.isEmpty()
                ? QStringLiteral("Could not reach GitHub.")
                : reply.error;
        }
        return false;
    }
    if (reply.status != 200) {
        if (error != nullptr) {
            *error = QStringLiteral("GitHub answered %1 for the release list.").arg(reply.status);
        }
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(reply.body, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("GitHub sent a release list this build cannot read.");
        }
        return false;
    }
    const QJsonObject root = document.object();
    const QString tag = withoutLeadingV(root.value(QStringLiteral("tag_name")).toString());
    if (tag.isEmpty() || !isNewerVersion(tag, current)) {
        return false;   // this copy already is the latest release
    }

    // The exact asset name wins; otherwise the first file of the right kind is
    // taken, the same fallback the phone uses.
    const QString wanted = appUpdateAssetName();
    const QString suffix = assetSuffix();
    QUrl picked;
    qint64 size = 0;
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const bool exact = isReleaseAsset(name);
        if (!exact && !(picked.isEmpty() && name.endsWith(suffix))) {
            continue;
        }
        picked = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
        size = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
        if (exact) {
            break;
        }
    }
    if (picked.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Release %1 has no package for this system yet.").arg(tag);
        }
        return false;
    }
    release->version = tag;
    release->url = picked;
    release->size = size;
    return true;
}

bool downloadRelease(const AppRelease& release, const QString& destination,
    const std::function<bool(qint64, qint64)>& progress, QString* error) {
    if (release.url.isEmpty()) {
        *error = QStringLiteral("There is nothing to download.");
        return false;
    }
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("The update folder is not writable: %1").arg(destination);
        return false;
    }
    HttpReply reply;
    if (!httpGet(release.url, progress, &file, &reply)) {
        *error = reply.error.isEmpty()
            ? QStringLiteral("The update could not be downloaded.")
            : reply.error;
        return false;
    }
    if (reply.status != 200) {
        *error = QStringLiteral("GitHub answered %1 instead of the update file.").arg(reply.status);
        return false;
    }
    const qint64 written = file.size();
    file.close();
    if (release.size > 0 && written != release.size) {
        *error = QStringLiteral("The update file is incomplete (%1 of %2 bytes).")
            .arg(written).arg(release.size);
        return false;
    }
    return true;
}

bool stageRelease(const QString& package, QString* payload, QString* error) {
    const QString folder = QFileInfo(package).absolutePath();
    const QString staging = folder + QStringLiteral("/unpacked");
    QDir(staging).removeRecursively();
    if (!QDir().mkpath(staging)) {
        *error = QStringLiteral("The update folder is not writable: %1").arg(staging);
        return false;
    }
    if (!unpack(package, staging, error)) {
        return false;
    }
    const QString executable = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const QString root = payloadRoot(staging, executable);
    if (root.isEmpty()) {
        *error = QStringLiteral("The downloaded file is not a MegaVaultProtect package.");
        return false;
    }
    *payload = root;
    return true;
}

bool applyRelease(const AppRelease& release, const QString& payload, QString* error) {
    if (payload.isEmpty() || !QDir(payload).exists()) {
        *error = QStringLiteral("The unpacked update is missing.");
        return false;
    }
    const QString install = QCoreApplication::applicationDirPath();
    const QString executable = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    if (!QFileInfo::exists(payload + QLatin1Char('/') + executable)) {
        *error = QStringLiteral("The unpacked update has no %1 in it.").arg(executable);
        return false;
    }
    const QString folder = updateFolder(release.version);
    if (!QDir().mkpath(folder)) {
        *error = QStringLiteral("The update folder is not writable: %1").arg(folder);
        return false;
    }
    const QString text = helperScript(folder, payload, install, executable);
#if defined(_WIN32)
    const QString script = folder + QStringLiteral("/apply-update.ps1");
    if (!writeTextFile(script, text, error)) {
        return false;
    }
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    const QString shell = systemRoot.isEmpty()
        ? QStringLiteral("powershell.exe")
        : systemRoot + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
    const QString program = QFileInfo::exists(shell) ? shell : QStringLiteral("powershell.exe");
    // Detached: the helper outlives this process and swaps the files once the
    // executable below is no longer locked. Hidden keeps its console off screen.
    if (!QProcess::startDetached(program, { QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"),
            QStringLiteral("Hidden"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-File"), script })) {
        *error = QStringLiteral("The update helper could not be started.");
        return false;
    }
#else
    const QString script = folder + QStringLiteral("/apply-update.sh");
    if (!writeTextFile(script, text, error)) {
        return false;
    }
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), { script })) {
        *error = QStringLiteral("The update helper could not be started.");
        return false;
    }
#endif
    return true;
}

} // namespace videovault::app
