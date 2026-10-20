#include "videovault/app/icons.hpp"
#include "videovault/app/cloud_browser.hpp"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QNetworkProxy>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <wincrypt.h>
#else
#include <QSslSocket>
#endif

#include <QPainter>
#include <QPixmap>

namespace videovault::app {
namespace {

struct CloudReply {
    int status{0};
    QByteArray body;
    QString error;
};

QString formatBytes(quint64 size) {
    if (size < 1024ULL) return QString::number(size) + QStringLiteral(" B");
    if (size < 1024ULL * 1024ULL) return QString::number(size / 1024ULL) + QStringLiteral(" KB");
    if (size < 1024ULL * 1024ULL * 1024ULL) {
        return QString::number(static_cast<double>(size) / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
    }
    return QString::number(static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0), 'f', 1) + QStringLiteral(" GB");
}

QIcon greenTick() {
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#107C10")));
    painter.drawEllipse(0, 0, 15, 15);
    painter.setPen(QPen(Qt::white, 1.7));
    painter.drawLine(QPointF(4, 8), QPointF(7, 11));
    painter.drawLine(QPointF(7, 11), QPointF(12, 4.5));
    return QIcon(pix);
}

#if defined(_WIN32)

class WinTls {
public:
    ~WinTls() { close(); }

