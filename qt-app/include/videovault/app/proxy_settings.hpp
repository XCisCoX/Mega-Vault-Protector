#pragma once
#include <QNetworkProxy>
#include <QString>
#include <QVector>
class QNetworkAccessManager;
namespace videovault::app {
struct ProxyEntry {
    QString id, server, username, password;
    int port = 1080;
    bool socks5 = true;
};
struct ProxySettings {
    bool enabled = false;
    QString activeId;
    QVector<ProxyEntry> entries;
};
// Never stores credentials in plaintext. Windows DPAPI, macOS Keychain,
// Linux Secret Service (secret-tool). Missing/locked stores fail closed.
ProxySettings loadProxySettings(QString* error = nullptr);
bool saveProxySettings(const ProxySettings&, QString* error = nullptr);
QString validateProxySettings(const ProxySettings&);
void applyProxySettings(const ProxySettings&);
void initializeProxySettings();
bool proxyEnabled();
void configureNetworkProxy(QNetworkAccessManager*);
}
