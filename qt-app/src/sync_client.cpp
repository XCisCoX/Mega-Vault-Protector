#include "videovault/app/sync_client.hpp"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace videovault::app::sync {

namespace {

constexpr int kChunk = 1024 * 1024;              // the server's PUT chunk limit
constexpr int kTimeoutMs = 60000;                // per request
constexpr qint64 kMaxResponse = 8 * 1024 * 1024; // a listing or one chunk, never a package
constexpr double kMaxPackage = 1024.0 * 1024.0 * 1024.0;

QByteArray jsonBody(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString idPath(const QString& id) { return QString::fromLatin1(QUrl::toPercentEncoding(id)); }

}  // namespace

void rejectStatus(int status) {
    if (status == 401) {
        throw Failure(QStringLiteral("The server rejected this device's token. Sign in again, "
                                     "or issue a new device token on the website."));
    }
    if (status == 403) throw Failure(QStringLiteral("The server refused this request."));
    if (status == 404) throw Failure(QStringLiteral("The server does not have that vault or record any more."));
    if (status == 409) throw Failure(QStringLiteral("The server reported a version conflict."));
    if (status == 413) throw Failure(QStringLiteral("The package is larger than this server accepts."));
    if (status == 416) throw Failure(QStringLiteral("The server refused the requested byte range."));
    if (status == 422) throw Failure(QStringLiteral("The server rejected a package because its bytes do not match its name."));
    if (status == 507) throw Failure(QStringLiteral("The account has no space left for this package."));
    throw Failure(QStringLiteral("The sync server answered HTTP %1.").arg(status));
}

QJsonObject Client::Response::object() const {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw Failure(QStringLiteral("The server returned invalid JSON."));
    }
    return document.object();
}

Client::Client(const QUrl& base) : base_(base), manager_(new QNetworkAccessManager) {}

Client::Client(const QUrl& base, const QString& token) : base_(base), token_(token.toLatin1()),
                                                         manager_(new QNetworkAccessManager) {}

Client::~Client() {
    if (!token_.isEmpty()) {
        token_.fill('\0');
        token_.clear();
    }
    delete manager_;
}

QString Client::recordPath(const QString& vault, const QString& id) {
    QString path = QStringLiteral("/v1/vaults/%1/records").arg(idPath(vault));
    if (!id.isEmpty()) path += QLatin1Char('/') + idPath(id);
    return path;
}

QString Client::blobPath(const QString& vault, const QString& package) {
    return QStringLiteral("/v1/vaults/%1/blobs/%2").arg(idPath(vault), idPath(package));
}

Client::Response Client::send(const QByteArray& method, const QString& target, const QByteArray& body,
                              const QList<QPair<QByteArray, QByteArray>>& extra) {
    QUrl url(base_);
    const int separator = target.indexOf(QLatin1Char('?'));
    url.setPath(separator < 0 ? target : target.left(separator));
    url.setQuery(separator < 0 ? QString() : target.mid(separator + 1));

    QNetworkRequest request(url);
    // A redirect would replay the bearer token at the wrong host.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token_.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token_);
    if (!cookie_.isEmpty()) request.setRawHeader("Cookie", "mv_session=" + cookie_);
    const bool blob = target.contains(QStringLiteral("/blobs/"));
    if (blob) request.setRawHeader("Content-Type", "application/octet-stream");
    else if (method == "PUT" || method == "POST" || method == "PATCH") request.setRawHeader("Content-Type", "application/json");
    for (const auto& header : extra) request.setRawHeader(header.first, header.second);

    QNetworkReply* reply = nullptr;
    if (method == "HEAD") reply = manager_->head(request);
    else if (method == "GET") reply = manager_->get(request);
    else if (method == "PUT") reply = manager_->put(request, body);
    else reply = manager_->sendCustomRequest(request, method, body);

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QByteArray received;
    bool overflow = false;
    QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
        const QByteArray chunk = reply->readAll();
        if (received.size() + chunk.size() > kMaxResponse) {
            overflow = true;
            reply->abort();
        } else {
            received += chunk;
        }
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { reply->abort(); });
    deadline.start(kTimeoutMs);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    received += reply->readAll();

    Response response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = received;
    for (const auto& header : reply->rawHeaderPairs()) {
        response.headers.insert(header.first.toLower(), header.second);
    }
    const bool finished = reply->isFinished();
    delete reply;

    if (overflow) throw Failure(QStringLiteral("The server sent more data than a sync response may carry."));
    if (!finished) throw Failure(QStringLiteral("The sync server did not answer in time."));
    if (response.status == 0) {
        throw Failure(QStringLiteral("Cannot reach the sync server. Check the URL and that it is running."));
    }
    return response;
}

