// Video controller geometry/interaction adaptation from Telegram Desktop 7.3.1:
// Telegram/SourceFiles/media/view/media_view_playback_controls.cpp/.h,
// media_view_overlay_widget.cpp and media_view.style.
// Copyright Telegram Desktop contributors; GPL-3.0-or-later.
// See qt-app/third_party/telegram-player/{NOTICE,LICENSE,LEGAL} for attribution.
// The vault decoder/worker and photo transform implementation remain local.
#include "videovault/app/icons.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/app/telegram_video_chrome.hpp"
#include "videovault/app/telegram_photo_chrome.hpp"
#include "videovault/app/telegram_player_icons.hpp"
#include "videovault/app/video_pacing.hpp"
#include <QMenu>
#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QThreadPool>

extern "C" {
#include <libavutil/error.h>
}

#include <QAbstractButton>
#include <QApplication>
#include <QAudioFormat>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QIcon>
#include <QFont>
#include <QGridLayout>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QList>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QEasingCurve>
#include <QTransform>
#include <QVariantAnimation>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace videovault::app {

namespace {

QThreadPool* stillImagePool() {
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(2); return true; }();
    Q_UNUSED(configured)
    return &pool;
}

enum class PlayerIcon {
    Play, Pause, Volume, Muted, Fullscreen, Previous, Next, Back, Forward,
    Chevron, Repeat, RepeatOne, Shuffle, Pip
};

// Crisp vector-style icons drawn with QPainter (no external assets).
QPixmap makePlayerIcon(const PlayerIcon kind, const int size, const QColor& color) {
    QString upstream;
    switch (kind) {
    case PlayerIcon::Play: upstream = QStringLiteral("player_play_big"); break;
    case PlayerIcon::Pause: upstream = QStringLiteral("player_pause_big"); break;
    case PlayerIcon::Volume: upstream = QStringLiteral("player_volume_on"); break;
    case PlayerIcon::Muted: upstream = QStringLiteral("player_volume_off"); break;
    case PlayerIcon::Fullscreen: upstream = QStringLiteral("player_fullscreen"); break;
    case PlayerIcon::Pip: upstream = QStringLiteral("player_pip"); break;
    default: break;
    }
    if (!upstream.isEmpty()) return telegram::icon(upstream, color).pixmap(size, size);
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const double s = static_cast<double>(size);
    switch (kind) {
    case PlayerIcon::Play: {
        QPolygonF triangle;
        triangle << QPointF(s * 0.28, s * 0.18) << QPointF(s * 0.82, s * 0.5)
                 << QPointF(s * 0.28, s * 0.82);
        painter.drawPolygon(triangle);
        break;
    }
    case PlayerIcon::Pause:
        painter.drawRoundedRect(
            QRectF(s * 0.26, s * 0.2, s * 0.16, s * 0.6), s * 0.04, s * 0.04);
        painter.drawRoundedRect(
            QRectF(s * 0.58, s * 0.2, s * 0.16, s * 0.6), s * 0.04, s * 0.04);
        break;
    case PlayerIcon::Volume: {
        QPolygonF speaker;
        speaker << QPointF(s * 0.14, s * 0.38) << QPointF(s * 0.36, s * 0.38)
                << QPointF(s * 0.56, s * 0.2) << QPointF(s * 0.56, s * 0.8)
                << QPointF(s * 0.36, s * 0.62) << QPointF(s * 0.14, s * 0.62);
        painter.drawPolygon(speaker);
        QPen wave(color, std::max(1.2, s * 0.07));
        wave.setCapStyle(Qt::RoundCap);
        painter.setPen(wave);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(s * 0.56, s * 0.28, s * 0.24, s * 0.44), -55 * 16, 110 * 16);
        painter.drawArc(QRectF(s * 0.66, s * 0.16, s * 0.28, s * 0.68), -55 * 16, 110 * 16);
        break;
    }
    case PlayerIcon::Muted: {
        QPolygonF speaker;
        speaker << QPointF(s * 0.14, s * 0.38) << QPointF(s * 0.36, s * 0.38)
                << QPointF(s * 0.56, s * 0.2) << QPointF(s * 0.56, s * 0.8)
                << QPointF(s * 0.36, s * 0.62) << QPointF(s * 0.14, s * 0.62);
        painter.drawPolygon(speaker);
        QPen cross(color, std::max(1.4, s * 0.08));
        cross.setCapStyle(Qt::RoundCap);
        painter.setPen(cross);
        painter.setBrush(Qt::NoBrush);
        const double cx = s * 0.68;
        const double cy = s * 0.5;
        const double r = s * 0.17;
        painter.drawLine(QPointF(cx - r, cy - r), QPointF(cx + r, cy + r));
        painter.drawLine(QPointF(cx - r, cy + r), QPointF(cx + r, cy - r));
        break;
    }
    case PlayerIcon::Fullscreen: {
        QPen bracket(color, std::max(1.4, s * 0.09));
        bracket.setCapStyle(Qt::RoundCap);
        painter.setPen(bracket);
        painter.setBrush(Qt::NoBrush);
        const double m = s * 0.22;
        const double l = s * 0.2;
        painter.drawLine(QPointF(m, m - l), QPointF(m, m));
        painter.drawLine(QPointF(m - l, m), QPointF(m, m));
        painter.drawLine(QPointF(s - m, m - l), QPointF(s - m, m));
        painter.drawLine(QPointF(s - m + l, m), QPointF(s - m, m));
        painter.drawLine(QPointF(m, s - m + l), QPointF(m, s - m));
        painter.drawLine(QPointF(m - l, s - m), QPointF(m, s - m));
        painter.drawLine(QPointF(s - m, s - m + l), QPointF(s - m, s - m));
        painter.drawLine(QPointF(s - m + l, s - m), QPointF(s - m, s - m));
        break;
    }
    case PlayerIcon::Previous:
    case PlayerIcon::Next: {
        const bool next = kind == PlayerIcon::Next;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const double barX = next ? s * 0.72 : s * 0.16;
        painter.drawRoundedRect(QRectF(barX, s * 0.24, s * 0.11, s * 0.52), s * 0.04, s * 0.04);
        QPolygonF triangle;
        if (next) {
            triangle << QPointF(s * 0.18, s * 0.24) << QPointF(s * 0.62, s * 0.5)
                     << QPointF(s * 0.18, s * 0.76);
        } else {
            triangle << QPointF(s * 0.82, s * 0.24) << QPointF(s * 0.38, s * 0.5)
                     << QPointF(s * 0.82, s * 0.76);
        }
        painter.drawPolygon(triangle);
        break;
    }
    case PlayerIcon::Chevron: {
        QPen stroke(color, std::max(1.7, s * 0.1), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(s * 0.64, s * 0.2);
        path.lineTo(s * 0.34, s * 0.5);
        path.lineTo(s * 0.64, s * 0.8);
        painter.drawPath(path);
        break;
    }
    case PlayerIcon::Repeat:
    case PlayerIcon::RepeatOne: {
        QPen stroke(color, std::max(1.45, s * 0.085), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        const QRectF loop(s * 0.16, s * 0.2, s * 0.68, s * 0.6);
        painter.drawArc(loop, 25 * 16, 150 * 16);
        painter.drawArc(loop, 205 * 16, 150 * 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        QPolygonF headTop;
        headTop << QPointF(s * 0.78, s * 0.16) << QPointF(s * 0.78, s * 0.4)
                << QPointF(s * 0.58, s * 0.28);
        QPolygonF headBottom;
        headBottom << QPointF(s * 0.22, s * 0.6) << QPointF(s * 0.22, s * 0.84)
                   << QPointF(s * 0.42, s * 0.72);
        painter.drawPolygon(headTop);
        painter.drawPolygon(headBottom);
        if (kind == PlayerIcon::RepeatOne) {
            QFont digit(QStringLiteral("Segoe UI"));
            digit.setPixelSize(static_cast<int>(s * 0.34));
            digit.setBold(true);
            painter.setFont(digit);
            painter.setPen(color);
            painter.drawText(QRectF(0, s * 0.28, s, s * 0.46), Qt::AlignCenter, QStringLiteral("1"));
        }
        break;
    }
    case PlayerIcon::Shuffle: {
        QPen stroke(color, std::max(1.45, s * 0.085), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(s * 0.16, s * 0.72), QPointF(s * 0.78, s * 0.28));
        painter.drawLine(QPointF(s * 0.16, s * 0.28), QPointF(s * 0.78, s * 0.72));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        QPolygonF up;
        up << QPointF(s * 0.58, s * 0.16) << QPointF(s * 0.86, s * 0.16) << QPointF(s * 0.72, s * 0.4);
        QPolygonF down;
        down << QPointF(s * 0.58, s * 0.84) << QPointF(s * 0.86, s * 0.84) << QPointF(s * 0.72, s * 0.6);
        painter.drawPolygon(up);
        painter.drawPolygon(down);
        break;
    }
    case PlayerIcon::Pip: {
        QPen stroke(color, std::max(1.4, s * 0.08), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(s * 0.1, s * 0.16, s * 0.58, s * 0.46), s * 0.06, s * 0.06);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(s * 0.38, s * 0.42, s * 0.5, s * 0.38), s * 0.05, s * 0.05);
        break;
    }
    case PlayerIcon::Back:
    case PlayerIcon::Forward: {
        const bool forward = kind == PlayerIcon::Forward;
        painter.setBrush(color);
        const double shift = forward ? 0.0 : s * 0.02;
        QPolygonF first;
        QPolygonF second;
        if (forward) {
            first << QPointF(shift + s * 0.08, s * 0.24) << QPointF(shift + s * 0.46, s * 0.5)
                  << QPointF(shift + s * 0.08, s * 0.76);
            second << QPointF(shift + s * 0.46, s * 0.24) << QPointF(shift + s * 0.84, s * 0.5)
                   << QPointF(shift + s * 0.46, s * 0.76);
        } else {
            first << QPointF(s * 0.92 - shift, s * 0.24) << QPointF(s * 0.54 - shift, s * 0.5)
                  << QPointF(s * 0.92 - shift, s * 0.76);
            second << QPointF(s * 0.54 - shift, s * 0.24) << QPointF(s * 0.16 - shift, s * 0.5)
                   << QPointF(s * 0.54 - shift, s * 0.76);
        }
        painter.drawPolygon(first);
        painter.drawPolygon(second);
        break;
    }
    }
    return pixmap;
}

// Top fade over the picture, or the floating control card.
enum class GlassShade { Top, Card, Video };

class PlayerGlass final : public QWidget {
public:
    PlayerGlass(const GlassShade shade, QWidget* parent)
        : QWidget(parent), shade_(shade) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
        fade_ = new QVariantAnimation(this);
        connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            opacity_ = v.toReal(); update();
        });
        connect(fade_, &QVariantAnimation::finished, this, [this] {
            if (opacity_ < 0.5) hide();
            restoreChildren();
            snapshot_ = QPixmap();
            update();
        });
    }

    void reveal() {
        // Cancel a cached fade before returning event routing to real children.
        // Unlike QGraphicsOpacityEffect this never redirects child painting.
        fade_->stop();
        restoreChildren();
        snapshot_ = QPixmap();
        show();
        update();
    }
    void fadeOut() {
        if (!isVisible() || !snapshot_.isNull()) return;
        snapshot_ = grab();
        for (auto* child : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!child->isHidden()) { fadedChildren_.push_back(child); child->hide(); }
        }
        opacity_ = 1.;
        fade_->setStartValue(1.);
        fade_->setEndValue(0.);
        fade_->setDuration(telegram_video::hideDuration);
        fade_->start();
    }

public:
    void setVideo(bool video) { shade_ = video ? GlassShade::Video : GlassShade::Card; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (!snapshot_.isNull()) {
            painter.setOpacity(opacity_);
            painter.drawPixmap(rect(), snapshot_);
            return;
        }
        if (shade_ == GlassShade::Video) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 153));
            painter.drawRoundedRect(rect(), telegram_video::radius, telegram_video::radius);
            return;
        }
        if (shade_ == GlassShade::Top) {
            QLinearGradient shade(0, 0, 0, height());
            shade.setColorAt(0.0, QColor(0, 0, 0, 150));
            shade.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), shade);
            return;
        }
        painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
        painter.setBrush(QColor(12, 12, 14, 214));
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 14, 14);
    }

private:
    void restoreChildren() {
        for (auto* child : fadedChildren_) child->show();
        fadedChildren_.clear();
    }
    QVariantAnimation* fade_{nullptr};
    QPixmap snapshot_;
    QList<QWidget*> fadedChildren_;
    qreal opacity_{1.};
    GlassShade shade_;
};

enum class PlayerCaptionGlyph { Minimize, Maximize, Restore, Close };

class PlayerCaptionButton final : public QAbstractButton {
public:
    explicit PlayerCaptionButton(const PlayerCaptionGlyph glyph, QWidget* parent = nullptr)
        : QAbstractButton(parent), glyph_(glyph) {
        setFixedSize(44, 32);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
    }

