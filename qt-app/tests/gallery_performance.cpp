// Large-library regression against the actual gallery widgets and decorators.
#include "videovault/app/main_window.hpp"
#include "videovault/app/plugin_host.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/app/tag_filter_button.hpp"
#include "videovault/app/settings_dialog.hpp"
#include "videovault/app/in_window_sheet.hpp"
#include "videovault/app/record_window.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QListWidget>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QThreadPool>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QFutureWatcher>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QCheckBox>
#include <QPushButton>
#include <QMenu>
#include <QLayout>
#include <QAction>
#include <QToolButton>
#include <QJsonArray>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <cstdio>

namespace videovault::app {
struct GalleryPerformanceTestAccess {
    // Inspect real command widgets without showing a window or opening a menu.
    static int libraryCommands() {
        MainWindow window;
        auto require = [](bool ok, const char* message) {
            if (!ok) std::fprintf(stderr, "FAIL library commands: %s\n", message);
            return ok;
        };
        auto* add = window.importButton_;
        auto* more = window.findChild<QPushButton*>(QStringLiteral("libraryMore"));
        if (!require(add->text() == QStringLiteral("Add") && !add->icon().isNull()
                && !add->accessibleName().isEmpty() && add->focusPolicy() != Qt::NoFocus,
                "primary Add must have a plus icon, visible text and keyboard focus")) return 80;
        if (!require(more && more->text() == QStringLiteral("More") && more->menu()
                && !more->accessibleName().isEmpty() && more->focusPolicy() != Qt::NoFocus,
                "labelled, accessible More menu is missing")) return 81;
        auto* row = add->parentWidget()->layout()->itemAt(0)->layout();
        int buttonCount = 0;
        for (int i = 0; row && i < row->count(); ++i) {
            if (qobject_cast<QPushButton*>(row->itemAt(i)->widget())) ++buttonCount;
        }
        if (!require(buttonCount == 2, "title row must contain only Add and More buttons")) return 82;
        auto* addMenu = add->menu();
        if (!require(addMenu && addMenu->actions().size() == 5,
                "Add menu must contain the five supported add/create actions")) return 83;
        const QStringList expected{
            QStringLiteral("Photos, videos, music or other files…"), QStringLiteral("Add a folder…"),
            QStringLiteral("New note"), QStringLiteral("New record list"), QStringLiteral("New folder")};
        for (int i = 0; i < expected.size(); ++i) {
            if (!require(addMenu->actions().at(i)->text() == expected.at(i),
                    "Add contains an unexpected or jargon-heavy action")) return 84;
        }
        auto* moreMenu = more->menu();
        auto hasAction = [](QMenu* menu, const QString& text) {
            for (auto* action : menu->actions()) if (action->text() == text) return true;
            return false;
        };
        for (const QString& label : {QStringLiteral("Music"), QStringLiteral("Sync…"),
                QStringLiteral("Settings…"), QStringLiteral("Lock this PC"),
                QStringLiteral("Share with phone…"), QStringLiteral("Updates…")}) {
            if (!require(hasAction(moreMenu, label), "an existing secondary action was lost")) return 85;
        }
        auto* backups = moreMenu->findChild<QMenu*>(QStringLiteral("encryptedBackups"));
        if (!require(backups && backups->actions().size() == 2
                && backups->title().contains(QStringLiteral(".mvpk")),
                "encrypted backup submenu must explain the format")) return 86;
        for (auto* action : backups->actions()) {
            if (!require(action->text().contains(QStringLiteral("encrypted"))
                    && action->toolTip().contains(QStringLiteral(".mvpk")),
                    "backup actions must explain encrypted .mvpk packages")) return 87;
        }
        add->setEnabled(false);
        QMetaObject::invokeMethod(moreMenu, "aboutToShow", Qt::DirectConnection);
        if (!require(!backups->menuAction()->isEnabled(),
                "moving backups out of Add must not bypass disabled import state")) return 88;
        add->setEnabled(true);
        window.batchBusy_ = true;
        QMetaObject::invokeMethod(moreMenu, "aboutToShow", Qt::DirectConnection);
        if (!require(!backups->menuAction()->isEnabled(), "backup actions must stay disabled during a batch")) return 89;
        window.batchBusy_ = false;
        QMetaObject::invokeMethod(moreMenu, "aboutToShow", Qt::DirectConnection);
        if (!require(backups->menuAction()->isEnabled(), "backup actions must recover after a batch")) return 90;
        std::printf("PASS library commands: Add/More labels, beginner actions, preserved destinations, encrypted backups and busy state\n");
        return 0;
    }

    static int creation() {
        QTemporaryDir temp(qEnvironmentVariable("TMPDIR", QDir::tempPath())
            + QStringLiteral("/MVP-creation-XXXXXX"));
        if (!temp.isValid()) return 50;
        for (int profile = 0; profile < 2; ++profile) {
            MainWindow window;
            window.showSetup();
            window.show();
            const QString root = temp.path() + QStringLiteral("/vault-%1").arg(profile);
            window.setupLocation_->setText(root);
            window.setupPassword_->setText(QStringLiteral("Disposable creation test"));
            window.setupConfirmation_->setText(QStringLiteral("Disposable creation test"));
            window.securityProfile_->setCurrentIndex(profile);
            window.beginCreate();
            QElapsedTimer clock;
            clock.start();
            while (window.watcher_ && clock.elapsed() < 60000) {
                QApplication::processEvents();
                QThread::msleep(10);
            }
            if (!window.vault_ || !window.vault_->is_unlocked()) {
                std::fprintf(stderr, "FAIL creation profile %d: %s\n", profile,
                    qPrintable(window.setupError_->text()));
                return 51;
            }
            window.lockVault();
            auto opened = core::Vault::open(std::filesystem::path(root.toStdWString()),
                "Disposable creation test");
            if (!opened) {
                std::fprintf(stderr, "FAIL reopen: %s\n", opened.error().technical_detail.c_str());
                return 52;
            }
            std::printf("PASS real setup worker: profile %d creation, unlock, reopen\n", profile);
        }
        return 0;
    }

