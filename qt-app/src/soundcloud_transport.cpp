#include "videovault/app/soundcloud_transport.hpp"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <thread>
#include <chrono>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#endif

namespace videovault::app {
struct SoundCloudTransport::Impl {
    bool fixture;
#ifdef Q_OS_WIN
    HINTERNET session{nullptr};
#endif
    explicit Impl(bool value) : fixture(value) {
#ifdef Q_OS_WIN
        session = WinHttpOpen(L"MegaVaultProtect/1.0 (music-provider)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) WinHttpSetTimeouts(session, 1000, 1000, 1000, 1000);
#endif
    }
    ~Impl() {
#ifdef Q_OS_WIN
        if (session) WinHttpCloseHandle(session);
#endif
    }
};
SoundCloudTransport::SoundCloudTransport(bool fixture) : impl_(std::make_unique<Impl>(fixture)) {}
SoundCloudTransport::~SoundCloudTransport() = default;
bool SoundCloudTransport::permits(const QUrl& url, bool fixture) {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    if (fixture && url.scheme() == "http" && url.host() == "127.0.0.1" && url.port() > 0) return true;
    if (url.scheme() != "https" || url.port(443) != 443) return false;
    const auto host = url.host().toLower();
    if (host == "api-v2.soundcloud.com" || host == "api.soundcloud.com") return true;
    // The manifest's wildcard matches exactly one label, not bare/multi-label hosts.
    return host.endsWith(".sndcdn.com") && host.count('.') == 2 && !host.startsWith('.');
}
SoundCloudTransport::Result SoundCloudTransport::get(QUrl url, int limit,
    const std::atomic<bool>& cancelled, bool redirect_target_only) {
    Result result;
    QElapsedTimer elapsed; elapsed.start();
    for (int redirects = 0; redirects <= 5; ++redirects) {
        result.body.clear(); result.status = 0; result.final_url = url;
        if (cancelled || elapsed.elapsed() > 15000) { result.error = "Request cancelled or timed out."; return result; }
        if (!permits(url, impl_->fixture)) { result.error = "SoundCloud URL is outside the permitted HTTPS hosts."; return result; }
        QUrl redirect;
#ifdef Q_OS_WIN
        struct Handle { HINTERNET h; ~Handle() { if (h) WinHttpCloseHandle(h); } };
        if (!impl_->session) { result.error = "Cannot create the system TLS session."; return result; }
        Handle connection{WinHttpConnect(impl_->session, reinterpret_cast<LPCWSTR>(url.host().utf16()),
            INTERNET_PORT(url.port(url.scheme() == "https" ? 443 : 80)), 0)};
        const auto path = url.path(QUrl::FullyEncoded) + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded) : "");
        Handle request{connection.h ? WinHttpOpenRequest(connection.h, L"GET", reinterpret_cast<LPCWSTR>(path.utf16()),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.scheme() == "https" ? WINHTTP_FLAG_SECURE : 0) : nullptr};
        // Closing a request is WinHTTP's documented cancellation mechanism. The
        // monitor is joined before stack/connection/session destruction and the
        // request handle is exchanged exactly once, including every error exit.
        struct AbortMonitor {
            Handle& request;
            std::atomic<HINTERNET> live;
            std::atomic<bool> done{false};
            std::thread thread;
            AbortMonitor(Handle& handle, const std::atomic<bool>& cancel, qint64 remaining)
                : request(handle), live(handle.h), thread([this,&cancel,remaining] {
                    QElapsedTimer timer; timer.start();
                    while (!done && !cancel && timer.elapsed() < remaining) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (!done) if (auto h = live.exchange(nullptr)) WinHttpCloseHandle(h);
                }) {}
            ~AbortMonitor() { done = true; thread.join(); request.h = live.exchange(nullptr); }
        } abort(request,cancelled,std::max<qint64>(0,15000-elapsed.elapsed()));
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
        if (!request.h || !WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))
            || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
            || cancelled || !WinHttpReceiveResponse(request.h, nullptr)) {
            result.error = "SoundCloud request failed (system TLS/HTTP)."; return result;
        }
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            result.error = "Missing HTTP status."; return result;
        }
        result.status = int(status);
        if (status >= 300 && status < 400) {
            wchar_t location[8192]; DWORD bytes = sizeof(location);
            if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                location, &bytes, WINHTTP_NO_HEADER_INDEX)) { result.error = "Missing redirect target."; return result; }
            redirect = url.resolved(QUrl(QString::fromWCharArray(location)));
        } else if (status == 200) {
            char buffer[32768];
            for (;;) {
                if (cancelled || elapsed.elapsed() > 15000) { result.error = "Request cancelled or timed out."; return result; }
                DWORD read = 0;
                if (!WinHttpReadData(request.h, buffer, sizeof(buffer), &read)) { result.error = "Truncated HTTP response."; return result; }
                if (!read) break;
                if (result.body.size() > limit - int(read)) { result.error = "SoundCloud response exceeded its memory limit."; return result; }
                result.body.append(buffer, int(read));
            }
        }
#else
        // One manager per worker thread keeps TLS connections reusable without crossing Qt affinity.
        thread_local QNetworkAccessManager manager;
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, "MegaVaultProtect/1.0 (music-provider)");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        auto* reply = manager.get(request);
        reply->setReadBufferSize(32768);
        QEventLoop loop; QTimer tick; tick.setInterval(25);
        QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
            if (cancelled || elapsed.elapsed() > 15000) { result.error = "Request cancelled or timed out."; reply->abort(); }
        });
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            const auto chunk = reply->read(32768);
            if (result.body.size() > limit - chunk.size()) { result.error = "SoundCloud response exceeded its memory limit."; reply->abort(); }
            else result.body.append(chunk);
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        tick.start(); loop.exec();
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        redirect = url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        if (result.status < 300 || result.status >= 400) redirect = {};
        if (reply->error() != QNetworkReply::NoError && result.error.isEmpty()) result.error = reply->errorString();
        delete reply;
        if (!result.error.isEmpty()) return result;
#endif
        if (redirect.isEmpty()) {
            if (result.status != 200) result.error = QStringLiteral("SoundCloud HTTP %1: access refused or service unavailable.").arg(result.status);
            return result;
        }
        if (redirect_target_only) {
            if (!permits(redirect, impl_->fixture)) { result.error = "Download redirect host is not permitted."; return result; }
            result.status = 200;
            result.body = QJsonDocument(QJsonObject{{QStringLiteral("redirectUri"),redirect.toString(QUrl::FullyEncoded)}}).toJson(QJsonDocument::Compact);
            return result;
        }
        url = redirect; // validated before any next-hop request; no credentials forwarded.
    }
    result.error = "Too many SoundCloud redirects."; return result;
}
} // namespace videovault::app