    bool connectTo(const QString& host, quint16 port, const QString& fingerprint, QString* error) {
        socket_.setProxy(QNetworkProxy::NoProxy);
        socket_.connectToHost(host, port);
        if (!socket_.waitForConnected(8000)) {
            if (error != nullptr) *error = QStringLiteral("Could not reach the cloud.");
            return false;
        }
        SCHANNEL_CRED cred{};
        cred.dwVersion = SCHANNEL_CRED_VERSION;
        cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
#ifdef SP_PROT_TLS1_3_CLIENT
        cred.grbitEnabledProtocols |= SP_PROT_TLS1_3_CLIENT;
#endif
        cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;
        TimeStamp expiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
            SECPKG_CRED_OUTBOUND, nullptr, &cred, nullptr, nullptr, &credentials_, &expiry);
        if (status != SEC_E_OK) {
            cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
            status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                SECPKG_CRED_OUTBOUND, nullptr, &cred, nullptr, nullptr, &credentials_, &expiry);
        }
        if (status != SEC_E_OK) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        haveCredentials_ = true;
        const std::wstring hostName = host.toStdWString();
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
            | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
        QByteArray incoming;
        bool first = true;
        for (int step = 0; step < 16; ++step) {
            SecBuffer inBuffers[2]{};
            inBuffers[0].BufferType = SECBUFFER_TOKEN;
            inBuffers[0].pvBuffer = incoming.isEmpty() ? nullptr : incoming.data();
            inBuffers[0].cbBuffer = static_cast<unsigned long>(incoming.size());
            inBuffers[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc inDesc{SECBUFFER_VERSION, 2, inBuffers};
            SecBuffer outBuffer{};
            outBuffer.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &outBuffer};
            status = InitializeSecurityContextW(
                &credentials_,
                first ? nullptr : &context_,
                const_cast<wchar_t*>(hostName.c_str()),
                flags,
                0,
                SECURITY_NATIVE_DREP,
                (first && incoming.isEmpty()) ? nullptr : &inDesc,
                0,
                &context_,
                &outDesc,
                &contextFlags_,
                nullptr);
            first = false;
            if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE
                || status == SEC_E_INCOMPLETE_MESSAGE) {
                haveContext_ = true;
            }
            if (outBuffer.cbBuffer != 0 && outBuffer.pvBuffer != nullptr) {
                if (!writeRaw(static_cast<const char*>(outBuffer.pvBuffer), static_cast<int>(outBuffer.cbBuffer))) {
                    FreeContextBuffer(outBuffer.pvBuffer);
                    if (error != nullptr) *error = QStringLiteral("Could not reach the cloud.");
                    return false;
                }
                FreeContextBuffer(outBuffer.pvBuffer);
            }
            if (status == SEC_E_OK) {
                if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                    pending_ = QByteArray(static_cast<const char*>(inBuffers[1].pvBuffer),
                        static_cast<int>(inBuffers[1].cbBuffer));
                }
                break;
            }
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!readMore(&incoming)) {
                    if (error != nullptr) *error = QStringLiteral("Could not reach the cloud.");
                    return false;
                }
                continue;
            }
            if (status == SEC_I_CONTINUE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
                if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                    incoming = QByteArray(static_cast<const char*>(inBuffers[1].pvBuffer),
                        static_cast<int>(inBuffers[1].cbBuffer));
                } else {
                    incoming.clear();
                }
                if (incoming.isEmpty() && !readMore(&incoming)) {
                    if (error != nullptr) *error = QStringLiteral("Could not reach the cloud.");
                    return false;
                }
                continue;
            }
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        if (status != SEC_E_OK) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        PCCERT_CONTEXT certificate = nullptr;
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) != SEC_E_OK
            || certificate == nullptr) {
            if (error != nullptr) *error = QStringLiteral("The certificate does not match.");
            return false;
        }
        const QByteArray der(reinterpret_cast<const char*>(certificate->pbCertEncoded),
            static_cast<int>(certificate->cbCertEncoded));
        CertFreeCertificateContext(certificate);
        const QString actual = QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex());
        if (actual.compare(fingerprint, Qt::CaseInsensitive) != 0) {
            if (error != nullptr) *error = QStringLiteral("The certificate does not match the fingerprint.");
            return false;
        }
        return true;
    }

    bool writePlain(const QByteArray& plain) {
        int off = 0;
        const int maxChunk = sizes_.cbMaximumMessage > 0 ? static_cast<int>(sizes_.cbMaximumMessage) : 16384;
        while (off < plain.size()) {
            const int n = std::min(maxChunk, plain.size() - off);
            QByteArray message(static_cast<int>(sizes_.cbHeader) + n + static_cast<int>(sizes_.cbTrailer), '\0');
            memcpy(message.data() + sizes_.cbHeader, plain.constData() + off, static_cast<std::size_t>(n));
            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            buffers[0].pvBuffer = message.data();
            buffers[0].cbBuffer = sizes_.cbHeader;
            buffers[1].BufferType = SECBUFFER_DATA;
            buffers[1].pvBuffer = message.data() + sizes_.cbHeader;
            buffers[1].cbBuffer = static_cast<unsigned long>(n);
            buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            buffers[2].pvBuffer = message.data() + sizes_.cbHeader + n;
            buffers[2].cbBuffer = sizes_.cbTrailer;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&context_, 0, &desc, 0) != SEC_E_OK) return false;
            const int wrote = static_cast<int>(buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
            if (!writeRaw(message.constData(), wrote)) return false;
            off += n;
        }
        return true;
    }

    bool readPlain(QByteArray* out, int timeoutMs) {
        out->clear();
        for (int spin = 0; spin < 8; ++spin) {
            if (pending_.isEmpty()) {
                if (!socket_.waitForReadyRead(timeoutMs)) return false;
                pending_ += socket_.readAll();
            }
            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[0].pvBuffer = pending_.data();
            buffers[0].cbBuffer = static_cast<unsigned long>(pending_.size());
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS status = DecryptMessage(&context_, &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!socket_.waitForReadyRead(timeoutMs)) return false;
                pending_ += socket_.readAll();
                continue;
            }
            if (status != SEC_E_OK && status != SEC_I_CONTEXT_EXPIRED) return false;
            QByteArray extra;
            for (const SecBuffer& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0 && buffer.pvBuffer != nullptr) {
                    out->append(static_cast<const char*>(buffer.pvBuffer), static_cast<int>(buffer.cbBuffer));
                } else if (buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer > 0 && buffer.pvBuffer != nullptr) {
                    extra = QByteArray(static_cast<const char*>(buffer.pvBuffer), static_cast<int>(buffer.cbBuffer));
                }
            }
            pending_ = extra;
            return true;
        }
        return false;
    }

    void close() {
        if (haveContext_) {
            DeleteSecurityContext(&context_);
            haveContext_ = false;
        }
        if (haveCredentials_) {
            FreeCredentialsHandle(&credentials_);
            haveCredentials_ = false;
        }
        socket_.disconnectFromHost();
    }

