// UI adaptations from Telegram Desktop media/player (widget, panel, dropdown,
// button, volume controller and media_player.style), copyright Telegram Desktop
// contributors, GPL-3.0-or-later. See ../third_party/telegram-player/LEGAL,
// LICENSE and NOTICE for upstream attribution. Vault playback remains native.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "videovault/app/music_window.hpp"

#include "videovault/app/media_decoder.hpp"
#include "videovault/app/local_audio_pipeline.hpp"
#include "videovault/app/music_queue_edit.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/app/telegram_music_controls.hpp"
#include "videovault/app/telegram_player_icons.hpp"
#include <QWheelEvent>
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QAudioFormat>
#include <QBuffer>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QLinearGradient>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QStyle>
#include <QtConcurrent/QtConcurrentRun>
#include <QSystemTrayIcon>

#ifdef Q_OS_WIN
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <SystemMediaTransportControlsInterop.h>

#include <chrono>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef small
#undef small
#endif
#endif
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "videovault/app/audio_time_stretch.hpp"
#include <cstddef>
#include <functional>
#include <random>
#include <utility>

namespace videovault::app {

namespace {

// Telegram media_player.style: 35px chrome + 1px line, 30px buttons.
constexpr int kBarHeight = telegram_music::barHeight;
QColor playerAccent() { return QApplication::palette().color(QPalette::Highlight); }
// The player is chrome, not video: unlike the video overlay it follows the live
// Light/Dark palette. These used to be dark literals (a black bar, white labels,
// a #1c1c1e popup card), which left the whole music player dark inside a light
// theme. Every one of them is read at paint time, so a theme switch repaints.
QColor playerBar() {
    return QApplication::palette().color(QPalette::Window);
}
QColor playerCard() {
    return QApplication::palette().color(QPalette::Base);
}
QColor playerInk() {
    return QApplication::palette().color(QPalette::WindowText);
}
QColor playerSecondaryInk() {
    return QApplication::palette().color(QPalette::PlaceholderText);
}
QColor playerHover() {
    auto color = QApplication::palette().color(QPalette::WindowText);
    color.setAlpha(18);
    return color;
}
QColor playerHairline() {
    QColor color = QApplication::palette().color(QPalette::Midlight);
    color.setAlpha(isDarkTheme() ? 90 : 160);
    return color;
}
QColor playerGroove() {
    QColor color = QApplication::palette().color(QPalette::Midlight);
    color.setAlpha(isDarkTheme() ? 255 : 200);
    return color;
}

// Slider painted straight from the palette. A stylesheet's literal groove and
// handle colours do not re-resolve when the theme changes, which is how the
// music bar kept dark controls inside a light window.
class PlayerSlider : public QSlider {
public:
    explicit PlayerSlider(QWidget* parent = nullptr) : QSlider(Qt::Horizontal, parent) {
        setFocusPolicy(Qt::NoFocus);
        setMinimumHeight(14);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { e->ignore(); return; }
        setSliderDown(true);
        jump(e->pos().x());
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (isSliderDown()) { jump(e->pos().x()); e->accept(); }
        else QSlider::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !isSliderDown()) { e->ignore(); return; }
        jump(e->pos().x());
        setSliderDown(false);
        e->accept();
    }
    void jump(int x) { setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x, std::max(1, width()))); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        const qreal centre = height() / 2.0;
        const QRectF groove(1, centre - 1.5, std::max(1, width() - 2), 3);
        painter.setBrush(playerGroove());
        painter.drawRoundedRect(groove, 1.5, 1.5);
        const int span = maximum() - minimum();
        const qreal fraction = span <= 0 ? 0.0 : qreal(value() - minimum()) / span;
        QRectF played = groove;
        played.setWidth(groove.width() * fraction);
        painter.setBrush(playerAccent());
        painter.drawRoundedRect(played, 1.5, 1.5);
        const qreal radius = (underMouse() || isSliderDown()) ? 6.0 : 5.0;
        painter.setBrush(playerAccent());
        painter.drawEllipse(QPointF(groove.left() + groove.width() * fraction, centre),
            radius, radius);
    }
};

QString speedLabel(const int milli) {
    const double shown = std::round(milli / 100.0) / 10.0;
    return QString::number(shown) + QLatin1Char('X');
}


} // namespace

QPixmap paintNotePlate(int side);

// Overlap-add stretch so 0.5x–2.5x keeps the original pitch. Playback at
// 1x bypasses this and writes the decoded samples unchanged.
class TimeStretch {
public:
    void reset() { stretch_.reset(); }

    std::vector<std::int16_t> process(
        const std::int16_t* samples,
        const std::size_t count,
        const int channels,
        const double speed,
        std::size_t* consumedFrames) {
        // Pitch-preserving speed change through FFmpeg's atempo filter (the
        // filter Telegram's own build enables), with a normalised fallback where
        // libavfilter is unavailable. The old overlap-add lost level and
        // comb-filtered the signal at anything but 1.0x.
        return stretch_.process(samples, count, channels, LocalAudioReader::rate,
            speed, consumedFrames);
    }

    std::vector<std::int16_t> flush(
        const int channels,
        const double speed,
        std::size_t* consumedFrames) {
        return stretch_.flush(channels, LocalAudioReader::rate, speed, consumedFrames);
    }

private:
    videovault::app::AudioTimeStretch stretch_;
};

enum class Glyph {
    Play,
    Pause,
    Previous,
    Next,
    VolumeOff,
    VolumeLow,
    VolumeFull,
    Repeat,
    RepeatOne,
    Reverse,
    Shuffle,
    Close,
};

class GlyphButton final : public QAbstractButton {
public:
    explicit GlyphButton(const Glyph glyph, QWidget* parent)
        : QAbstractButton(parent), glyph_(glyph) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        morph_.setDuration(200); // mediaPlayerPlayIcon.duration
        connect(&morph_, &QVariantAnimation::valueChanged, this, [this] { update(); });
        ripple_.setDuration(300);
        connect(&ripple_, &QVariantAnimation::valueChanged, this, [this] { update(); });
        connect(this, &QAbstractButton::pressed, this, [this] {
            ripple_.stop(); ripple_.setStartValue(0.); ripple_.setEndValue(1.); ripple_.start();
        });
    }

    void setGlyph(const Glyph glyph) {
        if (glyph_ == glyph) return;
        const bool animate = (glyph_ == Glyph::Play || glyph_ == Glyph::Pause)
            && (glyph == Glyph::Play || glyph == Glyph::Pause);
        if (animate) fromShape_ = currentShape();
        morph_.stop();
        glyph_ = glyph;
        if (animate) { morph_.setStartValue(0.); morph_.setEndValue(1.); morph_.start(); }
        update();
    }
    void setActive(const bool active) {
        active_ = active;
        update();
    }
    void setLabel(const QString& label) {
        label_ = label;
        setText(label);
        setAccessibleName(tr("Playback speed %1").arg(label));
        update();
    }

    std::function<void()> entered;
    std::function<void()> left;

protected:
    void enterEvent(QEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
        if (entered) {
            entered();
        }
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
        if (left) {
            left();
        }
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (isEnabled() && (ripple_.state() == QAbstractAnimation::Running || hasFocus())) {
            const auto progress = ripple_.currentValue().toReal();
            auto color = playerAccent();
            color.setAlpha(hasFocus() ? 20 : int(32 * (1. - progress)));
            painter.setPen(hasFocus() ? QPen(playerAccent(), 1.) : QPen(Qt::NoPen));
            painter.setBrush(color);
            const auto radius = hasFocus() ? 12. : 12. * progress;
            painter.drawEllipse(QPointF(width()/2., 17.), radius, radius);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink());
        // style_media_player iconPosition differs by control, not centered in
        // a generic 32px toolbar: transport (0,5), volume (5,6), close (4,6).
        QPoint pos(0, 5);
        if (glyph_ == Glyph::VolumeOff || glyph_ == Glyph::VolumeLow || glyph_ == Glyph::VolumeFull) pos = QPoint(5, 6);
        else if (glyph_ == Glyph::Close && label_.isEmpty()) pos = QPoint(4, 6);
        else if (glyph_ == Glyph::Repeat || glyph_ == Glyph::RepeatOne) pos = QPoint(2, 5);
        else if (glyph_ == Glyph::Reverse || glyph_ == Glyph::Shuffle) pos = QPoint(2, 6);
        if (height() != telegram_music::controlHeight)
            pos = QPoint((width()-24)/2, (height()-24)/2); // floating-card controls
        const QRect box(pos, QSize(24, 24));
        if (!label_.isEmpty()) {
            const QRect inner = rect().adjusted(0, 6, 0, 0);
            telegram::paint(painter, QRectF(inner.center().x()-12, inner.center().y()-12, 24, 24),
                QStringLiteral("player_speed"), ink());
            painter.setPen(ink());
            QFont font(QStringLiteral("Segoe UI"));
            font.setPixelSize(11);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(inner, Qt::AlignCenter, label_);
            return;
        }
        paintGlyph(painter, box);
    }

private:
    QColor ink() const {
        if (!isEnabled()) {
            return playerSecondaryInk();
        }
        if (active_) {
            return playerAccent();
        }
        if (underMouse()) {
            return playerInk();
        }
        return playerSecondaryInk();
    }

    void paintGlyph(QPainter& painter, const QRect& box) const {
        // Real Telegram artwork, not approximate hand-painted arrows/speakers.
        const char* asset = nullptr;
        switch (glyph_) {
        case Glyph::Previous: asset = "player_backward"; break;
        case Glyph::Next: asset = "player_forward"; break;
        case Glyph::VolumeOff: asset = "player_mini_off"; break;
        case Glyph::VolumeLow: asset = "player_mini_half"; break;
        case Glyph::VolumeFull: asset = "player_mini_full"; break;
        case Glyph::Repeat: asset = "player_repeat"; break;
        case Glyph::RepeatOne: asset = "player_repeat_single"; break;
        case Glyph::Reverse: asset = "player_order"; break;
        case Glyph::Shuffle: asset = "player_shuffle"; break;
        case Glyph::Close: asset = "panel_close"; break;
        case Glyph::Play: case Glyph::Pause: {
            painter.save();
            painter.translate(box.topLeft());
            telegram_music::paintShape(painter, currentShape(), ink());
            painter.restore();
            return;
        }
        }
        if (asset) telegram::paint(painter, box, QString::fromLatin1(asset), ink());
    }

    telegram_music::Shape currentShape() const {
        const auto to = glyph_ == Glyph::Pause
            ? telegram_music::pauseShape() : telegram_music::playShape();
        return morph_.state() == QAbstractAnimation::Running
            ? telegram_music::interpolate(fromShape_, to, morph_.currentValue().toReal()) : to;
    }
    QVariantAnimation ripple_{this};
    QVariantAnimation morph_{this};
    telegram_music::Shape fromShape_ = telegram_music::playShape();

    Glyph glyph_;
    bool active_{false};
    QString label_;
};

class PlayerPopup : public QWidget {
public:
    explicit PlayerPopup(QWidget* owner) : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(playerHairline()));
        painter.setBrush(playerCard());
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 8, 8);
    }
};

class MenuRow final : public QAbstractButton {
public:
    explicit MenuRow(const QString& label, QWidget* parent)
        : QAbstractButton(parent), label_(label) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(label);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFixedHeight(36);
    }

    void setIcon(const QString& asset, bool check = true) { asset_ = asset; check_ = check; }
    void setMarked(const bool marked) {
        marked_ = marked;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse() || hasFocus()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(playerHover());
            painter.drawRect(rect());
        }
        painter.setPen(marked_ ? playerAccent() : playerInk());
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(13);
        painter.setFont(font);
        if (!asset_.isEmpty()) telegram::paint(painter, QRectF(13, (height()-24)/2., 24, 24),
            asset_, marked_ ? playerAccent() : playerSecondaryInk());
        painter.drawText(rect().adjusted(check_ ? 54 : 49, 0, -32, 0), Qt::AlignVCenter | Qt::AlignLeft, label_);
        if (marked_ && check_) telegram::paint(painter, QRectF(width()-32, (height()-24)/2., 24, 24),
            QStringLiteral("player_check"), playerAccent());
    }
    void enterEvent(QEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QString label_;
    bool marked_{false};
    bool check_{true};
    QString asset_;
};

