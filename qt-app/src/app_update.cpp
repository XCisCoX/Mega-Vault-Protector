#include "videovault/app/proxy_settings.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
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
const char* const kReleaseListUrl =
    "https://api.github.com/repos/XCisCoX/Mega-Vault-Protector/releases?per_page=100";

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
    // Follow redirects ourselves so every hop is checked against the GitHub
    // allowlist rather than WinHTTP silently visiting an arbitrary host.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
            &redirectPolicy, sizeof(redirectPolicy))) {
        reply->error = winHttpError(L"WinHttpSetOption");
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

// A configured proxy must never fall back to WinHTTP/direct (WinHTTP has no
// SOCKS5 transport). Qt keeps TLS peer/hostname verification enabled by default.
bool proxyHttpsFetch(const QUrl& url, const std::function<bool(qint64, qint64)>& progress,
                     QFile* sink, HttpReply* result) {
    QNetworkAccessManager manager; configureNetworkProxy(&manager);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("User-Agent", "MegaVaultProtect");
    auto* response = manager.get(request); response->setReadBufferSize(kTransferBufferBytes);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    qint64 received = 0; bool failed = false;
    auto drain = [&] {
        const auto bytes = response->readAll(); received += bytes.size();
        if (isRedirect(response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())) return;
        if ((sink && sink->write(bytes) != bytes.size()) || (!sink && received > kApiLimit)) {
            result->error = QStringLiteral("Update reply exceeded its limit or could not be written."); failed = true; response->abort(); return;
        }
        if (!sink) result->body += bytes;
        if (progress && !progress(received, response->header(QNetworkRequest::ContentLengthHeader).toLongLong())) {
            result->error = QStringLiteral("Update download cancelled."); failed = true; response->abort();
        }
    };
    QObject::connect(response, &QIODevice::readyRead, &loop, drain);
    QObject::connect(response, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { failed = true; result->error = QStringLiteral("Proxy update request timed out."); response->abort(); });
    timer.start(kUnpackTimeoutMs); loop.exec(); timer.stop(); drain();
    result->status = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result->location = response->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (response->error() != QNetworkReply::NoError && !failed) { result->error = response->errorString(); failed = true; }
    delete response; return !failed;
}

[[nodiscard]] bool trustedAssetUrl(const QUrl& url, bool redirect = false);

// Follows GitHub's redirects to the asset host, starting the
// file over each time so a partial body from the first host is not kept.
bool httpGet(const QUrl& url, const std::function<bool(qint64, qint64)>& progress,
    QFile* sink, HttpReply* reply) {
    QUrl target = url;
    for (int hop = 0; hop <= kRedirectLimit; ++hop) {
        HttpReply current;
        if (!(proxyEnabled() ? proxyHttpsFetch(target, progress, sink, &current) : httpsFetch(target, progress, sink, &current))) {
            *reply = current;
            return false;
        }
        if (isRedirect(current.status) && !current.location.isEmpty()) {
            const QUrl next = current.location.isRelative()
                ? target.resolved(current.location)
                : current.location;
            const bool trusted = sink != nullptr ? trustedAssetUrl(next, true)
                : next.isValid() && next.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
                    && next.host().compare(QLatin1String("api.github.com"), Qt::CaseInsensitive) == 0
                    && next.userInfo().isEmpty() && !next.hasFragment()
                    && (next.port(-1) == -1 || next.port() == 443)
                    && next.path().startsWith(QLatin1String("/repos/"));
            if (!trusted) {
                reply->status = current.status;
                reply->error = QStringLiteral("GitHub redirected the request to an untrusted address.");
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

struct ReleaseVersion {
    std::array<quint64, 3> numbers{};
    quint64 betaNumber{0};
    bool beta{false};
    bool valid{false};
};

// Repository tags are vMAJOR.MINOR.PATCH[-beta.N]. Do not treat malformed
// numbers as zero, or use a finite beta sentinel that could outrank stable.
[[nodiscard]] ReleaseVersion versionParts(const QString& text) {
    // \z, not $: PCRE's $ also matches before a trailing newline, which would
    // let "2.0.4\n" masquerade as a release tag.
    static const QRegularExpression pattern(
        QStringLiteral("^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)(?:-beta\\.([0-9]+))?\\z"));
    const auto match = pattern.match(text);
    ReleaseVersion result;
    if (!match.hasMatch()) {
        return result;
    }
    for (int index = 0; index < 3; ++index) {
        bool ok = false;
        result.numbers[index] = match.captured(index + 1).toULongLong(&ok);
        if (!ok) {
            return result;
        }
    }
    result.beta = !match.captured(4).isEmpty();
    if (result.beta) {
        bool ok = false;
        result.betaNumber = match.captured(4).toULongLong(&ok);
        if (!ok) {
            return result;
        }
    }
    result.valid = true;
    return result;
}

[[nodiscard]] QString withoutLeadingV(const QString& tag) {
    return tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
}

// Only the repository's release links may start a download. GitHub's signed
// CDN URLs are allowed on redirects, never arbitrary HTTPS destinations.
[[nodiscard]] bool trustedAssetUrl(const QUrl& url, const bool redirect) {
    if (!url.isValid() || url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0
        || !url.userInfo().isEmpty() || url.hasFragment() || (url.port(-1) != -1 && url.port() != 443)) {
        return false;
    }
    const QString host = url.host().toLower();
    if (host == QLatin1String("github.com")) {
        const QString prefix = QStringLiteral("/XCisCoX/Mega-Vault-Protector/releases/download/");
        const QString path = url.path();
        if (!path.startsWith(prefix, Qt::CaseInsensitive)) {
            return false;
        }
        const auto parts = path.mid(prefix.size()).split(QLatin1Char('/'));
        if (parts.size() != 2) {
            return false;
        }
        for (const QString& part : parts) {
            if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")
                || part.contains(QLatin1Char('\\')) || part.contains(QChar::Null)) {
                return false;
            }
        }
        return true;
    }
    return redirect && !url.path().isEmpty()
        && (host == QLatin1String("release-assets.githubusercontent.com")
            || host == QLatin1String("objects.githubusercontent.com")
            || host == QLatin1String("github-releases.githubusercontent.com"));
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
    const ReleaseVersion left = versionParts(latest);
    const ReleaseVersion right = versionParts(current);
    if (!left.valid || !right.valid) {
        return false;
    }
    if (left.numbers != right.numbers) {
        return left.numbers > right.numbers;
    }
    if (left.beta != right.beta) {
        return !left.beta;
    }
    return left.beta && left.betaNumber > right.betaNumber;
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

bool fetchLatestRelease(const QString& current, AppRelease* release, QString* error,
    const bool includeBeta) {
    if (error != nullptr) {
        error->clear();
    }
    if (release == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("No release output was provided.");
        }
        return false;
    }
    *release = {};
    if (!appUpdateSupported()) {
        if (error != nullptr) {
            *error = QStringLiteral("This build cannot install updates.");
        }
        return false;
    }
    HttpReply reply;
    const char* endpoint = includeBeta ? kReleaseListUrl : kLatestReleaseUrl;
    if (!httpGet(QUrl(QString::fromLatin1(endpoint)), nullptr, nullptr, &reply)) {
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
    return selectReleaseFromJson(reply.body, current, includeBeta, release, error);
}

bool selectReleaseFromJson(const QByteArray& body, const QString& current,
    const bool includeBeta, AppRelease* release, QString* error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto fail = [&](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (release == nullptr) {
        return fail(QStringLiteral("No release output was provided."));
    }
    *release = {};
    if (!versionParts(current).valid) {
        return fail(QStringLiteral("The installed version is not a recognized release version."));
    }
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || (!document.isObject() && !document.isArray())) {
        if (error != nullptr) {
            *error = QStringLiteral("GitHub sent a release list this build cannot read.");
        }
        return false;
    }
    const QJsonArray entries = document.isArray() ? document.array() : QJsonArray{document.object()};
    AppRelease best;
    QString unavailable;
    for (const QJsonValue& entry : entries) {
        if (!entry.isObject()) {
            return fail(QStringLiteral("GitHub sent an invalid release entry."));
        }
        const QJsonObject root = entry.toObject();
        if (root.contains(QStringLiteral("message"))) {
            return fail(QStringLiteral("GitHub release API: %1")
                .arg(root.value(QStringLiteral("message")).toString()));
        }
        const auto draft = root.value(QStringLiteral("draft"));
        const auto prerelease = root.value(QStringLiteral("prerelease"));
        if ((!draft.isUndefined() && !draft.isBool())
            || (!prerelease.isUndefined() && !prerelease.isBool())) {
            return fail(QStringLiteral("GitHub sent invalid release flags."));
        }
        if (draft.toBool()) {
            continue;
        }
        if (!root.value(QStringLiteral("tag_name")).isString()
            || root.value(QStringLiteral("tag_name")).toString().isEmpty()
            || !root.value(QStringLiteral("assets")).isArray()) {
            return fail(QStringLiteral("GitHub sent an invalid release entry."));
        }
        const QString tag = withoutLeadingV(root.value(QStringLiteral("tag_name")).toString());
        const ReleaseVersion version = versionParts(tag);
        if (root.value(QStringLiteral("draft")).toBool()
            || (!includeBeta && (root.value(QStringLiteral("prerelease")).toBool() || version.beta))
            || !version.valid || !isNewerVersion(tag, current)) {
            continue;
        }
        AppRelease candidate;
        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue& value : assets) {
            const QJsonObject asset = value.toObject();
            if (!isReleaseAsset(asset.value(QStringLiteral("name")).toString())) {
                continue;
            }
            candidate.url = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
            const double size = asset.value(QStringLiteral("size")).toDouble();
            // JSON numbers are doubles. Guard the conversion (including the
            // rounded qint64 maximum) instead of overflowing on malformed data.
            candidate.size = size >= 0 && size < 9223372036854775808.0
                ? static_cast<qint64>(size) : 0;
            if (trustedAssetUrl(candidate.url)) {
                candidate.version = tag;
                break;
            }
        }
        if (candidate.version.isEmpty()) {
            if (unavailable.isEmpty() || isNewerVersion(tag, unavailable)) {
                unavailable = tag;
            }
        } else if (best.version.isEmpty() || isNewerVersion(candidate.version, best.version)) {
            best = candidate;
        }
    }
    if (best.version.isEmpty()) {
        if (error != nullptr) {
            *error = unavailable.isEmpty() ? QString()
                : QStringLiteral("Release %1 has no trusted package for this system yet.").arg(unavailable);
        }
        return false;
    }
    *release = best;
    return true;
}

bool downloadRelease(const AppRelease& release, const QString& destination,
    const std::function<bool(qint64, qint64)>& progress, QString* error) {
    if (!trustedAssetUrl(release.url)) {
        if (error != nullptr) {
            *error = QStringLiteral("The update has no trusted GitHub release download address.");
        }
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
