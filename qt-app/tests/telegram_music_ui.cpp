// Headless geometry/input regression for the production music controls.
// No vault, decoder worker, QAudioOutput or GUI platform is used.
#include "videovault/app/music_window.hpp"
#include "videovault/app/telegram_music_controls.hpp"
#include <QAbstractButton>
#include <QCursor>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QThread>
#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <iostream>

using namespace videovault::app;
namespace tmusic = videovault::app::telegram_music;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n'; return 1; } } while (false)
static void mouse(QWidget& w, QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent e(type, point, w.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&w, &e);
}
static void wait(int ms) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(2); }
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Do not read/write the user's playback preferences from this test.
    QTemporaryDir settingsDir(qEnvironmentVariable("TMPDIR", QDir::tempPath()) + QStringLiteral("/mvp-music-ui-XXXXXX"));
    CHECK(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("MegaVaultRegression"));
    QCoreApplication::setApplicationName(QStringLiteral("TelegramMusicUi"));
    for (int width : {320, 459, 460, 740}) {
        const auto g = tmusic::geometry(width, true, true, 40, 17, 13);
        CHECK(g.previous == QRect(9, 0, 24, 30));
        CHECK(g.play == QRect(34, 0, 24, 30));
        CHECK(g.next == QRect(59, 0, 24, 30));
        CHECK(g.name.x() == 91 && g.name.y() + 13 == 22);
        CHECK(g.close == QRect(width - 39, 0, 39, 30));
        CHECK(g.volume.width() == 34 && g.order.width() == 30);
        CHECK(g.repeat.width() == 30 && g.speed.width() == 30);
        CHECK(g.seek == QRect(0, 30, width, 6));
        CHECK(g.name.width() >= 0 && g.name.right() < g.time.left());
        CHECK(g.time.right() < g.volume.left());
    }
    ProgressLine seek;
    seek.resize(400, 6);
    int previews = 0, commits = 0, committed = -1;
    seek.moved = [&](int) { ++previews; };
    seek.released = [&](int value) { ++commits; committed = value; };
    mouse(seek, QEvent::MouseButtonPress, {100,3}, Qt::LeftButton, Qt::LeftButton);
    CHECK(seek.dragging() && seek.value() == 250 && commits == 0);
    mouse(seek, QEvent::MouseMove, {300,3}, Qt::NoButton, Qt::LeftButton);
    CHECK(seek.value() == 750 && commits == 0);
    mouse(seek, QEvent::MouseButtonRelease, {450,3}, Qt::LeftButton, Qt::NoButton);
    CHECK(!seek.dragging() && committed == 1000 && commits == 1 && previews == 3);
    mouse(seek, QEvent::MouseButtonPress, {0,3}, Qt::RightButton, Qt::RightButton);
    CHECK(!seek.dragging() && commits == 1);
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QApplication::sendEvent(&seek, &home);
    CHECK(seek.value() == 0 && commits == 2);
    seek.setEnabled(false);
    mouse(seek, QEvent::MouseButtonPress, {200,3}, Qt::LeftButton, Qt::LeftButton);
    CHECK(!seek.dragging() && commits == 2);

    // Render the actual source-derived pause shape: bars at 7..11 and 13..17,
    // unlike the old 14px-high generic pause glyph. Checks do not require fonts.
    QImage image(24, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    { QPainter p(&image); tmusic::paintShape(p, tmusic::pauseShape(), Qt::red); }
    CHECK(qAlpha(image.pixel(8, 10)) > 0);
    CHECK(qAlpha(image.pixel(14, 10)) > 0);
    CHECK(qAlpha(image.pixel(12, 10)) == 0);
    CHECK(qAlpha(image.pixel(8, 4)) == 0);

    QWidget shell;
    MusicWindow player(&shell);
    player.resize(740, tmusic::barHeight);
    shell.resize(800, 140);
    shell.show(); player.show();
    QApplication::processEvents();
    CHECK(player.height() == 36 && player.parentWidget() == &shell);
    auto* name = player.findChild<QLabel*>(QStringLiteral("musicSongLabel"));
    auto* volume = player.findChild<QAbstractButton*>(QStringLiteral("musicVolume"));
    auto* repeat = player.findChild<QAbstractButton*>(QStringLiteral("musicRepeat"));
    auto* speed = player.findChild<QAbstractButton*>(QStringLiteral("musicSpeed"));
    auto* playlist = player.findChild<QWidget*>(QStringLiteral("musicHoverPlaylist"));
    auto* orderMenu = player.findChild<QWidget*>(QStringLiteral("musicOrderMenu"));
    CHECK(name && volume && repeat && speed && playlist && orderMenu);
    // Null vault deliberately exercises queue/UI only; startWorker exits before
    // allocating a reader or output. No encrypted pipeline fixture is replaced.
    player.playQueue({}, {{1, QStringLiteral("Artist - First.wav")}, {2, QStringLiteral("Artist - Second.wav")}}, 0);
    CHECK(player.queueSize() == 2 && player.parentWidget() == &shell);
    CHECK(playlist->width() == 344);
    CHECK(volume->geometry().size() == QSize(34,30));
    for (int width : {320, 459, 460, 740}) {
        QCursor::setPos(player.mapToGlobal(QPoint(-100,-100)));
        player.resize(width, 36);
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&player, &leave);
        wait(5);
        CHECK(volume->isHidden() == (width < 460));
        QEvent enter(QEvent::Enter); QApplication::sendEvent(&player, &enter);
        CHECK(!volume->isHidden());
        CHECK(player.findChild<QWidget*>(QStringLiteral("musicSeekLine"))->geometry() == QRect(0,30,width,6));
        CHECK(name->geometry().right() < volume->geometry().left());
    }
    repeat->click(); CHECK(repeat->toolTip() == QStringLiteral("Repeat this song"));
    repeat->click(); CHECK(repeat->toolTip() == QStringLiteral("Repeat playlist"));
    repeat->click(); CHECK(repeat->toolTip() == QStringLiteral("Repeat"));
    auto* order = player.findChild<QAbstractButton*>(QStringLiteral("musicOrder"));
    CHECK(order);
    order->click(); CHECK(orderMenu->isVisible());
    QAbstractButton* reverse = nullptr;
    for (auto* row : orderMenu->findChildren<QAbstractButton*>())
        if (row->accessibleName() == QStringLiteral("Reverse order")) reverse = row;
    CHECK(reverse); reverse->click();
    CHECK(orderMenu->isHidden() && order->toolTip() == QStringLiteral("Reverse order"));
    order->click(); reverse->click();
    CHECK(order->toolTip() == QStringLiteral("Playback order"));
    QEvent speedEnter(QEvent::Enter); QApplication::sendEvent(speed, &speedEnter);
    auto* speedMenu = player.findChild<QWidget*>(QStringLiteral("musicSpeedMenu"));
    CHECK(speedMenu && speedMenu->isVisible());
    QAbstractButton* fast = nullptr;
    for (auto* row : speedMenu->findChildren<QAbstractButton*>())
        if (row->accessibleName() == QStringLiteral("Fast")) fast = row;
    CHECK(fast); fast->click();
    CHECK(speed->text() == QStringLiteral("1.5X") && speedMenu->isHidden());
    speed->click(); CHECK(speed->text() == QStringLiteral("1X"));
    speed->click(); CHECK(speed->text() == QStringLiteral("1.5X"));
    const QPoint wheelPos(4,4);
    QWheelEvent wheel(QPointF(wheelPos), QPointF(volume->mapToGlobal(wheelPos)),
        QPoint(), QPoint(0,-120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(volume, &wheel);
    CHECK(volume->accessibleName() == QStringLiteral("Volume 90%"));
    volume->click(); CHECK(volume->accessibleName() == QStringLiteral("Volume 0%"));
    volume->click(); CHECK(volume->accessibleName() == QStringLiteral("Volume 90%"));
    player.resize(740,36);
    const QPoint song(name->geometry().center().x(), 15);
    QCursor::setPos(player.mapToGlobal(song));
    mouse(player, QEvent::MouseMove, song, Qt::NoButton, Qt::NoButton);
    CHECK(playlist->isHidden()); // delayed hover, never inline queue expansion
    wait(340);
    CHECK(playlist->isVisible());
    CHECK(playlist->height() == 112); // two 50px OverviewFileLayout rows + margins
    mouse(player, QEvent::MouseButtonPress, song, Qt::LeftButton, Qt::LeftButton);
    CHECK(playlist->isHidden());
    QWidget* floating = nullptr;
    for (auto* w : QApplication::topLevelWidgets())
        if (w->objectName() == QStringLiteral("musicFloatingPlayer")) floating = w;
    CHECK(floating && floating->isVisible() && floating->parentWidget() == nullptr);
    CHECK(player.parentWidget() == &shell && player.height() == 36);
    // Dragging the card is retained independently from footer/playlist ownership.
    const auto old = floating->pos();
    mouse(*floating, QEvent::MouseButtonPress, {8,60}, Qt::LeftButton, Qt::LeftButton);
    mouse(*floating, QEvent::MouseMove, {28,75}, Qt::NoButton, Qt::LeftButton);
    mouse(*floating, QEvent::MouseButtonRelease, {8,60}, Qt::LeftButton, Qt::NoButton);
    CHECK(floating->pos() != old);
    floating->hide();
    mouse(player, QEvent::MouseButtonDblClick, song, Qt::LeftButton, Qt::LeftButton);
    CHECK(floating->isVisible() && playlist->isHidden());
    player.close();
    CHECK(!floating->isVisible() && player.queueSize() == 0);
    std::cout << "Telegram music UI: source geometry, widths, pause paths, seek preview/commit, repeat cycle, hover playlist, click/double-click and draggable floating card passed.\n";
    return 0;
}