int Client::protocol() {
    auto answer = send("GET", QStringLiteral("/healthz"));
    if (answer.status != 200) rejectStatus(answer.status);
    const int value = answer.object().value(QStringLiteral("protocol")).toInt();
    if (value != 4) {
        throw Failure(QStringLiteral("This server speaks sync protocol %1; this app needs protocol 4 "
                                     "(accounts, vaults and versioned records).").arg(value));
    }
    return value;
}

QJsonObject Client::login(const QString& email, const QString& password) {
    auto answer = send("POST", QStringLiteral("/auth/login"),
                       jsonBody({{QStringLiteral("email"), email}, {QStringLiteral("password"), password}}));
    if (answer.status == 401) throw Failure(QStringLiteral("The account email or password is wrong."));
    if (answer.status != 200) rejectStatus(answer.status);
    const QByteArray setCookie = answer.header("set-cookie");
    const QByteArray marker = "mv_session=";
    const int start = setCookie.indexOf(marker);
    if (start < 0) throw Failure(QStringLiteral("The server did not start a session."));
    const int end = setCookie.indexOf(';', start);
    cookie_ = setCookie.mid(start + marker.size(), end < 0 ? -1 : end - start - marker.size());
    return answer.object();
}

QJsonObject Client::issueToken(const QString& label) {
    auto answer = send("POST", QStringLiteral("/auth/token"),
                       jsonBody({{QStringLiteral("label"), label.isEmpty() ? QStringLiteral("desktop") : label}}));
    if (answer.status != 201) {
        if (answer.status == 401) {
            throw Failure(QStringLiteral("The sign-in session expired before a device token could be issued."));
        }
        rejectStatus(answer.status);
    }
    const QJsonObject body = answer.object();
    const QString token = body.value(QStringLiteral("token")).toString();
    if (!isHex(token, 64)) throw Failure(QStringLiteral("The server returned a malformed device token."));
    token_ = token.toLatin1();
    return body;
}

QJsonObject Client::vaults() {
    auto answer = send("GET", QStringLiteral("/v1/vaults"));
    if (answer.status != 200) rejectStatus(answer.status);
    return answer.object();
}

Client::Page Client::records(const QString& vault, qint64 since, int limit) {
    const QString target = QStringLiteral("%1?since=%2&limit=%3")
                               .arg(recordPath(vault))
                               .arg(qMax<qint64>(0, since))
                               .arg(qBound(1, limit, 1000));
    auto answer = send("GET", target);
    if (answer.status != 200) rejectStatus(answer.status);
    const QJsonObject body = answer.object();
    if (!body.value(QStringLiteral("records")).isArray()) {
        throw Failure(QStringLiteral("The server returned an invalid record list."));
    }
    Page page;
    const QJsonArray array = body.value(QStringLiteral("records")).toArray();
    if (array.size() > 1000) throw Failure(QStringLiteral("The server returned more records than one page may hold."));
    for (const auto& value : array) {
        const QJsonObject entry = value.toObject();
        RecordRef record = RecordRef::fromJson(entry);
        if (!isRecordId(record.id)) throw Failure(QStringLiteral("The server listed an invalid record id."));
        if (!record.package.isEmpty() && !isPackage(record.package)) {
            throw Failure(QStringLiteral("The server listed an invalid package address."));
        }
        if (record.size < 0 || double(record.size) > kMaxPackage) {
            throw Failure(QStringLiteral("The server listed an invalid package size."));
        }
        page.records.append(record);
    }
    page.cursor = qint64(body.value(QStringLiteral("cursor")).toDouble());
    return page;
}