    void setGlyph(const PlayerCaptionGlyph glyph) {
        glyph_ = glyph;
        update();
    }

protected:
    void enterEvent(QEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool close = glyph_ == PlayerCaptionGlyph::Close;
        if (underMouse()) {
            painter.fillRect(rect(), close ? QColor(232, 17, 35) : QColor(255, 255, 255, 24));
        }
        painter.setPen(QPen(Qt::white, 1.0));
        painter.setBrush(Qt::NoBrush);
        const QPointF center(width() / 2.0, height() / 2.0);
        switch (glyph_) {
        case PlayerCaptionGlyph::Minimize:
            painter.drawLine(QPointF(center.x() - 5, center.y()), QPointF(center.x() + 5, center.y()));
            break;
        case PlayerCaptionGlyph::Maximize:
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 5, 10, 10), 1.5, 1.5);
            break;
        case PlayerCaptionGlyph::Restore:
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 2, 8, 8), 1.2, 1.2);
            painter.drawLine(QPointF(center.x() - 2, center.y() - 4), QPointF(center.x() + 5, center.y() - 4));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 4), QPointF(center.x() + 5, center.y() + 3));
            break;
        case PlayerCaptionGlyph::Close:
            painter.drawLine(QPointF(center.x() - 5, center.y() - 5), QPointF(center.x() + 5, center.y() + 5));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 5), QPointF(center.x() - 5, center.y() + 5));
            break;
        }
    }

private:
    PlayerCaptionGlyph glyph_;
};

// Still-image codecs (FFmpeg demuxes jpg/png/... as single-frame "videos").
bool is_image_codec(const std::string& codec_name) {
    static const std::vector<std::string> kImageCodecs{
        "png", "apng", "mjpeg", "bmp", "webp", "gif", "tiff"};
    return std::find(kImageCodecs.begin(), kImageCodecs.end(), codec_name)
        != kImageCodecs.end();
}

} // namespace

AudioSink::AudioSink(QObject* parent) : QIODevice(parent) {
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

AudioSink::~AudioSink() = default;

void AudioSink::append(const std::int16_t* samples, const std::size_t count) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (read_cursor_ > 8192U && read_cursor_ * 2U > buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
        read_cursor_ = 0;
    }
    buffer_.insert(buffer_.end(), samples, samples + count);
}

void AudioSink::clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    buffer_.clear();
    read_cursor_ = 0;
    consumedSamples_ = 0;
}

std::size_t AudioSink::buffered_samples() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return buffer_.size() - read_cursor_;
}

std::uint64_t AudioSink::consumed_samples() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return consumedSamples_;
}

qint64 AudioSink::readData(char* data, qint64 maxSize) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto byte_count = static_cast<std::size_t>(std::max<qint64>(maxSize, 0));
    const auto available = (buffer_.size() - read_cursor_) * sizeof(std::int16_t);
    // Never consume half an s16 sample on an odd-sized pull.
    const auto take = std::min(byte_count, available) / sizeof(std::int16_t) * sizeof(std::int16_t);
    if (take > 0) {
        std::memcpy(data, buffer_.data() + read_cursor_, take);
        read_cursor_ += take / sizeof(std::int16_t);
        consumedSamples_ += take / sizeof(std::int16_t);
        // The device reads from the front forever. Dropping the consumed
        // prefix is what keeps a long playlist from holding every sample.
        if (read_cursor_ > 8192U && read_cursor_ * 2U > buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
            read_cursor_ = 0;
        }
    }
    return static_cast<qint64>(take);
}

qint64 AudioSink::writeData(const char*, qint64) {
    return -1; // read-only sink
}

