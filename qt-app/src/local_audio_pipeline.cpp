#include "videovault/app/local_audio_pipeline.hpp"

#include "videovault/core/ffmpeg_compat.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#include <sodium.h>
#include <algorithm>

namespace videovault::app {
namespace {
void wipe(std::vector<std::int16_t>& samples) {
    if (!samples.empty()) sodium_memzero(samples.data(), samples.size() * sizeof(samples[0]));
    samples.clear();
}
}
struct LocalAudioReader::Impl {
    MediaDecoder decoder;
    SwrContext* resampler{nullptr};
    std::shared_ptr<core::Vault> vault;
    std::uint64_t generation{0};
    bool eof{false};
    ~Impl() { swr_free(&resampler); }
    bool valid() const {
        return vault && vault->is_unlocked() && vault->lock_generation() == generation;
    }
};
LocalAudioReader::LocalAudioReader() : impl_(std::make_unique<Impl>()) {}
LocalAudioReader::~LocalAudioReader() = default;
bool LocalAudioReader::open(const std::shared_ptr<core::Vault>& vault, std::int64_t id,
                            const QByteArray& hint, QString* error) {
    impl_ = std::make_unique<Impl>();
    auto& p = *impl_;
    p.vault = vault;
    p.generation = vault ? vault->lock_generation() : 0;
    if (!p.valid()) { if (error) *error = QStringLiteral("The vault is locked."); return false; }
    p.decoder.prepare_for_audio();
    p.decoder.set_stream_cache_bytes(1U << 20);
    if (!p.decoder.open(vault, id, error, hint.isEmpty() ? nullptr : hint.constData())
        || !p.decoder.has_audio() || !p.valid()) return false;
    const int status = core::ffmpeg_compat::make_stereo_rate_resampler(
        &p.resampler, rate, p.decoder.audio_sample_rate());
    if (status < 0 || !p.resampler || swr_init(p.resampler) < 0) {
        if (error) *error = QStringLiteral("Could not prepare the audio resampler.");
        return false;
    }
    return true;
}
bool LocalAudioReader::read(std::vector<std::int16_t>* samples) {
    wipe(*samples);
    auto& p = *impl_;
    if (!p.valid() || !p.resampler) return false;
    while (samples->empty()) {
        std::vector<std::int16_t> input;
        if (!p.eof) {
            p.eof = !p.decoder.pump_audio(std::max(1, p.decoder.audio_sample_rate() / 10) * channels, nullptr);
            input = p.decoder.take_audio_samples();
        }
        const int frames = static_cast<int>(input.size() / channels);
        // swr_get_out_samples includes the filter delay, drained at the end.
        const int capacity = std::max(64, swr_get_out_samples(p.resampler, frames));
        samples->resize(static_cast<std::size_t>(capacity) * channels);
        auto* output = reinterpret_cast<std::uint8_t*>(samples->data());
        const auto* source = reinterpret_cast<const std::uint8_t*>(input.data());
        const int count = swr_convert(p.resampler, &output, capacity,
            frames > 0 ? &source : nullptr, frames);
        wipe(input);
        if (count < 0 || !p.valid()) { wipe(*samples); return false; }
        samples->resize(static_cast<std::size_t>(count) * channels);
        if (p.eof && count == 0) return false;
    }
    return true;
}
bool LocalAudioReader::seek(std::int64_t ms) {
    auto& p = *impl_;
    if (!p.valid() || p.decoder.seek_to(ms) < 0) return false;
    swr_close(p.resampler);
    p.eof = false;
    return swr_init(p.resampler) >= 0;
}
std::int64_t LocalAudioReader::duration() const { return impl_->decoder.duration_ms(); }
AudioTags LocalAudioReader::tags() const { return impl_->decoder.audio_tags(); }
QImage LocalAudioReader::cover() const {
    auto image = impl_->decoder.attached_cover();
    if (image.width() > 256 || image.height() > 256)
        image = image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

LocalAudioPrefetch::Prepared::~Prepared() { wipe(samples); }
LocalAudioPrefetch::LocalAudioPrefetch() : thread_([this] { run(); }) {}
LocalAudioPrefetch::~LocalAudioPrefetch() {
    { std::lock_guard<std::mutex> guard(mutex_); stopping_ = true; ++revision_; }
    changed_.notify_one();
    thread_.join();
}
void LocalAudioPrefetch::request(const std::shared_ptr<core::Vault>& vault, std::int64_t id,
                               const QByteArray& hint, std::uint64_t ticket) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        ++revision_;
        ticket_ = ticket; vault_ = vault; id_ = id; hint_ = hint;
        generation_ = vault ? vault->lock_generation() : 0;
        prepared_.reset(); requested_ = true;
    }
    changed_.notify_one();
}
void LocalAudioPrefetch::cancel() {
    std::lock_guard<std::mutex> guard(mutex_);
    ++revision_; requested_ = false; prepared_.reset(); vault_.reset();
}
std::unique_ptr<LocalAudioPrefetch::Prepared> LocalAudioPrefetch::take(std::uint64_t ticket) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (ticket != ticket_ || !vault_ || !vault_->is_unlocked()
        || vault_->lock_generation() != generation_) { prepared_.reset(); return {}; }
    return std::move(prepared_);
}
void LocalAudioPrefetch::run() {
    for (;;) {
        std::unique_lock<std::mutex> guard(mutex_);
        changed_.wait(guard, [this] { return stopping_ || requested_; });
        if (stopping_) return;
        const auto revision = revision_;
        const auto vault = vault_;
        const auto generation = vault ? vault->lock_generation() : 0;
        const auto id = id_;
        const auto hint = hint_;
        requested_ = false;
        guard.unlock();
        auto result = std::make_unique<Prepared>();
        result->reader = std::make_unique<LocalAudioReader>();
        if (!result->reader->open(vault, id, hint, &result->error)
            || !result->reader->read(&result->samples)) {
            if (result->error.isEmpty()) result->error = QStringLiteral("The next track has no playable audio.");
            result->reader.reset();
        }
        guard.lock();
        if (!stopping_ && revision == revision_ && vault && vault->is_unlocked()
            && vault->lock_generation() == generation) prepared_ = std::move(result);
        // A stale result (including its plaintext PCM) dies here, before waiting.
    }
}
} // namespace videovault::app