class TrackRow final : public QAbstractButton {
public:
    explicit TrackRow(const QString& title, const QString& detail, QWidget* parent)
        : QAbstractButton(parent), title_(title), detail_(detail) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFixedHeight(50); // overviewFileLayout: 7 + 36 + 6 + 1
    }

    void setCurrent(const bool current) {
        current_ = current;
        update();
    }
    void setPlaying(bool playing) { playing_ = playing; update(); }
    void setCaption(const QString& title, const QString& detail) {
        title_ = title;
        detail_ = detail;
        update();
    }
    void setArt(const QPixmap& art) {
        art_ = art;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(playerHover());
            painter.drawRoundedRect(rect().adjusted(6, 2, -6, -2), 8, 8);
        }
        const QRect icon(17, 7, 36, 36);
        painter.setPen(Qt::NoPen);
        if (!art_.isNull()) {
            QPainterPath clip;
            clip.addEllipse(QRectF(icon));
            painter.save();
            painter.setClipPath(clip);
            painter.drawPixmap(icon, art_);
            painter.restore();
        } else {
            painter.setBrush(playerAccent());
            painter.drawEllipse(icon);
        }
        if (!art_.isNull()) {
            painter.setBrush(QColor(0, 0, 0, 70));
            painter.drawEllipse(icon);
        }
        const bool pause = current_ && playing_;
        const QSizeF glyphSize = pause ? QSizeF(12, 14) : QSizeF(18, 16);
        const QRectF glyphRect(QPointF(icon.x() + (icon.width()-glyphSize.width())/2.,
            icon.y() + (icon.height()-glyphSize.height())/2.), glyphSize);
        telegram::paint(painter, glyphRect, pause
            ? QStringLiteral("playlist_pause") : QStringLiteral("playlist_play"), Qt::white);
        painter.setPen(current_ ? playerAccent() : playerInk());
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(13);
        painter.setFont(font);
        const int textLeft = 63; // 17 + 36 + 10
        const QString elided = QFontMetrics(font).elidedText(title_, Qt::ElideRight, width() - textLeft - 16);
        painter.drawText(QRect(textLeft, 7, width() - textLeft - 17, 18), Qt::AlignLeft | Qt::AlignVCenter, elided);
        painter.setPen(playerSecondaryInk());
        font.setPixelSize(11);
        painter.setFont(font);
        painter.drawText(QRect(textLeft, 25, width() - textLeft - 17, 16), Qt::AlignLeft | Qt::AlignVCenter, detail_);
    }
    void enterEvent(QEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QString title_;
    QString detail_;
    QPixmap art_;
    bool current_{false};
    bool playing_{false};
};

class PlaylistPopup final : public PlayerPopup {
public:
    explicit PlaylistPopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedWidth(telegram_music::playlistWidth);
        scroll_ = new QScrollArea(this);
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setWidgetResizable(true);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; }"
            "QScrollBar:vertical { width: 8px; background: transparent; margin: 4px 0; }"
            "QScrollBar::handle:vertical { background: palette(mid); border-radius: 4px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));
        scroll_->viewport()->setAutoFillBackground(false);
        host_ = new QWidget();
        host_->setAutoFillBackground(false);
        host_->setStyleSheet(QStringLiteral("background: transparent;"));
        scroll_->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
        rows_ = new QVBoxLayout(host_);
        rows_->setContentsMargins(0, 0, 0, 0);
        rows_->setSpacing(0);
        scroll_->setWidget(host_);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->addWidget(scroll_);
    }

    void sync(const std::vector<MusicTrack>& tracks, const int index) {
        std::vector<std::int64_t> ids;
        ids.reserve(tracks.size());
        for (const auto& track : tracks) {
            ids.push_back(track.id);
        }
        if (ids != ids_) {
            ids_ = ids;
            while (QLayoutItem* item = rows_->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            rowWidgets_.clear();
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
                auto* row = new TrackRow(QString(), QString(), host_);
                rows_->addWidget(row);
                rowWidgets_.push_back(row);
                connect(row, &QAbstractButton::clicked, this, [this, i] {
                    if (picked) {
                        picked(i);
                    }
                });
            }
        }
        for (int i = 0; i < static_cast<int>(rowWidgets_.size()); ++i) {
            QString title;
            QString detail;
            if (labels) {
                labels(i, &title, &detail);
            }
            rowWidgets_[static_cast<std::size_t>(i)]->setCaption(title, detail);
            rowWidgets_[static_cast<std::size_t>(i)]->setCurrent(i == index);
        }
        const int list = std::min(static_cast<int>(tracks.size()) * 50, telegram_music::playlistMaxHeight);
        scroll_->setFixedHeight(std::max(50, list));
        setFixedHeight(scroll_->height() + 12);
        if (index >= 0 && index < static_cast<int>(rowWidgets_.size())) {
            scroll_->ensureWidgetVisible(rowWidgets_[static_cast<std::size_t>(index)], 0, 24);
        }
    }

    void setArt(const std::int64_t id, const QPixmap& art) {
        for (std::size_t i = 0; i < ids_.size() && i < rowWidgets_.size(); ++i) {
            if (ids_[i] == id) {
                rowWidgets_[i]->setArt(art);
            }
        }
    }

    void setPlaying(bool playing) {
        for (auto* row : rowWidgets_) row->setPlaying(playing);
    }
    std::function<void(int)> picked;
    std::function<void(int index, QString* title, QString* detail)> labels;

private:
    QScrollArea* scroll_{nullptr};
    QWidget* host_{nullptr};
    QVBoxLayout* rows_{nullptr};
    std::vector<TrackRow*> rowWidgets_;
    std::vector<std::int64_t> ids_;
};

class OrderPopup final : public PlayerPopup {
public:
    explicit OrderPopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedWidth(220);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(0);
        reverse_ = new MenuRow(tr("Reverse order"), this);
        shuffle_ = new MenuRow(tr("Shuffle"), this);
        reverse_->setIcon(QStringLiteral("player_order"), false);
        shuffle_->setIcon(QStringLiteral("player_shuffle"), false);
        layout->addWidget(reverse_);
        layout->addWidget(shuffle_);
        connect(reverse_, &QAbstractButton::clicked, this, [this] {
            if (picked) {
                picked(1);
            }
        });
        connect(shuffle_, &QAbstractButton::clicked, this, [this] {
            if (picked) {
                picked(2);
            }
        });
    }

    void sync(const int order) {
        reverse_->setMarked(order == 1);
        shuffle_->setMarked(order == 2);
    }

    std::function<void(int)> picked;

private:
    MenuRow* reverse_{nullptr};
    MenuRow* shuffle_{nullptr};
};

class SpeedPopup final : public PlayerPopup {
public:
    explicit SpeedPopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedWidth(204); // 50 + 122 + 12, plus popup margins
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(5, 10, 5, 10);
        layout->setSpacing(0);
        readout_ = new QLabel(this);
        readout_->setForegroundRole(QPalette::WindowText);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(12);
        font.setBold(true);
        readout_->setFont(font);
        slider_ = new PlayerSlider(this);
        slider_->setRange(5, 25);
        auto* sliderRow = new QHBoxLayout;
        sliderRow->setContentsMargins(0, 8, 12, 8);
        sliderRow->setSpacing(0);
        readout_->setFixedWidth(50);
        slider_->setFixedWidth(122);
        sliderRow->addWidget(readout_);
        sliderRow->addWidget(slider_);
        layout->addLayout(sliderRow);
        auto* separator = new QFrame(this);
        separator->setFrameShape(QFrame::HLine);
        separator->setForegroundRole(QPalette::Midlight);
        layout->addWidget(separator);
        debounce_ = new QTimer(this);
        debounce_->setSingleShot(true);
        debounce_->setInterval(1000); // media_player_dropdown.cpp kSpeedDebounceTimeout
        const auto commit = [this] {
            debounce_->stop();
            if (!suppress_ && picked) picked(slider_->value() * 100);
        };
        connect(debounce_, &QTimer::timeout, this, commit);
        connect(slider_, &QSlider::sliderReleased, this, commit);
        connect(slider_, &QSlider::valueChanged, this, [this](const int value) {
            readout_->setText(QString::number(value / 10., 'f', 1) + QLatin1Char('x'));
            for (const auto& row : rows_) {
                const bool chosen = row.milli == value * 100;
                row.widget->setMarked(chosen);
                row.widget->setEnabled(!chosen);
            }
            if (!suppress_) debounce_->start();
        });
        const struct {
            int milli;
            const char* label;
        } points[] = {
            {500, "Slow"},
            {1000, "Normal"},
            {1200, "Medium"},
            {1500, "Fast"},
            {1700, "Very fast"},
            {2000, "Super fast"},
        };
        for (const auto& point : points) {
            auto* row = new MenuRow(tr(point.label), this);
            row->setIcon(QStringLiteral("speed/audiospeed_menu_%1").arg(point.milli / 1000., 0, 'f', 1));
            layout->addWidget(row);
            rows_.push_back({point.milli, row});
            connect(row, &QAbstractButton::clicked, this, [this, milli = point.milli] {
                if (picked) {
                    picked(milli);
                }
                debounce_->stop();
                hide();
            });
        }
    }

    void sync(const int milli) {
        suppress_ = true;
        slider_->setValue(std::clamp(milli / 100, 5, 25));
        readout_->setText(QString::number(milli / 1000., 'f', 1) + QLatin1Char('x'));
        suppress_ = false;
        for (const auto& row : rows_) {
            const bool chosen = std::abs(row.milli - milli) < 40;
            row.widget->setMarked(chosen);
            row.widget->setEnabled(!chosen);
        }
    }

    std::function<void(int)> picked;

private:
    struct Row {
        int milli;
        MenuRow* widget;
    };
    QTimer* debounce_{nullptr};
    QLabel* readout_{nullptr};
    QSlider* slider_{nullptr};
    bool suppress_{false};
    std::vector<Row> rows_;
};

class VolumePopup final : public PlayerPopup {
public:
    explicit VolumePopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedSize(47, 116); // 27x100 content + 10px side/bottom, 6px seek margin
        setMouseTracking(true);
    }

    void sync(const int percent) {
        percent_ = std::clamp(percent, 0, 100);
        update();
    }

    std::function<void(int)> picked;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(playerHairline()));
        painter.setBrush(playerCard());
        painter.drawRoundedRect(QRectF(10, 6, 27, 100), 8, 8);
        const QRect track(width() / 2 - 1, 14, 3, 84);
        painter.setPen(Qt::NoPen);
        painter.setBrush(playerGroove());
        painter.drawRoundedRect(track, 2, 2);
        const int filled = track.height() * percent_ / 100;
        painter.setBrush(playerAccent());
        painter.drawRoundedRect(QRect(track.left(), track.bottom() - filled + 1, track.width(), filled), 2, 2);
        const int handleY = track.bottom() - filled;
        painter.setBrush(playerAccent());
        painter.drawEllipse(QPoint(track.center().x(), handleY), 6, 6);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) { apply(event->pos().y()); event->accept(); }
    }
    void wheelEvent(QWheelEvent* event) override {
        if (event->angleDelta().y()) { applyPercent(percent_ + event->angleDelta().y() / 12); event->accept(); }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            apply(event->pos().y());
        }
    }

private:
    void apply(const int y) {
        const int top = 14;
        const int height = 84;
        const int percent = height <= 0
            ? 0
            : std::clamp(100 - (y - top) * 100 / height, 0, 100);
        applyPercent(percent);
    }
    void applyPercent(int percent) {
        percent_ = std::clamp(percent, 0, 100);
        update();
        if (picked) picked(percent_);
    }

    int percent_{100};
};