PlayerWindow::PlayerWindow(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::int64_t video_id,
    const QString& title,
    QWidget* parent,
    const bool stillImage)
    : QDialog(parent), vault_(vault), video_id_(video_id), title_(title) {
    setWindowTitle(title);
    resize(960, 620);
    setMinimumSize(480, stillImage ? 320 : 360);
    setMouseTracking(true);

    QSettings settings;
    volumePercent_ = settings.value(QStringLiteral("player/volume"), 100).toInt();
    volumePercent_ = std::clamp(volumePercent_, 0, 100);
    muted_ = settings.value(QStringLiteral("player/muted"), false).toBool();
    const int cache_mib =
        settings.value(QStringLiteral("player/streamCacheMib"), 32).toInt();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    windowCaption_ = new QWidget(this);
    windowCaption_->setFixedHeight(32);
    windowCaption_->setObjectName(QStringLiteral("playerCaption"));
    windowCaption_->setAttribute(Qt::WA_StyledBackground, true);
    windowCaption_->setStyleSheet(QStringLiteral(
        "QWidget#playerCaption { background: #000000; border: none; }"));
    auto* captionLayout = new QHBoxLayout(windowCaption_);
    captionLayout->setContentsMargins(14, 0, 0, 0);
    captionLayout->setSpacing(0);
    captionTitle_ = new QLabel(title, windowCaption_);
    captionTitle_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QFont captionFont(QStringLiteral("Segoe UI"));
    captionFont.setPointSize(9);
    captionTitle_->setFont(captionFont);
    captionTitle_->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 210); background: transparent;"));
    captionLayout->addWidget(captionTitle_, 1);
    auto* minimize = new PlayerCaptionButton(PlayerCaptionGlyph::Minimize, windowCaption_);
    windowMax_ = new PlayerCaptionButton(PlayerCaptionGlyph::Maximize, windowCaption_);
    auto* closeCaption = new PlayerCaptionButton(PlayerCaptionGlyph::Close, windowCaption_);
    captionLayout->addWidget(minimize);
    captionLayout->addWidget(windowMax_);
    captionLayout->addWidget(closeCaption);
    connect(minimize, &QAbstractButton::clicked, this, [this] { showMinimized(); });
    connect(windowMax_, &QAbstractButton::clicked, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(closeCaption, &QAbstractButton::clicked, this, &QWidget::close);
    windowCaption_->hide();
    layout->addWidget(windowCaption_);

    surface_ = new VideoSurface(this);
    surface_->setPhotoMode(stillImage);
    surface_->setMinimumSize(320, 200);
    surface_->setText(QStringLiteral("Loading…"));
    // App-level key interception: the player's shortcuts (Space, arrows, +-,
    // F) work regardless of which child widget holds focus.
    qApp->installEventFilter(this);
    layout->addWidget(surface_, 1);

    // Overlay chrome: title bar on top, center play button, bottom controls —
    // all direct children of the surface, so the empty video area keeps its
    // own mouse handling (click-to-seek) while the controls work normally.
    topOverlay_ = new PlayerGlass(GlassShade::Top, surface_);
    topOverlay_->setFixedHeight(56);
    titleLabel_ = new QLabel(title, topOverlay_);
    QFont titleFont(QStringLiteral("Segoe UI"));
    titleFont.setPointSize(11);
    titleLabel_->setFont(titleFont);
    titleLabel_->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    indexLabel_ = new QLabel(topOverlay_);
    QFont indexFont(QStringLiteral("Segoe UI"));
    indexFont.setPointSize(9);
    indexLabel_->setFont(indexFont);
    indexLabel_->setStyleSheet(QStringLiteral("color: rgba(255,255,255,150); background: transparent;"));
    auto* topLayout = new QHBoxLayout(topOverlay_);
    topLayout->setContentsMargins(8, 6, 16, 6);
    topLayout->setSpacing(4);
    auto* backViewer = new QPushButton(topOverlay_);
    backViewer->setObjectName(QStringLiteral("playerBackToLibrary"));
    backViewer->setIcon(makePlayerIcon(PlayerIcon::Chevron, 20, Qt::white));
    backViewer->setIconSize(QSize(20, 20));
    backViewer->setFixedSize(40, 40);
    backViewer->setCursor(Qt::PointingHandCursor);
    backViewer->setFocusPolicy(Qt::NoFocus);
    backViewer->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 20px; }"
        "QPushButton:hover { background: rgba(255,255,255,28); }"
        "QPushButton:pressed { background: rgba(255,255,255,44); }"));
    backViewer->setToolTip(QStringLiteral("Back"));
    connect(backViewer, &QPushButton::clicked, this, &QWidget::close);
    topLayout->addWidget(backViewer);
    topLayout->addWidget(titleLabel_, 1);
    topLayout->addWidget(indexLabel_);

    centerPlayButton_ = new QPushButton(surface_);
    centerPlayButton_->setFixedSize(72, 72);
    centerPlayButton_->setCursor(Qt::PointingHandCursor);
    centerPlayButton_->setIconSize(QSize(36, 36));
    centerPlayButton_->setIcon(makePlayerIcon(PlayerIcon::Play, 36, Qt::white));
    centerPlayButton_->setFocusPolicy(Qt::NoFocus);
    centerPlayButton_->setStyleSheet(QStringLiteral(
        "QPushButton { border-radius: 36px; background: rgba(0,0,0,90);"
        " border: 1px solid rgba(255,255,255,70); }"
        "QPushButton:hover { background: rgba(38,114,200,200); }"));
    connect(centerPlayButton_, &QPushButton::clicked, this, [this] {
        togglePlayPause();
    });

    controlsOverlay_ = new PlayerGlass(GlassShade::Video, surface_);
    auto* controlsLayout = new QVBoxLayout(controlsOverlay_);
    controlsLayout->setContentsMargins(16, 8, 16, 8);
    controlsLayout->setSpacing(2);

    auto styleTransport = [](QPushButton* button, const int diameter) {
        button->setFixedSize(diameter, diameter);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(diameter / 2, diameter / 2));
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; border-radius: %1px; }"
            "QPushButton:hover { background: rgba(255,255,255,36); }"
            "QPushButton:pressed { background: rgba(255,255,255,56); }"
            "QPushButton:disabled { background: transparent; }").arg(diameter / 2));
    };

    positionSlider_ = new SeekSlider(controlsOverlay_);
    positionSlider_->setRange(0, 1);
    positionSlider_->setTracking(false);
    positionSlider_->setFixedHeight(22);

    backButton_ = new QPushButton(controlsOverlay_);
    backButton_->setIcon(makePlayerIcon(PlayerIcon::Back, 18, Qt::white));
    backButton_->setToolTip(QStringLiteral("Back 10 seconds (Left)"));
    previousButton_ = new QPushButton(controlsOverlay_);
    previousButton_->setIcon(makePlayerIcon(PlayerIcon::Previous, 18, Qt::white));
    previousButton_->setToolTip(QStringLiteral("Previous (P)"));
    previousButton_->setEnabled(false);
    playButton_ = new QPushButton(controlsOverlay_);
    playButton_->setIcon(makePlayerIcon(PlayerIcon::Pause, 26, Qt::white));
    playButton_->setToolTip(QStringLiteral("Play or pause (Space)"));
    nextButton_ = new QPushButton(controlsOverlay_);
    nextButton_->setIcon(makePlayerIcon(PlayerIcon::Next, 18, Qt::white));
    nextButton_->setToolTip(QStringLiteral("Next (N)"));
    nextButton_->setEnabled(false);
    forwardButton_ = new QPushButton(controlsOverlay_);
    forwardButton_->setIcon(makePlayerIcon(PlayerIcon::Forward, 18, Qt::white));
    forwardButton_->setToolTip(QStringLiteral("Forward 10 seconds (Right)"));
    for (auto* button : {backButton_, previousButton_, nextButton_, forwardButton_}) {
        styleTransport(button, 40);
    }
    styleTransport(playButton_, 44);
    playButton_->setIconSize(QSize(26, 26));

    QFont timeFont(QStringLiteral("Segoe UI"));
    timeFont.setPointSize(9);
    const QString timeStyle = QStringLiteral("color: rgba(255,255,255,220); background: transparent;");
    positionLabel_ = new QLabel(QStringLiteral("0:00"), controlsOverlay_);
    positionLabel_->setFont(timeFont);
    positionLabel_->setStyleSheet(timeStyle);
    timeLeftLabel_ = new QLabel(QString(QChar(0x2212)) + QStringLiteral("0:00"), controlsOverlay_);
    timeLeftLabel_->setFont(timeFont);
    timeLeftLabel_->setStyleSheet(timeStyle);
    // Video geometry is ported from PlaybackControls::resizeEvent, not a box layout.

    muteButton_ = new QPushButton(controlsOverlay_);
    muteButton_->setIcon(makePlayerIcon(PlayerIcon::Volume, 18, Qt::white));
    muteButton_->setToolTip(QStringLiteral("Mute (M)"));
    styleTransport(muteButton_, 40);
    muteButton_->setCheckable(true);
    muteButton_->setChecked(muted_);

    cacheCombo_ = new QComboBox(controlsOverlay_);
    const std::vector<std::pair<QString, int>> cache_options{
        {QStringLiteral("4 MB"), 4}, {QStringLiteral("8 MB"), 8},
        {QStringLiteral("16 MB"), 16}, {QStringLiteral("32 MB"), 32},
        {QStringLiteral("64 MB"), 64}, {QStringLiteral("128 MB"), 128}};
    int cache_index = 2; // 16 MB fallback
    for (std::size_t i = 0U; i < cache_options.size(); ++i) {
        cacheCombo_->addItem(cache_options[i].first, cache_options[i].second);
        if (cache_options[i].second == cache_mib) {
            cache_index = static_cast<int>(i);
        }
    }
    cacheCombo_->setCurrentIndex(cache_index);
    cacheCombo_->hide();

    fullscreenButton_ = new QPushButton(controlsOverlay_);
    fullscreenButton_->setIcon(makePlayerIcon(PlayerIcon::Fullscreen, 18, Qt::white));
    fullscreenButton_->setToolTip(QStringLiteral("Fullscreen (F)"));
    styleTransport(fullscreenButton_, 40);

    repeatButton_ = new QPushButton(controlsOverlay_);
    styleTransport(repeatButton_, 40);
    shuffleButton_ = new QPushButton(controlsOverlay_);
    shuffleButton_->setIcon(makePlayerIcon(PlayerIcon::Shuffle, 18, QColor(255, 255, 255, 120)));
    shuffleButton_->setToolTip(QStringLiteral("Shuffle"));
    styleTransport(shuffleButton_, 40);
    popButton_ = new QPushButton(controlsOverlay_);
    styleTransport(popButton_, 40);
    updatePopIcon();
    repeatMode_ = settings.value(QStringLiteral("player/repeat"), 0).toInt();
    if (repeatMode_ < 0 || repeatMode_ > 2) {
        repeatMode_ = 0;
    }
    shuffle_ = settings.value(QStringLiteral("player/shuffle"), false).toBool();
    repeatWatch_.store(repeatMode_);
    refreshTransportIcons();
    applyVolume();

    volumeSlider_ = new QSlider(Qt::Horizontal, controlsOverlay_);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(volumePercent_);
    volumeSlider_->setFixedWidth(92);
    volumeSlider_->setToolTip(QStringLiteral("Volume (Up / Down)"));
    volumeSlider_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 3px; background: rgba(255,255,255,50); border-radius: 1px; }"
        "QSlider::sub-page:horizontal { background: rgba(255,255,255,220); border-radius: 1px; }"
        "QSlider::handle:horizontal { width: 10px; margin: -4px 0; border-radius: 5px; background: white; }"));
    backButton_->hide();
    forwardButton_->hide();

    // Library navigation belongs at the stage edges, outside Telegram's
    // compact playback controller. Repeat/shuffle remain in the overflow.
    previousButton_->setParent(surface_);
    nextButton_->setParent(surface_);
    repeatButton_->hide();
    shuffleButton_->hide();
    videoMenuButton_ = new QPushButton(controlsOverlay_);
    styleTransport(videoMenuButton_, 32);
    videoMenuButton_->setIcon(telegram::icon(QStringLiteral("player_more"), Qt::white));
    videoMenuButton_->setIconSize(QSize(24, 24));
    videoMenuButton_->setToolTip(QStringLiteral("Playback options"));
    connect(videoMenuButton_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background:#292929; color:white; border:1px solid #454545; padding:4px; }"
            "QMenu::item { padding:8px 24px; } QMenu::item:selected { background:#454545; }"));
        auto* repeat = menu.addAction(repeatButton_->toolTip());
        auto* shuffle = menu.addAction(shuffleButton_->toolTip());
        menu.addSeparator();
        auto* previous = menu.addAction(QStringLiteral("Previous video"));
        previous->setEnabled(previousButton_->isEnabled());
        auto* next = menu.addAction(QStringLiteral("Next video"));
        next->setEnabled(nextButton_->isEnabled());
        auto* chosen = menu.exec(videoMenuButton_->mapToGlobal(QPoint(0, 0)));
        if (chosen == repeat) repeatButton_->click();
        else if (chosen == shuffle) shuffleButton_->click();
        else if (chosen == previous) emit previousRequested();
        else if (chosen == next) emit nextRequested();
        showChrome();
        uiHideTimer_.start();
    });
    styleTransport(playButton_, 40);
    playButton_->setIconSize(QSize(24, 24));
    for (auto* button : {muteButton_, fullscreenButton_, popButton_}) {
        styleTransport(button, 32);
        button->setIconSize(QSize(24, 24));
    }
    volumeSlider_->setFixedWidth(75);
    positionSlider_->setFixedHeight(12);
    timeFont.setPixelSize(12);
    timeFont.setWeight(QFont::DemiBold);
    positionLabel_->setFont(timeFont);
    timeLeftLabel_->setFont(timeFont);

    // Image-viewer row (hidden until an image is opened; zoom controls).
    auto* imageRow = new QHBoxLayout();
    imageRow->setSpacing(6);
    zoomOutButton_ = new QPushButton(QStringLiteral("−"), controlsOverlay_);
    zoomOutButton_->setToolTip(QStringLiteral("Zoom out (-)"));
    zoomLabel_ = new QLabel(QStringLiteral("Fit"), controlsOverlay_);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zoomInButton_ = new QPushButton(QStringLiteral("+"), controlsOverlay_);
    zoomInButton_->setToolTip(QStringLiteral("Zoom in (+)"));
    fitButton_ = new QPushButton(QStringLiteral("Fit"), controlsOverlay_);
    fitButton_->setToolTip(QStringLiteral("Fit to window (0)"));
    // Compact overlay controls: the glyph carries the meaning, the tooltip the words.
    // The overlay sits on video, so the ink stays light regardless of the app palette.
    struct OverlayControl {
        QPushButton* button;
        const char* icon;
    };
    for (const auto& control : {OverlayControl{zoomOutButton_, "minus"},
             OverlayControl{zoomInButton_, "plus"}, OverlayControl{fitButton_, "maximize"}}) {
        control.button->setText(QString());
        control.button->setMinimumWidth(34);
        control.button->setIconSize(QSize(16, 16));
        control.button->setIcon(icons::lucideSolid(
            QString::fromLatin1(control.icon), QColor(255, 255, 255, 230)));
        control.button->setAccessibleName(control.button->toolTip());
    }
    for (auto* button : {zoomOutButton_, zoomInButton_, fitButton_}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: rgba(255,255,255,20);"
            " border: 1px solid rgba(255,255,255,40);"
            " border-radius: 14px; color: rgba(255,255,255,230);"
            " padding: 4px 12px; min-height: 26px; font-size: 12px; }"
            "QPushButton:hover { background: rgba(255,255,255,36); }"));
    }
    zoomLabel_->setStyleSheet(QStringLiteral(
        "color: rgba(255,255,255,205); font-size: 12px; min-width: 46px;"));
    const QPointF centre = QPointF(surface_->width() / 2.0, surface_->height() / 2.0);
    connect(zoomOutButton_, &QPushButton::clicked, this, [this, centre] {
        animateImageZoom((imageZoom_ <= 0.0 ? fittedImageZoom() : imageZoom_) * 0.8, centre);
    });
    connect(zoomInButton_, &QPushButton::clicked, this, [this, centre] {
        animateImageZoom((imageZoom_ <= 0.0 ? fittedImageZoom() : imageZoom_) * 1.25, centre);
    });
    connect(fitButton_, &QPushButton::clicked, this, [this] { resetImageFit(); });
    rotateButton_ = new QPushButton(controlsOverlay_);
    rotateButton_->setCursor(Qt::PointingHandCursor);
    rotateButton_->setFocusPolicy(Qt::NoFocus);
    rotateButton_->setToolTip(QStringLiteral("Rotate 90° (R)"));
    rotateButton_->setAccessibleName(QStringLiteral("Rotate the photo"));
    rotateButton_->setStyleSheet(zoomOutButton_->styleSheet());
    const QIcon rotateGlyph =
        icons::lucideSolid(QStringLiteral("rotate-ccw"), QColor(0xE6, 0xE6, 0xEC));
    if (!rotateGlyph.isNull()) {
        const QIcon& icon = rotateGlyph;
        rotateButton_->setIcon(icon);
        rotateButton_->setIconSize(QSize(15, 15));
    } else {
        rotateButton_->setText(QStringLiteral("⟲"));
    }
    connect(rotateButton_, &QPushButton::clicked, this, [this] { rotateImage(); });
    imageRow->addWidget(zoomOutButton_);
    imageRow->addWidget(zoomLabel_);
    imageRow->addWidget(zoomInButton_);
    imageRow->addWidget(fitButton_);
    imageRow->addWidget(rotateButton_);
    imageRow->addStretch(1);
    controlsLayout->addLayout(imageRow);

    // Telegram's photo viewer has no bottom card. Its visible controls are the
    // right-aligned 46x54 icon cells (hover circle 36px) plus the edge arrows,
    // with the name at the bottom-left; zoom is wheel/pinch/double-click.
    auto stylePhotoCell = [](QPushButton* button) {
        button->setFixedSize(telegram_photo::hoverDiameter, telegram_photo::hoverDiameter);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(24, 24));
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; border-radius: %1px; }"
            "QPushButton:hover { background: rgba(0, 0, 0, 120); }"
            "QPushButton:pressed { background: rgba(0, 0, 0, 175); }")
            .arg(telegram_photo::hoverDiameter / 2));
    };
    photoMoreButton_ = new QPushButton(surface_);
    photoMoreButton_->setObjectName(QStringLiteral("photoMoreButton"));
    stylePhotoCell(photoMoreButton_);
    photoMoreButton_->setIcon(telegram::icon(QStringLiteral("player_more"), Qt::white));
    photoMoreButton_->setToolTip(QStringLiteral("More"));
    photoMoreButton_->setAccessibleName(QStringLiteral("Photo options"));
    connect(photoMoreButton_, &QPushButton::clicked, this, [this] { showPhotoMenu(); });
    photoRotateButton_ = new QPushButton(surface_);
    photoRotateButton_->setObjectName(QStringLiteral("photoRotateButton"));
    stylePhotoCell(photoRotateButton_);
    photoRotateButton_->setToolTip(QStringLiteral("Rotate 90° (R)"));
    photoRotateButton_->setAccessibleName(QStringLiteral("Rotate the photo"));
    if (!rotateGlyph.isNull()) {
        photoRotateButton_->setIcon(rotateGlyph);
    } else {
        photoRotateButton_->setIcon(makePlayerIcon(PlayerIcon::Repeat, 22, Qt::white));
    }
    connect(photoRotateButton_, &QPushButton::clicked, this, [this] { rotateImage(); });
    photoName_ = new QLabel(surface_);
    photoName_->setObjectName(QStringLiteral("photoName"));
    QFont photoNameFont(QStringLiteral("Segoe UI"));
    photoNameFont.setPointSize(9);
    photoName_->setFont(photoNameFont);
    photoName_->setStyleSheet(QStringLiteral(
        "color: rgba(255, 255, 255, 235); background: transparent;"));
    photoName_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    photoMoreButton_->hide();
    photoRotateButton_->hide();
    photoName_->hide();
    zoomOutButton_->hide();
    zoomLabel_->hide();
    zoomInButton_->hide();
    fitButton_->hide();
    rotateButton_->hide();

    // Position the chrome strips and the center button over the surface.
    layoutChrome();

    connect(playButton_, &QPushButton::clicked, this, [this] { togglePlayPause(); });
    connect(previousButton_, &QPushButton::clicked, this, [this] { emit previousRequested(); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { emit nextRequested(); });
    connect(backButton_, &QPushButton::clicked, this, [this] { seekRelative(-10000); });
    connect(forwardButton_, &QPushButton::clicked, this, [this] { seekRelative(10000); });
    connect(positionSlider_, &QSlider::sliderPressed, this, [this] {
        seeking_.store(true);
        uiHideTimer_.stop();
    });
    connect(positionSlider_, &QSlider::sliderMoved, this, [this](const int value) {
        if (seeking_.load()) {
            // Live position preview while dragging.
            positionLabel_->setText(formatTime(value));
            const auto left = std::max<std::int64_t>(0, durationMs_.load() - value);
            Q_UNUSED(left)
            timeLeftLabel_->setText(telegram_video::remaining(durationMs_.load(), value));
            layoutChrome();
        }
    });
    connect(positionSlider_, &QSlider::sliderReleased, this, [this] {
        doSeek(positionSlider_->value());
        seeking_.store(false);
        uiHideTimer_.start();
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](const int value) {
        volumePercent_ = value;
        applyVolume();
        QSettings settings;
        settings.setValue(QStringLiteral("player/volume"), value);
    });
    connect(muteButton_, &QPushButton::toggled, this, [this](const bool checked) {
        muted_ = checked;
        applyVolume();
        QSettings settings;
        settings.setValue(QStringLiteral("player/muted"), checked);
    });
    connect(fullscreenButton_, &QPushButton::clicked, this, [this] { toggleFullscreen(); });
    connect(repeatButton_, &QPushButton::clicked, this, [this] {
        repeatMode_ = (repeatMode_ + 1) % 3;
        repeatWatch_.store(repeatMode_);
        QSettings settings;
        settings.setValue(QStringLiteral("player/repeat"), repeatMode_);
        refreshTransportIcons();
    });
    connect(shuffleButton_, &QPushButton::clicked, this, [this] {
        shuffle_ = !shuffle_;
        QSettings settings;
        settings.setValue(QStringLiteral("player/shuffle"), shuffle_);
        refreshTransportIcons();
    });
    connect(popButton_, &QPushButton::clicked, this, [this] {
        if (viewerOverlay_) {
            emit dockRequested();
        } else if (detached_) {
            emit dockRequested();
        } else {
            emit detachRequested();
        }
    });
    connect(cacheCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) {
            const std::size_t bytes = static_cast<std::size_t>(
                cacheCombo_->currentData().toInt()) << 20;
            decoder_.set_stream_cache_bytes(bytes);
            QSettings settings;
            settings.setValue(QStringLiteral("player/streamCacheMib"),
                cacheCombo_->currentData().toInt());
        });

    uiHideTimer_.setSingleShot(true);
    uiHideTimer_.setInterval(telegram_video::hideDelay);
    connect(&uiHideTimer_, &QTimer::timeout, this, [this] {
        // Hide the chrome while the video plays and the mouse stays still, and
        // also while a photo is on screen: a picture meant to be looked at must
        // not carry a permanent control strip across it.
        if (positionSlider_->isSliderDown() || volumeSlider_->isSliderDown()
            || controlsOverlay_->underMouse() || QApplication::activePopupWidget()) {
            uiHideTimer_.start();
            return;
        }
        if (playing_.load() || imageMode_) hideChrome();
    });

    // Animated zoom for buttons and double-click. Wheel zoom stays immediate so
    // continuous scrolling keeps working.
    imageZoomAnim_ = new QVariantAnimation(this);
    imageZoomAnim_->setDuration(170);
    imageZoomAnim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(imageZoomAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (!imageMode_ || surface_ == nullptr) {
            return;
        }
        const double zoom = value.toDouble();
        imageZoom_ = zoom;
        imageOffset_ = imageZoomAnchor_ - imageZoomImagePoint_ * zoom;
        clampImageOffset();
        surface_->setView(imageZoom_, imageOffset_);
        updateZoomLabel();
    });

    decoder_.set_stream_cache_bytes(
        static_cast<std::size_t>(cacheCombo_->currentData().toInt()) << 20);
    surfaceSize_ = surface_->size();

    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    if (stillImage) {
        playStill(vault, video_id);
    } else {
        timer_.start();
        playing_.store(true);
        worker_ = std::thread(&PlayerWindow::workerLoop, this);
        uiHideTimer_.start();
    }
}

