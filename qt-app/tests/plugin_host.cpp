#include "videovault/app/plugin_host.hpp"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>
using namespace videovault::app;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n';                                        \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    app.setOrganizationName("PluginTest");
    app.setApplicationName("Isolated");
    QFile f(":/plugins/monochrome.json");
    CHECK(f.open(QIODevice::ReadOnly));
    const auto bytes = f.readAll();
    QString error;
    PluginManifest m;
    CHECK(PluginManifest::parse(bytes, &m, &error));
    CHECK(m.id == "org.megavault.monochrome");
    CHECK(m.allows(QUrl("https://tracks.monochrome.st/search?q=x")));
    CHECK(!m.allows(QUrl("http://tracks.monochrome.st/search")));
    CHECK(!m.allows(QUrl("https://tracks.monochrome.st.evil.test/")));
    CHECK(!m.allows(QUrl("https://user:pass@tracks.monochrome.st/")));
    CHECK(!m.allows(QUrl("https://tracks.monochrome.st:444/")));
    auto obj = QJsonDocument::fromJson(bytes).object();
    obj["schemaVersion"] = 2;
    CHECK(!PluginManifest::parse(QJsonDocument(obj).toJson(), &m, &error));
    obj["schemaVersion"] = 1;
    obj["permissions"] = QJsonArray{"network", "execute"};
    CHECK(!PluginManifest::parse(QJsonDocument(obj).toJson(), &m, &error));
    CHECK(!PluginManifest::parse(QByteArray(524289, 'x'), &m, &error));
    auto local = QJsonDocument::fromJson(bytes).object();
    local["network"] = QJsonObject{{"baseUrl", "https://provider.localhost"},
                                   {"allowedHosts", QJsonArray{"provider.localhost"}}};
    CHECK(!PluginManifest::parse(QJsonDocument(local).toJson(), &m, &error));
    CHECK(pluginArtistNames(QJsonObject{{"artists", QJsonArray{QJsonObject{{"name", "Composer"}},
                                                               QJsonObject{{"displayName", "Pianist"}}}}}) ==
          "Composer, Pianist");
    CHECK(pluginArtistNames(QJsonObject{{"artistNames", QJsonArray{"Metadata artist"}}}) ==
          "Metadata artist");
    auto tool = QJsonDocument::fromJson(bytes).object();
    tool["id"] = "org.example.text-tools";
    tool["protocol"] = "megavault-actions-v1";
    tool.remove("network"); tool["permissions"] = QJsonArray{};
    tool["commands"] = QJsonArray{QJsonObject{{"id","statistics"},{"title","Text statistics"},{"inputLabel","Text to analyze"}}};
    tool["script"] = "globalThis.extension={commands:{statistics:input=>({title:'Statistics',text:input.text})}};";
    CHECK(PluginManifest::parse(QJsonDocument(tool).toJson(), &m, &error));
    CHECK(!m.allows(QUrl("https://tracks.monochrome.st/")));
    auto maliciousTool=tool; maliciousTool["permissions"]=QJsonArray{"network"};
    CHECK(!PluginManifest::parse(QJsonDocument(maliciousTool).toJson(),&m,&error));
    PluginHost host(dir.path() + "/plugins");
    const int builtinCount=host.manifests().size();CHECK(builtinCount>=1);
    CHECK(!host.enabled("org.megavault.monochrome"));
    CHECK(!host.install(bytes, &error));
    CHECK(host.setEnabled("org.megavault.monochrome", true));
    CHECK(host.enabled("org.megavault.monochrome"));
    CHECK(host.setEnabled("org.megavault.monochrome", false));
    auto custom = QJsonDocument::fromJson(bytes).object();
    custom["id"] = "org.example.music";
    CHECK(host.install(QJsonDocument(custom).toJson(), &error));
    CHECK(host.manifests().size() == builtinCount+1);
    CHECK(!host.enabled("org.example.music"));
    CHECK(!host.install(QJsonDocument(custom).toJson(), &error));
    CHECK(!host.remove("org.megavault.monochrome",&error));
    CHECK(host.install(QJsonDocument(tool).toJson(),&error));
    CHECK(!host.enabled("org.example.text-tools"));
    CHECK(host.setEnabled("org.example.text-tools",true));
    CHECK(!host.enabled("org.example.music"));
    CHECK(host.remove("org.example.text-tools",&error));
    CHECK(!host.enabled("org.example.text-tools"));
    CHECK(!QFile::exists(dir.path()+"/plugins/org.example.text-tools.json"));
    MusicPluginPage page(&host);
    CHECK(page.findChild<QPushButton *>("pluginDownload"));
    CHECK(!page.findChild<QPushButton *>("pluginDownload")->isEnabled());
    page.stopAndClear();
    std::cout << "PASS manifest safety, install isolation, disabled defaults, player lifecycle\n";
}
