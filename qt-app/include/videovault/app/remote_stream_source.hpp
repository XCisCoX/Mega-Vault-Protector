#pragma once
#include "videovault/app/byte_source.hpp"
#include "videovault/core/vault.hpp"
#include <QObject>
#include <QByteArray>
#include <QStringList>
#include <QUrl>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
namespace videovault::app {
// Owner-thread HTTP transport, blocking decoder reads, bounded plaintext RAM.
// Destroy only after all borrowed ByteSource readers have stopped.
class RemoteStreamSource final : public QObject, public ByteSource {
    Q_OBJECT
public:
    struct Limits {
        qint64 buffer_bytes = 16 * 1024 * 1024;
        int stall_timeout_ms = 20000;
        // Maximum object extent, not cumulative traffic (seeks reread bytes).
        qint64 max_bytes = 0;
        QStringList allowed_hosts;
        // HTTP only for explicitly enabled loopback test fixtures.
        bool require_https = true;
        bool require_flac = false;
    };
    using Progress = std::function<void(qint64, qint64)>;
    using Failure = std::function<void(const QString&)>;
    // Owner-thread, nonblocking callback. Contiguous bytes exactly once from
    // offset zero, even when playback seeks. Late attachment refetches missing
    // prefix bytes; ordinary playback + import share the same fetch.
    using ByteSink = std::function<void(std::int64_t, const char*, std::ptrdiff_t)>;
    using SinkReady = std::function<bool()>;
    explicit RemoteStreamSource(QUrl, Limits, QObject* parent = nullptr);
    ~RemoteStreamSource() override;
    void start();
    int read(unsigned char*, int) override;
    std::int64_t seek(std::int64_t, int) override;
    std::int64_t size() const override;
    void set_buffer_budget(std::size_t) override;
    void cancel() override;
    QString contentType() const;
    QUrl finalUrl() const;
    bool honours_ranges() const;
    bool failed() const;
    bool complete() const;
    QString errorString() const;
    qint64 received_bytes() const;
    qint64 buffered_bytes() const;
    qint64 bytes_served() const;
    void set_sink(ByteSink);
    void set_sink_ready(SinkReady);
    void set_progress_callback(Progress);
    void set_failure_callback(Failure);
private:
    struct Chunk {
        std::int64_t offset = 0;
        std::vector<char> data;
        std::int64_t end() const { return offset + static_cast<std::int64_t>(data.size()); }
    };
    struct NativeRequest;
    void pump();
    void open_request(std::int64_t);
    void on_ready_read(QNetworkReply*);
    void on_finished(QNetworkReply*);
    bool accept_headers(int, const QByteArray&, const QByteArray&, const QString&, const QUrl&, const QByteArray& validator);
    void accept_bytes(const char*, qint64);
    void finish_response(const QString&);
    void fail(const QString&);
    void abort_request_now();
    bool host_allowed(const QUrl&) const;
    int take_locked(unsigned char*, int);
    bool covers_locked(std::int64_t) const;
    void trim_locked();
    bool transport_active() const;
    QUrl url_;
    Limits limits_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Chunk> chunks_;
    std::int64_t position_ = 0, buffered_total_ = 0, stream_offset_ = 0;
    std::int64_t total_ = -1, received_ = 0, served_ = 0;
    std::int64_t request_offset_ = -1, sink_offset_ = 0;
    std::int64_t response_end_ = -1, eof_offset_ = -1;
    bool started_ = false, cancelled_ = false, complete_ = false, failed_ = false;
    bool range_ignored_ = false, headers_done_ = false, reader_waiting_ = false;
    int redirects_ = 0;
    qint64 last_activity_ms_ = 0;
    QString error_, content_type_;
    QUrl final_url_;
    QByteArray validator_, flac_header_;
    bool flac_checked_ = false;
    ByteSink sink_;
    SinkReady sink_ready_;
    Progress progress_;
    Failure failure_;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<NativeRequest> native_;
};
// Bounded nonblocking tap. All begin/append/commit/abort and disk/crypto run on
// a worker. Cancellation never joins a disk worker on the UI; the worker retains
// the vault until staging cleanup finishes. Source must outlive the tap.
class RemoteStreamImport final {
public:
    RemoteStreamImport(RemoteStreamSource&, std::shared_ptr<core::Vault>,
        QString name, std::int64_t folder, core::TrackProvenance, bool requireFlac = true);
    ~RemoteStreamImport();
    void cancel();
    bool finished() const;
    bool committed() const;
    std::int64_t videoId() const;
    QString errorString() const;
    qint64 queuedBytes() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace videovault::app
