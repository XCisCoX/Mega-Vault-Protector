#include "videovault/app/proxy_settings.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QUrl>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#elif defined(Q_OS_MACOS)
#include <Security/Security.h>
#endif
namespace videovault::app {
namespace {
QMutex mutex;
QNetworkProxy current(QNetworkProxy::NoProxy);
qulonglong revision = 1;
QString storeId() {
    return QString::fromLatin1(QCryptographicHash::hash(QSettings().fileName().toUtf8(), QCryptographicHash::Sha256).toHex());
}
QByteArray protect(const QByteArray& input, bool reading, bool* ok) {
    *ok = false;
#ifdef Q_OS_WIN
    DATA_BLOB in{DWORD(input.size()), reinterpret_cast<BYTE*>(const_cast<char*>(input.constData()))}, out{};
    const bool success = reading
        ? CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
        : CryptProtectData(&in, L"MegaVault proxy settings", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!success) return {};
    QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData);
    *ok = true; return result;
#elif defined(Q_OS_MACOS)
    const QByteArray service("MegaVaultProtect.Proxy"), account = storeId().toUtf8();
    void* bytes = nullptr; UInt32 length = 0; SecKeychainItemRef item = nullptr;
    const auto found = SecKeychainFindGenericPassword(nullptr, service.size(), service.constData(), account.size(), account.constData(), &length, &bytes, &item);
    QByteArray result;
    if (reading && found == errSecSuccess) { result = QByteArray(static_cast<char*>(bytes), int(length)); *ok = true; }
    if (bytes) SecKeychainItemFreeContent(nullptr, bytes);
    if (!reading) {
        const auto status = found == errSecSuccess
            ? SecKeychainItemModifyAttributesAndData(item, nullptr, input.size(), input.constData())
            : SecKeychainAddGenericPassword(nullptr, service.size(), service.constData(), account.size(), account.constData(), input.size(), input.constData(), nullptr);
        *ok = status == errSecSuccess;
    }
    if (item) CFRelease(item);
    return reading ? result : QByteArray("keychain");
#else
    QProcess process;
    QStringList args = reading ? QStringList{"lookup"} : QStringList{"store", "--label=MegaVault proxy settings"};
    args << "application" << "MegaVaultProtect.Proxy" << "profile" << storeId();
    process.start("secret-tool", args);
    if (!process.waitForStarted(3000)) return {};
    if (!reading) process.write(input.toBase64() + '\n');
    process.closeWriteChannel();
    if (!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(); return {}; }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
    *ok = true;
    return reading ? QByteArray::fromBase64(process.readAllStandardOutput().trimmed()) : QByteArray("secret-service");
#endif
}
ProxySettings blocked() {
    ProxySettings result; result.enabled = true; result.activeId = "unavailable";
    result.entries.append({"unavailable", "127.0.0.1", {}, {}, 9, true}); return result;
}
}
QString validateProxySettings(const ProxySettings& settings) {
    QSet<QString> ids;
    for (const auto& entry : settings.entries) {
        const QString host = entry.server.trimmed();
        bool invalidCharacter = false;
        for (const auto character : host) if (character.isSpace() || character.category() == QChar::Other_Control) invalidCharacter = true;
        if (entry.id.isEmpty() || ids.contains(entry.id) || host.isEmpty() || invalidCharacter || host != entry.server || host.contains('/') || host.contains('@') || host.contains('?') || host.contains('#') || host.contains(' ') || host.contains('\n') || host.contains('\r') || entry.port < 1 || entry.port > 65535)
            return QStringLiteral("Enter a server name or IP address and a port from 1 to 65535.");
        if (entry.username.toUtf8().size() > 255 || entry.password.toUtf8().size() > 255)
            return QStringLiteral("Proxy credentials must be at most 255 UTF-8 bytes.");
        ids.insert(entry.id);
    }
    if (settings.enabled && !ids.contains(settings.activeId)) return QStringLiteral("Select a saved proxy first.");
    return {};
}
ProxySettings loadProxySettings(QString* error) {
    if (error) error->clear();
    const QByteArray sealed = QSettings().value("network/proxyStore").toByteArray();
    if (sealed.isEmpty()) return {};
    bool ok = false; QByteArray plain = protect(sealed, true, &ok);
    const auto document = QJsonDocument::fromJson(plain); plain.fill(0);
    if (!ok || !document.isObject() || document.object().value("version").toInt() != 1) {
        if (error) *error = QStringLiteral("Proxy settings cannot be unlocked. Networking is blocked. Unlock your OS credential store or save replacement settings.");
        return blocked();
    }
    ProxySettings result; const auto object = document.object();
    result.enabled = object.value("enabled").toBool(); result.activeId = object.value("active").toString();
    for (const auto& value : object.value("entries").toArray()) {
        const auto entry = value.toObject();
        result.entries.append({entry.value("id").toString(), entry.value("server").toString(), entry.value("username").toString(), entry.value("password").toString(), entry.value("port").toInt(), entry.value("socks5").toBool()});
    }
    const QString validation = validateProxySettings(result);
    if (!validation.isEmpty()) { if (error) *error = validation; return blocked(); }
    return result;
}
bool saveProxySettings(const ProxySettings& settings, QString* error) {
    QString failure = validateProxySettings(settings);
    if (!failure.isEmpty()) { if (error) *error = failure; return false; }
    QJsonArray entries;
    for (const auto& e : settings.entries) entries.append(QJsonObject{{"id", e.id}, {"server", e.server}, {"port", e.port}, {"username", e.username}, {"password", e.password}, {"socks5", e.socks5}});
    QByteArray plain = QJsonDocument(QJsonObject{{"version", 1}, {"enabled", settings.enabled}, {"active", settings.activeId}, {"entries", entries}}).toJson(QJsonDocument::Compact);
    bool ok = false; const QByteArray sealed = protect(plain, false, &ok); plain.fill(0);
    if (!ok) { if (error) *error = QStringLiteral("Cannot save securely. Unlock the OS credential store (Linux requires secret-tool and Secret Service). Nothing was applied."); return false; }
    QSettings store; store.setValue("network/proxyStore", sealed); store.sync();
    if (store.status() != QSettings::NoError) { if (error) *error = QStringLiteral("Cannot save proxy settings."); return false; }
    applyProxySettings(settings); if (error) error->clear(); return true;
}
void applyProxySettings(const ProxySettings& settings) {
    QNetworkProxy proxy(QNetworkProxy::NoProxy);
    if (settings.enabled) {
        // Invalid enabled settings are never interpreted as direct access.
        proxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", 9);
        for (const auto& e : settings.entries) if (e.id == settings.activeId) {
            proxy = QNetworkProxy(e.socks5 ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy, e.server, quint16(e.port), e.username, e.password);
            proxy.setCapabilities(proxy.capabilities() | QNetworkProxy::HostNameLookupCapability);
            break;
        }
    }
    QMutexLocker lock(&mutex); current = proxy; ++revision;
    QNetworkProxy::setApplicationProxy(proxy);
}
void initializeProxySettings() { applyProxySettings(loadProxySettings()); }
bool proxyEnabled() { QMutexLocker lock(&mutex); return current.type() != QNetworkProxy::NoProxy; }
void configureNetworkProxy(QNetworkAccessManager* manager) {
    QMutexLocker lock(&mutex);
    if (manager->property("mvpProxyRevision").toULongLong() == revision) return;
    manager->clearAccessCache(); manager->setProxy(current);
    manager->setProperty("mvpProxyRevision", QVariant::fromValue(revision));
}
}