PlayerWindow::~PlayerWindow() {
    qApp->removeEventFilter(this);
    timer_.stop();
    quit_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    decoder_.close();
    if (audioOutput_ != nullptr) {
        audioOutput_->stop();
    }
}

void PlayerWindow::playVideo(std::shared_ptr<videovault::core::Vault> vault, std::int64_t video_id)
{
    resetPlayback(std::move(vault), video_id, true);
}

void PlayerWindow::resetPlayback(std::shared_ptr<videovault::core::Vault> vault,
    const std::int64_t video_id, const bool startVideo)
{
    static_cast<PlayerGlass*>(controlsOverlay_)->reveal();
    static_cast<PlayerGlass*>(topOverlay_)->reveal();
    // Tear down the current playback exactly like the destructor: the worker
    // loop only exits when quit_ is set, so joining without it blocks the UI
    // thread forever (the old worker keeps streaming). quit_ must be reset
    // afterwards or the new worker would exit on its first loop check.
    quit_.store(true);
    timer_.stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    quit_.store(false);

    decoder_.close();
    if (audioOutput_ != nullptr) {
        audioOutput_->stop();
        audioOutput_->deleteLater();
        audioOutput_ = nullptr;
    }
    if (audioSink_ != nullptr) {
        audioSinkAtomic_.store(nullptr);
        audioSink_->deleteLater();
        audioSink_ = nullptr;
    }
    audioSetupDone_ = false;
    audioReady_.store(false);
    audioSampleRate_.store(0);
    audioChannels_.store(0);
    audioBaseMs_.store(-1);
    audioSeekPending_.store(false);
    audioDeviceFailed_.store(false);
    // Epochs stay monotonic across playbacks: resetting to zero could collide
    // with a worker from the previous playback that still holds that value.
    audioSeekSerial_.fetch_add(1);
    workerAudioSerial_ = deviceAudioSerial_ = 0;
    seekRequested_.store(false);
    rebaseRequested_.store(false);

    // Reset per-playback state so the new video starts clean.
    ended_.store(false);
    playing_.store(false);
    openFailed_.store(false);
    seekErrorPending_.store(false);
    seeking_.store(false);
    clickPending_ = false;
    panning_ = false;
    autoSized_ = false;
    durationMs_.store(0);
    videoWidth_.store(0);
    videoHeight_.store(0);
    imageModeFlag_.store(false);
    if (imageMode_) {
        // Back to the video chrome (playback controls instead of zoom row).
        imageMode_ = false;
        surface_->setPhotoMode(false);
        static_cast<PlayerGlass*>(controlsOverlay_)->setVideo(true);
        uiHideTimer_.setInterval(telegram_video::hideDelay);
        surface_->setInfo({});
        positionSlider_->show();
        positionLabel_->show();
        timeLeftLabel_->show();
        playButton_->show();
        previousButton_->show();
        nextButton_->show();
        muteButton_->show();
        volumeSlider_->show();
        if (repeatButton_ != nullptr) {
            repeatButton_->show();
        }
        if (shuffleButton_ != nullptr) {
            shuffleButton_->show();
        }
        if (popButton_ != nullptr) {
            popButton_->show();
        }
        zoomOutButton_->hide();
        zoomLabel_->hide();
        zoomInButton_->hide();
        fitButton_->hide();
        imageZoom_ = 0.0;
        imageOffset_ = QPointF();
        surface_->setView(0.0, QPointF());
        rotateButton_->hide();
        repeatButton_->hide();
        shuffleButton_->hide();
        fullscreenButton_->show();
        videoMenuButton_->show();
        if (photoName_ != nullptr) photoName_->hide();
        if (photoMoreButton_ != nullptr) photoMoreButton_->hide();
        if (photoRotateButton_ != nullptr) photoRotateButton_->hide();
        layoutChrome();
    }
    positionSlider_->setRange(0, 1);
    positionSlider_->setValue(0);
    positionSlider_->setEnabled(true);
    playButton_->setEnabled(true);
    positionLabel_->setText(QStringLiteral("0:00"));
    timeLeftLabel_->setText(QString(QChar(0x2212)) + QStringLiteral("0:00"));
    errorShown_ = false;
    advanceArmed_ = false;
    ++playbackEpoch_;
    surface_->setText(QStringLiteral("Loading…"));
    surface_->clearOverlay();
    showChrome();
    updateCenterButton();

    // Start the new playback. The timer->tick connection is made once in the
    // constructor — connecting again here would run tick() twice per tick.
    vault_ = std::move(vault);
    video_id_ = video_id;
    ++stillEpoch_;
    if (startVideo) {
        timer_.setInterval(16);
        timer_.start();
        playing_.store(true);
        worker_ = std::thread(&PlayerWindow::workerLoop, this);
    }
}

void PlayerWindow::playStill(
    std::shared_ptr<videovault::core::Vault> vault, const std::int64_t video_id) {
    // Reset the old playback without launching a decoder just to join it.
    resetPlayback(std::move(vault), video_id, false);
    playing_.store(false);
    ended_.store(false);
    imageModeFlag_.store(true);
    enterImageMode();
    surface_->setInfo({});
    surface_->setText(QStringLiteral("Decrypting %1…").arg(windowTitle()));
    const int epoch = ++stillEpoch_;
    const auto source = vault_;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, epoch] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (epoch != stillEpoch_) {
            return;
        }
        if (image.isNull()) {
            // Qt could not read it. The video decoder still handles WebP and
            // anything else FFmpeg knows, through the same decrypt stream.
            playVideo(vault_, video_id_);
            return;
        }
        {
            std::lock_guard<std::mutex> guard(frameMutex_);
            latestFrame_ = image;
        }
        surface_->setText({});
        surface_->setFrame(image);
        resetImageFit();
    });
    watcher->setFuture(QtConcurrent::run(stillImagePool(), [source, video_id] {
        std::vector<unsigned char> plain;
        std::uint64_t offset = 0;
        constexpr std::size_t kChunk = 1024U * 1024U;
        for (;;) {
            auto chunk = source->read_video_range(video_id, offset, kChunk);
            if (!chunk || chunk.value().empty()) {
                break;
            }
            plain.insert(plain.end(), chunk.value().begin(), chunk.value().end());
            offset += static_cast<std::uint64_t>(chunk.value().size());
            if (chunk.value().size() < kChunk || plain.size() > 80U * 1024U * 1024U) {
                break;
            }
        }
        QImage image;
        if (!plain.empty()) {
            image.loadFromData(plain.data(), static_cast<int>(plain.size()));
        }
        return image;
    }));
}

void PlayerWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    layoutChrome();
    {
        std::lock_guard<std::mutex> guard(sizeMutex_);
        surfaceSize_ = surface_->size();
    }
    surface_->update();
    if (imageMode_) {
        // "Fit 41%" is a function of the window size.
        updateImageInfo();
    }
}

void PlayerWindow::closeEvent(QCloseEvent* event) {
    timer_.stop();
    QDialog::closeEvent(event);
}

void PlayerWindow::updatePopIcon() {
    if (popButton_ == nullptr) {
        return;
    }
    popButton_->setIcon(telegram::icon(detached_
        ? QStringLiteral("player_pip_enlarge") : QStringLiteral("player_pip"), Qt::white));
    if (fullscreenButton_) {
        fullscreenButton_->setIcon(telegram::icon(fullscreen_ ? QStringLiteral("player_minimize")
            : QStringLiteral("player_fullscreen"), Qt::white));
    }
    popButton_->setToolTip(detached_ ? QStringLiteral("Play in the main window") : QStringLiteral("Pop out"));
}

void PlayerWindow::refreshTransportIcons() {
    if (repeatButton_ != nullptr) {
        const bool active = repeatMode_ != 0;
        const QColor ink = active ? QColor(255, 255, 255) : QColor(255, 255, 255, 120);
        repeatButton_->setIcon(makePlayerIcon(
            repeatMode_ == 2 ? PlayerIcon::RepeatOne : PlayerIcon::Repeat, 18, ink));
        repeatButton_->setToolTip(repeatMode_ == 2
            ? QStringLiteral("Repeat this video")
            : (repeatMode_ == 1
                ? QStringLiteral("Repeat all")
                : QStringLiteral("Repeat off")));
    }
    if (shuffleButton_ != nullptr) {
        shuffleButton_->setIcon(makePlayerIcon(
            PlayerIcon::Shuffle, 18, shuffle_ ? QColor(255, 255, 255) : QColor(255, 255, 255, 120)));
        shuffleButton_->setToolTip(shuffle_
            ? QStringLiteral("Shuffle on")
            : QStringLiteral("Shuffle"));
    }
    const bool roam = shuffle_ || repeatMode_ == 1;
    if (previousButton_ != nullptr) {
        previousButton_->setEnabled(roam || clipHasPrevious_);
    }
    if (nextButton_ != nullptr) {
        nextButton_->setEnabled(roam || clipHasNext_);
    }
    if (!imageMode_) {
        previousButton_->setVisible(chromeVisible_ && previousButton_->isEnabled());
        nextButton_->setVisible(chromeVisible_ && nextButton_->isEnabled());
    }
}

void PlayerWindow::presentViewer(QWidget* owner) {
    if (viewerOverlay_) {
        if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
        show();
        raise();
        activateWindow();
        return;
    }
    if (fullscreen_) toggleFullscreen();
    // Detach from a hidden state so no stale embedded surface stays behind.
    hide();
    detached_ = true;
    viewerOverlay_ = true;
    frameReady_ = false;
    setParent(nullptr);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint
        | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::NonModal);
    setMinimumSize(480, 360);
    // Window controls overlay the picture; no title-bar row takes space from it.
    layout()->removeWidget(windowCaption_);
    windowCaption_->setParent(surface_);
    windowCaption_->layout()->setContentsMargins(0, 0, 0, 0);
    captionTitle_->hide();
    if (auto* back = topOverlay_->findChild<QPushButton*>(QStringLiteral("playerBackToLibrary"))) back->hide();
    windowCaption_->setFixedSize(132, 32);
    windowCaption_->setStyleSheet(QStringLiteral(
        "QWidget#playerCaption { background: transparent; border: none; }"));
    QScreen* target = owner ? QGuiApplication::screenAt(owner->mapToGlobal(owner->rect().center())) : nullptr;
    if (!target) target = QGuiApplication::primaryScreen();
    if (target) {
        const QRect area = target->availableGeometry();
        const QSize initial(std::min(960, area.width()), std::min(620, area.height()));
        setGeometry(QRect(area.center() - QPoint(initial.width() / 2, initial.height() / 2), initial));
    }
    updatePopIcon();
    show();
    layout()->activate();
    layoutChrome();
    showChrome();
    uiHideTimer_.start();
    raise();
    activateWindow();
}

void PlayerWindow::clearSurface() {
    if (surface_ == nullptr) return;
    surface_->setFrame(QImage());
    surface_->setText({});
    surface_->update();
    surface_->repaint();
}

