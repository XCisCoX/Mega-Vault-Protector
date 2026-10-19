#include "videovault/app/plugin_host.hpp"
#include "videovault/app/plugin_audio.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QHostInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <span>
static void pluginResources() { Q_INIT_RESOURCE(plugins); }
namespace videovault::app {
namespace {
constexpr int metadataLimit = 1024 * 1024;
constexpr int manifestLimit = 512 * 1024;
QString identity(const std::shared_ptr<core::Vault> &vault) {
    if (!vault)
        return {};
    const auto text = vault->root_path().lexically_normal().u8string();
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(text.data()), int(text.size())),
                                 QCryptographicHash::Sha256)
            .toHex());
}
bool validId(const QString &s) { return QRegularExpression("^[0-9]{17,20}$").match(s).hasMatch(); }
void wipe(QByteArray &bytes) {
    if (!bytes.isEmpty()) {
        volatile char *p = bytes.data();
        for (int i = 0; i < bytes.size(); ++i)
            p[i] = 0;
    }
    bytes.clear();
    bytes.squeeze();
}
#ifdef Q_OS_WIN
// Qt 5.12 loads OpenSSL 1.1, while the shipped app carries OpenSSL 3, so
// QSslSocket cannot initialise in the release build and every Qt HTTPS request
// fails before it is sent. The updater already goes through WinHTTP for the
// same reason; provider requests take that path too so music works in the
// shipped Windows build, with the Windows certificate store and no extra DLLs.
struct WinHttpHandle {
    HINTERNET handle{nullptr};
    ~WinHttpHandle() {
        if (handle != nullptr)
            WinHttpCloseHandle(handle);
    }
};
QString winHttpError(const wchar_t *step) {
    return QStringLiteral("%1 failed (Windows error %2).")
        .arg(QString::fromWCharArray(step))
        .arg(GetLastError());
}
bool winHttpGet(const QUrl &url, const qint64 limit,
                const std::function<void(qint64, qint64)> &progress,
                QByteArray *body, int *status, QUrl *location, QString *error) {
    if (!url.isValid() || url.host().isEmpty()) {
        *error = QStringLiteral("Invalid provider URL.");
        return false;
    }
    QString target = url.path(QUrl::FullyEncoded);
    if (target.isEmpty())
        target = QStringLiteral("/");
    if (!url.query().isEmpty())
        target += QLatin1Char('?') + url.query(QUrl::FullyEncoded);

    WinHttpHandle session(WinHttpOpen(L"MegaVaultProtect", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) {
        *error = winHttpError(L"WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session.handle, 8000, 8000, 30000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    WinHttpHandle connection(WinHttpConnect(session.handle,
                                            reinterpret_cast<LPCWSTR>(url.host().utf16()),
                                            static_cast<INTERNET_PORT>(url.port(443)), 0));
    if (!connection.handle) {
        *error = winHttpError(L"WinHttpConnect");
        return false;
    }
    WinHttpHandle request(WinHttpOpenRequest(connection.handle, L"GET",
                                             reinterpret_cast<LPCWSTR>(target.utf16()), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request.handle) {
        *error = winHttpError(L"WinHttpOpenRequest");
        return false;
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                     sizeof(redirectPolicy));
    const QString headerText =
        QStringLiteral("User-Agent: MegaVaultProtect/2.0.3 (music plugin)") + QChar(13) +
        QChar(10) + QStringLiteral("Accept: application/json, audio/*") + QChar(13) + QChar(10);
    WinHttpAddRequestHeaders(request.handle, reinterpret_cast<LPCWSTR>(headerText.utf16()),
                             static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        const DWORD failure = GetLastError();
        // 12002/12029/12030 mean timed out, could not connect, or connection
        // aborted. On a filtered network that is the provider being
        // unreachable, so say so instead of printing a Windows error number.
        if (failure == ERROR_WINHTTP_TIMEOUT || failure == ERROR_WINHTTP_CANNOT_CONNECT ||
            failure == ERROR_WINHTTP_CONNECTION_ERROR) {
            *error = QStringLiteral(
                "the provider is unreachable from this network (connection timed out). "
                "If it is blocked here, turn on your VPN or system proxy and try again.");
        } else {
            *error = winHttpError(L"WinHttpSendRequest");
        }
        return false;
    }

    DWORD code = 0;
    DWORD codeSize = sizeof(code);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX);
    *status = static_cast<int>(code);
    if (code >= 300 && code < 400) {
        DWORD length = 0;
        if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION,
                                WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &length,
                                WINHTTP_NO_HEADER_INDEX) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring value(length / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &length,
                                    WINHTTP_NO_HEADER_INDEX))
                *location = QUrl(QString::fromWCharArray(value.c_str()));
        }
        return true;
    }
    if (code < 200 || code >= 300) {
        *error = QStringLiteral("HTTP %1").arg(code);
        return true;
    }

    DWORD reportedLength = 0;
    DWORD lengthSize = sizeof(reportedLength);
    const bool hasLength =
        WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &reportedLength, &lengthSize,
                            WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (hasLength && qint64(reportedLength) > limit) {
        *error = QStringLiteral("Response is %1 MiB, over the %2 MiB limit.")
                     .arg(double(reportedLength) / (1024 * 1024), 0, 'f', 1)
                     .arg(double(limit) / (1024 * 1024), 0, 'f', 0);
        return false;
    }
    qint64 received = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available) || available == 0)
            break;
        if (received + qint64(available) > limit) {
            *error = QStringLiteral("Response exceeds the configured memory limit.");
            return false;
        }
        const int offset = body->size();
        body->resize(offset + int(available));
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, body->data() + offset, available, &read) || read == 0) {
            body->resize(offset);
            break;
        }
        body->resize(offset + int(read));
        received += qint64(read);
        if (progress)
            progress(received, hasLength ? qint64(reportedLength) : 0);
    }
    return true;
}
#endif
class Switch final : public QCheckBox {
  public:
    explicit Switch(QWidget *p) : QCheckBox(p) {
        setFixedSize(44, 26);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName("Enable plugin");
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(isChecked() ? QPalette::Highlight : QPalette::Mid));
        p.drawRoundedRect(QRectF(1, 3, 42, 20), 10, 10);
        p.setBrush(palette().color(QPalette::Base));
        p.drawEllipse(QRectF(isChecked() ? 25 : 3, 5, 16, 16));
    }
    bool hitButton(const QPoint &p) const override { return rect().contains(p); }
};
QString errorText(const QString &s) { return QStringLiteral("Plugin: ") + s; }
} // namespace
bool PluginManifest::parse(const QByteArray &bytes, PluginManifest *out, QString *error) {
    auto fail = [&](const QString &e) {
        if (error)
            *error = e;
        return false;
    };
    if (bytes.isEmpty() || bytes.size() > manifestLimit)
        return fail("Manifest must be 1–524288 bytes.");
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail("Invalid JSON manifest.");
    auto o = doc.object();
    const QSet<QString> fields{"schemaVersion", "id",        "name",        "version", "description",
                               "protocol",      "platforms", "permissions", "network", "sourceNotice", "commands", "script", "settings"};
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!fields.contains(it.key()))
            return fail("Unsupported manifest field: " + it.key());
    const auto protocol = o.value("protocol").toString();
    if (o.value("schemaVersion") != QJsonValue(1) || (protocol != "monochrome-tracks-v1" && protocol != "megavault-actions-v1"))
        return fail("Unsupported schema or provider protocol.");
    PluginManifest m;
    m.protocol = protocol;
    m.id = o.value("id").toString();
    m.name = o.value("name").toString();
    m.version = o.value("version").toString();
    m.description = o.value("description").toString();
    m.sourceNotice = o.value("sourceNotice").toString();
    if (m.sourceNotice.isEmpty() && protocol == "megavault-actions-v1") m.sourceNotice = "Sandboxed local tool. No network, filesystem or vault access.";
    if (!QRegularExpression("^[a-z][a-z0-9]*(?:\\.[a-z][a-z0-9-]*){2,7}$").match(m.id).hasMatch() ||
        m.id.size() > 120)
        return fail("Invalid plugin identifier.");
    if (!QRegularExpression("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
             .match(m.version)
             .hasMatch() ||
        m.version.size() > 32)
        return fail("Version must be major.minor.patch.");
    for (const auto &text : {m.name, m.description, m.sourceNotice})
        if (text.trimmed().isEmpty() || text.size() > 2048 || text.contains(QChar(0)))
            return fail("Missing or oversized display text.");
    if (m.name.size() > 100)
        return fail("Plugin name too long.");
    if(o.contains("settings")) {
        if(!o.value("settings").isArray())return fail("Extension settings must be an array.");
        QSet<QString> ids;
        for(auto entry:o.value("settings").toArray()) {
            const auto setting=entry.toObject();PluginSetting descriptor{setting.value("id").toString(),setting.value("title").toString(),setting.value("default").toBool()};
            if(setting.size()!=4 || setting.value("type").toString()!="boolean" || !setting.value("default").isBool() || !QRegularExpression("^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$").match(descriptor.id).hasMatch() || descriptor.id.size()>64 || ids.contains(descriptor.id) || descriptor.title.trimmed().isEmpty() || descriptor.title.size()>80 || descriptor.title.contains(QChar(0)))return fail("Invalid, duplicate or unsupported extension setting.");
            ids.insert(descriptor.id);m.settings.append(descriptor);
        }
        if(m.settings.size()>16)return fail("Extension may declare at most 16 boolean settings.");
    }
#if defined(Q_OS_WIN)
    const QString platform = "windows";
#elif defined(Q_OS_ANDROID)
    const QString platform = "android";
#else
    const QString platform = "linux";
#endif
    if (!o.value("platforms").isArray())
        return fail("Missing platform list.");
    QSet<QString> platforms;
    for (auto p : o.value("platforms").toArray()) {
        if (!p.isString() || !QStringList{"windows", "linux", "android"}.contains(p.toString()) ||
            platforms.contains(p.toString()))
            return fail("Unsupported or duplicate platform.");
        platforms.insert(p.toString());
    }
    if (!platforms.contains(platform))
        return fail("Plugin does not support this platform.");
    QSet<QString> permissions;
    if (!o.value("permissions").isArray()) return fail("Missing permissions list.");
    for (auto p : o.value("permissions").toArray()) {
        if (!p.isString() || permissions.contains(p.toString()))
            return fail("Invalid permission list.");
        permissions.insert(p.toString());
    }
    if (protocol == "megavault-actions-v1") {
        if (!permissions.isEmpty() || o.contains("network")) return fail("Sandboxed action extensions must request no host permissions or network policy.");
        m.script = o.value("script").toString();
        if (m.script.trimmed().isEmpty() || m.script.toUtf8().size() > 256*1024 || m.script.contains(QChar(0))) return fail("Extension script must be 1–262144 UTF-8 bytes.");
        if (!o.value("commands").isArray()) return fail("Extension must declare commands.");
        QSet<QString> ids;
        for (auto value : o.value("commands").toArray()) {
            const auto command=value.toObject();
            PluginCommand c{command.value("id").toString(),command.value("title").toString(),command.value("inputLabel").toString()};
            if(command.size()!=3 || !QRegularExpression("^[a-z][a-z0-9-]{0,63}$").match(c.id).hasMatch() || ids.contains(c.id) || c.title.trimmed().isEmpty() || c.title.size()>120 || c.inputLabel.trimmed().isEmpty() || c.inputLabel.size()>200) return fail("Invalid or duplicate extension command.");
            ids.insert(c.id);m.commands.append(c);
        }
        if(m.commands.isEmpty() || m.commands.size()>32) return fail("Extension requires 1–32 commands.");
        if(out)*out=m;if(error)error->clear();return true;
    }
    if(o.contains("commands") || o.contains("script")) return fail("Music providers cannot include executable scripts.");
    if (permissions != QSet<QString>{"network", "audio-playback", "vault-import"})
        return fail("This protocol requires exactly network, audio-playback and vault-import permissions.");
    const auto net = o.value("network").toObject();
    if (net.size() != 2 || !net.value("allowedHosts").isArray())
        return fail("Invalid network policy.");
    for (auto h : net.value("allowedHosts").toArray()) {
        const auto host = h.toString();
        if (!QRegularExpression("^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\.)+[a-z]{2,63}$")
                 .match(host)
                 .hasMatch() ||
            m.hosts.contains(host) || host.endsWith(".localhost") || host.endsWith(".local") ||
            host.endsWith(".internal") || host.endsWith(".home") || host.endsWith(".lan"))
            return fail("Only exact public HTTPS DNS hosts are supported.");
        m.hosts.append(host);
    }
    if (m.hosts.isEmpty() || m.hosts.size() > 8)
        return fail("Network policy needs 1–8 hosts.");
    m.baseUrl = QUrl(net.value("baseUrl").toString(), QUrl::StrictMode);
    if (!m.allows(m.baseUrl) || m.baseUrl.hasQuery() || m.baseUrl.hasFragment() ||
        (!m.baseUrl.path().isEmpty() && m.baseUrl.path() != "/"))
        return fail("Base URL must be an approved HTTPS origin.");
    if (out)
        *out = m;
    if (error)
        error->clear();
    return true;
}
bool PluginManifest::allows(const QUrl &url) const {
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() && url.port(443) == 443 &&
           !url.hasFragment() && hosts.contains(url.host().toLower());
}
QString pluginArtistNames(const QJsonObject &o) {
    QStringList names;
    for (auto a : o.value("artistNames").toArray())
        if (a.isString() && !a.toString().isEmpty())
            names.append(a.toString());
    if (names.isEmpty())
        for (auto a : o.value("artists").toArray()) {
            const auto ob = a.toObject();
            const auto n = ob.value("name").toString(ob.value("displayName").toString());
            if (!n.isEmpty())
                names.append(n);
        }
    return names.join(", ");
}
bool pluginFlac(const QByteArray &b) {
    return b.size() >= 42 && b.startsWith("fLaC") && (static_cast<unsigned char>(b[4]) & 0x7f) == 0 &&
           static_cast<unsigned char>(b[5]) == 0 && static_cast<unsigned char>(b[6]) == 0 &&
           static_cast<unsigned char>(b[7]) == 34;
}
PluginHost::PluginHost(QString directory, QObject *parent)
    : QObject(parent), directory_(std::move(directory)) {
    pluginResources();
    if (directory_.isEmpty())
        directory_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/plugins";
    PluginManifest m;
    QString e;
    for(const auto& path : {":/plugins/monochrome.json", ":/plugins/text-tools.json"}) {
        QFile builtin(path);
        if (builtin.open(QIODevice::ReadOnly) && PluginManifest::parse(builtin.readAll(), &m, &e)) { m.builtin=true;manifests_.append(m); }
    }
    QDir dir(directory_);
    for (const auto &name : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
        if (manifests_.size() >= 32)
            break;
        QFile f(dir.filePath(name));
        if (f.size() > manifestLimit || !f.open(QIODevice::ReadOnly))
            continue;
        if (!PluginManifest::parse(f.read(manifestLimit+1), &m, &e))
            continue;
        bool duplicate = false;
        for (auto existing : manifests_)
            duplicate |= existing.id == m.id;
        if (!duplicate)
            manifests_.append(m);
    }
}
QString PluginHost::settingKey(const QString &id, const QString &vault) const {
    return "plugins/" + id + (vault.isEmpty() ? QString() : "/vaults/" + vault) + "/";
}
bool PluginHost::enabled(const QString &id) const {
    return QSettings().value(settingKey(id) + "enabled", false).toBool();
}
bool PluginHost::setEnabled(const QString &id, bool value) {
    for (auto m : manifests_)
        if (m.id == id) {
            QSettings().setValue(settingKey(id) + "enabled", value);
            emit changed(id);
            return true;
        }
    return false;
}
bool PluginHost::install(const QByteArray &bytes, QString *error) {
    PluginManifest m;
    if (!PluginManifest::parse(bytes, &m, error))
        return false;
    if (manifests_.size() >= 32) {
        if (error)
            *error = "Plugin limit reached (32).";
        return false;
    }
    for (auto p : manifests_)
        if (p.id == m.id) {
            if (error)
                *error = "That plugin ID is already installed; built-in plugins cannot be replaced.";
            return false;
        }
    if (!QDir().mkpath(directory_)) {
        if (error)
            *error = "Cannot create plugin directory.";
        return false;
    }
    QSaveFile file(QDir(directory_).filePath(m.id + ".json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = "Cannot store manifest.";
        return false;
    }
    manifests_.append(m);
    QSettings().setValue(settingKey(m.id) + "enabled", false);
    emit changed(m.id);
    return true;
}
bool PluginHost::remove(const QString& id, QString* error) {
    for(int i=0;i<manifests_.size();++i) if(manifests_[i].id==id) {
        if(manifests_[i].builtin){if(error)*error="Built-in extensions can be disabled, not removed.";return false;}
        if(!QFile::remove(QDir(directory_).filePath(id+".json"))){if(error)*error="Could not remove installed manifest.";return false;}
        manifests_.removeAt(i);QSettings().remove(settingKey(id));emit changed(id);return true;
    }
    if(error)*error="Extension is not installed.";return false;
}
QWidget *PluginHost::manager(const std::shared_ptr<core::Vault> &vault, QWidget *parent) {
    auto *page = new QWidget(parent);
    page->setObjectName("pluginManager");
    // The section is laid out inside a 480px-wide settings window. Nothing here
    // may impose a horizontal minimum wider than that: labels wrap and the
    // compact controls below are shrinkable, so let the host width win.
    page->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *description = new QLabel(
        "Extensions start disabled. Local tools run in an isolated, bounded JavaScript sandbox without network, filesystem or vault access. Music providers use only their declared host permissions. No native libraries are loaded; no input, search or listening history is saved.",
        page);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *list = new QWidget(page);
    auto *rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(list);
    auto rebuild = [this, rows, vault, list] {
        while (auto *item = rows->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (const auto &m : manifests_) {
            auto *panel = new QWidget(list);
            auto *box = new QVBoxLayout(panel);
            auto *top = new QHBoxLayout;
            auto *title = new QLabel(m.name + " · " + m.version, panel);
            title->setTextFormat(Qt::PlainText);
            top->addWidget(title, 1);
            auto *enable = new Switch(panel);
            enable->setObjectName("pluginEnable_" + m.id);
            enable->setChecked(enabled(m.id));
            top->addWidget(enable);
            box->addLayout(top);
            const auto permissionNotice = m.protocol=="monochrome-tracks-v1" ? "Permissions: network (" + m.hosts.join(", ") + "), audio playback, encrypted vault import." : QString("Host permissions: none. Commands run locally inside the sandbox.");
            auto *notice = new QLabel(m.description + "\n" + m.sourceNotice + "\n" + permissionNotice,
                                      panel);
            notice->setTextFormat(Qt::PlainText);
            notice->setWordWrap(true);
            box->addWidget(notice);
            auto *options = new QWidget(panel);
            auto *opts = new QVBoxLayout(options);
            opts->setContentsMargins(0, 4, 0, 4);
            for(const auto& setting:m.settings) {
                auto* row=new QHBoxLayout;auto* label=new QLabel(setting.title,options);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);row->addWidget(label,1);auto* toggle=new Switch(options);toggle->setAccessibleName(setting.title);toggle->setObjectName("pluginPreference_"+m.id+"_"+setting.id);const auto key=settingKey(m.id)+"preferences/"+setting.id;toggle->setChecked(QSettings().value(key,setting.defaultValue).toBool());row->addWidget(toggle);opts->addLayout(row);connect(toggle,&QCheckBox::toggled,this,[this,key,id=m.id](bool checked){QSettings().setValue(key,checked);emit changed(id);});
            }
            if(m.protocol=="monochrome-tracks-v1") {
            const auto key = settingKey(m.id);
            const auto vk = settingKey(m.id, identity(vault));
            auto *autoRow = new QHBoxLayout;
            auto *autoLabel = new QLabel("Save automatically after playback starts", options);
            autoLabel->setTextFormat(Qt::PlainText);
            autoLabel->setWordWrap(true);
            autoRow->addWidget(autoLabel, 1);
            auto *autosave = new Switch(options);
            autosave->setAccessibleName("Save music automatically after playback starts");
            autosave->setObjectName("pluginAutoSave_" + m.id);
            autosave->setChecked(QSettings().value(key + "autoSave", false).toBool());
            autoRow->addWidget(autosave);
            opts->addLayout(autoRow);
            connect(autosave, &QCheckBox::toggled, this,
                    [key](bool b) { QSettings().setValue(key + "autoSave", b); });
            auto *folder = new QComboBox(options);
            folder->setAccessibleName("Encrypted vault destination folder");
            folder->setObjectName("pluginFolder_" + m.id);
            folder->addItem("Library root", qlonglong(0));
            if (vault && vault->is_unlocked()) {
                auto folders = vault->list_folders();
                if (folders)
                    for (const auto &f : folders.value())
                        folder->addItem(QString::fromStdString(f.name) + " · #" + QString::number(f.id),
                                        qlonglong(f.id));
            }
            int idx = folder->findData(QSettings().value(vk + "folder", qlonglong(0)));
            folder->setCurrentIndex(idx < 0 ? 0 : idx);
            auto *destinationLabel = new QLabel("Encrypted destination (this vault only)", options);
            destinationLabel->setTextFormat(Qt::PlainText);
            destinationLabel->setWordWrap(true);
            opts->addWidget(destinationLabel);
            opts->addWidget(folder);
            connect(folder, qOverload<int>(&QComboBox::currentIndexChanged), this, [folder, vk](int i) {
                if (i >= 0)
                    QSettings().setValue(vk + "folder", folder->itemData(i));
            });
            auto *caps = new QHBoxLayout;
            auto *capLabel = new QLabel("Audio memory limit", options);
            capLabel->setTextFormat(Qt::PlainText);
            capLabel->setWordWrap(true);
            caps->addWidget(capLabel, 1);
            // One combo instead of three buttons: the same three choices fit a
            // narrow window without pushing the whole section wider.
            auto *capsBox = new QComboBox(options);
            capsBox->setAccessibleName("Audio memory limit");
            capsBox->setObjectName("pluginMemoryLimit_" + m.id);
            for (int cap : {32, 64, 128})
                capsBox->addItem(QString::number(cap) + " MiB", cap);
            const int capIndex = capsBox->findData(QSettings().value(key + "memoryMiB", 32).toInt());
            capsBox->setCurrentIndex(capIndex < 0 ? 0 : capIndex);
            caps->addWidget(capsBox);
            connect(capsBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, capsBox, key, id = m.id](int index) {
                        if (index >= 0) {
                            QSettings().setValue(key + "memoryMiB", capsBox->itemData(index));
                            emit changed(id);
                        }
                    });
            opts->addLayout(caps);
            } else {
                auto* explanation=new QLabel("Fresh runtime per command · bounded memory and execution time · plain-text results only",options);
                explanation->setWordWrap(true);opts->addWidget(explanation);
            }
            box->addWidget(options);
            auto* open=new QPushButton(m.protocol=="monochrome-tracks-v1"?"Open music":"Open tools",panel);
            open->setObjectName("pluginOpen_"+m.id);open->setEnabled(enabled(m.id));box->addWidget(open,0,Qt::AlignLeft);
            connect(open,&QPushButton::clicked,this,[this,id=m.id]{if(enabled(id))emit openRequested(id);});
            options->setEnabled(enabled(m.id));
            connect(enable, &QCheckBox::toggled, this, [this, id = m.id, options, open](bool on) {
                options->setEnabled(on);
                open->setEnabled(on);
                setEnabled(id, on);
            });
            if(!m.builtin) {
                auto* removeButton=new QPushButton("Remove extension",panel);removeButton->setObjectName("pluginRemove_"+m.id);box->addWidget(removeButton,0,Qt::AlignLeft);
                connect(removeButton,&QPushButton::clicked,panel,[this,id=m.id,panel,notice]{QString error;if(remove(id,&error))panel->deleteLater();else notice->setText(error);});
            }
            rows->addWidget(panel);
        }
    };
    rebuild();
    auto *status = new QLabel(page);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *installButton = new QPushButton("Install extension manifest…", page);
    installButton->setObjectName("installPlugin");
    layout->addWidget(installButton, 0, Qt::AlignLeft);
    connect(installButton, &QPushButton::clicked, page, [this, page, status, rebuild] {
        const auto path = QFileDialog::getOpenFileName(page, "Install extension", {},
                                                       "Extension manifest (*.json)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (f.size() > manifestLimit || !f.open(QIODevice::ReadOnly)) {
            status->setText("Cannot read manifest (maximum 512 KiB).");
            return;
        }
        QString e;
        if (!install(f.read(manifestLimit+1), &e)) {
            status->setText(e);
            return;
        }
        status->setText("Extension installed disabled. Review its source and permissions before enabling.");
        rebuild();
    });
    return page;
}
MusicPluginPage::MusicPluginPage(PluginHost *host, QWidget *parent)
    : QWidget(parent), host_(host), network_(new QNetworkAccessManager(this)),
      player_(new PluginAudioPlayer(this)), buffer_(new QBuffer(this)) {
    setObjectName("musicPluginPage");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    auto *top = new QHBoxLayout;
    auto *back = new QPushButton("‹ Library", this);
    back->setObjectName("musicBack");
    back_ = back;
    top->addWidget(back);
    auto *title = new QLabel("Music", this);
    auto font = title->font();
    font.setPointSize(17);
    font.setBold(true);
    title->setFont(font);
    title_ = title;
    top->addWidget(title, 1);
    providers_ = new QComboBox(this);
    providers_->setObjectName("musicProvider");
    top->addWidget(providers_);
    layout->addLayout(top);
    connect(back, &QPushButton::clicked, this, &MusicPluginPage::closed);
    notice_ = new QLabel(this);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    auto *searchRow = new QHBoxLayout;
    query_ = new QLineEdit(this);
    query_->setObjectName("musicSearch");
    query_->setPlaceholderText("Search Monochrome songs, artists and albums");
    query_->setMaxLength(200);
    auto *searchButton = new QPushButton("Search", this);
    searchButton->setObjectName("musicSearchButton");
    searchRow->addWidget(query_, 1);
    searchRow->addWidget(searchButton);
    layout->addLayout(searchRow);
    connect(searchButton, &QPushButton::clicked, this, &MusicPluginPage::search);
    connect(query_, &QLineEdit::returnPressed, this, &MusicPluginPage::search);
    auto *filters = new QHBoxLayout;
    auto *group = new QButtonGroup(this);
    int n = 0;
    for (const auto &name : QStringList{"All", "Songs", "Artists", "Albums"}) {
        const int value = n++;
        auto *b = new QPushButton(name, this);
        b->setCheckable(true);
        b->setChecked(value == 0);
        group->addButton(b);
        filters->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, value] {
            filter_ = value;
            applyFilter();
        });
    }
    filters->addStretch();
    layout->addLayout(filters);
    results_ = new QListWidget(this);
    results_->setObjectName("musicResults");
    results_->setIconSize(QSize(42, 42));
    results_->setUniformItemSizes(true);
    layout->addWidget(results_, 1);
    connect(results_, &QListWidget::itemActivated, this, &MusicPluginPage::activate);
    connect(results_, &QListWidget::itemClicked, this, &MusicPluginPage::activate);
    nowPlaying_ =
        new QLabel("Select a song to play. Selecting an album or artist opens its catalogue.", this);
    nowPlaying_->setWordWrap(true);
    nowPlaying_->setTextFormat(Qt::PlainText);
    layout->addWidget(nowPlaying_);
    auto *controls = new QHBoxLayout;
    play_ = new QPushButton("Play", this);
    play_->setObjectName("pluginPlay");
    play_->setEnabled(false);
    controls->addWidget(play_);
    auto *stop = new QPushButton("Stop", this);
    stop->setObjectName("pluginStop");
    controls->addWidget(stop);
    seek_ = new QSlider(Qt::Horizontal, this);
    seek_->setObjectName("pluginSeek");
    seek_->setAccessibleName("Playback position");
    controls->addWidget(seek_, 1);
    download_ = new QPushButton("Download to vault", this);
    download_->setObjectName("pluginDownload");
    download_->setEnabled(false);
    controls->addWidget(download_);
    layout->addLayout(controls);
    status_ = new QLabel("Search to browse. Audio stays in memory unless you explicitly save it.", this);
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setObjectName("musicStatus");
    layout->addWidget(status_);
    connect(play_, &QPushButton::clicked, this, [this] {
        if (player_->state() == QMediaPlayer::PlayingState)
            player_->pause();
        else if (buffer_->isOpen())
            player_->play();
    });
    connect(stop, &QPushButton::clicked, this, &MusicPluginPage::stopAndClear);
    connect(download_, &QPushButton::clicked, this, &MusicPluginPage::saveAudio);
    connect(seek_, &QSlider::sliderMoved, this, [this](int pos) { player_->setPosition(pos); });
    connect(player_, &PluginAudioPlayer::durationChanged, this,
            [this](qint64 d) { seek_->setRange(0, int(std::min<qint64>(d, 2147483647))); });
    connect(player_, &PluginAudioPlayer::positionChanged, this, [this](qint64 p) {
        if (!seek_->isSliderDown())
            seek_->setValue(int(p));
        if (p > 0 && player_->state() == QMediaPlayer::PlayingState && autoSave_ && !autoSaveAttempted_ && !saved_ && !importBusy_) {
            autoSaveAttempted_ = true;
            saveAudio();
        }
    });
    connect(player_, &PluginAudioPlayer::stateChanged, this, [this](QMediaPlayer::State s) {
        play_->setText(s == QMediaPlayer::PlayingState ? "Pause" : "Play");
    });
    connect(player_, qOverload<QMediaPlayer::Error>(&PluginAudioPlayer::error), this,
            [this](QMediaPlayer::Error e) {
                if (e != QMediaPlayer::NoError) {
                    autoSave_ = false;
                    status_->setText("Playback backend could not decode the in-memory FLAC: " +
                                     player_->errorString() + ". No automatic save was performed.");
                }
            });
    connect(host_, &PluginHost::changed, this, [this](const QString& id) {
        const auto current = providers_->currentData().toString();
        if (current == id || !host_->enabled(current)) stopAndClear();
        refreshProviders();
    });
    connect(providers_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        stopAndClear();
        notice_->setText(manifest().sourceNotice);
    });
    refreshProviders();
}
MusicPluginPage::~MusicPluginPage() { stopAndClear(); }
void MusicPluginPage::setEmbedded(const bool embedded) {
    if (embedded_ == embedded)
        return;
    embedded_ = embedded;
    // Embedded in the library folder the page has no chrome of its own: the
    // breadcrumb already says where the user is and the library closes it.
    if (back_ != nullptr)
        back_->setVisible(!embedded);
    if (title_ != nullptr)
        title_->setVisible(!embedded);
    if (auto *layout = qobject_cast<QVBoxLayout *>(this->layout())) {
        layout->setContentsMargins(embedded ? 12 : 20, embedded ? 8 : 16,
                                   embedded ? 12 : 20, embedded ? 8 : 16);
    }
}
QString MusicPluginPage::vaultIdentity() const { return identity(vault_); }
PluginManifest MusicPluginPage::manifest() const {
    for (const auto &m : host_->manifests())
        if (m.protocol=="monochrome-tracks-v1" && m.id == providers_->currentData().toString() && host_->enabled(m.id))
            return m;
    return {};
}
void MusicPluginPage::refreshProviders() {
    const auto id = providers_->currentData();
    QSignalBlocker blocker(providers_);
    providers_->clear();
    for (auto m : host_->manifests())
        if (m.protocol=="monochrome-tracks-v1" && host_->enabled(m.id))
            providers_->addItem(m.name, m.id);
    int idx = providers_->findData(id);
    if (idx >= 0)
        providers_->setCurrentIndex(idx);
    query_->setEnabled(providers_->count() > 0);
    notice_->setText(manifest().sourceNotice);
}
void MusicPluginPage::setVault(std::shared_ptr<core::Vault> vault) {
    if (vault_ != vault) {
        stopAndClear();
        vault_ = std::move(vault);
    }
    refreshProviders();
}
void MusicPluginPage::stopAndClear() { clearPlayback(true); }
void MusicPluginPage::clearPlayback(bool clearCatalogue) {
    ++generation_;
    if (cancellation_)
        cancellation_->store(true);
    const auto replies = replies_;
    for (auto *r : replies) {
        r->disconnect(this);
        r->abort();
        if (requestBuffers_.contains(r))
            wipe(*requestBuffers_.value(r));
        r->deleteLater();
    }
    replies_.clear();
    requestBuffers_.clear();
    artwork_.clear();
    thumbnailBusy_ = false;
    player_->stop();
    player_->setMedia(QMediaContent());
    buffer_->close();
    wipe(buffer_->buffer());
    if (clearCatalogue) {
        results_->clear();
        query_->clear();
    }
    seek_->setValue(0);
    play_->setEnabled(false);
    download_->setEnabled(false);
    pinnedVault_.reset();
    pinnedPlugin_.clear();
    pinnedName_.clear();
    autoSave_ = false;
    saved_ = false;
    autoSaveAttempted_ = false;
    importBusy_ = false;
    nowPlaying_->setText("Select a song to play.");
    status_->setText("Stopped. In-memory audio and results cleared.");
}
void MusicPluginPage::request(QUrl url, qint64 limit, std::function<void(QByteArray)> done, int redirects) {
    const auto m = manifest();
    if (!vault_ || !vault_->is_unlocked() || !m.allows(url) || redirects > 4) {
        status_->setText("Request blocked: locked vault or unapproved HTTPS host/redirect.");
        return;
    }
    const auto generation = generation_;
    QHostInfo::lookupHost(
        url.host(), this, [this, url, limit, done, redirects, generation](const QHostInfo &info) {
            if (generation != generation_ || !vault_ || !vault_->is_unlocked())
                return;
            bool publicOnly = info.error() == QHostInfo::NoError && !info.addresses().isEmpty();
            for (const auto &address : info.addresses()) {
                bool ok = address.isGlobal();
                if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                    for (const auto *subnet :
                         {"0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8", "169.254.0.0/16",
                          "172.16.0.0/12", "192.0.0.0/24", "192.0.2.0/24", "192.168.0.0/16", "198.18.0.0/15",
                          "198.51.100.0/24", "203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4"})
                        if (address.isInSubnet(QHostAddress::parseSubnet(QString::fromLatin1(subnet))))
                            ok = false;
                } else {
                    ok = ok && address.isInSubnet(QHostAddress::parseSubnet("2000::/3")) &&
                         !address.isInSubnet(QHostAddress::parseSubnet("2001::/23")) &&
                         !address.isInSubnet(QHostAddress::parseSubnet("2002::/16"));
                }
                publicOnly = publicOnly && ok;
            }
            if (!publicOnly) {
                status_->setText(
                    "Provider DNS rejected: destination must resolve only to public Internet addresses.");
                return;
            }
#ifdef Q_OS_WIN
            QPointer<MusicPluginPage> self(this);
            auto deliver = [self, url, limit, done, redirects, generation](
                                     QByteArray body, int status, QUrl location, QString error) mutable {
                if (!self)
                    return;
                self->completeRequest(url, std::move(body), status, location, error, limit,
                                      done, redirects, generation);
            };
            QtConcurrent::run([self, url, limit, deliver, generation]() mutable {
                if (!self)
                    return;
                QByteArray body;
                int status = 0;
                QUrl location;
                QString error;
                const auto progress = [self, generation, limit](qint64 received, qint64 total) {
                    if (!self)
                        return;
                    QMetaObject::invokeMethod(
                        self,
                        [self, generation, limit, received, total] {
                            if (!self || generation != self->generation_ || limit <= metadataLimit)
                                return;
                            self->status_->setText(
                                QString("Loading audio: %1 MiB%2")
                                    .arg(double(received) / (1024 * 1024), 0, 'f', 1)
                                    .arg(total > 0 ? QString(" / %1 MiB")
                                                         .arg(double(total) / (1024 * 1024), 0, 'f', 1)
                                                   : QString()));
                        },
                        Qt::QueuedConnection);
                };
                const bool ok = winHttpGet(url, limit, progress, &body, &status, &location, &error);
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self,
                    [deliver, body = std::move(body), status, location,
                     error = ok ? QString() : error]() mutable {
                        deliver(std::move(body), status, location, error);
                    },
                    Qt::QueuedConnection);
            });
            return;
#endif
            QNetworkRequest req(url);
            req.setRawHeader("User-Agent", "MegaVaultProtect/2.0.3 (music plugin)");
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
            req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
            auto *reply = network_->get(req);
            replies_.insert(reply);
            reply->setReadBufferSize(std::min<qint64>(limit + 1, 65536));
            auto bytes = std::make_shared<QByteArray>();
            auto exceeded = std::make_shared<bool>(false);
            auto *timer = new QTimer(reply);
            timer->setSingleShot(true);
            timer->start(limit > metadataLimit ? 120000 : 30000);
            connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
            requestBuffers_.insert(reply, bytes);
            connect(
                reply, &QNetworkReply::downloadProgress, this,
                [this, limit, generation](qint64 received, qint64 total) {
                    if (generation == generation_ && limit > metadataLimit)
                        status_->setText(
                            QString("Loading audio: %1 MiB%2")
                                .arg(double(received) / (1024 * 1024), 0, 'f', 1)
                                .arg(total > 0
                                         ? QString(" / %1 MiB").arg(double(total) / (1024 * 1024), 0, 'f', 1)
                                         : QString()));
                });
            connect(reply, &QNetworkReply::metaDataChanged, this, [reply, limit, exceeded] {
                const auto length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
                if (length > limit) {
                    *exceeded = true;
                    reply->abort();
                }
            });
            connect(reply, &QNetworkReply::readyRead, this, [reply, bytes, limit, exceeded] {
                const qint64 remaining = limit - bytes->size();
                if (reply->bytesAvailable() > remaining) {
                    *exceeded = true;
                    reply->abort();
                    return;
                }
                bytes->append(reply->read(remaining));
            });
            connect(reply, &QNetworkReply::finished, this,
                    [this, reply, bytes, exceeded, limit, done, generation, url, redirects] {
                        replies_.remove(reply);
                        requestBuffers_.remove(reply);
                        reply->deleteLater();
                        if (generation != generation_ || !vault_ || !vault_->is_unlocked()) {
                            wipe(*bytes);
                            return;
                        }
                        bytes->append(reply->readAll());
                        completeRequest(
                            url, std::move(*bytes),
                            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                            reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl(),
                            reply->error() == QNetworkReply::NoError ? QString()
                                                                     : reply->errorString(),
                            limit, done, redirects, generation);
                    });
        });
}
void MusicPluginPage::completeRequest(const QUrl &url, QByteArray bytes, const int code,
                                      const QUrl &redirect, const QString &error, const qint64 limit,
                                      const std::function<void(QByteArray)> &done, const int redirects,
                                      const quint64 generation) {
    if (generation != generation_ || !vault_ || !vault_->is_unlocked()) {
        wipe(bytes);
        return;
    }
    if (code >= 300 && code < 400 && redirect.isValid() && !redirect.isEmpty()) {
        wipe(bytes);
        request(url.resolved(redirect), limit, done, redirects + 1);
        return;
    }
    if (!error.isEmpty() || code < 200 || code >= 300) {
        wipe(bytes);
        status_->setText("Provider request failed (HTTP " + QString::number(code) + "): " +
                         (error.isEmpty() ? QStringLiteral("no response") : error));
        if (thumbnailBusy_) {
            thumbnailBusy_ = false;
            thumbnailNext();
        }
        return;
    }
    if (bytes.size() > limit) {
        wipe(bytes);
        status_->setText("Response exceeds the configured memory limit. Choose 64 or 128 "
                         "MiB in Extensions for large FLAC tracks.");
        return;
    }
    done(std::move(bytes));
}
void MusicPluginPage::searchFor(const QString &term) {
    if (query_ == nullptr) {
        return;
    }
    query_->setText(term);
    refreshProviders();
    search();
}

