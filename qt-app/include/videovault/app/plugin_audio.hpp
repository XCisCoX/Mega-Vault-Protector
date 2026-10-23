#pragma once
#include <QMediaPlayer>
#include <QObject>
#include <memory>
namespace videovault::app {
class ByteSource;
// Uses the app's in-process FFmpeg decoder + Qt PCM output, not the OS FLAC
// file/URL decoder (Windows DirectShow cannot reliably consume a QBuffer).
class PluginAudioPlayer final : public QObject {
    Q_OBJECT
  public:
    explicit PluginAudioPlayer(QObject *parent = nullptr);
    ~PluginAudioPlayer() override;
    void setMedia(const QMediaContent &, QIODevice *stream = nullptr);
    // Progressive source: decoded while the bytes are still arriving. The caller
    // owns it and must keep it alive until stop()/the next setSource().
    void setSource(ByteSource *source);
    void play();
    void pause();
    void stop();
    void setPosition(qint64);
    QMediaPlayer::State state() const;
    QMediaPlayer::Error error() const;
    QMediaPlayer::MediaStatus mediaStatus() const;
    QString errorString() const;
    qint64 position() const;
    qint64 duration() const;
  signals:
    void stateChanged(QMediaPlayer::State);
    void error(QMediaPlayer::Error);
    void positionChanged(qint64);
    void durationChanged(qint64);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace videovault::app
