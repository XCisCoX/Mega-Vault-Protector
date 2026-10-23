#pragma once

#include <QCoreApplication>
#include <QRect>
#include <QSize>

// Photo-viewer chrome geometry, adapted from Telegram Desktop 7.3.1
// (media/view/media_view.style and media_view_overlay_widget.cpp):
//   mediaviewIconSize 46x54 cells anchored to the bottom-right corner,
//   mediaviewIconOver 36px hover circle, mediaviewControlSize 90px edge nav
//   strips starting mediaviewHeaderTop 47px below the top, and the name/date
//   footer mediaviewTextLeft 14px / mediaviewTextTop 26px from the bottom-left.
// Telegram has no bottom control card for photos and no zoom percentage text:
// zoom is wheel/pinch/double-click, and the visible controls are the edge
// arrows plus a right-aligned icon row.
namespace videovault::app::telegram_photo {

constexpr int cellWidth = 46;
constexpr int cellHeight = 54;
constexpr int hoverDiameter = 36;
constexpr int navWidth = 90;
constexpr int navTop = 47;
constexpr int footerLeft = 14;
constexpr int footerBottom = 26;
constexpr int idleHideMs = 2500;

// The n-th control cell counted from the right edge, 1-based.
inline QRect cell(const int indexFromRight, const QSize& viewport) {
    const int i = indexFromRight < 1 ? 1 : indexFromRight;
    return QRect(viewport.width() - cellWidth * i,
        viewport.height() - cellHeight, cellWidth, cellHeight);
}

// Centre of a cell, where the hover circle and the glyph sit.
inline QRect hoverCircle(const QRect& cellRect) {
    const int x = cellRect.x() + (cellRect.width() - hoverDiameter) / 2;
    const int y = cellRect.y() + (cellRect.height() - hoverDiameter) / 2;
    return QRect(x, y, hoverDiameter, hoverDiameter);
}

// Edge navigation strip. The glyph is centred in it, as in the source.
inline QRect navStrip(const bool right, const QSize& viewport) {
    const int height = viewport.height() - 2 * navTop;
    return QRect(right ? viewport.width() - navWidth : 0, navTop, navWidth,
        height < 0 ? 0 : height);
}

inline QPoint navCentre(const bool right, const QSize& viewport) {
    const QRect strip = navStrip(right, viewport);
    return QPoint(strip.x() + strip.width() / 2, strip.y() + strip.height() / 2);
}

// Name/date footer, bottom-left. `textWidth` is the measured line width.
inline QRect footer(const QSize& viewport, const int textWidth, const int textHeight) {
    const int width = qBound(0, textWidth, qMax(0, viewport.width() - footerLeft - cellWidth * 2 - 8));
    return QRect(footerLeft, viewport.height() - footerBottom - textHeight, width, textHeight);
}

} // namespace videovault::app::telegram_photo
