// Large-library regression against the actual gallery widgets and decorators.
#include "videovault/app/main_window.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/app/tag_filter_button.hpp"
#include "videovault/app/settings_dialog.hpp"
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
#include <cstdio>

namespace videovault::app {
struct GalleryPerformanceTestAccess {
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
    static int run() {
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
    return videovault::app::GalleryPerformanceTestAccess::run();
}
