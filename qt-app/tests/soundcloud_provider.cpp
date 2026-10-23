// SoundCloud provider rules, exercised without any network access.
//
// The point of these cases is the rights gate and the request shapes: a preview,
// blocked, geo-restricted or non-downloadable track must be refused in the
// provider layer, so no UI path can accidentally import it into the vault.

#include "videovault/app/soundcloud_provider.hpp"
#include "videovault/app/soundcloud_hls_source.hpp"
#include "videovault/app/soundcloud_transport.hpp"
#include "videovault/app/media_decoder.hpp"
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QThread>
#include <QHash>
#include <future>
#include <atomic>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using videovault::app::SoundCloudProvider;

namespace {

int fail(const char* message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

#define CHECK(x)                                                                                      \
    do {                                                                                              \
        if (!(x)) {                                                                                   \
            std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n';                                 \
            return 1;                                                                                 \
        }                                                                                             \
    } while (false)

QJsonObject sample_track() {
    QJsonObject track;
    track["id"] = 123456789.0;
    track["title"] = QStringLiteral("Night Drive");
    track["duration"] = 214000.0;
    track["streamable"] = true;
    track["downloadable"] = true;
    track["permalink_url"] = QStringLiteral("https://soundcloud.com/the-signals/night-drive");
    track["artwork_url"] = QStringLiteral("https://i1.sndcdn.com/artworks-000-large.jpg");
    track["user"] = QJsonObject{{"username", QStringLiteral("The Signals")}};
    track["media"] = QJsonObject{
        {"transcodings",
            QJsonArray{
                QJsonObject{{"url", QStringLiteral("https://api-v2.soundcloud.com/media/soundcloud:tracks:1/hls")},
                    {"preset", QStringLiteral("mp3_0_1")},
                    {"format", QJsonObject{{"protocol", QStringLiteral("hls")},
                                  {"mime_type", QStringLiteral("audio/mpeg")}}}},
                QJsonObject{{"url", QStringLiteral("https://api-v2.soundcloud.com/media/soundcloud:tracks:1/stream/progressive")},
                    {"preset", QStringLiteral("mp3_0_1")},
                    {"format", QJsonObject{{"protocol", QStringLiteral("progressive")},
                                  {"mime_type", QStringLiteral("audio/mpeg")}}}},
            }},
    };
    return track;
}

// A real loopback server; never an API credential or a live SoundCloud request.
class HlsFixture final {
public:
    QTcpServer server;
    QHash<QByteArray,QByteArray> bodies;
    QHash<QByteArray,QByteArray> redirects;
    QHash<QByteArray,int> requests;
    HlsFixture() {
        server.listen(QHostAddress::LocalHost,0);
        QObject::connect(&server,&QTcpServer::newConnection,&server,[this] {
            while (auto* socket=server.nextPendingConnection()) {
                QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                QObject::connect(socket,&QTcpSocket::readyRead,socket,[this,socket] {
                    auto data=socket->property("request").toByteArray()+socket->readAll();
                    if (!data.contains("\r\n\r\n")) { socket->setProperty("request",data); return; }
                    const auto path=data.split(' ').value(1); ++requests[path];
                    if (path=="/stall") return;
                    if (redirects.contains(path)) socket->write("HTTP/1.1 302 Found\r\nLocation: "+redirects[path]+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    else if (bodies.contains(path)) socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(bodies[path].size())+"\r\nConnection: close\r\n\r\n"+bodies[path]);
                    else socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url(const char* path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(QString::fromLatin1(path))); }
};
template<class T> bool waitFuture(std::future<T>& future,int timeout=15000) {
    QElapsedTimer elapsed; elapsed.start();
    while (future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && elapsed.elapsed()<timeout) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;
}
QByteArray silentAdts() {
    // 0.3s mono AAC, generated from anullsrc. Kept in source to avoid a tool/runtime dependency.
    return QByteArray::fromBase64("//FQQANf/NwATGF2YzYzLjEuMTAxAAIwQA7/8VBAAX/8ARggB//xUEABf/wBGCAH//FQQAF//AEYIAf/8VBAAX/8ARggB//xUEABf/wBGCAH//FQQAF//AEYIAf/8VBAAX/8ARggB//xUEABf/wBGCAH//FQQAF//AEYIAf/8VBAAX/8ARggB//xUEABf/wBGCAH//FQQAF//AEYIAf/8VBAAX/8ARggBw==");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // --- parsing ------------------------------------------------------------
    {
        const auto object = sample_track();
        SoundCloudProvider::Track track;
        CHECK(SoundCloudProvider::parse_track(object, &track));
        CHECK(track.id == QStringLiteral("123456789")); // never 1.23457e+08
        CHECK(track.title == QStringLiteral("Night Drive"));
        CHECK(track.artist == QStringLiteral("The Signals"));
        CHECK(track.duration_ms == 214000);
        CHECK(track.streamable);
        CHECK(track.downloadable);
        CHECK(track.codec == QStringLiteral("mp3"));
        CHECK(SoundCloudProvider::playback_rights(track) == SoundCloudProvider::Rights::None);
        CHECK(SoundCloudProvider::download_rights(track) == SoundCloudProvider::Rights::None);

        bool has_hls = false;
        const auto stream = SoundCloudProvider::progressive_stream_url(track, object, &has_hls);
        CHECK(has_hls); // the HLS variant was seen ...
        CHECK(stream.toString().contains(QStringLiteral("progressive"))); // ... and the progressive one chosen
    }

    // An HLS-only track must not masquerade as a progressive file.
    {
        auto object = sample_track();
        auto media = object["media"].toObject();
        auto variants = media["transcodings"].toArray();
        variants.removeLast();
        media["transcodings"] = variants;
        object["media"] = media;
        SoundCloudProvider::Track track;
        CHECK(SoundCloudProvider::parse_track(object, &track));
        bool hls = false;
        CHECK(SoundCloudProvider::progressive_stream_url(track, object, &hls).isEmpty());
        CHECK(hls);
    }

    // --- the download gate --------------------------------------------------
    {
        auto object = sample_track();
        object["downloadable"] = false;
        SoundCloudProvider::Track track;
        CHECK(SoundCloudProvider::parse_track(object, &track));
        CHECK(SoundCloudProvider::playback_rights(track) == SoundCloudProvider::Rights::None);
        // Streaming is fine, storing it is not: the artist did not allow it.
        CHECK(SoundCloudProvider::download_rights(track)
            == SoundCloudProvider::Rights::NotDownloadable);
        CHECK(!SoundCloudProvider::rights_message(SoundCloudProvider::Rights::NotDownloadable)
                   .isEmpty());
    }
    {
        auto object = sample_track();
        object["policy"] = QStringLiteral("SNIP");
        SoundCloudProvider::Track track;
        CHECK(SoundCloudProvider::parse_track(object, &track));
        CHECK(track.preview_only);
        CHECK(SoundCloudProvider::playback_rights(track)
            == SoundCloudProvider::Rights::PreviewOnly);
        // Even though `downloadable` is true, a snippet must never be saved.
        CHECK(SoundCloudProvider::download_rights(track)
            == SoundCloudProvider::Rights::PreviewOnly);
    }
    {
        auto object = sample_track();
        object["policy"] = QStringLiteral("BLOCK");
        SoundCloudProvider::Track track;
        CHECK(SoundCloudProvider::parse_track(object, &track));
        CHECK(SoundCloudProvider::playback_rights(track) == SoundCloudProvider::Rights::Blocked);
        CHECK(SoundCloudProvider::download_rights(track) == SoundCloudProvider::Rights::Blocked);
    }
    {
        auto object = sample_track();
        object["streamable"] = false;
        SoundCloudProvider::Track track;
        CHECK(SoundCloudProvider::parse_track(object, &track));
        CHECK(SoundCloudProvider::playback_rights(track)
            == SoundCloudProvider::Rights::NotStreamable);
    }

    // Rendition-only preview markers cannot evade the top-level rights gate.
    {
        auto raw=sample_track(); SoundCloudProvider::Track track;
        auto variants=raw["media"].toObject()["transcodings"].toArray();
        for (int i=0;i<variants.size();++i) { auto entry=variants[i].toObject(); entry["snipped"]=true; variants[i]=entry; }
        raw["media"]=QJsonObject{{"transcodings",variants}};
        CHECK(SoundCloudProvider::parse_track(raw,&track));
        CHECK(SoundCloudProvider::playback_rights(track)==SoundCloudProvider::Rights::PreviewOnly);
        CHECK(SoundCloudProvider::download_rights(track)==SoundCloudProvider::Rights::PreviewOnly);
        bool hls=false; CHECK(SoundCloudProvider::progressive_stream_url(track,raw,&hls).isEmpty());
        CHECK(SoundCloudProvider::hls_stream_url(raw).isEmpty());
        raw=sample_track(); variants=raw["media"].toObject()["transcodings"].toArray();
        for (int i=0;i<variants.size();++i) { auto entry=variants[i].toObject(); entry["url"]="https://api-v2.soundcloud.com/media/preview/stream"; variants[i]=entry; }
        raw["media"]=QJsonObject{{"transcodings",variants}};
        CHECK(SoundCloudProvider::parse_track(raw,&track));
        CHECK(SoundCloudProvider::download_rights(track)==SoundCloudProvider::Rights::PreviewOnly);
        raw=sample_track(); raw["has_downloads_left"]=false;
        CHECK(SoundCloudProvider::parse_track(raw,&track));
        CHECK(SoundCloudProvider::download_rights(track)!=SoundCloudProvider::Rights::None);
    }
    // --- malformed identity and denied access stay fail-closed ---------------
    {
        auto object = sample_track(); SoundCloudProvider::Track track;
        object["id"] = QString(121,'1');
        CHECK(!SoundCloudProvider::parse_track(object,&track));
        object = sample_track(); object["id"] = 1.5;
        CHECK(!SoundCloudProvider::parse_track(object,&track));
        object = sample_track(); object["access"] = "blocked";
        CHECK(SoundCloudProvider::parse_track(object,&track));
        CHECK(SoundCloudProvider::download_rights(track) == SoundCloudProvider::Rights::Blocked);
        object = sample_track(); object["access"] = "subscription_required";
        CHECK(SoundCloudProvider::parse_track(object,&track));
        CHECK(SoundCloudProvider::playback_rights(track) == SoundCloudProvider::Rights::Unknown);
        const auto url = SoundCloudProvider::withClientId(QUrl("https://api-v2.soundcloud.com/tracks?not_client_id=other"),"mine");
        CHECK(url.query().contains("&client_id=mine"));
    }
    // --- request shapes -----------------------------------------------------
    {
        const auto search = SoundCloudProvider::searchTracksPath(QStringLiteral("night drive"), 20);
        CHECK(search.startsWith(QStringLiteral("/search/tracks?q=night%20drive")));
        CHECK(search.contains(QStringLiteral("limit=20")));
        CHECK(SoundCloudProvider::searchPlaylistsPath(QStringLiteral("a&b"), 5)
                  .contains(QStringLiteral("a%26b")));
        CHECK(SoundCloudProvider::resolvePath(
                  QStringLiteral("https://soundcloud.com/the-signals/night-drive"))
                  .startsWith(QStringLiteral("/resolve?url=https%3A%2F%2Fsoundcloud.com")));
        CHECK(SoundCloudProvider::playlistPath(QStringLiteral("42")) == QStringLiteral("/playlists/42"));
        CHECK(SoundCloudProvider::downloadPath(QStringLiteral("42"))
            == QStringLiteral("/tracks/42/download"));

        const auto signed_url = SoundCloudProvider::withClientId(
            QUrl(QStringLiteral("https://api-v2.soundcloud.com/tracks/42/stream")),
            QStringLiteral("abc123"));
        CHECK(signed_url.query().contains(QStringLiteral("client_id=abc123")));
        // Idempotent: an existing key is not appended twice.
        const auto twice = SoundCloudProvider::withClientId(signed_url, QStringLiteral("abc123"));
        CHECK(twice.query().count(QStringLiteral("client_id=")) == 1);
        // No key, no query change.
        CHECK(SoundCloudProvider::withClientId(
                  QUrl(QStringLiteral("https://api-v2.soundcloud.com/tracks/42/stream")), {})
                  .query()
                  .isEmpty());
        CHECK(SoundCloudProvider::download_url(QStringLiteral("42"), QStringLiteral("k"))
                  .toString()
                  .startsWith(QStringLiteral(
                      "https://api-v2.soundcloud.com/tracks/42/download?client_id=k")));
    }

    // --- collections and playlists -----------------------------------------
    {
        auto wrapped = QJsonObject{{"track", sample_track()}};
        auto missing = QJsonObject{{"title", QStringLiteral("removed")}}; // no id
        const QJsonObject playlist_object{
            {"id", 77.0},
            {"title", QStringLiteral("Late")},
            {"track_count", 2},
            {"user", QJsonObject{{"username", QStringLiteral("The Signals")}}},
            {"tracks", QJsonArray{wrapped, QJsonObject{{"track", missing}}}},
        };
        SoundCloudProvider::Playlist playlist;
        CHECK(SoundCloudProvider::parse_playlist(playlist_object, &playlist));
        CHECK(playlist.id == QStringLiteral("77"));
        CHECK(playlist.title == QStringLiteral("Late"));
        CHECK(playlist.artist == QStringLiteral("The Signals"));
        // The removed upload is skipped, not rendered as an empty row.
        CHECK(playlist.tracks.size() == 1);
        CHECK(playlist.tracks[0].id == QStringLiteral("123456789"));
    }

    // --- pasted links -------------------------------------------------------
    {
        CHECK(SoundCloudProvider::looks_like_track_url(
            QStringLiteral("https://soundcloud.com/the-signals/night-drive")));
        CHECK(SoundCloudProvider::looks_like_track_url(
            QStringLiteral("https://m.soundcloud.com/the-signals/night-drive?si=abc")));
        CHECK(!SoundCloudProvider::looks_like_track_url(
            QStringLiteral("https://soundcloud.com/the-signals"))); // a profile, not a track
        CHECK(!SoundCloudProvider::looks_like_track_url(
            QStringLiteral("https://example.com/the-signals/night-drive")));
        CHECK(!SoundCloudProvider::looks_like_track_url(QStringLiteral("night drive")));
    }

    using namespace videovault::app;
    // Artist/profile and playlist route fixtures, including ID-only playlist placeholders.
    {
        SoundCloudProvider::Artist artist;
        CHECK(SoundCloudProvider::parse_artist(QJsonObject{{"id","42"},{"username","Signals"},{"avatar_url","https://i1.sndcdn.com/avatar.jpg"},{"track_count",9}},&artist));
        CHECK(artist.id == "42" && artist.name == "Signals" && artist.track_count == 9);
        CHECK(SoundCloudProvider::searchUsersPath("a&b",500) == "/search/users?q=a%26b&limit=50");
        CHECK(SoundCloudProvider::userTracksPath("42",25) == "/users/42/tracks?limit=25");
        CHECK(SoundCloudProvider::looks_like_link("https://soundcloud.com/signals"));
        CHECK(SoundCloudProvider::looks_like_link("https://soundcloud.com/signals/sets/night"));
        CHECK(!SoundCloudProvider::looks_like_link("https://soundcloud.com.evil.test/signals"));
        CHECK(!SoundCloudProvider::looks_like_link("https://u:p@soundcloud.com/signals"));
        CHECK(!SoundCloudProvider::looks_like_link("https://soundcloud.com:444/signals"));
    }
    // Manifest parser is bounded and fails closed before any media/key fetch.
    {
        SoundCloudHlsPlaylist playlist; QString error;
        const QUrl base("https://cf-media.sndcdn.com/path/list.m3u8?token=signed");
        CHECK(SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXTINF:1,\nfirst.aac\n#EXTINF:1,\nsecond.aac\n#EXT-X-ENDLIST\n",base,&playlist,&error));
        CHECK(playlist.segments.size()==2);
        CHECK(playlist.segments[0].url == QUrl("https://cf-media.sndcdn.com/path/first.aac"));
        CHECK(!SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXTINF:1,\nfirst.aac\n",base,&playlist,&error));
        CHECK(error.contains("finite"));
        CHECK(!SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"secret.key\"\n#EXTINF:1,\nfirst.aac\n#EXT-X-ENDLIST\n",base,&playlist,&error));
        CHECK(error.contains("Encrypted"));
        CHECK(!SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXTINF:1,\nhttp://127.0.0.1/private\n#EXT-X-ENDLIST\n",base,&playlist,&error));
        CHECK(!SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXTINF:1,\nfile:///secret\n#EXT-X-ENDLIST\n",base,&playlist,&error));
        CHECK(!SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXT-X-BYTERANGE:12@0\n#EXTINF:1,\nfirst.aac\n#EXT-X-ENDLIST\n",base,&playlist,&error));
        CHECK(SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:1,\nfirst.m4s\n#EXT-X-ENDLIST\n",base,&playlist,&error));
        CHECK(playlist.segments[0].initialization.path() == "/path/init.mp4");
        CHECK(SoundCloudHlsPlaylist::parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000\nlow.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=256000\nhigh.m3u8\n",base,&playlist,&error));
        CHECK(playlist.variant.path() == "/path/low.m3u8");
        CHECK(SoundCloudTransport::permits(QUrl("https://i1.sndcdn.com/art.jpg")));
        CHECK(!SoundCloudTransport::permits(QUrl("https://a.b.sndcdn.com/art.jpg")));
        CHECK(!SoundCloudTransport::permits(QUrl("https://sndcdn.com/art.jpg")));
        CHECK(!SoundCloudTransport::permits(QUrl("http://127.0.0.1:1234/test")));
    }
    // Real HTTP -> playlist/master -> per-segment demux/remux -> existing FFmpeg decoder.
    {
        HlsFixture fixture;
        fixture.bodies["/master"]="#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000\n/media\n";
        fixture.bodies["/media"]="#EXTM3U\n#EXTINF:0.3,\n/one.aac\n#EXT-X-DISCONTINUITY\n#EXTINF:0.3,\n/two.aac\n#EXT-X-ENDLIST\n";
        fixture.bodies["/one.aac"]=silentAdts(); fixture.bodies["/two.aac"]=silentAdts();
        SoundCloudHlsSource source(fixture.url("/master"),true); source.start();
        auto decode=std::async(std::launch::async,[&] {
            MediaDecoder decoder; QString error; decoder.prepare_for_audio();
            if (!decoder.open_source(&source,&error,"hls.nut")) { std::cerr << error.toStdString() << '\n'; return -1; }
            int samples=0; std::int64_t pts=0;
            while (decoder.pump_audio(8192,&pts)) { auto pcm=decoder.take_audio_samples(); samples+=int(pcm.size()); if (pcm.empty()) break; }
            return samples;
        });
        if (!waitFuture(decode)) { source.cancel(); CHECK(waitFuture(decode)); CHECK(false); }
        const auto samples=decode.get();
        if (!source.errorString().isEmpty()) std::cerr << source.errorString().toStdString() << '\n';
        CHECK(samples > 20000); // both segments, not just a successfully decoded header
        CHECK(source.errorString().isEmpty());
        CHECK(fixture.requests["/master"]==1 && fixture.requests["/media"]==1);
        CHECK(fixture.requests["/one.aac"]==1 && fixture.requests["/two.aac"]==1);
        CHECK(source.peak_buffered() <= (4u<<20)); CHECK(source.seek(0,kSeekSize)==-1);
    }
    // Packet remux supports the provider's distinct media formats, not byte concatenation.
    for (const auto& encoded : QVector<QByteArray>{
        "AAAFenic83QxZmEAAaWQ4GBXBgYGPgYGZp/EsjQzYz1DPUMDQwYE+P9b4AgD8wEGxiUMDAwKDAwmDQwMLD6Ovq4meoYKGok5BRmJCgaaoWQChEH/fwsd0WxGs4dC03FYJHAkmGYWoXqolplOHlrOTB8PXaBXDF2jk4eu0s5DCIDVQ1S1AWYPFg/RwJ5RD4WSCQDsV7FB",
        "AAADrHicc3cQFGBw+qDKwHiQgeE/438Gxj8NIh5CjGxubrkFqemcwalFZZnJqQaG5TXOp/4PLuDuwCDAwLCBF+x2BsYPDFobWTb9HxrAPYBBgIFpgxDY7Q8ZPjDwg4htsw/c/D/ogbsjgwF7AANDNU8dKNwPMGxsaGBVZGC/kfj/Y4ADc/yfOww+iWXJZsZ6hnqGBoYMTAYOfCAZxvo/jBIK7APKdGdkMNzAMNAhSCKAuh8AawHmgw==",
        "AAABznic809PD2ZgYoCC5QujwfTJ1Ur/GIX9C0qLPVITUxgZLRgbdoMl/EHKGZCVMzIwMHBZqnxltAEpD0lML+ZhYGDwSSxLMzPWM9QzNDAEqZBhYGBIzUvOT0ktsvVJLEuGySnkZCblF5QWg81lqbBEmAtyk5br7T0C/IJCKKCC+8lBszdHe3s83WYuUdtQwX6ySP2hy6vQjx8OXL37Miihgv1k5YmT4QcWCSn/ep/+OeXAg5EjAgB94csO",
        "AAAEPnictVO/axRBFP5m7xItJCwa0eKKEyIIashhPCwPkiKFYCEKos3kZocs2dlZZt4dUUiYwn9GEP+CQJpU1hYWaf0/5M3tXvYuEkjhY4dv3s9533ssgJ6mz1Xu7UsgAWPu7dBU2wMgOTXWTgEUZnqgsCDfdyOI+F3KgnJVH+FaSQBxRk4eAvhEh/HNzjXVo03c+F3x0ahcAugb9W9e786j8vxAFa7xeDsp25Fv7aRUe7JURcYxYtXkpQaQerNQdEPN7D3lMt1q787EFf36/tPTfgHgxJNvctlemmpbXqEkkF62imHmledLJ4TwGALdEMKD0f3oPI5nJYSwcvf2+z9YDSGIBMD6PjlCKyZ2ThQr1ZJ68uOWvu7Jf1kadOppbFv6EzPNjni05CI2nS8vSE4UMbMPJovI8qg9bqNyJ6uKpzKXZ3nhuemNb2T50Z6SMTkWfy2nevhic7A52BoA+GWsjeswer4OjjslJ+M6iO0JXs3sXfbfS2qipBUtdfyVHO+/w+Y1ABdNTo3d/4edHzPEG6MkXTDT8ZxpsjVaEw/7t256AOwY7Xh8T0k7WdNtWC+xT36L+KOlRrtm3Tt/AYNdemI="}) {
        HlsFixture fixture; const auto media=qUncompress(QByteArray::fromBase64(encoded)); CHECK(!media.isEmpty());
        fixture.bodies["/media"]="#EXTM3U\n#EXTINF:0.3,\n/first\n#EXTINF:0.3,\n/second\n#EXT-X-ENDLIST\n";
        fixture.bodies["/first"]=media; fixture.bodies["/second"]=media;
        SoundCloudHlsSource source(fixture.url("/media"),true); source.start();
        auto decode=std::async(std::launch::async,[&] {
            MediaDecoder decoder; QString error; decoder.prepare_for_audio();
            if (!decoder.open_source(&source,&error,"hls.nut")) { std::cerr << error.toStdString() << '\n'; return -1; }
            int samples=0; std::int64_t pts=0;
            while (decoder.pump_audio(8192,&pts)) { auto pcm=decoder.take_audio_samples(); samples+=int(pcm.size()); if (pcm.empty()) break; }
            return samples;
        });
        if (!waitFuture(decode)) { source.cancel(); CHECK(waitFuture(decode)); CHECK(false); }
        auto samples=decode.get();
        if (!source.errorString().isEmpty()) std::cerr<<source.errorString().toStdString()<<'\n';
        CHECK(samples>20000); CHECK(source.errorString().isEmpty());
        CHECK(fixture.requests["/first"]==1 && fixture.requests["/second"]==1);
    }
    // Redirect validation and byte caps are enforced while receiving, not afterward.
    {
        HlsFixture fixture; fixture.redirects["/redirect"]="http://localhost:1/private";
        fixture.redirects["/download"]="/large";
        fixture.bodies["/large"]=QByteArray(65536,'x'); std::atomic<bool> cancelled{false}; SoundCloudTransport transport(true);
        auto original=std::async(std::launch::async,[&] { return transport.get(fixture.url("/download"),1024,cancelled,true); });
        CHECK(waitFuture(original));
        const auto resolved=original.get();
        CHECK(resolved.error.isEmpty()); CHECK(resolved.body.contains("redirectUri"));
        CHECK(fixture.requests["/large"]==0); // resolve authorized original without a duplicate audio fetch
        auto get=std::async(std::launch::async,[&] { return transport.get(fixture.url("/redirect"),1024,cancelled); });
        CHECK(waitFuture(get)); CHECK(get.get().error.contains("permitted"));
        auto large=std::async(std::launch::async,[&] { return transport.get(fixture.url("/large"),1024,cancelled); });
        CHECK(waitFuture(large)); CHECK(large.get().error.contains("memory"));
        auto stall=std::async(std::launch::async,[&] { return transport.get(fixture.url("/stall"),1024,cancelled); });
        QElapsedTimer time; time.start(); while (!fixture.requests.contains("/stall") && time.elapsed()<3000) { app.processEvents(); QThread::msleep(1); }
        cancelled=true; CHECK(waitFuture(stall,2500)); CHECK(!stall.get().error.isEmpty());
    }
    std::cout << "SoundCloud provider and HLS fixtures passed.\n";
    return 0;
}
