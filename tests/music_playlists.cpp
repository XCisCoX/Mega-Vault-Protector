// Real SQLCipher vaults, authenticated packages and FFmpeg media probes.
// No mocked persistence or media classification.
#include "videovault/core/ffmpeg_compat.hpp"
#include "videovault/core/vault.hpp"
#include "internal/database.hpp"
#include "internal/metadata.hpp"
#include <sqlcipher/sqlite3.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace core = videovault::core;
namespace internal = core::internal;
namespace {
constexpr std::string_view password = "Playlist-Test-Password-42";
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T value(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().technical_detail);
    return std::move(result.value());
}
template<class T> void invalid(core::Result<T> result) {
    check(!result && result.error().code == core::VaultErrorCode::InvalidArgument,
        "expected InvalidArgument");
}
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("MegaVaultMusicPlaylists_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
core::Vault create(const std::filesystem::path& path) {
    return value(core::Vault::create(path, password, {8192, 2, 1}));
}
std::vector<unsigned char> wav(unsigned char sample = 0) {
    const std::uint32_t data_size = 16000;
    std::vector<unsigned char> bytes(44 + data_size, sample);
    auto text = [&](std::size_t at, std::string_view s) { std::copy(s.begin(), s.end(), bytes.begin() + at); };
    auto le = [&](std::size_t at, std::uint32_t n, int width) {
        for (int i = 0; i < width; ++i) bytes[at + i] = static_cast<unsigned char>(n >> (i * 8));
    };
    text(0, "RIFF"); le(4, 36 + data_size, 4); text(8, "WAVEfmt "); le(16, 16, 4);
    le(20, 1, 2); le(22, 1, 2); le(24, 8000, 4); le(28, 16000, 4);
    le(32, 2, 2); le(34, 16, 2); text(36, "data"); le(40, data_size, 4);
    return bytes;
}
std::vector<unsigned char> taggedWav() {
    auto bytes = wav();
    const std::string album = "Embedded Album Regression";
    std::vector<unsigned char> info{'I', 'N', 'F', 'O', 'I', 'P', 'R', 'D'};
    const auto length = static_cast<std::uint32_t>(album.size() + 1);
    for (int i = 0; i < 4; ++i) info.push_back(static_cast<unsigned char>(length >> (i * 8)));
    info.insert(info.end(), album.begin(), album.end());
    info.push_back(0);
    if (length % 2) info.push_back(0);
    bytes.insert(bytes.end(), {'L', 'I', 'S', 'T'});
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(info.size() >> (i * 8)));
    bytes.insert(bytes.end(), info.begin(), info.end());
    for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<unsigned char>((bytes.size() - 8) >> (i * 8));
    return bytes;
}