SongTags songFromFileName(const QString& fileName) {
    SongTags song;
    const QFileInfo info(fileName);
    QString base = info.completeBaseName().trimmed();
    if (base.isEmpty()) {
        base = fileName.trimmed();
    }
    const QString original = base;
    const QRegularExpression leading(QStringLiteral("^\\s*(?:\\d{1,3}\\s*[.\\-_]\\s+)+"));
    base.remove(leading);
    base = base.trimmed();
    if (base.isEmpty()) {
        base = original;
    }
    const QStringList separators{
        QStringLiteral(" — "), QStringLiteral(" – "), QStringLiteral(" - ")};
    for (const QString& separator : separators) {
        const int at = base.indexOf(separator);
        if (at > 0 && at + separator.size() < base.size()) {
            song.artist = base.left(at).trimmed();
            song.title = base.mid(at + separator.size()).trimmed();
            return song;
        }
    }
    song.title = base;
    return song;
}

QPixmap paintNotePlate(const int side) {
    const int s = std::max(1, side);
    const QPixmap cover(QStringLiteral(":/icons/nocover.png"));
    if (!cover.isNull()) {
        return cover.scaled(s, s, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QPixmap plate(s, s);
    plate.fill(playerCard());
    return plate;
}

QIcon musicTrayIcon() {
    QIcon icon;
    for (const int side : {16, 20, 24, 32, 48}) {
        icon.addPixmap(paintNotePlate(side));
    }
    return icon;
}

QPixmap roundedCover(const QImage& image, const int side) {
    if (image.isNull()) {
        return paintNotePlate(side);
    }
    QPixmap plate(side, side);
    plate.fill(Qt::transparent);
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, side, side), 10, 10);
    painter.setClipPath(clip);
    painter.drawImage(QRect(0, 0, side, side), image);
    return plate;
}

class ClickSlider final : public PlayerSlider {
public:
    explicit ClickSlider(QWidget* parent) : PlayerSlider(parent) {}

    std::function<void()> began;
    std::function<void(int)> jumped;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || width() <= 0) {
            QSlider::mousePressEvent(event);
            return;
        }
        setSliderDown(true);
        jump(event->pos().x());
        if (began) {
            began();
        }
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!isSliderDown() || width() <= 0) {
            QSlider::mouseMoveEvent(event);
            return;
        }
        jump(event->pos().x());
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!isSliderDown() || width() <= 0) {
            QSlider::mouseReleaseEvent(event);
            return;
        }
        jump(event->pos().x());
        setSliderDown(false);
        if (jumped) {
            jumped(value());
        }
        event->accept();
    }

private:
    void jump(const int x) {
        setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x, width()));
    }
};

class NowPlayingCard final : public QWidget {
public:
    explicit NowPlayingCard(QWidget*)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint) {
        setObjectName(QStringLiteral("musicFloatingPlayer"));
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(340);
        auto* root = new QHBoxLayout(this);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(12);
        cover_ = new QLabel(this);
        cover_->setFixedSize(84, 84);
        cover_->setPixmap(roundedCover({}, 84));
        cover_->setAttribute(Qt::WA_TransparentForMouseEvents);
        root->addWidget(cover_, 0, Qt::AlignTop);

        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        title_ = new QLabel(this);
        artist_ = new QLabel(this);
        album_ = new QLabel(this);
        QFont titleFont(QStringLiteral("Segoe UI"));
        titleFont.setPixelSize(15);
        titleFont.setBold(true);
        title_->setFont(titleFont);
        // Foreground roles instead of literal colours: labels without a
        // stylesheet follow the palette, including a live theme switch.
        title_->setForegroundRole(QPalette::WindowText);
        artist_->setForegroundRole(QPalette::PlaceholderText);
        album_->setForegroundRole(QPalette::PlaceholderText);
        QFont small(QStringLiteral("Segoe UI"));
        small.setPixelSize(12);
        artist_->setFont(small);
        album_->setFont(small);
        title_->setWordWrap(false);
        title_->setAttribute(Qt::WA_TransparentForMouseEvents);
        artist_->setAttribute(Qt::WA_TransparentForMouseEvents);
        album_->setAttribute(Qt::WA_TransparentForMouseEvents);
        column->addWidget(title_);
        column->addWidget(artist_);
        column->addWidget(album_);
        album_->setVisible(false);

        timeline_ = new ClickSlider(this);
        timeline_->setRange(0, 1000);
        timeline_->setFocusPolicy(Qt::NoFocus);
        timeline_->setFixedHeight(16);
        column->addSpacing(4);
        column->addWidget(timeline_);

        auto* controls = new QHBoxLayout();
        controls->setSpacing(2);
        previous_ = new GlyphButton(Glyph::Previous, this);
        play_ = new GlyphButton(Glyph::Pause, this);
        next_ = new GlyphButton(Glyph::Next, this);
        play_->setActive(true);
        previous_->setFixedSize(32, 32);
        play_->setFixedSize(36, 32);
        next_->setFixedSize(32, 32);
        previous_->setToolTip(QStringLiteral("Previous"));
        play_->setToolTip(QStringLiteral("Pause"));
        next_->setToolTip(QStringLiteral("Next"));
        volume_ = new PlayerSlider(this);
        volume_->setRange(0, 100);
        controls->addWidget(previous_);
        controls->addWidget(play_);
        controls->addWidget(next_);
        controls->addWidget(volume_, 1);
        column->addSpacing(6);
        column->addLayout(controls);
        root->addLayout(column, 1);

        connect(previous_, &QAbstractButton::clicked, this, [this] { if (onPrevious) onPrevious(); });
        connect(play_, &QAbstractButton::clicked, this, [this] { if (onPlay) onPlay(); });
        connect(next_, &QAbstractButton::clicked, this, [this] { if (onNext) onNext(); });
        connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
            if (!volumeGuard_ && onVolume) {
                onVolume(value);
            }
        });
        connect(timeline_, &QSlider::sliderPressed, this, [this] { timelineScrub_ = true; });
        connect(timeline_, &QSlider::sliderReleased, this, [this] {
            timelineScrub_ = false;
            if (onSeek) {
                onSeek(timeline_->value());
            }
        });

        hideButton_ = new GlyphButton(Glyph::Close, this);
        hideButton_->setFixedSize(22, 22);
        hideButton_->setToolTip(QStringLiteral("Hide"));
        connect(hideButton_, &QAbstractButton::clicked, this, [this] {
            hide();
            if (onClose) {
                onClose();
            }
        });
    }

    void placeHideButton() {
        if (hideButton_ != nullptr) {
            hideButton_->move(width() - hideButton_->width() - 8, 8);
            hideButton_->raise();
        }
    }

    void setTimeline(const int value) {
        if (timelineScrub_ || timeline_ == nullptr || timeline_->value() == value) {
            return;
        }
        timeline_->setValue(value);
    }

    void present(
        const QString& title,
        const QString& artist,
        const QString& album,
        const QImage& cover,
        const bool playing,
        const int volume,
        const bool hasPrevious,
        const bool hasNext) {
        const QFontMetrics titleMetrics(title_->font());
        title_->setText(titleMetrics.elidedText(title.isEmpty() ? QStringLiteral("Nothing to play") : title, Qt::ElideRight, 210));
        artist_->setText(QFontMetrics(artist_->font()).elidedText(artist, Qt::ElideRight, 210));
        artist_->setVisible(!artist.isEmpty());
        album_->setText(QFontMetrics(album_->font()).elidedText(album, Qt::ElideRight, 210));
        album_->setVisible(!album.isEmpty());
        cover_->setPixmap(roundedCover(cover, 84));
        play_->setGlyph(playing ? Glyph::Pause : Glyph::Play);
        play_->setToolTip(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
        previous_->setEnabled(hasPrevious);
        next_->setEnabled(hasNext);
        if (volume_->value() != volume) {
            volumeGuard_ = true;
            volume_->setValue(volume);
            volumeGuard_ = false;
        }
        adjustSize();
        placeHideButton();
    }

    std::function<void()> onPrevious;
    std::function<void()> onPlay;
    std::function<void()> onNext;
    std::function<void(int)> onVolume;
    std::function<void(int)> onSeek;
    std::function<void()> onClose;
    std::function<void()> onMoved;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(playerHairline()));
        painter.setBrush(playerCard());
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 16, 16);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        placeHideButton();
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            dragOffset_ = event->globalPos() - frameGeometry().topLeft();
        }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            move(event->globalPos() - dragOffset_);
        }
        QWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            if (onMoved) {
                onMoved();
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QLabel* cover_{nullptr};
    QLabel* title_{nullptr};
    QLabel* artist_{nullptr};
    QLabel* album_{nullptr};
    GlyphButton* previous_{nullptr};
    GlyphButton* play_{nullptr};
    GlyphButton* next_{nullptr};
    QSlider* volume_{nullptr};
    QSlider* timeline_{nullptr};
    GlyphButton* hideButton_{nullptr};
    bool volumeGuard_{false};
    bool timelineScrub_{false};
    bool dragging_{false};
    QPoint dragOffset_{};
};

struct MusicWindow::SystemMediaSession {
#ifdef Q_OS_WIN
    winrt::Windows::Media::SystemMediaTransportControls controls{nullptr};
    winrt::event_token buttonToken{};
    winrt::event_token seekToken{};
    bool seekHooked{false};
    bool enabled{false};
    bool playing{false};
    bool hasNext{false};
    bool hasPrevious{false};
    QString title;
    QString artist;
    QString album;
    std::int64_t coverToken{0};
    std::int64_t timelinePosition{-1};
#endif
};

#ifdef Q_OS_WIN
namespace {

winrt::hstring mediaText(const QString& text) {
    return winrt::hstring(text.toStdWString());
}

void writeCover(
    const winrt::Windows::Media::SystemMediaTransportControlsDisplayUpdater& updater,
    const QImage& cover) {
    if (cover.isNull()) {
        updater.Thumbnail(nullptr);
        return;
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    const QImage plate = cover.scaled(300, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!plate.save(&buffer, "JPEG", 82) || encoded.isEmpty()) {
        return;
    }
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
    {
        winrt::Windows::Storage::Streams::DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(encoded.constData()),
            static_cast<std::uint32_t>(encoded.size())));
        writer.StoreAsync().get();
        writer.DetachStream();
    }
    stream.Seek(0);
    updater.Thumbnail(
        winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromStream(stream));
}

} // namespace
#endif

