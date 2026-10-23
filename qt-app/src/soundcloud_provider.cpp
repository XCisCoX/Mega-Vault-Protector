#include "videovault/app/soundcloud_provider.hpp"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>
#include <QUrlQuery>
#include <cmath>

#include <algorithm>

namespace videovault::app {
namespace {

constexpr int kDefaultLimit = 20;

QString truncated(const QString& value, const int max) {
    return value.size() > max ? value.left(max) : value;
}

QString number_to_id(const QJsonValue& value) {
    if (value.isString()) {
        const auto id = value.toString();
        static const QRegularExpression decimal(QStringLiteral("^[1-9][0-9]{0,119}$"));
        return decimal.match(id).hasMatch() ? id : QString{};
    }
    if (value.isDouble()) {
        const auto number = value.toDouble();
        if (!std::isfinite(number) || number < 1 || number > 9007199254740991.0 || std::floor(number) != number) return {};
        return QString::number(static_cast<qint64>(number));
    }
    return {};
}

QString artist_of(const QJsonObject& object) {
    const auto user = object.value("user").toObject();
    auto name = user.value("username").toString();
    if (name.isEmpty()) {
        name = user.value("permalink").toString();
    }
    if (name.isEmpty()) {
        name = object.value("username").toString();
    }
    return truncated(name, 300);
}

// SoundCloud returns 403/"preview" markers in several shapes; all of them mean
// the track is not fully usable.
bool is_preview(const QJsonObject& object) {
    const auto policy = object.value("policy").toString().toUpper();
    const auto access = object.value("access").toString().toLower();
    return policy == QStringLiteral("SNIP") || policy == QStringLiteral("PREVIEW")
        || access == QStringLiteral("preview") || access.startsWith(QStringLiteral("snip"));
}

bool is_blocked(const QJsonObject& object) {
    const auto policy = object.value("policy").toString().toUpper();
    const auto access = object.value("access").toString().toLower();
    return policy == QStringLiteral("BLOCK") || policy == QStringLiteral("BLOCKED")
        || access == QStringLiteral("block") || access == QStringLiteral("blocked");
}

bool is_geo_blocked(const QJsonObject& object) {
    return object.value("geo_blocked").toBool(false)
        || object.value("streamable").toBool(true) == false
               && object.value("policy").toString().isEmpty()
               && object.value("access").toString().isEmpty();
}

} // namespace

QString SoundCloudProvider::searchTracksPath(const QString& query, int limit) {
    const auto capped = std::clamp(limit > 0 ? limit : kDefaultLimit, 1, 50);
    return QStringLiteral("/search/tracks?q=%1&limit=%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(query, 240))))
        .arg(capped);
}

QString SoundCloudProvider::searchUsersPath(const QString& query, int limit) {
    const auto capped = std::clamp(limit > 0 ? limit : kDefaultLimit, 1, 50);
    return QStringLiteral("/search/users?q=%1&limit=%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(query, 240))))
        .arg(capped);
}

QString SoundCloudProvider::searchPlaylistsPath(const QString& query, int limit) {
    const auto capped = std::clamp(limit > 0 ? limit : kDefaultLimit, 1, 50);
    return QStringLiteral("/search/playlists?q=%1&limit=%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(query, 240))))
        .arg(capped);
}

QString SoundCloudProvider::resolvePath(const QString& permalinkUrl) {
    return QStringLiteral("/resolve?url=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(permalinkUrl, 2048))));
}

QString SoundCloudProvider::userTracksPath(const QString& userIdOrUrl, int limit) {
    const auto capped = std::clamp(limit > 0 ? limit : kDefaultLimit, 1, 200);
    // A pasted profile URL is resolved first, so the caller can pass either.
    if (userIdOrUrl.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) {
        return QStringLiteral("/resolve?url=%1")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(userIdOrUrl, 2048))));
    }
    return QStringLiteral("/users/%1/tracks?limit=%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(userIdOrUrl, 120))))
        .arg(capped);
}

QString SoundCloudProvider::playlistPath(const QString& playlistId) {
    return QStringLiteral("/playlists/%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(playlistId, 120))));
}

QString SoundCloudProvider::downloadPath(const QString& trackId) {
    return QStringLiteral("/tracks/%1/download")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(truncated(trackId, 120))));
}

QUrl SoundCloudProvider::withClientId(const QUrl& url, const QString& clientId) {
    if (clientId.isEmpty()) {
        return url;
    }
    auto copy = url;
    QUrlQuery query(copy);
    if (!query.hasQueryItem(QStringLiteral("client_id"))) query.addQueryItem(QStringLiteral("client_id"), clientId);
    copy.setQuery(query);
    return copy;
}

QString SoundCloudProvider::rights_message(const Rights rights) {
    switch (rights) {
    case Rights::None:
        return {};
    case Rights::NotStreamable:
        return QStringLiteral("SoundCloud does not allow streaming this track.");
    case Rights::Blocked:
        return QStringLiteral("SoundCloud has blocked this track in this region.");
    case Rights::GeoBlocked:
        return QStringLiteral("This track is not available in your country.");
    case Rights::PreviewOnly:
        return QStringLiteral("Only a preview is available; the full track needs "
                              "SoundCloud Go+ or the artist's permission.");
    case Rights::NotDownloadable:
        return QStringLiteral("The artist has not enabled downloads, so this track "
                              "streams but cannot be stored in the vault.");
    case Rights::Unknown:
        break;
    }
    return QStringLiteral("This track cannot be used.");
}

SoundCloudProvider::Rights SoundCloudProvider::playback_rights(const Track& track) {
    if (track.blocked) {
        return Rights::Blocked;
    }
    if (track.geo_blocked) {
        return Rights::GeoBlocked;
    }
    if (track.preview_only) {
        return Rights::PreviewOnly;
    }
    if (track.access_unknown) return Rights::Unknown;
    if (!track.streamable) {
        return Rights::NotStreamable;
    }
    return Rights::None;
}

SoundCloudProvider::Rights SoundCloudProvider::download_rights(const Track& track) {
    const auto playable = playback_rights(track);
    if (playable != Rights::None) {
        // A preview or blocked track is never saved, even when the stream exists.
        return playable;
    }
    if (!track.downloadable || track.downloads_exhausted) {
        return Rights::NotDownloadable;
    }
    return Rights::None;
}

bool SoundCloudProvider::parse_track(const QJsonObject& object, Track* track) {
    if (track == nullptr) {
        return false;
    }
    const auto id = number_to_id(object.value("id"));
    if (id.isEmpty()) {
        return false;
    }
    Track parsed;
    parsed.id = truncated(id, 120);
    parsed.title = truncated(object.value("title").toString(), 400);
    if (parsed.title.isEmpty()) {
        parsed.title = QStringLiteral("Untitled");
    }
    parsed.artist = artist_of(object);
    parsed.artwork_url = object.value("artwork_url").toString();
    if (parsed.artwork_url.isEmpty()) {
        // Playlists and some uploads carry the picture on the user instead.
        parsed.artwork_url = object.value("user").toObject().value("avatar_url").toString();
    }
    parsed.permalink_url = object.value("permalink_url").toString();
    const auto duration = object.value("duration").toDouble(0);
    parsed.duration_ms = std::isfinite(duration) ? static_cast<qint64>(std::clamp(duration, 0.0, 86400000.0)) : 0;
    parsed.streamable = object.value("streamable").toBool(false);
    parsed.downloadable = object.value("downloadable").toBool(false);
    parsed.downloads_exhausted = object.contains("has_downloads_left")
        && !object.value("has_downloads_left").toBool(true);
    parsed.preview_only = is_preview(object);
    parsed.blocked = is_blocked(object);
    parsed.geo_blocked = object.value("geo_blocked").toBool(false);
    const auto access = object.value("access").toString().toLower();
    const auto policy = object.value("policy").toString().toUpper();
    parsed.access_unknown = (!access.isEmpty() && access != "playable" && access != "full"
        && !parsed.preview_only && !parsed.blocked) || (!policy.isEmpty() && policy != "ALLOW"
        && !parsed.preview_only && !parsed.blocked);
    parsed.track_authorization = object.value("track_authorization").toString();

    // Choose the best progressive transcoding, if the API exposed one.
    // A rendition can be a snippet even when the track-level flags say nothing:
    // yt-dlp treats `snipped` and preview media URLs as preview evidence, and a
    // restricted rendition must never be played or saved as if it were the whole
    // track. One such rendition marks the track preview-only.
    const auto transcodings = object.value("media").toObject().value("transcodings").toArray();
    for (const auto& entry : transcodings) {
        const auto transcoding = entry.toObject();
        if (transcoding.value("snipped").toBool(false)) {
            parsed.preview_only = true;
        }
        const auto url = transcoding.value("url").toString();
        if (url.contains(QStringLiteral("preview"), Qt::CaseInsensitive)
            || url.contains(QStringLiteral("snipped"), Qt::CaseInsensitive)) {
            parsed.preview_only = true;
        }
    }
    for (const auto& entry : transcodings) {
        const auto transcoding = entry.toObject();
        const auto protocol = transcoding.value("format").toObject().value("protocol").toString();
        const auto mime = transcoding.value("format").toObject().value("mime_type").toString();
        if (protocol == QStringLiteral("progressive")) {
            parsed.codec = mime.contains(QStringLiteral("mpeg")) ? QStringLiteral("mp3")
                : mime.contains(QStringLiteral("mp4"))                  ? QStringLiteral("aac")
                                                                        : QStringLiteral("opus");
            parsed.quality = transcoding.value("preset").toString();
            break;
        }
    }
    *track = parsed;
    return true;
}

bool SoundCloudProvider::parse_artist(const QJsonObject& object, Artist* artist) {
    if (!artist || number_to_id(object.value("id")).isEmpty()) return false;
    Artist parsed;
    parsed.id = number_to_id(object.value("id"));
    parsed.name = truncated(object.value("username").toString(), 300);
    parsed.artwork_url = object.value("avatar_url").toString();
    parsed.permalink_url = object.value("permalink_url").toString();
    parsed.track_count = std::max(0, object.value("track_count").toInt());
    *artist = std::move(parsed); return true;
}

QUrl SoundCloudProvider::hls_stream_url(const QJsonObject& raw) {
    const auto transcodings = raw.value("media").toObject().value("transcodings").toArray();
    for (const auto& entry : transcodings) {
        const auto item = entry.toObject();
        const auto text = item.value("url").toString();
        const QUrl url(text);
        // A snipped rendition (or a preview media URL) is not the whole track:
        // selecting it would play a snippet as if it were the release.
        if (item.value("snipped").toBool(false)
            || text.contains(QStringLiteral("preview"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("snipped"), Qt::CaseInsensitive)) {
            continue;
        }
        if (item.value("format").toObject().value("protocol").toString() == "hls"
            && url.scheme() == "https" && url.isValid()) return url;
    }
    return {};
}

bool SoundCloudProvider::looks_like_link(const QString& text) {
    const QUrl url(text.trimmed());
    const auto host = url.host().toLower();
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty()
        && url.port(443) == 443 && (host == "soundcloud.com" || host == "www.soundcloud.com" || host == "m.soundcloud.com")
        && url.path().size() > 1;
}

bool SoundCloudProvider::parse_playlist(const QJsonObject& object, Playlist* playlist) {
    if (playlist == nullptr) {
        return false;
    }
    const auto id = number_to_id(object.value("id"));
    if (id.isEmpty()) {
        return false;
    }
    Playlist parsed;
    parsed.id = truncated(id, 120);
    parsed.title = truncated(object.value("title").toString(), 400);
    parsed.artist = artist_of(object);
    parsed.artwork_url = object.value("artwork_url").toString();
    parsed.permalink_url = object.value("permalink_url").toString();
    parsed.track_count = static_cast<int>(object.value("track_count").toDouble(0));
    parsed.tracks = parse_track_collection(object);
    *playlist = parsed;
    return true;
}

QVector<SoundCloudProvider::Track> SoundCloudProvider::parse_track_collection(
    const QJsonObject& object) {
    QVector<Track> tracks;
    const auto collection = object.value("collection").toArray().isEmpty()
        ? object.value("tracks").toArray()
        : object.value("collection").toArray();
    for (const auto& entry : collection) {
        auto item = entry.toObject();
        // Playlists wrap entries as {"track": {...}} and may leave gaps for
        // removed uploads; those are skipped rather than shown as blank rows.
        if (item.contains("track") && item.value("track").isObject()) {
            item = item.value("track").toObject();
        }
        Track track;
        if (parse_track(item, &track)) {
            tracks.append(track);
        }
    }
    return tracks;
}

QUrl SoundCloudProvider::progressive_stream_url(
    const Track& track, const QJsonObject& raw, bool* has_hls) {
    if (has_hls != nullptr) {
        *has_hls = false;
    }
    // Rendition URLs are only useful for a track whose rights allow full
    // playback: a preview or blocked track must not reach a decoder at all.
    if (playback_rights(track) != Rights::None) {
        return {};
    }
    const auto transcodings = raw.value("media").toObject().value("transcodings").toArray();
    QUrl fallback;
    for (const auto& entry : transcodings) {
        const auto transcoding = entry.toObject();
        const auto format = transcoding.value("format").toObject();
        const auto protocol = format.value("protocol").toString();
        const auto text = transcoding.value("url").toString();
        const auto url = QUrl(text);
        if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
            continue;
        }
        if (transcoding.value("snipped").toBool(false)
            || text.contains(QStringLiteral("preview"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("snipped"), Qt::CaseInsensitive)) {
            continue;
        }
        if (protocol == QStringLiteral("progressive")) {
            return url;
        }
        if (protocol == QStringLiteral("hls") && has_hls != nullptr) {
            *has_hls = true;
            if (!fallback.isValid()) {
                fallback = url;
            }
        }
    }
    return {}; // HLS is selected explicitly, never passed to a file source.
}

QUrl SoundCloudProvider::streaming_url_for_transcoding(
    const QUrl& transcoding_url, const QString& clientId) {
    return withClientId(transcoding_url, clientId);
}

QUrl SoundCloudProvider::download_url(const QString& trackId, const QString& clientId) {
    auto url = QUrl(QStringLiteral("https://api-v2.soundcloud.com") + downloadPath(trackId));
    return withClientId(url, clientId);
}

bool SoundCloudProvider::looks_like_track_url(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("^https?://(?:www\\.|m\\.)?soundcloud\\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(text.trimmed()).hasMatch();
}

} // namespace videovault::app
