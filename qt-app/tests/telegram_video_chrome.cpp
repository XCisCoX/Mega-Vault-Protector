// Standalone offscreen geometry/policy test; no vault, media worker or audio device.
// Registration: add_executable(VideoVaultTelegramVideoChromeTests tests/telegram_video_chrome.cpp)
// target_include_directories(VideoVaultTelegramVideoChromeTests PRIVATE include)
// target_link_libraries(VideoVaultTelegramVideoChromeTests PRIVATE Qt5::Widgets)
// add_test(NAME telegram_video_chrome COMMAND VideoVaultTelegramVideoChromeTests)
// set_tests_properties(telegram_video_chrome PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
#include "videovault/app/telegram_video_chrome.hpp"
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QWidget>
#include <QDebug>
#include <cstdlib>

using namespace videovault::app;
namespace {
void require(bool condition, const char* message) {
    if (!condition) { qCritical() << message; std::exit(1); }
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace telegram_video;
    require(controllerRect({960, 620}) == QRect(240, 542, 480, 72), "480x72 centred controller, six pixel bottom inset");
    require(controllerRect({480, 320}) == QRect(6, 242, 468, 72), "minimum player width uses six pixel side insets");
    require(duration(-1) == QStringLiteral("0:00"), "negative time clamped");
    require(duration(59999) == QStringLiteral("0:59"), "seconds floor");
    require(duration(3600000) == QStringLiteral("1:00:00"), "hours retained");
    require(remaining(10000, 1500) == QString(QChar(8722)) + QStringLiteral("0:09"), "remaining subtracts whole seconds like upstream");
    require(remaining(10000, 12000) == QString(QChar(8722)) + QStringLiteral("0:00"), "remaining clamps at EOF");
    require(hideDelay == 1100 && hideDuration == 600 && showDuration == 200, "source timing constants");
    for (int viewportWidth : {320, 480, 640, 960, 1920}) {
        QWidget stage;
        stage.resize(viewportWidth, 620);
        QWidget panel(&stage);
        panel.setGeometry(controllerRect(stage.size()));
        require(stage.rect().contains(panel.geometry()), "controller contained by stage");
        QFont font;
        font.setPixelSize(12);
        font.setWeight(QFont::DemiBold);
        QFontMetrics metrics(font);
        for (const auto& text : {QStringLiteral("0:00"), QStringLiteral("12:34:56")}) {
            const auto g = controls(panel.width(), metrics.horizontalAdvance(text),
                metrics.horizontalAdvance(QString(QChar(8722)) + text), metrics.height());
            for (const auto& rect : {g.elapsed, g.remaining, g.seek, g.play, g.volumeToggle,
                     g.volume, g.fullscreen, g.popout, g.menu}) {
                QWidget child(&panel);
                child.setGeometry(rect);
                require(panel.rect().contains(child.geometry()), "control contained by controller");
            }
            require(g.play.center().x() == (panel.width() - 1) / 2, "play remains centred independent of labels");
            require(g.elapsed.right() < g.seek.left() && g.seek.right() < g.remaining.left(), "time texts do not overlap seek track");
            require(g.volume.right() < g.play.left(), "volume does not overlap play");
            require(g.play.right() < g.fullscreen.left(), "right controls do not overlap play");
            require(g.fullscreen.right() < g.popout.left() && g.popout.right() < g.menu.left(), "source right control ordering");
        }
    }
    qInfo() << "Telegram video chrome geometry and time-policy checks passed";
    return 0;
}
