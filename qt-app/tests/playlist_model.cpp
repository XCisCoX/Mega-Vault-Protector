#include "videovault/app/playlist_model.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace model = videovault::app::playlist_model;
struct Track { std::int64_t id; std::string title; };
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        const std::vector<Track> catalog{{1, "Alpha"}, {2, "Beta"}, {3, "Gamma"}};
        const std::vector<std::int64_t> saved{3, 1, 99, 2};
        const auto all = model::orderedTracks(catalog, saved, [](const Track&) { return true; });
        check(all.size() == 3 && all[0].id == 3 && all[1].id == 1 && all[2].id == 2, "saved order/missing item");
        const auto filtered = model::orderedTracks(catalog, saved, [](const Track& t) { return t.id != 1; });
        check(filtered.size() == 2 && filtered[0].id == 3 && filtered[1].id == 2, "filter must preserve saved order");
        check(model::orderedTracks(catalog, saved, [](const Track&) { return false; }).empty(), "no matches");
        check(model::orderedTracks(catalog, {}, [](const Track&) { return true; }).empty(), "empty playlist");
        check(model::orderedTracks(std::vector<Track>{}, saved, [](const Track&) { return true; }).empty(), "empty catalog");
        auto ids = std::vector<std::int64_t>{3, 1, 2};
        check(!model::moveTrack(ids, 3, -1), "first boundary");
        check(!model::moveTrack(ids, 2, 1), "last boundary");
        check(!model::moveTrack(ids, 99, -1) && !model::moveTrack(ids, 1, 0), "invalid moves");
        check(model::moveTrack(ids, 1, -1) && ids == std::vector<std::int64_t>({1, 3, 2}), "move up");
        check(model::moveTrack(ids, 1, 1) && ids == std::vector<std::int64_t>({3, 1, 2}), "move down");
        check(!model::appendTrack(ids, 1) && !model::appendTrack(ids, 0), "duplicate/invalid append");
        check(model::appendTrack(ids, 4) && ids.back() == 4, "append order");
        check(model::removeTrack(ids, 1) && ids == std::vector<std::int64_t>({3, 2, 4}), "remove exact member");
        check(!model::removeTrack(ids, 99), "missing removal");
        auto empty = std::vector<std::int64_t>{};
        check(!model::moveTrack(empty, 1, 1) && !model::removeTrack(empty, 1), "empty edits");
        check(model::appendTrack(empty, 7) && model::removeTrack(empty, 7) && empty.empty(), "last removal");
        std::cout << "playlist view/order/edit policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
