#pragma once

// Geometry/behaviour port from Telegram Desktop (GPL-3.0-or-later):
// Telegram/SourceFiles/media/view/media_view.style (18-112, 279-287),
// media_view_playback_controls.cpp::resizeEvent, refreshTimeTexts, and
// media_view_overlay_widget.cpp::refreshClipControllerGeometry.
// Copyright Telegram Desktop contributors. Upstream attribution and license:
// https://github.com/telegramdesktop/tdesktop/blob/dev/LEGAL
// This Qt 5 adaptation does not import Telegram's lib_ui / rpl runtime.
#include <QRect>
#include <QSize>
#include <QString>
#include <algorithm>
#include <cstdint>

namespace videovault::app::telegram_video {
inline constexpr int controllerWidth = 480;
inline constexpr int controllerHeight = 72;
inline constexpr int bottomSkip = 6;
inline constexpr int hideDelay = 1100;
inline constexpr int showDuration = 200;
inline constexpr int hideDuration = 600;
inline constexpr int radius = 9;

inline QRect controllerRect(QSize viewport) {
    const int width = std::max(0, std::min(controllerWidth, viewport.width() - 2 * bottomSkip));
    return {(viewport.width() - width) / 2,
        std::max(0, viewport.height() - controllerHeight - bottomSkip), width, controllerHeight};
}

struct Controls {
    QRect elapsed, remaining, seek, play, volumeToggle, volume, fullscreen, popout, menu;
};
inline Controls controls(int width, int elapsedWidth, int remainingWidth, int textHeight) {
    // Source's right-to-left order: settings, PiP, fullscreen. Play stays centred.
    return {{14, 46, elapsedWidth, textHeight},
        {width - 14 - remainingWidth, 46, remainingWidth, textHeight},
        {24 + elapsedWidth, 49, std::max(0, width - 48 - elapsedWidth - remainingWidth), 12},
        {(width - 40) / 2, 2, 40, 40}, {6, 6, 32, 32}, {41, 16, 75, 12},
        {width - 112, 6, 32, 32}, {width - 76, 6, 32, 32}, {width - 40, 6, 32, 32}};
}
inline QString duration(std::int64_t ms) {
    const auto seconds = std::max<std::int64_t>(0, ms) / 1000;
    if (seconds >= 3600) {
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600)
            .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
inline QString remaining(std::int64_t length, std::int64_t position) {
    // Telegram subtracts whole seconds, not floor((length-position)/1000).
    return QString(QChar(8722)) + duration(std::max<std::int64_t>(0,
        length / 1000 - position / 1000) * 1000);
}
} // namespace videovault::app::telegram_video