RecordRef Client::write(const QString& vault, const RecordRef& record, int baseVersion,
                        const QString& operation, int* status) {
    QJsonArray tags;
    for (const auto& tag : record.tags) tags.append(tag);
    const QJsonObject body{{QStringLiteral("base_version"), baseVersion},
                           {QStringLiteral("operation"), operation},
                           {QStringLiteral("name"), record.name},
                           {QStringLiteral("path"), record.path},
                           {QStringLiteral("tags"), tags},
                           {QStringLiteral("package"), record.package.isEmpty() ? QJsonValue() : QJsonValue(record.package)},
                           {QStringLiteral("size"), double(record.size)}};
    auto answer = send("PUT", recordPath(vault, record.id), jsonBody(body));
    if (status != nullptr) *status = answer.status;
    if (answer.status == 409) {
        const QJsonObject body409 = answer.object();
        Conflict conflict;
        conflict.present = body409.value(QStringLiteral("record")).isObject();
        if (conflict.present) conflict.current = RecordRef::fromJson(body409.value(QStringLiteral("record")).toObject());
        throw conflict;
    }
    if (answer.status != 200 && answer.status != 201) rejectStatus(answer.status);
    const QJsonObject answerBody = answer.object();
    if (!answerBody.value(QStringLiteral("record")).isObject()) {
        throw Failure(QStringLiteral("The server did not echo the record it stored."));
    }
    return RecordRef::fromJson(answerBody.value(QStringLiteral("record")).toObject());
}

RecordRef Client::record(const QString& vault, const QString& id) {
    auto answer = send("GET", recordPath(vault, id));
    if (answer.status == 404) throw Failure(QStringLiteral("That item is no longer on the server."));
    if (answer.status != 200) rejectStatus(answer.status);
    return RecordRef::fromJson(answer.object().value(QStringLiteral("record")).toObject());
}

RecordRef Client::restore(const QString& vault, const QString& id) {
    auto answer = send("POST", recordPath(vault, id) + QStringLiteral("/restore"), {});
    if (answer.status != 200) rejectStatus(answer.status);
    return RecordRef::fromJson(answer.object().value(QStringLiteral("record")).toObject());
}

RecordRef Client::tombstone(const QString& vault, const QString& id) {
    auto answer = send("DELETE", recordPath(vault, id));
    if (answer.status == 204) {
        RecordRef gone;
        gone.id = id;
        gone.deleted = true;
        return gone;
    }
    if (answer.status != 200) rejectStatus(answer.status);
    return RecordRef::fromJson(answer.object().value(QStringLiteral("record")).toObject());
}

void Client::purge(const QString& vault, const QString& id) {
    auto answer = send("DELETE", recordPath(vault, id) + QStringLiteral("?purge=1"));
    if (answer.status != 204 && answer.status != 200) rejectStatus(answer.status);
}

Client::BlobState Client::head(const QString& vault, const QString& package) {
    if (!isPackage(package)) throw Failure(QStringLiteral("That is not a package address."));
    auto answer = send("HEAD", blobPath(vault, package));
    BlobState state;
    if (answer.status == 404) return state;
    if (answer.status != 200) rejectStatus(answer.status);
    state.exists = true;
    state.size = answer.header("content-length").toLongLong();
    state.offset = answer.header("x-offset").toLongLong();
    const QByteArray complete = answer.header("x-complete");
    state.complete = complete == "true" || (complete.isEmpty() && state.offset == state.size);
    if (state.size < 0 || double(state.size) > kMaxPackage) {
        throw Failure(QStringLiteral("The server reported an invalid package size."));
    }
    if (state.offset < 0 || state.offset > state.size) {
        throw Failure(QStringLiteral("The server reported an invalid upload offset."));
    }
    return state;
}