void MusicWindow::syncSystemMedia(const bool metadata) {
#ifdef Q_OS_WIN
    if (systemMedia_ == nullptr) {
        systemMedia_ = std::make_unique<SystemMediaSession>();
    }
    auto& session = *systemMedia_;
    try {
        if (session.controls == nullptr) {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (const winrt::hresult_error& error) {
                if (error.code() != static_cast<winrt::hresult>(0x80010106)) {
                    throw;
                }
            }
            QWidget* host = window();
            if (host == nullptr) {
                return;
            }
            const auto hwnd = reinterpret_cast<HWND>(host->winId());
            if (hwnd == nullptr) {
                return;
            }
            auto factory = winrt::get_activation_factory<winrt::Windows::Media::SystemMediaTransportControls>();
            auto interop = factory.as<ISystemMediaTransportControlsInterop>();
            void* raw = nullptr;
            winrt::check_hresult(interop->GetForWindow(
                hwnd,
                winrt::guid_of<winrt::Windows::Media::ISystemMediaTransportControls>(),
                &raw));
            session.controls = winrt::Windows::Media::SystemMediaTransportControls{
                raw, winrt::take_ownership_from_abi};
            QPointer<MusicWindow> self(this);
            session.buttonToken = session.controls.ButtonPressed(
                [self](const auto&, const winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs& args) {
                    if (self.isNull()) {
                        return;
                    }
                    const auto button = args.Button();
                    QMetaObject::invokeMethod(self.data(), [self, button] {
                        if (self.isNull() || self->tracks_.empty()) {
                            return;
                        }
                        using Button = winrt::Windows::Media::SystemMediaTransportControlsButton;
                        if (button == Button::Play) {
                            if (self->paused_.load() || self->ended_.load()) {
                                self->togglePlay();
                            }
                        } else if (button == Button::Pause) {
                            self->setPaused(true);
                        } else if (button == Button::Stop) {
                            self->stopPlayback();
                        } else if (button == Button::Next) {
                            self->advance(1);
                        } else if (button == Button::Previous) {
                            self->advance(-1);
                        } else if (button == Button::FastForward) {
                            self->seekBy(10000);
                        } else if (button == Button::Rewind) {
                            self->seekBy(-10000);
                        }
                    }, Qt::QueuedConnection);
                });
            if (auto extended = session.controls.try_as<winrt::Windows::Media::ISystemMediaTransportControls2>()) {
                session.seekToken = extended.PlaybackPositionChangeRequested(
                    [self](const auto&, const winrt::Windows::Media::PlaybackPositionChangeRequestedEventArgs& args) {
                        if (self.isNull()) {
                            return;
                        }
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            args.RequestedPlaybackPosition()).count();
                        QMetaObject::invokeMethod(self.data(), [self, ms] {
                            if (!self.isNull() && !self->tracks_.empty()) {
                                self->seekTo(ms);
                            }
                        }, Qt::QueuedConnection);
                    });
                session.seekHooked = true;
            }
        }

        const bool active = !tracks_.empty() && errorText_.isEmpty();
        const bool playing = active && !paused_.load() && !terminalEnded_;
        const std::int64_t id = tracks_.empty() ? 0 : tracks_[static_cast<std::size_t>(index_)].id;
        const bool haveCover = coverId_ == id && !cover_.isNull();
        const std::int64_t coverToken = haveCover ? id : 0;
        (void)metadata;
        const bool changed = session.title != songTitle_
            || session.artist != artistText_
            || session.album != albumText_
            || session.playing != playing
            || session.enabled != active
            || session.hasNext != nextAvailable()
            || session.hasPrevious != previousAvailable()
            || session.coverToken != coverToken;
        if (changed) {
            session.title = songTitle_;
            session.artist = artistText_;
            session.album = albumText_;
            session.playing = playing;
            session.enabled = active;
            session.hasNext = nextAvailable();
            session.hasPrevious = previousAvailable();
            session.coverToken = coverToken;
            auto& controls = session.controls;
            controls.IsEnabled(active);
            controls.IsPlayEnabled(active);
            controls.IsPauseEnabled(active);
            controls.IsNextEnabled(active && nextAvailable());
            controls.IsPreviousEnabled(active && previousAvailable());
            controls.IsFastForwardEnabled(active);
            controls.IsRewindEnabled(active);
            controls.PlaybackStatus(playing
                ? winrt::Windows::Media::MediaPlaybackStatus::Playing
                : active
                    ? winrt::Windows::Media::MediaPlaybackStatus::Paused
                    : winrt::Windows::Media::MediaPlaybackStatus::Closed);
            auto updater = controls.DisplayUpdater();
            updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
            auto music = updater.MusicProperties();
            const QString title = songTitle_.isEmpty() ? QStringLiteral("Mega Vault Protect") : songTitle_;
            music.Title(mediaText(title));
            music.Artist(mediaText(artistText_));
            music.AlbumTitle(mediaText(albumText_));
            if (haveCover) {
                writeCover(updater, cover_);
            } else if (metadata) {
                updater.Thumbnail(nullptr);
            }
            updater.Update();
            session.timelinePosition = -1;
        }
        if (auto extended = session.controls.try_as<winrt::Windows::Media::ISystemMediaTransportControls2>()) {
            const auto duration = std::max<std::int64_t>(0, durationMs_.load());
            const auto position = std::min<std::int64_t>(duration, std::max<std::int64_t>(0, positionMs()));
            if (session.timelinePosition < 0
                || std::llabs(position - session.timelinePosition) >= 900) {
                session.timelinePosition = position;
                winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties timeline;
                const auto end = std::chrono::milliseconds(std::max<std::int64_t>(1, duration));
                timeline.StartTime(std::chrono::milliseconds(0));
                timeline.MinSeekTime(std::chrono::milliseconds(0));
                timeline.EndTime(end);
                timeline.MaxSeekTime(end);
                timeline.Position(std::chrono::milliseconds(position));
                extended.UpdateTimelineProperties(timeline);
            }
        }
    } catch (const winrt::hresult_error&) {
        if (systemMedia_ != nullptr) {
            systemMedia_->controls = nullptr;
        }
    }
#else
    (void)metadata;
#endif
}

void MusicWindow::clearSystemMedia() {
#ifdef Q_OS_WIN
    if (systemMedia_ == nullptr || systemMedia_->controls == nullptr) {
        return;
    }
    auto& session = *systemMedia_;
    try {
        session.controls.ButtonPressed(session.buttonToken);
        if (session.seekHooked) {
            if (auto extended = session.controls.try_as<winrt::Windows::Media::ISystemMediaTransportControls2>()) {
                extended.PlaybackPositionChangeRequested(session.seekToken);
            }
        }
        session.controls.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Closed);
        session.controls.IsEnabled(false);
        session.controls.DisplayUpdater().ClearAll();
    } catch (const winrt::hresult_error&) {
    }
    session.controls = nullptr;
    session.seekHooked = false;
    session.enabled = false;
    session.timelinePosition = -1;
#endif
}

namespace {
// One slot for the process, shared by shared() and instance(): asking for the
// instance must never construct a player that nothing asked for.
QPointer<MusicWindow>& musicWindowSlot() {
    // QObject ownership tears the bar down with the main shell. A guarded slot
    // cannot hand a later window a dangling instance after that destruction.
    static QPointer<MusicWindow> slot;
    return slot;
}
} // namespace

MusicWindow* MusicWindow::shared(QWidget* owner) {
    // The first caller determines the MainWindow owner. Pages only borrow the
    // singleton footer; navigation must never reparent it.
    auto& slot = musicWindowSlot();
    if (slot == nullptr) {
        slot = new MusicWindow(owner != nullptr ? owner->window() : nullptr);
        slot->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    return slot;
}

MusicWindow* MusicWindow::instance() {
    return musicWindowSlot();
}

MusicWindow::MusicWindow(QWidget* parent)
    : QWidget(parent)
    , stretch_(std::make_unique<TimeStretch>()) {
    setObjectName(QStringLiteral("musicBar"));
    setFixedHeight(kBarHeight);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    QFont textFont(QStringLiteral("Segoe UI"));
    textFont.setPixelSize(13);

    previousButton_ = new GlyphButton(Glyph::Previous, this);
    playButton_ = new GlyphButton(Glyph::Pause, this);
    nextButton_ = new GlyphButton(Glyph::Next, this);
    volumeButton_ = new GlyphButton(Glyph::VolumeFull, this);
    orderButton_ = new GlyphButton(Glyph::Reverse, this);
    repeatButton_ = new GlyphButton(Glyph::Repeat, this);
    speedButton_ = new GlyphButton(Glyph::Close, this);
    speedButton_->setLabel(QStringLiteral("1X"));
    closeButton_ = new GlyphButton(Glyph::Close, this);
    previousButton_->setObjectName(QStringLiteral("musicPrevious"));
    playButton_->setObjectName(QStringLiteral("musicPlayPause"));
    nextButton_->setObjectName(QStringLiteral("musicNext"));
    volumeButton_->setObjectName(QStringLiteral("musicVolume"));
    orderButton_->setObjectName(QStringLiteral("musicOrder"));
    repeatButton_->setObjectName(QStringLiteral("musicRepeat"));
    speedButton_->setObjectName(QStringLiteral("musicSpeed"));
    closeButton_->setObjectName(QStringLiteral("musicClose"));
    playButton_->setActive(true);
    volumeButton_->setActive(true);
    previousButton_->setToolTip(QStringLiteral("Previous"));
    playButton_->setToolTip(QStringLiteral("Pause"));
    nextButton_->setToolTip(QStringLiteral("Next"));
    volumeButton_->setToolTip(QStringLiteral("Volume"));
    orderButton_->setToolTip(QStringLiteral("Playback order"));
    repeatButton_->setToolTip(QStringLiteral("Repeat"));
    speedButton_->setToolTip(QStringLiteral("Playback speed"));
    closeButton_->setToolTip(QStringLiteral("Close"));
    for (auto* button : {previousButton_, playButton_, nextButton_, volumeButton_, orderButton_, repeatButton_, speedButton_, closeButton_})
        button->setAccessibleName(button->toolTip());

    nameLabel_ = new QLabel(this);
    nameLabel_->setObjectName(QStringLiteral("musicSongLabel"));
    nameLabel_->setAccessibleName(tr("Current song. Click to open the floating player."));
    nameLabel_->setTextFormat(Qt::RichText);
    timeLabel_ = new QLabel(QStringLiteral("0:00"), this);
    timeLabel_->setObjectName(QStringLiteral("musicTime"));
    nameLabel_->setFont(textFont);
    timeLabel_->setFont(textFont);
    nameLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    timeLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    nameLabel_->setForegroundRole(QPalette::WindowText);
    timeLabel_->setForegroundRole(QPalette::PlaceholderText);
    nameLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    progress_ = new ProgressLine(this);
    playlist_ = new PlaylistPopup(this);
    orderMenu_ = new OrderPopup(this);
    speedMenu_ = new SpeedPopup(this);
    volumeMenu_ = new VolumePopup(this);
    playlist_->setObjectName(QStringLiteral("musicHoverPlaylist"));
    orderMenu_->setObjectName(QStringLiteral("musicOrderMenu"));
    speedMenu_->setObjectName(QStringLiteral("musicSpeedMenu"));
    volumeMenu_->setObjectName(QStringLiteral("musicVolumeMenu"));

    previousButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    nextButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    playButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    closeButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    repeatButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    volumeButton_->entered = [this] { showOnly(volumeMenu_, volumeButton_, true); };
    orderButton_->entered = [this] { showOnly(orderMenu_, orderButton_, true); };
    speedButton_->entered = [this] { showOnly(speedMenu_, speedButton_, true); };
    for (auto* button : {previousButton_, playButton_, nextButton_, volumeButton_, orderButton_, repeatButton_, speedButton_, closeButton_}) {
        button->left = [this] { armHide(); };
    }

    connect(previousButton_, &QAbstractButton::clicked, this, [this] { advance(-1); });
    connect(nextButton_, &QAbstractButton::clicked, this, [this] { advance(1); });
    connect(playButton_, &QAbstractButton::clicked, this, [this] { togglePlay(); });
    connect(closeButton_, &QAbstractButton::clicked, this, &QWidget::close);
    connect(repeatButton_, &QAbstractButton::clicked, this, [this] {
        invalidateNextTrack();
        adoptPendingTrack(false);
        repeat_ = (repeat_ + 1) % 3;
        prepareNextTrack();
        syncTransport();
        savePlayback();
    });
    connect(volumeButton_, &QAbstractButton::clicked, this, [this] { toggleMute(); });
    connect(speedButton_, &QAbstractButton::clicked, this, [this] { toggleSpeed(); });
    connect(orderButton_, &QAbstractButton::clicked, this, [this] { showOnly(orderMenu_, orderButton_, true); });

    playlist_->picked = [this](const int index) {
        if (index == index_) togglePlay();
        else playIndex(index);
    };
    playlist_->labels = [this](const int index, QString* title, QString* detail) {
        const SongTags song = songFor(index);
        if (title != nullptr) {
            *title = song.title;
        }
        if (detail == nullptr) {
            return;
        }
        if (!song.artist.isEmpty()) {
            *detail = song.artist;
        } else if (!song.album.isEmpty()) {
            *detail = song.album;
        } else if (index >= 0 && index < static_cast<int>(tracks_.size())) {
            const QFileInfo info(tracks_[static_cast<std::size_t>(index)].title);
            *detail = info.suffix().isEmpty() ? QStringLiteral("Audio") : info.suffix().toUpper();
        }
    };
    orderMenu_->picked = [this](const int mode) { setOrder(order_ == mode ? 0 : mode); orderMenu_->hide(); };
    speedMenu_->picked = [this](const int milli) { setSpeedMilli(milli); };
    volumeMenu_->picked = [this](const int percent) { setVolumePercent(percent); };
    progress_->hoverText = [this](const int value) {
        const auto duration = durationMs_.load();
        return duration > 0 ? formatTime(duration * value / 1000) : QString();
    };
    progress_->moved = [this](const int value) {
        scrubbing_ = true;
        const auto duration = durationMs_.load();
        timeLabel_->setText(formatTime(duration > 0 ? duration * value / 1000 : 0));
    };
    progress_->released = [this](const int value) {
        scrubbing_ = false;
        pendingSeek_ = value;
        const auto duration = durationMs_.load();
        if (duration > 0) {
            seekTo(duration * value / 1000);
            pendingSeek_ = -1;
        }
    };

    sink_ = new AudioSink(this);
    clock_ = new QTimer(this);
    clock_->setInterval(200);
    connect(clock_, &QTimer::timeout, this, [this] { refreshClock(); });
    clock_->start();
    playlistShowTimer_ = new QTimer(this);
    playlistShowTimer_->setSingleShot(true);
    playlistShowTimer_->setInterval(300); // Panel::showFromOther
    connect(playlistShowTimer_, &QTimer::timeout, this, [this] {
        if (nameRect_.contains(mapFromGlobal(QCursor::pos()))) showOnly(playlist_, nameLabel_, false);
    });
    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    hideTimer_->setInterval(300);
    connect(hideTimer_, &QTimer::timeout, this, [this] {
        if (!cursorOnChrome()) {
            hidePopups();
        }
    });
    qApp->installEventFilter(this);

    tray_ = new QSystemTrayIcon(musicTrayIcon(), this);
    tray_->setToolTip(QStringLiteral("Mega Vault Protect"));
    trayCard_ = new NowPlayingCard(this);
    trayCard_->onPrevious = [this] { advance(-1); };
    trayCard_->onPlay = [this] { togglePlay(); };
    trayCard_->onNext = [this] { advance(1); };
    trayCard_->onVolume = [this](const int percent) { setVolumePercent(percent); };
    trayCard_->onSeek = [this](const int value) {
        const auto duration = durationMs_.load();
        if (duration > 0) {
            seekTo(duration * value / 1000);
        }
    };
    trayCard_->onClose = [this] { trayDismissedAt_ = QDateTime::currentMSecsSinceEpoch(); };
    trayCard_->onMoved = [this] {
        if (trayCard_ == nullptr) {
            return;
        }
        QSettings settings;
        settings.setValue(QStringLiteral("player/popupX"), trayCard_->x());
        settings.setValue(QStringLiteral("player/popupY"), trayCard_->y());
        popupPlaced_ = true;
    };
    connect(tray_, &QSystemTrayIcon::activated, this, [this](const QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger
            && reason != QSystemTrayIcon::DoubleClick
            && reason != QSystemTrayIcon::Context) {
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() - trayDismissedAt_ < 350) {
            return;
        }
        if (trayCard_ != nullptr && trayCard_->isVisible()) {
            trayCard_->hide();
            return;
        }
        showTrayCard();
    });
    loadPlayback();
}