void MusicPluginPage::search() {
    const auto q = query_->text().trimmed();
    if (q.isEmpty() || manifest().id.isEmpty())
        return;
    stopAndClear();
    query_->setText(q);
    query_->setCursorPosition(0);
    status_->setText("Searching Monochrome…");
    auto u = manifest().baseUrl;
    u.setPath("/search");
    u.setQuery("q=" + QString::fromLatin1(QUrl::toPercentEncoding(q)));
    request(u, metadataLimit, [this](QByteArray b) {
        QJsonParseError e;
        auto doc = QJsonDocument::fromJson(b, &e);
        if (e.error != QJsonParseError::NoError || !doc.isObject()) {
            status_->setText("Provider returned invalid catalogue JSON.");
            return;
        }
        showResults(doc.object());
    });
}
void MusicPluginPage::showResults(const QJsonObject &o, bool detail) {
    results_->clear();
    artwork_.clear();
    auto append = [this](const QJsonArray &arr, int kind) {
        for (auto value : arr) {
            if (results_->count() >= 120)
                break;
            auto item = value.toObject();
            const auto id = item.value(kind == 1   ? "trackId"
                                       : kind == 2 ? "artistId"
                                                   : "releaseId")
                                .toString(item.value("id").toString());
            if (!validId(id))
                continue;
            item["_id"] = id;
            item["_kind"] = kind;
            QString name = kind == 2 ? item.value("name").toString(item.value("displayName").toString())
                                     : item.value("title").toString();
            if (name.isEmpty())
                name = "Untitled";
            const QString type = kind == 1 ? "Song" : kind == 2 ? "Artist" : "Album";
            const auto artists = pluginArtistNames(item);
            auto *row = new QListWidgetItem(
                name + (artists.isEmpty() ? QString() : " — " + artists) + "  ·  " + type, results_);
            row->setSizeHint(QSize(100, 52));
            row->setData(Qt::UserRole, item);
            if (kind == 1 && item.value("playable") == QJsonValue(false)) {
                row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
                row->setToolTip("Not available from this provider");
            }
            const auto image = item.value(kind == 2 ? "avatar" : "artwork").toString();
            QUrl art(image);
            if (manifest().allows(art))
                artwork_.append({art, results_->count() - 1});
        }
    };
    append(o.value("tracks").toArray(), 1);
    append(o.value("topTracks").toArray(), 1);
    append(o.value("artists").toArray(), 2);
    append(o.value("releases").toArray(), 3);
    for (auto key : {"albums", "singles", "compilations"})
        append(o.value(key).toArray(), 3);
    applyFilter();
    status_->setText(results_->count() ? QString::number(results_->count()) +
                                             " results · select a song to play or an album/artist to explore."
                                       : "No results from this provider.");
    if (detail && o.value("topTracks").toArray().isEmpty() && results_->count())
        status_->setText("Choose an album below to explore its tracks.");
    thumbnailNext();
}
void MusicPluginPage::applyFilter() {
    for (int i = 0; i < results_->count(); ++i) {
        auto *r = results_->item(i);
        r->setHidden(filter_ != 0 && r->data(Qt::UserRole).toJsonObject().value("_kind").toInt() != filter_);
    }
}
void MusicPluginPage::thumbnailNext() {
    if (thumbnailBusy_ || artwork_.isEmpty())
        return;
    thumbnailBusy_ = true;
    auto next = artwork_.takeFirst();
    request(next.first, metadataLimit, [this, index = next.second](QByteArray b) {
        QBuffer source(&b);
        source.open(QIODevice::ReadOnly);
        QImageReader reader(&source);
        const auto size = reader.size();
        if (size.isValid() && size.width() <= 4096 && size.height() <= 4096) {
            reader.setScaledSize(QSize(42, 42));
            const auto img = reader.read();
            if (index < results_->count() && !img.isNull())
                results_->item(index)->setIcon(QPixmap::fromImage(img));
        }
        thumbnailBusy_ = false;
        thumbnailNext();
    });
}
void MusicPluginPage::activate(QListWidgetItem *row) {
    if (!row || !vault_ || !vault_->is_unlocked())
        return;
    const auto obj = row->data(Qt::UserRole).toJsonObject();
    const int kind = obj.value("_kind").toInt();
    const auto id = obj.value("_id").toString();
    if (!validId(id))
        return;
    if (kind != 1) {
        const auto m = manifest();
        stopAndClear();
        auto u = m.baseUrl;
        u.setPath((kind == 2 ? "/artists/" : "/releases/") + id);
        status_->setText("Opening catalogue…");
        request(u, metadataLimit, [this](QByteArray b) {
            QJsonParseError e;
            const auto doc = QJsonDocument::fromJson(b, &e);
            if (e.error != QJsonParseError::NoError || !doc.isObject()) {
                status_->setText("Invalid detail response.");
                return;
            }
            filter_ = 0;
            showResults(doc.object(), true);
        });
        return;
    }
    const auto m = manifest();
    const auto name = obj.value("title").toString("Track");
    clearPlayback(false);
    pinnedVault_ = vault_;
    pinnedPlugin_ = m.id;
    auto key = host_->settingKey(m.id);
    auto vk = host_->settingKey(m.id, vaultIdentity());
    pinnedFolder_ = QSettings().value(vk + "folder", qlonglong(0)).toLongLong();
    autoSave_ = QSettings().value(key + "autoSave", false).toBool();
    int cap = QSettings().value(key + "memoryMiB", 32).toInt();
    if (cap != 32 && cap != 64 && cap != 128)
        cap = 32;
    QString safe = name;
    safe.replace(QRegularExpression("[\\\\/:*?\"<>|\\x00-\\x1f]"), "_");
    safe = safe.left(110).trimmed();
    if (safe.isEmpty())
        safe = "Track";
    pinnedName_ = safe + ".flac";
    nowPlaying_->setText(name + " — " + pluginArtistNames(obj) + " · Monochrome");
    status_->setText("Loading verified FLAC into memory…");
    auto u = m.baseUrl;
    u.setPath("/track/" + id);
    request(u, qint64(cap) * 1024 * 1024, [this](QByteArray b) {
        if (!pluginFlac(b)) {
            wipe(b);
            status_->setText(
                "Provider did not return a valid FLAC stream header. Playback and save rejected.");
            return;
        }
        buffer_->setData(std::move(b));
        buffer_->open(QIODevice::ReadOnly);
        download_->setEnabled(true);
        play_->setEnabled(true);
        status_->setText("Audio loaded in memory. Download saves directly to the encrypted vault, not your "
                         "Downloads folder.");
        player_->setMedia(QMediaContent(), buffer_);
        player_->play();
    });
}
void MusicPluginPage::saveAudio() {
    if (importBusy_ || saved_ || !buffer_->isOpen() || buffer_->size() == 0 || !pinnedVault_ ||
        pinnedVault_ != vault_ || !vault_->is_unlocked() || !host_->enabled(pinnedPlugin_))
        return;
    importBusy_ = true;
    download_->setEnabled(false);
    status_->setText("Encrypting audio directly into vault…");
    const auto gen = generation_;
    auto vault = pinnedVault_;
    auto folder = pinnedFolder_;
    auto name = pinnedName_;
    auto token = std::make_shared<std::atomic_bool>(false);
    cancellation_ = token;
    auto bytes = std::make_shared<QByteArray>(buffer_->data());
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, gen] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (gen != generation_)
            return;
        importBusy_ = false;
        saved_ = result.isEmpty();
        download_->setEnabled(!saved_);
        status_->setText(saved_ ? "Saved in encrypted vault (identical content is deduplicated)." : result);
        if (saved_)
            emit imported();
    });
    watcher->setFuture(QtConcurrent::run([vault, folder, name, token, bytes] {
        QString error;
        if (token->load() || !vault->is_unlocked())
            error = "Save cancelled.";
        else {
            auto utf8 = name.toUtf8();
            auto result = vault->import_bytes(
                std::span<const unsigned char>(reinterpret_cast<const unsigned char *>(bytes->constData()),
                                               size_t(bytes->size())),
                std::string_view(utf8.constData(), size_t(utf8.size())), folder);
            if (!result)
                error = QString::fromUtf8(core::user_message(result.error().code).data());
        }
        wipe(*bytes);
        return error;
    }));
}
} // namespace videovault::app
