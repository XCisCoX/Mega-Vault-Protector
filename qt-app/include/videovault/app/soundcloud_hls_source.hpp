#pragma once
#include "videovault/app/byte_source.hpp"
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>
#include <memory>

namespace videovault::app {
struct SoundCloudHlsPlaylist {
    struct Segment { QUrl url; QUrl initialization; };
    QVector<Segment> segments;
    QUrl variant;
    static bool parse(const QByteArray&, const QUrl&, SoundCloudHlsPlaylist*, QString*, bool fixture = false);
};
// Fetches one bounded segment at a time and demuxes/remuxes packets into a single
// nonseekable NUT stream. No playlist, media or decoded bytes touch disk.
// VOD only. Encryption/DRM, byte ranges and codec changes are explicitly refused.
class SoundCloudHlsSource final : public ByteSource {
public:
    explicit SoundCloudHlsSource(QUrl manifest, bool fixture = false);
    ~SoundCloudHlsSource() override;
    void start();
    int read(unsigned char*, int) override;
    std::int64_t seek(std::int64_t, int) override;
    std::int64_t size() const override { return -1; }
    void cancel() override;
    QString errorString() const;
    std::size_t peak_buffered() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace videovault::app
