// Opt-in real-service integration test. Only generated vaults; no audio files.
#include "videovault/app/plugin_audio.hpp"
#include "videovault/app/proxy_settings.hpp"
#include "videovault/app/plugin_host.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
using namespace videovault::app;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::cerr << "FAIL line " << __LINE__ << ": " #x << "\n";                                        \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
bool waitFor(const std::function<bool()> &predicate, int ms = 45000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    return predicate();
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    // Same identity as the app: the proxy store is sealed per QSettings file.
    QCoreApplication::setOrganizationName(QStringLiteral("Mega Vault Protect"));
    QCoreApplication::setApplicationName(QStringLiteral("Mega Vault Protect"));
    // Apply the same saved proxy the shipped app uses, so a live run tests that route.
    initializeProxySettings();
    if (!qEnvironmentVariableIsSet("MVP_PLUGIN_LIVE")) {
        std::cout << "SKIP: set MVP_PLUGIN_LIVE=1 for real provider requests\n";
        return 77;
    }
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
#endif
    QTemporaryDir dir(qEnvironmentVariable("TMPDIR", QDir::tempPath()) + "/MVP-plugin-live-XXXXXX");
    CHECK(dir.isValid());
    app.setOrganizationName("PluginLiveTest");
    app.setApplicationName("GeneratedVaultOnly");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    const auto root = std::filesystem::u8path(dir.path().toUtf8().constData()) / "vault";
    auto result = videovault::core::Vault::create(root, "generated-live-fixture", {8192, 1, 1});
    CHECK(result);
    auto vault = std::make_shared<videovault::core::Vault>(std::move(result.value()));
    PluginHost host(dir.path() + "/plugins");
    CHECK(host.setEnabled("org.megavault.monochrome", true));
    MusicPluginPage page(&host);
    page.setVault(vault);
    page.resize(1000, 720);
    page.show();
    auto *query = page.findChild<QLineEdit *>("musicSearch");
    auto *search = page.findChild<QPushButton *>("musicSearchButton");
    auto *list = page.findChild<QListWidget *>("musicResults");
    auto *status = page.findChild<QLabel *>("musicStatus");
    auto *download = page.findChild<QPushButton *>("pluginDownload");
    auto *player = page.findChild<PluginAudioPlayer *>();
    query->setText("Ludwig van Beethoven");
    search->click();
    CHECK(waitFor([&] { return list->count() > 0 || !status->text().startsWith("Searching"); }));
    std::cout << "Search: " << status->text().toStdString() << "\n";
    CHECK(list->count() > 0);
    CHECK(vault->list_videos().value().empty());
    CHECK(waitFor([&] {
        for (int i = 0; i < list->count(); ++i)
            if (!list->item(i)->icon().isNull())
                return true;
        return false;
    }));
    std::cout << "PASS live HTTPS search + actual in-memory artwork\n";
    for (int kind : {3, 2}) {
        QListWidgetItem *chosen = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toJsonObject().value("_kind").toInt() == kind) {
                chosen = list->item(i);
                break;
            }
        }
        CHECK(chosen);
        list->itemClicked(chosen);
        CHECK(waitFor([&] { return list->count() > 0 || !status->text().startsWith("Opening"); }));
        std::cout << "Detail " << kind << ": " << status->text().toStdString() << "\n";
        CHECK(list->count() > 0);
        CHECK(vault->list_videos().value().empty());
        query->setText("Ludwig van Beethoven");
        search->click();
        CHECK(waitFor([&] { return list->count() > 0 || !status->text().startsWith("Searching"); }));
        CHECK(list->count() > 0);
    }
    // A known live track selected through the real UI signal; this is test input,
    // not a synthesized provider response. Audio comes from the real HTTPS route.
    auto *fixture = new QListWidgetItem("Live short track", list);
    fixture->setData(Qt::UserRole, QJsonObject{{"_id", "282968934603341824"},
                                               {"_kind", 1},
                                               {"title", "Live short track"},
                                               {"artistNames", QJsonArray{"Live fixture"}}});
    list->itemClicked(fixture);
    CHECK(waitFor(
        [&] {
            return download->isEnabled() ||
                   (!status->text().startsWith("Loading") && !status->text().startsWith("Audio loaded"));
        },
        140000));
    std::cout << "Audio: " << status->text().toStdString() << "\n";
    CHECK(download->isEnabled());
    auto *buffer = page.findChild<QBuffer *>();
    CHECK(buffer && buffer->size() > 1000000 && buffer->size() < 32 * 1024 * 1024);
    CHECK(pluginFlac(buffer->data()));
    CHECK(vault->list_videos().value().empty());
    const bool played =
        waitFor([&] { return player->position() > 200 || player->error() != QMediaPlayer::NoError; },
                15000) &&
        player->position() > 200;
    std::cout << "Backend state=" << player->state() << " media=" << player->mediaStatus()
              << " position=" << player->position() << " error=" << player->errorString().toStdString()
              << "\n";
    CHECK(player->mediaStatus() != QMediaPlayer::NoMedia);
    CHECK(played);
    if (played) {
        page.findChild<QPushButton *>("pluginPlay")->click();
        CHECK(player->state() == QMediaPlayer::PausedState);
        page.findChild<QPushButton *>("pluginPlay")->click();
        CHECK(waitFor([&] { return player->state() == QMediaPlayer::PlayingState; }));
        player->setPosition(3000);
        CHECK(waitFor([&] { return player->position() >= 2500; }));
        std::cout << "PASS real QBuffer playback, pause/resume and seek\n";
    } else
        std::cout << "LIMITATION: local Qt multimedia backend did not decode in-memory FLAC; UI reports it "
                     "honestly\n";
    const auto expectedSize = buffer->size();
    download->click();
    CHECK(waitFor(
        [&] { return status->text().startsWith("Saved") || !status->text().startsWith("Encrypting"); }));
    std::cout << "Import: " << status->text().toStdString() << "\n";
    CHECK(status->text().startsWith("Saved"));
    auto videos = vault->list_videos();
    CHECK(videos && videos.value().size() == 1);
    auto bytes = vault->read_video_bytes(videos.value()[0].id);
    CHECK(bytes && qint64(bytes.value().size()) == expectedSize);
    CHECK(std::equal(bytes.value().begin(), bytes.value().end(),
                     reinterpret_cast<const unsigned char *>(buffer->data().constData())));
    std::cout << "PASS live " << expectedSize
              << " bytes -> encrypted vault -> exact authenticated RAM readback\n";
    if (!qEnvironmentVariableIsEmpty("MVP_PLUGIN_SCREENSHOTS")) {
        const auto folder = qEnvironmentVariable("MVP_PLUGIN_SCREENSHOTS");
        QDir().mkpath(folder);
        page.grab().save(folder + "/music-live.png");
        auto *manager = host.manager(vault);
        manager->resize(760, 660);
        manager->show();
        app.processEvents();
        manager->grab().save(folder + "/plugin-manager.png");
        delete manager;
    }
    host.setEnabled("org.megavault.monochrome", false);
    CHECK(buffer->size() == 0 && !download->isEnabled() && list->count() == 0);
    page.setVault({});
    vault->lock();
    CHECK(!download->isEnabled());
    std::cout << "PASS disable/lock clear audio + results and disable save\n";
    return 0;
}
