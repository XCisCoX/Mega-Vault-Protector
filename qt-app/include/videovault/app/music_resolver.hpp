#pragma once

#include "videovault/core/provenance.hpp"
#include "videovault/core/vault.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace videovault::app {

// Local-first resolution for provider tracks.
//
// The identity of a provider track is (provider id, provider track id), recorded
// in the vault database when the encrypted import commits. This class is the only
// place that answers "do I already own this?", so every caller — the provider
// page, the library and (later) the mobile client — resolves the same way and a
// track that was imported once is never downloaded twice, and never depends on a
// file name to be found.
class MusicResolver final {
public:
    struct LocalCopy {
        std::int64_t video_id{0};
        videovault::core::TrackProvenance provenance;
    };

    explicit MusicResolver(std::shared_ptr<videovault::core::Vault> vault);

    // True when the vault holds this provider track and it can be played offline.
    [[nodiscard]] bool has_local_copy(
        const std::string& provider_id, const std::string& track_id) const;

    [[nodiscard]] std::optional<LocalCopy> find(
        const std::string& provider_id, const std::string& track_id) const;

    // Every provider-linked item, for "offline" badges in a provider listing.
    [[nodiscard]] std::vector<videovault::core::TrackProvenance> offline_tracks() const;

    // The item id for one provider track, or 0.
    [[nodiscard]] std::int64_t video_id_for(
        const std::string& provider_id, const std::string& track_id) const;

private:
    std::shared_ptr<videovault::core::Vault> vault_;
};

} // namespace videovault::app