MusicWindow::~MusicWindow() {
    clearSystemMedia();
    qApp->removeEventFilter(this);
    hidePopups();
    coverSerial_.fetch_add(1);
    if (trayCard_ != nullptr) {
        trayCard_->hide();
        delete trayCard_;
        trayCard_ = nullptr;
    }
    while (noteProbe_ && noteProbe_->inflight.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stopWorker();
}

void MusicWindow::playQueue(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::vector<MusicTrack>& tracks,
    const int index) {
    stopWorker();
    coverSerial_.fetch_add(1);
    if (vault_ != vault) {
        art_.clear();
        tags_.clear();
        cover_ = QImage();
        coverId_ = 0;
        noteProbe_ = std::make_shared<NoteProbe>();
    }
    vault_ = vault;
    tracks_ = tracks;
    index_ = tracks_.empty() ? 0 : std::max(0, std::min(index, static_cast<int>(tracks_.size()) - 1));
    history_.clear();
    refillBag();
    showTrack();
    startWorker();
    if (tray_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_->show();
    }
    updateTray();
}

void MusicWindow::enqueue(const std::vector<MusicTrack>& tracks, const bool playNext) {
    if (tracks.empty()) {
        return;
    }
    if (tracks_.empty()) {
        // A fresh bar has no vault context. Its owner must use playQueue(vault,
        // tracks, 0); never display a fictitious playing queue with a null vault.
        if (vault_ && vault_->is_unlocked()) playQueue(vault_, tracks, 0);
        return;
    }
    invalidateNextTrack();
    adoptPendingTrack(false);
    // Insert in playback order, not merely visual order: reverse mode needs the
    // batch before the current row (reversed), and shuffle must put it atop the
    // bag. The current decoder and its position are never restarted by enqueue.
    const int position = MusicQueueEdit::insert(tracks_, index_, tracks, playNext, order_);
    history_.clear();
    bag_.clear();
    if (order_ == 1 || order_ == 2) {
        refillBag();
    }
    if (playNext && order_ == 2)
        MusicQueueEdit::prioritize(bag_, position, static_cast<int>(tracks.size()));
    prepareNextTrack();
    applySong();
    syncTransport();
}

void MusicWindow::showTrack() {
    errorText_.clear();
    terminalEnded_ = false;
    ended_.store(false);
    if (tracks_.empty() || coverId_ != tracks_[static_cast<std::size_t>(index_)].id) {
        cover_ = QImage();
        coverId_ = 0;
    }
    paused_.store(false);
    playButton_->setGlyph(Glyph::Pause);
    playButton_->setToolTip(QStringLiteral("Pause"));
    timeLabel_->setText(QStringLiteral("0:00"));
    progress_->setValue(0);
    applySong();
    syncTransport();
}

void MusicWindow::startWorker() {
    stopWorker();
    terminalEnded_ = false;
    if (!vault_ || !vault_->is_unlocked() || tracks_.empty()) {
        paused_.store(true);
        syncTransport();
        return;
    }
    stop_.store(false);
    workerFinished_.store(false);
    ended_.store(false);
    paused_.store(false);
    seekMs_.store(-1);
    pendingSeek_ = -1;
    scrubbing_ = false;
    originMs_.store(0);
    consumedFrames_.store(0);
    durationMs_.store(0);
    sampleRate_.store(LocalAudioReader::rate);
    channels_.store(LocalAudioReader::channels);
    {
        std::lock_guard<std::mutex> guard(errorMutex_);
        error_.clear();
    }
    stretch_->reset();
    sink_->clear();
    // Manual jumps discard both the software and hardware queues. Automatic
    // transitions below deliberately do neither, and keep this output alive.
    if (output_) { output_->stop(); output_->start(sink_); }
    prefetch_ = std::make_unique<LocalAudioPrefetch>();
    prepareNextTrack();
    const auto vault = vault_;
    const auto generation = vault->lock_generation();
    const auto playbackSerial = playbackSerial_.load();
    const auto id = tracks_[static_cast<std::size_t>(index_)].id;
    const auto hint = QByteArray("audio.") + QFileInfo(tracks_[static_cast<std::size_t>(index_)].title).suffix().toLower().toUtf8();
    worker_ = std::thread([this, vault, generation, id, hint, playbackSerial] {
        auto reader = std::make_unique<LocalAudioReader>();
        QString error;
        if (!reader->open(vault, id, hint, &error)) {
            std::lock_guard<std::mutex> guard(errorMutex_);
            error_ = error.isEmpty() ? "This file has no playable audio." : error.toStdString();
            workerFinished_.store(true);
            return;
        }
        durationMs_.store(reader->duration());
        auto tags = reader->tags();
        auto cover = reader->cover();
        QMetaObject::invokeMethod(this, [this, id, tags, cover, playbackSerial, vault, generation] {
            if (playbackSerial_.load() == playbackSerial && vault->is_unlocked() && vault->lock_generation() == generation)
                publishTrackInfo(id, tags.title, tags.artist, tags.album, cover);
        }, Qt::QueuedConnection);
        std::vector<std::int16_t> samples;
        bool flushed = false;
        bool eof = false;
        while (!stop_.load() && vault->is_unlocked() && vault->lock_generation() == generation) {
            const auto seek = seekMs_.exchange(-1);
            if (seek >= 0) {
                if (!reader->seek(seek)) {
                    std::lock_guard<std::mutex> guard(errorMutex_);
                    error_ = "Could not seek this audio track.";
                    break;
                }
                consumedFrames_.store(0);
                originMs_.store(seek);
                stretch_->reset();
                flushed = false;
                eof = false;
                const int ticket = clearRequest_.fetch_add(1) + 1;
                while (!stop_.load() && clearApplied_.load() != ticket)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (paused_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // Await the UI's queue-index adoption, not an EOF timer. It happens
            // with the next track's first PCM already behind the previous tail.
            {
                std::lock_guard<std::mutex> guard(chainMutex_);
                if (pendingTrack_.index >= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
            }
            const auto buffered = sink_->buffered_samples();
            if (buffered > static_cast<std::size_t>(LocalAudioReader::rate * LocalAudioReader::channels / 2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const double speed = std::max(0.5, speedMilli_.load() / 1000.0);
            if (!eof) eof = !reader->read(&samples);
            if (!eof) {
                std::size_t consumed = 0;
                auto shaped = stretch_->process(samples.data(), samples.size(), LocalAudioReader::channels, speed, &consumed);
                consumedFrames_.fetch_add(static_cast<std::int64_t>(consumed));
                if (!shaped.empty()) sink_->append(shaped.data(), shaped.size());
                continue;
            }
            NextTrack next;
            { std::lock_guard<std::mutex> guard(chainMutex_); next = nextTrack_; }
            if (next.updating) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!flushed && next.index < 0) {
                std::size_t consumed = 0;
                auto tail = stretch_->flush(LocalAudioReader::channels, speed, &consumed);
                consumedFrames_.fetch_add(static_cast<std::int64_t>(consumed));
                if (!tail.empty()) sink_->append(tail.data(), tail.size());
                flushed = true;
            }
            if (next.index >= 0) {
                auto prepared = prefetch_->take(next.ticket);
                if (!prepared) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                std::lock_guard<std::mutex> guard(chainMutex_);
                if (nextTrack_.ticket != next.ticket) continue;
                if (!prepared->reader) {
                    std::lock_guard<std::mutex> errorGuard(errorMutex_);
                    error_ = prepared->error.toStdString();
                    break;
                }
                reader = std::move(prepared->reader);
                tags = reader->tags();
                cover = reader->cover();
                std::size_t consumed = 0;
                auto first = stretch_->process(prepared->samples.data(), prepared->samples.size(), LocalAudioReader::channels, speed, &consumed);
                // positionMs subtracts the entire buffered tail, staying at zero
                // until this track is heard. No device reset at the boundary.
                originMs_.store(0);
                consumedFrames_.store(static_cast<std::int64_t>(consumed));
                if (!first.empty()) sink_->append(first.data(), first.size());
                pendingTrack_ = {next.index, next.id, reader->duration(), tags.title, tags.artist, tags.album, cover};
                nextTrack_.index = -1;
                QMetaObject::invokeMethod(this, [this, playbackSerial] {
                    if (playbackSerial_.load() == playbackSerial) adoptPendingTrack();
                }, Qt::QueuedConnection);
                eof = false;
                flushed = false;
                continue;
            }
            if (sink_->buffered_samples() == 0U) {
                ended_.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        workerFinished_.store(true);
    });
}

void MusicWindow::stopWorker() {
    playbackSerial_.fetch_add(1);
    stop_.store(true);
    // No processEvents here: re-entering playback while joining the same worker
    // used to mutate tracks_/vault_ beneath a newly started decode thread.
    if (worker_.joinable()) worker_.join();
    prefetch_.reset(); // joins preparation before releasing plaintext/session
    {
        std::lock_guard<std::mutex> guard(chainMutex_);
        nextTrack_ = {};
        pendingTrack_ = {};
    }
    workerFinished_.store(true);
    stop_.store(false);
}

void MusicWindow::prepareNextTrack() {
    invalidateNextTrack();
    if (!adoptingTrack_) adoptPendingTrack(false);
    NextTrack next;
    next.ticket = ++nextTicket_;
    const int count = static_cast<int>(tracks_.size());
    if (count > 0) {
        if (repeat_ == 1) next.index = index_;
        else if (order_ == 2) {
            if (bag_.empty() && repeat_ == 2) refillBag();
            if (!bag_.empty()) next.index = bag_.back();
            else if (repeat_ == 2) next.index = index_;
        } else {
            const int candidate = index_ + (order_ == 1 ? -1 : 1);
            if (candidate >= 0 && candidate < count) next.index = candidate;
            else if (repeat_ == 2) next.index = candidate < 0 ? count - 1 : 0;
        }
    }
    if (next.index >= 0) {
        const auto& track = tracks_[static_cast<std::size_t>(next.index)];
        next.id = track.id;
        next.hint = QByteArray("audio.") + QFileInfo(track.title).suffix().toLower().toUtf8();
    }
    { std::lock_guard<std::mutex> guard(chainMutex_); nextTrack_ = next; }
    if (prefetch_) {
        if (next.index >= 0) prefetch_->request(vault_, next.id, next.hint, next.ticket);
        else prefetch_->cancel();
    }
}

void MusicWindow::invalidateNextTrack() {
    std::lock_guard<std::mutex> guard(chainMutex_);
    nextTrack_.index = -1;
    nextTrack_.updating = true;
    nextTrack_.ticket = ++nextTicket_;
}

void MusicWindow::adoptPendingTrack(bool prepare) {
    PendingTrack pending;
    {
        std::lock_guard<std::mutex> guard(chainMutex_);
        if (pendingTrack_.index < 0) return;
        pending = std::move(pendingTrack_);
        pendingTrack_ = {};
    }
    if (pending.index >= static_cast<int>(tracks_.size())
        || tracks_[static_cast<std::size_t>(pending.index)].id != pending.id) return;
    adoptingTrack_ = true;
    if (order_ == 2 && pending.index != index_) {
        history_.push_back(index_);
        if (!bag_.empty() && bag_.back() == pending.index) bag_.pop_back();
    }
    index_ = pending.index;
    durationMs_.store(pending.duration);
    cover_ = QImage();
    coverId_ = 0;
    errorText_.clear();
    if (prepare) prepareNextTrack();
    publishTrackInfo(pending.id, pending.title, pending.artist, pending.album, pending.cover);
    syncTransport();
    adoptingTrack_ = false;
}

bool MusicWindow::nextAvailable() const {
    const int count = static_cast<int>(tracks_.size());
    if (count <= 0) {
        return false;
    }
    if (count == 1) {
        return repeat_ == 2;
    }
    if (order_ == 2) {
        return !bag_.empty() || repeat_ == 2;
    }
    const int step = order_ == 1 ? -1 : 1;
    const int next = index_ + step;
    return (next >= 0 && next < count) || repeat_ == 2;
}

bool MusicWindow::previousAvailable() const {
    const int count = static_cast<int>(tracks_.size());
    if (count <= 0) {
        return false;
    }
    if (order_ == 2) {
        return !history_.empty();
    }
    if (count == 1) {
        return repeat_ == 2;
    }
    const int step = order_ == 1 ? 1 : -1;
    const int previous = index_ + step;
    return (previous >= 0 && previous < count) || repeat_ == 2;
}

void MusicWindow::refillBag() {
    bag_.clear();
    const int count = static_cast<int>(tracks_.size());
    std::vector<int> ids;
    ids.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (i != index_) {
            ids.push_back(i);
        }
    }
    std::mt19937 rng{std::random_device{}()};
    std::shuffle(ids.begin(), ids.end(), rng);
    bag_ = std::move(ids);
}

bool MusicWindow::advance(const int delta) {
    if (tracks_.empty() || delta == 0) {
        return false;
    }
    invalidateNextTrack();
    adoptPendingTrack(false);
    const int count = static_cast<int>(tracks_.size());
    int next = index_;
    if (order_ == 2) {
        if (delta > 0) {
            if (bag_.empty()) {
                if (repeat_ != 2) {
                    prepareNextTrack();
                    return false;
                }
                refillBag();
                if (bag_.empty()) {
                    next = index_;
                }
            }
            if (!bag_.empty()) {
                history_.push_back(index_);
                next = bag_.back();
                bag_.pop_back();
            }
        } else {
            if (history_.empty()) {
                prepareNextTrack();
                return false;
            }
            bag_.push_back(index_);
            next = history_.back();
            history_.pop_back();
        }
    } else {
        const int step = (order_ == 1 ? -1 : 1) * delta;
        next = index_ + step;
        if (next < 0 || next >= count) {
            if (repeat_ != 2) {
                prepareNextTrack();
                return false;
            }
            next = next < 0 ? count - 1 : 0;
        }
    }
    index_ = next;
    showTrack();
    startWorker();
    return true;
}

void MusicWindow::playIndex(const int index) {
    if (index < 0 || index >= static_cast<int>(tracks_.size())) return;
    invalidateNextTrack();
    adoptPendingTrack(false);
    if (index == index_ && !workerFinished_.load()) {
        setPaused(false);
        prepareNextTrack();
        return;
    }
    history_.clear();
    index_ = index;
    refillBag();
    showTrack();
    startWorker();
}

void MusicWindow::pauseForVideo() {
    if (!tracks_.empty()) setPaused(true);
}

void MusicWindow::showWhileMinimized() {
    if (tracks_.empty()) {
        return;
    }
    showTrayCard();
    if (output_ != nullptr && !paused_.load() && output_->state() == QAudio::SuspendedState) {
        output_->resume();
    }
}

void MusicWindow::togglePlay() {
    if (tracks_.empty()) {
        return;
    }
    if (workerFinished_.load()) {
        showTrack();
        startWorker();
        return;
    }
    setPaused(!paused_.load());
}

void MusicWindow::setPaused(const bool pause) {
    paused_.store(pause);
    playButton_->setGlyph(pause ? Glyph::Play : Glyph::Pause);
    playButton_->setToolTip(pause ? QStringLiteral("Play") : QStringLiteral("Pause"));
    updateTray();
    if (output_ != nullptr) {
        if (pause) {
            output_->suspend();
        } else {
            output_->resume();
        }
    }
}

void MusicWindow::stopPlayback() {
    if (tracks_.empty()) return;
    invalidateNextTrack();
    adoptPendingTrack(false);
    stopWorker();
    if (output_) output_->stop();
    sink_->clear();
    consumedFrames_.store(0);
    originMs_.store(0);
    terminalEnded_ = true;
    ended_.store(true);
    setPaused(true);
    progress_->setValue(0);
    timeLabel_->setText(formatTime(0));
}

void MusicWindow::seekTo(const std::int64_t ms) {
    if (tracks_.empty()) return;
    invalidateNextTrack();
    adoptPendingTrack(false);
    const bool wasPaused = paused_.load();
    if (workerFinished_.load()) {
        startWorker();
        setPaused(wasPaused);
    }
    seekMs_.store(std::max<std::int64_t>(0, ms));
    ended_.store(false);
    terminalEnded_ = false;
    prepareNextTrack();
}

void MusicWindow::requestCovers() {
    if (!vault_ || tracks_.empty() || !noteProbe_) {
        return;
    }
    const int serial = coverSerial_.fetch_add(1) + 1;
    const auto vault = vault_;
    const auto generation = vault->lock_generation();
    const auto probe = noteProbe_;
    std::vector<std::int64_t> ids;
    for (const auto& track : tracks_) {
        if (art_.find(track.id) == art_.end()) {
            ids.push_back(track.id);
        }
    }
    if (ids.empty()) {
        return;
    }
    probe->inflight.fetch_add(1);
    QPointer<MusicWindow> self(this);
    QtConcurrent::run([self, serial, vault, generation, probe, ids] {
        struct Leave {
            std::atomic<int>* inflight;
            ~Leave() { inflight->fetch_sub(1); }
        } leave{&probe->inflight};
        for (const auto id : ids) {
            if (self.isNull() || self->coverSerial_.load() != serial || !vault->is_unlocked()
                || vault->lock_generation() != generation) return;
            if (self.isNull()) {
                return;
            }
            auto stored = vault->display_thumbnail(id, 128U);
            const bool note = stored && (stored.value().mime == "image/x-mvp-note"
                || stored.value().mime == "image/x-mvp-empty");
            bool retry = !stored || stored.value().bytes.empty();
            if (note) {
                std::lock_guard<std::mutex> guard(probe->mutex);
                retry = probe->tried.insert(id).second;
            }
            if (retry && (!stored || note || stored.value().bytes.empty())) {
                auto refreshed = vault->generate_thumbnail(id, 128U);
                if (refreshed) {
                    stored = std::move(refreshed);
                }
            }
            if (self.isNull() || !stored || stored.value().bytes.empty()
                || stored.value().mime == "image/x-mvp-note"
                || stored.value().mime == "image/x-mvp-empty") {
                continue;
            }
            QImage image;
            image.loadFromData(
                reinterpret_cast<const uchar*>(stored.value().bytes.data()),
                static_cast<int>(stored.value().bytes.size()));
            if (image.isNull() || self.isNull()) {
                continue;
            }
            image = image.scaled(80, 80, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
            QMetaObject::invokeMethod(self.data(), [self, serial, id, image, vault, generation] {
                if (self.isNull() || self->coverSerial_.load() != serial || !vault->is_unlocked()
                    || vault->lock_generation() != generation) {
                    return;
                }
                self->art_[id] = image;
                if (!self->tracks_.empty() && self->tracks_[static_cast<std::size_t>(self->index_)].id == id) {
                    self->cover_ = image;
                    self->coverId_ = id;
                    self->updateTray();
                }
                if (self->playlist_ != nullptr) {
                    self->playlist_->setArt(id, QPixmap::fromImage(image));
                }
            }, Qt::QueuedConnection);
        }
    });
}

void MusicWindow::seekBy(const std::int64_t deltaMs) {
    const auto duration = durationMs_.load();
    auto target = positionMs() + deltaMs;
    if (duration > 0) {
        target = std::min(target, duration);
    }
    seekTo(std::max<std::int64_t>(0, target));
}

void MusicWindow::ensureOutput() {
    const int rate = sampleRate_.load();
    const int channels = std::max(1, channels_.load());
    if (rate <= 0) {
        return;
    }
    if (output_ != nullptr && outputRate_ == rate && outputChannels_ == channels) {
        const float volume = muted_.load() ? 0.f : volumePercent_.load() / 100.f;
        output_->setVolume(volume);
        return;
    }
    if (output_ != nullptr) {
        output_->stop();
        output_->deleteLater();
        output_ = nullptr;
    }
    QAudioFormat format;
    format.setSampleRate(rate);
    format.setChannelCount(channels);
    format.setSampleSize(16);
    format.setCodec(QStringLiteral("audio/pcm"));
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    output_ = new QAudioOutput(format, this);
    output_->setBufferSize(rate / 2);
    output_->setVolume(muted_.load() ? 0.f : volumePercent_.load() / 100.f);
    output_->start(sink_);
    outputRate_ = rate;
    outputChannels_ = channels;
    if (paused_.load()) {
        output_->suspend();
    }
}

void MusicWindow::applySeekClear() {
    const int requested = clearRequest_.load();
    if (requested == clearApplied_.load()) {
        return;
    }
    if (sink_ != nullptr) {
        sink_->clear();
    }
    if (output_ != nullptr) {
        output_->stop();
        output_->start(sink_);
        if (paused_.load()) {
            output_->suspend();
        }
    }
    clearApplied_.store(requested);
}

QString MusicWindow::formatTime(const std::int64_t ms) const {
    const auto total = std::max<std::int64_t>(0, ms) / 1000;
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto seconds = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QChar('0'));
}

std::int64_t MusicWindow::positionMs() const {
    const int rate = sampleRate_.load();
    if (rate <= 0) {
        return 0;
    }
    const int channels = std::max(1, channels_.load());
    const double speed = std::max(0.5, speedMilli_.load() / 1000.0);
    const auto buffered = sink_ == nullptr
        ? 0
        : static_cast<std::int64_t>(sink_->buffered_samples() / static_cast<std::size_t>(channels));
    const auto hardwareFrames = output_ && output_->state() != QAudio::StoppedState
        ? std::max(0, output_->bufferSize() - output_->bytesFree()) / (channels * 2) : 0;
    const auto heard = consumedFrames_.load()
        - static_cast<std::int64_t>(std::llround(static_cast<double>(buffered + hardwareFrames) * speed));
    return std::max<std::int64_t>(0, originMs_.load() + heard * 1000 / rate);
}

void MusicWindow::refreshClock() {
    if (vault_ && !vault_->is_unlocked()) {
        stopWorker();
        if (output_) output_->stop();
        sink_->clear();
        tracks_.clear(); art_.clear(); tags_.clear();
        cover_ = QImage(); coverId_ = 0;
        paused_.store(true); sampleRate_.store(0);
        durationMs_.store(0); consumedFrames_.store(0); originMs_.store(0);
        coverSerial_.fetch_add(1);
        vault_.reset();
        clearSystemMedia();
        if (trayCard_) trayCard_->hide();
        if (tray_) tray_->hide();
        applySong(); syncTransport();
        return;
    }
    adoptPendingTrack();
    ensureOutput();
    applySeekClear();
    if (output_ != nullptr && !paused_.load() && sink_ != nullptr
        && sink_->buffered_samples() > 0U) {
        const auto state = output_->state();
        // Windows leaves the device idle after a short underrun and never
        // pulls again. Pause and resume restarts that pull; do it here.
        if (state == QAudio::IdleState || state == QAudio::StoppedState) {
            output_->stop();
            output_->start(sink_);
        }
    }
    if (pendingSeek_ >= 0 && durationMs_.load() > 0) {
        const int value = pendingSeek_;
        pendingSeek_ = -1;
        seekTo(durationMs_.load() * value / 1000);
    }
    if (workerFinished_.load()) {
        std::string error;
        {
            std::lock_guard<std::mutex> guard(errorMutex_);
            error = error_;
        }
        if (!error.empty()) {
            if (errorText_.isEmpty() && sink_->buffered_samples() == 0U
                && (!output_ || output_->state() == QAudio::IdleState || output_->state() == QAudio::StoppedState)) {
                errorText_ = QString::fromStdString(error);
                setPaused(true);
                applyName();
                playButton_->setGlyph(Glyph::Play);
                playButton_->setToolTip(QStringLiteral("Play"));
            }
        } else if (ended_.load() && !terminalEnded_
                   && (!output_ || output_->state() == QAudio::IdleState || output_->state() == QAudio::StoppedState)) {
            ended_.store(false);
            onTrackEnded();
            return;
        }
    }
    const auto duration = durationMs_.load();
    progress_->setEnabled(duration > 0 && !tracks_.empty() && errorText_.isEmpty());
    if (scrubbing_) {
        return;
    }
    const auto position = positionMs();
    if (duration > 0) {
        const int value = static_cast<int>(std::min<std::int64_t>(1000, position * 1000 / duration));
        progress_->setValue(value);
        if (trayCard_ != nullptr && trayCard_->isVisible()) {
            trayCard_->setTimeline(value);
        }
    }
    const QString text = formatTime(position);
    if (timeLabel_->text() != text) {
        timeLabel_->setText(text);
        layoutControls();
    }
    syncSystemMedia(false);
}

void MusicWindow::onTrackEnded() {
    // The decode worker already chains successors before the sink drains.
    // This is a terminal EOF, not a timer-driven restart of the next track.
    terminalEnded_ = true;
    {
        paused_.store(true);
        playButton_->setGlyph(Glyph::Play);
        playButton_->setToolTip(QStringLiteral("Play"));
        if (output_ != nullptr) {
            output_->suspend();
        }
        const auto duration = durationMs_.load();
        if (duration > 0) {
            progress_->setValue(1000);
            timeLabel_->setText(formatTime(duration));
        }
        updateTray();
    }
}

void MusicWindow::layoutControls() {
    const bool wide = width() >= telegram_music::wideWidth || barOver_ || cursorOnChrome();
    const bool skip = previousAvailable() || nextAvailable();
    previousButton_->setVisible(skip);
    nextButton_->setVisible(skip);
    volumeButton_->setVisible(wide);
    orderButton_->setVisible(wide);
    repeatButton_->setVisible(wide);
    speedButton_->setVisible(wide);

    const int timeWidth = std::max(32, timeLabel_->fontMetrics().horizontalAdvance(timeLabel_->text()) + 2);
    const auto g = telegram_music::geometry(width(), skip, wide, timeWidth,
        nameLabel_->fontMetrics().height(), nameLabel_->fontMetrics().ascent());
    previousButton_->setGeometry(g.previous);
    playButton_->setGeometry(g.play);
    nextButton_->setGeometry(g.next);
    closeButton_->setGeometry(g.close);
    speedButton_->setGeometry(g.speed);
    repeatButton_->setGeometry(g.repeat);
    orderButton_->setGeometry(g.order);
    volumeButton_->setGeometry(g.volume);
    timeLabel_->setVisible(wide); // Telegram hides the entire right-controls wrap.
    timeLabel_->setGeometry(g.time);
    nameLabel_->setGeometry(g.name);
    nameRect_ = QRect(g.name.x(), 0, g.name.width(), kBarHeight - telegram_music::seekHeight);
    progress_->setGeometry(g.seek);
    progress_->raise();
    applyName();
}

void MusicWindow::applyName() {
    const bool failed = !errorText_.isEmpty();
    // The normal colour is the palette's window text so the label follows the
    // theme; only the failure state keeps a dedicated warning colour.
    if (failed) {
        nameLabel_->setStyleSheet(QStringLiteral("color: #c0392b; background: transparent;"));
    } else {
        nameLabel_->setStyleSheet(QString());
        nameLabel_->setForegroundRole(QPalette::WindowText);
    }
    const QString full = failed ? errorText_ : titleText_;
    const auto elided = nameLabel_->fontMetrics().elidedText(full, Qt::ElideRight, std::max(0, nameLabel_->width()));
    // FormatSongDocumentName: performer emphasized, song name normal.
    const auto artist = !failed && !artistText_.isEmpty() && elided.startsWith(artistText_)
        ? artistText_ : QString();
    nameLabel_->setText(artist.isEmpty() ? elided.toHtmlEscaped()
        : QStringLiteral("<b>%1</b>%2").arg(artist.toHtmlEscaped(), elided.mid(artist.size()).toHtmlEscaped()));
}

void MusicWindow::syncTransport() {
    const bool playing = !tracks_.empty() && !paused_.load() && !terminalEnded_ && errorText_.isEmpty();
    progress_->setEnabled(!tracks_.empty() && durationMs_.load() > 0 && errorText_.isEmpty());
    playlist_->setPlaying(playing);
    playButton_->setEnabled(!tracks_.empty());
    playButton_->setGlyph(playing ? Glyph::Pause : Glyph::Play);
    playButton_->setToolTip(playing ? tr("Pause") : tr("Play"));
    playButton_->setAccessibleName(playButton_->toolTip());
    const bool skip = previousAvailable() || nextAvailable();
    previousButton_->setVisible(skip);
    nextButton_->setVisible(skip);
    previousButton_->setEnabled(previousAvailable());
    nextButton_->setEnabled(nextAvailable());
    switch (repeat_) {
    case 1:
        repeatButton_->setGlyph(Glyph::RepeatOne);
        repeatButton_->setActive(true);
        repeatButton_->setToolTip(QStringLiteral("Repeat this song"));
        break;
    case 2:
        repeatButton_->setGlyph(Glyph::Repeat);
        repeatButton_->setActive(true);
        repeatButton_->setToolTip(QStringLiteral("Repeat playlist"));
        break;
    default:
        repeatButton_->setGlyph(Glyph::Repeat);
        repeatButton_->setActive(false);
        repeatButton_->setToolTip(QStringLiteral("Repeat"));
        break;
    }
    if (order_ == 2) {
        orderButton_->setGlyph(Glyph::Shuffle);
        orderButton_->setActive(true);
        orderButton_->setToolTip(QStringLiteral("Shuffle"));
    } else if (order_ == 1) {
        orderButton_->setGlyph(Glyph::Reverse);
        orderButton_->setActive(true);
        orderButton_->setToolTip(QStringLiteral("Reverse order"));
    } else {
        orderButton_->setGlyph(Glyph::Reverse);
        orderButton_->setActive(false);
        orderButton_->setToolTip(QStringLiteral("Playback order"));
    }
    const int volume = muted_.load() ? 0 : volumePercent_.load();
    volumeButton_->setGlyph(volume <= 0 ? Glyph::VolumeOff : volume < 66 ? Glyph::VolumeLow : Glyph::VolumeFull);
    volumeButton_->setAccessibleName(tr("Volume %1%").arg(volume));
    repeatButton_->setAccessibleName(repeatButton_->toolTip());
    orderButton_->setAccessibleName(orderButton_->toolTip());
    layoutControls();
    updateTray();
}

void MusicWindow::setSpeedMilli(const int milli) {
    const int snapped = std::clamp((milli + 50) / 100 * 100, 500, 2500);
    speedMilli_.store(snapped);
    if (snapped != 1000) {
        lastSpeedMilli_ = snapped;
    }
    speedButton_->setLabel(speedLabel(snapped));
    speedButton_->setActive(snapped != 1000);
    if (speedMenu_ != nullptr && speedMenu_->isVisible()) {
        speedMenu_->sync(snapped);
    }
    savePlayback();
}

void MusicWindow::toggleSpeed() {
    if (std::abs(speedMilli_.load() - 1000) < 50) {
        setSpeedMilli(lastSpeedMilli_);
    } else {
        setSpeedMilli(1000);
    }
}

void MusicWindow::setOrder(const int order) {
    invalidateNextTrack();
    adoptPendingTrack(false);
    order_ = order;
    history_.clear();
    if (order_ == 2) {
        refillBag();
    } else {
        bag_.clear();
    }
    prepareNextTrack();
    syncTransport();
    if (orderMenu_ != nullptr && orderMenu_->isVisible()) {
        orderMenu_->sync(order_);
    }
    savePlayback();
}

void MusicWindow::setVolumePercent(const int percent) {
    const int next = std::clamp(percent, 0, 100);
    if (next > 0) {
        rememberedVolume_ = next;
    }
    volumePercent_.store(next);
    muted_.store(next == 0);
    syncTransport();
    if (output_ != nullptr) {
        output_->setVolume(next / 100.f);
    }
    if (volumeMenu_ != nullptr && volumeMenu_->isVisible()) {
        volumeMenu_->sync(next);
    }
    savePlayback();
}

void MusicWindow::toggleMute() {
    if (!muted_.load() && volumePercent_.load() > 0) {
        rememberedVolume_ = volumePercent_.load();
        muted_.store(true);
    } else {
        muted_.store(false);
        if (volumePercent_.load() <= 0) {
            volumePercent_.store(rememberedVolume_ > 0 ? rememberedVolume_ : 80);
        }
    }
    syncTransport();
    if (output_ != nullptr) {
        output_->setVolume(muted_.load() ? 0.f : volumePercent_.load() / 100.f);
    }
    if (volumeMenu_ != nullptr && volumeMenu_->isVisible()) {
        volumeMenu_->sync(muted_.load() ? 0 : volumePercent_.load());
    }
    savePlayback();
}

void MusicWindow::showOnly(QWidget* popup, QWidget* anchor, const bool alignRight) {
    for (QWidget* other : {static_cast<QWidget*>(playlist_), static_cast<QWidget*>(orderMenu_), static_cast<QWidget*>(speedMenu_), static_cast<QWidget*>(volumeMenu_)}) {
        if (other != nullptr && other != popup) {
            other->hide();
        }
    }
    if (popup == nullptr || anchor == nullptr) {
        return;
    }
    if (popup == playlist_) {
        if (tracks_.empty()) {
            return;
        }
        playlist_->sync(tracks_, index_);
    } else if (popup == orderMenu_) {
        orderMenu_->sync(order_);
    } else if (popup == speedMenu_) {
        speedMenu_->sync(speedMilli_.load());
    } else if (popup == volumeMenu_) {
        volumeMenu_->sync(muted_.load() ? 0 : volumePercent_.load());
    }
    popup->adjustSize();
    const QPoint origin = anchor->mapToGlobal(QPoint(0, 0));
    int x = alignRight ? origin.x() + anchor->width() - popup->width() : origin.x();
    // Telegram's bar is above its chat; this footer opens upward instead.
    if (popup == volumeMenu_) x = origin.x() + (anchor->width() - popup->width()) / 2;
    int y = origin.y() - popup->height() + 1;
    if (QScreen* screen = QGuiApplication::screenAt(origin)) {
        const QRect area = screen->availableGeometry();
        x = std::clamp(x, area.left() + 8, std::max(area.left() + 8, area.right() - popup->width() - 8));
        y = std::max(area.top() + 8, y);
    }
    popup->move(x, y);
    popup->show();
    popup->raise();
}

void MusicWindow::hidePopups() {
    if (playlistShowTimer_) playlistShowTimer_->stop();
    for (QWidget* popup : {static_cast<QWidget*>(playlist_), static_cast<QWidget*>(orderMenu_), static_cast<QWidget*>(speedMenu_), static_cast<QWidget*>(volumeMenu_)}) {
        if (popup != nullptr) {
            popup->hide();
        }
    }
}

void MusicWindow::armHide() {
    hideTimer_->start();
}

bool MusicWindow::cursorOnChrome() const {
    const QPoint cursor = QCursor::pos();
    const auto hits = [&](QWidget* widget) {
        return widget != nullptr && widget->isVisible()
            && QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size()).contains(cursor);
    };
    if (hits(playlist_) || hits(orderMenu_) || hits(speedMenu_) || hits(volumeMenu_)) {
        return true;
    }
    if (!isVisible()) {
        return false;
    }
    if (QRect(mapToGlobal(nameRect_.topLeft()), nameRect_.size()).contains(cursor)) {
        return true;
    }
    return hits(volumeButton_) || hits(orderButton_) || hits(speedButton_);
}

void MusicWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutControls();
}

void MusicWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), playerBar());
    painter.fillRect(0, 0, width(), 1, playerHairline());
}

void MusicWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && nameRect_.contains(event->pos())) {
        hidePopups();
        showTrayCard();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MusicWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && nameRect_.contains(event->pos())) {
        hidePopups();
        showTrayCard();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MusicWindow::mouseMoveEvent(QMouseEvent* event) {
    QWidget::mouseMoveEvent(event);
    if (nameRect_.contains(event->pos())) {
        if (!playlist_->isVisible() && !playlistShowTimer_->isActive()) playlistShowTimer_->start();
    } else {
        playlistShowTimer_->stop();
        armHide();
    }
}

void MusicWindow::enterEvent(QEvent* event) {
    QWidget::enterEvent(event);
    barOver_ = true;
    layoutControls();
}

void MusicWindow::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    playlistShowTimer_->stop();
    QTimer::singleShot(0, this, [this] {
        if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
            barOver_ = false;
            layoutControls();
        }
        armHide();
    });
}

