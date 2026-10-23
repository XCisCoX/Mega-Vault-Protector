#include "videovault/app/music_resolver.hpp"

#include <utility>

namespace videovault::app {

MusicResolver::MusicResolver(std::shared_ptr<videovault::core::Vault> vault)
    : vault_(std::move(vault)) {}

bool MusicResolver::has_local_copy(
    const std::string& provider_id, const std::string& track_id) const {
    return video_id_for(provider_id, track_id) > 0;
}

std::optional<MusicResolver::LocalCopy> MusicResolver::find(
    const std::string& provider_id, const std::string& track_id) const {
    if (vault_ == nullptr || !vault_->is_unlocked()) {
        return std::nullopt;
    }
    auto resolved = vault_->find_video_by_provenance(provider_id, track_id);
    if (!resolved || !resolved.value()) {
        // A locked vault, a missing row and a damaged database all mean the same
        // thing to a caller: there is no offline copy to play right now.
        return std::nullopt;
    }
    const auto video_id = resolved.value().value();
    auto provenance = vault_->video_provenance(video_id);
    LocalCopy copy;
    copy.video_id = video_id;
    if (provenance && provenance.value()) {
        copy.provenance = provenance.value().value();
    } else {
        copy.provenance.provider_id = provider_id;
        copy.provenance.provider_track_id = track_id;
        copy.provenance.video_id = video_id;
    }
    return copy;
}

std::vector<videovault::core::TrackProvenance> MusicResolver::offline_tracks() const {
    if (vault_ == nullptr || !vault_->is_unlocked()) {
        return {};
    }
    auto listed = vault_->list_video_provenance();
    if (!listed) {
        return {};
    }
    return listed.value();
}

std::int64_t MusicResolver::video_id_for(
    const std::string& provider_id, const std::string& track_id) const {
    const auto copy = find(provider_id, track_id);
    return copy ? copy->video_id : 0;
}

} // namespace videovault::app
