#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace videovault::app {

// The phone side of a library swap. The phone cannot accept connections, so
// it keeps asking the share server for work: call() queues one request and
// blocks until the phone posts the answer. Calls block, so they belong on
// worker threads, never on the UI thread.
class PeerLink {
public:
    struct Reply {
        int status{0};
        QByteArray body;
    };

    struct Job {
        quint64 id{0};
        QByteArray request;
    };

    // Empty when the phone is gone, the swap ended, or the phone did not
    // answer within `timeout_ms`.
    std::optional<Reply> call(QJsonObject request, int timeout_ms = 30000);

    [[nodiscard]] bool linked() const;
    [[nodiscard]] QString name() const;

    // Share server side.
    void open(const QString& name);
    void close();
    std::optional<Job> next(int wait_ms);
    void answer(quint64 id, int status, QByteArray body);

private:
    struct Pending {
        bool done{false};
        Reply reply;
    };

    mutable std::mutex mutex_;
    std::condition_variable queued_;
    std::condition_variable answered_;
    bool open_{false};
    quint64 session_{0};
    quint64 nextId_{1};
    QString name_;
    std::deque<Job> queue_;
    std::map<quint64, Pending> pending_;
};

} // namespace videovault::app