bool MusicWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress && !tracks_.empty()) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_MediaPlay: case Qt::Key_MediaTogglePlayPause:
        case Qt::Key_MediaPause: case Qt::Key_MediaStop:
        case Qt::Key_MediaNext: case Qt::Key_MediaPrevious:
            keyPressEvent(key);
            return true;
        default: break;
        }
    }
    if (!isVisible()) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Wheel && watched == volumeButton_) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (wheel->angleDelta().y()) {
            setVolumePercent((muted_.load() ? 0 : volumePercent_.load()) + wheel->angleDelta().y() / 12);
            wheel->accept();
            return true;
        }
    }
    if (event->type() == QEvent::Enter &&
        (watched == playlist_ || watched == orderMenu_ || watched == speedMenu_ || watched == volumeMenu_)) {
        hideTimer_->stop();
    } else if (event->type() == QEvent::Leave &&
        (watched == playlist_ || watched == orderMenu_ || watched == speedMenu_ || watched == volumeMenu_)) {
        armHide();
    }
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        hidePopups();
    }
    if (event->type() == QEvent::MouseButtonPress) {
        if (!cursorOnChrome() && !rect().contains(mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPos()))) {
            hidePopups();
        }
    }
    if (event->type() == QEvent::KeyPress) {
        if (QApplication::activeModalWidget() != nullptr) {
            return QWidget::eventFilter(watched, event);
        }
        auto* focus = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(focus) != nullptr || qobject_cast<QComboBox*>(focus) != nullptr) {
            return QWidget::eventFilter(watched, event);
        }
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && !key->isAutoRepeat()
            && qobject_cast<QAbstractButton*>(focus) == nullptr) {
            togglePlay();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MusicWindow::loadPlayback() {
    QSettings settings;
    volumePercent_.store(std::clamp(settings.value(QStringLiteral("player/volume"), 100).toInt(), 0, 100));
    muted_.store(settings.value(QStringLiteral("player/muted"), false).toBool());
    rememberedVolume_ = std::clamp(
        settings.value(QStringLiteral("player/rememberedVolume"), volumePercent_.load()).toInt(), 1, 100);
    order_ = std::clamp(settings.value(QStringLiteral("player/order"), 0).toInt(), 0, 2);
    repeat_ = std::clamp(settings.value(QStringLiteral("player/repeat"), 0).toInt(), 0, 2);
    const int speed = std::clamp(settings.value(QStringLiteral("player/speedMilli"), 1000).toInt(), 500, 2500);
    speedMilli_.store(speed);
    lastSpeedMilli_ = std::clamp(settings.value(QStringLiteral("player/lastSpeedMilli"), 1500).toInt(), 500, 2500);
    speedButton_->setLabel(speedLabel(speed));
    speedButton_->setActive(std::abs(speed - 1000) > 40);
    syncTransport();
}

void MusicWindow::savePlayback() {
    QSettings settings;
    settings.setValue(QStringLiteral("player/volume"), volumePercent_.load());
    settings.setValue(QStringLiteral("player/muted"), muted_.load());
    settings.setValue(QStringLiteral("player/rememberedVolume"), rememberedVolume_);
    settings.setValue(QStringLiteral("player/order"), order_);
    settings.setValue(QStringLiteral("player/repeat"), repeat_);
    settings.setValue(QStringLiteral("player/speedMilli"), speedMilli_.load());
    settings.setValue(QStringLiteral("player/lastSpeedMilli"), lastSpeedMilli_);
}

SongTags MusicWindow::songFor(const int index) const {
    SongTags song;
    if (index < 0 || index >= static_cast<int>(tracks_.size())) {
        return song;
    }
    song = songFromFileName(tracks_[static_cast<std::size_t>(index)].title);
    const auto known = tags_.find(tracks_[static_cast<std::size_t>(index)].id);
    if (known != tags_.end()) {
        if (!known->second.title.isEmpty()) {
            song.title = known->second.title;
        }
        if (!known->second.artist.isEmpty()) {
            song.artist = known->second.artist;
        }
        if (!known->second.album.isEmpty()) {
            song.album = known->second.album;
        }
    }
    if (song.title.isEmpty()) {
        song.title = tracks_[static_cast<std::size_t>(index)].title;
    }
    return song;
}

void MusicWindow::applySong() {
    if (tracks_.empty()) {
        songTitle_ = QStringLiteral("Nothing to play");
        artistText_.clear();
        albumText_.clear();
        titleText_ = songTitle_;
    } else {
        const SongTags song = songFor(index_);
        songTitle_ = song.title;
        artistText_ = song.artist;
        albumText_ = song.album;
        titleText_ = artistText_.isEmpty()
            ? songTitle_
            : artistText_ + QStringLiteral(" — ") + songTitle_;
    }
    applyName();
    if (playlist_ != nullptr) {
        playlist_->sync(tracks_, index_);
        for (const auto& entry : art_) {
            playlist_->setArt(entry.first, QPixmap::fromImage(entry.second));
        }
    }
    requestCovers();
    updateTray();
}

void MusicWindow::publishTrackInfo(
    const std::int64_t id,
    const QString& title,
    const QString& artist,
    const QString& album,
    const QImage& cover) {
    if (tracks_.empty() || tracks_[static_cast<std::size_t>(index_)].id != id) {
        return;
    }
    SongTags tags;
    tags.title = title.trimmed();
    tags.artist = artist.trimmed();
    tags.album = album.trimmed();
    if (!tags.title.isEmpty() || !tags.artist.isEmpty() || !tags.album.isEmpty()) {
        tags_[id] = tags;
    }
    if (!cover.isNull()) {
        cover_ = cover;
        coverId_ = id;
        const QImage plate = cover.scaled(80, 80, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
        art_[id] = plate;
        if (playlist_ != nullptr) {
            playlist_->setArt(id, QPixmap::fromImage(plate));
        }
    }
    applySong();
}

void MusicWindow::updateTray() {
    if (tray_ == nullptr) {
        return;
    }
    QString tip = songTitle_.isEmpty() ? QStringLiteral("Mega Vault Protect") : songTitle_;
    if (!artistText_.isEmpty() && !tracks_.empty()) {
        tip = artistText_ + QStringLiteral(" — ") + songTitle_;
    }
    tray_->setToolTip(tip);
    if (trayCard_ == nullptr) {
        return;
    }
    const std::int64_t id = tracks_.empty() ? 0 : tracks_[static_cast<std::size_t>(index_)].id;
    trayCard_->present(
        tracks_.empty() ? QStringLiteral("Nothing to play") : songTitle_,
        artistText_,
        albumText_,
        coverId_ == id ? cover_ : QImage(),
        !paused_.load() && !tracks_.empty() && errorText_.isEmpty(),
        muted_.load() ? 0 : volumePercent_.load(),
        previousAvailable(),
        nextAvailable());
    syncSystemMedia(true);
}

void MusicWindow::showTrayCard() {
    if (trayCard_ == nullptr) {
        return;
    }
    updateTray();
    trayCard_->adjustSize();
    if (!popupPlaced_) {
        QSettings settings;
        const bool saved = settings.contains(QStringLiteral("player/popupX"))
            && settings.contains(QStringLiteral("player/popupY"));
        int x = settings.value(QStringLiteral("player/popupX")).toInt();
        int y = settings.value(QStringLiteral("player/popupY")).toInt();
        QScreen* screen = QGuiApplication::primaryScreen();
        const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
        if (!saved) {
            x = area.right() - trayCard_->width() - 16;
            y = area.bottom() - trayCard_->height() - 48;
        }
        const QRect card(x, y, trayCard_->width(), trayCard_->height());
        bool visible = false;
        for (QScreen* candidate : QGuiApplication::screens()) {
            if (candidate->availableGeometry().intersects(card)) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            x = area.right() - trayCard_->width() - 16;
            y = area.bottom() - trayCard_->height() - 48;
        }
        trayCard_->move(x, y);
        popupPlaced_ = true;
    }
    trayCard_->show();
    trayCard_->raise();
}

void MusicWindow::closeEvent(QCloseEvent* event) {
    clearSystemMedia();
    hidePopups();
    if (trayCard_ != nullptr) {
        trayCard_->hide();
    }
    if (tray_ != nullptr) {
        tray_->hide();
    }
    stopWorker();
    coverSerial_.fetch_add(1);
    if (output_) output_->stop();
    sink_->clear();
    paused_.store(true);
    tracks_.clear(); art_.clear(); tags_.clear();
    vault_.reset(); cover_ = QImage(); coverId_ = 0;
    durationMs_.store(0); consumedFrames_.store(0); originMs_.store(0);
    applySong(); syncTransport();
    clearSystemMedia();
    QWidget::closeEvent(event);
}

void MusicWindow::keyPressEvent(QKeyEvent* event) {
    if (tracks_.empty() || event->isAutoRepeat()) { event->ignore(); return; }
    // Keyboard media keys: the bar is often the only visible transport, so the
    // hardware keys must drive it even when the window is not focused.
    switch (event->key()) {
    case Qt::Key_MediaPlay:
        if (workerFinished_.load()) { showTrack(); startWorker(); }
        else setPaused(false);
        event->accept();
        return;
    case Qt::Key_MediaTogglePlayPause:
        togglePlay();
        event->accept();
        return;
    case Qt::Key_MediaPause:
        setPaused(true);
        event->accept();
        return;
    case Qt::Key_MediaStop: {
        stopPlayback();
        event->accept();
        return;
    }
    case Qt::Key_MediaNext:
        advance(1);
        event->accept();
        return;
    case Qt::Key_MediaPrevious:
        advance(-1);
        event->accept();
        return;
    default:
        break;
    }
    if (event->key() == Qt::Key_Space) {
        togglePlay();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Left) {
        seekBy(-5000);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        seekBy(5000);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace videovault::app