    // Theme screenshots of the real folder surface (no user vault involved).
    static void shoot(MainWindow& window, const QString& name) {
        const QString dir = qEnvironmentVariable("MVP_FOLDER_SHOTS");
        if (dir.isEmpty()) return;
        QDir().mkpath(dir);
        QApplication::processEvents();
        window.grab().save(dir + "/" + name + ".png");
    }
    static void shootBothThemes(MainWindow& window, const QString& name) {
        if (qEnvironmentVariableIsEmpty("MVP_FOLDER_SHOTS")) return;
        setThemeMode(ThemeMode::Dark);
        shoot(window, name + "-dark");
        setThemeMode(ThemeMode::Light);
        shoot(window, name + "-light");
        setThemeMode(ThemeMode::Dark);
    }
    static void drain(MainWindow& window) {
        QElapsedTimer clock;
        clock.start();
        int idle = 0;
        while (clock.elapsed() < 12000 && idle < 10) {
            QApplication::processEvents();
            bool busy = window.galleryListing_ || window.galleryTasksInFlight_ > 0
                || window.searchTimer_->isActive();
            for (auto* watcher : window.findChildren<QFutureWatcherBase*>()) {
                busy = busy || !watcher->isFinished();
            }
            idle = busy ? 0 : idle + 1;
            QThread::msleep(10);
        }
    }
    // One provider folder driven through the real widgets: the mixed root list,
    // an album opening as a folder of its rows, and a row download reaching
    // Saved with a progress state seen in between. Only the HTTP transport is
    // replaced (setFetcher); rows, buttons, the FLAC check and the encrypted
    // import are the production code.

    // Creating a record from the library: sheet prompt, vault import, editor open.
    static int newRecord() {
        const auto scratch = qEnvironmentVariable("TMPDIR", QDir::tempPath());
        QTemporaryDir temp(scratch + QStringLiteral("/MVP-new-record-XXXXXX"));
        if (!temp.isValid()) return 70;
        core::Argon2Parameters parameters;
        parameters.memory_kib = 8192;
        parameters.iterations = 1;
        auto created = core::Vault::create(std::filesystem::path(temp.path().toStdWString()) / L"vault",
            "generated record fixture", parameters);
        if (!created) return 71;
        // The light theme is where the record window used to restyle itself in a
        // loop; keep the regression test in that state.
        videovault::app::setThemeMode(videovault::app::ThemeMode::Light);
        MainWindow window;
        window.vault_ = std::make_shared<core::Vault>(std::move(created.value()));
        window.pages_->setCurrentIndex(2);
        window.contentStack_->setCurrentWidget(window.pages_);
        window.resize(1100, 760);
        window.show();
        QApplication::processEvents();
        const QString shots = qEnvironmentVariable("MVP_NEW_RECORD_SHOTS");

        InWindowSheet::setTestDriver([&](InWindowSheet* sheet) {
            QLineEdit* entry = nullptr;
            for (QLineEdit* candidate : sheet->findChildren<QLineEdit*>()) {
                if (candidate->echoMode() == QLineEdit::Normal && candidate->isVisible()) {
                    entry = candidate;
                    break;
                }
            }
            if (entry == nullptr) {
                std::fprintf(stderr, "FAIL record sheet has no title field\n");
                return;
            }
            entry->setText(QStringLiteral("Field notes"));
            QApplication::processEvents();
            if (!shots.isEmpty()) {
                QDir().mkpath(shots);
                window.grab().save(shots + "/record-prompt.png");
            }
            for (QPushButton* button : sheet->findChildren<QPushButton*>()) {
                if (button->text().contains(QStringLiteral("Create"))) {
                    button->click();
                    return;
                }
            }
            std::fprintf(stderr, "FAIL record sheet has no Create button\n");
        });
        window.beginNewRecord();
        InWindowSheet::setTestDriver({});

        QElapsedTimer clock;
        clock.start();
        while (window.recordWindow_ == nullptr && clock.elapsed() < 20000) {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        if (window.recordWindow_ == nullptr) {
            std::fprintf(stderr, "FAIL record editor did not open: %s\n",
                qPrintable(window.galleryStatus_->text()));
            return 72;
        }
        for (int i = 0; i < 60; ++i) { QApplication::processEvents(); QThread::msleep(10); }
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            window.grab().save(shots + "/record-editor.png");
        }

        // Inside the editor: add a record (the same sheet prompt), then save.
        QPushButton* addRecord = nullptr;
        for (QPushButton* candidate : window.recordWindow_->findChildren<QPushButton*>()) {
            if (candidate->accessibleName() == QStringLiteral("Add a record to this file")
                || candidate->toolTip() == QStringLiteral("Add a record to this file")) {
                addRecord = candidate;
            }
        }
        if (addRecord == nullptr) {
            std::fprintf(stderr, "FAIL editor has no Add record button\n");
            return 73;
        }
        InWindowSheet::setTestDriver([](InWindowSheet* sheet) {
            for (QLineEdit* entry : sheet->findChildren<QLineEdit*>()) {
                if (entry->echoMode() == QLineEdit::Normal && entry->isVisible()) {
                    entry->setText(QStringLiteral("Web Services"));
                    break;
                }
            }
            QApplication::processEvents();
            for (QPushButton* button : sheet->findChildren<QPushButton*>()) {
                if (button->text().contains(QStringLiteral("Save"))) { button->click(); return; }
            }
        });
        addRecord->click();
        InWindowSheet::setTestDriver({});
        for (int i = 0; i < 40; ++i) { QApplication::processEvents(); QThread::msleep(10); }

        QPushButton* saveButton = nullptr;
        for (QPushButton* candidate : window.recordWindow_->findChildren<QPushButton*>()) {
            if (candidate->property("primary").toBool()) { saveButton = candidate; break; }
        }
        if (saveButton == nullptr) {
            std::fprintf(stderr, "FAIL editor has no save button\n");
            return 74;
        }
        saveButton->click();
        for (int i = 0; i < 120; ++i) { QApplication::processEvents(); QThread::msleep(10); }
        if (!shots.isEmpty()) {
            window.grab().save(shots + "/record-saved.png");
        }
        window.closeRecord();
        for (int i = 0; i < 40; ++i) { QApplication::processEvents(); QThread::msleep(10); }
        std::printf("PASS new record: prompt, vault import, editor open, add record, save\n");
        return 0;
    }