void PlayerWindow::repaintFrame() {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull()) frame = lastFrame_;
    if (frame.isNull() || surface_ == nullptr) return;
    surface_->setFrame(frame);
    surface_->update();
    if (topOverlay_ != nullptr) topOverlay_->update();
    if (controlsOverlay_ != nullptr) controlsOverlay_->update();
}

void PlayerWindow::prepareDock() {
    viewerOverlay_ = false;
    if (fullscreen_) {
        fullscreen_ = false;
        QWidget* host = detached_ ? static_cast<QWidget*>(this) : window();
        host->showNormal();
        unsetCursor();
    }
    if (auto* back = topOverlay_->findChild<QPushButton*>(QStringLiteral("playerBackToLibrary"))) back->show();
    detached_ = false;
    viewerOverlay_ = false;
    frameReady_ = false;
    // Hide before changing window flags. Converting a *visible* top-level
    // widget into a child leaves its old native window painted on Windows,
    // which is the duplicate video surface users see.
    hide();
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_NativeWindow, false);
    if (windowCaption_ != nullptr) {
        windowCaption_->hide();
    }
    updatePopIcon();
}

void PlayerWindow::detach() {
    viewerOverlay_ = false;
    if (fullscreen_) {
        fullscreen_ = false;
        QWidget* host = detached_ ? static_cast<QWidget*>(this) : window();
        host->showNormal();
        unsetCursor();
    }
    detached_ = true;
    if (isMaximized()) showNormal();
    setParent(nullptr);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint
        | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    if (windowCaption_ != nullptr) {
        windowCaption_->show();
    }
    updatePopIcon();
    resize(960, 640);
    show();
    raise();
    activateWindow();
}

void PlayerWindow::applyDetachedFrame() {
#ifdef Q_OS_WIN
    if (!detached_ || frameReady_) {
        return;
    }
    frameReady_ = true;
    HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    const MARGINS shadow{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    const int corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(this)
#endif
}

void PlayerWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (detached_) {
        applyDetachedFrame();
    }
}

void PlayerWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange && windowMax_ != nullptr) {
        static_cast<PlayerCaptionButton*>(windowMax_)->setGlyph(
            isMaximized() ? PlayerCaptionGlyph::Restore : PlayerCaptionGlyph::Maximize);
    }
    QDialog::changeEvent(event);
}

bool PlayerWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    if (detached_ && eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
            if (IsZoomed(msg->hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                const int pad = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                params->rgrc[0].left += pad;
                params->rgrc[0].top += pad;
                params->rgrc[0].right -= pad;
                params->rgrc[0].bottom -= pad;
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST && windowCaption_ != nullptr && windowCaption_->isVisible()) {
            const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            const QPoint pos = mapFromGlobal(global);
            const bool zoomed = IsZoomed(msg->hwnd);
            const QPoint inCaption = windowCaption_->mapFromGlobal(global);
            if (windowCaption_->rect().contains(inCaption)) {
                QWidget* child = windowCaption_->childAt(inCaption);
                if (qobject_cast<QAbstractButton*>(child) != nullptr) {
                    *result = HTCLIENT;
                } else if (!zoomed && inCaption.y() < 4) {
                    *result = HTTOP;
                } else {
                    *result = HTCAPTION;
                }
                return true;
            }
            if (!zoomed) {
                constexpr int border = 6;
                const bool left = pos.x() < border;
                const bool right = pos.x() >= width() - border;
                const bool bottom = pos.y() >= height() - border;
                if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
                if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
                if (left) { *result = HTLEFT; return true; }
                if (right) { *result = HTRIGHT; return true; }
                if (bottom) { *result = HTBOTTOM; return true; }
            }
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QDialog::nativeEvent(eventType, message, result);
}

void PlayerWindow::keyPressEvent(QKeyEvent* event) {
    // The app-level event filter normally consumes player keys before they
    // reach the focused widget; this path covers the dialog itself having
    // focus (e.g. no child focused).
    if (handleKey(event)) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

bool PlayerWindow::handleKey(QKeyEvent* event) {
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    switch (event->key()) {
    case Qt::Key_N:
    case Qt::Key_MediaNext:
        emit nextRequested();
        return true;
    case Qt::Key_P:
    case Qt::Key_MediaPrevious:
        emit previousRequested();
        return true;
    case Qt::Key_M:
        toggleMute();
        return true;
    case Qt::Key_Left:
        if (control) {
            emit previousRequested();
            return true;
        }
        break;
    case Qt::Key_Right:
        if (control) {
            emit nextRequested();
            return true;
        }
        break;
    default:
        break;
    }
    if (imageMode_) {
        switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoomImage(1.25, QPointF(surface_->width() / 2.0, surface_->height() / 2.0));
            return true;
        case Qt::Key_Minus:
            zoomImage(0.8, QPointF(surface_->width() / 2.0, surface_->height() / 2.0));
            return true;
        case Qt::Key_R:
            if (imageMode_) {
                rotateImage();
                return true;
            }
            break;
        case Qt::Key_0:
            resetImageFit();
            return true;
        case Qt::Key_1:
            setImageZoom100();
            return true;
        case Qt::Key_Space:
        case Qt::Key_Left:
        case Qt::Key_Right:
            // No seek/play semantics for still images.
            return true;
        default:
            break;
        }
    }
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlayPause();
        return true;
    case Qt::Key_Left:
        seekRelative(-10000);
        return true;
    case Qt::Key_Right:
        seekRelative(10000);
        return true;
    case Qt::Key_Up:
        setVolumePercent(volumePercent_ + 5);
        return true;
    case Qt::Key_Down:
        setVolumePercent(volumePercent_ - 5);
        return true;
    case Qt::Key_F:
        toggleFullscreen();
        return true;
    case Qt::Key_Escape:
        if (fullscreen_) {
            toggleFullscreen();
        } else {
            close();
        }
        return true;
    default:
        return false;
    }
}

void PlayerWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    clickPending_ = false;
    if (imageMode_) {
        event->accept();
        return;
    }
    // A double-click means fullscreen, not two play/pause toggles.
    toggleFullscreen();
    event->accept();
}

