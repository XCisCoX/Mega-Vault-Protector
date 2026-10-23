#include "videovault/app/share_tls.hpp"
#include "videovault/core/vault.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRunnable>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kSessionMs = 60000;
constexpr int kChunk = 256 * 1024;

struct Request {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QByteArray authorization;
    QByteArray range;
    QByteArray syncSpeed;
    QByteArray user;
    QByteArray model;
    QByteArray kind;
    QByteArray body;
};

struct Session {
    QByteArray token;
    QString model;
    QString kind;
    qint64 seen{0};
};

struct State {
    QMutex sessionsMu;
    QMutex vaultMu;
    // Null until a client signs in with the vault password. The cloud never
    // keeps that password anywhere but here, and only while it runs.
    std::unique_ptr<videovault::core::Vault> vault;
    std::string password;
    std::filesystem::path root;
    std::vector<Session> sessions;
    std::shared_ptr<videovault::app::ShareTlsIdentity> tls;
    bool plain{false};
};

struct Pipe {
    videovault::app::TlsChannel* tls{nullptr};
    QTcpSocket* tcp{nullptr};

    qint64 write(const char* data, qint64 size) {
        if (tls != nullptr) return tls->write(data, size);
        return tcp->write(data, size);
    }
    bool waitForBytesWritten(int timeoutMs) {
        if (tls != nullptr) return tls->waitForBytesWritten(timeoutMs);
        return tcp->waitForBytesWritten(timeoutMs);
    }
    bool waitForReadyRead(int timeoutMs) {
        if (tls != nullptr) return tls->waitForReadyRead(timeoutMs);
        return tcp->waitForReadyRead(timeoutMs);
    }
    QByteArray readAll() {
        if (tls != nullptr) return tls->readAll();
        return tcp->readAll();
    }
};

QString jsonEscape(const QString& text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '\\':
        case '"':
            out += QLatin1Char('\\');
            out += ch;
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        default:
            if (ch.unicode() < 0x20) {
                out += QStringLiteral("\\u%1").arg(ch.unicode(), 4, 16, QLatin1Char('0'));
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

QString jsonEscape(const std::string& text) {
    return jsonEscape(QString::fromUtf8(text.data(), static_cast<int>(text.size())));
}

bool sameSecret(const QByteArray& got, const QByteArray& expected) {
    if (expected.isEmpty() || got.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < got.size(); ++i) {
        diff |= static_cast<unsigned char>(got.at(i) ^ expected.at(i));
    }
    return diff == 0;
}

bool samePassword(const QByteArray& got, const std::string& expected) {
    if (expected.empty() || static_cast<int>(expected.size()) != got.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < got.size(); ++i) {
        diff |= static_cast<unsigned char>(got.at(i) ^ static_cast<unsigned char>(expected[static_cast<std::size_t>(i)]));
    }
    return diff == 0;
}

QByteArray randomHex() {
    QByteArray raw(32, '\0');
    for (int i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);
    }
    return raw.toHex();
}

QString cleanLabel(const QByteArray& raw, const QString& fallback) {
    QString text = QString::fromUtf8(raw).trimmed();
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('"'), QLatin1Char('\''));
    if (text.size() > 64) text.truncate(64);
    return text.isEmpty() ? fallback : text;
}

void wipe(QByteArray* bytes) {
    if (bytes == nullptr || bytes->isEmpty()) return;
    bytes->fill('\0');
    bytes->clear();
}

bool writeAll(Pipe* pipe, const char* data, int size) {
    int off = 0;
    while (off < size) {
        const qint64 wrote = pipe->write(data + off, size - off);
        if (wrote <= 0) return false;
        off += static_cast<int>(wrote);
        // On a TLS channel write() has already pushed these bytes to the socket,
        // so an unsuccessful wait here means "nothing left to wait for", not
        // "the bytes were lost". Treating it as a failure cut every reply off
        // after its headers: the client saw a truncated response.
        pipe->waitForBytesWritten(20000);
    }
    return true;
}

bool sendReply(Pipe* pipe, int code, const char* reason, const QByteArray& type, const QByteArray& body) {
    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
    head += "Content-Type: " + type + "\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    if (!writeAll(pipe, head.constData(), head.size())) return false;
    if (body.isEmpty()) return true;
    return writeAll(pipe, body.constData(), body.size());
}

