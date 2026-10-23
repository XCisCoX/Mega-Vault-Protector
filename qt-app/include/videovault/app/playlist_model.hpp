#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace videovault::app::playlist_model {

// A filtered view must preserve saved order and never invent missing tracks.
// Track is a value with an integral id; Match has no side effects.
template <typename Track, typename Match>
std::vector<Track> orderedTracks(const std::vector<Track>& catalog,
    const std::vector<std::int64_t>& ids, Match matches) {
    std::map<std::int64_t, const Track*> byId;
    for (const auto& track : catalog) byId.emplace(track.id, &track);
    std::vector<Track> shown;
    shown.reserve(ids.size());
    for (const auto id : ids) {
        const auto found = byId.find(id);
        if (found != byId.end() && matches(*found->second)) shown.push_back(*found->second);
    }
    return shown;
}

inline bool moveTrack(std::vector<std::int64_t>& ids, const std::int64_t id, const int direction) {
    if (direction != -1 && direction != 1) return false;
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (found == ids.end()) return false;
    const auto index = static_cast<std::size_t>(found - ids.begin());
    if ((direction < 0 && index == 0) || (direction > 0 && index + 1 == ids.size())) return false;
    std::iter_swap(found, found + direction);
    return true;
}

inline bool appendTrack(std::vector<std::int64_t>& ids, const std::int64_t id) {
    if (id <= 0 || std::find(ids.begin(), ids.end(), id) != ids.end()) return false;
    ids.push_back(id);
    return true;
}

inline bool removeTrack(std::vector<std::int64_t>& ids, const std::int64_t id) {
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (found == ids.end()) return false;
    ids.erase(found);
    return true;
}

} // namespace videovault::app::playlist_model
