#include "videovault/core/ffmpeg_compat.hpp"
#include "videovault/app/soundcloud_hls_source.hpp"
#include "videovault/app/soundcloud_transport.hpp"
#include <QRegularExpression>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <sodium.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
namespace videovault::app {
namespace {
constexpr int kSegmentLimit = 8 << 20;
constexpr int kRingLimit = 4 << 20;
QString uriAttribute(const QString& line) {
    static const QRegularExpression pattern(QStringLiteral("(?:[:,])URI=\"([^\"]+)\""));
    return pattern.match(line).captured(1);
}
struct MemoryInput {
    QByteArray bytes; qint64 cursor{0};
    ~MemoryInput() { if (!bytes.isEmpty()) sodium_memzero(bytes.data(), size_t(bytes.size())); }
    static int read(void* opaque, uint8_t* buffer, int size) {
        auto& self = *static_cast<MemoryInput*>(opaque);
        const auto amount = std::min<qint64>(size, self.bytes.size() - self.cursor);
        if (amount <= 0) return AVERROR_EOF;
        std::memcpy(buffer, self.bytes.constData() + self.cursor, size_t(amount)); self.cursor += amount;
        return int(amount);
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& self = *static_cast<MemoryInput*>(opaque);
        if (whence == AVSEEK_SIZE) return self.bytes.size();
        const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? self.cursor : whence == SEEK_END ? self.bytes.size() : -1;
        if (base < 0 || offset < -base || offset > self.bytes.size() - base) return -1;
        return self.cursor = base + offset;
    }
};
struct Input {
    AVFormatContext* format{nullptr}; AVIOContext* io{nullptr}; MemoryInput memory;
    ~Input() { avformat_close_input(&format); if (io) { av_freep(&io->buffer); avio_context_free(&io); } }
    bool open(QByteArray data) {
        memory.bytes = std::move(data);
        io = avio_alloc_context(static_cast<unsigned char*>(av_malloc(32768)), 32768, 0, &memory, MemoryInput::read, nullptr, MemoryInput::seek);
        format = avformat_alloc_context(); if (!io || !format) return false;
        format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->probesize = std::min<int>(memory.bytes.size(), 1 << 20);
        format->max_analyze_duration = 2 * AV_TIME_BASE;
        AVDictionary* options = nullptr;
        // Prevent malicious nested manifests or reference movies from opening a URL/file.
        av_dict_set(&options, "format_whitelist", "mp3,aac,mpegts,mov,ogg", 0);
        av_dict_set(&options, "protocol_whitelist", "", 0);
        const AVInputFormat* hint = nullptr;
        // Short audio-only TS fragments can score below ADTS during generic
        // probing. Validate packet sync before forcing the container demuxer.
        if (memory.bytes.size() >= 3 * 188 && static_cast<unsigned char>(memory.bytes[0]) == 0x47
            && static_cast<unsigned char>(memory.bytes[188]) == 0x47 && static_cast<unsigned char>(memory.bytes[376]) == 0x47)
            hint = av_find_input_format("mpegts");
        const auto opened = avformat_open_input(&format, nullptr, hint, &options);
        av_dict_free(&options);
        return opened >= 0 && avformat_find_stream_info(format, nullptr) >= 0;
    }
};
}
bool SoundCloudHlsPlaylist::parse(const QByteArray& bytes, const QUrl& base,
    SoundCloudHlsPlaylist* output, QString* error, bool fixture) {
    auto refuse = [&](const char* reason) { if (error) *error = QString::fromLatin1(reason); return false; };
    if (!output || bytes.size() > (1 << 20) || !bytes.startsWith("#EXTM3U")) return refuse("Invalid or oversized HLS manifest.");
    SoundCloudHlsPlaylist parsed;
    bool end = false, variant = false, pending = false; QUrl map;
    for (const auto& raw : QString::fromUtf8(bytes).split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith("#EXT-X-KEY:") || line.startsWith("#EXT-X-SESSION-KEY:")) {
            if (!line.contains("METHOD=NONE") || line.contains("URI=")) return refuse("Encrypted/DRM HLS is not supported; no keys are fetched.");
        } else if (line.startsWith("#EXT-X-BYTERANGE:") || line.contains("BYTERANGE=")
            || line.startsWith("#EXT-X-PART:") || line.startsWith("#EXT-X-GAP")) return refuse("Byte-range, partial or missing HLS segments are not supported.");
        else if (line.startsWith("#EXT-X-MAP:")) {
            const auto uri = uriAttribute(line); if (uri.isEmpty()) return refuse("Missing HLS initialization URI.");
            map = base.resolved(QUrl(uri)); if (!SoundCloudTransport::permits(map, fixture)) return refuse("HLS initialization host is not permitted.");
        } else if (line.startsWith("#EXT-X-STREAM-INF:")) variant = true;
        else if (line.startsWith("#EXTINF:")) pending = true;
        else if (line == "#EXT-X-ENDLIST") end = true;
        else if (!line.startsWith('#')) {
            auto url = base.resolved(QUrl(line));
            if (!SoundCloudTransport::permits(url, fixture)) return refuse("HLS segment/variant host is not permitted.");
            if (variant) { if (parsed.variant.isEmpty()) parsed.variant = url; variant = false; }
            else if (pending) {
                if (parsed.segments.size() >= 10000) return refuse("HLS segment count exceeds the limit.");
                parsed.segments.append({url, map}); pending = false;
            } else return refuse("Unexpected HLS URI.");
        }
    }
    if (variant || pending) return refuse("Incomplete HLS manifest.");
    if (!parsed.variant.isEmpty() && !parsed.segments.isEmpty()) return refuse("Mixed HLS master/media playlist.");
    if (parsed.variant.isEmpty() && (!end || parsed.segments.isEmpty())) return refuse("Only finite HLS VOD playlists are supported.");
    *output = std::move(parsed); if (error) error->clear(); return true;
}
struct SoundCloudHlsSource::Impl {
    QUrl manifest; bool fixture;
    std::atomic<bool> cancelled{false}; std::thread worker;
    mutable std::mutex mutex; std::condition_variable wake;
    std::vector<unsigned char> ring = std::vector<unsigned char>(kRingLimit);
    size_t head{0}, count{0}, peak{0}; qint64 position{0}; bool done{false}; QString error;
    Impl(QUrl url, bool local) : manifest(std::move(url)), fixture(local) {}
    ~Impl() { sodium_memzero(ring.data(), ring.size()); }
    void fail(QString text) { std::lock_guard<std::mutex> lock(mutex); error = std::move(text); done = true; wake.notify_all(); }
    static int write(void* opaque, const uint8_t* data, int size) {
        auto& self = *static_cast<Impl*>(opaque); int copied = 0;
        std::unique_lock<std::mutex> lock(self.mutex);
        while (copied < size && !self.cancelled) {
            self.wake.wait(lock, [&] { return self.cancelled || self.count < self.ring.size(); });
            if (self.cancelled) break;
            const auto tail = (self.head + self.count) % self.ring.size();
            const auto take = std::min({size_t(size - copied), self.ring.size() - self.count, self.ring.size() - tail});
            std::memcpy(self.ring.data() + tail, data + copied, take); copied += int(take); self.count += take;
            self.peak = std::max(self.peak, self.count); self.wake.notify_all();
        }
        return self.cancelled ? AVERROR_EXIT : copied;
    }
    void run() {
        SoundCloudTransport transport(fixture); SoundCloudHlsPlaylist playlist; auto url = manifest;
        for (int depth = 0; depth < 3; ++depth) {
            const auto fetched = transport.get(url, 1 << 20, cancelled);
            QString reason;
            if (!fetched.error.isEmpty()) { fail(fetched.error); return; }
            if (!SoundCloudHlsPlaylist::parse(fetched.body, fetched.final_url, &playlist, &reason, fixture)) { fail(reason); return; }
            if (playlist.variant.isEmpty()) break;
            url = playlist.variant;
        }
        if (!playlist.variant.isEmpty()) { fail("HLS master nesting exceeds the limit."); return; }
        struct Output {
            AVFormatContext* format{nullptr}; AVIOContext* io{nullptr};
            ~Output() { if (format) { format->pb = nullptr; avformat_free_context(format); } if (io) { av_freep(&io->buffer); avio_context_free(&io); } }
        } out;
        AVCodecParameters* reference = nullptr;
        QByteArray initialization; QUrl mapUrl;
        struct WipeInitialization {
            QByteArray& data;
            ~WipeInitialization() { if (!data.isEmpty()) sodium_memzero(data.data(), size_t(data.size())); }
        } wipeInitialization{initialization};
        int64_t nextDts = 0;
        for (const auto& segment : playlist.segments) {
            if (cancelled) break;
            if (segment.initialization != mapUrl) {
                if (!initialization.isEmpty()) sodium_memzero(initialization.data(), size_t(initialization.size()));
                initialization.clear(); mapUrl = segment.initialization;
                if (!mapUrl.isEmpty()) {
                    const auto fetched = transport.get(mapUrl, 2 << 20, cancelled);
                    if (!fetched.error.isEmpty()) { fail(fetched.error); return; }
                    initialization = fetched.body;
                }
            }
            auto fetched = transport.get(segment.url, kSegmentLimit, cancelled);
            if (!fetched.error.isEmpty()) { fail(fetched.error); return; }
            // Initialization + media is one valid fragmented-MP4 input, not concatenated audio.
            QByteArray inputBytes; inputBytes.reserve(initialization.size() + fetched.body.size());
            inputBytes.append(initialization); inputBytes.append(fetched.body);
            if (!fetched.body.isEmpty()) sodium_memzero(fetched.body.data(), size_t(fetched.body.size()));
            fetched.body.clear();
            Input in;
            if (!in.open(std::move(inputBytes))) { fail("HLS segment is not a supported audio container."); return; }
            const int audio = av_find_best_stream(in.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (audio < 0) { fail("HLS segment has no audio."); return; }
            const auto* parameters = in.format->streams[audio]->codecpar;
            if (!out.format) {
                if (avformat_alloc_output_context2(&out.format, nullptr, "nut", nullptr) < 0 || !out.format) { fail("Cannot allocate HLS remuxer."); return; }
                out.io = avio_alloc_context(static_cast<unsigned char*>(av_malloc(32768)), 32768, 1, this, nullptr, write, nullptr);
                auto* stream = avformat_new_stream(out.format, nullptr);
                if (!out.io || !stream || avcodec_parameters_copy(stream->codecpar, parameters) < 0) { fail("Cannot initialize HLS audio output."); return; }
                stream->codecpar->codec_tag = 0; // Container-specific tags must not leak into NUT.
                stream->time_base = AVRational{1, 1000}; reference = stream->codecpar;
                out.io->seekable = 0; out.format->pb = out.io; out.format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
                AVDictionary* options = nullptr; av_dict_set(&options, "write_index", "0", 0);
                const auto result = avformat_write_header(out.format, &options); av_dict_free(&options);
                if (result < 0) { fail("HLS codec cannot be remuxed for memory-only playback."); return; }
                avio_flush(out.io);
            } else if (parameters->codec_id != reference->codec_id || parameters->sample_rate != reference->sample_rate
                || !core::ffmpeg_compat::same_layout(parameters, reference)
                || parameters->extradata_size != reference->extradata_size
                || (parameters->extradata_size && std::memcmp(parameters->extradata, reference->extradata, size_t(parameters->extradata_size)))) {
                fail("HLS codec changes are not supported."); return;
            }
            AVPacket* packet = av_packet_alloc();
            if (!packet) { fail("Cannot allocate HLS packet."); return; }
            int read = 0; int64_t firstDts = AV_NOPTS_VALUE, segmentEnd = nextDts; bool any = false;
            while (!cancelled && (read = av_read_frame(in.format, packet)) >= 0) {
                if (packet->stream_index != audio) { av_packet_unref(packet); continue; }
                any = true;
                av_packet_rescale_ts(packet, in.format->streams[audio]->time_base, out.format->streams[0]->time_base);
                if (firstDts == AV_NOPTS_VALUE) firstDts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts != AV_NOPTS_VALUE ? packet->pts : 0;
                if (packet->dts == AV_NOPTS_VALUE) packet->dts = firstDts;
                if (packet->pts == AV_NOPTS_VALUE) packet->pts = packet->dts;
                packet->dts = packet->dts - firstDts + nextDts; packet->pts = packet->pts - firstDts + nextDts;
                if (packet->duration <= 0) packet->duration = av_rescale_q(1024, AVRational{1, std::max(1, parameters->sample_rate)}, out.format->streams[0]->time_base);
                segmentEnd = std::max(segmentEnd, packet->dts + std::max<int64_t>(1, packet->duration));
                packet->stream_index = 0; packet->pos = -1;
                const auto written = av_interleaved_write_frame(out.format, packet); av_packet_unref(packet);
                if (written < 0) { av_packet_free(&packet); fail("HLS packet remux failed."); return; }
                avio_flush(out.io);
            }
            av_packet_free(&packet);
            if (!cancelled && (read != AVERROR_EOF || !any)) { fail("Truncated or empty HLS audio segment."); return; }
            nextDts = segmentEnd;
        }
        if (out.format && !cancelled) {
            if (av_write_trailer(out.format) < 0) { fail("HLS remux did not finish."); return; }
            avio_flush(out.io);
        }
        std::lock_guard<std::mutex> lock(mutex); done = true; wake.notify_all();
    }
};
SoundCloudHlsSource::SoundCloudHlsSource(QUrl url, bool fixture) : impl_(std::make_unique<Impl>(std::move(url), fixture)) {}
SoundCloudHlsSource::~SoundCloudHlsSource() { cancel(); if (impl_->worker.joinable()) impl_->worker.join(); }
void SoundCloudHlsSource::start() { if (!impl_->worker.joinable() && !impl_->cancelled) impl_->worker = std::thread([this] { impl_->run(); }); }
void SoundCloudHlsSource::cancel() { impl_->cancelled = true; impl_->wake.notify_all(); }
int SoundCloudHlsSource::read(unsigned char* buffer, int size) {
    if (!buffer || size <= 0) return 0;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->wake.wait(lock, [&] { return impl_->cancelled || impl_->done || impl_->count; });
    if (impl_->cancelled) return AVERROR_EXIT;
    if (!impl_->error.isEmpty()) return AVERROR(EIO);
    if (!impl_->count) return 0;
    const auto take = std::min({size_t(size), impl_->count, impl_->ring.size() - impl_->head});
    std::memcpy(buffer, impl_->ring.data() + impl_->head, take);
    sodium_memzero(impl_->ring.data() + impl_->head, take);
    impl_->head = (impl_->head + take) % impl_->ring.size(); impl_->count -= take; impl_->position += qint64(take); impl_->wake.notify_all();
    return int(take);
}
std::int64_t SoundCloudHlsSource::seek(std::int64_t offset, int whence) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return whence == SEEK_CUR && offset == 0 ? impl_->position : -1;
}
QString SoundCloudHlsSource::errorString() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }
std::size_t SoundCloudHlsSource::peak_buffered() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->peak; }
} // namespace videovault::app
