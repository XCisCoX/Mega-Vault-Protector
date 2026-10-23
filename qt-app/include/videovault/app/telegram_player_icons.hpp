#pragma once

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QString>

// The bundled masks are unmodified Telegram Desktop assets, GPL-3.0-or-later.
// See qt-app/third_party/telegram-player/NOTICE.md and LICENSE.
// Telegram stores coverage in the RGB intensity (white on black), not alpha.
namespace videovault::app::telegram {

inline QPixmap mask(const QString& name, const QColor& color, int scale) {
    // Restrict lookup to the bundled mask tree, including its speed/ directory.
    if (name.isEmpty() || name.contains(QStringLiteral(".."))
        || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char(':'))
        || name.contains(QLatin1Char('\\'))) {
        return {};
    }
    scale = qBound(1, scale, 3);
    const QString key = QStringLiteral("mvp.telegram.player.%1.%2.%3")
        .arg(name).arg(color.rgba()).arg(scale);
    QPixmap result;
    if (QPixmapCache::find(key, &result)) {
        return result;
    }
    const QString suffix = scale == 1 ? QString() : QStringLiteral("@%1x").arg(scale);
    const QImage source(QStringLiteral(":/telegram/player/%1%2.png").arg(name, suffix));
    if (source.isNull()) {
        return {};
    }
    const QImage rgb = source.convertToFormat(QImage::Format_RGB32);
    QImage tinted(rgb.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* from = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        auto* to = reinterpret_cast<QRgb*>(tinted.scanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            const int alpha = (qGray(from[x]) * color.alpha() + 127) / 255;
            to[x] = qPremultiply(qRgba(color.red(), color.green(), color.blue(), alpha));
        }
    }
    result = QPixmap::fromImage(tinted);
    result.setDevicePixelRatio(scale);
    QPixmapCache::insert(key, result);
    return result;
}

inline QIcon icon(const QString& name, const QColor& color) {
    QIcon result;
    for (int scale = 1; scale <= 3; ++scale) {
        const QPixmap pixmap = mask(name, color, scale);
        if (!pixmap.isNull()) {
            result.addPixmap(pixmap);
        }
    }
    return result;
}

inline bool paint(QPainter& painter, const QRectF& rect,
                  const QString& name, const QColor& color) {
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const int scale = ratio > 2.0 ? 3 : (ratio > 1.0 ? 2 : 1);
    const QPixmap pixmap = mask(name, color, scale);
    if (pixmap.isNull()) {
        return false;
    }
    painter.drawPixmap(rect, pixmap, QRectF(pixmap.rect()));
    return true;
}

} // namespace videovault::app::telegram