private:
    bool writeRaw(const char* data, int size) {
        int off = 0;
        while (off < size) {
            const qint64 wrote = socket_.write(data + off, size - off);
            if (wrote <= 0) return false;
            off += static_cast<int>(wrote);
            if (!socket_.waitForBytesWritten(20000)) return false;
        }
        return true;
    }

    bool readMore(QByteArray* incoming) {
        if (!socket_.waitForReadyRead(8000)) return false;
        *incoming += socket_.readAll();
        return !incoming->isEmpty();
    }

    QTcpSocket socket_;
    CredHandle credentials_{};
    CtxtHandle context_{};
    SecPkgContext_StreamSizes sizes_{};
    DWORD contextFlags_{0};
    bool haveCredentials_{false};
    bool haveContext_{false};
    QByteArray pending_;
};

#else

class WinTls {
public:
    bool connectTo(const QString& host, quint16 port, const QString& fingerprint, QString* error) {
        socket_.setProxy(QNetworkProxy::NoProxy);
        socket_.setPeerVerifyMode(QSslSocket::VerifyNone);
        socket_.connectToHostEncrypted(host, port);
        if (!socket_.waitForEncrypted(8000)) {
            if (error != nullptr) *error = QStringLiteral("Could not reach the cloud.");
            return false;
        }
        const QByteArray der = socket_.peerCertificate().toDer();
        const QString actual = QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex());
        if (der.isEmpty() || actual.compare(fingerprint, Qt::CaseInsensitive) != 0) {
            if (error != nullptr) *error = QStringLiteral("The certificate does not match the fingerprint.");
            return false;
        }
        return true;
    }

    bool writePlain(const QByteArray& plain) {
        int off = 0;
        while (off < plain.size()) {
            const qint64 wrote = socket_.write(plain.constData() + off, plain.size() - off);
            if (wrote <= 0) return false;
            off += static_cast<int>(wrote);
            if (!socket_.waitForBytesWritten(20000)) return false;
        }
        return true;
    }

    bool readPlain(QByteArray* out, int timeoutMs) {
        if (!socket_.waitForReadyRead(timeoutMs)) return false;
        *out = socket_.readAll();
        return !out->isEmpty();
    }

    void close() { socket_.disconnectFromHost(); }

private:
    QSslSocket socket_;
};

#endif

CloudReply exchange(
    const QString& host,
    quint16 port,
    const QString& fingerprint,
    const QByteArray& method,
    const QString& path,
    const QList<QByteArray>& headers,
    const QByteArray& body) {
    CloudReply reply;
    WinTls tls;
    if (!tls.connectTo(host, port, fingerprint, &reply.error)) return reply;
    QByteArray request;
    request += method + " " + path.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: " + host.toUtf8() + "\r\n";
    request += "Connection: close\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    for (const QByteArray& header : headers) {
        request += header + "\r\n";
    }
    request += "\r\n";
    request += body;
    if (!tls.writePlain(request)) {
        reply.error = QStringLiteral("Could not reach the cloud.");
        return reply;
    }
    QByteArray raw;
    for (int i = 0; i < 100000 && !raw.contains("\r\n\r\n"); ++i) {
        QByteArray chunk;
        if (!tls.readPlain(&chunk, 20000)) break;
        raw += chunk;
    }
    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0 || raw.size() < 12) {
        reply.error = QStringLiteral("The cloud closed the connection.");
        return reply;
    }
    const QByteArray statusLine = raw.left(raw.indexOf("\r\n"));
    const QList<QByteArray> statusParts = statusLine.split(' ');
    if (statusParts.size() >= 2) reply.status = statusParts.at(1).toInt();
    int contentLength = -1;
    const QList<QByteArray> lines = raw.left(headerEnd).split('\n');
    for (const QByteArray& line : lines) {
        if (line.toLower().trimmed().startsWith("content-length:")) {
            contentLength = line.mid(int(sizeof("content-length:") - 1)).trimmed().toInt();
        }
    }
    QByteArray payload = raw.mid(headerEnd + 4);
    if (contentLength >= 0) {
        while (payload.size() < contentLength) {
            QByteArray chunk;
            if (!tls.readPlain(&chunk, 20000) || chunk.isEmpty()) break;
            payload += chunk;
        }
        payload.truncate(contentLength);
    }
    reply.body = payload;
    tls.close();
    return reply;
}

QByteArray headerValue(QString text) {
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.toUtf8();
}

QList<QByteArray> identityHeaders(const QString& model, const QString& speed, bool media) {
    QList<QByteArray> headers;
    headers << ("X-Model: " + headerValue(model));
    headers << QByteArray("X-Kind: pc");
    if (media) headers << ("X-Sync-Speed: " + speed.toUtf8());
    return headers;
}

} // namespace

