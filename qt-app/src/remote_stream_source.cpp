#include "videovault/app/remote_stream_source.hpp"
#include "videovault/app/proxy_settings.hpp"
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QSslSocket>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif
namespace videovault::app {
namespace {
constexpr qint64 kBlock = 64 * 1024;
constexpr qint64 kQueue = 1024 * 1024;
void erase(std::vector<char>& bytes) {
    volatile char* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
}
void erase(QByteArray& bytes) {
    volatile char* p = bytes.data();
    for (int i = 0; i < bytes.size(); ++i) p[i] = 0;
    bytes.clear();
}
bool loopback(const QString& host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}
}
// The Windows transport uses the OS TLS stack, not Qt5's unavailable OpenSSL1.1.
// A fixed-size mailbox is the only cross-thread payload; no queued per-byte Qt
// events or whole-body buffers. Redirects are returned to the owner for approval.
struct RemoteStreamSource::NativeRequest {
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic_bool cancelled{false};
#ifdef Q_OS_WIN
    std::atomic<HINTERNET> request{nullptr};
#endif
    bool headers = false, done = false;
    int status = 0;
    QByteArray range, length, validator;
    QString type, error;
    QUrl location;
    std::deque<QByteArray> blocks;
    qint64 queued = 0;
    ~NativeRequest() { for (auto& block : blocks) erase(block); }
};
RemoteStreamSource::RemoteStreamSource(QUrl url, Limits limits, QObject* parent)
    : QObject(parent), url_(std::move(url)), limits_(std::move(limits)) {
    limits_.buffer_bytes = std::clamp<qint64>(limits_.buffer_bytes, 256 * 1024, 128LL << 20);
    limits_.stall_timeout_ms = std::max(1000, limits_.stall_timeout_ms);
}
RemoteStreamSource::~RemoteStreamSource() {
    cancel();
    for (auto& chunk : chunks_) erase(chunk.data);
}
bool RemoteStreamSource::host_allowed(const QUrl& url) const {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    const auto host = url.host().toLower();
    if (url.scheme() != "https") {
        return !limits_.require_https && url.scheme() == "http" && loopback(host)
            && (limits_.allowed_hosts.isEmpty() || limits_.allowed_hosts.contains(host));
    }
    if (url.port(443) != 443 || host.isEmpty()) return false;
    for (const auto& allowed : limits_.allowed_hosts) {
        const auto entry = allowed.toLower();
        if (host == entry) return true;
        if (entry.startsWith("*.") && host.endsWith(entry.mid(1))) {
            const auto label = host.left(host.size() - entry.size() + 1);
            if (!label.isEmpty() && !label.contains('.')) return true;
        }
    }
    return false;
}
void RemoteStreamSource::start() {
    { std::lock_guard lock(mutex_); if (started_ || cancelled_) return; started_ = true; }
    network_ = new QNetworkAccessManager(this);
    configureNetworkProxy(network_);
    timer_ = new QTimer(this);
    timer_->setInterval(10);
    connect(timer_, &QTimer::timeout, this, [this] { pump(); });
    timer_->start();
    last_activity_ms_ = QDateTime::currentMSecsSinceEpoch();
    pump();
}
void RemoteStreamSource::set_sink(ByteSink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
    sink_offset_ = 0;
}
void RemoteStreamSource::set_sink_ready(SinkReady ready) { sink_ready_ = std::move(ready); }
void RemoteStreamSource::set_progress_callback(Progress cb) { progress_ = std::move(cb); }
void RemoteStreamSource::set_failure_callback(Failure cb) { failure_ = std::move(cb); }
bool RemoteStreamSource::transport_active() const { return reply_ != nullptr || native_ != nullptr; }
void RemoteStreamSource::pump() {
    bool stopped = false;
    { std::lock_guard lock(mutex_); stopped = cancelled_ || failed_; }
    if (stopped) { abort_request_now(); if (timer_) timer_->stop(); return; }
    if (!host_allowed(url_)) { fail("The provider stream URL/redirect is outside its approved HTTPS hosts."); return; }
    // Demand has priority over the import frontier. Once the playhead has bytes,
    // fill the missing import prefix. Never pass a seek gap to an append-only sink.
    const bool sinkReady = !sink_ready_ || sink_ready_();
    bool needPlayback = false, needImport = false;
    std::int64_t desired = -1;
    {
        std::lock_guard lock(mutex_);
        trim_locked();
        const auto end = total_ >= 0 ? total_ : eof_offset_;
        needPlayback = (end < 0 || position_ < end) && !covers_locked(position_);
        needImport = sink_ && sinkReady && (end < 0 || sink_offset_ < end);
        if (needPlayback) desired = position_;
        else if (needImport) desired = sink_offset_;
        else if (!sink_ && (end < 0 || stream_offset_ < end)
            && buffered_total_ < limits_.buffer_bytes / 2) desired = stream_offset_;
        if (limits_.require_flac && !flac_checked_) desired = 0;
    }
    if (transport_active()) {
        bool superseded = false, canRead = false;
        {
            std::lock_guard lock(mutex_);
            // Let a live response reach the requested playhead even if the
            // server ignored Range and we must discard its prefix.
            superseded = desired >= 0 && desired != stream_offset_
                && !(limits_.require_flac && !flac_checked_ && request_offset_ == 0)
                && !(needPlayback && stream_offset_ <= position_
                    && (position_ - stream_offset_ < kBlock || range_ignored_));
            canRead = (!sink_ || sinkReady || stream_offset_ > sink_offset_)
                && (needImport || needPlayback || buffered_total_ < limits_.buffer_bytes);
        }
        if (superseded) abort_request_now();
        else {
            if (!canRead) return; // bounded network + import queues, no UI wait
            if (native_) {
                const auto state = native_;
                bool headers = false, done = false;
                int status = 0; QByteArray range, length, block, validator; QString type, error; QUrl location;
                {
                    std::lock_guard lock(state->mutex);
                    headers = state->headers; status = state->status; range = state->range;
                    length = state->length; type = state->type; location = state->location; validator = state->validator;
                    if (!state->blocks.empty()) {
                        block = std::move(state->blocks.front()); state->blocks.pop_front();
                        state->queued -= block.size(); state->wake.notify_all();
                    }
                    done = state->done && state->blocks.empty(); error = state->error;
                }
                if (headers && !headers_done_ && !accept_headers(status, range, length, type, location, validator)) return;
                if (!block.isEmpty()) { accept_bytes(block.constData(), block.size()); erase(block); }
                if (done && state == native_) { native_.reset(); finish_response(error); }
            } else if (reply_) {
                auto* current = reply_;
                on_ready_read(current);
                if (current == reply_ && current->isFinished() && current->bytesAvailable() == 0)
                    on_finished(current);
            }
            if (transport_active() && QDateTime::currentMSecsSinceEpoch() - last_activity_ms_ > limits_.stall_timeout_ms)
                fail("The provider stream stalled while buffering.");
            return;
        }
    }
    if (desired >= 0) open_request(desired);
}
void RemoteStreamSource::open_request(std::int64_t offset) {
    { std::lock_guard lock(mutex_); request_offset_ = stream_offset_ = offset; headers_done_ = false; response_end_ = -1; }
    last_activity_ms_ = QDateTime::currentMSecsSinceEpoch();
#ifdef Q_OS_WIN
    const auto proxy = network_->proxy();
    const bool configuredProxy = proxyEnabled();
    if (configuredProxy && proxy.type() == QNetworkProxy::Socks5Proxy && url_.scheme() == "https" && !QSslSocket::supportsSsl()) {
        fail("SOCKS5 streaming requires a Qt-compatible TLS backend. Use an HTTP proxy or system VPN on this Windows build."); return;
    }
    if (!configuredProxy || proxy.type() == QNetworkProxy::HttpProxy) {
        const auto state = std::make_shared<NativeRequest>(); native_ = state;
        const auto url = url_;
        const auto validator = validator_;
        std::thread([state, url, offset, proxy, configuredProxy, validator] {
            struct Handle { HINTERNET h = nullptr; ~Handle() { if (h) WinHttpCloseHandle(h); } };
            auto finish = [&](QString error) {
                std::lock_guard lock(state->mutex); state->error = std::move(error); state->done = true;
            };
            const auto proxyName = proxy.hostName() + ":" + QString::number(proxy.port());
            Handle session{WinHttpOpen(L"MegaVaultProtect/2 (music-provider)",
                configuredProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                configuredProxy ? reinterpret_cast<LPCWSTR>(proxyName.utf16()) : WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
            if (!session.h) { finish("Windows TLS session could not start."); return; }
            WinHttpSetTimeouts(session.h, 8000, 8000, 10000, 10000);
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
            Handle connection{WinHttpConnect(session.h, reinterpret_cast<LPCWSTR>(url.host().utf16()), INTERNET_PORT(url.port(url.scheme() == "https" ? 443 : 80)), 0)};
            QString path = url.path(QUrl::FullyEncoded); if (path.isEmpty()) path = "/";
            if (url.hasQuery()) path += '?' + url.query(QUrl::FullyEncoded);
            Handle request{connection.h ? WinHttpOpenRequest(connection.h, L"GET", reinterpret_cast<LPCWSTR>(path.utf16()),
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.scheme() == "https" ? WINHTTP_FLAG_SECURE : 0) : nullptr};
            if (!request.h) { finish("Windows TLS request could not start."); return; }
            state->request = request.h;
            struct RequestGuard {
                Handle& handle; std::shared_ptr<NativeRequest> state;
                ~RequestGuard() { if (auto live = state->request.exchange(nullptr)) WinHttpCloseHandle(live); handle.h = nullptr; }
            } requestGuard{request, state};
            if (state->cancelled.load()) { finish({}); return; }
            DWORD disabled = WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
            if (configuredProxy && !proxy.user().isEmpty())
                WinHttpSetCredentials(request.h, WINHTTP_AUTH_TARGET_PROXY, WINHTTP_AUTH_SCHEME_BASIC,
                    reinterpret_cast<LPCWSTR>(proxy.user().utf16()), reinterpret_cast<LPCWSTR>(proxy.password().utf16()), nullptr);
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            auto headers = QStringLiteral("Range: bytes=%1-\r\nAccept-Encoding: identity\r\n").arg(offset);
            if (!validator.isEmpty()) headers += "If-Range: " + QString::fromLatin1(validator) + "\r\n";
            if (!WinHttpSendRequest(request.h, reinterpret_cast<LPCWSTR>(headers.utf16()), headers.size(),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) {
                finish(QStringLiteral("Windows HTTPS transport failed (%1).").arg(GetLastError())); return;
            }
            auto header = [&](DWORD field) {
                DWORD size = 0;
                WinHttpQueryHeaders(request.h, field, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
                if (!size) return QString();
                std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
                if (!WinHttpQueryHeaders(request.h, field, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size, WINHTTP_NO_HEADER_INDEX)) return QString();
                return QString::fromWCharArray(text.c_str());
            };
            const int status = header(WINHTTP_QUERY_STATUS_CODE).toInt();
            {
                std::lock_guard lock(state->mutex);
                state->status = status; state->range = header(WINHTTP_QUERY_CONTENT_RANGE).toLatin1();
                state->length = header(WINHTTP_QUERY_CONTENT_LENGTH).toLatin1();
                state->validator = header(WINHTTP_QUERY_ETAG).toLatin1();
                if (state->validator.isEmpty()) state->validator = header(WINHTTP_QUERY_LAST_MODIFIED).toLatin1();
                state->type = header(WINHTTP_QUERY_CONTENT_TYPE);
                state->location = QUrl(header(WINHTTP_QUERY_LOCATION)); state->headers = true;
            }
            if (status != 200 && status != 206) { finish({}); return; }
            while (!state->cancelled.load()) {
                {
                    std::unique_lock lock(state->mutex);
                    state->wake.wait(lock, [&] { return state->cancelled.load() || state->queued < 2 * kBlock; });
                    if (state->cancelled.load()) break;
                }
                QByteArray block(static_cast<int>(kBlock), Qt::Uninitialized); DWORD got = 0;
                if (!WinHttpReadData(request.h, block.data(), static_cast<DWORD>(block.size()), &got)) {
                    finish(QStringLiteral("Windows HTTPS read failed (%1).").arg(GetLastError())); return;
                }
                if (!got) break;
                block.resize(static_cast<int>(got));
                std::lock_guard lock(state->mutex); state->queued += got; state->blocks.push_back(std::move(block));
            }
            finish({});
        }).detach();
        return;
    }
#endif
    QNetworkRequest request(url_);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Range", "bytes=" + QByteArray::number(static_cast<qint64>(offset)) + "-");
    if (!validator_.isEmpty()) request.setRawHeader("If-Range", validator_);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "MegaVaultProtect/2 (music-provider)");
    reply_ = network_->get(request); reply_->setReadBufferSize(2 * kBlock);
    auto* reply = reply_;
    connect(reply, &QNetworkReply::readyRead, this, [this] { pump(); });
    connect(reply, &QNetworkReply::finished, this, [this] { pump(); });
}
bool RemoteStreamSource::accept_headers(int status, const QByteArray& range, const QByteArray& length,
    const QString& type, const QUrl& location, const QByteArray& validator) {
    if (status >= 300 && status < 400) {
        const auto next = url_.resolved(location);
        if (location.isEmpty() || ++redirects_ > 5 || !host_allowed(next)) {
            fail("The provider redirect is not approved."); return false;
        }
        abort_request_now(); url_ = next; return false;
    }
    if (status != 200 && status != 206) { fail(QStringLiteral("The provider stream failed (HTTP %1).").arg(status)); return false; }
    bool lengthOk = false; const auto size = length.toLongLong(&lengthOk);
    std::int64_t total = -1, start = 0, end = -1;
    if (status == 206) {
        const auto match = QRegularExpression("^bytes ([0-9]+)-([0-9]+)/([0-9]+|\\*)$").match(QString::fromLatin1(range));
        bool a = false, b = false, c = true;
        if (match.hasMatch()) {
            start = match.captured(1).toLongLong(&a); end = match.captured(2).toLongLong(&b);
            if (match.captured(3) != "*") total = match.captured(3).toLongLong(&c);
        }
        if (!a || !b || !c || start != request_offset_ || end < start
            || end == std::numeric_limits<std::int64_t>::max() || (total >= 0 && end >= total)
            || (lengthOk && size != end - start + 1)) {
            fail("The provider returned an invalid or mismatched Content-Range."); return false;
        }
    } else if (lengthOk && size >= 0) total = size;
    bool invalid = false;
    {
        std::lock_guard lock(mutex_);
        invalid = (!validator_.isEmpty() && validator != validator_)
            || (total_ >= 0 && total >= 0 && total != total_) || (limits_.max_bytes > 0 && total > limits_.max_bytes);
        if (!invalid) {
            if (total >= 0) total_ = total;
            if (validator_.isEmpty()) validator_ = validator;
            stream_offset_ = start; response_end_ = status == 206 ? end + 1 : total;
            range_ignored_ = range_ignored_ || status == 200;
            content_type_ = type; final_url_ = url_; headers_done_ = true;
        }
    }
    if (invalid || total == 0 || (limits_.require_flac && total >= 0 && total < 42)) { fail("The provider returned an empty stream, changed its length or exceeded the size limit."); return false; }
    last_activity_ms_ = QDateTime::currentMSecsSinceEpoch();
    return true;
}
void RemoteStreamSource::on_ready_read(QNetworkReply* reply) {
    if (!reply || reply != reply_) return;
    if (!headers_done_) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!status) {
            if (reply->isFinished()) finish_response(reply->errorString());
            return;
        }
        if (!accept_headers(status, reply->rawHeader("Content-Range"), reply->rawHeader("Content-Length"),
            reply->header(QNetworkRequest::ContentTypeHeader).toString(),
            reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl(),
            reply->hasRawHeader("ETag") ? reply->rawHeader("ETag") : reply->rawHeader("Last-Modified"))) return;
    }
    auto bytes = reply->read(std::min<qint64>(kBlock, reply->bytesAvailable()));
    if (!bytes.isEmpty()) { accept_bytes(bytes.constData(), bytes.size()); erase(bytes); }
}
void RemoteStreamSource::accept_bytes(const char* data, qint64 size) {
    ByteSink sink; std::int64_t offset = 0, tapOffset = 0; qint64 skip = 0;
    bool invalid = false; Progress progress; qint64 received = 0, total = -1;
    {
        std::lock_guard lock(mutex_);
        if (cancelled_ || failed_) return;
        offset = stream_offset_;
        invalid = size < 0 || offset > std::numeric_limits<std::int64_t>::max() - size
            || (response_end_ >= 0 && size > response_end_ - offset)
            || (limits_.max_bytes > 0 && size > limits_.max_bytes - offset);
        if (!invalid) {
            stream_offset_ += size; received_ += size;
            if (limits_.require_flac && !flac_checked_ && offset == flac_header_.size()) {
                flac_header_.append(data, static_cast<int>(std::min<qint64>(size, 42 - flac_header_.size())));
                if (flac_header_.size() == 42) {
                    flac_checked_ = flac_header_.startsWith("fLaC")
                        && (static_cast<unsigned char>(flac_header_[4]) & 0x7f) == 0
                        && flac_header_.at(5) == 0 && flac_header_.at(6) == 0 && flac_header_.at(7) == 34;
                    invalid = !flac_checked_;
                }
            }
            // Keep only the playhead window. Import-prefetch bytes behind/ahead
            // of it go directly to the tap, not into an unbounded sparse cache.
            const auto begin = std::max(offset, position_);
            // Qt's qint64 is `long long` while int64_t is `long` on Linux, so the
            // template arguments must be pinned to one type explicitly.
            const auto stop = std::min<std::int64_t>(
                stream_offset_, position_ + static_cast<std::int64_t>(limits_.buffer_bytes));
            if (stop > begin && !covers_locked(begin)) {
                const auto room = limits_.buffer_bytes - buffered_total_;
                const auto count = std::min<std::int64_t>(stop - begin, room);
                if (count > 0) {
                    Chunk chunk; chunk.offset = begin;
                    chunk.data.assign(data + (begin - offset), data + (begin - offset) + count);
                    buffered_total_ += count; chunks_.push_back(std::move(chunk));
                }
            }
            if (sink_ && offset <= sink_offset_ && stream_offset_ > sink_offset_) {
                skip = sink_offset_ - offset; tapOffset = sink_offset_; sink_offset_ = stream_offset_; sink = sink_;
            }
            complete_ = total_ >= 0 && stream_offset_ == total_;
            received = sink_ ? sink_offset_ : received_; total = total_; progress = progress_; wake_.notify_all();
        }
    }
    if (invalid) { fail("The provider sent bytes outside the declared stream bounds."); return; }
    last_activity_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (sink) sink(tapOffset, data + skip, static_cast<std::ptrdiff_t>(size - skip));
    if (progress) progress(received, total);
}
void RemoteStreamSource::on_finished(QNetworkReply* reply) {
    if (!reply || reply != reply_) return;
    const auto error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply_ = nullptr; reply->deleteLater(); finish_response(error);
}
void RemoteStreamSource::finish_response(const QString& error) {
    bool shortBody = false;
    {
        std::lock_guard lock(mutex_);
        shortBody = response_end_ >= 0 && stream_offset_ != response_end_;
        if (error.isEmpty() && !shortBody) {
            if (total_ < 0 && response_end_ < 0) eof_offset_ = stream_offset_;
            complete_ = total_ < 0 ? eof_offset_ >= 0 : stream_offset_ == total_;
        }
        request_offset_ = -1; wake_.notify_all();
    }
    if (!error.isEmpty() || shortBody) fail(error.isEmpty() ? "The provider stream ended before its declared length." : error);
}
void RemoteStreamSource::abort_request_now() {
    if (native_) {
        native_->cancelled = true;
#ifdef Q_OS_WIN
        if (auto request = native_->request.exchange(nullptr)) WinHttpCloseHandle(request);
#endif
        native_->wake.notify_all(); native_.reset();
    }
    if (reply_) {
        auto* reply = reply_; reply_ = nullptr; reply->disconnect(this); reply->abort(); reply->deleteLater();
    }
    std::lock_guard lock(mutex_); request_offset_ = -1;
}
void RemoteStreamSource::fail(const QString& message) {
    { std::lock_guard lock(mutex_); if (failed_ || cancelled_) return; failed_ = true; complete_ = false; error_ = message; wake_.notify_all(); }
    abort_request_now(); if (failure_) failure_(message);
}
void RemoteStreamSource::cancel() {
    {
        std::lock_guard lock(mutex_); cancelled_ = true;
        for (auto& chunk : chunks_) erase(chunk.data);
        chunks_.clear(); buffered_total_ = 0; erase(flac_header_); wake_.notify_all();
    }
    if (QThread::currentThread() == thread()) { abort_request_now(); if (timer_) timer_->stop(); }
    else QMetaObject::invokeMethod(this, [this] { pump(); }, Qt::QueuedConnection);
}
bool RemoteStreamSource::covers_locked(std::int64_t offset) const {
    for (const auto& c : chunks_) if (offset >= c.offset && offset < c.end()) return true;
    return false;
}
void RemoteStreamSource::trim_locked() {
    // Preserve a small backseek window, but release consumed chunks before the
    // buffer reaches its ceiling (the previous implementation never did).
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (it->end() <= position_ - kBlock || buffered_total_ > limits_.buffer_bytes) {
            buffered_total_ -= it->data.size(); erase(it->data); it = chunks_.erase(it);
        } else ++it;
    }
}
int RemoteStreamSource::take_locked(unsigned char* buffer, int size) {
    for (const auto& c : chunks_) {
        if (position_ < c.offset || position_ >= c.end()) continue;
        const auto got = static_cast<int>(std::min<std::int64_t>(size, c.end() - position_));
        std::memcpy(buffer, c.data.data() + position_ - c.offset, got);
        position_ += got; served_ += got; trim_locked(); return got;
    }
    return 0;
}
int RemoteStreamSource::read(unsigned char* buffer, int size) {
    if (!buffer || size <= 0) return 0;
    std::unique_lock lock(mutex_);
    auto last = std::chrono::steady_clock::now(); auto received = received_;
    for (;;) {
        if (cancelled_) return 0;
        if (failed_) return -1;
        if (!limits_.require_flac || flac_checked_)
            if (const int got = take_locked(buffer, size)) { reader_waiting_ = false; return got; }
        const auto end = total_ >= 0 ? total_ : eof_offset_;
        if (end >= 0 && position_ >= end) { reader_waiting_ = false; return 0; }
        reader_waiting_ = true;
        if (received != received_) { received = received_; last = std::chrono::steady_clock::now(); }
        if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(limits_.stall_timeout_ms)) {
            // Owner reports the failure/callback on the next pump, never call
            // QObject/network methods from the decoder thread.
            lock.unlock();
            QMetaObject::invokeMethod(this, [this] { fail("The provider stream stalled while buffering."); }, Qt::QueuedConnection);
            return -1;
        }
        wake_.wait_for(lock, std::chrono::milliseconds(50));
    }
}
std::int64_t RemoteStreamSource::seek(std::int64_t offset, int whence) {
    std::lock_guard lock(mutex_);
    if (whence == kSeekSize) return total_;
    std::int64_t base = 0;
    if (whence == SEEK_CUR) base = position_;
    else if (whence == SEEK_END) { if (total_ < 0) return -1; base = total_; }
    else if (whence != SEEK_SET) return -1;
    if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset)
        || (offset < 0 && offset < -base)) return -1;
    position_ = std::max<std::int64_t>(0, base + offset);
    if (total_ >= 0) position_ = std::min(position_, total_);
    // An old window cannot prevent a missing seek target from being cached.
    if (!covers_locked(position_)) {
        for (auto& c : chunks_) erase(c.data);
        chunks_.clear(); buffered_total_ = 0;
    }
    wake_.notify_all(); return position_;
}
std::int64_t RemoteStreamSource::size() const { std::lock_guard lock(mutex_); return total_; }
void RemoteStreamSource::set_buffer_budget(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    limits_.buffer_bytes = static_cast<qint64>(std::clamp<std::size_t>(bytes, 256U << 10, 128U << 20)); trim_locked();
}
QString RemoteStreamSource::contentType() const { std::lock_guard lock(mutex_); return content_type_; }
QUrl RemoteStreamSource::finalUrl() const { std::lock_guard lock(mutex_); return final_url_; }
bool RemoteStreamSource::honours_ranges() const { std::lock_guard lock(mutex_); return !range_ignored_; }
bool RemoteStreamSource::failed() const { std::lock_guard lock(mutex_); return failed_; }
bool RemoteStreamSource::complete() const { std::lock_guard lock(mutex_); return complete_; }
QString RemoteStreamSource::errorString() const { std::lock_guard lock(mutex_); return error_; }
qint64 RemoteStreamSource::received_bytes() const { std::lock_guard lock(mutex_); return received_; }
qint64 RemoteStreamSource::buffered_bytes() const { std::lock_guard lock(mutex_); return buffered_total_; }
qint64 RemoteStreamSource::bytes_served() const { std::lock_guard lock(mutex_); return served_; }

