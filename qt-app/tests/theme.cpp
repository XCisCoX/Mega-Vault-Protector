#include "videovault/app/theme.hpp"
#include "videovault/app/glass.hpp"
#include "videovault/app/tag_filter_button.hpp"
#include <QApplication>
#include <QSettings>
#include <QStyle>
#include <QTemporaryDir>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QImage>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>
#include <iostream>
#include <stdexcept>

using namespace videovault::app;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QPalette initialNativePalette = app.palette();
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName(QStringLiteral("MegaVaultThemeTests"));
    app.setApplicationName(QStringLiteral("Theme"));
    try {
        require(savedThemeMode() == ThemeMode::Dark, "missing preference preserves dark appearance");
        setThemeMode(ThemeMode::Light);
        require(savedThemeMode() == ThemeMode::Light, "light preference round trip");
        require(QSettings().value(QStringLiteral("appearance/theme")).toString() == QStringLiteral("light"), "stable settings key and value");
        initializeTheme(app);
        require(!isDarkTheme(), "initialize applies saved light preference");
        require(app.palette().color(QPalette::Window).lightness() > 200, "light application background");
        setThemeMode(ThemeMode::Dark);
        require(isDarkTheme(), "live dark mode");
        require(app.palette().color(QPalette::Window) == QColor(Qt::black), "original black field");
        require(app.styleSheet() == appStyleSheet(), "dark stylesheet unchanged");
        std::cout << "PASS: saved mode and live application palette\n";
        const QString source = QStringLiteral("QLabel#ffffff { color: #FFFFFF; background: #1c1c1e; border: 1px solid rgba(255, 255, 255, 28); } QPushButton[primary=\"true\"] { color: white; background: #3390ec; }");
        setThemeMode(ThemeMode::Light);
        const QString converted = themedStyleSheet(source);
        require(converted != source, "light converts dark inline styles");
        require(themedStyleSheet(converted) == converted, "light token transform is idempotent");
        const QString comment = QStringLiteral("/* old { color: #fff; background: #000; } */");
        require(themedStyleSheet(comment + source).startsWith(comment), "stylesheet comments are preserved");
        require(themedStyleSheet(QStringLiteral("border-image: url(\":/white/#ffffff.png\");"))
                    == QStringLiteral("border-image: url(\":/white/#ffffff.png\");"), "resource URLs are preserved");
        require(converted.contains(QStringLiteral("QLabel#ffffff")), "selectors are not color tokens");
        require(converted.contains(QStringLiteral("background: #3390ec")), "accent preserved");
        QLabel label;
        label.setStyleSheet(themedStyleSheet(QStringLiteral("color: #f2f2f7; background: #1c1c1e;")));
        label.ensurePolished();
        require(label.palette().color(QPalette::WindowText).lightness() < 80, "light converted foreground is dark");
        require(label.palette().color(QPalette::Window).lightness() > 220, "light converted panel is bright");
        QPushButton primary;
        primary.setProperty("primary", true);
        primary.setStyleSheet(converted);
        primary.ensurePolished();
        require(primary.palette().color(QPalette::ButtonText).lightness() > 220, "white primary text stays readable on blue");
        const QPalette light = lightPalette();
        require(light.color(QPalette::Text).lightness() < 80 && light.color(QPalette::Base).lightness() > 220,
                "light palette contrast");
        setThemeMode(ThemeMode::Dark);
        require(themedStyleSheet(source) == source, "dark transformation is exactly identity");
        std::cout << "PASS: atomic local style transformation and contrast\n";
        const QString local = QStringLiteral("color: #f2f2f7; background: #1c1c1e;");
        QLabel live;
        live.setStyleSheet(local);
        live.ensurePolished();
        setThemeMode(ThemeMode::Light);
        require(live.styleSheet() != local, "existing local styles adapt live");
        const QString lightLocal = live.styleSheet();
        for (int i = 0; i < 5; ++i) {
            setThemeMode(ThemeMode::Dark);
            require(live.styleSheet() == local, "dark restores original local stylesheet");
            setThemeMode(ThemeMode::Light);
            require(live.styleSheet() == lightLocal, "no cumulative conversion across toggles");
        }
        QLabel createdInLight;
        createdInLight.setStyleSheet(local);
        createdInLight.ensurePolished();
        require(createdInLight.styleSheet() == lightLocal, "new widget styles adapt in light mode");
        const QString changed = QStringLiteral("color: #8e8e93; background: #161618;");
        live.setStyleSheet(changed);
        require(live.styleSheet() == themedStyleSheet(changed), "dynamic local styles adapt immediately");
        live.setStyleSheet(QString());
        setThemeMode(ThemeMode::Dark);
        require(live.styleSheet().isEmpty(), "clearing a local stylesheet is retained");
        require(createdInLight.styleSheet() == local, "new widgets restore their dark source");
        std::cout << "PASS: local styles, dynamic updates, new widgets, toggle stability\n";
        FrostedBar glass(FrostedBar::Band::Top);
        glass.resize(160, 40);
        TagFilterButton tags;
        tags.resize(180, 38);
        tags.setTags({{QStringLiteral("Work"), 4}, {QStringLiteral("Travel"), 2}});
        tags.setSelectedTags({QStringLiteral("Work")});
        tags.show();
        tags.click();
        app.processEvents();
        auto* popup = tags.findChild<QWidget*>(QStringLiteral("tagFilterPopup"));
        auto* search = popup ? popup->findChild<QLineEdit*>(QStringLiteral("tagSearch")) : nullptr;
        auto* rows = popup ? popup->findChild<QListWidget*>(QStringLiteral("tagList")) : nullptr;
        require(popup && search && rows, "tag popup created");
        search->setText(QStringLiteral("wor"));
        rows->setCurrentRow(0);
        const QImage darkGlass = glass.grab().toImage();
        const QImage darkButton = tags.grab().toImage();
        require(darkGlass.pixelColor(20, 20).lightness() < 40, "original dark glass veil");
        setThemeMode(ThemeMode::Light);
        app.processEvents();
        require(glass.grab().toImage().pixelColor(20, 20).lightness() > 200, "glass veil follows light theme");
        require(tags.grab().toImage().pixelColor(20, 6).lightness() > 200, "tag custom button follows light theme");
        require(popup->grab().toImage().pixelColor(5, popup->height() / 2).lightness() > 200,
                "open popup follows light theme");
        if (search->palette().color(QPalette::Text).lightness() >= 80) {
            std::cerr << "popup QSS: " << popup->styleSheet().toStdString()
                      << "\nsearch palette text=" << search->palette().color(QPalette::Text).name().toStdString()
                      << " app=" << app.palette().color(QPalette::Text).name().toStdString() << '\n';
        }
        require(search->palette().color(QPalette::Text).lightness() < 80, "popup search light foreground");
        require(popup->isVisible() && search->text() == QStringLiteral("wor") && rows->currentRow() == 0
                && rows->item(1)->isHidden() && rows->item(0)->checkState() == Qt::Checked
                && tags.selectedTags() == QStringList{QStringLiteral("Work")}, "restyling retains open popup state");
        setThemeMode(ThemeMode::Dark);
        require(glass.grab().toImage() == darkGlass, "glass dark rendering restored");
        require(tags.grab().toImage() == darkButton, "button dark rendering restored");
        std::cout << "PASS: glass, tag painters and open popup live state\n";
        popup->hide();
        bool expectedSystemDark = initialNativePalette.color(QPalette::Window).lightness()
            < initialNativePalette.color(QPalette::WindowText).lightness();
#ifdef Q_OS_WIN
        QSettings windows(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QSettings::NativeFormat);
        bool valid = false;
        const int systemLight = windows.value(QStringLiteral("AppsUseLightTheme")).toInt(&valid);
        if (valid) expectedSystemDark = systemLight == 0;
#endif
        setThemeMode(ThemeMode::System);
        require(savedThemeMode() == ThemeMode::System && QSettings().value(QStringLiteral("appearance/theme")).toString() == QStringLiteral("system"), "system saved value");
        require(isDarkTheme() == expectedSystemDark, "system follows actual Windows app preference or native Qt fallback");
        QEventLoop wait;
        QTimer::singleShot(1100, &wait, &QEventLoop::quit);
        setThemeMode(ThemeMode::Light);
        wait.exec();
        require(!isDarkTheme() && app.palette().color(QPalette::Window).lightness() > 200, "system polling does not override explicit light");
        QPalette foreign = darkPalette();
        app.setPalette(foreign);
        app.processEvents();
        require(!isDarkTheme() && app.palette().color(QPalette::Window).lightness() > 200, "native palette notifications cannot override explicit light");
#ifndef Q_OS_WIN
        setThemeMode(ThemeMode::System);
        app.setPalette(lightPalette());
        app.processEvents();
        require(!isDarkTheme(), "system follows native light palette notification");
        app.setPalette(darkPalette());
        app.processEvents();
        require(isDarkTheme(), "system follows native dark palette notification");
#endif
        QSettings().setValue(QStringLiteral("appearance/theme"), QStringLiteral("unknown"));
        require(savedThemeMode() == ThemeMode::Dark, "invalid preference safely falls back dark");
        initializeTheme(app);
        require(isDarkTheme(), "repeat initialization reapplies saved preference");
        std::cout << "PASS: system preference, polling, explicit overrides and invalid preference\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
