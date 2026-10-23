// Offscreen regression of the actual MainWindow embed/pop-out/redock routes.
// An absent media row intentionally isolates window policy from device decoding.
#include "videovault/app/main_window.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPointer>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QDebug>
#include <filesystem>
#include <cstdio>
#include <stdexcept>

namespace videovault::app {
struct GalleryPerformanceTestAccess {
    static void check(bool value, const char* reason) {
        if (!value) {
            std::fprintf(stderr, "FAIL: %s\n", reason);
            throw std::runtime_error(reason);
        }
    }
    // How many video players exist as visible top-level windows. The reported
    // "two of them appear" defect means this is >1 after a mode change.
    static int visibleTopLevelPlayers() {
        int count = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (qobject_cast<PlayerWindow*>(widget) != nullptr && widget->isVisible()) {
                ++count;
            }
        }
        return count;
    }
    static void drain(int ms = 60) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < ms) {
            QApplication::processEvents();
            QThread::msleep(2);
        }
    }
    static void run() {
        QTemporaryDir dir(qEnvironmentVariable("TMPDIR", QDir::tempPath())
            + QStringLiteral("/MVP-video-navigation-XXXXXX"));
        check(dir.isValid(), "temporary directory");
        auto created = core::Vault::create(std::filesystem::u8path(dir.path().toUtf8().constData()) / "vault",
            "window-route-fixture", {8192, 1, 1});
        check(bool(created), "fixture vault creation");
        auto vault = std::make_shared<core::Vault>(std::move(created.value()));
        QSettings().setValue(QStringLiteral("updates/checkAutomatically"), false);
        MainWindow main;
        main.vault_ = vault;
        auto* player = new PlayerWindow(vault, -1, QStringLiteral("Window route fixture"));
        player->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<PlayerWindow> guard(player);
        main.playerWindow_ = player;
        QObject::connect(player, &QObject::destroyed, &main, [&main] { main.playerWindow_ = nullptr; });
        QObject::connect(player, &PlayerWindow::detachRequested, &main, [&main] { main.popPlayer(); });
        QObject::connect(player, &PlayerWindow::dockRequested, &main, [&main] { main.embedPlayer(); });
        main.resize(1000, 700);
        main.show();
        drain();
        const auto appState = main.windowState();
        const auto appGeometry = main.geometry();
        main.embedPlayer();
        drain();
        check(main.windowState() == appState && main.geometry() == appGeometry,
            "opening a video must not maximize or resize the app window");
        check(main.contentStack_->currentWidget() == player && player->window() == &main,
            "default video must stay embedded in the main window");
        check(!player->isWindow() && !player->isDetached(), "default video must not open a second window");
        check(visibleTopLevelPlayers() == 0, "embedded playback leaves no duplicate top-level player");
        main.popPlayer();
        drain();
        check(main.contentStack_->currentWidget() == main.pages_,
            "popping out restores the library page instead of leaving the old frame behind");
        check(player->isWindow() && player->isMediaViewer()
            && player->windowState() == Qt::WindowNoState,
            "pop-out must use the independent viewer at a normal size");
        check(main.contentStack_->indexOf(player) < 0 && main.windowState() == appState,
            "pop-out detaches without maximizing or changing main-window state");
        check(visibleTopLevelPlayers() == 1, "pop-out shows exactly one video window");
        main.embedPlayer();
        drain();
        check(!player->isWindow() && !player->isDetached() && !player->isMediaViewer(),
            "redock must return the same instance to embedded mode");
        check(main.contentStack_->currentWidget() == player && player->window() == &main,
            "redock must restore the actual content page");
        check(main.windowState() == appState, "redock keeps the app at its own size");
        check(visibleTopLevelPlayers() == 0, "redock leaves no ghost second video window");
        main.popPlayer();
        drain();
        check(guard && guard.data() == player && player->isMediaViewer(),
            "repeated pop-out reuses one player");
        main.interactiveLock_ = false;
        main.lockVault();
        drain();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!vault->is_unlocked(), "hard lock revokes the video vault");
        check(!guard, "hard lock closes the popped-out viewer");
        qInfo() << "Main-window maximize/embed, pop-out, redock, reuse and hard-lock checks passed";
    }
};
} // namespace videovault::app

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings(qEnvironmentVariable("TMPDIR", QDir::tempPath())
        + QStringLiteral("/MVP-video-navigation-settings-XXXXXX"));
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("MVP-regression"));
    QCoreApplication::setApplicationName(QStringLiteral("video-navigation"));
    try {
        videovault::app::GalleryPerformanceTestAccess::run();
        return 0;
    } catch (const std::exception& error) {
        qCritical() << error.what();
        return 1;
    }
}
