// Offscreen settings regression tests. Only a generated vault and isolated INI settings.
#include "videovault/app/settings_dialog.hpp"
#include "videovault/app/proxy_settings.hpp"
#include "videovault/app/in_window_sheet.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QThread>
#include <QDir>
#include <QDialog>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

// Progress marks keep a CI timeout diagnosable: the last mark names the stage
// that stopped answering, which no assertion message can do once it hangs.
static void stageMark(const char* stage) {
    std::fprintf(stderr, "[stage] %s\n", stage);
    std::fflush(stderr);
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // The Windows offscreen plugin does not enumerate the native font database.
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
#endif
    QFont appFont(QStringLiteral("Segoe UI"));
    appFont.setPointSize(10);
    app.setFont(appFont);
    app.setPalette(videovault::app::darkPalette());
    app.setStyleSheet(videovault::app::appStyleSheet());
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    QCoreApplication::setOrganizationName("MegaVaultSettingsTests");
    QCoreApplication::setApplicationName("IsolatedSettings");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    try {
        stageMark("start");
#ifdef Q_OS_WIN
        const auto root = std::filesystem::path(temporary.path().toStdWString()) / L"vault";
#else
        const auto root = std::filesystem::u8path(temporary.path().toUtf8().constData()) / "vault";
#endif
        auto created = videovault::core::Vault::create(root, "settings-test-only", {8192, 1, 1});
        check(bool(created), "create generated test vault");
        auto vault = std::make_shared<videovault::core::Vault>(std::move(created.value()));
        videovault::app::SettingsDialog page(vault);
        check(qobject_cast<QDialog*>(&page) == nullptr, "settings must be an in-window QWidget, not QDialog");
        auto child = [&page]<typename T>(const char* name) {
            auto* result = page.findChild<T*>(QString::fromLatin1(name));
            check(result != nullptr, name); return result;
        };
        auto* tabs = child.operator()<QTabBar>("settingsNavigation");
        auto* sections = child.operator()<QStackedWidget>("settingsSections");
        check(page.findChild<QCheckBox*>("useProxy") != nullptr, "proxy settings switch must be reachable");
        QStringList sectionNames;
        for (int index = 0; index < tabs->count(); ++index) {
            sectionNames.append(tabs->tabText(index));
        }
        check(tabs->count() == sections->count(), "every settings tab has a page");
        check(sectionNames.value(0) == "Updates", "the first section is Updates");
        check(sectionNames.contains("Library"), "the library section is present");
        check(sectionNames.contains("Record templates"),
            "templates are managed in settings, not in the library");
        page.resize(480, 640); page.show(); app.processEvents();
        stageMark("page shown");
        {
            videovault::app::InWindowSheet sheet(&page);
            sheet.setTitle(QStringLiteral("Generated cancellation check"));
            sheet.addButton(QStringLiteral("Cancel"), 0, false);
            videovault::app::InWindowSheet::setTestDriver([](videovault::app::InWindowSheet* currentSheet) {
                currentSheet->finish(0);
            });
            check(sheet.run() == 0, "sheet driver must preserve cancellation instead of reporting Save");
            videovault::app::InWindowSheet::setTestDriver({});
        }
        int preferences = 0, libraryChanges = 0, appearanceChanges = 0, checks = 0, closes = 0;
        QObject::connect(&page, &videovault::app::SettingsDialog::updatePreferencesChanged, [&] { ++preferences; });
        QObject::connect(&page, &videovault::app::SettingsDialog::settingsChanged, [&] { ++libraryChanges; });
        QObject::connect(&page, &videovault::app::SettingsDialog::appearanceChanged, [&] { ++appearanceChanges; });
        QObject::connect(&page, &videovault::app::SettingsDialog::updateCheckRequested, [&] { ++checks; });
        QObject::connect(&page, &videovault::app::SettingsDialog::closed, [&] { ++closes; });
        const char* switches[]{"checkAutomatically", "downloadAutomatically", "includeBeta"};
        const bool defaults[]{true, false, false};
        for (int i = 0; i < 3; ++i) {
            auto* toggle = child.operator()<QCheckBox>(switches[i]);
            check(toggle->isChecked() == defaults[i], "update defaults");
            check(!toggle->accessibleName().isEmpty(), "named accessible switch");
            auto* accessible = QAccessible::queryAccessibleInterface(toggle);
            check(accessible && accessible->role() == QAccessible::CheckBox, "switch retains checkbox accessibility role");
            toggle->setFocus();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(toggle, &press); QApplication::sendEvent(toggle, &release);
            check(toggle->isChecked() != defaults[i], "space toggles actual switch");
            check(QSettings().value(QString("updates/") + switches[i]).toBool() == !defaults[i], "switch saved");
        }
        check(preferences == 3 && libraryChanges == 0, "preferences use separate signal");
        auto* manual = child.operator()<QPushButton>("checkUpdatesNow");
        manual->click(); check(checks == 1, "manual update request");
        page.setUpdateStatus("Checking test release", true);
        check(!manual->isEnabled(), "busy manual check disabled");
        manual->click(); check(checks == 1, "busy check cannot fire");
        check(child.operator()<QLabel>("updateStatus")->text() == "Checking test release", "inline update state");
        page.setUpdateStatus("Up to date"); check(manual->isEnabled(), "manual check restored");
        stageMark("updates section");
        tabs->setCurrentIndex(1); app.processEvents();
        check(child.operator()<QPushButton>("theme_dark")->isChecked(), "default dark appearance");
        for (const auto& theme : {QString("light"), QString("system"), QString("dark")}) {
            auto* button = page.findChild<QPushButton*>("theme_" + theme);
            button->click();
            check(button->isChecked(), "appearance selected");
            check(QSettings().value("appearance/theme").toString() == theme, "appearance saved");
        }
        check(appearanceChanges == 3 && libraryChanges == 0, "appearance isolated signal");
        stageMark("appearance section");
        tabs->setCurrentIndex(2); app.processEvents();
        check(page.findChild<QWidget*>("pluginManager") != nullptr, "real data-only plugin manager");
        check(!child.operator()<QCheckBox>("pluginEnable_org.megavault.monochrome")->isChecked(), "Monochrome starts disabled");
        check(child.operator()<QPushButton>("installPlugin")->isVisible(), "manifest installer reachable");
        stageMark("extensions section");
        tabs->setCurrentIndex(3); app.processEvents();
        auto* lock = child.operator()<QComboBox>("autoLockMinutes");
        check(lock->currentData().toInt() == 5 && lock->count() == 5, "auto-lock choices and default");
        lock->setCurrentIndex(lock->findData(15));
        check(QSettings().value("security/autoLockMinutes").toInt() == 15 && libraryChanges == 1, "auto-lock saves and resets host timer via signal");
        auto* profile = child.operator()<QComboBox>("securityProfile");
        check(profile->count() == 2 && profile->itemText(1).contains("512 MiB"), "existing Argon2id profiles");
        auto* current = child.operator()<QLineEdit>("currentPassword");
        auto* next = child.operator()<QLineEdit>("newPassword");
        auto* confirm = child.operator()<QLineEdit>("confirmPassword");
        check(current->echoMode() == QLineEdit::Password && next->echoMode() == QLineEdit::Password && confirm->echoMode() == QLineEdit::Password, "password fields hidden");
        current->setText("settings-test-only"); next->setText("new-test-password"); confirm->setText("mismatch");
        auto* rotate = child.operator()<QPushButton>("changePassword");
        rotate->click();
        check(child.operator()<QLabel>("settingsStatus")->text().contains("does not match"), "password mismatch inline validation");
        confirm->setText("new-test-password"); rotate->click();
        check(!rotate->isEnabled(), "async password operation busy");
        QElapsedTimer timer; timer.start();
        while (!rotate->isEnabled() && timer.elapsed() < 30000) { app.processEvents(); QThread::msleep(5); }
        check(rotate->isEnabled() && child.operator()<QLabel>("settingsStatus")->text() == "Password changed.", "real generated vault password rotation completes");
        check(current->text().isEmpty() && next->text().isEmpty() && confirm->text().isEmpty(), "password fields cleared after rotation");
        check(bool(videovault::core::Vault::validate_password(root, "new-test-password")), "new password opens generated vault");
        stageMark("security section");
        tabs->setCurrentIndex(4); app.processEvents();
        auto* tagList = child.operator()<QListWidget>("tagList");
        child.operator()<QLineEdit>("newTag")->setText("Settings test tag"); child.operator()<QPushButton>("addTag")->click();
        check(tagList->count() == 1, "add tag using real vault");
        tagList->setCurrentRow(0); tagList->item(0)->setText("Renamed test tag");
        auto storedTags = vault->list_tags();
        check(storedTags && storedTags.value().front().name == "Renamed test tag", "inline tag rename persists");
        tagList->item(0)->setText(" ");
        check(tagList->item(0)->text() == "Renamed test tag", "invalid rename restores previous name");
        child.operator()<QPushButton>("removeTag")->click();
        check(tagList->count() == 0, "delete tag");
        auto* regenerate = child.operator()<QPushButton>("regenerateMissing"); regenerate->click();
        timer.restart();
        while (!regenerate->isEnabled() && timer.elapsed() < 10000) { app.processEvents(); QThread::msleep(5); }
        check(regenerate->isEnabled() && child.operator()<QLabel>("settingsStatus")->text().contains("already has a thumbnail"), "empty library regeneration completes inline");
        {
            videovault::app::SettingsDialog reopened(vault);
            for (int i = 0; i < 3; ++i) check(reopened.findChild<QCheckBox*>(switches[i])->isChecked() == !defaults[i], "switch survives new page");
            check(reopened.findChild<QComboBox*>("autoLockMinutes")->currentData().toInt() == 15, "auto-lock survives new page");
            check(reopened.findChild<QPushButton*>("theme_dark")->isChecked(), "appearance survives new page");
        }
        stageMark("library section");
        for (const auto size : {QSize(1100, 760), QSize(480, 640)}) {
            page.resize(size);
            for (int index = 0; index < tabs->count(); ++index) {
                tabs->setCurrentIndex(index); app.processEvents();
                check(sections->currentIndex() == index, "navigation changes actual page");
                auto* scroll = qobject_cast<QScrollArea*>(sections->currentWidget());
                if (scroll && scroll->widget()->width() > scroll->viewport()->width()) {
                    std::cerr << "layout size=" << size.width() << " section=" << index << " content=" << scroll->widget()->width() << " viewport=" << scroll->viewport()->width() << '\n';
                    // Name the controls that force the width so a font-dependent
                    // failure on another platform is diagnosable from CI logs.
                    std::vector<std::pair<int, std::string>> widest;
                    for (auto* child : scroll->widget()->findChildren<QWidget*>()) {
                        const int minimum = child->minimumSizeHint().width();
                        if (minimum < 200) continue;
                        std::string name = child->metaObject()->className();
                        if (!child->objectName().isEmpty()) name += " '" + child->objectName().toStdString() + "'";
                        if (auto* text = qobject_cast<QLabel*>(child)) name += " text='" + text->text().left(40).toStdString() + "'";
                        if (auto* button = qobject_cast<QAbstractButton*>(child)) name += " text='" + button->text().left(40).toStdString() + "'";
                        widest.emplace_back(minimum, name);
                    }
                    std::sort(widest.rbegin(), widest.rend());
                    for (std::size_t i = 0; i < widest.size() && i < 8; ++i)
                        std::cerr << "  min=" << widest[i].first << ' ' << widest[i].second << '\n';
                }
                check(scroll && scroll->widget()->width() <= scroll->viewport()->width(), "no horizontally clipped content");
            }
        }
        // A theme switch must reach live controls, not only a newly built page.
        // This goes through the real engine, which is what the app does: setting
        // a palette directly and hoping Qt re-resolves style-sheet colours is
        // exactly the path that differs between Qt 5.15 builds.
        stageMark("responsive layout");
        videovault::app::initializeTheme(app);
        videovault::app::setThemeMode(videovault::app::ThemeMode::Light);
        tabs->setCurrentIndex(0);
        app.processEvents();
        const QPalette light = videovault::app::lightPalette();
        check(current->palette().color(QPalette::Text) == light.color(QPalette::Text), "live field palette updates to light");
        check(current->palette().color(QPalette::Base) == light.color(QPalette::Base), "live field background follows the light theme");
        auto* secondary = child.operator()<QLabel>("updateStatus");
        check(secondary->palette().color(secondary->foregroundRole()) == light.color(QPalette::PlaceholderText), "secondary copy remains readable after palette change");
        if (!qEnvironmentVariableIsEmpty("MVP_SETTINGS_SCREENSHOTS")) {
            const QString folder = qEnvironmentVariable("MVP_SETTINGS_SCREENSHOTS"); QDir().mkpath(folder);
            page.resize(480, 640); app.processEvents(); page.grab().save(folder + "/settings-light-480.png");
            page.resize(1100, 760); app.processEvents(); page.grab().save(folder + "/settings-light-1100.png");
        }
        videovault::app::setThemeMode(videovault::app::ThemeMode::Dark);
        app.processEvents();
        const QPalette dark = videovault::app::darkPalette();
        check(current->palette().color(QPalette::Text) == dark.color(QPalette::Text), "live field palette returns to dark");
        if (!qEnvironmentVariableIsEmpty("MVP_SETTINGS_SCREENSHOTS")) {
            const QString folder = qEnvironmentVariable("MVP_SETTINGS_SCREENSHOTS");
            page.resize(480, 640); app.processEvents(); page.grab().save(folder + "/settings-dark-480.png");
            tabs->setCurrentIndex(3); app.processEvents(); page.grab().save(folder + "/settings-security-480.png");
        }
        tabs->setCurrentIndex(5); app.processEvents();
        auto* proxySwitch = child.operator()<QCheckBox>("useProxy");
        auto* proxyList = child.operator()<QListWidget>("proxyList");
        check(proxyList->count() == 0 && !proxySwitch->isChecked(), "empty proxies start disabled");
        // Validation is a pure function, so the rules are asserted without a dialog.
        {
            videovault::app::ProxySettings bad;
            videovault::app::ProxyEntry badEntry;
            badEntry.id = "x"; badEntry.server = "has space"; badEntry.port = 1080;
            bad.entries.append(badEntry);
            check(!videovault::app::validateProxySettings(bad).isEmpty(), "invalid server is rejected");
            badEntry.server = "proxy.example.test"; badEntry.port = 70000;
            bad.entries[0] = badEntry;
            check(!videovault::app::validateProxySettings(bad).isEmpty(), "out-of-range port is rejected");
        }
        stageMark("theme switch");
        // The editor is an in-window sheet: the driver stands in for the user,
        // exactly where a person would click, so the real save path runs.
        // Linux CI has no OS keyring. Test its refusal/cancel path instead of
        // clicking Save forever when secure credential persistence is unavailable.
        QString credentialError;
        const bool canSealCredentials = videovault::app::saveProxySettings({}, &credentialError);
#ifdef Q_OS_WIN
        check(canSealCredentials, "Windows DPAPI credential storage must be available");
#endif
        QString shotFolder = qEnvironmentVariable("MVP_SETTINGS_SCREENSHOTS");
        bool grabbed = false;
        int editorVisits = 0;
        videovault::app::InWindowSheet::setTestDriver(
            [&](videovault::app::InWindowSheet* sheet) {
                ++editorVisits;
                check(editorVisits <= 2, "proxy editor must not enter an unbounded save retry loop");
                if (editorVisits == 2) {
                    check(!canSealCredentials, "proxy save unexpectedly failed with an available credential store");
                    bool showsError = false;
                    for (auto* label : sheet->findChildren<QLabel*>())
                        showsError |= label->text().contains(credentialError);
                    check(!credentialError.isEmpty() && showsError, "unavailable credential store must be explained inline");
                    sheet->findChild<QPushButton*>("cancelProxy")->click();
                    return;
                }
                auto* server = sheet->findChild<QLineEdit*>("proxyServer");
                auto* port = sheet->findChild<QLineEdit*>("proxyPort");
                auto* user = sheet->findChild<QLineEdit*>("proxyUsername");
                auto* pass = sheet->findChild<QLineEdit*>("proxyPassword");
                check(server && port && user && pass, "proxy sheet carries its fields");
                check(pass->echoMode() == QLineEdit::Password, "proxy password is masked");
                server->setText("proxy.example.test");
                port->setText("1080");
                user->setText("test-only-user");
                pass->setText("test-only-secret");
                app.processEvents();
                if (!shotFolder.isEmpty() && !grabbed) {
                    // The sheet covers the settings window: grab the window, which
                    // is what a person sees, including the blurred backdrop.
                    grabbed = true;
                    page.window()->grab().save(shotFolder + "/proxy-sheet-dark.png");
                }
                sheet->findChild<QPushButton*>("saveProxy")->click();
            });
        child.operator()<QPushButton>("addProxy")->click(); app.processEvents();
        if (!canSealCredentials) {
            check(editorVisits == 2 && proxyList->count() == 0 && !proxySwitch->isChecked(),
                "unavailable keyring must reject saving and allow cancellation without enabling a proxy");
            check(QSettings().value("network/proxyStore").toByteArray().isEmpty(),
                "failed credential sealing must not persist plaintext settings");
            check(!page.window()->findChild<QWidget*>("inWindowSheet"), "cancel closes failed proxy editor");
        } else {
        check(proxyList->count() == 1, "proxy saved through the in-window sheet");
        check(!page.window()->findChild<QWidget*>("inWindowSheet"), "sheet closes after saving");
        proxySwitch->click(); check(proxySwitch->isChecked(), "saved proxy enables switch");
        if (!shotFolder.isEmpty()) {
            page.resize(480, 640); app.processEvents(); page.grab().save(shotFolder + "/proxy-list-dark-480.png");
        }
#ifdef Q_OS_WIN
        videovault::app::InWindowSheet::setTestDriver(
            [&](videovault::app::InWindowSheet* sheet) {
                check(sheet->findChild<QLineEdit*>("proxyServer")->text() == "proxy.example.test", "edit restores server");
                check(sheet->findChild<QLineEdit*>("proxyPassword")->text() == "test-only-secret", "edit restores securely stored password");
                sheet->findChild<QRadioButton*>("proxyHttp")->setChecked(true);
                sheet->findChild<QLineEdit*>("proxyPort")->setText("8080");
                app.processEvents();
                if (!shotFolder.isEmpty()) {
                    // A short window is where a fixed-height card used to clip
                    // half of every field; the body has to scroll instead.
                    const QSize was = page.window()->size();
                    page.window()->resize(460, 380);
                    app.processEvents();
                    page.window()->grab().save(shotFolder + "/proxy-sheet-short.png");
                    page.window()->resize(was);
                    app.processEvents();
                    page.window()->grab().save(shotFolder + "/proxy-sheet-light.png");
                }
                sheet->findChild<QPushButton*>("saveProxy")->click();
            });
        videovault::app::setThemeMode(videovault::app::ThemeMode::Light);
        child.operator()<QPushButton>("editProxy")->click(); app.processEvents();
        check(proxyList->item(0)->text().contains("8080") && proxyList->item(0)->text().contains("HTTP"), "edit updates protocol and port");
        videovault::app::setThemeMode(videovault::app::ThemeMode::Dark);
        app.processEvents();
        child.operator()<QPushButton>("deleteProxy")->click();
        check(proxyList->count() == 0 && !proxySwitch->isChecked(), "deleting active proxy disables use");
#else
        child.operator()<QPushButton>("deleteProxy")->click();
#endif
        }
        videovault::app::InWindowSheet::setTestDriver({});
        page.reject(); check(closes == 1 && page.isVisible(), "reject requests host navigation without destroying or hiding page");
        std::cout << "Settings page tests passed: switches, accessibility, updates, appearance, navigation, security, tags, thumbnails, persistence, responsive layout, live palette\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
