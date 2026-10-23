#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>

namespace videovault::app {
// Shared TLS session; never installs cookies or a disk cache. Every hop is checked.
class SoundCloudTransport final {
public:
    struct Result { QByteArray body; QString error; QUrl final_url; int status{0}; };
    explicit SoundCloudTransport(bool loopback_fixture = false);
    ~SoundCloudTransport();
    Result get(QUrl url, int limit, const std::atomic<bool>& cancelled, bool redirect_target_only = false);
    static bool permits(const QUrl&, bool loopback_fixture = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace videovault::app