void Client::putBlob(const QString& vault, const QString& package, const QString& path,
                     const std::function<bool(qint64, qint64)>& progress) {
    if (!isPackage(package)) throw Failure(QStringLiteral("The package does not hash to a valid address."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw Failure(QStringLiteral("Cannot read the exported package."));
    const qint64 size = file.size();
    if (size <= 0) throw Failure(QStringLiteral("The exported package is empty."));
    if (double(size) > kMaxPackage) throw Failure(QStringLiteral("The package is larger than this server accepts."));

    qint64 offset = 0;
    const BlobState existing = head(vault, package);
    if (existing.exists) {
        if (existing.size != size) throw Failure(QStringLiteral("The server holds a different package under this name."));
        if (existing.complete) {
            if (progress) progress(size, size);
            return;  // already there: no bytes are sent at all
        }
        offset = existing.offset;
    }

    int stale = 0;
    while (offset < size) {
        if (!file.seek(offset)) throw Failure(QStringLiteral("Cannot read the exported package."));
        const QByteArray chunk = file.read(qMin<qint64>(kChunk, size - offset));
        if (chunk.isEmpty()) throw Failure(QStringLiteral("Cannot read the exported package."));
        auto answer = send("PUT", blobPath(vault, package) + QStringLiteral("?offset=%1").arg(offset), chunk,
                           {{"X-Total-Size", QByteArray::number(size)}});
        if (answer.status == 409) {
            const qint64 reported = qint64(answer.object().value(QStringLiteral("offset")).toDouble());
            if (++stale > 4 || reported < 0 || reported > size) {
                throw Failure(QStringLiteral("The server kept reporting a different upload offset."));
            }
            offset = reported;
            if (offset == size) break;
            continue;
        }
        if (answer.status != 200 && answer.status != 201) rejectStatus(answer.status);
        const QJsonObject body = answer.object();
        const qint64 next = qint64(body.value(QStringLiteral("offset")).toDouble());
        const bool complete = body.value(QStringLiteral("complete")).toBool();
        if (next != offset + chunk.size()) throw Failure(QStringLiteral("The server acknowledged a different number of bytes."));
        offset = next;
        if (offset == size && !complete) throw Failure(QStringLiteral("The server did not finish the package."));
        if (progress && !progress(offset, size)) throw Cancelled{};
    }
    if (progress) progress(size, size);
}

QByteArray Client::getRange(const QString& vault, const QString& package, qint64 from, qint64 to, qint64 total) {
    if (!isPackage(package)) throw Failure(QStringLiteral("That is not a package address."));
    if (from < 0 || to < from) throw Failure(QStringLiteral("Invalid download range."));
    auto answer = send("GET", blobPath(vault, package), {},
                       {{"Range", QStringLiteral("bytes=%1-%2").arg(from).arg(to).toLatin1()}});
    if (answer.status == 404) throw Failure(QStringLiteral("A package this vault lists is not on the server yet."));
    if (answer.status == 416) throw Failure(QStringLiteral("The server refused the requested byte range."));
    if (answer.status != 206) rejectStatus(answer.status);
    const QByteArray expected =
        QStringLiteral("bytes %1-%2/%3").arg(from).arg(to).arg(total).toLatin1();
    if (answer.header("content-range") != expected) {
        throw Failure(QStringLiteral("The server answered with an unexpected byte range."));
    }
    if (answer.body.size() != to - from + 1) throw Failure(QStringLiteral("The download stopped before the range ended."));
    return answer.body;
}

void Client::deleteBlob(const QString& vault, const QString& package) {
    auto answer = send("DELETE", blobPath(vault, package));
    if (answer.status != 204 && answer.status != 404) rejectStatus(answer.status);
}

}  // namespace videovault::app::sync