struct RemoteStreamImport::State {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::vector<char>> queue;
    qint64 queued = 0, expected = -1;
    bool cancelled = false, finished = false, committed = false;
    std::int64_t id = 0;
    QString error;
};
RemoteStreamImport::RemoteStreamImport(RemoteStreamSource& source, std::shared_ptr<core::Vault> vault,
    QString name, std::int64_t folder, core::TrackProvenance provenance, bool requireFlac)
    : state_(std::make_shared<State>()) {
    const auto state = state_;
    source.set_sink_ready([state] {
        std::lock_guard lock(state->mutex);
        return state->finished || state->cancelled || state->queued <= kQueue - kBlock;
    });
    source.set_sink([state, &source](std::int64_t, const char* bytes, std::ptrdiff_t size) {
        std::lock_guard lock(state->mutex);
        if (state->cancelled || state->finished) return;
        state->expected = source.size();
        if (state->expected < 0) {
            state->error = "Offline saving requires a known stream length. Playback can continue.";
            state->cancelled = true;
        } else {
            state->queue.emplace_back(bytes, bytes + size); state->queued += size;
        }
        state->wake.notify_all();
    });
    std::thread([state, vault = std::move(vault), name = std::move(name), folder,
        provenance = std::move(provenance), requireFlac] {
        core::StreamImport import;
        std::vector<char> prefix;
        auto finish = [&](QString error, bool success = false, std::int64_t id = 0) {
            // Cleanup stays on the worker, including abort/remove staging.
            if (!success) import.abort();
            erase(prefix);
            std::lock_guard lock(state->mutex);
            for (auto& bytes : state->queue) erase(bytes);
            state->queue.clear(); state->queued = 0; state->finished = true;
            state->committed = success; state->id = id;
            if (state->error.isEmpty()) state->error = std::move(error);
        };
        for (;;) {
            std::vector<char> bytes; qint64 expected = -1; bool cancelled = false;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return state->cancelled || !state->queue.empty(); });
                cancelled = state->cancelled || !vault || !vault->is_unlocked();
                if (!state->queue.empty()) {
                    bytes = std::move(state->queue.front()); state->queue.pop_front(); state->queued -= bytes.size();
                }
                expected = state->expected;
            }
            if (cancelled) { erase(bytes); finish("Offline save cancelled or vault locked."); return; }
            if (bytes.empty()) continue;
            if (!import.active()) {
                prefix.insert(prefix.end(), bytes.begin(), bytes.end()); erase(bytes); bytes.clear();
                if (requireFlac && prefix.size() < 42) {
                    if (expected >= 0 && prefix.size() >= static_cast<std::size_t>(expected)) { finish("The FLAC header is truncated."); return; }
                    continue;
                }
                if (requireFlac && (std::memcmp(prefix.data(), "fLaC", 4) != 0
                    || (static_cast<unsigned char>(prefix[4]) & 0x7f) != 0
                    || prefix[5] != 0 || prefix[6] != 0 || prefix[7] != 34)) {
                    finish("The provider did not return a valid FLAC stream header."); return;
                }
                auto begun = vault->begin_stream_import(name.toUtf8().toStdString(), static_cast<std::uint64_t>(expected), folder, &provenance);
                if (!begun) { finish(QString::fromUtf8(core::user_message(begun.error().code).data())); return; }
                import = std::move(begun.value()); bytes.swap(prefix);
            }
            auto stored = import.append(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
            erase(bytes);
            if (!stored) { finish(QString::fromUtf8(core::user_message(stored.error().code).data())); return; }
            if (import.written() == import.expected_size()) {
                { std::lock_guard lock(state->mutex); cancelled = state->cancelled; }
                if (cancelled) { finish("Offline save cancelled."); return; }
                const auto result = import.commit();
                if (!result) finish(QString::fromUtf8(core::user_message(result.error().code).data()));
                else finish({}, true, result.value());
                return;
            }
        }
    }).detach();
}
RemoteStreamImport::~RemoteStreamImport() { cancel(); }
void RemoteStreamImport::cancel() { std::lock_guard lock(state_->mutex); state_->cancelled = true; state_->wake.notify_all(); }
bool RemoteStreamImport::finished() const { std::lock_guard lock(state_->mutex); return state_->finished; }
bool RemoteStreamImport::committed() const { std::lock_guard lock(state_->mutex); return state_->committed; }
std::int64_t RemoteStreamImport::videoId() const { std::lock_guard lock(state_->mutex); return state_->id; }
QString RemoteStreamImport::errorString() const { std::lock_guard lock(state_->mutex); return state_->error; }
qint64 RemoteStreamImport::queuedBytes() const { std::lock_guard lock(state_->mutex); return state_->queued; }
} // namespace videovault::app
