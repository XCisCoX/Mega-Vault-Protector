#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QAudioOutput;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QSystemTrayIcon;
class QTimer;

namespace videovault::core {
class Vault;
} // namespace videovault::core

namespace videovault::app {

class AudioSink;
class GlyphButton;
class MediaDecoder;
class LocalAudioReader;
class LocalAudioPrefetch;
class OrderPopup;
class PlaylistPopup;
class ProgressLine;
class NowPlayingCard;
class SpeedPopup;
class TimeStretch;
class VolumePopup;

struct SongTags {
    QString title;
    QString artist;
    QString album;
};

struct MusicTrack {
    std::int64_t id{0};
    QString title;
};

// Desktop now-playing bar, laid out like Telegram Desktop's
// Media::Player::Widget: transport, title, time, volume, order, repeat,
// speed, and close, with the playlist and menus opening above the bar.
class MusicWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MusicWindow(QWidget* parent = nullptr);
    ~MusicWindow() override;

    // One player per process, owned by the main shell. Pages borrow this bar
    // without re-parenting it, so navigation cannot hide it or create a second
    // audio device, queue or tray icon.
    static MusicWindow* shared(QWidget* owner);
    [[nodiscard]] static MusicWindow* instance();

    void playQueue(
        const std::shared_ptr<videovault::core::Vault>& vault,
        const std::vector<MusicTrack>& tracks,
        int index);
    // Queue management for the page that owns the bar. `playNext` inserts in
    // playback order (also reverse/shuffle), otherwise tracks go to the end.
    // An empty bar needs playQueue(vault, tracks, 0) to establish its vault.
    void enqueue(const std::vector<MusicTrack>& tracks, bool playNext);
    // Tracks currently in the queue, for a queue view or a test.
    [[nodiscard]] std::size_t queueSize() const { return tracks_.size(); }
    [[nodiscard]] bool isPlaying() const { return !tracks_.empty() && !paused_.load() && !ended_.load() && errorText_.isEmpty(); }
    // Leave the bar up and the position where it is. A video should not
    // tear the player down.
    void pauseForVideo();
    // The now-playing card is a top-level window, so minimizing the library
    // does not hide it or stop the audio device.
    void showWhileMinimized();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showTrack();
    void startWorker();
    void stopWorker();
    bool advance(int delta);
    void playIndex(int index);
    void togglePlay();
    void seekTo(std::int64_t ms);
    void refreshClock();
    void onTrackEnded();
    void ensureOutput();
    void applySeekClear();
    void layoutControls();
    void applyName();
    void syncTransport();
    void setSpeedMilli(int milli);
    void toggleSpeed();
    void setOrder(int order);
    void setVolumePercent(int percent);
    void toggleMute();
    void showOnly(QWidget* popup, QWidget* anchor, bool alignRight);
    void hidePopups();
    void armHide();
    bool cursorOnChrome() const;
    bool nextAvailable() const;
    bool previousAvailable() const;
    void refillBag();
    void loadPlayback();
    void savePlayback();
    void applySong();
    void updateTray();
    void showTrayCard();
    void syncSystemMedia(bool metadata);
    void clearSystemMedia();
    void publishTrackInfo(
        std::int64_t id,
        const QString& title,
        const QString& artist,
        const QString& album,
        const QImage& cover);
    SongTags songFor(int index) const;
    std::int64_t positionMs() const;
    QString formatTime(std::int64_t ms) const;
    void seekBy(std::int64_t deltaMs);
    void requestCovers();
    void setPaused(bool paused);
    void stopPlayback();
    void prepareNextTrack();
    void invalidateNextTrack();
    void adoptPendingTrack(bool prepare = true);

    std::shared_ptr<videovault::core::Vault> vault_;
    std::vector<MusicTrack> tracks_;
    int index_{0};
    int order_{0};
    int repeat_{0};
    int lastSpeedMilli_{1500};
    int rememberedVolume_{100};
    bool scrubbing_{false};
    int pendingSeek_{-1};
    bool barOver_{false};
    std::vector<int> history_;
    std::vector<int> bag_;

    std::unique_ptr<LocalAudioPrefetch> prefetch_;
    struct NextTrack {
        std::uint64_t ticket{0};
        bool updating{false};
        int index{-1};
        std::int64_t id{0};
        QByteArray hint;
    };
    struct PendingTrack {
        int index{-1};
        std::int64_t id{0};
        std::int64_t duration{0};
        QString title, artist, album;
        QImage cover;
    };
    std::mutex chainMutex_;
    NextTrack nextTrack_;
    PendingTrack pendingTrack_;
    std::uint64_t nextTicket_{0};
    bool adoptingTrack_{false};
    bool terminalEnded_{false};
    std::unique_ptr<TimeStretch> stretch_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> workerFinished_{true};
    std::atomic<bool> paused_{false};
    std::atomic<bool> ended_{false};
    std::atomic<std::int64_t> seekMs_{-1};
    std::atomic<std::int64_t> originMs_{0};
    std::atomic<std::int64_t> consumedFrames_{0};
    std::atomic<std::int64_t> durationMs_{0};
    std::atomic<int> sampleRate_{0};
    std::atomic<int> channels_{2};
    std::atomic<int> clearRequest_{0};
    std::atomic<int> clearApplied_{0};
    std::atomic<int> speedMilli_{1000};
    std::atomic<int> volumePercent_{100};
    std::atomic<bool> muted_{false};
    std::mutex errorMutex_;
    std::string error_;

    AudioSink* sink_{nullptr};
    QAudioOutput* output_{nullptr};
    int outputRate_{0};
    int outputChannels_{0};

    GlyphButton* previousButton_{nullptr};
    GlyphButton* playButton_{nullptr};
    GlyphButton* nextButton_{nullptr};
    GlyphButton* volumeButton_{nullptr};
    GlyphButton* orderButton_{nullptr};
    GlyphButton* repeatButton_{nullptr};
    GlyphButton* speedButton_{nullptr};
    GlyphButton* closeButton_{nullptr};
    QLabel* nameLabel_{nullptr};
    QLabel* timeLabel_{nullptr};
    ProgressLine* progress_{nullptr};
    PlaylistPopup* playlist_{nullptr};
    OrderPopup* orderMenu_{nullptr};
    SpeedPopup* speedMenu_{nullptr};
    VolumePopup* volumeMenu_{nullptr};
    QTimer* clock_{nullptr};
    QTimer* hideTimer_{nullptr};
    QTimer* playlistShowTimer_{nullptr};
    bool popupPlaced_{false};
    struct SystemMediaSession;
    std::unique_ptr<SystemMediaSession> systemMedia_;
    QSystemTrayIcon* tray_{nullptr};
    NowPlayingCard* trayCard_{nullptr};
    qint64 trayDismissedAt_{0};
    QString titleText_;
    QString songTitle_;
    QString artistText_;
    QString albumText_;
    QString errorText_;
    QRect nameRect_;
    QImage cover_;
    std::int64_t coverId_{0};
    std::unordered_map<std::int64_t, QImage> art_;
    struct NoteProbe {
        std::mutex mutex;
        std::unordered_set<std::int64_t> tried;
        std::atomic<int> inflight{0};
    };
    std::shared_ptr<NoteProbe> noteProbe_{std::make_shared<NoteProbe>()};
    std::atomic<int> coverSerial_{0};
    std::atomic<std::uint64_t> playbackSerial_{0};
    std::unordered_map<std::int64_t, SongTags> tags_;
};

} // namespace videovault::app