CloudBrowser::CloudBrowser(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("Cloud"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    title->setFont(titleFont);
    layout->addWidget(title);
    people_ = new QLabel(QStringLiteral("Connecting…"), this);
    people_->setWordWrap(true);
    layout->addWidget(people_);
    auto* speedRow = new QHBoxLayout();
    speedRow->addWidget(new QLabel(QStringLiteral("Sync"), this));
    speedBox_ = new QComboBox(this);
    speedBox_->addItem(QStringLiteral("Slow"), QStringLiteral("slow"));
    speedBox_->addItem(QStringLiteral("Normal"), QStringLiteral("normal"));
    speedBox_->addItem(QStringLiteral("Fast"), QStringLiteral("fast"));
    speedBox_->setCurrentIndex(1);
    speedRow->addWidget(speedBox_, 1);
    layout->addLayout(speedRow);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    list_ = new QListWidget(this);
    layout->addWidget(list_, 1);
    auto* buttons = new QHBoxLayout();
    auto* remove = new QPushButton(QStringLiteral("Delete"), this);
    icons::withText(remove, QStringLiteral("trash"));
    auto* leave = new QPushButton(QStringLiteral("Leave"), this);
    icons::withText(leave, QStringLiteral("log-out"));
    buttons->addWidget(remove);
    buttons->addWidget(leave);
    layout->addLayout(buttons);
    connect(speedBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        speed_ = speedBox_->currentData().toString();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto* item = list_->currentItem();
        if (item == nullptr || token_.isEmpty() || item->data(Qt::UserRole + 2).toBool()) return;
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        const bool folder = item->data(Qt::UserRole + 1).toBool();
        const QString path = folder
            ? QStringLiteral("/v1/remove-folder?id=%1").arg(id)
            : QStringLiteral("/v1/remove?id=%1").arg(id);
        status_->setText(QStringLiteral("Removing…"));
        const QString host = host_;
        const quint16 port = port_;
        const QString fingerprint = fingerprint_;
        const QByteArray token = token_;
        const QString model = model_;
        auto* watcher = new QFutureWatcher<CloudReply>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            const CloudReply reply = watcher->result();
            watcher->deleteLater();
            if (reply.status != 200) {
                status_->setText(reply.error.isEmpty() ? QStringLiteral("Could not remove it.") : reply.error);
                return;
            }
            refreshRemote();
        });
        watcher->setFuture(QtConcurrent::run([host, port, fingerprint, token, path, model] {
            QList<QByteArray> headers = identityHeaders(model, QStringLiteral("fast"), false);
            headers.push_front("Authorization: Bearer " + token);
            return exchange(host, port, fingerprint, "POST", path, headers, {});
        }));
    });
    connect(leave, &QPushButton::clicked, this, [this] {
        if (onLeave_) onLeave_();
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item == nullptr) return;
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        if (item->data(Qt::UserRole + 1).toBool()) {
            showFolder(id);
            return;
        }
        const QString path = cacheFile(id);
        if (QFile::exists(path)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
        priorityId_ = id;
        status_->setText(QStringLiteral("This file is still syncing."));
        pump();
    });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] { refreshRemote(); });
}

void CloudBrowser::setOnLeave(std::function<void()> callback) { onLeave_ = std::move(callback); }

QString CloudBrowser::cacheFile(std::int64_t id) const {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/cloud");
    QDir().mkpath(root);
    return QDir(root).filePath(QString::number(id));
}

bool CloudBrowser::isSynced(const CloudRow& row) const {
    if (row.folder || row.size == 0) return !row.folder;
    const QFileInfo info(cacheFile(row.id));
    return info.isFile() && static_cast<quint64>(info.size()) == row.size;
}

