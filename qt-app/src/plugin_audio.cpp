#include "videovault/app/plugin_audio.hpp"
#include "videovault/app/byte_source.hpp"
#include "videovault/app/media_decoder.hpp"
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include <QBuffer>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstring>
namespace videovault::app {
namespace {
// Pull-mode PCM has bounded buffering; reads may arrive on the audio thread.
class PcmSource final : public QIODevice {
  public:
    std::shared_ptr<MediaDecoder> decoder;
    mutable QMutex mutex;
    QByteArray pending;
    int cursor = 0;
    bool eof = false;
    explicit PcmSource(std::shared_ptr<MediaDecoder> d, QObject *parent)
        : QIODevice(parent), decoder(std::move(d)) {
        open(QIODevice::ReadOnly);
    }
    ~PcmSource() override { pending.fill(0); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        QMutexLocker lock(&mutex);
        return pending.size() - cursor + (eof ? 0 : 32768) + QIODevice::bytesAvailable();
    }
    bool atEnd() const override {
        QMutexLocker lock(&mutex);
        return eof && cursor >= pending.size();
    }
    bool seekTime(qint64 ms) {
        QMutexLocker lock(&mutex);
        pending.fill(0);
        pending.clear();
        cursor = 0;
        eof = false;
        return decoder->seek_to(ms) >= 0;
    }