bool PlayerWindow::eventFilter(QObject* watched, QEvent* event) {
    // The filter is installed on qApp so the player's shortcuts work no
    // matter which child widget holds focus: the dialog's own keyPressEvent
    // never fires while a child (slider/button) consumes keys first.
    QObject* current = watched;
    while (current != nullptr && current != this) {
        current = current->parent();
    }
    if (current == this) {
        if (event->type() == QEvent::KeyPress && QApplication::activePopupWidget()) {
            return false; // The overflow owns arrows/Enter/Escape while open.
        }
        if (event->type() == QEvent::KeyPress
            && handleKey(static_cast<QKeyEvent*>(event))) {
            return true; // consumed by the player
        }
        if (event->type() == QEvent::WindowDeactivate) windowDragPressed_ = false;
        if (watched == surface_ && !imageMode_) {
            if (event->type() == QEvent::MouseButtonPress) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                windowDragPressed_ = detached_ && !fullscreen_ && !isMaximized()
                    && mouse->button() == Qt::LeftButton;
                windowDragMoved_ = false;
                windowDragStart_ = mouse->globalPos();
                windowDragPosition_ = pos();
            } else if (event->type() == QEvent::MouseMove && windowDragPressed_) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (!mouse->buttons().testFlag(Qt::LeftButton)) {
                    windowDragPressed_ = false;
                } else {
                    const QPoint delta = mouse->globalPos() - windowDragStart_;
                    if (delta.manhattanLength() >= QApplication::startDragDistance()) windowDragMoved_ = true;
                    if (windowDragMoved_) {
                        clickPending_ = false;
                        move(windowDragPosition_ + delta);
                        return true;
                    }
                }
            } else if (event->type() == QEvent::MouseButtonRelease && windowDragPressed_) {
                windowDragPressed_ = false;
                if (windowDragMoved_) {
                    clickPending_ = false;
                    return true; // Moving a window is not a play/pause click.
                }
            }
        }
        if (event->type() == QEvent::MouseButtonPress && !imageMode_) showChrome();
        if (watched == surface_ && !imageMode_ && event->type() == QEvent::MouseButtonDblClick
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            clickPending_ = false;
            windowDragPressed_ = false;
            suppressVideoRelease_ = true;
            toggleFullscreen();
            return true;
        }
        if (event->type() == QEvent::MouseMove) {
            // Any mouse movement brings the chrome back and postpones the
            // auto-hide, in video and photo mode alike.
            showChrome();
            uiHideTimer_.start();
        }
        if (imageMode_ && event->type() == QEvent::MouseButtonDblClick) {
            const QPointF anchor = surface_->mapFromGlobal(
                static_cast<QMouseEvent*>(event)->globalPos());
            if (imageZoom_ <= 0.0) {
                // Fit -> actual size, zooming toward the clicked point.
                animateImageZoom(1.0, anchor);
            } else {
                resetImageFit();
            }
            showChrome();
            uiHideTimer_.start();
            return true;
        }
        if (imageMode_ && watched == surface_ && event->type() == QEvent::NativeGesture) {
            const auto* gesture = static_cast<QNativeGestureEvent*>(event);
            if (gesture->gestureType() == Qt::ZoomNativeGesture && gesture->value() != 0.0) {
                zoomImage(1.0 + gesture->value(),
                    surface_->mapFromGlobal(gesture->globalPos()));
                showChrome();
                uiHideTimer_.start();
                return true;
            }
        }
        if (imageMode_ && event->type() == QEvent::Wheel) {
            const auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->angleDelta().y() != 0) {
                zoomImage(wheel->angleDelta().y() > 0 ? 1.25 : 0.8,
                    surface_->mapFromGlobal(wheel->globalPos()));
                return true;
            }
        }
        if (imageMode_ && watched == surface_) {
            // Image viewer: wheel zooms (anchored at the cursor), left-drag
            // pans, and a plain click is consumed (no play/pause toggle).
            if (event->type() == QEvent::MouseButtonPress) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    panning_ = true;
                    panStartPos_ = mouse->pos();
                    panStartOffset_ = imageOffset_;
                    if (imageZoom_ <= 0.0) {
                        // Starting a drag from the fitted view: pin the same
                        // rectangle the surface paints, so the picture does not jump.
                        QImage frame;
                        {
                            std::lock_guard<std::mutex> guard(frameMutex_);
                            frame = latestFrame_;
                        }
                        if (!frame.isNull() && frame.width() > 0 && frame.height() > 0) {
                            const QSize fitted = frame.size().scaled(
                                surface_->size(), Qt::KeepAspectRatio);
                            imageZoom_ = static_cast<double>(fitted.width())
                                / static_cast<double>(frame.width());
                            panStartOffset_ = QPointF(
                                (surface_->width() - fitted.width()) / 2.0,
                                (surface_->height() - fitted.height()) / 2.0);
                            imageOffset_ = panStartOffset_;
                            surface_->setView(imageZoom_, imageOffset_);
                            updateZoomLabel();
                        }
                    }
                    panAnchor_ = mouse->globalPos();
                    surface_->grabMouse();
                    surface_->setCursor(Qt::ClosedHandCursor);
                    return true;
                }
            }
            if (event->type() == QEvent::MouseMove && panning_) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                imageOffset_ = panStartOffset_ + QPointF(mouse->globalPos() - panAnchor_);
                clampImageOffset();
                surface_->setView(imageZoom_, imageOffset_);
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease && panning_) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    panning_ = false;
                    surface_->releaseMouse();
                    surface_->setCursor(Qt::OpenHandCursor);
                    return true;
                }
            }
        }
        if (watched == surface_ && event->type() == QEvent::MouseButtonRelease
            && !imageMode_) {
            // Single click on the video toggles play/pause (a delayed
            // single-click so a double-click still toggles fullscreen).
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                if (suppressVideoRelease_) {
                    suppressVideoRelease_ = false;
                    return true;
                }
                clickPending_ = true;
                QTimer::singleShot(QApplication::doubleClickInterval(), this, [this] {
                    if (!clickPending_) {
                        return;
                    }
                    clickPending_ = false;
                    togglePlayPause();
                });
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

// Runs on the decode worker thread. Streams through MediaDecoder (whose
// read-ahead window may block the worker on refills — never the UI), paces
// frames against a monotonic real-time clock, and publishes frames/audio for
// the UI thread to consume.
void PlayerWindow::workerLoop() {
    QString open_error;
    if (!decoder_.open(vault_, video_id_, &open_error)) {
        {
            std::lock_guard<std::mutex> guard(errorMutex_);
            openError_ = open_error;
        }
        openFailed_.store(true);
        playing_.store(false);
        return;
    }

    loopFrames_.clear();
    loopPcm_.clear();
    loopCapture_ = true;
    stableFrameW_ = 0;
    stableFrameH_ = 0;
    stableFrameRun_ = 0;
    durationMs_.store(decoder_.duration_ms());
    if (durationMs_.load() > 1500) {
        loopCapture_ = false;
    }
    videoWidth_.store(decoder_.video_width());
    videoHeight_.store(decoder_.video_height());
    // A still-image codec (png/mjpeg/...) is opened as an image viewer:
    // single frame, zoom/pan instead of playback controls. Duration alone
    // cannot detect this — FFmpeg reports ~40 ms for a 1-frame image.
    imageModeFlag_.store(is_image_codec(decoder_.video_codec_name()));
    if (decoder_.has_audio()) {
        audioSampleRate_.store(decoder_.audio_sample_rate());
        audioChannels_.store(decoder_.audio_channels());
        audioReady_.store(true);
    }

    QElapsedTimer clock;
    clock.start();
    std::int64_t wallBase = 0, wallStart = 0, master = 0;
    std::int64_t tailStart = -1, tailBase = 0;
    bool eof = false;
    std::deque<DecodedFrame> pending;
    workerAudioSerial_ = audioSeekSerial_.load();
    // The PCM timeline starts at the media start; the sink is empty here.
    if (decoder_.has_audio()) audioBaseMs_.store(0);

    // QAudioOutput pulls ahead by at most its device buffer. Counting actual
    // PCM pulls (not elapsed time or decoded samples) bounds that fixed lead
    // and, crucially, cannot accumulate drift across decoder underruns.
    const auto masterPosition = [&]() -> std::int64_t {
        auto* sink = audioSinkAtomic_.load();
        const int rate = audioSampleRate_.load(), channels = audioChannels_.load();
        const auto base = audioBaseMs_.load();
        if (sink && base >= 0 && rate > 0 && channels > 0 && !audioDeviceFailed_.load()) {
            // The picture follows the audible PCM position (device pulls minus
            // its own buffer lead). Both sides advance on one device clock, so
            // a slow decoder stall freezes the picture instead of drifting it.
            master = std::max(master, video_pacing::audibleMs(
                base, sink->consumed_samples(), channels, rate,
                audioDeviceBufferMs_.load()));
            wallBase = master;
            wallStart = clock.elapsed();
        } else {
            master = std::max(master, wallBase + clock.elapsed() - wallStart);
        }
        return master;
    };

    while (!quit_.load()) {
        bool seek = false;
        std::int64_t target = 0;
        {
            std::lock_guard<std::mutex> guard(audioPipelineMutex_);
            seek = seekRequested_.exchange(false);
            if (seek) {
                target = seekTargetMs_.load();
                workerAudioSerial_ = audioSeekSerial_.load();
            }
        }
        if (seek) {
            pending.clear();
            eof = false;
            tailStart = -1;
            dropLoopCapture();
            const int status = decoder_.seek_to(target);
            if (status < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(status, errbuf, sizeof(errbuf));
                std::lock_guard<std::mutex> guard(seekErrorMutex_);
                seekErrorMessage_ = QStringLiteral("Seek failed: %1").arg(QString::fromUtf8(errbuf));
                seekErrorPending_.store(true);
                // A failed seek did not move the decoder: do not relabel its
                // remaining samples/frame timestamps as the requested target.
                target = playheadMs_.load();
            } else {
                // Decode references normally all the way to target; the decoder
                // trims only pre-target PCM, retaining the boundary packet tail.
                DecodedFrame frame;
                while (!quit_.load() && workerAudioSerial_ == audioSeekSerial_.load()
                    && decoder_.decode_next_video_frame(&frame)) {
                    if (frame.pts_ms >= target && frameSizeMatches(frame)) {
                        pending.push_back(std::move(frame));
                        break;
                    }
                }
            }
            {
                std::lock_guard<std::mutex> guard(audioPipelineMutex_);
                if (workerAudioSerial_ != audioSeekSerial_.load()) continue;
                // The decoder trims pre-target PCM, so the next sample appended
                // to the cleared sink is the new audio origin.
                audioBaseMs_.store(target);
                master = wallBase = target;
                wallStart = clock.elapsed();
                audioSeekPending_.store(false);
                if (!pending.empty()) {
                    publishFrame(pending.front().image, pending.front().pts_ms);
                }
            }
            feedAudio();
            continue;
        }
        if (rebaseRequested_.exchange(false)) {
            // Only the silent/device-failed fallback is rebased on resume.
            // An audio clock already stopped advancing when the device paused.
            wallBase = master;
            wallStart = clock.elapsed();
        }
        if (!playing_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (workerAudioSerial_ != audioSeekSerial_.load()) continue;
        feedAudio();
        auto now = masterPosition();
        auto* sink = audioSinkAtomic_.load();
        const int rate = audioSampleRate_.load(), channels = audioChannels_.load();
        const auto buffered = sink ? sink->buffered_samples() : 0U;
        const auto maxPcm = rate > 0 && channels > 0
            ? static_cast<std::size_t>(rate) * static_cast<std::size_t>(channels) / 2U : 0U;

        // Bounded lookahead keeps demuxing audio while a future picture waits.
        // Never skip compressed packets/reference decoding. Only presentation
        // of superseded pictures is skipped below. One decoder call may add
        // an interleave burst of PCM; it is drained before another call.
        const bool audioHungry = maxPcm > 0 && buffered < maxPcm / 2U
            && !audioDeviceFailed_.load();
        if (!eof && (pending.size() < 8U || audioHungry)
            && (pending.empty() || pending.back().pts_ms < now + 200 || audioHungry)
            && (maxPcm == 0 || buffered < maxPcm || audioDeviceFailed_.load())) {
            DecodedFrame frame;
            if (decoder_.decode_next_video_frame(&frame)) {
                if (workerAudioSerial_ != audioSeekSerial_.load()) continue;
                if (frameSizeMatches(frame)) {
                    noteLoopFrame(frame.image, frame.pts_ms);
                    pending.push_back(std::move(frame));
                }
                // A hungry audio device may have forced a decode while the
                // picture queue was full; discard pictures already superseded.
                while (pending.size() > 8U && pending[1].pts_ms <= now) pending.pop_front();
            } else {
                eof = true;
            }
            feedAudio(); // includes PCM produced by the final EOF/drain call
            now = masterPosition();
        }

        // At EOF let the device's final buffer drain, then allow video-only
        // tails to finish. Never declare ended at demux EOF and truncate PCM.
        if (eof && (!sink || sink->buffered_samples() == 0U || audioDeviceFailed_.load())) {
            if (tailStart < 0) { tailStart = clock.elapsed(); tailBase = now; }
            now = std::max(now, tailBase + clock.elapsed() - tailStart);
        } else {
            tailStart = -1;
        }
        {
            std::lock_guard<std::mutex> guard(audioPipelineMutex_);
            if (workerAudioSerial_ != audioSeekSerial_.load()) continue;
            // Publish the newest due frame, not a burst of already-late frames.
            while (pending.size() > 1U && pending[1].pts_ms <= now) pending.pop_front();
            if (!pending.empty() && (pending.front().pts_ms <= now || imageModeFlag_.load())) {
                publishFrame(pending.front().image, pending.front().pts_ms);
                pending.pop_front();
            }
            playheadMs_.store(now);
        }
        if (eof && pending.empty() && tailStart >= 0
            && clock.elapsed() - tailStart >= audioDeviceBufferMs_.load()) {
            if (repeatWatch_.load() == 2 && !imageModeFlag_.load()) {
                // Audio-bearing loops use the same seek epoch/device reset as
                // manual seeks, not the old independent cached wall-clock loop.
                requestSeek(0);
            } else {
                ended_.store(true);
                playing_.store(false);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    decoder_.close();
}

void PlayerWindow::publishFrame(const QImage& image, const std::int64_t pts_ms) {
    if (imageModeFlag_.load()) {
        // Images keep their full resolution so zooming shows real pixels.
        decoder_.set_display_size(videoWidth_.load(), videoHeight_.load());
    } else {
        QSize surface;
        {
            std::lock_guard<std::mutex> guard(sizeMutex_);
            surface = surfaceSize_;
        }
        const int nativeW = videoWidth_.load();
        const int nativeH = videoHeight_.load();
        if (!surface.isEmpty() && nativeW > 0 && nativeH > 0) {
            // Fit inside the player. The picture gets larger with the window
            // and is never stretched.
            const double scale = std::min(
                static_cast<double>(surface.width()) / static_cast<double>(nativeW),
                static_cast<double>(surface.height()) / static_cast<double>(nativeH));
            decoder_.set_display_size(
                std::max(1, static_cast<int>(std::lround(nativeW * scale))),
                std::max(1, static_cast<int>(std::lround(nativeH * scale))));
        }
    }
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        latestFrame_ = image;
    }
    frameDirty_.store(true);
}

void PlayerWindow::restartFromStart() {
    if (imageMode_ || imageModeFlag_.load()) {
        return;
    }
    advanceArmed_ = false;
    doSeek(0);
    playing_.store(true);
    playIconPlaying_ = true;
    if (playButton_ != nullptr) {
        playButton_->setIcon(makePlayerIcon(PlayerIcon::Pause, 26, Qt::white));
    }
    rebaseRequested_.store(true);
    if (audioOutput_ != nullptr) {
        audioOutput_->resume();
    }
}

void PlayerWindow::dropLoopCapture() {
    loopCapture_ = false;
    loopFrames_.clear();
    loopPcm_.clear();
}

bool PlayerWindow::frameSizeMatches(const DecodedFrame& frame) {
    const int width = frame.source_width;
    const int height = frame.source_height;
    if (width <= 0 || height <= 0) {
        return true;
    }
    // The size is not settled yet. Keep showing frames, and lock onto the
    // size that actually repeats. A loop seek can then ignore the odd frames
    // the decoder emits before it is back to that size.
    if (stableFrameRun_ < 3) {
        if (width == stableFrameW_ && height == stableFrameH_) {
            ++stableFrameRun_;
        } else {
            stableFrameW_ = width;
            stableFrameH_ = height;
            stableFrameRun_ = 1;
        }
        return true;
    }
    return width == stableFrameW_ && height == stableFrameH_;
}

void PlayerWindow::noteLoopFrame(const QImage& image, const std::int64_t ptsMs) {
    if (!loopCapture_ || image.isNull()) {
        return;
    }
    if (ptsMs > 1500 || static_cast<int>(loopFrames_.size()) >= 48) {
        dropLoopCapture();
        return;
    }
    loopFrames_.push_back(LoopPicture{image, ptsMs});
}

void PlayerWindow::noteLoopAudio(const std::int16_t* samples, const std::size_t count) {
    if (!loopCapture_ || samples == nullptr || count == 0U) {
        return;
    }
    // About two seconds of stereo. Past that, this is not a short clip.
    constexpr std::size_t kMaxSamples = 48000U * 2U * 2U;
    if (loopPcm_.size() + count > kMaxSamples) {
        dropLoopCapture();
        return;
    }
    loopPcm_.insert(loopPcm_.end(), samples, samples + count);
}

void PlayerWindow::replayCachedLoop() {
    const std::int64_t span = std::max<std::int64_t>(
        loopFrames_.back().ptsMs + 40, durationMs_.load());
    while (!quit_.load() && repeatWatch_.load() == 2 && !seekRequested_.load()) {
        if (AudioSink* sink = audioSinkAtomic_.load()) {
            sink->clear();
            if (!loopPcm_.empty()) {
                sink->append(loopPcm_.data(), loopPcm_.size());
            }
        }
        QElapsedTimer clock;
        clock.start();
        std::int64_t origin = 0;
        bool frozen = false;
        std::size_t index = 0U;
        playheadMs_.store(0);
        while (!quit_.load() && repeatWatch_.load() == 2 && !seekRequested_.load()) {
            if (!playing_.load()) {
                frozen = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (frozen) {
                origin = clock.elapsed() - playheadMs_.load();
                frozen = false;
            }
            const std::int64_t now = std::max<std::int64_t>(0, clock.elapsed() - origin);
            while (index < loopFrames_.size() && loopFrames_[index].ptsMs <= now) {
                publishFrame(loopFrames_[index].image, loopFrames_[index].ptsMs);
                playheadMs_.store(loopFrames_[index].ptsMs);
                ++index;
            }
            if (now >= span) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void PlayerWindow::feedAudio() {
    AudioSink* sink = audioSinkAtomic_.load();
    if (sink == nullptr) {
        return;
    }
    if (workerAudioSerial_ != audioSeekSerial_.load()) {
        // A seek was requested: discard PCM decoded from the old position
        // instead of appending it to the cleared sink under the new origin.
        (void)decoder_.take_audio_samples();
        return;
    }
    auto samples = decoder_.take_audio_samples();
    if (!samples.empty()) {
        noteLoopAudio(samples.data(), samples.size());
        sink->append(samples.data(), samples.size());
    }
    // Backpressure: never let the sink buffer more than ~1 s of audio while
    // the decoder catches up after a stall or seek.
    const int rate = audioSampleRate_.load();
    if (rate > 0 && sink->buffered_samples() > static_cast<std::size_t>(rate)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void PlayerWindow::requestSeek(const std::int64_t target_ms) {
    seekTargetMs_.store(target_ms);
    audioSeekPending_.store(true);
    // Bump the epoch before the request is visible: PCM the decoder produces
    // from the pre-seek position must not be labelled with the new timeline.
    audioSeekSerial_.fetch_add(1);
    seekRequested_.store(true);
}

void PlayerWindow::doSeek(const std::int64_t target_ms) {
    // A manual seek means the video is no longer "ended" — otherwise the
    // tick would keep snapping the slider to the end and pressing Play after
    // the video had finished would force a restart from 0.
    ended_.store(false);
    requestSeek(target_ms);
    if (audioSink_ != nullptr) {
        audioSink_->clear();
    }
    if (audioOutput_ != nullptr) {
        audioOutput_->stop();
        audioOutput_->start(audioSink_);
        if (!playing_.load()) {
            audioOutput_->suspend();
        }
    }
    audioOffsetMs_ = target_ms;
    playheadMs_.store(target_ms);
    positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
        target_ms, positionSlider_->maximum())));
    updatePositionLabel();
}

void PlayerWindow::togglePlayPause() {
    if (ended_.load()) {
        ended_.store(false);
        requestSeek(0);
        if (audioSink_ != nullptr) {
            audioSink_->clear();
        }
    }
    playing_.store(!playing_.load());
    playIconPlaying_ = playing_.load();
    playButton_->setIcon(makePlayerIcon(
        playing_.load() ? PlayerIcon::Pause : PlayerIcon::Play, 26, Qt::white));
    updateCenterButton();
    if (playing_.load()) {
        rebaseRequested_.store(true);
        if (audioOutput_ != nullptr) {
            audioOutput_->resume();
        }
        uiHideTimer_.start();
    } else {
        if (audioOutput_ != nullptr) {
            audioOutput_->suspend();
        }
        uiHideTimer_.stop();
        showChrome();
    }
}

void PlayerWindow::seekRelative(const std::int64_t delta_ms) {
    const std::int64_t duration = durationMs_.load();
    const std::int64_t target = std::clamp<std::int64_t>(
        playheadMs_.load() + delta_ms, 0, duration > 0 ? duration : playheadMs_.load());
    doSeek(target);
}

void PlayerWindow::setVolumePercent(const int percent) {
    volumeSlider_->setValue(std::clamp(percent, 0, 100));
}

void PlayerWindow::toggleMute() {
    muteButton_->setChecked(!muteButton_->isChecked());
}

void PlayerWindow::toggleFullscreen() {
    QWidget* host = detached_ ? static_cast<QWidget*>(this) : window();
    if (!fullscreen_) fullscreenWasMaximized_ = host->isMaximized();
    fullscreen_ = !fullscreen_;
    fullscreenButton_->setIcon(telegram::icon(fullscreen_
        ? QStringLiteral("player_minimize") : QStringLiteral("player_fullscreen"), Qt::white));
    fullscreenButton_->setToolTip(fullscreen_ ? QStringLiteral("Exit fullscreen (Esc)")
        : QStringLiteral("Fullscreen (F)"));
    if (fullscreen_) {
        if (windowCaption_ != nullptr) {
            windowCaption_->hide();
        }
        showChrome();
        host->showFullScreen();
        uiHideTimer_.start();
    } else {
        if (fullscreenWasMaximized_) host->showMaximized(); else host->showNormal();
        unsetCursor();
        if (windowCaption_ != nullptr && detached_) {
            windowCaption_->show();
        }
        showChrome();
        uiHideTimer_.start();
    }
}

void PlayerWindow::applyVolume() {
    if (audioOutput_ != nullptr) {
        audioOutput_->setVolume(muted_ ? 0.0f : (volumePercent_ / 100.0f));
    }
    muteButton_->setIcon(telegram::icon((muted_ || volumePercent_ == 0)
        ? QStringLiteral("player_volume_off") : (volumePercent_ < 50
            ? QStringLiteral("player_volume_small") : QStringLiteral("player_volume_on")), Qt::white));
}

// Shows the chrome (top bar + bottom controls; the center button follows the
// playback state). Reveal is immediate; hide uses an upstream-style cached
// 600 ms fade, avoiding the opacity effect that broke Windows child routing.
void PlayerWindow::showChrome() {
    chromeVisible_ = true;
    unsetCursor();
    if (!imageMode_) surface_->unsetCursor();
    if (imageMode_) {
        topOverlay_->hide();
        controlsOverlay_->hide();
        photoName_->show();
        photoMoreButton_->show();
        photoRotateButton_->show();
        previousButton_->setVisible(previousButton_->isEnabled());
        nextButton_->setVisible(nextButton_->isEnabled());
    } else {
        static_cast<PlayerGlass*>(controlsOverlay_)->reveal();
        static_cast<PlayerGlass*>(topOverlay_)->reveal();
        previousButton_->setVisible(previousButton_->isEnabled());
        nextButton_->setVisible(nextButton_->isEnabled());
    }
    // Fullscreen is just the picture, and a photo has no title bar at all
    // (Telegram puts the name at the bottom-left instead).
    if (fullscreen_ || imageMode_) {
        topOverlay_->hide();
    } else {
        topOverlay_->show();
    }
    if (viewerOverlay_ && !fullscreen_) {
        windowCaption_->show();
        windowCaption_->raise();
    }
    if (!imageMode_) {
        controlsOverlay_->show();
    }
    updateCenterButton();
}

// Hides the chrome (only while the video is playing; pausing re-shows it).
void PlayerWindow::hideChrome() {
    if (!chromeVisible_) {
        return;
    }
    chromeVisible_ = false;
    if (viewerOverlay_) windowCaption_->hide();
    if (imageMode_) {
        topOverlay_->hide();
        controlsOverlay_->hide();
        photoName_->hide();
        photoMoreButton_->hide();
        photoRotateButton_->hide();
        previousButton_->hide();
        nextButton_->hide();
    } else {
        static_cast<PlayerGlass*>(topOverlay_)->fadeOut();
        static_cast<PlayerGlass*>(controlsOverlay_)->fadeOut();
        previousButton_->hide();
        nextButton_->hide();
        surface_->setCursor(Qt::BlankCursor);
    }
    centerPlayButton_->hide();
}

// Positions the chrome strips and the center button over the surface.
void PlayerWindow::layoutChrome() {
    if (topOverlay_ == nullptr) {
        return;
    }
    const int w = surface_->width();
    const int h = surface_->height();
    const bool overlayCaption = detached_ && windowCaption_->parentWidget() == surface_;
    topOverlay_->setGeometry(0, 0, overlayCaption ? std::max(0, w - 132) : w, 56);
    if (overlayCaption) {
        windowCaption_->move(w - 132, 0);
        windowCaption_->raise();
    }
    // Keep the photo pill unchanged; video uses the source controller geometry.
    if (!imageMode_) {
        controlsOverlay_->setFixedHeight(telegram_video::controllerHeight);
        controlsOverlay_->setGeometry(telegram_video::controllerRect(surface_->size()));
        const auto g = telegram_video::controls(controlsOverlay_->width(),
            positionLabel_->fontMetrics().horizontalAdvance(positionLabel_->text()),
            timeLeftLabel_->fontMetrics().horizontalAdvance(timeLeftLabel_->text()),
            positionLabel_->fontMetrics().height());
        positionLabel_->setGeometry(g.elapsed);
        timeLeftLabel_->setGeometry(g.remaining);
        positionSlider_->setGeometry(g.seek);
        playButton_->setGeometry(g.play);
        muteButton_->setGeometry(g.volumeToggle);
        volumeSlider_->setGeometry(g.volume);
        fullscreenButton_->setGeometry(g.fullscreen);
        popButton_->setGeometry(g.popout);
        videoMenuButton_->setGeometry(g.menu);
        for (auto* button : {previousButton_, nextButton_}) {
            button->setFixedSize(40, 40);
            button->setStyleSheet(QStringLiteral(
                "QPushButton { background: transparent; border: none; border-radius: 20px; }"
                "QPushButton:hover { background: rgba(255, 255, 255, 36); }"
                "QPushButton:pressed { background: rgba(255, 255, 255, 56); }"
                "QPushButton:disabled { background: transparent; }"));
        }
        previousButton_->move(8, (h - previousButton_->height()) / 2);
        nextButton_->move(w - nextButton_->width() - 8, (h - nextButton_->height()) / 2);
        return;
    }
    // A photo is shown the way Telegram shows it: the picture is the window,
    // with no control card floating over it.
    controlsOverlay_->hide();
    centerPlayButton_->hide();
    layoutPhotoChrome();
}

// Geometry from media_view.style: 46x54 cells anchored to the bottom-right,
// 36px hover circles, 90px edge nav strips 47px below the top, and the name
// 14px from the left / 26px from the bottom.
void PlayerWindow::layoutPhotoChrome() {
    if (surface_ == nullptr || photoName_ == nullptr) {
        return;
    }
    const QSize viewport = surface_->size();
    const QRect moreCell = telegram_photo::cell(1, viewport);
    const QRect rotateCell = telegram_photo::cell(2, viewport);
    photoMoreButton_->setGeometry(telegram_photo::hoverCircle(moreCell));
    photoRotateButton_->setGeometry(telegram_photo::hoverCircle(rotateCell));

    const QFontMetrics metrics(photoName_->font());
    const int room = std::max(80, rotateCell.x() - telegram_photo::footerLeft - 8);
    const QString shown = metrics.elidedText(title_, Qt::ElideMiddle, room);
    photoName_->setText(shown);
    photoName_->setGeometry(telegram_photo::footer(
        viewport, metrics.horizontalAdvance(shown), metrics.height()));

    const int glyph = telegram_photo::hoverDiameter;
    for (auto* button : {previousButton_, nextButton_}) {
        button->setFixedSize(glyph, glyph);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; border-radius: 18px; }"
            "QPushButton:hover { background: rgba(0, 0, 0, 120); }"
            "QPushButton:pressed { background: rgba(0, 0, 0, 175); }"));
    }
    previousButton_->move(telegram_photo::navCentre(false, viewport) - QPoint(glyph / 2, glyph / 2));
    nextButton_->move(telegram_photo::navCentre(true, viewport) - QPoint(glyph / 2, glyph / 2));

    photoName_->raise();
    photoMoreButton_->raise();
    photoRotateButton_->raise();
    previousButton_->raise();
    nextButton_->raise();
}

// The overflow menu carries the actions Telegram keeps out of the photo chrome
// (zoom and fit are gestures in the source) plus a close action, because our
// viewer hides the top bar entirely for photos.
void PlayerWindow::showPhotoMenu() {
    if (photoMoreButton_ == nullptr) {
        return;
    }
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background:#292929; color:white; border:1px solid #454545; padding:4px; }"
        "QMenu::item { padding:8px 24px; } QMenu::item:selected { background:#454545; }"));
    auto* zoomIn = menu.addAction(QStringLiteral("Zoom in"));
    auto* zoomOut = menu.addAction(QStringLiteral("Zoom out"));
    auto* actual = menu.addAction(QStringLiteral("Actual size"));
    auto* fit = menu.addAction(QStringLiteral("Fit to window"));
    menu.addSeparator();
    auto* rotate = menu.addAction(QStringLiteral("Rotate right"));
    auto* closeAction = menu.addAction(QStringLiteral("Close"));
    const QPointF centre(surface_->width() / 2.0, surface_->height() / 2.0);
    auto* chosen = menu.exec(photoMoreButton_->mapToGlobal(
        QPoint(photoMoreButton_->width(), photoMoreButton_->height())));
    if (chosen == zoomIn) animateImageZoom((imageZoom_ <= 0.0 ? fittedImageZoom() : imageZoom_) * 1.25, centre);
    else if (chosen == zoomOut) animateImageZoom((imageZoom_ <= 0.0 ? fittedImageZoom() : imageZoom_) * 0.8, centre);
    else if (chosen == actual) setImageZoom100();
    else if (chosen == fit) resetImageFit();
    else if (chosen == rotate) rotateImage();
    else if (chosen == closeAction) close();
    showChrome();
    uiHideTimer_.start();
}

// The big center button is visible whenever playback is not running (and
// never in image mode).
void PlayerWindow::updateCenterButton() {
    if (imageMode_) {
        centerPlayButton_->hide();
        return;
    }
    centerPlayButton_->hide();
}

// Switches to the existing photo controls and fitted view, with photo idle hiding.
void PlayerWindow::enterImageMode() {
    imageMode_ = true;
    surface_->setPhotoMode(true);
    static_cast<PlayerGlass*>(controlsOverlay_)->setVideo(false);
    uiHideTimer_.setInterval(telegram_photo::idleHideMs);
    positionSlider_->hide();
    positionLabel_->hide();
    timeLeftLabel_->hide();
    playButton_->hide();
    previousButton_->hide();
    nextButton_->hide();
    backButton_->hide();
    forwardButton_->hide();
    muteButton_->hide();
    volumeSlider_->hide();
    cacheCombo_->hide();
    if (repeatButton_ != nullptr) {
        repeatButton_->hide();
    }
    if (shuffleButton_ != nullptr) {
        shuffleButton_->hide();
    }
    // The card and the whole video row stay hidden: photos use Telegram's
    // floating cells instead of a floating bar.
    for (auto* child : controlsOverlay_->findChildren<QWidget*>(QString(),
             Qt::FindDirectChildrenOnly)) {
        child->hide();
    }
    controlsOverlay_->hide();
    topOverlay_->hide();
    zoomOutButton_->hide();
    zoomLabel_->hide();
    zoomInButton_->hide();
    fitButton_->hide();
    rotateButton_->hide();
    updateCenterButton();
    showChrome();
    layoutChrome();
    resetImageFit();
    surface_->setCursor(Qt::OpenHandCursor);
    // Chrome fades out of the way after a couple of idle seconds, exactly like
    // the video player; any mouse move brings it back.
    uiHideTimer_.start();
}

// Zooms by `factor`, keeping the image point under `cursor_pos` fixed. From
// the fitted view the fitted geometry is used as the starting scale.
void PlayerWindow::zoomImage(const double factor, const QPointF& cursor_pos) {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull()) {
        return;
    }
    double old_zoom = imageZoom_;
    QPointF old_offset = imageOffset_;
    if (old_zoom <= 0.0) {
        const double fit = std::min(
            static_cast<double>(surface_->width()) / frame.width(),
            static_cast<double>(surface_->height()) / frame.height());
        old_zoom = fit;
        old_offset = QPointF(
            (surface_->width() - frame.width() * fit) / 2.0,
            (surface_->height() - frame.height() * fit) / 2.0);
    }
    const double new_zoom = std::clamp(old_zoom * factor, 0.02, 64.0);
    const QPointF image_point = (cursor_pos - old_offset) / old_zoom;
    imageZoom_ = new_zoom;
    imageOffset_ = cursor_pos - image_point * new_zoom;
    clampImageOffset();
    surface_->setView(imageZoom_, imageOffset_);
    updateZoomLabel();
}

double PlayerWindow::fittedImageZoom() {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull() || surface_ == nullptr || frame.width() <= 0 || frame.height() <= 0) {
        return 1.0;
    }
    return std::min(static_cast<double>(surface_->width()) / frame.width(),
                    static_cast<double>(surface_->height()) / frame.height());
}

