#include "videovault/app/peer_link.hpp"

#include <QJsonDocument>

#include <algorithm>

namespace videovault::app {

std::optional<PeerLink::Reply> PeerLink::call(QJsonObject request, const int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!open_) {
        return std::nullopt;
    }
    const quint64 id = nextId_++;
    const quint64 session = session_;
    request.insert(QStringLiteral("req"), static_cast<double>(id));
    queue_.push_back(Job{id, QJsonDocument(request).toJson(QJsonDocument::Compact)});
    pending_[id] = Pending{};
    queued_.notify_one();

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(1, timeout_ms));
    answered_.wait_until(lock, deadline, [this, id, session] {
        const auto found = pending_.find(id);
        return (found != pending_.end() && found->second.done)
            || !open_ || session_ != session;
    });

    std::optional<Reply> out;
    const auto found = pending_.find(id);
    if (found != pending_.end() && found->second.done && session_ == session) {
        out = std::move(found->second.reply);
    }
    pending_.erase(id);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                     [id](const Job& job) { return job.id == id; }),
        queue_.end());
    return out;
}

bool PeerLink::linked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

QString PeerLink::name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
}

void PeerLink::open(const QString& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++session_;
    queue_.clear();
    open_ = true;
    name_ = name;
    answered_.notify_all();
    queued_.notify_all();
}

void PeerLink::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++session_;
    queue_.clear();
    open_ = false;
    name_.clear();
    answered_.notify_all();
    queued_.notify_all();
}

std::optional<PeerLink::Job> PeerLink::next(const int wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const quint64 session = session_;
    queued_.wait_for(lock, std::chrono::milliseconds(std::max(1, wait_ms)), [this, session] {
        return !queue_.empty() || !open_ || session_ != session;
    });
    if (!open_ || session_ != session || queue_.empty()) {
        return std::nullopt;
    }
    Job job = std::move(queue_.front());
    queue_.pop_front();
    return job;
}

void PeerLink::answer(const quint64 id, const int status, QByteArray body) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pending_.find(id);
    if (found == pending_.end() || found->second.done) {
        return;
    }
    found->second.done = true;
    found->second.reply = Reply{status, std::move(body)};
    answered_.notify_all();
}

} // namespace videovault::app
