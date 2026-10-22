#pragma once

#include "videovault/app/media_decoder.hpp"

#include <QAudioOutput>
#include <QColor>
#include <QDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSize>
#include <QSlider>
#include <QTimer>

class QVariantAnimation;

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QAbstractButton;
class QAudioFormat;
class QComboBox;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QShowEvent;

namespace videovault::app {

// Pull-mode QIODevice that QAudioOutput reads decoded PCM from. The decode
// worker appends s16le samples; the audio thread drains them at its own pace.
// Thread-safe (append may be called from the worker thread).
class AudioSink final : public QIODevice {
    Q_OBJECT
public:
    explicit AudioSink(QObject* parent = nullptr);
    ~AudioSink() override;

    void append(const std::int16_t* samples, std::size_t count);
    void clear();
    // Number of interleaved s16 samples still buffered.
    std::size_t buffered_samples() const;

    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    mutable std::mutex mutex_;
    std::vector<std::int16_t> buffer_;
    std::size_t read_cursor_{0};
};

// Video display surface: paints the latest frame directly (no QPixmap
// conversion), aspect-fit on black, with an optional centered text overlay
// ("Loading…" / error messages) and a transient bottom overlay for events
// like failed seeks. The worker's frames are already display-sized.
class VideoSurface final : public QWidget {
    Q_OBJECT
public:
    explicit VideoSurface(QWidget* parent = nullptr)
        : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMouseTracking(true);
    }

    void setFrame(const QImage& image) {
        frame_ = image;
        update();
    }

    // Image-viewer view transform: zoom <= 0 means aspect-fit; otherwise the
    // image is drawn at `zoom` image-pixels-per-widget-pixel with its top-left
    // at `offset`.
    void setView(const double zoom, const QPointF& offset) {
        zoom_ = zoom;
        offset_ = offset;
        update();
    }

    void setText(const QString& text) {
        text_ = text;
        frame_ = QImage();
        update();
    }

    void setOverlay(const QString& text) {
        overlayText_ = text;
        update();
    }

    void clearOverlay() {
        overlayText_.clear();
        update();
    }

    // A quiet chip in the corner of a still: "1920 × 1080 · Fit 41%". Empty
    // hides it, and video playback never sets it.
    void setInfo(const QString& text) {
        if (info_ == text) {
            return;
        }
        info_ = text;
        update();
    }

    const QImage& frame() const { return frame_; }
    const QString& statusText() const { return text_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintBackdrop(painter);
        if (!frame_.isNull()) {
            if (zoom_ > 0.0) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                const QRectF target(offset_, QSizeF(frame_.size()) * zoom_);
                paintPlate(painter, target);
                painter.drawImage(target, frame_);
            } else {
                // Grow and shrink with the window, but never past one image
                // pixel per screen pixel: a 300px still should look like a
                // 300px still, not a blurred wall.
                QSize fitted = frame_.size().scaled(size(), Qt::KeepAspectRatio);
                if (fitted.width() > frame_.width() || fitted.height() > frame_.height()) {
                    fitted = frame_.size();
                }
                const QRectF target(
                    QPointF((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0),
                    QSizeF(fitted));
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                paintPlate(painter, target);
                painter.drawImage(target, fitCache(fitted));
            }
            paintInfo(painter);
            if (!overlayText_.isEmpty()) {
                // Transient message band at the bottom of the picture.
                const QRect band(0, height() - 34, width(), 34);
                painter.fillRect(band, QColor(0, 0, 0, 170));
                painter.setPen(QColor(255, 220, 120));
                painter.drawText(band, Qt::AlignCenter, overlayText_);
            }
        } else if (!text_.isEmpty()) {
            painter.setPen(QColor(200, 200, 200));
            painter.drawText(rect(), Qt::AlignCenter, text_);
        }
    }

private:
    // A neutral studio backdrop rather than pure black, with a soft lift behind
    // the picture so it reads as an object on a surface.
    void paintBackdrop(QPainter& painter) const {
        QLinearGradient base(0, 0, 0, height());
        base.setColorAt(0.0, QColor(17, 17, 20));
        base.setColorAt(1.0, QColor(9, 9, 11));
        painter.fillRect(rect(), base);
        if (frame_.isNull()) {
            return;
        }
        QRadialGradient glow(QPointF(width() / 2.0, height() / 2.0),
                             std::max(width(), height()) * 0.62);
        glow.setColorAt(0.0, QColor(255, 255, 255, 12));
        glow.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.fillRect(rect(), glow);
    }