double bytesPerSecond(const QByteArray& speed) {
    const QByteArray name = speed.trimmed().toLower();
    if (name == "fast") return 0;
    if (name == "slow") return 256.0 * 1024.0;
    return 2.0 * 1024.0 * 1024.0;
}

bool writePaced(Pipe* pipe, const char* data, int size, const QByteArray& speed) {
    const double rate = bytesPerSecond(speed);
    int off = 0;
    while (off < size) {
        const int n = std::min(32 * 1024, size - off);
        if (!writeAll(pipe, data + off, n)) return false;
        off += n;
        if (rate > 0) {
            const int ms = static_cast<int>((static_cast<double>(n) / rate) * 1000.0);
            if (ms > 0) QThread::msleep(static_cast<unsigned long>(ms));
        }
    }
    return true;
}

bool readRequest(Pipe* pipe, Request* request) {
    QByteArray raw;
    while (!raw.contains("\r\n\r\n")) {
        if (!pipe->waitForReadyRead(8000)) return false;
        raw += pipe->readAll();
        if (raw.size() > 65536) return false;
    }
    const int end = raw.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = raw.left(end).split('\n');
    if (lines.isEmpty()) return false;
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2) return false;
    request->method = parts.at(0);
    const QByteArray target = parts.at(1);
    const int queryAt = target.indexOf('?');
    if (queryAt >= 0) {
        request->path = QString::fromUtf8(target.left(queryAt));
        request->query = QUrlQuery(QString::fromUtf8(target.mid(queryAt + 1)));
    } else {
        request->path = QString::fromUtf8(target);
    }
    int contentLength = 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const QByteArray lower = line.toLower();
        const auto value = [&](const char* name) {
            return line.mid(static_cast<int>(std::strlen(name))).trimmed();
        };
        if (lower.startsWith("authorization:")) request->authorization = value("authorization:");
        else if (lower.startsWith("range:")) request->range = value("range:");
        else if (lower.startsWith("x-sync-speed:")) request->syncSpeed = value("x-sync-speed:");
        else if (lower.startsWith("x-user:")) request->user = value("x-user:");
        else if (lower.startsWith("x-model:")) request->model = value("x-model:");
        else if (lower.startsWith("x-kind:")) request->kind = value("x-kind:");
        else if (lower.startsWith("content-length:")) {
            bool ok = false;
            contentLength = line.mid(int(sizeof("content-length:") - 1)).trimmed().toInt(&ok);
            if (!ok || contentLength < 0 || contentLength > 4096) return false;
        }
    }
    QByteArray body = raw.mid(end + 4);
    if (contentLength > 0) {
        while (body.size() < contentLength) {
            if (!pipe->waitForReadyRead(8000)) return false;
            body += pipe->readAll();
            if (body.size() > contentLength + 4096) return false;
        }
        body.truncate(contentLength);
    } else {
        body.clear();
    }
    request->body = body;
    return true;
}

qlonglong queryId(const Request& request) {
    bool ok = false;
    const qlonglong id = request.query.queryItemValue(QStringLiteral("id")).toLongLong(&ok);
    return ok ? id : -1;
}

void pruneSessions(State* state) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    state->sessions.erase(
        std::remove_if(state->sessions.begin(), state->sessions.end(),
            [now](const Session& session) { return now - session.seen > kSessionMs; }),
        state->sessions.end());
}

bool authorize(State* state, Request* request) {
    const QByteArray prefix = "Bearer ";
    if (!request->authorization.startsWith(prefix)) return false;
    const QByteArray token = request->authorization.mid(prefix.size()).trimmed().toLower();
    QMutexLocker lock(&state->sessionsMu);
    pruneSessions(state);
    for (Session& session : state->sessions) {
        if (!sameSecret(token, session.token)) continue;
        session.seen = QDateTime::currentMSecsSinceEpoch();
        if (!request->model.isEmpty()) session.model = cleanLabel(request->model, session.model);
        if (!request->kind.isEmpty()) session.kind = cleanLabel(request->kind, session.kind);
        return true;
    }
    return false;
}

