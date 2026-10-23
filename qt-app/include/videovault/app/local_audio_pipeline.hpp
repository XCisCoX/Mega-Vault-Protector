#pragma once

#include "videovault/app/media_decoder.hpp"
#include "videovault/core/vault.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace videovault::app {

// Every local queue member uses the SAME device format, including mixed-rate
// albums. The reader and its FFmpeg state have exactly one owning thread.
class LocalAudioReader final {
public:
    static constexpr int rate = 48000;
    static constexpr int channels = 2;
    LocalAudioReader();
    ~LocalAudioReader();
    LocalAudioReader(const LocalAudioReader&) = delete;
    LocalAudioReader& operator=(const LocalAudioReader&) = delete;
    bool open(const std::shared_ptr<core::Vault>& vault, std::int64_t id,
              const QByteArray& hint, QString* error);
    bool read(std::vector<std::int16_t>* samples);
    bool seek(std::int64_t ms);
    std::int64_t duration() const;
    AudioTags tags() const;
    QImage cover() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One next decoder and one ~100ms PCM block, never an entire next song. Requests
// replace/cancel earlier work; a completed stale decoder is destroyed on the
// preparation thread, and cannot be taken by a newer request. No UI callbacks.
class LocalAudioPrefetch final {
public:
    struct Prepared {
        std::unique_ptr<LocalAudioReader> reader;
        std::vector<std::int16_t> samples;
        QString error;
        ~Prepared();
    };
    LocalAudioPrefetch();
    ~LocalAudioPrefetch();
    void request(const std::shared_ptr<core::Vault>& vault, std::int64_t id,
                 const QByteArray& hint, std::uint64_t ticket);
    void cancel();
    std::unique_ptr<Prepared> take(std::uint64_t ticket);
private:
    void run();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool stopping_{false};
    bool requested_{false};
    std::uint64_t revision_{0};
    std::uint64_t ticket_{0};
    std::uint64_t generation_{0};
    std::int64_t id_{0};
    QByteArray hint_;
    std::shared_ptr<core::Vault> vault_;
    std::unique_ptr<Prepared> prepared_;
};

} // namespace videovault::app
