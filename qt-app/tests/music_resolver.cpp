// Local-first resolution: a provider track resolves to its encrypted vault copy,
// and nothing resolves when the vault is locked or the track was never imported.

#include "videovault/app/music_resolver.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <iostream>
#include <vector>

namespace core = videovault::core;
using videovault::app::MusicResolver;

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

core::TrackProvenance provenance_for(const std::string& track_id) {
    core::TrackProvenance provenance;
    provenance.provider_id = "org.megavault.soundcloud";
    provenance.provider_track_id = track_id;
    provenance.source_url = "https://soundcloud.com/the-signals/" + track_id;
    provenance.title = "Night Drive";
    provenance.artist = "The Signals";
    provenance.duration_ms = 214000;
    provenance.codec = "mp3";
    return provenance;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto root = std::filesystem::u8path(directory.path().toUtf8().constData()) / "vault";
    auto created = core::Vault::create(root, "resolver-fixture", {8192, 1, 1});
    CHECK(created);
    auto vault = std::make_shared<core::Vault>(std::move(created.value()));

    std::vector<unsigned char> bytes(64 * 1024);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>((i * 17 + 3) & 0xff);
    }
    auto imported = vault->import_bytes(bytes, "Night Drive.mp3");
    CHECK(imported);
    const auto video_id = imported.value();
    CHECK(vault->set_video_provenance(video_id, provenance_for("night-drive")));

    auto resolver = std::make_unique<MusicResolver>(vault);
    CHECK(resolver->has_local_copy("org.megavault.soundcloud", "night-drive"));
    CHECK(!resolver->has_local_copy("org.megavault.soundcloud", "other-track"));
    CHECK(!resolver->has_local_copy("org.megavault.monochrome", "night-drive"));
    CHECK(resolver->video_id_for("org.megavault.soundcloud", "night-drive") == video_id);
    CHECK(resolver->video_id_for("org.megavault.soundcloud", "missing") == 0);

    const auto copy = resolver->find("org.megavault.soundcloud", "night-drive");
    CHECK(copy.has_value());
    CHECK(copy->video_id == video_id);
    CHECK(copy->provenance.title == "Night Drive");
    CHECK(copy->provenance.artist == "The Signals");
    CHECK(copy->provenance.codec == "mp3");

    auto listed = resolver->offline_tracks();
    CHECK(listed.size() == 1);
    CHECK(listed[0].video_id == video_id);

    // The resolved copy really is playable plaintext (what the player reads).
    auto plain = vault->read_video_bytes(copy->video_id);
    CHECK(plain);
    CHECK(plain.value().size() == bytes.size());

    // A hard lock removes the capability: nothing resolves until the vault is
    // unlocked again, which is what stops a provider page from streaming stale
    // metadata after lock.
    vault->lock();
    CHECK(!resolver->has_local_copy("org.megavault.soundcloud", "night-drive"));
    CHECK(resolver->offline_tracks().empty());
    CHECK(!resolver->find("org.megavault.soundcloud", "night-drive").has_value());

    auto reopened = core::Vault::open(root, "resolver-fixture");
    CHECK(reopened);
    auto reopened_vault = std::make_shared<core::Vault>(std::move(reopened.value()));
    auto again = std::make_unique<MusicResolver>(reopened_vault);
    // Identity survives a restart: no filename matching, no re-download.
    CHECK(again->video_id_for("org.megavault.soundcloud", "night-drive") == video_id);

    std::cout << "Music resolver tests passed.\n";
    return 0;
}