  protected:
    qint64 writeData(const char *, qint64) override { return -1; }
    qint64 readData(char *data, qint64 maximum) override {
        QMutexLocker lock(&mutex);
        qint64 copied = 0;
        while (copied < maximum) {
            if (cursor == pending.size()) {
                pending.fill(0);
                pending.clear();
                cursor = 0;
                if (eof)
                    break;
                std::int64_t pts = 0;
                if (!decoder->pump_audio(8192, &pts)) {
                    eof = true;
                    break;
                }
                auto samples = decoder->take_audio_samples();
                if (samples.empty()) {
                    eof = true;
                    break;
                }
                pending = QByteArray(reinterpret_cast<const char *>(samples.data()),
                                     int(samples.size() * sizeof(std::int16_t)));
                std::fill(samples.begin(), samples.end(), 0);
            }
            const qint64 amount = std::min<qint64>(maximum - copied, pending.size() - cursor);
            std::memcpy(data + copied, pending.constData() + cursor, size_t(amount));
            cursor += int(amount);
            copied += amount;
        }
        return copied;
    }
};
struct OpenResult {
    std::shared_ptr<MediaDecoder> decoder;
    QString error;
};
} // namespace
class PluginAudioPlayer::Impl {
  public:
    PluginAudioPlayer *owner;
    QAudioOutput *output = nullptr;
    PcmSource *source = nullptr;
    QTimer timer;
    quint64 generation = 0;
    bool wantPlay = false;
    QMediaPlayer::State state = QMediaPlayer::StoppedState;
    QMediaPlayer::Error error = QMediaPlayer::NoError;
    QMediaPlayer::MediaStatus status = QMediaPlayer::NoMedia;
    QString message;
    qint64 base = 0, lastPosition = 0, length = 0;
    explicit Impl(PluginAudioPlayer *o) : owner(o) {
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, o, [this] {
            if (output && state == QMediaPlayer::PlayingState) {
                lastPosition = std::min(length, base + output->processedUSecs() / 1000);
                emit owner->positionChanged(lastPosition);
            }
        });
    }
    ~Impl() { clear(); }
    void change(QMediaPlayer::State next) {
        if (state != next) {
            state = next;
            emit owner->stateChanged(state);
        }
    }
    void fail(QString text) {
        message = std::move(text);
        error = QMediaPlayer::ResourceError;
        status = QMediaPlayer::InvalidMedia;
        wantPlay = false;
        change(QMediaPlayer::StoppedState);
        emit owner->error(error);
    }
    void clear() {
        ++generation;
        wantPlay = false;
        timer.stop();
        if (output) {
            output->disconnect(owner);
            output->stop();
            delete output;
            output = nullptr;
        }
        delete source;
        source = nullptr;
        base = lastPosition = length = 0;
        error = QMediaPlayer::NoError;
        status = QMediaPlayer::NoMedia;
        message.clear();
        change(QMediaPlayer::StoppedState);
        emit owner->positionChanged(0);
        emit owner->durationChanged(0);
    }
    void start() {
        wantPlay = true;
        if (!output || !source)
            return;
        if (state == QMediaPlayer::PausedState && output->state() == QAudio::SuspendedState)
            output->resume();
        else if (state != QMediaPlayer::PlayingState)
            output->start(source);
        if (output->error() != QAudio::NoError) {
            fail("Qt audio output could not start (device error " + QString::number(output->error()) + ").");
            return;
        }
        change(QMediaPlayer::PlayingState);
        timer.start();
    }
    // Wires a freshly opened decoder to the audio device and starts playback if
    // the caller already asked for it. Shared by the RAM and streaming paths.
    void attach(std::shared_ptr<MediaDecoder> decoder) {
        QAudioFormat format;
        format.setCodec("audio/pcm");
        format.setSampleRate(decoder->audio_sample_rate());
        format.setChannelCount(decoder->audio_channels());
        format.setSampleSize(16);
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);
        const auto device = QAudioDeviceInfo::defaultOutputDevice();
        if (device.isNull() || !device.isFormatSupported(format)) {
            fail("No audio device supports the decoded PCM format.");
            return;
        }
        length = decoder->duration_ms();
        source = new PcmSource(std::move(decoder), owner);
        output = new QAudioOutput(device, format, owner);
        output->setBufferSize(65536);
        output->setVolume(0.5);
        status = QMediaPlayer::LoadedMedia;
        emit owner->durationChanged(length);
        QObject::connect(output, &QAudioOutput::stateChanged, owner, [this](QAudio::State next) {
            if (next == QAudio::StoppedState && output && output->error() != QAudio::NoError)
                fail("Qt audio device stopped with error " + QString::number(output->error()) + ".");
            else if (next == QAudio::IdleState && source && source->atEnd()) {
                timer.stop();
                status = QMediaPlayer::EndOfMedia;
                lastPosition = length;
                change(QMediaPlayer::StoppedState);
                emit owner->positionChanged(length);
            }
        });
        if (wantPlay)
            start();
    }

    // Opens a live provider stream. The worker thread blocks inside the source
    // until the demuxer has the bytes it needs, which is why playback starts
    // before the track has been fully fetched.
    void load_source(ByteSource *source) {
        clear();
        status = QMediaPlayer::LoadingMedia;
        const auto gen = generation;
        auto *watcher = new QFutureWatcher<OpenResult>(owner);
        QObject::connect(watcher, &QFutureWatcher<OpenResult>::finished, owner, [this, watcher, gen] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (gen != generation)
                return;
            if (!result.decoder) {
                fail("Cannot decode the provider stream: " + result.error);
                return;
            }
            attach(std::move(result.decoder));
        });
        watcher->setFuture(QtConcurrent::run([source] {
            OpenResult result;
            result.decoder = std::make_shared<MediaDecoder>();
            result.decoder->prepare_for_audio();
            result.decoder->set_stream_cache_bytes(256U << 10);
            if (!result.decoder->open_source(source, &result.error, "music.mp3") ||
                !result.decoder->has_audio())
                result.decoder.reset();
            return result;
        }));
    }

    void load(const QByteArray &bytes) {
        clear();
        status = QMediaPlayer::LoadingMedia;
        const auto gen = generation;
        auto *watcher = new QFutureWatcher<OpenResult>(owner);
        QObject::connect(watcher, &QFutureWatcher<OpenResult>::finished, owner, [this, watcher, gen] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (gen != generation)
                return;
            if (!result.decoder) {
                fail("Cannot decode in-memory audio: " + result.error);
                return;
            }
            attach(std::move(result.decoder));
        });
        watcher->setFuture(QtConcurrent::run([bytes] {
            OpenResult result;
            result.decoder = std::make_shared<MediaDecoder>();
            if (!result.decoder->open_memory(bytes, &result.error) || !result.decoder->has_audio())
                result.decoder.reset();
            return result;
        }));
    }
};
PluginAudioPlayer::PluginAudioPlayer(QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
PluginAudioPlayer::~PluginAudioPlayer() = default;
void PluginAudioPlayer::setMedia(const QMediaContent &, QIODevice *stream) {
    auto *buffer = qobject_cast<QBuffer *>(stream);
    if (buffer)
        impl_->load(buffer->data());
    else
        impl_->clear();
}
void PluginAudioPlayer::setSource(ByteSource *source) {
    if (source != nullptr)
        impl_->load_source(source);
    else
        impl_->clear();
}
void PluginAudioPlayer::play() {
    if (impl_->status == QMediaPlayer::EndOfMedia)
        setPosition(0);
    impl_->start();
}
void PluginAudioPlayer::pause() {
    impl_->wantPlay = false;
    if (impl_->output) {
        impl_->output->suspend();
        impl_->timer.stop();
        impl_->change(QMediaPlayer::PausedState);
    }
}
void PluginAudioPlayer::stop() { impl_->clear(); }
void PluginAudioPlayer::setPosition(qint64 position) {
    if (!impl_->source || !impl_->output)
        return;
    const auto state = impl_->state;
    impl_->output->stop();
    position = std::clamp<qint64>(position, 0, impl_->length);
    if (!impl_->source->seekTime(position)) {
        impl_->fail("Cannot seek the in-memory audio stream.");
        return;
    }
    impl_->base = impl_->lastPosition = position;
    impl_->change(QMediaPlayer::StoppedState);
    if (state == QMediaPlayer::PlayingState)
        impl_->start();
    else if (state == QMediaPlayer::PausedState)
        impl_->change(QMediaPlayer::PausedState);
    emit positionChanged(position);
}
QMediaPlayer::State PluginAudioPlayer::state() const { return impl_->state; }
QMediaPlayer::Error PluginAudioPlayer::error() const { return impl_->error; }
QMediaPlayer::MediaStatus PluginAudioPlayer::mediaStatus() const { return impl_->status; }
QString PluginAudioPlayer::errorString() const { return impl_->message; }
qint64 PluginAudioPlayer::position() const { return impl_->lastPosition; }
qint64 PluginAudioPlayer::duration() const { return impl_->length; }
} // namespace videovault::app