QByteArray videosJson(videovault::core::Vault& vault) {
    auto list = vault.list_videos();
    if (!list) return {};
    QString out = QStringLiteral("[");
    bool first = true;
    for (const auto& video : list.value()) {
        if (!first) out += QLatin1Char(',');
        first = false;
        QString tags = QStringLiteral("[");
        bool tagFirst = true;
        for (const auto& tag : video.tags) {
            if (!tagFirst) tags += QLatin1Char(',');
            tagFirst = false;
            tags += QLatin1Char('"') + jsonEscape(tag) + QLatin1Char('"');
        }
        tags += QLatin1Char(']');
        out += QStringLiteral("{\"id\":%1,\"name\":\"%2\",\"size\":%3,\"importedAt\":%4,\"folderId\":%5,\"tags\":%6}")
            .arg(video.id)
            .arg(jsonEscape(video.display_name))
            .arg(static_cast<qulonglong>(video.original_size))
            .arg(static_cast<qulonglong>(video.imported_at))
            .arg(video.folder_id)
            .arg(tags);
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

QByteArray foldersJson(videovault::core::Vault& vault) {
    auto list = vault.list_folders();
    if (!list) return {};
    QString out = QStringLiteral("[");
    bool first = true;
    for (const auto& folder : list.value()) {
        if (!first) out += QLatin1Char(',');
        first = false;
        out += QStringLiteral("{\"id\":%1,\"parentId\":%2,\"name\":\"%3\"}")
            .arg(folder.id)
            .arg(folder.parent_id)
            .arg(jsonEscape(folder.name));
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

QByteArray tagsJson(videovault::core::Vault& vault) {
    auto list = vault.list_tags();
    if (!list) return {};
    QString out = QStringLiteral("[");
    bool first = true;
    for (const auto& tag : list.value()) {
        if (!first) out += QLatin1Char(',');
        first = false;
        out += QStringLiteral("{\"id\":%1,\"name\":\"%2\",\"count\":%3}")
            .arg(tag.id)
            .arg(jsonEscape(tag.name))
            .arg(tag.video_count);
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

QByteArray sessionsJson(State* state) {
    QMutexLocker lock(&state->sessionsMu);
    pruneSessions(state);
    QString out = QStringLiteral("[");
    bool first = true;
    for (const Session& session : state->sessions) {
        if (!first) out += QLatin1Char(',');
        first = false;
        out += QStringLiteral("{\"model\":\"%1\",\"kind\":\"%2\"}")
            .arg(jsonEscape(session.model), jsonEscape(session.kind));
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

bool findSize(videovault::core::Vault& vault, qlonglong id, std::uint64_t* size) {
    auto list = vault.list_videos();
    if (!list) return false;
    for (const auto& video : list.value()) {
        if (video.id == id) {
            *size = video.original_size;
            return true;
        }
    }
    return false;
}

void sendFile(Pipe* pipe, State* state, qlonglong id, const Request& request) {
    std::uint64_t total = 0;
    {
        QMutexLocker lock(&state->vaultMu);
        if (!findSize(*state->vault, id, &total)) {
            sendReply(pipe, 404, "Not Found", "text/plain", "not found");
            return;
        }
    }
    std::uint64_t start = 0;
    std::uint64_t end = total == 0 ? 0 : total - 1;
    bool partial = false;
    if (total > 0 && !request.range.isEmpty()) {
        const QByteArray header = request.range.trimmed();
        if (header.startsWith("bytes=")) {
            const QByteArray spec = header.mid(6);
            const int dash = spec.indexOf('-');
            if (dash <= 0) {
                sendReply(pipe, 416, "Range Not Satisfiable", "text/plain", "bad range");
                return;
            }
            bool startOk = false;
            bool endOk = true;
            start = spec.left(dash).toULongLong(&startOk);
            if (dash + 1 < spec.size()) end = spec.mid(dash + 1).toULongLong(&endOk);
            if (!startOk || !endOk || start > end || start >= total) {
                sendReply(pipe, 416, "Range Not Satisfiable", "text/plain", "bad range");
                return;
            }
            if (end >= total) end = total - 1;
            partial = true;
        }
    }
    const std::uint64_t count = total == 0 ? 0 : (end - start + 1);
    QByteArray head = partial
        ? QByteArray("HTTP/1.1 206 Partial Content\r\n")
        : QByteArray("HTTP/1.1 200 OK\r\n");
    head += "Content-Type: application/octet-stream\r\n";
    head += "Accept-Ranges: bytes\r\n";
    head += "Content-Length: " + QByteArray::number(static_cast<qulonglong>(count)) + "\r\n";
    if (partial) {
        head += "Content-Range: bytes " + QByteArray::number(static_cast<qulonglong>(start))
            + "-" + QByteArray::number(static_cast<qulonglong>(end))
            + "/" + QByteArray::number(static_cast<qulonglong>(total)) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";
    if (!writeAll(pipe, head.constData(), head.size())) return;
    std::uint64_t sent = 0;
    while (sent < count) {
        const auto ask = std::min<std::uint64_t>(static_cast<std::uint64_t>(kChunk), count - sent);
        std::vector<unsigned char> data;
        {
            QMutexLocker lock(&state->vaultMu);
            auto bytes = state->vault->read_video_range(id, start + sent, static_cast<std::size_t>(ask));
            if (!bytes || bytes.value().empty()) return;
            data = std::move(bytes.value());
        }
        const int n = static_cast<int>(std::min<std::size_t>(data.size(), static_cast<std::size_t>(count - sent)));
        if (n <= 0) return;
        if (!writePaced(pipe, reinterpret_cast<const char*>(data.data()), n, request.syncSpeed)) return;
        sent += static_cast<std::uint64_t>(n);
    }
}

// The cloud keeps no password of its own. The first client to sign in opens
// the vault with the password it types; later clients are checked against that
// same password, which lives only in this process's memory while it runs.
void handleLogin(Pipe* pipe, State* state, Request* request) {
    const QByteArray given = request->body;
    bool openedNow = false;
    bool ok = false;
    {
        QMutexLocker lock(&state->vaultMu);
        if (state->vault == nullptr) {
            const std::string password(given.constData(), static_cast<std::size_t>(given.size()));
            auto opened = videovault::core::Vault::open(state->root, password);
            if (opened) {
                state->password = password;
                state->vault = std::make_unique<videovault::core::Vault>(std::move(opened.value()));
                openedNow = true;
                ok = true;
            }
        } else {
            ok = samePassword(given, state->password);
        }
    }
    wipe(&request->body);
    if (!ok) {
        std::cout << "Rejected a sign-in: wrong vault password.\n";
        std::cout.flush();
        sendReply(pipe, 401, "Unauthorized", "text/plain", "wrong password");
        return;
    }
    Session session;
    session.token = randomHex();
    session.model = cleanLabel(request->model, cleanLabel(request->kind, QStringLiteral("device")));
    session.kind = cleanLabel(request->kind, QStringLiteral("pc"));
    session.seen = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lock(&state->sessionsMu);
        pruneSessions(state);
        state->sessions.erase(
            std::remove_if(state->sessions.begin(), state->sessions.end(),
                [&](const Session& old) {
                    return old.model == session.model && old.kind == session.kind;
                }),
            state->sessions.end());
        state->sessions.push_back(session);
    }
    std::cout << (openedNow ? "Vault opened by " : "Signed in: ")
              << session.model.toStdString() << " (" << session.kind.toStdString() << ")\n";
    std::cout.flush();
    const QByteArray body = QStringLiteral("{\"token\":\"%1\"}").arg(QString::fromLatin1(session.token)).toUtf8();
    sendReply(pipe, 200, "OK", "application/json", body);
}

void handle(Pipe* pipe, const std::shared_ptr<State>& state) {
    Request request;
    if (!readRequest(pipe, &request)) return;
    if (request.method == "POST" && request.path == QStringLiteral("/v1/login")) {
        handleLogin(pipe, state.get(), &request);
        return;
    }
    if (!authorize(state.get(), &request)) {
        sendReply(pipe, 401, "Unauthorized", "text/plain", "sign in again");
        return;
    }
    {
        QMutexLocker lock(&state->vaultMu);
        if (state->vault == nullptr) {
            sendReply(pipe, 503, "Service Unavailable", "text/plain", "sign in first");
            return;
        }
    }
    if (request.method == "GET" && request.path == QStringLiteral("/v1/sessions")) {
        sendReply(pipe, 200, "OK", "application/json", sessionsJson(state.get()));
        return;
    }
    if (request.method == "GET" && request.path == QStringLiteral("/v1/videos")) {
        QByteArray body;
        {
            QMutexLocker lock(&state->vaultMu);
            body = videosJson(*state->vault);
        }
        sendReply(pipe, 200, "OK", "application/json", body);
        return;
    }
    if (request.method == "GET" && request.path == QStringLiteral("/v1/folders")) {
        QByteArray body;
        {
            QMutexLocker lock(&state->vaultMu);
            body = foldersJson(*state->vault);
        }
        sendReply(pipe, 200, "OK", "application/json", body);
        return;
    }
    if (request.method == "GET" && request.path == QStringLiteral("/v1/tags")) {
        QByteArray body;
        {
            QMutexLocker lock(&state->vaultMu);
            body = tagsJson(*state->vault);
        }
        sendReply(pipe, 200, "OK", "application/json", body);
        return;
    }
    const qlonglong id = queryId(request);
    if (request.method == "POST" && request.path == QStringLiteral("/v1/remove")) {
        if (id < 0) {
            sendReply(pipe, 400, "Bad Request", "text/plain", "missing id");
            return;
        }
        QMutexLocker lock(&state->vaultMu);
        auto removed = state->vault->remove_video(id);
        if (!removed || !removed.value()) {
            sendReply(pipe, 404, "Not Found", "text/plain", "not found");
            return;
        }
        sendReply(pipe, 200, "OK", "text/plain", "removed");
        return;
    }
    if (request.method == "POST" && request.path == QStringLiteral("/v1/remove-folder")) {
        if (id < 0) {
            sendReply(pipe, 400, "Bad Request", "text/plain", "missing id");
            return;
        }
        QMutexLocker lock(&state->vaultMu);
        auto removed = state->vault->remove_folder(id);
        if (!removed || !removed.value()) {
            sendReply(pipe, 404, "Not Found", "text/plain", "not found");
            return;
        }
        sendReply(pipe, 200, "OK", "text/plain", "removed");
        return;
    }
    if (request.method != "GET") {
        sendReply(pipe, 405, "Method Not Allowed", "text/plain", "method");
        return;
    }
    if (id < 0) {
        sendReply(pipe, 400, "Bad Request", "text/plain", "missing id");
        return;
    }
    if (request.path == QStringLiteral("/v1/size")) {
        std::uint64_t bytes = 0;
        QMutexLocker lock(&state->vaultMu);
        if (!findSize(*state->vault, id, &bytes)) {
            sendReply(pipe, 404, "Not Found", "text/plain", "not found");
            return;
        }
        sendReply(pipe, 200, "OK", "text/plain", QByteArray::number(static_cast<qulonglong>(bytes)));
        return;
    }
    if (request.path == QStringLiteral("/v1/media")) {
        QMutexLocker lock(&state->vaultMu);
        auto info = state->vault->media_info(id);
        if (!info) {
            sendReply(pipe, 404, "Not Found", "application/json", "{}");
            return;
        }
        const auto& media = info.value();
        const QByteArray body = QStringLiteral(
            "{\"durationMs\":%1,\"width\":%2,\"height\":%3,\"rotation\":%4,\"codec\":\"%5\",\"title\":\"%6\",\"artist\":\"%7\"}")
            .arg(static_cast<qulonglong>(media.duration_ms))
            .arg(media.width)
            .arg(media.height)
            .arg(media.rotation_degrees)
            .arg(jsonEscape(media.codec_name))
            .arg(jsonEscape(media.title))
            .arg(jsonEscape(media.artist))
            .toUtf8();
        sendReply(pipe, 200, "OK", "application/json", body);
        return;
    }
    if (request.path == QStringLiteral("/v1/thumbnail")) {
        bool ok = false;
        int maxDimension = request.query.queryItemValue(QStringLiteral("max")).toInt(&ok);
        if (!ok) maxDimension = 320;
        maxDimension = qBound(64, maxDimension, 720);
        QMutexLocker lock(&state->vaultMu);
        auto thumb = state->vault->display_thumbnail(id, static_cast<std::uint32_t>(maxDimension));
        const bool placeholder = thumb && (thumb.value().mime == "image/x-mvp-note"
            || thumb.value().mime == "image/x-mvp-empty");
        if (!thumb || placeholder || thumb.value().bytes.empty()) {
            sendReply(pipe, 404, "Not Found", "text/plain", "no thumbnail");
            return;
        }
        const auto& bytes = thumb.value().bytes;
        sendReply(pipe, 200, "OK", "image/jpeg",
            QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())));
        return;
    }
    if (request.path == QStringLiteral("/v1/file")) {
        sendFile(pipe, state.get(), id, request);
        return;
    }
    if (request.path == QStringLiteral("/v1/read")) {
        bool offsetOk = false;
        bool sizeOk = false;
        const qulonglong offset = request.query.queryItemValue(QStringLiteral("offset")).toULongLong(&offsetOk);
        int size = request.query.queryItemValue(QStringLiteral("size")).toInt(&sizeOk);
        if (!offsetOk || !sizeOk || size <= 0 || size > (1 << 20)) {
            sendReply(pipe, 400, "Bad Request", "text/plain", "bad range");
            return;
        }
        std::vector<unsigned char> data;
        {
            QMutexLocker lock(&state->vaultMu);
            auto bytes = state->vault->read_video_range(id, offset, static_cast<std::size_t>(size));
            if (!bytes) {
                sendReply(pipe, 404, "Not Found", "text/plain", "not found");
                return;
            }
            data = std::move(bytes.value());
        }
        QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
            + QByteArray::number(static_cast<qulonglong>(data.size()))
            + "\r\nConnection: close\r\n\r\n";
        if (!writeAll(pipe, head.constData(), head.size())) return;
        if (!data.empty()) {
            writePaced(pipe, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), request.syncSpeed);
        }
        return;
    }
    sendReply(pipe, 404, "Not Found", "text/plain", "not found");
}

class ClientTask final : public QRunnable {
public:
    ClientTask(qintptr handle, std::shared_ptr<State> state)
        : handle_(handle), state_(std::move(state)) {}

    void run() override {
        if (state_->plain) {
            QTcpSocket socket;
            if (!socket.setSocketDescriptor(handle_)) return;
            Pipe pipe;
            pipe.tcp = &socket;
            try {
                handle(&pipe, state_);
            } catch (...) {
            }
            socket.disconnectFromHost();
            return;
        }
        videovault::app::TlsChannel channel;
        if (!channel.accept(handle_, state_->tls)) {
            // A failed handshake must still give the descriptor back, or a
            // scanner on the network leaks one per attempt.
            QTcpSocket orphan;
            if (orphan.setSocketDescriptor(handle_)) orphan.abort();
            return;
        }
        Pipe pipe;
        pipe.tls = &channel;
        // Never let a throw escape a pool thread: terminating the whole server
        // because one client sent something odd is not acceptable.
        try {
            handle(&pipe, state_);
        } catch (...) {
        }
        channel.flush();
        channel.waitForBytesWritten(20000);
        if (channel.state() != QAbstractSocket::UnconnectedState) {
            channel.disconnectFromHost();
            if (channel.state() != QAbstractSocket::UnconnectedState) {
                channel.waitForDisconnected(15000);
            }
        }
    }

private:
    qintptr handle_;
    std::shared_ptr<State> state_;
};

class CloudListen final : public QTcpServer {
public:
    explicit CloudListen(std::shared_ptr<State> state) : state_(std::move(state)) {}

protected:
    void incomingConnection(qintptr handle) override {
        auto* task = new ClientTask(handle, state_);
        task->setAutoDelete(true);
        QThreadPool::globalInstance()->start(task);
    }

private:
    std::shared_ptr<State> state_;
};

std::filesystem::path vaultPath(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toUtf8().constData());
#endif
}

QString findConfig() {
    const QStringList args = QCoreApplication::arguments();
    if (args.size() >= 2 && !args.at(1).startsWith(QLatin1Char('-'))) return args.at(1);
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.json"));
    if (QFile::exists(beside)) return beside;
    if (QFile::exists(QStringLiteral("cloud/config.json"))) return QStringLiteral("cloud/config.json");
    return {};
}

void printUsage() {
    std::cout << "MegaVaultCloud [--vault <folder>] [--port <n>] [--bind <address>] [--plain] [config.json]\n";
    std::cout << "Nothing secret goes on the command line or in a file: each client signs in\n";
    std::cout << "with the vault password, typed in its own app.\n";
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    QString flagVault;
    QString flagBind;
    QString configPath;
    int flagPort = 0;
    bool flagPlain = false;
    bool plainGiven = false;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        const bool hasNext = i + 1 < args.size();
        if (arg == QStringLiteral("--vault") && hasNext) {
            flagVault = args.at(++i).trimmed();
        } else if (arg == QStringLiteral("--port") && hasNext) {
            flagPort = args.at(++i).toInt();
        } else if (arg == QStringLiteral("--bind") && hasNext) {
            flagBind = args.at(++i).trimmed();
        } else if (arg == QStringLiteral("--plain")) {
            flagPlain = true;
            plainGiven = true;
        } else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            printUsage();
            return 0;
        } else if (!arg.startsWith(QLatin1Char('-'))) {
            configPath = arg;
        } else {
            std::cerr << "Unknown option " << arg.toStdString() << "\n";
            printUsage();
            return 1;
        }
    }

    // The file is optional; the same values can come from the command line.
    QJsonObject config;
    if (configPath.isEmpty()) configPath = findConfig();
    if (!configPath.isEmpty()) {
        QFile file(configPath);
        if (!file.open(QIODevice::ReadOnly)) {
            std::cerr << "Could not read the config file " << configPath.toStdString() << "\n";
            return 1;
        }
        QByteArray configBytes = file.readAll();
        if (configBytes.startsWith("\xEF\xBB\xBF")) configBytes = configBytes.mid(3);
        config = QJsonDocument::fromJson(configBytes).object();
        file.close();
    }
    const QString root = (flagVault.isEmpty()
        ? config.value(QStringLiteral("vault")).toString()
        : flagVault).trimmed();
    const int port = flagPort > 0 ? flagPort : config.value(QStringLiteral("port")).toInt(8766);
    const QString bind = (flagBind.isEmpty()
        ? config.value(QStringLiteral("bind")).toString(QStringLiteral("0.0.0.0"))
        : flagBind).trimmed();
    const bool plain = plainGiven ? flagPlain : config.value(QStringLiteral("plain")).toBool(false);
    if (root.isEmpty()) {
        std::cerr << "Point the server at a vault: --vault <folder>.\n";
        printUsage();
        return 1;
    }
    if (!videovault::core::Vault::exists(vaultPath(root))) {
        std::cerr << "There is no vault in " << root.toStdString()
                  << ". Create it in Mega Vault Protect first.\n";
        return 1;
    }
    if (port < 1 || port > 65535) {
        std::cerr << "Port must be between 1 and 65535.\n";
        return 1;
    }

    auto state = std::make_shared<State>();
    state->plain = plain;
    state->root = vaultPath(root);

    QString fingerprint;
    if (!plain) {
        QString error;
        state->tls = videovault::app::ShareTlsIdentity::create(
            {QStringLiteral("127.0.0.1")}, &fingerprint, &error);
        if (!state->tls) {
            std::cerr << (error.isEmpty() ? QStringLiteral("Could not start TLS.") : error).toStdString() << "\n";
            return 1;
        }
    }

    CloudListen server(state);
    const QHostAddress address = (bind == QLatin1String("0.0.0.0") || bind.isEmpty())
        ? QHostAddress::AnyIPv4
        : QHostAddress(bind);
    if (!server.listen(address, static_cast<quint16>(port))) {
        std::cerr << "Could not listen on " << bind.toStdString() << ":" << port << ".\n";
        return 1;
    }
    QThreadPool::globalInstance()->setMaxThreadCount(8);
    std::cout << "Mega Vault Cloud\n";
    std::cout << "Vault: " << root.toStdString() << "\n";
    std::cout << "Listening on " << bind.toStdString() << ":" << server.serverPort() << "\n";
    if (plain) {
        std::cout << "TLS is off. Put a TLS proxy in front before anyone connects.\n";
        std::cout << "See docs/cloud-ssl.md.\n";
    } else {
        std::cout << "Public key:\n" << fingerprint.toStdString() << "\n";
        std::cout << "On the phone or PC choose Cloud login, then enter the address, this public key\n";
        std::cout << "and your vault password. The vault opens with the first sign-in.\n";
    }
    std::cout << "Stop with Ctrl+C. Pass --vault <folder> to point at another vault.\n";
    std::cout.flush();
    return app.exec();
}
