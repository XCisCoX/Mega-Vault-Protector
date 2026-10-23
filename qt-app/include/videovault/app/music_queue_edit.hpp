#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace videovault::app {
// Pure queue edits shared by MusicWindow and the headless regression suite.
// Indices identify occurrences, so duplicate file IDs are deliberately allowed.
struct MusicQueueEdit {
    template<class Track>
    static int insert(std::vector<Track>& queue, int& current,
                      const std::vector<Track>& incoming, bool playNext, int order) {
        if (queue.empty()) {
            queue = incoming;
            current = 0;
            return 0;
        }
        const bool reverseNext = playNext && order == 1;
        const int at = playNext ? current + (reverseNext ? 0 : 1) : static_cast<int>(queue.size());
        if (reverseNext) {
            queue.insert(queue.begin() + at, incoming.rbegin(), incoming.rend());
            current += static_cast<int>(incoming.size());
        } else queue.insert(queue.begin() + at, incoming.begin(), incoming.end());
        return at;
    }
    static void prioritize(std::vector<int>& bag, int begin, int count) {
        const int end = begin + count;
        bag.erase(std::remove_if(bag.begin(), bag.end(), [begin, end](int row) {
            return row >= begin && row < end;
        }), bag.end());
        for (int row = end; row > begin;) bag.push_back(--row);
    }
    static int afterRemoval(int current, int row, int remaining, int order) {
        if (remaining == 0) return 0;
        if (row < current || (row == current && order == 1)) --current;
        return std::clamp(current, 0, remaining - 1);
    }
    static int afterMove(int current, int from, int to) {
        if (current == from) return to;
        if (current == to) return from;
        return current;
    }
};
} // namespace videovault::app
