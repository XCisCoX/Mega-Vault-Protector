#include "videovault/app/theme.hpp"

#include <QApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QHash>
#include <QEvent>
#include <QScopedValueRollback>
#include <QWidget>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <utility>
#include <vector>

namespace videovault::app {
namespace {
constexpr auto themeKey = "appearance/theme";

class ThemeManager final : public QObject {
public:
    explicit ThemeManager(QApplication& app)
        : QObject(&app), app_(app), nativePalette_(app.palette()) {
        timer_.setParent(this);
        timer_.setInterval(1000);
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (mode_ == ThemeMode::System && systemIsDark() != dark_) apply(mode_);
        });
        timer_.start();
        app_.installEventFilter(this);
        connect(&app_, &QGuiApplication::paletteChanged, this, [this](const QPalette& palette) {
            if (applying_) return;
#ifndef Q_OS_WIN
            // On Windows the registry decides; elsewhere the platform palette is
            // the only signal a System preference has changed.
            nativePalette_ = palette;
#else
            Q_UNUSED(palette);
#endif
            // A palette published from outside this object -- the platform theme,
            // a plugin, a test -- must never win. Re-apply here rather than in a
            // queued call: Qt hands the incoming palette to the widgets that are
            // already polished, so a deferred fix leaves the app visibly wrong
            // for as long as the event loop takes to run it, and on some Qt 5.15
            // builds a style-sheet colour is resolved once and never again.
            // apply() sets applying_ for its whole body, so the nested
            // paletteChanged it causes returns immediately.
            apply(mode_);
        });
    }
    bool systemIsDark() const {
#ifdef Q_OS_WIN
        const QSettings preferences(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                                    QSettings::NativeFormat);
        bool valid = false;
        const int light = preferences.value(QStringLiteral("AppsUseLightTheme")).toInt(&valid);
        if (valid) return light == 0;
#endif
        return nativePalette_.color(QPalette::Window).lightness()
            < nativePalette_.color(QPalette::WindowText).lightness();
    }
    void apply(ThemeMode mode) {
        const QScopedValueRollback<bool> guard(applying_, true);
        // Capture sources before global repolishing emits StyleChange events.
        for (auto* widget : QApplication::allWidgets()) remember(widget);
        mode_ = mode;
        dark_ = mode == ThemeMode::Dark || (mode == ThemeMode::System && systemIsDark());
        app_.setPalette(dark_ ? darkPalette() : lightPalette());
        app_.setStyleSheet(themedStyleSheet(appStyleSheet()));
        for (auto* widget : QApplication::allWidgets()) adapt(widget);
        {
            // Make every widget resolve its style-sheet colours again against the
            // new palette. Newer Qt 5.15 does this when the application style
            // sheet changes; 5.15.2 keeps the palette a polished widget cached,
            // which left a live QLineEdit on the previous theme's colours.
            const QScopedValueRollback<bool> guard(adapting_, true);
            for (auto* widget : QApplication::allWidgets()) {
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
                widget->update();
            }
        }
    }
    // Re-apply the mode that is in force. Public callers use it after they have
    // changed the application palette themselves.
    void refresh() { apply(mode_); }
    bool dark() const { return dark_; }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (!applying_ && !adapting_ && (event->type() == QEvent::StyleChange
            || event->type() == QEvent::Polish || event->type() == QEvent::Show
            || event->type() == QEvent::ParentChange)) {
            if (auto* widget = qobject_cast<QWidget*>(object)) adapt(widget);
        }
        return QObject::eventFilter(object, event);
    }