    // Shadow + hairline so the picture has an edge instead of bleeding away.
    void paintPlate(QPainter& painter, const QRectF& target) const {
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int step = 3; step >= 1; --step) {
            const qreal spread = step * 6.0;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, static_cast<int>(26 / step)));
            painter.drawRoundedRect(target.adjusted(-spread, -spread * 0.6, spread, spread),
                                    4.0 + spread * 0.5, 4.0 + spread * 0.5);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
        painter.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));
    }

    // One high-quality downscale per size: drawing a 6000px photo straight into
    // a 900px rect on every repaint shimmers while panning.
    const QImage& fitCache(const QSize& target) {
        if (cacheKey_ != target || cachedSource_ != frame_.cacheKey()) {
            cacheKey_ = target;
            cachedSource_ = frame_.cacheKey();
            cached_ = frame_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return cached_.isNull() ? frame_ : cached_;
    }

    void paintInfo(QPainter& painter) const {
        if (info_.isEmpty() || zoom_ > 0.0) {
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        QFont font = painter.font();
        font.setPointSizeF(9.0);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const int padding = 10;
        const int textWidth = metrics.horizontalAdvance(info_);
        const QRectF chip(width() - textWidth - padding * 2 - 14, 14, textWidth + padding * 2, 26);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(10, 10, 12, 190));
        painter.drawRoundedRect(chip, 13, 13);
        painter.setPen(QColor(235, 235, 240, 210));
        painter.drawText(chip, Qt::AlignCenter, info_);
    }

    QImage frame_;
    QString text_;
    QString overlayText_;
    QString info_;
    QImage cached_;
    QSize cacheKey_;
    qint64 cachedSource_{0};
    double zoom_{0.0};
    QPointF offset_;
};

// Modern seek bar: a thin rounded track that thickens on hover, an
// accent-colored played portion, a round handle while hovering/dragging, and
// a time bubble above the cursor showing the position it would jump to.
// Jumps to the exact clicked position instead of QSlider's default "page
// step" nudge, then drags normally from there.
class SeekSlider final : public QSlider {
    Q_OBJECT
public:
    explicit SeekSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent) {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool active = hovered_ || isSliderDown();
        const int track_h = active ? 6 : 3;
        const int track_y = (height() - track_h) / 2;
        const double fraction = maximum() > minimum()
            ? static_cast<double>(value() - minimum())
                / static_cast<double>(maximum() - minimum())
            : 0.0;
        const int fill_w = static_cast<int>(width() * fraction);

        // Base track and played portion.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 46));
        painter.drawRoundedRect(QRect(0, track_y, width(), track_h), track_h / 2, track_h / 2);
        if (fill_w > 0) {
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.drawRoundedRect(QRect(0, track_y, fill_w, track_h), track_h / 2, track_h / 2);
        }

        if (active && maximum() > minimum()) {
            const int handle_x = static_cast<int>(width() * fraction);
            // Round handle.
            painter.setBrush(QColor(255, 255, 255));
            painter.drawEllipse(QPointF(handle_x, height() / 2.0), 7.0, 7.0);
            // Time bubble above the cursor.
            const int shown_ms = isSliderDown() ? value() : hoverMs_;
            const QString time_text = formatSliderTime(shown_ms);
            const QFontMetrics fm(font());
            const int bubble_w = fm.horizontalAdvance(time_text) + 16;
            const int bubble_h = fm.height() + 8;
            const int bubble_x = std::clamp(handle_x - bubble_w / 2, 2, width() - bubble_w - 2);
            const QRect bubble(bubble_x, 1, bubble_w, bubble_h);
            painter.setBrush(QColor(18, 22, 30, 235));
            painter.setPen(QPen(QColor(255, 255, 255, 70), 1));
            painter.drawRoundedRect(bubble, 6, 6);
            painter.setPen(QColor(238, 242, 248));
            painter.drawText(bubble, Qt::AlignCenter, time_text);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        hovered_ = true;
        if (maximum() > minimum()) {
            const double fraction = std::clamp(
                static_cast<double>(event->pos().x())
                    / static_cast<double>(std::max(1, width())),
                0.0, 1.0);
            hoverMs_ = minimum() + static_cast<int>(fraction * (maximum() - minimum()));
        }
        update();
        QSlider::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hovered_ = false;
        hoverMs_ = value();
        update();
        QSlider::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            const double fraction = std::clamp(
                static_cast<double>(event->pos().x())
                    / static_cast<double>(std::max(1, width())),
                0.0, 1.0);
            setValue(minimum() + static_cast<int>(fraction * (maximum() - minimum())));
        }
        QSlider::mousePressEvent(event);
    }