    static int pluginFolder() {
        const auto scratch = qEnvironmentVariable("TMPDIR", QDir::tempPath());
        QTemporaryDir temp(scratch + QStringLiteral("/MVP-plugin-folder-XXXXXX"));
        if (!temp.isValid()) return 60;
        QElapsedTimer stage;
        stage.start();
        core::Argon2Parameters parameters;
        parameters.memory_kib = 8192;
        parameters.iterations = 1;
        auto created = core::Vault::create(std::filesystem::path(temp.path().toStdWString()) / L"vault",
            "generated provider folder fixture", parameters);
        if (!created) return 61;
        MainWindow window;
        window.vault_ = std::make_shared<core::Vault>(std::move(created.value()));
        window.pages_->setCurrentIndex(2);
        window.contentStack_->setCurrentWidget(window.pages_);
        window.resize(1100, 760);
        window.show();
        QApplication::processEvents();
        if (!window.pluginHost_->setEnabled(QStringLiteral("org.megavault.monochrome"), true)) return 62;

        const QString albumId = QStringLiteral("190000000000000001");
        const QString songA = QStringLiteral("190000000000000002");
        const QString songB = QStringLiteral("190000000000000003");
        const QString track1 = QStringLiteral("190000000000000004");
        const QString track2 = QStringLiteral("190000000000000005");
        QJsonObject catalogue;
        catalogue.insert(QStringLiteral("tracks"), QJsonArray{
            QJsonObject{{QStringLiteral("trackId"), songA}, {QStringLiteral("title"), QStringLiteral("Alpha Song")},
                {QStringLiteral("artistNames"), QJsonArray{QStringLiteral("Fixture Artist")}},
                {QStringLiteral("durationMs"), 187000}},
            QJsonObject{{QStringLiteral("trackId"), songB}, {QStringLiteral("title"), QStringLiteral("Beta Song")},
                {QStringLiteral("artistNames"), QJsonArray{QStringLiteral("Fixture Artist")}},
                {QStringLiteral("durationMs"), 95000}}});
        catalogue.insert(QStringLiteral("releases"), QJsonArray{
            QJsonObject{{QStringLiteral("releaseId"), albumId}, {QStringLiteral("title"), QStringLiteral("Fixture Album")},
                {QStringLiteral("artistNames"), QJsonArray{QStringLiteral("Fixture Band")}}}});

        window.enterPluginFolder(QStringLiteral("fixture"));
        std::printf("[stage] enter=%lld\n", (long long)stage.elapsed());
        // The live search the folder just started may still answer; its term no
        // longer matches, so the catalogue below is the one that is listed.
        window.pluginQuery_ = QStringLiteral("fixture-catalogue");
        window.pluginCatalogue_ = catalogue;
        window.rebuildPluginFolder();
        QApplication::processEvents();
        if (!qEnvironmentVariableIsEmpty("MVP_MENU_SHOTS")) {
            // The gallery context menu is a popup that runs its own event loop, so
            // the shot is taken from a timer that then dismisses it.
            const QString dir = qEnvironmentVariable("MVP_MENU_SHOTS");
            QDir().mkpath(dir);
            // The library itself (folder pane + rows), in both themes.
            window.leavePluginFolder();
            QApplication::processEvents();
            const QByteArray onlyTheme = qEnvironmentVariable("MVP_MENU_THEME").toLatin1();
            for (const char* mode : {"dark", "light"}) {
                const bool darkMode = QByteArray(mode) == QByteArrayLiteral("dark");
                if (!onlyTheme.isEmpty() && onlyTheme != QByteArray(mode)) {
                    continue;
                }
                window.activateWindow();
                QApplication::processEvents();
                for (int i = 0; i < 20; ++i) { QApplication::processEvents(); QThread::msleep(10); }
                videovault::app::setThemeMode(darkMode
                    ? videovault::app::ThemeMode::Dark
                    : videovault::app::ThemeMode::Light);
                QApplication::processEvents();
                window.grab().save(dir + "/library-" + QString::fromLatin1(mode) + ".png");
            }
            window.enterPluginFolder(QStringLiteral("fixture"));
            window.pluginCatalogue_ = catalogue;
            window.rebuildPluginFolder();
            QApplication::processEvents();
            for (const char* mode : {"dark", "light"}) {
                const bool darkMode = QByteArray(mode) == QByteArrayLiteral("dark");
                if (!onlyTheme.isEmpty() && onlyTheme != QByteArray(mode)) {
                    continue;
                }
                videovault::app::setThemeMode(darkMode
                    ? videovault::app::ThemeMode::Dark
                    : videovault::app::ThemeMode::Light);
                QApplication::processEvents();
                const QString name = QString::fromLatin1(mode);
                QTimer::singleShot(250, [&window, dir, name] {
                    if (auto* popup = QApplication::activePopupWidget()) {
                        window.grab().save(dir + "/menu-over-window-" + name + ".png");
                        popup->grab().save(dir + "/menu-" + name + ".png");
                        popup->close();
                    }
                });
                window.showGalleryContextMenu(window.mapToGlobal(QPoint(620, 430)));
                QApplication::processEvents();
            }
            // The unlock card in a short window: it must scroll, never clip.
            videovault::app::setThemeMode(videovault::app::ThemeMode::Light);
            window.resize(700, 420);
            window.lockVault();
            // Let the lock transition fade finish before capturing.
            for (int i = 0; i < 40; ++i) { QApplication::processEvents(); QThread::msleep(10); }
            window.grab().save(dir + "/login-short-light.png");
            videovault::app::setThemeMode(videovault::app::ThemeMode::Dark);
            QApplication::processEvents();
            window.grab().save(dir + "/login-short-dark.png");
            std::puts("menu screenshots written");
            return 0;
        }
        if (!qEnvironmentVariableIsEmpty("MVP_FOLDER_SHOTS")) {
            shootBothThemes(window, "provider-root");
            window.pluginAlbumId_ = albumId;
            window.pluginAlbumName_ = QStringLiteral("Fixture Album");
            window.pluginAlbumTracks_ = catalogue.value(QStringLiteral("tracks")).toArray();
            window.pluginAlbumPending_ = false;
            window.rebuildPluginFolder();
            shootBothThemes(window, "provider-album");
            std::puts("screenshots written");
            return 0;
        }

        // 1. The root is one mixed list: album folders and song rows, no
        //    Songs/Albums/Artists level and no artists anywhere.
        int backs = 0, albums = 0, songs = 0;
        QStringList labels;
        QListWidgetItem* albumRow = nullptr;
        for (int i = 0; i < window.iconList_->count(); ++i) {
            QListWidgetItem* row = window.iconList_->item(i);
            const int kind = row->data(Qt::UserRole + 12).toInt();
            labels << row->text();
            if (kind == 5) { ++backs; continue; }
            if (kind == 8) { ++albums; albumRow = row; continue; }
            if (kind == 7) {
                ++songs;
                if (row->data(Qt::UserRole + 20).toString().isEmpty()) {
                    std::fprintf(stderr, "FAIL: song row without a track id\n");
                    return 63;
                }
                if (!row->data(Qt::UserRole + 19).toString().contains(QStringLiteral("·"))) {
                    std::fprintf(stderr, "FAIL: song row has no artist · duration line: %s\n",
                        qPrintable(row->data(Qt::UserRole + 19).toString()));
                    return 64;
                }
            }
            if (kind == 6) {
                std::fprintf(stderr, "FAIL: category row still present\n");
                return 65;
            }
        }
        for (const QString& label : labels) {
            const QString trimmed = label.trimmed();
            if (trimmed.compare(QStringLiteral("Songs"), Qt::CaseInsensitive) == 0
                || trimmed.compare(QStringLiteral("Albums"), Qt::CaseInsensitive) == 0
                || trimmed.compare(QStringLiteral("Artists"), Qt::CaseInsensitive) == 0) {
                std::fprintf(stderr, "FAIL: category row still present: %s\n", qPrintable(trimmed));
                return 66;
            }
        }
        if (backs != 1 || albums != 1 || songs != 2 || albumRow == nullptr) {
            std::fprintf(stderr, "FAIL: root rows backs=%d albums=%d songs=%d\n", backs, albums, songs);
            return 67;
        }
        std::printf("Root: 1 Back, %d album folder(s), %d song row(s) · footer \"%s\"\n",
            albums, songs, qPrintable(window.statusCountLabel_->text()));
        if (!window.statusCountLabel_->text().startsWith(QStringLiteral("3 items"))) {
            std::fprintf(stderr, "FAIL: footer does not count song rows + album folders: %s\n",
                qPrintable(window.statusCountLabel_->text()));
            return 68;
        }
        // Every song row carries a Download button over its right edge.
        QList<QToolButton*> buttons;
        for (auto* button : window.iconList_->viewport()->findChildren<QToolButton*>()) {
            if (button->objectName() == QStringLiteral("pluginRowDownload") && button->isVisible())
                buttons.append(button);
        }
        if (buttons.size() != songs) {
            std::fprintf(stderr, "FAIL: %d visible row buttons for %d songs\n",
                int(buttons.size()), songs);
            return 69;
        }
        for (auto* button : buttons) {
            if (button->text() != QStringLiteral("Download")) {
                std::fprintf(stderr, "FAIL: fresh row button reads \"%s\"\n", qPrintable(button->text()));
                return 70;
            }
        }

        std::printf("[stage] root=%lld\n", (long long)stage.elapsed());
        // 2. Opening the album is a folder open, not a jump to the music page.
        window.iconList_->itemDoubleClicked(albumRow);
        QApplication::processEvents();
        if (!window.pluginAlbumPending_ || window.pluginAlbumId_.isEmpty()) {
            std::fprintf(stderr, "FAIL: double-clicking the album row did not open it\n");
            return 71;
        }
        if (!labels.contains(QStringLiteral("Fixture Album"))) {
            std::fprintf(stderr, "FAIL: album row is not named after the release\n");
            return 72;
        }
        if (window.contentStack_->currentWidget() != window.pages_) {
            std::fprintf(stderr, "FAIL: the album open left the library page\n");
            return 73;
        }
        // The provider's track list for that release, as its answer delivers it.
        window.pluginAlbumPending_ = false;
        window.pluginAlbumTracks_ = QJsonArray{
            QJsonObject{{QStringLiteral("trackId"), track1}, {QStringLiteral("title"), QStringLiteral("Album Track One")},
                {QStringLiteral("artistNames"), QJsonArray{QStringLiteral("Fixture Band")}},
                {QStringLiteral("durationMs"), 211000}},
            QJsonObject{{QStringLiteral("trackId"), track2}, {QStringLiteral("title"), QStringLiteral("Album Track Two")},
                {QStringLiteral("artistNames"), QJsonArray{QStringLiteral("Fixture Band")}},
                {QStringLiteral("durationMs"), 65000}}};
        window.rebuildPluginFolder();
        QApplication::processEvents();
        int trackRows = 0, otherRows = 0;
        QStringList trackLabels;
        for (int i = 0; i < window.iconList_->count(); ++i) {
            QListWidgetItem* row = window.iconList_->item(i);
            const int kind = row->data(Qt::UserRole + 12).toInt();
            if (kind == 5) continue;
            if (kind == 7) { ++trackRows; trackLabels << row->text(); continue; }
            ++otherRows;
        }
        if (trackRows != 2 || otherRows != 0) {
            std::fprintf(stderr, "FAIL: album folder rows tracks=%d other=%d\n", trackRows, otherRows);
            return 74;
        }
        std::printf("Album folder: %s\n", qPrintable(trackLabels.join(QStringLiteral(" | "))));
        if (window.iconList_->item(0)->data(Qt::UserRole + 12).toInt() != 5) {
            std::fprintf(stderr, "FAIL: album folder lost its Back row\n");
            return 75;
        }

        std::printf("[stage] album=%lld\n", (long long)stage.elapsed());
        // 3. A row download: real queue, real FLAC check, real encrypted import.
        QByteArray flac(42 + 4096, '\0');
        flac[0] = 'f'; flac[1] = 'L'; flac[2] = 'a'; flac[3] = 'C';
        flac[4] = 0; flac[5] = 0; flac[6] = 0; flac[7] = 34;
        for (int i = 42; i < flac.size(); ++i) flac[i] = static_cast<char>((i * 37) & 0xff);
        if (!pluginFlac(flac)) return 76;
        window.ensurePluginDownloads();
        // Every state the download machine reports, and the ticks the fetcher
        // below feeds it, so "a progress state in between" is a fact and not a
        // lucky sample of a widget caption.
        QVector<QPair<int, int>> reported;
        QObject::connect(window.pluginDownloads_, &PluginTrackDownloader::stateChanged,
            [&reported](const QString&, int state, int percent) {
                reported.append(qMakePair(state, percent));
            });
        std::function<void(QByteArray, QString)> deliver;
        window.pluginDownloads_->setFetcher([&deliver](
                const QString&, std::function<void(qint64, qint64)> progress,
                std::function<void(QByteArray, QString)> done) {
            // Two byte-count ticks, then the body is released by the test: the
            // row's downloading state cannot be raced past by the transfer
            // finishing in the same event-loop pass.
            progress(512, 1024);
            progress(819, 1024);
            deliver = done;
        });
        // The first track row's own button, exactly as a user clicks it.
        QToolButton* rowButton = nullptr;
        for (auto* button : window.iconList_->viewport()->findChildren<QToolButton*>()) {
            if (button->objectName() == QStringLiteral("pluginRowDownload") && button->isVisible()) {
                rowButton = button;
                break;
            }
        }
        if (rowButton == nullptr) return 77;
        const QString downloadingText = rowButton->text();
        rowButton->click();
        QApplication::processEvents();
        // Observed before the body arrives: the button reads Downloading… N% and
        // the row itself is in its downloading state, not the Download button.
        QString midDownload;
        int midState = 0;
        for (auto* button : window.iconList_->viewport()->findChildren<QToolButton*>()) {
            if (button->objectName() != QStringLiteral("pluginRowDownload")) continue;
            if (button->text().contains(QLatin1Char('%'))) midDownload = button->text();
        }
        for (int i = 0; i < window.iconList_->count(); ++i) {
            if (window.iconList_->item(i)->data(Qt::UserRole + 18).toInt() == 2) midState = 2;
        }
        if (midDownload.isEmpty() || midState != 2 || deliver == nullptr) {
            std::fprintf(stderr, "FAIL: no downloading state before the body arrived (\"%s\", row state %d)\n",
                qPrintable(midDownload), midState);
            return 78;
        }
        std::printf("[stage] mid=%lld\n", (long long)stage.elapsed());
        std::printf("Mid-download row: \"%s\" (row state %d)\n", qPrintable(midDownload), midState);
        deliver(flac, QString());  // the transfer's body arrives
        QStringList seen;
        QElapsedTimer clock;
        clock.start();
        bool sawProgress = !midDownload.isEmpty();
        bool saved = false;
        while (clock.elapsed() < 20000) {
            QApplication::processEvents();
            for (auto* button : window.iconList_->viewport()->findChildren<QToolButton*>()) {
                if (button->objectName() != QStringLiteral("pluginRowDownload")) continue;
                if (!seen.contains(button->text())) seen << button->text();
                if (button->text().startsWith(QStringLiteral("Downloading")) && button->text().contains(QLatin1Char('%')))
                    sawProgress = true;
                if (button->text() == QStringLiteral("Saved")) saved = true;
            }
            if (saved && sawProgress) break;
            QThread::msleep(5);
        }
        int reportedProgress = 0;
        for (const auto& state : reported) {
            if (state.first == 2 && state.second > 0 && state.second < 100) ++reportedProgress;
        }
        std::printf("Download states seen on the row: %s (reported progress states=%d)\n",
            qPrintable(seen.join(QStringLiteral(" -> "))), reportedProgress);
        if (!sawProgress && reportedProgress > 0) sawProgress = true;
        if (!sawProgress) {
            std::fprintf(stderr, "FAIL: no progress state was shown while downloading (first %s)\n",
                qPrintable(downloadingText));
            return 78;
        }
        if (!saved) {
            std::fprintf(stderr, "FAIL: the row never reached Saved\n");
            return 79;
        }
        if (window.pluginDownloads_->active() > 2) {
            std::fprintf(stderr, "FAIL: more than two downloads ran at once\n");
            return 80;
        }
        std::printf("[stage] saved=%lld\n", (long long)stage.elapsed());
        // Encrypted import, in the vault, with the exact bytes from RAM.
        auto videos = window.vault_->list_videos();
        if (!videos || videos.value().size() != 1) {
            std::fprintf(stderr, "FAIL: the vault holds %d files after one download\n",
                videos ? int(videos.value().size()) : -1);
            return 81;
        }
        const auto& info = videos.value()[0];
        if (QString::fromStdString(info.display_name) != QStringLiteral("Album Track One.flac")) {
            std::fprintf(stderr, "FAIL: saved name is %s\n", info.display_name.c_str());
            return 82;
        }
        if (info.folder_id != 0) {
            std::fprintf(stderr, "FAIL: saved outside the configured destination folder\n");
            return 83;
        }
        auto bytes = window.vault_->read_video_bytes(info.id);
        if (!bytes || QByteArray(reinterpret_cast<const char*>(bytes.value().data()),
                        int(bytes.value().size())) != flac) {
            std::fprintf(stderr, "FAIL: the vault does not hold the exact downloaded bytes\n");
            return 84;
        }
        std::printf("Saved: %s (%d bytes, folder #%lld) read back from the encrypted vault\n",
            info.display_name.c_str(), int(flac.size()), static_cast<long long>(info.folder_id));

        // Back walks one level at a time: album -> provider root -> library.
        if (!window.handlePluginRow(5, -1)) return 85;
        QApplication::processEvents();
        if (!window.pluginFolder_ || !window.pluginAlbumId_.isEmpty()) {
            std::fprintf(stderr, "FAIL: Back from the album did not return to the provider root\n");
            return 86;
        }
        if (window.iconList_->count() != 4) {
            std::fprintf(stderr, "FAIL: provider root rows after Back = %d\n", window.iconList_->count());
            return 87;
        }
        window.handlePluginRow(5, -1);
        QApplication::processEvents();
        if (window.pluginFolder_) {
            std::fprintf(stderr, "FAIL: Back from the provider root did not return to the library\n");
            return 88;
        }
        std::printf("PASS provider folder: mixed root, album as a folder, per-row download -> Saved (%lld ms)\n",
            (long long)stage.elapsed());
        return 0;
    }
    static int run() {
        if (qEnvironmentVariableIsSet("MVP_VAULT_CREATION_TEST")) return creation();
        if (qEnvironmentVariableIsSet("MVP_PLUGIN_FOLDER_TEST")) return pluginFolder();
        if (qEnvironmentVariableIsSet("MVP_NEW_RECORD_TEST")) return newRecord();
        const auto scratch = qEnvironmentVariable("TMPDIR", QDir::tempPath());
        QTemporaryDir temp(scratch + QStringLiteral("/MVP-gallery-XXXXXX"));
        core::Argon2Parameters parameters;
        parameters.memory_kib = 8192;
        parameters.iterations = 1;
        auto created = core::Vault::create(std::filesystem::path(temp.path().toStdWString()) / L"vault",
            "generated performance fixture", parameters);
        if (!created) return 2;
        MainWindow window;
        window.vault_ = std::make_shared<core::Vault>(std::move(created.value()));
        if (qEnvironmentVariableIsSet("MVP_PLUGIN_NAVIGATION_TEST")) {
            window.pages_->setCurrentIndex(2); window.show(); QApplication::processEvents();
            window.openSettings(); QApplication::processEvents();
            auto* enable = window.findChild<QCheckBox*>("pluginEnable_org.megavault.monochrome");
            if (!enable || enable->isChecked()) return 41;
            enable->setChecked(true);
            window.settingsPage_->reject(); QApplication::processEvents();
            auto* music = window.findChild<QPushButton*>("musicSection");
            if (!music || !music->isVisible()) { std::fprintf(stderr,"FAIL: enabled plugin Music navigation absent\n"); return 42; }
            music->click(); QApplication::processEvents();
            if (window.contentStack_->currentWidget()->objectName() != "musicPluginPage") return 43;
            window.findChild<QPushButton*>("musicBack")->click(); QApplication::processEvents();
            if(window.contentStack_->currentWidget()!=window.pages_)return 44;
            window.openSettings(); QApplication::processEvents();
            enable=window.settingsPage_->findChild<QCheckBox*>("pluginEnable_org.megavault.monochrome"); enable->setChecked(false);
            if(music->isVisible())return 45;
            auto* toolEnable=window.settingsPage_->findChild<QCheckBox*>("pluginEnable_org.megavault.text-tools");
            if(!toolEnable)return 46;toolEnable->setChecked(true);
            window.settingsPage_->reject();QApplication::processEvents();
            auto* tools=window.findChild<QPushButton*>("toolsSection");
            if(!tools||!tools->isVisible()||music->isVisible()){std::fprintf(stderr,"FAIL: independent Tools navigation absent\n");return 47;}
            tools->click();QApplication::processEvents();
            if(window.contentStack_->currentWidget()->objectName()!="extensionToolsPage")return 48;
            window.findChild<QPushButton*>("extensionBack")->click();QApplication::processEvents();
            if(window.contentStack_->currentWidget()!=window.pages_)return 49;
            window.lockVault(); QApplication::processEvents();
            std::puts("PASS actual settings -> independent Music/Tools navigation -> inline pages -> back -> lock"); return 0;
        }
        // Real encrypted photos exercise the decode/cache path as well as the
        // 20,000-row metadata fixture. All source pixels are generated here.
        for (int i = 1; i <= 64; ++i) {
            QImage image(640, 480, QImage::Format_RGB32);
            image.fill(QColor::fromHsv((i * 31) % 360, 160, 210));
            QPainter painter(&image);
            painter.setPen(Qt::white);
            painter.setFont(QFont(QStringLiteral("Arial"), 50));
            painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("Photo %1").arg(i));
            painter.end();
            const auto path = temp.path() + QStringLiteral("/generated-%1.jpg").arg(i, 5, 10, QLatin1Char('0'));
            if (!image.save(path, "JPEG", 85)) return 6;
            const auto imported = window.vault_->import_file(std::filesystem::path(path.toStdWString()));
            if (!imported || imported.value() != i) return 7;
            if (i <= 16 && !window.vault_->add_tag(i, "Photos")) return 14;
            if ((i == 1 || (i >= 17 && i <= 32)) && !window.vault_->add_tag(i, "Favorites")) return 14;
            if (i >= 33 && i <= 48 && !window.vault_->add_tag(i, "Work")) return 14;
        }
        window.sortKey_ = 0;
        window.resize(1100, 760);
        window.pages_->setCurrentIndex(2);
        window.contentStack_->setCurrentWidget(window.pages_);
        window.galleryStack_->setCurrentWidget(window.iconList_);
        window.show();
        QApplication::processEvents();
        // Production unlock populates the tag filter separately from rows.
        // Exercise that path too instead of displaying an empty combo box.
        window.refreshTagFilter();
        drain(window);
        if (!window.tagFilterButton_->selectedTags().isEmpty()
            || window.tagFilterButton_->text() != QStringLiteral("All items")) {
            std::fprintf(stderr, "FAIL: no selection must show the All items filter\n");
            return 12;
        }
        // Automated stress uses 20,000 rows; the interactive preview contains
        // only the 64 real encrypted photos, so filtering reflects real data.
        const int librarySize = qEnvironmentVariableIsSet("MVP_GALLERY_VISUAL") ? 64 : 20000;
        for (int i = 1; i <= librarySize; ++i) {
            MainWindow::PendingGalleryVideo row;
            row.id = i;
            row.name = QStringLiteral("generated-%1.%2").arg(i, 5, 10, QLatin1Char('0'))
                .arg(i <= 64 ? QStringLiteral("jpg") : QStringLiteral("bin"));
            row.size = 1024;
            window.galleryQueue_.push_back(std::move(row));
        }
        window.galleryListing_ = true;
        QElapsedTimer clock;
        clock.start();
        window.paintGalleryBatch();
        int peakTasks = 0;
        while (window.galleryListing_ && clock.elapsed() < 30000) {
            QApplication::processEvents();
            peakTasks = std::max(peakTasks, window.galleryTasksInFlight_);
            QThread::msleep(1);
        }
        if (window.iconList_->count() != librarySize || window.galleryListing_) {
            std::fprintf(stderr, "FAIL: 20,000 rows did not finish loading\n");
            return 3;
        }
        std::printf("%d metadata rows: %lld ms\n", librarySize, clock.elapsed());
        std::printf("Peak queued gallery tasks: %d\n", peakTasks);
        if (peakTasks > 4) {
            std::fprintf(stderr, "FAIL: thumbnail/metadata fan-out exceeded the four-task budget\n");
            return 11;
        }
        if (qEnvironmentVariableIsSet("MVP_GALLERY_VISUAL")) {
            window.setWindowTitle(QStringLiteral("Generated photo gallery — UI test"));
            window.setViewMode(1);
            window.decorateVisible();
            if (qEnvironmentVariableIsSet("MVP_SETTINGS_VISUAL")) {
                window.openSettings();
            }
            return QApplication::exec();
        }
        // Rapid navigation must not enqueue work proportional to the library.
        for (int row = 1000; row <= 10000; row += 1000) {
            window.iconList_->scrollToItem(window.iconList_->item(row), QAbstractItemView::PositionAtTop);
            window.decorateVisible();
            if (window.galleryTasksInFlight_ > 4) return 10;
        }
        drain(window);
        window.iconList_->scrollToTop();
        // Traverse more than the retention budget. Unknown-file badges use the
        // same decoded 320px pixmap and per-row ownership as photo thumbnails.
        for (int i = 1; i <= 384; ++i) {
            window.decorateVideo(i, QStringLiteral("generated.bin"), 1024);
        }
        drain(window);
        int resident = 0;
        for (int i = 0; i < window.iconList_->count(); ++i) {
            if (!window.iconList_->item(i)->data(Qt::UserRole + 4).value<QPixmap>().isNull()) ++resident;
        }
        std::printf("Resident decoded tiles after traversal: %d\n", resident);
        if (resident > 128) {
            std::fprintf(stderr, "FAIL: scrolling retains unbounded decoded thumbnails in row data\n");
            return 4;
        }
        // Resolution/duration/codec sort only needs metadata, not 20,000
        // decoded thumbnails. Probe an off-screen row through the real path.
        window.decorateVideo(19000, QStringLiteral("offscreen.bin"), 1024, false);
        drain(window);
        if (!window.iconList_->item(18999)->data(Qt::UserRole + 4).value<QPixmap>().isNull()) {
            std::fprintf(stderr, "FAIL: metadata-only sorting decoded an off-screen tile\n");
            return 5;
        }
        // Revisit an evicted real photo, and switch modes. Neither the cache
        // eviction nor metadata sorting may leave a permanent blank tile.
        window.decorateVideo(1, QStringLiteral("generated-00001.jpg"), 1024);
        drain(window);
        if (window.galleryIconItem(1)->data(Qt::UserRole + 4).value<QPixmap>().isNull()) return 8;
        window.setViewMode(2);
        window.setViewMode(0);
        window.setViewMode(1);
        drain(window);
        if (window.galleryTreeItem(1)->icon(0).isNull()) return 9;
        std::printf("Off-screen metadata-only probe, bounded queue, revisit and all view modes: OK\n");
        window.tagFilterButton_->setSelectedTags({QStringLiteral("Photos")});
        drain(window);
        if (window.iconList_->count() != 16) {
            std::fprintf(stderr, "FAIL: selecting Photos must show its 16 real files\n");
            return 15;
        }
        window.tagFilterButton_->setSelectedTags({QStringLiteral("Photos"), QStringLiteral("Favorites")});
        drain(window);
        if (window.iconList_->count() != 32) {
            std::fprintf(stderr, "FAIL: two tags must show their union without duplicate files\n");
            return 16;
        }
        window.refreshTagFilter();
        drain(window);
        if (window.tagFilterButton_->selectedTags().size() != 2) return 17;
        window.tagFilterButton_->setSelectedTags({});
        drain(window);
        if (window.iconList_->count() != 64) return 18;
        std::printf("Single tag, multi-tag union, selection retention and clear-to-all: OK\n");
        QSettings preferences;
        preferences.setValue(QStringLiteral("updates/checkAutomatically"), false);
        window.configureUpdateChecks();
        if (window.updateCheckTimer_->isActive() || window.updateChecking_) return 19;
        preferences.setValue(QStringLiteral("updates/lastCheckUtc"), QDateTime::currentDateTimeUtc());
        preferences.setValue(QStringLiteral("updates/lastCheckBeta"), false);
        preferences.setValue(QStringLiteral("updates/checkAutomatically"), true);
        window.configureUpdateChecks();
        if (!window.updateCheckTimer_->isActive() || window.updateChecking_) return 20;
        preferences.setValue(QStringLiteral("updates/checkAutomatically"), false);
        window.configureUpdateChecks();
        window.openSettings();
        if (!window.settingsPage_ || window.contentStack_->currentWidget() != window.settingsPage_
            || QApplication::activeModalWidget()) return 21;
        window.settingsPage_->reject();
        drain(window);
        if (window.settingsPage_ || window.contentStack_->currentWidget() != window.pages_) return 22;
        std::printf("Update preference scheduling and in-window settings navigation: OK\n");
        return 0;
    }
};
}

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    // Match production startup: native defaults turn the search field and
    // combo boxes white and make visual/performance results misleading.
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSize(10);
    app.setFont(font);
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    app.setOrganizationName(QStringLiteral("MegaVaultPerformanceTests"));
    app.setApplicationName(QStringLiteral("GeneratedGallery"));
    QTemporaryDir settings(qEnvironmentVariable("TMPDIR", QDir::tempPath())
        + QStringLiteral("/MVP-gallery-settings-XXXXXX"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    if (qEnvironmentVariableIsSet("MVP_GALLERY_THEME")) {
        QSettings().setValue(QStringLiteral("appearance/theme"), qEnvironmentVariable("MVP_GALLERY_THEME"));
    }
    videovault::app::initializeTheme(app);
    if (qEnvironmentVariableIsSet("MVP_LIBRARY_COMMANDS_TEST"))
        return videovault::app::GalleryPerformanceTestAccess::libraryCommands();
    return videovault::app::GalleryPerformanceTestAccess::run();
}