void PlayerWindow::animateImageZoom(const double target, const QPointF& anchor) {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull() || surface_ == nullptr || imageZoomAnim_ == nullptr) {
        return;
    }
    const double from = imageZoom_ <= 0.0 ? fittedImageZoom() : imageZoom_;
    const QPointF fromOffset = imageZoom_ <= 0.0
        ? QPointF((surface_->width() - frame.width() * from) / 2.0,
                  (surface_->height() - frame.height() * from) / 2.0)
        : imageOffset_;
    const double to = std::clamp(target, 0.02, 64.0);
    // The image coordinate under the anchor stays under it for every frame.
    imageZoomAnchor_ = anchor;
    imageZoomImagePoint_ = (anchor - fromOffset) / std::max(1e-6, from);
    if (std::abs(to - from) < 0.0005) {
        imageZoom_ = to;
        imageOffset_ = anchor - imageZoomImagePoint_ * to;
        clampImageOffset();
        surface_->setView(imageZoom_, imageOffset_);
        updateZoomLabel();
        return;
    }
    imageZoomAnim_->stop();
    imageZoomAnim_->setStartValue(from);
    imageZoomAnim_->setEndValue(to);
    imageZoomAnim_->start();
}

void PlayerWindow::rotateImage() {
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        if (latestFrame_.isNull()) {
            return;
        }
        // Quarter turns are lossless; the viewer keeps no second copy to undo
        // them, so four presses return the picture to its original state.
        latestFrame_ = latestFrame_.transformed(QTransform().rotate(90));
        if (surface_ != nullptr) {
            surface_->setFrame(latestFrame_);
        }
    }
    resetImageFit();
    showChrome();
    uiHideTimer_.start();
}