private:
    static QString formatSliderTime(const int ms) {
        const int total = std::max(0, ms) / 1000;
        const int hours = total / 3600;
        const int minutes = (total % 3600) / 60;
        const int seconds = total % 60;
        if (hours > 0) {
            return QStringLiteral("%1:%2:%3")
                .arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1:%2")
            .arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
    }

    bool hovered_{false};
    int hoverMs_{0};
};

// PotPlayer-style window with a worker-thread decode pipeline: the worker
// streams through MediaDecoder (read-ahead window cache) and paces frames
// against a monotonic real-time clock, so decode stalls or slow scaling can
// never freeze or speed up playback. The UI thread only paints the latest
// frame, updates the seek bar, and owns the audio device.
class PlayerWindow final : public QDialog {
    Q_OBJECT
public:
    PlayerWindow(
        const std::shared_ptr<videovault::core::Vault>& vault,
        std::int64_t video_id,
        const QString& title,
        QWidget* parent = nullptr,
        bool stillImage = false);
    ~PlayerWindow() override;
    void playVideo(std::shared_ptr<videovault::core::Vault> vault, std::int64_t video_id);
    // Same file, from the start, without opening it again.
    void restartFromStart();
    // A still picture. Decrypts the package in chunks, like the video stream,
    // then shows it in the image viewer instead of the playback clock.
    void playStill(std::shared_ptr<videovault::core::Vault> vault, std::int64_t video_id);
    void presentClip(const QString& title, int index, int count, bool hasPrevious, bool hasNext);
    std::int64_t videoId() const { return video_id_; }
    // 0 off, 1 repeat the library, 2 repeat this file.
    int repeatMode() const { return repeatMode_; }
    bool shuffleOn() const { return shuffle_; }
    // Plays inside the main window until the viewer is popped into its own window.
    void prepareDock();
    void detach();
    bool isDetached() const { return detached_; }

signals:
    void detachRequested();
    void dockRequested();
    void previousRequested();
    void nextRequested();
    void playbackFinished();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend struct PlayerWindowTestAccess;
    void resetPlayback(std::shared_ptr<videovault::core::Vault> vault, std::int64_t video_id, bool startVideo);
    void workerLoop();
    void publishFrame(const QImage& image, std::int64_t pts_ms);
    void feedAudio();
    void requestSeek(std::int64_t target_ms);
    void doSeek(std::int64_t target_ms);
    void togglePlayPause();
    void seekRelative(std::int64_t delta_ms);
    void setVolumePercent(int percent);
    void toggleMute();
    void toggleFullscreen();
    bool handleKey(QKeyEvent* event);
    void applyVolume();
    void showChrome();
    void hideChrome();
    void layoutChrome();
    void updatePopIcon();
    void refreshTransportIcons();
    void applyDetachedFrame();
    void updateCenterButton();
    void enterImageMode();
    void zoomImage(double factor, const QPointF& cursor_pos);
    void animateImageZoom(double target, const QPointF& anchor);
    [[nodiscard]] double fittedImageZoom();
    void rotateImage();
    void resetImageFit();
    void clampImageOffset();
    void setImageZoom100();
    void updateZoomLabel();
    void updateImageInfo();
    void tick();
    void fitToVideoSize();
    void updatePositionLabel();
    QString formatTime(std::int64_t ms) const;
    void noteLoopFrame(const QImage& image, std::int64_t ptsMs);
    // False when this frame's decoder size is not the size the rest of the
    // video settled on. Those frames are what make a loop look like it
    // changes resolution.
    bool frameSizeMatches(const DecodedFrame& frame);
    void noteLoopAudio(const std::int16_t* samples, std::size_t count);
    void dropLoopCapture();
    // Plays a short clip from frames already decoded. Returns when repeat,
    // seek, or close ends it.
    void replayCachedLoop();

    std::shared_ptr<videovault::core::Vault> vault_;
    std::int64_t video_id_{0};
    MediaDecoder decoder_;

