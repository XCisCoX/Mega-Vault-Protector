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
#include "videovault/core/vault.hpp"
#include "videovault/app/theme.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QPushButton>
#include <QLineEdit>
#include <QSettings>
#include <QStyle>
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

    // --- the library, with a vault created on the spot -----------------------
    const auto vaultRoot = std::filesystem::path(settingsDir.path().toStdString()) / "vault";
    if (auto created = videovault::core::Vault::create(vaultRoot, "local", {8192, 1, 1})) {
        {
            auto vault = std::make_shared<videovault::core::Vault>(std::move(created.value()));
            const std::vector<unsigned char> note{'n', 'o', 't', 'e'};
            vault->import_bytes(note, "note.txt");
        }
        for (auto* edit : window.findChildren<QLineEdit*>()) {
            if (edit->placeholderText().contains(QStringLiteral("Vault storage"))) {
                edit->setText(QDir::toNativeSeparators(QString::fromStdString(vaultRoot.string())));
            } else if (edit->echoMode() == QLineEdit::Password) {
                edit->setText(QStringLiteral("local"));
            }
        }
        for (auto* button : window.findChildren<QPushButton*>()) {
            if (button->isVisible() && button->text().contains(QStringLiteral("Unlock vault"))) {
                button->click();
                break;
            }
        }
        pump(2500);
        capture(window, QStringLiteral("library"));

        // The path bar is the reported bug: a drag highlight used to resize the row.
        QWidget* host = nullptr;
        QList<QPushButton*> crumbs;
        for (auto* button : window.findChildren<QPushButton*>()) {
            if (button->property("mvpDropFolder").isValid()) {
                crumbs.append(button);
                host = button->parentWidget();
            }
        }
        if (host != nullptr && !crumbs.isEmpty()) {
            QPushButton* crumb = crumbs.first();
            const int hostBefore = host->height();
            const int crumbBefore = crumb->height();
            crumb->setProperty("dropTarget", true);
            crumb->style()->unpolish(crumb);
            crumb->style()->polish(crumb);
            pump(300);
            const int hostDuring = host->height();
            const int crumbDuring = crumb->height();
            capture(window, QStringLiteral("library-highlight"));
            crumb->setProperty("dropTarget", false);
            crumb->style()->unpolish(crumb);
            crumb->style()->polish(crumb);
            pump(300);
            std::printf("path bar: host %d -> %d -> %d | crumb %d -> %d -> %d | crumbs %d\n",
                hostBefore, hostDuring, host->height(), crumbBefore, crumbDuring, crumb->height(),
                int(crumbs.size()));
            std::printf("  highlight invariant %s\n",
                (hostBefore == hostDuring && hostDuring == host->height()
                    && crumbBefore == crumbDuring && crumbDuring == crumb->height())
                    ? "HOLDS (highlight does not resize the row)" : "BROKEN");

            // A rebuild empties the row while it recreates the crumbs; the bar used to be
            // sized from a hint taken in that window and then clipped. Clicking the
            // current crumb forces a full rebuild, then the bar must be unchanged.
            QWidget* bar = host->parentWidget();
            while (bar != nullptr && bar->parentWidget() != window.findChild<QWidget*>()
                   && bar->parentWidget() != nullptr && !bar->isWindow()) {
                if (bar->height() > host->height() + 8) {
                    break;
                }
                bar = bar->parentWidget();
            }
            const int barBefore = bar != nullptr ? bar->height() : -1;
            const int hostFloor = host->minimumHeight();
            crumb->click();
            pump(900);
            QWidget* hostAfter = nullptr;
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->property("mvpDropFolder").isValid()) {
                    hostAfter = button->parentWidget();
                    break;
                }
            }
            const int barAfter = bar != nullptr ? bar->height() : -1;
            std::printf("path bar: bar %d -> %d after a rebuild | row floor %d | row %d\n",
                barBefore, barAfter, hostFloor, hostAfter != nullptr ? hostAfter->height() : -1);
            std::printf("  rebuild invariant %s\n",
                (barBefore == barAfter && hostFloor >= 40) ? "HOLDS (bar does not shrink on rebuild)"
                                                           : "BROKEN");
            capture(window, QStringLiteral("library-after-rebuild"));

            // Regression check for the reported bug: force the bar short the way the bug
            // did (a degenerate row hint measured mid-rebuild), then let a rebuild run.
            // The rebuild has to restore the bar without the user resizing the window.
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->property("mvpDropFolder").isValid()) {
                    button->setVisible(false);
                }
            }
            host->setMinimumHeight(0);
            window.resize(window.width() + 1, window.height());
            pump(400);
            const int shrunk = bar->height();
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->property("mvpDropFolder").isValid()) {
                    button->setVisible(true);
                }
            }
            host->setMinimumHeight(hostFloor);
            bool rebuilt = false;
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->property("mvpDropFolder").isValid() && button->isVisible()) {
                    button->click();
                    rebuilt = true;
                    break;
                }
            }
            pump(900);
            std::printf("path bar: forced short to %d, after a rebuild %d (full height %d)\n",
                shrunk, bar->height(), barBefore);
            std::printf("  recovery invariant %s\n",
                (rebuilt && shrunk < barBefore && bar->height() == barBefore)
                    ? "HOLDS (a rebuild restores the bar)" : "BROKEN");
        } else {
            std::printf("path bar: no crumbs found (login did not reach the library)\n");
        }
    }

    // The PDF viewer builds its header and footer controls in its constructor, so its
    // buttons can be reviewed without a document loaded.
    videovault::app::PdfWindow pdf;
    pdf.resize(1000, 760);
    pdf.show();
    pump(500);
    capture(pdf, QStringLiteral("pdf-viewer"));
    return 0;
}