// Valid AVI with raw video AND PCM audio. Tests the case that an audio-stream
// check alone would accept, including after renaming the container to .wav.
std::vector<unsigned char> audioVideo() {
    AVFormatContext* raw = nullptr;
    check(avformat_alloc_output_context2(&raw, nullptr, "avi", nullptr) >= 0 && raw,
        "allocate AVI fixture");
    const auto cleanup = [](AVFormatContext* f) {
        if (f->pb) { unsigned char* data = nullptr; avio_close_dyn_buf(f->pb, &data); av_free(data); }
        avformat_free_context(f);
    };
    std::unique_ptr<AVFormatContext, decltype(cleanup)> format(raw, cleanup);
    AVStream* video = avformat_new_stream(raw, nullptr);
    AVStream* audio = avformat_new_stream(raw, nullptr);
    check(video && audio, "create AVI streams");
    video->time_base = {1, 10};
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    video->codecpar->width = 16; video->codecpar->height = 16;
    video->codecpar->format = AV_PIX_FMT_BGR24;
    video->codecpar->bits_per_coded_sample = 24;
    audio->time_base = {1, 8000};
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio->codecpar->sample_rate = 8000;
    audio->codecpar->bits_per_coded_sample = 16;
    audio->codecpar->block_align = 2;
    audio->codecpar->bit_rate = 128000;
    videovault::core::ffmpeg_compat::default_channels(audio->codecpar, 1);
    check(avio_open_dyn_buf(&raw->pb) >= 0, "open AVI fixture buffer");
    check(avformat_write_header(raw, nullptr) >= 0, "write AVI fixture header");
    for (int i = 0; i < 2; ++i) {
        AVPacket* packet = av_packet_alloc();
        check(packet != nullptr, "allocate fixture packet");
        std::unique_ptr<AVPacket, void(*)(AVPacket*)> owned(packet,
            [](AVPacket* p) { av_packet_free(&p); });
        check(av_new_packet(packet, i == 0 ? 16 * 16 * 3 : 1600) >= 0, "allocate packet bytes");
        std::fill(packet->data, packet->data + packet->size, 0);
        packet->stream_index = i; packet->pts = 0; packet->dts = 0;
        packet->duration = i == 0 ? 1 : 800;
        packet->flags = AV_PKT_FLAG_KEY;
        check(av_write_frame(raw, packet) >= 0, "write AVI fixture packet");
    }
    check(av_write_trailer(raw) >= 0, "finish AVI fixture");
    unsigned char* data = nullptr;
    const int size = avio_close_dyn_buf(raw->pb, &data);
    raw->pb = nullptr;
    check(size > 0, "read AVI fixture");
    std::vector<unsigned char> bytes(data, data + size);
    av_free(data);
    return bytes;
}
using Sql = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>;
Sql cipher(const std::filesystem::path& root) {
    auto metadata = value(internal::read_metadata_file(root / "vault.meta"));
    auto master = value(internal::derive_master_key(password, metadata.salt, metadata.parameters));
    constexpr std::array<char, 8> context{'M','V','P','D','B','0','0','1'};
    auto key = value(internal::derive_subkey(master, 1, context));
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2((root / "vault.db").string().c_str(), &raw,
        SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "open keyed fixture connection");
    Sql db(raw, sqlite3_close_v2);
    check(sqlite3_key(raw, key.data(), static_cast<int>(key.size())) == SQLITE_OK, "key fixture connection");
    check(sqlite3_exec(raw, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr) == SQLITE_OK,
        "enable fixture foreign keys");
    return db;
}
void exec(sqlite3* db, const char* sql) {
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (status != SQLITE_OK) throw std::runtime_error(std::string("fixture SQL: ") + sqlite3_errmsg(db));
}
std::int64_t scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK, "prepare fixture scalar");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owned(stmt, sqlite3_finalize);
    check(sqlite3_step(stmt) == SQLITE_ROW, "read fixture scalar");
    return sqlite3_column_int64(stmt, 0);
}
core::MusicPlaylist playlist(core::Vault& vault, std::int64_t id) {
    for (const auto& p : value(vault.list_music_playlists())) if (p.id == id) return p;
    throw std::runtime_error("playlist was not found");
}
void tracks(core::Vault& vault, std::int64_t id, std::vector<std::int64_t> wanted) {
    check(playlist(vault, id).track_ids == wanted, "playlist order or membership differs");
}
void set(core::Vault& vault, std::int64_t id, std::vector<std::int64_t> ids) {
    value(vault.set_music_playlist_tracks(id, ids));
    tracks(vault, id, ids);
}
void noPlaintext(const std::filesystem::path& root, const std::string& needle) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        check(bytes.find(needle) == std::string::npos, "playlist name leaked to disk in plaintext");
    }
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2((root / "vault.db").string().c_str(), &raw,
        SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open unkeyed fixture connection");
    Sql db(raw, sqlite3_close_v2);
    check(sqlite3_exec(raw, "SELECT name FROM music_playlists;", nullptr, nullptr, nullptr) != SQLITE_OK,
        "unkeyed SQLCipher connection must not read playlists");
}
void lifecycle() {
    TemporaryDirectory temp;
    const auto root = temp.path / "one";
    auto vault = create(root);
    check(value(vault.list_music_playlists()).empty(), "new vault must have no playlists");
    const auto a = value(vault.import_bytes(wav(0), "One.wav"));
    const auto b = value(vault.import_bytes(wav(1), "Two.WAV"));
    const auto folder = value(vault.create_folder(0, "Music"));
    const auto c = value(vault.import_bytes(wav(2), "Three.wav", folder));
    const auto tagged = value(vault.import_bytes(taggedWav(), "Tagged.wav"));
    check(value(vault.media_info(tagged)).album == "Embedded Album Regression",
        "lazy media probe must expose the embedded album tag");
    check(value(vault.media_info(a)).album.empty(), "untagged audio album must be empty");
    const auto p = value(vault.create_music_playlist("  Private-Playlist-Sentinel-67219  "));
    check(playlist(vault, p).name == "Private-Playlist-Sentinel-67219", "trim playlist name");
    check(playlist(vault, p).created_at > 0, "creation timestamp");
    check(playlist(vault, p).updated_at >= playlist(vault, p).created_at, "update timestamp");
    const auto q = value(vault.create_music_playlist("Another"));
    set(vault, p, {a});                      // add
    set(vault, p, {a, b, c});                // append
    set(vault, p, {c, a, b});                // reorder
    set(vault, p, {c, b});                   // remove
    set(vault, p, {c, b});                   // idempotent
    set(vault, q, {b, a});                   // independent collection
    // Listing is database-only: it must not open every audio package.
    {
        const auto package = value(vault.package_path(a));
        auto moved = package;
        moved += ".held";
        std::filesystem::rename(package, moved);
        tracks(vault, q, {b, a});
        std::filesystem::rename(moved, package);
    }
    invalid(vault.create_music_playlist("   "));
    invalid(vault.create_music_playlist(std::string(121, 'x')));
    invalid(vault.create_music_playlist("bad/name"));
    invalid(vault.rename_music_playlist(p, std::string("bad\0name", 8)));
    invalid(vault.rename_music_playlist(99999, "Unknown"));
    invalid(vault.delete_music_playlist(99999));
    invalid(vault.set_music_playlist_tracks(99999, {}));
    for (const auto ids : {std::vector<std::int64_t>{a, a}, {a, 99999}, {0}, {-1}}) {
        invalid(vault.set_music_playlist_tracks(p, ids));
        tracks(vault, p, {c, b});
    }
    const std::vector<unsigned char> doc{'%', 'P', 'D', 'F', '-', '1', '.', '7'};
    const auto document = value(vault.import_bytes(doc, "Pretend.wav"));
    const auto video = value(vault.import_bytes(audioVideo(), "Movie.wav"));
    invalid(vault.set_music_playlist_tracks(p, std::vector<std::int64_t>{a, document}));
    invalid(vault.set_music_playlist_tracks(p, std::vector<std::int64_t>{a, video}));
    tracks(vault, p, {c, b});
    auto other = create(temp.path / "two");
    check(value(other.list_music_playlists()).empty(), "playlists must be vault isolated");
    invalid(other.set_music_playlist_tracks(value(other.create_music_playlist("Other")),
        std::vector<std::int64_t>{a}));
    invalid(other.rename_music_playlist(q, "Not mine"));
    // The validation callback runs after reserving the writer: a second
    // connection cannot replace the audio that is currently being validated.
    {
        auto writer = cipher(root);
        internal::Database database(writer.release());
        auto competitor = cipher(root);
        const std::vector<std::int64_t> ids{c, b};
        value(database.set_music_playlist_tracks(p, ids, [&]() -> core::Result<bool> {
            const int status = sqlite3_exec(competitor.get(),
                "UPDATE videos SET display_name='Concurrent.wav';", nullptr, nullptr, nullptr);
            check(status == SQLITE_BUSY, "validation must hold the database writer reservation");
            return true;
        }));
        tracks(vault, p, ids);
    }
    // A real failure after the old rows have been deleted and the first new
    // row inserted must roll back both membership AND metadata.
    {
        auto db = cipher(root);
        exec(db.get(), "UPDATE music_playlists SET updated_at=123;"
            "CREATE TRIGGER fail_playlist_insert BEFORE INSERT ON music_playlist_tracks"
            " WHEN NEW.position=1 BEGIN SELECT RAISE(ABORT,'injected playlist failure'); END;");
        invalid(vault.set_music_playlist_tracks(p, std::vector<std::int64_t>{a, b, c}));
        tracks(vault, p, {c, b});
        check(playlist(vault, p).updated_at == 123, "failed transaction changed timestamp");
        exec(db.get(), "DROP TRIGGER fail_playlist_insert;");
    }
    set(vault, p, {c, b}); // proves the failed transaction was closed
    value(vault.rename_music_playlist(p, "Private-Playlist-Sentinel-67219"));
    check(playlist(vault, p).updated_at > 123, "rename updates timestamp");
    value(vault.rename_video(b, "Renamed.wav"));
    value(vault.move_video(b, folder));
    tracks(vault, p, {c, b});
    const auto audio_bytes = value(vault.read_video_bytes(b));
    const auto cpath = value(vault.package_path(c));
    value(vault.remove_video(c));
    check(!std::filesystem::exists(cpath), "deleted track package must be cleaned up");
    tracks(vault, p, {b});
    value(vault.remove_folder(folder));
    tracks(vault, p, {});
    tracks(vault, q, {a});
    value(vault.replace_video_content(a, doc, "Now a document.pdf"));
    tracks(vault, q, {}); // content replacement invalidates the validated identity
    const auto fresh = value(vault.import_bytes(audio_bytes, "Fresh.wav"));
    set(vault, p, {fresh});
    value(vault.delete_music_playlist(q));
    invalid(vault.delete_music_playlist(q));
    check(value(vault.read_video_bytes(fresh)) == audio_bytes, "deleting playlists must not delete media");
    noPlaintext(root, "Private-Playlist-Sentinel-67219");
    vault.lock();
    invalid(vault.list_music_playlists());
    invalid(vault.create_music_playlist("Locked"));
    invalid(vault.rename_music_playlist(p, "Locked"));
    invalid(vault.delete_music_playlist(p));
    invalid(vault.set_music_playlist_tracks(p, std::vector<std::int64_t>{fresh}));
    auto wrong = core::Vault::open(root, "Wrong-Password-42");
    check(!wrong, "wrong password must reject playlist vault");
    vault = value(core::Vault::open(root, password));
    tracks(vault, p, {fresh});
    check(value(vault.list_music_playlists()).size() == 1, "deleted playlist resurrected after reopen");
    value(vault.change_password(password, "New-Playlist-Password-42", {8192, 2, 1}));
    vault.lock();
    vault = value(core::Vault::open(root, "New-Playlist-Password-42"));
    tracks(vault, p, {fresh});
    check(value(vault.read_video_bytes(fresh)) == audio_bytes, "rekey lost playlist media");
    set(vault, p, {});
    value(vault.delete_music_playlist(p));
    check(value(vault.list_music_playlists()).empty(), "last playlist deletion");
    std::cout << "PASS lifecycle, ordering, media validation, isolation, lock, rollback, cleanup, rekey\n";
}
void migration() {
    TemporaryDirectory temp;
    const auto root = temp.path / "legacy";
    auto vault = create(root);
    const auto track = value(vault.import_bytes(wav(), "Legacy.wav"));
    value(vault.add_tag(track, "Preserved"));
    core::TrackProvenance provenance;
    provenance.provider_id = "test.provider"; provenance.provider_track_id = "legacy-track";
    value(vault.set_video_provenance(track, provenance));
    vault.lock();
    // Reconstruct the exact schema-7 shape by removing only migration-8 DDL.
    {
        auto db = cipher(root);
        check(scalar(db.get(), "SELECT max(version) FROM schema_migrations;") == 8, "fresh schema version");
        exec(db.get(), "DROP TRIGGER music_playlist_content_replaced;"
            "DROP TRIGGER music_playlist_track_removed; DROP TABLE music_playlist_tracks;"
            "DROP TABLE music_playlists; DELETE FROM schema_migrations WHERE version=8;"
            // Force failure after the first CREATE TABLE in migration 8.
            "CREATE TABLE music_playlist_tracks(conflict INTEGER);");
    }
    auto failed = core::Vault::open(root, password);
    check(!failed, "conflicting migration must fail");
    {
        auto db = cipher(root);
        check(scalar(db.get(), "SELECT max(version) FROM schema_migrations;") == 7, "failed migration advanced version");
        check(scalar(db.get(), "SELECT count(*) FROM sqlite_schema WHERE name='music_playlists';") == 0,
            "failed migration left partial table");
        exec(db.get(), "DROP TABLE music_playlist_tracks;");
    }
    vault = value(core::Vault::open(root, password));
    check(value(vault.list_music_playlists()).empty(), "migration must not invent playlists");
    check(value(vault.read_video_bytes(track)) == wav(), "migration damaged legacy package");
    check(value(vault.tags_for_video(track)).size() == 1, "migration lost legacy tags");
    check(value(vault.find_video_by_provenance("test.provider", "legacy-track")) == track,
        "migration lost schema-7 provenance");
    const auto p = value(vault.create_music_playlist("Migrated"));
    set(vault, p, {track});
    vault.lock();
    vault = value(core::Vault::open(root, password));
    tracks(vault, p, {track});
    auto db = cipher(root);
    check(scalar(db.get(), "SELECT count(*) FROM schema_migrations WHERE version=8;") == 1,
        "migration must be idempotent");
    check(scalar(db.get(), "SELECT count(*) FROM pragma_foreign_key_check;") == 0, "foreign key integrity");
    value(vault.delete_music_playlist(p));
    check(scalar(db.get(), "SELECT count(*) FROM music_playlist_tracks;") == 0, "playlist delete must cascade");
    std::cout << "PASS schema7 migration, failed migration rollback/retry, provenance preservation, cascade\n";
}
} // namespace
int main() {
    try { lifecycle(); migration(); return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return EXIT_FAILURE; }
}
