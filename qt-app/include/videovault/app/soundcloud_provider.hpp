#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace videovault::app {

// SoundCloud provider: API identity, stream selection and rights gating.
//
// This layer is deliberately UI-free and header-only in its API surface, so the
// parsing and rights rules can be tested without a network and reused by the
// desktop page and (later) the Android client.
//
// Rights rules enforced here, not in the UI:
//   * a track is playable only when SoundCloud reports it streamable and not
//     geo-blocked/preview-only;
//   * an offline copy may only be created when SoundCloud itself marks the track
//     `downloadable` (the artist enabled it) — a go+/paywalled or preview track
//     is refused with an explicit reason, never silently saved;
//   * the API key is supplied by the user and kept in the host's settings, never
//     in a manifest and never scraped from the web player.
//
// The class does not bypass DRM, paywalls, subscription gates or access
// restrictions: it uses the same public endpoints the web player uses with the
// user's own API key, and reports refusal instead of working around it.
class SoundCloudProvider final {
public:
    // Endpoints (api-v2.soundcloud.com), kept as path builders so the tests can
    // assert exactly what is requested.
    static QString searchTracksPath(const QString& query, int limit);
    static QString searchUsersPath(const QString& query, int limit);
    static QString searchPlaylistsPath(const QString& query, int limit);
    static QString resolvePath(const QString& permalinkUrl);
    static QString userTracksPath(const QString& userIdOrUrl, int limit);
    static QString playlistPath(const QString& playlistId);
    static QString downloadPath(const QString& trackId);
    // Appends the user's API key to a SoundCloud URL that requires one.
    static QUrl withClientId(const QUrl& url, const QString& clientId);

    struct Track {
        QString id;
        QString title;
        QString artist;
        QString artwork_url;
        QString permalink_url;
        qint64 duration_ms{0};
        bool streamable{false};
        bool downloadable{false};
        // SoundCloud's own flags: a snippet ("SNIP"/preview), a blocked track, or
        // a geo-restricted one. Any of them makes an offline copy impossible.
        bool preview_only{false};
        bool blocked{false};
        bool geo_blocked{false};
        bool access_unknown{false};
        // `has_downloads_left: false` means the artist's allowance is used up:
        // downloadable stays true while no original can be fetched any more.
        bool downloads_exhausted{false};
        QString codec;   // "mp3" / "opus" / "aac" when known
        QString quality; // preset name, e.g. "mp3_0_1"
        // Signing/authorization token the stream URL may require.
        QString track_authorization;
    };

    struct Artist {
        QString id;
        QString name;
        QString artwork_url;
        QString permalink_url;
        int track_count{0};
    };
    static bool parse_artist(const QJsonObject&, Artist*);
    static bool looks_like_link(const QString&);
    static QUrl hls_stream_url(const QJsonObject&);

    struct Playlist {
        QString id;
        QString title;
        QString artist;
        QString artwork_url;
        QString permalink_url;
        int track_count{0};
        QVector<Track> tracks;
    };

    // Why a track cannot be played or saved. `None` means it can.
    enum class Rights {
        None,
        NotStreamable,
        Blocked,
        GeoBlocked,
        PreviewOnly,
        NotDownloadable,
        Unknown
    };

    static QString rights_message(Rights);
    static Rights playback_rights(const Track&);
    // The gate that stands in front of every encrypted import.
    static Rights download_rights(const Track&);

    // Parses one track object from any search/resolve/playlist response.
    static bool parse_track(const QJsonObject& object, Track* track);
    static bool parse_playlist(const QJsonObject& object, Playlist* playlist);
    // Extracts tracks from a search response, a user's tracks array or a
    // playlist's `tracks` array (which may hold {"track": {...}} wrappers).
    static QVector<Track> parse_track_collection(const QJsonObject& object);

    // Picks the progressive (single-file) transcoding. HLS is reported
    // separately because its segments cannot be concatenated into a valid file
    // without format-aware remuxing; playback can use it later, an import may not.
    static QUrl progressive_stream_url(const Track& track, const QJsonObject& raw,
        bool* has_hls);
    // The official download endpoint, only meaningful when downloadable.
    static QUrl download_url(const QString& trackId, const QString& clientId);
    static QUrl streaming_url_for_transcoding(const QUrl& transcoding_url,
        const QString& clientId);
    // True for a track link the user pasted that this provider understands.
    static bool looks_like_track_url(const QString& text);
};

} // namespace videovault::app
