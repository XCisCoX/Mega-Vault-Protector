#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace videovault::core {
class Vault;
} // namespace videovault::core

namespace videovault::app {

class ByteSource;

// AudioReady is backpressure/progress, never end-of-stream. Drain PCM and retry.
enum class DecodeStep { VideoFrame, AudioReady, End };

// A decoded video frame with its presentation time (ms).
struct DecodedFrame {
    QImage image;
    std::int64_t pts_ms{0};
    // Decoder output size, before it is scaled to the window. A seek can
    // emit a few frames at a different size than the rest of the stream.
    int source_width{0};
    int source_height{0};
};

// Title, artist, and album read from the container tags (ID3, Vorbis, MP4).
struct AudioTags {
    QString title;
    QString artist;
    QString album;
};

// In-process FFmpeg player engine. It streams the plaintext of an encrypted
// video through `Vault::read_video_range` (bounded, per-chunk authenticated)
// via a custom AVIO context — nothing is ever written to disk and no
// whole-file plaintext buffer is materialized. Widget- and audio-device-free:
// the player window owns the QAudioOutput and paints the emitted frames.
class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;

    // Opens the video for streaming. On failure returns false and fills
    // `error` with a human-readable message.
    bool open(
        const std::shared_ptr<videovault::core::Vault>& vault,
        std::int64_t video_id,
        QString* error,
        const char* name_hint = nullptr);

    // Plugin-only RAM source. Shares Qt's immutable byte storage; never stages
    // plaintext in a file or imports into a vault merely to start playback.
    bool open_memory(const QByteArray& bytes, QString* error);

    // Opens any random-access byte source: an encrypted package, a RAM buffer,
    // or a live provider stream. The caller owns `source` and must keep it alive
    // until close() (the decoder does not take ownership, because a network
    // source belongs to the UI thread that created it). A progressive source
    // blocks inside read() until the bytes it needs have arrived, which is what
    // makes playback start before the whole track exists locally.
    bool open_source(ByteSource* source, QString* error,
        const char* name_hint = nullptr);

    bool is_open() const;
    std::int64_t duration_ms() const;
    int video_width() const;
    int video_height() const;
    // FFmpeg codec name of the video stream (e.g. "h264", "png", "mjpeg").
    std::string video_codec_name() const;

    // True when a real video stream (not an embedded album cover) was opened.
    bool has_video() const;
    // True when an audio stream is available and configured.
    bool has_audio() const;
    int audio_sample_rate() const;
    int audio_channels() const;
    // Tag fields from the open container. Empty when the file has none.
    AudioTags audio_tags() const;
    // Embedded cover art, when the container carries an attached picture.
    QImage attached_cover() const;

    // Streaming memory budget for the read-ahead window (bytes): how much of
    // the file the player pulls into RAM ahead of the playhead. Larger values
    // prefetch more (fewer disk/decrypt hits, more memory); applies to the
    // next window load. Clamped to >= 1 MiB. Music uses 1 MiB so opening a
    // song does not decrypt the rest of the file.
    void set_stream_cache_bytes(std::size_t bytes);
    std::size_t stream_cache_bytes() const;

    // Open the next file as audio: a short header probe, no still-image
    // demuxer attempts, and no scan of the rest of the song.
    void prepare_for_audio();

    // Decodes frames directly at the given display size (sws scale target),
    // so the player avoids a second scaling pass. Rebuilds the scaler only
    // when the size changes. Safe to call only from the decode thread.
    void set_display_size(int width, int height);

    // Seeks to a presentation time (ms). Buffered audio is discarded.
    // Returns 0 on success or a negative AVERROR code on failure.
    [[nodiscard]] int seek_to(std::int64_t ms);

    // Resident interleaved PCM has a soft cap: at most one decoded audio frame
    // may cross it. Timestamp silence remains sparse and is not resident PCM.
    static constexpr std::size_t kMaxResidentAudioSamples = 1U << 20;
    std::size_t resident_audio_samples() const;
    std::size_t buffered_audio_samples() const;

    // Returns AudioReady when PCM must be drained before demux can continue.
    // Only VideoFrame initializes `frame`; End means both codecs are drained.
    [[nodiscard]] DecodeStep decode_next_video_frame(DecodedFrame* frame);

    // Decodes until at least `min_samples` interleaved PCM samples are buffered,
    // or the file ends. Requests are clamped to the resident cap.
    // Returns false when no samples are available.
    bool pump_audio(int min_samples, std::int64_t* pts_ms);

    // Interleaved s16le PCM decoded since the last call, in the format
    // reported by audio_sample_rate()/audio_channels().
    // Timestamp gaps become silence, overlaps/preroll are trimmed. A bounded
    // pull lets the player keep a short queue even across a long silent gap.
    std::vector<std::int16_t> take_audio_samples(std::size_t max_samples = 1U << 20);

    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace videovault::app
