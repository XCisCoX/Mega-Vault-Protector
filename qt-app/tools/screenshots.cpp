// Offscreen screenshots of the desktop UI, for reviewing icon and layout work when
// there is no human at the keyboard.
//
//   cmake -S . -B out/build/local-ninja -DMVP_SCREENSHOT_HARNESS=ON
//   cmake --build out/build/local-ninja --target MvpScreenshotHarness
//   set MVP_SHOT_DIR=%TEMP%\mvp-shots
//   set MVP_SHOT_THEME=light          (optional; dark by default)
//   set QT_QPA_PLATFORM=offscreen
//   MvpScreenshotHarness
//
// QSettings is redirected to a temporary directory, so a run never reads or writes
// a real profile, and each screen also prints an icon audit: how many buttons carry
// a glyph, and which ones are still text-only.
#include "videovault/app/main_window.hpp"
#include "videovault/app/pdf_window.hpp"
#include "videovault/app/theme.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {

void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QString shotDir() {
    QString dir = QString::fromLocal8Bit(qgetenv("MVP_SHOT_DIR"));
    if (dir.isEmpty()) {
        dir = QDir::tempPath() + QStringLiteral("/mvp-shots");
    }
    QDir().mkpath(dir);
    return dir;
}

QString themeSuffix() {
    return qgetenv("MVP_SHOT_THEME") == "light" ? QStringLiteral("-light") : QString();
}

// Saves the window and reports how the buttons on it are labelled.
void capture(QWidget& window, const QString& name) {
    const QString path = shotDir() + QLatin1Char('/') + name + themeSuffix() + QStringLiteral(".png");
    std::printf("%s saved=%d\n", qPrintable(name), int(window.grab().save(path)));

    int withIcon = 0;
    QStringList textOnly;
    for (auto* button : window.findChildren<QPushButton*>()) {
        if (!button->isVisible()) {
            continue;
        }
        if (!button->icon().isNull()) {
            ++withIcon;
        } else if (!button->text().isEmpty()) {
            textOnly.append(button->text());
        }
    }
    std::printf("  buttons with an icon: %d | text-only: %d%s%s\n", withIcon, int(textOnly.size()),
        textOnly.isEmpty() ? "" : " -> ", qPrintable(textOnly.join(QStringLiteral(", "))));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSize(10);
    application.setFont(font);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::printf("cannot create a settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QApplication::setOrganizationName(QStringLiteral("MegaVaultScreenshots"));
    QApplication::setApplicationName(QStringLiteral("Screenshots"));
    // Production startup: the theme manager reads this setting and applies the
    // palette, the application stylesheet and every themed inline style. Skipping it
    // renders the app in native Windows styling, which is useless for review.
    const bool light = qgetenv("MVP_SHOT_THEME") == "light";
    QSettings().setValue(QStringLiteral("appearance/theme"),
        light ? QStringLiteral("light") : QStringLiteral("dark"));
    videovault::app::initializeTheme(application);

    videovault::app::MainWindow window;
    window.resize(1200, 820);
    window.show();
    pump(800);
    capture(window, QStringLiteral("setup"));

    // The setup card's "Open an existing vault" switches to the login page without
    // touching the filesystem, so the unlock screen can be captured the same way.
    for (auto* button : window.findChildren<QPushButton*>()) {
        if (button->isVisible() && button->text().contains(QStringLiteral("Open an existing vault"))) {
            button->click();
            break;
        }
    }
    pump(600);
    capture(window, QStringLiteral("login"));

    // The PDF viewer builds its header and footer controls in its constructor, so its
    // buttons can be reviewed without a document loaded.
    videovault::app::PdfWindow pdf;
    pdf.resize(1000, 760);
    pdf.show();
    pump(500);
    capture(pdf, QStringLiteral("pdf-viewer"));
    return 0;
}