private:
    struct LocalStyle { QString source; QString applied; };
    static bool darkSurface(const QWidget* widget) {
        for (auto* ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
            if (ancestor->property("preserveDarkTheme").toBool()
                || ancestor->inherits("videovault::app::PlayerWindow")
                || ancestor->inherits("videovault::app::PdfWindow")) return true;
        }
        return false;
    }
    void remember(QWidget* widget) {
        if (darkSurface(widget)) return;
        const QString current = widget->styleSheet();
        auto existing = styles_.find(widget);
        if (existing == styles_.end()) {
            if (current.isEmpty()) return;
            styles_.insert(widget, {current, current});
            connect(widget, &QObject::destroyed, this, [this, widget] { styles_.remove(widget); });
        } else if (current != existing->applied) {
            existing->source = current;
            existing->applied = current;
        }
    }
    void adapt(QWidget* widget) {
        const QScopedValueRollback<bool> guard(adapting_, true);
        if (darkSurface(widget)) {
            // Media canvases and their overlay controls retain dark contrast.
            if (widget->inherits("videovault::app::PlayerWindow")
                || widget->inherits("videovault::app::PdfWindow")
                || widget->property("preserveDarkTheme").toBool()) widget->setPalette(darkPalette());
            return;
        }
        remember(widget);
        auto existing = styles_.find(widget);
        if (existing == styles_.end()) return;
        const QString next = themedStyleSheet(existing->source);
        existing->applied = next; // Set before StyleChange can be delivered.
        if (widget->styleSheet() != next) widget->setStyleSheet(next);
    }
    QHash<QWidget*, LocalStyle> styles_;
    bool applying_{false};
    bool adapting_{false};
    QApplication& app_;
    QPalette nativePalette_;
    QTimer timer_;
    ThemeMode mode_{ThemeMode::Dark};
    bool dark_{true};
};
QPointer<ThemeManager> manager;
} // namespace