    VideoSurface* surface_{nullptr};
    // Overlay chrome drawn on top of the video (direct children of the
    // surface so the empty video area keeps its own mouse handling).
    QWidget* topOverlay_{nullptr};
    QLabel* titleLabel_{nullptr};
    QLabel* indexLabel_{nullptr};
    QLabel* captionTitle_{nullptr};
    QWidget* windowCaption_{nullptr};
    QAbstractButton* windowMax_{nullptr};
    QPushButton* popButton_{nullptr};
    bool detached_{false};
    bool frameReady_{false};
    QWidget* controlsOverlay_{nullptr};
    QPushButton* centerPlayButton_{nullptr};
    bool chromeVisible_{true};
    QPushButton* playButton_{nullptr};
    QPushButton* previousButton_{nullptr};
    QPushButton* nextButton_{nullptr};
    QPushButton* backButton_{nullptr};
    QPushButton* forwardButton_{nullptr};
    QSlider* positionSlider_{nullptr};
    QLabel* positionLabel_{nullptr};
    QLabel* timeLeftLabel_{nullptr};
    QPushButton* muteButton_{nullptr};
    QSlider* volumeSlider_{nullptr};
    QComboBox* cacheCombo_{nullptr};
    QPushButton* fullscreenButton_{nullptr};
    QPushButton* repeatButton_{nullptr};
    QPushButton* shuffleButton_{nullptr};
    int repeatMode_{0};
    std::atomic<int> repeatWatch_{0};
    bool shuffle_{false};
    struct LoopPicture {
        QImage image;
        std::int64_t ptsMs{0};
    };
    std::vector<LoopPicture> loopFrames_;
    std::vector<std::int16_t> loopPcm_;
    bool loopCapture_{true};
    int stableFrameW_{0};
    int stableFrameH_{0};
    int stableFrameRun_{0};
    bool clipHasPrevious_{false};
    bool clipHasNext_{false};
    // Image-viewer chrome (shown instead of the playback row for images).
    QPushButton* zoomOutButton_{nullptr};
    QLabel* zoomLabel_{nullptr};
    QPushButton* zoomInButton_{nullptr};
    QPushButton* fitButton_{nullptr};
    QPushButton* rotateButton_{nullptr};

    // Audio device (UI thread only).
    AudioSink* audioSink_{nullptr};
    QAudioOutput* audioOutput_{nullptr};
    std::atomic<AudioSink*> audioSinkAtomic_{nullptr};
    std::int64_t audioOffsetMs_{0};
    bool audioSetupDone_{false};

    // Decode worker communication.
    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> ended_{false};
    std::atomic<bool> seeking_{false};
    std::atomic<bool> openFailed_{false};
    std::atomic<bool> errorShown_{false};
    std::atomic<bool> seekRequested_{false};
    std::atomic<bool> rebaseRequested_{false};
    std::atomic<std::int64_t> seekTargetMs_{0};
    std::atomic<std::int64_t> playheadMs_{0};
    std::atomic<std::int64_t> durationMs_{0};
    std::atomic<int> audioSampleRate_{0};
    std::atomic<int> audioChannels_{0};
    std::atomic<bool> audioReady_{false};
    // Video pixel dimensions reported by the decoder once open succeeds
    // (worker writes; the UI reads them once to fit the window to the video).
    std::atomic<int> videoWidth_{0};
    std::atomic<int> videoHeight_{0};

    // Frame slot (worker writes, UI reads on its timer).
    std::mutex frameMutex_;
    QImage latestFrame_;
    std::atomic<bool> frameDirty_{false};

    // Surface size for worker-side decode scaling.
    std::mutex sizeMutex_;
    QSize surfaceSize_;

    // Open error (worker -> UI).
    std::mutex errorMutex_;
    QString openError_;

    // Seek failure (worker -> UI overlay).
    std::mutex seekErrorMutex_;
    QString seekErrorMessage_;
    std::atomic<bool> seekErrorPending_{false};

    QTimer timer_;
    QTimer uiHideTimer_;

    bool fullscreen_{false};
    bool muted_{false};
    bool clickPending_{false};
    bool autoSized_{false};
    bool playIconPlaying_{true};
    bool imageMode_{false};
    std::atomic<bool> imageModeFlag_{false};
    bool panning_{false};
    QPoint panStartPos_;
    QPoint panAnchor_;
    QPointF panStartOffset_;
    // Photo viewer: an animated zoom keeps the point under the cursor still,
    // the way Telegram's media viewer does, instead of snapping.
    QVariantAnimation* imageZoomAnim_{nullptr};
    QPointF imageZoomAnchor_;
    QPointF imageZoomImagePoint_;
    double imageZoom_{0.0};
    int stillEpoch_{0};
    QPointF imageOffset_;
    QString title_;
    int volumePercent_{100};
    int playbackEpoch_{0};
    bool advanceArmed_{false};
    QImage lastFrame_;
};

} // namespace videovault::app
