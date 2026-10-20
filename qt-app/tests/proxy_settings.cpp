#include "videovault/app/proxy_settings.hpp"
#include "videovault/app/sync_client.hpp"
#include "videovault/app/plugin_host.hpp"
#include "videovault/app/app_update.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QSslSocket>
#include <QSslCertificate>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
using namespace videovault::app;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication app(argc, argv); QTemporaryDir dir;
    app.setOrganizationName("ProxyTests"); app.setApplicationName("Isolated");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    try {
        ProxySettings settings; settings.enabled = true; settings.activeId = "fixture";
        settings.entries.append({"fixture", "bad\thost", "fixture-user", "fixture-password", 1080, true});
        check(!validateProxySettings(settings).isEmpty(), "control characters in proxy server must be rejected");
        settings.entries[0].server = "127.0.0.1";
        QString error;
        check(saveProxySettings(settings, &error), error.toUtf8().constData());
        auto loaded = loadProxySettings(&error);
        check(error.isEmpty() && loaded.entries.size() == 1 && loaded.entries[0].password == "fixture-password", "protected proxy roundtrip");
        QFile file(QSettings().fileName()); check(file.open(QIODevice::ReadOnly), "settings file");
        const auto disk = file.readAll();
        check(!disk.contains("fixture-password") && !disk.contains("fixture-user"), "credentials must not be plaintext in preferences");
        check(!disk.contains(QByteArray("fixture-password").toBase64()), "credentials must not merely be base64 encoded");
        file.close();
        auto invalid = settings; invalid.entries[0].port = 65536;
        check(!saveProxySettings(invalid, &error), "invalid port rejected without replacing settings");
        check(loadProxySettings().entries[0].port == 1080, "invalid save leaves protected settings intact");
        QSettings().setValue("network/proxyStore", QByteArray("corrupt"));
        initializeProxySettings(); check(proxyEnabled(), "damaged credential store must fail closed, not direct");
        settings.enabled = false; applyProxySettings(settings);
        check(!proxyEnabled(), "disable restores direct mode");
        if (argc >= 4) {
            settings.enabled = true; settings.entries[0].port = QString::fromLocal8Bit(argv[2]).toInt();
            settings.entries[0].socks5 = QString::fromLocal8Bit(argv[1]) == "socks5";
            applyProxySettings(settings);
            sync::Client client(QUrl(QString("http://proxy-only.invalid:%1").arg(QString::fromLocal8Bit(argv[3]))));
            check(client.send("GET", "/fixture").body == "proxy-origin-ok", "sync request traverses proxy with remote DNS");
            settings.entries[0].port = 1; applyProxySettings(settings);
            bool failed = false;
            try { client.send("GET", "/fixture"); } catch (const sync::Failure&) { failed = true; }
            check(failed, "changing an existing sync manager to a dead proxy must not reuse old route");
            if (argc == 5) {
                settings.entries[0].port = QString::fromLocal8Bit(argv[2]).toInt(); applyProxySettings(settings);
                check(QSslSocket::supportsSsl(), "Qt TLS runtime is required for proxied HTTPS");
                QSslSocket::addDefaultCaCertificates(QSslCertificate::fromPath(QString::fromLocal8Bit(argv[4])));
                QFile manifestFile(":/plugins/monochrome.json"); check(manifestFile.open(QIODevice::ReadOnly), "builtin manifest");
                auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
                manifest["id"] = "org.example.proxy-fixture";
                manifest["network"] = QJsonObject{{"baseUrl", "https://proxy-only.invalid"}, {"allowedHosts", QJsonArray{"proxy-only.invalid"}}};
                PluginHost host(dir.path() + "/plugins");
                check(host.install(QJsonDocument(manifest).toJson(), &error), error.toUtf8().constData());
                check(host.setEnabled("org.example.proxy-fixture", true), "enable fixture");
                const auto root = std::filesystem::path(dir.path().toStdWString()) / "vault";
                auto created = videovault::core::Vault::create(root, "fixture-only", {8192, 1, 1});
                check(bool(created), "create isolated fixture vault");
                auto vault = std::make_shared<videovault::core::Vault>(std::move(created.value()));
                MusicPluginPage page(&host); page.setVault(vault);
                bool ready = false; QEventLoop loop; QString reason;
                QObject::connect(&page, &MusicPluginPage::catalogueReady, &loop, [&](const QString&, const QJsonObject& result) { ready = result.value("fixture").toBool(); loop.quit(); });
                QObject::connect(&page, &MusicPluginPage::catalogueFailed, &loop, [&](const QString&, const QString& failure) { reason = failure; loop.quit(); });
                QTimer::singleShot(18000, &loop, &QEventLoop::quit);
                page.searchFor("proxy fixture"); loop.exec();
                if (!ready) std::cerr << "provider: " << reason.toStdString() << '\n';
                check(ready, "provider HTTPS request must traverse configured proxy, not WinHTTP direct");
                AppRelease release;
                fetchLatestRelease("999999.0.0", &release, &error);
                check(error.isEmpty(), error.toUtf8().constData());
            }
        }
        std::cout << "PASS proxy validation, protected persistence, tamper fail-closed, disable, actual sync fixture transport\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