ThemeMode savedThemeMode() {
    const QString value = QSettings().value(QString::fromLatin1(themeKey), QStringLiteral("dark")).toString();
    if (value == QStringLiteral("system")) return ThemeMode::System;
    if (value == QStringLiteral("light")) return ThemeMode::Light;
    return ThemeMode::Dark;
}
void setThemeMode(ThemeMode mode) {
    const QString value = mode == ThemeMode::System ? QStringLiteral("system")
        : mode == ThemeMode::Light ? QStringLiteral("light") : QStringLiteral("dark");
    QSettings settings;
    settings.setValue(QString::fromLatin1(themeKey), value);
    settings.sync();
    if (manager) manager->apply(mode);
}
void initializeTheme(QApplication& application) {
    if (!manager) manager = new ThemeManager(application);
    manager->apply(savedThemeMode());
}
void refreshTheme() {
    if (manager) manager->refresh();
}
bool isDarkTheme() {
    return manager ? manager->dark() : savedThemeMode() != ThemeMode::Light;
}
QPalette lightPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#f2f2f7")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#1c1c1e")));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#eaeaef")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#1c1c1e")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#636366")));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#1c1c1e")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#3390ec")));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8e8e93")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#8e8e93")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#d1d1d6")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#aeaeb2")));
    return palette;
}
QString themedStyleSheet(const QString& darkStyle) {
    if (isDarkTheme()) return darkStyle;
    // Transform declarations, never selectors, strings, comments, or URLs.
    // A single token pass avoids cascades (a light output may be a dark input).
    static const QRegularExpression declarations(
        QStringLiteral(R"((?:^|[;{])\s*([\w-]+)\s*:\s*([^;{}]*))"));
    static const QRegularExpression tokens(QStringLiteral(
        R"re(/\*[\s\S]*?\*/|"[^"]*"|'[^']*'|url\([^)]*\)|rgba?\([^)]*\)|#[\da-fA-F]{3,8}\b|\bwhite\b)re"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rgba(QStringLiteral(
        R"(rgba\(\s*255\s*,\s*255\s*,\s*255\s*(,\s*[^)]+)\))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression accentBackground(QStringLiteral(
        R"((?:^|[;{])\s*background(?:-color)?\s*:\s*#(?:3390ec|4ba0f5|4aa0f0|1d5b96)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    // Resolve to concrete RGB values, not palette(text): Qt stylesheet
    // palettes can still contain the previous local stylesheet's white text
    // during a live repolish. RGB outputs also cannot cascade through hex keys.
    static const QHash<QString, QString> colors{
        {"#000", "rgb(242, 242, 247)"}, {"#000000", "rgb(242, 242, 247)"},
        {"#101012", "rgb(234, 234, 239)"}, {"#101014", "rgb(242, 242, 247)"},
        {"#121214", "rgb(255, 255, 255)"}, {"#141416", "rgb(255, 255, 255)"},
        {"#161618", "rgb(255, 255, 255)"}, {"#17171c", "rgb(255, 255, 255)"},
        {"#1c1c1e", "rgb(255, 255, 255)"}, {"#242426", "rgb(229, 229, 234)"},
        {"#2c2c2e", "rgb(229, 229, 234)"}, {"#3a3a3c", "rgb(199, 199, 204)"},
        {"#48484a", "rgb(174, 174, 178)"}, {"#fff", "rgb(28, 28, 30)"},
        {"#ffffff", "rgb(28, 28, 30)"}, {"white", "rgb(28, 28, 30)"},
        {"#f2f2f7", "rgb(28, 28, 30)"}, {"#d1d1d6", "rgb(58, 58, 60)"},
        {"#d0d0d4", "rgb(58, 58, 60)"}, {"#aeaeb2", "rgb(99, 99, 102)"},
        {"#a8a8ae", "rgb(99, 99, 102)"}, {"#999999", "rgb(99, 99, 102)"},
        {"#8e8e93", "rgb(99, 99, 102)"}, {"#9a9aa0", "rgb(115, 115, 120)"},
        {"#636366", "rgb(142, 142, 147)"}, {"#6d6d72", "rgb(142, 142, 147)"},
        {"#2c1518", "rgb(255, 235, 237)"}, {"#5c2a30", "rgb(240, 189, 195)"},
        {"#ff6b6b", "rgb(198, 40, 40)"}, {"#ff453a", "rgb(198, 40, 40)"},
        {"#8ec8ff", "rgb(23, 103, 178)"}, {"#ff8f85", "rgb(183, 28, 28)"},
        // Accents and semantic reds are theme-invariant: declared here so every
        // stylesheet colour is accounted for by this map (see tests/scripts).
        {"#3390ec", "rgb(51, 144, 236)"}, {"#4ba0f5", "rgb(75, 160, 245)"},
        {"#5aa6f0", "rgb(90, 166, 240)"}, {"#c62828", "rgb(198, 40, 40)"}
    };
    QString result;
    int cursor = 0;
    // A comment is prose, not CSS: "#fff" inside it must survive verbatim.
    static const QRegularExpression commentPattern(QStringLiteral("/\\*[\\s\\S]*?\\*/"));
    std::vector<std::pair<int, int>> comments;
    for (auto found = commentPattern.globalMatch(darkStyle); found.hasNext();) {
        const auto comment = found.next();
        comments.emplace_back(comment.capturedStart(), comment.capturedEnd());
    }
    const auto inComment = [&comments](const int position) {
        for (const auto& span : comments) {
            if (position > span.first && position < span.second) return true;
        }
        return false;
    };
    auto matches = declarations.globalMatch(darkStyle);
    while (matches.hasNext()) {
        const auto declaration = matches.next();
        const int start = declaration.capturedStart(2);
        if (inComment(start)) continue;
        const QString property = declaration.captured(1).toLower();
        if (!(property.contains(QStringLiteral("color")) || property.startsWith(QStringLiteral("background"))
              || property.startsWith(QStringLiteral("border")))) continue;
        const int end = declaration.capturedEnd(2);
        const int blockStart = darkStyle.lastIndexOf(QLatin1Char('{'), start);
        const int blockEnd = darkStyle.indexOf(QLatin1Char('}'), end);
        const QString block = darkStyle.mid(blockStart + 1,
            (blockEnd < 0 ? darkStyle.size() : blockEnd) - blockStart - 1);
        const bool whiteOnAccent = property == QStringLiteral("color") && accentBackground.match(block).hasMatch();
        const QString value = declaration.captured(2);
        result += darkStyle.mid(cursor, start - cursor);
        int valueCursor = 0;
        auto colorMatches = tokens.globalMatch(value);
        while (colorMatches.hasNext()) {
            const auto token = colorMatches.next();
            const QString color = token.captured().toLower();
            QString replacement = colors.value(color, token.captured());
            if (whiteOnAccent && (color == QStringLiteral("white") || color == QStringLiteral("#ffffff")
                                  || color == QStringLiteral("#fff") || color == QStringLiteral("#f2f2f7")))
                replacement = QStringLiteral("palette(highlighted-text)");
            const auto alpha = rgba.match(color);
            if (alpha.hasMatch())
                replacement = alpha.captured(1).isEmpty() ? QStringLiteral("rgb(28, 28, 30)")
                    : QStringLiteral("rgba(28, 28, 30%1)").arg(alpha.captured(1));
            result += value.mid(valueCursor, token.capturedStart() - valueCursor) + replacement;
            valueCursor = token.capturedEnd();
        }
        result += value.mid(valueCursor);
        cursor = end;
    }
    return result + darkStyle.mid(cursor);
}
} // namespace videovault::app