void CloudBrowser::openSession(
    const QString& address, const QString& fingerprint, const QString& password) {
    const int colon = address.lastIndexOf(QLatin1Char(':'));
    host_ = colon > 0 ? address.left(colon).trimmed() : address.trimmed();
    bool portOk = false;
    const uint port = colon > 0 ? address.mid(colon + 1).trimmed().toUInt(&portOk) : 0U;
    port_ = portOk ? static_cast<quint16>(port) : 0;
    fingerprint_ = fingerprint.trimmed().toLower();
    model_ = QSysInfo::machineHostName();
    people_->setText(QStringLiteral("Connecting…"));
    status_->clear();
    if (host_.isEmpty() || port_ == 0 || fingerprint_.size() != 64) {
        people_->setText(QStringLiteral("Enter the address as host:port and the server public key."));
        return;
    }
    const QString host = host_;
    const quint16 cloudPort = port_;
    const QString pin = fingerprint_;
    const QString cloudModel = model_;
    const QByteArray secret = password.toUtf8();
    auto* watcher = new QFutureWatcher<CloudReply>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const CloudReply reply = watcher->result();
        watcher->deleteLater();
        if (reply.status != 200) {
            people_->setText(reply.error.isEmpty()
                ? (reply.status == 401 ? QStringLiteral("Wrong password.") : QStringLiteral("Could not sign in."))
                : reply.error);
            return;
        }
        const QJsonObject json = QJsonDocument::fromJson(reply.body).object();
        token_ = json.value(QStringLiteral("token")).toString().toUtf8();
        if (token_.isEmpty()) {
            people_->setText(QStringLiteral("The cloud did not accept the password."));
            return;
        }
        timer_->start(4000);
        refreshRemote();
    });
    watcher->setFuture(QtConcurrent::run([host, cloudPort, pin, cloudModel, secret] {
        QList<QByteArray> headers = identityHeaders(cloudModel, QStringLiteral("fast"), false);
        return exchange(host, cloudPort, pin, "POST", QStringLiteral("/v1/login"), headers, secret);
    }));
}

void CloudBrowser::refreshRemote() {
    if (token_.isEmpty() || refreshing_) return;
    refreshing_ = true;
    const QString host = host_;
    const quint16 port = port_;
    const QString fingerprint = fingerprint_;
    const QByteArray token = token_;
    const QString model = model_;
    struct Bundle {
        CloudReply sessions;
        CloudReply videos;
        CloudReply folders;
    };
    auto* watcher = new QFutureWatcher<Bundle>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const Bundle bundle = watcher->result();
        watcher->deleteLater();
        refreshing_ = false;
        if (bundle.videos.status != 200 || bundle.folders.status != 200) {
            status_->setText(bundle.videos.error.isEmpty() ? QStringLiteral("Could not load the vault.") : bundle.videos.error);
            return;
        }
        QString people;
        const QJsonArray users = QJsonDocument::fromJson(bundle.sessions.body).array();
        for (const QJsonValue& value : users) {
            const QJsonObject person = value.toObject();
            if (!people.isEmpty()) people += QStringLiteral("    ");
            people += person.value(QStringLiteral("model")).toString() + QStringLiteral(" · ")
                + person.value(QStringLiteral("kind")).toString();
        }
        applyLibrary(bundle.videos.body, bundle.folders.body, people.isEmpty() ? QStringLiteral("No one else is here.") : people);
    });
    watcher->setFuture(QtConcurrent::run([host, port, fingerprint, token, model] {
        QList<QByteArray> headers = identityHeaders(model, QStringLiteral("fast"), false);
        headers.push_front("Authorization: Bearer " + token);
        Bundle bundle;
        bundle.sessions = exchange(host, port, fingerprint, "GET", QStringLiteral("/v1/sessions"), headers, {});
        bundle.videos = exchange(host, port, fingerprint, "GET", QStringLiteral("/v1/videos"), headers, {});
        bundle.folders = exchange(host, port, fingerprint, "GET", QStringLiteral("/v1/folders"), headers, {});
        return bundle;
    }));
}

void CloudBrowser::applyLibrary(const QByteArray& videos, const QByteArray& folders, const QString& sessions) {
    people_->setText(sessions);
    rows_.clear();
    const QJsonArray folderList = QJsonDocument::fromJson(folders).array();
    for (const QJsonValue& value : folderList) {
        const QJsonObject folder = value.toObject();
        CloudRow row;
        row.id = folder.value(QStringLiteral("id")).toVariant().toLongLong();
        row.parent = folder.value(QStringLiteral("parentId")).toVariant().toLongLong();
        row.name = folder.value(QStringLiteral("name")).toString();
        row.folder = true;
        rows_.push_back(row);
    }
    const QJsonArray videoList = QJsonDocument::fromJson(videos).array();
    for (const QJsonValue& value : videoList) {
        const QJsonObject video = value.toObject();
        CloudRow row;
        row.id = video.value(QStringLiteral("id")).toVariant().toLongLong();
        row.parent = video.value(QStringLiteral("folderId")).toVariant().toLongLong();
        row.name = video.value(QStringLiteral("name")).toString();
        row.size = video.value(QStringLiteral("size")).toVariant().toULongLong();
        rows_.push_back(row);
    }
    showFolder(folderId_);
    pump();
}

