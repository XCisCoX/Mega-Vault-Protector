// Offscreen integration regression of the production PlayerWindow widgets.
// Separate executable (do NOT combine with gallery_performance.cpp).
// Compile with player_window.cpp, media_decoder.cpp, icons.cpp and their normal
// headers/AUTOMOC; link the same Core/FFmpeg/Qt dependencies as the app plus
// telegram_player.qrc. Set QT_QPA_PLATFORM=offscreen in CTest. No audio fixture.
#include "videovault/app/player_window.hpp"
#include "videovault/app/telegram_video_chrome.hpp"
#include "videovault/app/telegram_photo_chrome.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLayout>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QDebug>
#include <filesystem>
#include <cstdio>
#include <cstdlib>

namespace videovault::app {
struct PlayerWindowTestAccess {
    static void check(bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    }
    static void wait(int ms) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < ms) {
            QApplication::processEvents();
            QThread::msleep(2);
        }
    }
    static void mouse(QWidget* target, QEvent::Type kind, QPoint point,
        Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(kind, point, target->mapToGlobal(point), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    }
    static int run() {
        QTemporaryDir dir(qEnvironmentVariable("TMPDIR", QDir::tempPath())
            + QStringLiteral("/MVP-video-chrome-XXXXXX"));
        check(dir.isValid(), "temporary fixture directory");
        auto created = core::Vault::create(std::filesystem::u8path(dir.path().toUtf8().constData()) / "vault",
            "video-chrome-fixture", {8192, 1, 1});
        check(bool(created), "create real encrypted fixture vault");
        auto vault = std::make_shared<core::Vault>(std::move(created.value()));
        PlayerWindow window(vault, -1, QStringLiteral("Video regression"));
        // The deliberately absent item must fail safely, then isolate widget
        // behaviour without synthetic decoded data or an audio device.
        window.quit_.store(true);
        window.worker_.join();
        window.timer_.stop();
        window.openFailed_.store(false);
        window.playing_.store(true);
        window.resize(960, 620);
        window.show();
        window.layout()->activate();
        window.layoutChrome();
        window.showChrome();
        check(window.controlsOverlay_->geometry()
            == telegram_video::controllerRect(window.surface_->size()), "production controller geometry");
        check(window.controlsOverlay_->size() == QSize(480, 72), "source controller size");
        check(window.playButton_->geometry() == QRect(220, 2, 40, 40), "source play position");
        check(window.positionSlider_->y() == 49 && window.positionSlider_->height() == 12, "source seek position");
        check(window.volumeSlider_->width() == 75 && window.muteButton_->x() == 6, "source volume geometry");
        check(window.fullscreenButton_->x() < window.popButton_->x()
            && window.popButton_->x() < window.videoMenuButton_->x(), "right control ordering");
        check(window.repeatButton_->isHidden() && window.shuffleButton_->isHidden(), "overflow-only extra controls");
        window.durationMs_.store(10000);
        window.positionSlider_->setRange(0, 10000);
        auto* slider = window.positionSlider_;
        const QPoint target(slider->width() * 3 / 4, 6);
        mouse(slider, QEvent::MouseButtonPress, target, Qt::LeftButton, Qt::LeftButton);
        check(window.seeking_.load(), "groove click begins seek transaction");
        check(slider->isSliderDown(), "groove click becomes drag rather than page step");
        window.updatePositionLabel();
        check(window.positionLabel_->text() == telegram_video::duration(slider->sliderPosition()), "drag preview not overwritten by playhead");
        mouse(slider, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
        check(!window.seeking_.load(), "seek transaction ends on release");
        check(window.seekTargetMs_.load() == slider->value(), "exact click target reaches real worker request");
        window.uiHideTimer_.stop();
        window.hideChrome();
        wait(telegram_video::hideDuration + 60);
        check(window.controlsOverlay_->isHidden(), "cached fade finishes hidden");
        mouse(window.surface_, QEvent::MouseMove, QPoint(100,100), Qt::NoButton, Qt::NoButton);
        check(!window.controlsOverlay_->isHidden() && !window.playButton_->isHidden(), "movement restores real clickable children");
        const bool before = window.playing_.load();
        window.playButton_->click();
        check(window.playing_.load() != before, "play button remains connected after fade");
        window.playing_.store(true);
        window.hideChrome();
        wait(50);
        window.showChrome();
        check(!window.playButton_->isHidden(), "interrupted fade restores children");
        QWidget library;
        library.resize(720, 480);
        library.show();
        const auto libraryState = library.windowState();
        window.playheadMs_.store(4312);
        window.presentViewer(&library);
        wait(40);
        window.layout()->activate();
        window.layoutChrome();
        check(window.isWindow() && window.parentWidget() == nullptr && window.isMediaViewer(),
            "video is an independent media viewer, not a library page");
        check(!window.isMaximized() && window.windowFlags().testFlag(Qt::FramelessWindowHint),
            "pop-out opens at a normal size, not maximized or fullscreen");
        check(window.surface_->geometry() == window.rect(), "picture fills window without a reserved caption row");
        check(window.windowCaption_->parentWidget() == window.surface_
            && window.windowCaption_->geometry().right() == window.surface_->width() - 1,
            "window controls overlay the top-right picture corner");
        check(window.playheadMs_.load() == 4312, "opening viewer does not restart playback");
        window.toggleFullscreen();
        wait(40);
        check(window.isFullScreen() && library.windowState() == libraryState,
            "fullscreen changes only the viewer, never the library window");
        window.presentViewer(&library);
        check(window.isFullScreen(), "next-item presentation retains fullscreen");
        window.toggleFullscreen();
        wait(40);
        check(!window.isMaximized() && !window.isFullScreen(),
            "exiting fullscreen returns to the normal windowed size");
        window.detach();
        wait(40);
        check(window.isWindow() && !window.isMaximized() && !window.isMediaViewer(),
            "smaller-window action actually exits maximized viewer");
        check(window.playheadMs_.load() == 4312, "window transitions preserve playback clock");
        window.move(30, 30);
        const QPoint start = window.pos();
        const bool playingBeforeDrag = window.playing_.load();
        mouse(window.surface_, QEvent::MouseButtonPress, QPoint(100,100), Qt::LeftButton, Qt::LeftButton);
        mouse(window.surface_, QEvent::MouseMove, QPoint(130,120), Qt::NoButton, Qt::LeftButton);
        mouse(window.surface_, QEvent::MouseButtonRelease, QPoint(130,120), Qt::LeftButton, Qt::NoButton);
        check(window.pos() == start + QPoint(30,20), "drag picture moves the windowed viewer");
        wait(QApplication::doubleClickInterval() + 30);
        check(window.playing_.load() == playingBeforeDrag, "window drag never toggles playback");
        window.presentViewer(&library);
        window.hideChrome();
        check(window.windowCaption_->isHidden(), "idle chrome hides overlaid window buttons");
        window.showChrome();
        check(!window.windowCaption_->isHidden(), "pointer reveal restores window buttons");
        window.enterImageMode();
        window.layoutChrome();
        check(window.positionSlider_->isHidden() && window.videoMenuButton_->isHidden()
            && window.controlsOverlay_->isHidden(), "photo hides the video controller entirely");
        check(window.topOverlay_->isHidden(), "photo has no title bar, like the source viewer");
        const QSize viewport = window.surface_->size();
        check(!window.photoMoreButton_->isHidden() && !window.photoRotateButton_->isHidden(),
            "photo shows the Telegram icon row");
        check(window.photoMoreButton_->geometry()
                == videovault::app::telegram_photo::hoverCircle(
                       videovault::app::telegram_photo::cell(1, viewport)),
            "more sits in the rightmost 46x54 cell");
        check(window.photoRotateButton_->geometry()
                == videovault::app::telegram_photo::hoverCircle(
                       videovault::app::telegram_photo::cell(2, viewport)),
            "rotate is the second cell from the right");
        check(!window.photoName_->isHidden()
                && window.photoName_->geometry().left()
                    == videovault::app::telegram_photo::footerLeft,
            "name footer sits 14px from the left");
        check(window.photoName_->geometry().bottom()
                == viewport.height() - videovault::app::telegram_photo::footerBottom - 1,
            "name footer sits 26px above the bottom");
        check(window.previousButton_->parentWidget() == window.surface_
                && window.nextButton_->parentWidget() == window.surface_,
            "edge arrows live on the picture, not in a card");
        window.uiHideTimer_.stop();
        window.close();
        qInfo() << "Production video chrome geometry, seeking, fade/reveal and photo-isolation checks passed";
        return 0;
    }
};
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MVP-test"));
    QCoreApplication::setApplicationName(QStringLiteral("telegram-video-chrome"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
        qEnvironmentVariable("TMPDIR", QDir::tempPath()));
    return videovault::app::PlayerWindowTestAccess::run();
}
