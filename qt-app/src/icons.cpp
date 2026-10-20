#include "videovault/app/icons.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace videovault::app::icons {
namespace {

constexpr int kSizes[] = {16, 20, 32, 64};

QPixmap tinted(const int size, const QColor& color, const QString& name) {
    const QString path = QStringLiteral(":/icons/lucide/") + name + QLatin1Char('-')
        + QString::number(size) + QStringLiteral(".png");
    QPixmap source(path);
    if (source.isNull()) {
        return QPixmap();
    }
    if (source.width() != size) {
        source = source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QPixmap result(source.size());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.drawPixmap(0, 0, source);
    // SourceIn keeps the glyph's alpha and replaces its colour: the masters ship as
    // one flat ink precisely so a single pixmap can be re-tinted for any theme.
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(result.rect(), color);
    painter.end();
    return result;
}

QColor toneColor(const QPalette& palette, const Tone tone) {
    QColor color = palette.color(QPalette::ButtonText);
    if (tone == Tone::Quiet) {
        color.setAlpha(150);
    }
    return color;
}

QColor activeColor(const QPalette& palette, const Tone tone) {
    QColor color = palette.color(QPalette::ButtonText);
    if (tone == Tone::Quiet) {
        color.setAlpha(255);
    }
    return color;
}

QColor disabledColor(const QPalette& palette) {
    return palette.color(QPalette::Disabled, QPalette::ButtonText);
}

QIcon build(const QString& name, const Tone tone, const QPalette& palette) {
    QIcon icon;
    for (const int size : kSizes) {
        const QPixmap normal = tinted(size, toneColor(palette, tone), name);
        if (normal.isNull()) {
            continue;
        }
        icon.addPixmap(normal, QIcon::Normal);
        icon.addPixmap(tinted(size, activeColor(palette, tone), name), QIcon::Active);
        icon.addPixmap(tinted(size, disabledColor(palette), name), QIcon::Disabled);
    }
    return icon;
}

} // namespace

QIcon lucide(const QString& name, const Tone tone) {
    return build(name, tone, QApplication::palette());
}

QIcon lucideSolid(const QString& name, const QColor& color) {
    QIcon icon;
    for (const int size : kSizes) {
        const QPixmap normal = tinted(size, color, name);
        if (normal.isNull()) {
            continue;
        }
        icon.addPixmap(normal, QIcon::Normal);
        icon.addPixmap(normal, QIcon::Active);
        QColor faded = color;
        faded.setAlpha(110);
        icon.addPixmap(tinted(size, faded, name), QIcon::Disabled);
    }
    return icon;
}

void withText(QAbstractButton* button, const QString& name, const int size) {
    if (button == nullptr) {
        return;
    }
    button->setIconSize(QSize(size, size));
    const auto repaint = [button, name, size](const QPalette& palette) {
        // A primary button sits on the accent fill, so its glyph has to be the
        // highlighted text colour: ButtonText on blue is barely legible.
        const QColor ink = button->property("primary").toBool()
            ? palette.color(QPalette::HighlightedText)
            : palette.color(QPalette::ButtonText);
        QIcon icon;
        for (const int step : kSizes) {
            QPixmap pixmap = tinted(step, ink, name);
            if (pixmap.isNull()) {
                continue;
            }
            if (step > size) {
                pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            icon.addPixmap(pixmap, QIcon::Normal);
            icon.addPixmap(tinted(step, disabledColor(palette), name), QIcon::Disabled);
        }
        button->setIcon(icon);
    };
    repaint(QApplication::palette());
    QObject::connect(qApp, &QApplication::paletteChanged, button, repaint);
}

void apply(QAbstractButton* button, const QString& name, const Tone tone) {
    if (button == nullptr) {
        return;
    }
    const auto repaint = [button, name, tone](const QPalette& palette) {
        button->setIcon(build(name, tone, palette));
    };
    repaint(QApplication::palette());
    QObject::connect(qApp, &QApplication::paletteChanged, button, repaint);
}

} // namespace videovault::app::icons