void CloudBrowser::showFolder(std::int64_t folderId) {
    folderId_ = folderId;
    list_->clear();
    if (folderId != 0) {
        std::int64_t parent = 0;
        for (const CloudRow& row : rows_) {
            if (row.folder && row.id == folderId) parent = row.parent;
        }
        auto* up = new QListWidgetItem(QStringLiteral(".."), list_);
        up->setData(Qt::UserRole, static_cast<qlonglong>(parent));
        up->setData(Qt::UserRole + 1, true);
        up->setData(Qt::UserRole + 2, true);
    }
    for (const CloudRow& row : rows_) {
        if (!row.folder || row.parent != folderId) continue;
        auto* item = new QListWidgetItem(row.name, list_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(row.id));
        item->setData(Qt::UserRole + 1, true);
    }
    for (const CloudRow& row : rows_) {
        if (row.folder || row.parent != folderId) continue;
        auto* item = new QListWidgetItem(row.name + QStringLiteral("    ") + formatBytes(row.size), list_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(row.id));
        item->setData(Qt::UserRole + 1, false);
        if (isSynced(row)) item->setIcon(greenTick());
    }
}

void CloudBrowser::markSynced(std::int64_t id) {
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* item = list_->item(i);
        if (!item->data(Qt::UserRole + 1).toBool() && item->data(Qt::UserRole).toLongLong() == id) {
            item->setIcon(greenTick());
        }
    }
    bool pending = false;
    for (const CloudRow& row : rows_) {
        if (!row.folder && row.size > 0 && !isSynced(row)) pending = true;
    }
    status_->setText(pending ? QStringLiteral("Syncing files…") : QStringLiteral("Synced"));
}

void CloudBrowser::pump() {
    if (downloading_ || token_.isEmpty()) return;
    const CloudRow* next = nullptr;
    for (const CloudRow& row : rows_) {
        if (row.folder || row.size == 0 || isSynced(row) || failed_.contains(row.id)) continue;
        if (row.id == priorityId_) {
            next = &row;
            break;
        }
        if (next == nullptr) next = &row;
    }
    if (next == nullptr) {
        status_->setText(rows_.empty() ? QString() : QStringLiteral("Synced"));
        return;
    }
    const CloudRow row = *next;
    priorityId_ = -1;
    downloading_ = true;
    status_->setText(QStringLiteral("Syncing ") + row.name);
    const QString host = host_;
    const quint16 port = port_;
    const QString fingerprint = fingerprint_;
    const QByteArray token = token_;
    const QString model = model_;
    const QString speed = speed_;
    const QString dest = cacheFile(row.id);
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, row] {
        const bool ok = watcher->result();
        watcher->deleteLater();
        downloading_ = false;
        if (ok) markSynced(row.id);
        else {
            failed_.insert(row.id);
            status_->setText(QStringLiteral("Could not sync ") + row.name);
        }
        pump();
    });
    watcher->setFuture(QtConcurrent::run([host, port, fingerprint, token, model, speed, row, dest] {
        QFile part(dest + QStringLiteral(".part"));
        if (!part.open(QIODevice::WriteOnly)) return false;
        std::uint64_t at = 0;
        while (at < row.size) {
            const int ask = static_cast<int>(std::min<std::uint64_t>(256ULL * 1024ULL, row.size - at));
            const QString path = QStringLiteral("/v1/read?id=%1&offset=%2&size=%3").arg(row.id).arg(at).arg(ask);
            QList<QByteArray> headers = identityHeaders(model, speed, true);
            headers.push_front("Authorization: Bearer " + token);
            const CloudReply reply = exchange(host, port, fingerprint, "GET", path, headers, {});
            if (reply.status != 200 || reply.body.isEmpty()) {
                part.close();
                part.remove();
                return false;
            }
            if (part.write(reply.body) != reply.body.size()) {
                part.close();
                part.remove();
                return false;
            }
            at += static_cast<std::uint64_t>(reply.body.size());
        }
        part.close();
        QFile::remove(dest);
        return QFile::rename(dest + QStringLiteral(".part"), dest);
    }));
}

} // namespace videovault::app