void PlayerWindow::clampImageOffset() {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull() || imageZoom_ <= 0.0 || surface_ == nullptr) {
        return;
    }
    const double pictureWidth = frame.width() * imageZoom_;
    const double pictureHeight = frame.height() * imageZoom_;
    const double viewWidth = surface_->width();
    const double viewHeight = surface_->height();
    if (pictureWidth <= viewWidth) {
        imageOffset_.setX((viewWidth - pictureWidth) / 2.0);
    } else {
        imageOffset_.setX(std::clamp(imageOffset_.x(), viewWidth - pictureWidth, 0.0));
    }
    if (pictureHeight <= viewHeight) {
        imageOffset_.setY((viewHeight - pictureHeight) / 2.0);
    } else {
        imageOffset_.setY(std::clamp(imageOffset_.y(), viewHeight - pictureHeight, 0.0));
    }
}

// Back to the aspect-fit view.
void PlayerWindow::resetImageFit() {
    imageZoom_ = 0.0;
    imageOffset_ = QPointF();
    surface_->setView(0.0, QPointF());
    updateZoomLabel();
}

// 100% zoom (one image pixel per screen pixel), centered.
void PlayerWindow::setImageZoom100() {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull()) {
        return;
    }
    imageZoom_ = 1.0;
    imageOffset_ = QPointF(
        (surface_->width() - frame.width()) / 2.0,
        (surface_->height() - frame.height()) / 2.0);
    surface_->setView(imageZoom_, imageOffset_);
    updateZoomLabel();
}

void PlayerWindow::updateZoomLabel() {
    zoomLabel_->setText(imageZoom_ <= 0.0
        ? QStringLiteral("Fit")
        : QStringLiteral("%1%").arg(qRound(imageZoom_ * 100.0)));
    updateImageInfo();
}

// The chip in the corner of a still: pixel size and the scale actually on
// screen, so "Fit" is not a guess.
void PlayerWindow::updateImageInfo() {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull() || surface_ == nullptr) {
        if (surface_ != nullptr) {
            surface_->setInfo({});
        }
        return;
    }
    double shown = imageZoom_;
    if (shown <= 0.0) {
        shown = std::min(static_cast<double>(surface_->width()) / frame.width(),
                         static_cast<double>(surface_->height()) / frame.height());
        shown = std::min(shown, 1.0);  // fit never magnifies
    }
    // Telegram's media viewer shows no zoom/size text over a photo; the
    // picture itself carries the feedback. Keep the chip empty on purpose.
    surface_->setInfo({});
    Q_UNUSED(shown)
}

// Sizes the window so the video surface matches the video's pixel dimensions
// (never upscaling; capped to 80% of the screen area for large videos). Runs
// once per player window, from the UI timer, as soon as the worker reports
// the dimensions.
void PlayerWindow::fitToVideoSize() {
    // The window stays the size the user gave it. The picture is not resized to match.
}

void PlayerWindow::tick() {
    // Fit the window to the video's pixel size once, as soon as the worker
    // reports the dimensions (before the first frame is even shown).
    fitToVideoSize();

    // Switch to the image viewer as soon as the worker reports an image.
    if (imageModeFlag_.load() && !imageMode_) {
        enterImageMode();
    }

    // Keep the center button and the play/pause icon in sync with state
    // changes that originate on the worker thread (e.g. EOF).
    updateCenterButton();
    if (!imageMode_) {
        const bool playing = playing_.load();
        if (playing != playIconPlaying_) {
            playIconPlaying_ = playing;
            playButton_->setIcon(makePlayerIcon(
                playing ? PlayerIcon::Pause : PlayerIcon::Play, 26, Qt::white));
        }
    }

    // Open-failure feedback.
    if (openFailed_.load() && !errorShown_) {
        errorShown_ = true;
        QString message;
        {
            std::lock_guard<std::mutex> guard(errorMutex_);
            message = openError_;
        }
        surface_->setText(QStringLiteral("Cannot open this file:\n%1").arg(message));
        playButton_->setEnabled(false);
        positionSlider_->setEnabled(false);
    }

    // Transient overlay for a failed seek (the worker logged the reason).
    if (seekErrorPending_.exchange(false)) {
        QString message;
        {
            std::lock_guard<std::mutex> guard(seekErrorMutex_);
            message = seekErrorMessage_;
        }
        surface_->setOverlay(message);
        QTimer::singleShot(3500, this, [this] { surface_->clearOverlay(); });
    }

    // Create the audio device once the worker has opened the stream.
    if (!audioSetupDone_ && audioReady_.load()) {
        audioSetupDone_ = true;
        QAudioFormat format;
        format.setSampleRate(audioSampleRate_.load());
        format.setChannelCount(audioChannels_.load());
        format.setSampleSize(16);
        format.setCodec(QStringLiteral("audio/pcm"));
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);
        audioSink_ = new AudioSink(this);
        audioSinkAtomic_.store(audioSink_);
        audioOutput_ = new QAudioOutput(format, this);
        const int rate = std::max(1, audioSampleRate_.load());
        const int channels = std::max(1, audioChannels_.load());
        const int bytesPerSecond = rate * channels * 2;
        // A little under a fifth of a second: enough to survive a heavy decode
        // burst without giving the pull clock a large, variable lead.
        audioOutput_->setBufferSize(std::max(rate / 2, bytesPerSecond / 5));
        audioDeviceBufferMs_.store(std::clamp(
            audioOutput_->bufferSize() * 1000 / bytesPerSecond, 0, 250));
        applyVolume();
        // The device is started by the frame gate below, not here. Starting it
        // before the first decoded picture lets audio run ahead of the video.
    }

    // Hold the audio device until the first picture is on screen: otherwise
    // the PCM clock starts running while the decoder is still opening, and the
    // audio leads the video for the whole file. Audio-only streams have no
    // picture to wait for.
    const bool audioPrimed = videoWidth_.load() <= 0 || !lastFrame_.isNull();
    // QAudioOutput parks in IdleState (and stops pulling) after an empty read;
    // wake it while there may be fresh samples.
    if (audioOutput_ != nullptr && playing_.load() && audioPrimed
        && (audioOutput_->state() == QAudio::IdleState
            || audioOutput_->state() == QAudio::StoppedState)) {
        audioOutput_->start(audioSink_);
        audioDeviceBufferMs_.store(std::clamp(
            audioOutput_->bufferSize() * 1000
                / std::max(1, std::max(1, audioSampleRate_.load())
                    * std::max(1, audioChannels_.load()) * 2),
            0, 250));
    }

    // Paint the latest frame from the worker.
    if (frameDirty_.exchange(false)) {
        QImage image;
        {
            std::lock_guard<std::mutex> guard(frameMutex_);
            image = latestFrame_;
        }
        if (!image.isNull()) {
            lastFrame_ = image;
            surface_->setFrame(image);
            if (topOverlay_ != nullptr) {
                topOverlay_->update();
            }
            if (controlsOverlay_ != nullptr) {
                controlsOverlay_->update();
            }
        }
    }

    // Seek-bar range becomes known once the worker reports the duration.
    const std::int64_t duration = durationMs_.load();
    if (duration > 0 && positionSlider_->maximum() != static_cast<int>(duration)) {
        positionSlider_->setRange(0, static_cast<int>(std::max<std::int64_t>(duration, 1)));
        // Clicking the slider groove jumps by a page: ~5% of the video
        // (minimum 2 s) so a single click actually moves the position.
        positionSlider_->setPageStep(static_cast<int>(std::max<std::int64_t>(
            duration / 20, 2000)));
    }

    const std::int64_t pts = playheadMs_.load();
    // Never fight the user's slider drag: the tick must not overwrite the
    // position while the thumb is being moved (that made scrubbing snap back
    // and look like the seek never happened).
    if (!seeking_.load()) {
        positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
            pts, positionSlider_->maximum())));
    }
    updatePositionLabel();

    if (ended_.load() && duration > 0 && !seeking_.load()) {
        positionSlider_->setValue(positionSlider_->maximum());
        if (!advanceArmed_ && !imageMode_ && !imageModeFlag_.load()) {
            advanceArmed_ = true;
            const int epoch = playbackEpoch_;
            const int waitMs = repeatMode_ == 1 ? 0 : 500;
            QTimer::singleShot(waitMs, this, [this, epoch] {
                if (epoch != playbackEpoch_ || !ended_.load() || imageMode_) {
                    return;
                }
                emit playbackFinished();
            });
        }
    }
}

void PlayerWindow::presentClip(
    const QString& title, const int index, const int count,
    const bool hasPrevious, const bool hasNext) {
    title_ = title.isEmpty() ? QStringLiteral("Video") : title;
    setWindowTitle(title_);
    if (titleLabel_ != nullptr) {
        titleLabel_->setText(title_);
    }
    if (captionTitle_ != nullptr) {
        captionTitle_->setText(title_);
    }
    if (indexLabel_ != nullptr) {
        if (count > 0 && index > 0) {
            indexLabel_->setText(QStringLiteral("%1 / %2").arg(index).arg(count));
            indexLabel_->show();
        } else {
            indexLabel_->clear();
            indexLabel_->hide();
        }
    }
    clipHasPrevious_ = hasPrevious;
    clipHasNext_ = hasNext;
    refreshTransportIcons();
}

QString PlayerWindow::formatTime(const std::int64_t ms) const {
    const auto total_seconds = ms / 1000;
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto seconds = total_seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

void PlayerWindow::updatePositionLabel() {
    const auto duration = durationMs_.load();
    const auto position = seeking_.load() ? positionSlider_->sliderPosition()
        : (ended_.load() ? duration : playheadMs_.load());
    positionLabel_->setText(telegram_video::duration(position));
    timeLeftLabel_->setText(telegram_video::remaining(duration, position));
    if (!imageMode_) layoutChrome();
}

} // namespace videovault::app
