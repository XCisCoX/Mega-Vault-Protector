#pragma once

#include "videovault/app/sync_state.hpp"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <stdexcept>

class QNetworkAccessManager;

namespace videovault::app::sync {

// A failure the user is shown as written; it ends the operation.
class Failure : public std::runtime_error {
public:
    explicit Failure(const QString& message) : std::runtime_error(message.toUtf8().constData()) {}
};

// stop() was called: not a failure, so nothing is reported as broken.
struct Cancelled {};

// A 409: someone else wrote the record first. It carries the current row so the
// caller can keep both items (the loser becomes a conflict copy).
struct Conflict : Failure {
    Conflict() : Failure(QStringLiteral("The server reported a version conflict.")) {}
    RecordRef current;      // the record the server actually holds
    bool present{false};    // false when the record id is gone entirely
};

// The protocol 4 client: accounts, vaults, versioned records and the
// content-addressed blob tree. One instance per operation, living in the
// thread that performs it, so no reply crosses a thread.
//
// Every /v1 route takes the device token (never the account password, never the
// vault password). Sign-in is POST /auth/login for the website cookie and then
// POST /auth/token, which returns the bearer token exactly once.
class Client {
public:
    explicit Client(const QUrl& base);
    Client(const QUrl& base, const QString& token);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    struct Response {
        int status{0};
        QByteArray body;
        QHash<QByteArray, QByteArray> headers;

        QByteArray header(const char* name) const { return headers.value(QByteArray(name).toLower()); }
        QJsonObject object() const;
    };

    Response send(const QByteArray& method, const QString& target, const QByteArray& body = {},
                  const QList<QPair<QByteArray, QByteArray>>& extra = {});

    void setToken(const QString& token) { token_ = token.toLatin1(); }
    QString token() const { return QString::fromLatin1(token_); }
    QString server() const { return base_.toString(); }

    // -- accounts ---------------------------------------------------------
    int protocol();                                        // GET /healthz
    QJsonObject login(const QString& email, const QString& password);
    QJsonObject issueToken(const QString& label);          // needs the cookie
    QJsonObject vaults();                                  // GET /v1/vaults

    // -- records ----------------------------------------------------------
    struct Page {
        QList<RecordRef> records;
        qint64 cursor{0};
    };
    Page records(const QString& vault, qint64 since, int limit);
    RecordRef write(const QString& vault, const RecordRef& record, int baseVersion,
                    const QString& operation, int* status = nullptr);
    RecordRef record(const QString& vault, const QString& id);
    RecordRef restore(const QString& vault, const QString& id);
    RecordRef tombstone(const QString& vault, const QString& id);   // reversible debris
    void purge(const QString& vault, const QString& id);            // permanent

    // -- blobs ------------------------------------------------------------
    struct BlobState {
        bool exists{false};
        qint64 size{0};
        qint64 offset{0};
        bool complete{false};
    };
    BlobState head(const QString& vault, const QString& package);
    // Uploads `path` as the package named by `package`, resuming from the
    // server's durable offset. `progress` gets (done, total) and may return
    // false to stop.
    void putBlob(const QString& vault, const QString& package, const QString& path,
                 const std::function<bool(qint64, qint64)>& progress = {});
    QByteArray getRange(const QString& vault, const QString& package, qint64 from, qint64 to, qint64 total);
    void deleteBlob(const QString& vault, const QString& package);

    static QString recordPath(const QString& vault, const QString& id = {});
    static QString blobPath(const QString& vault, const QString& package);

private:
    QUrl base_;
    QByteArray token_;
    QByteArray cookie_;
    QNetworkAccessManager* manager_;
};

// The server's own words for the statuses the client cannot resolve itself.
[[noreturn]] void rejectStatus(int status);

}
