// QCoreApplication only: real encrypted packages, FFmpeg decode/resampling and
// the production bounded prefetch helper. No widget, audio device or GUI launch.
#include "videovault/app/local_audio_pipeline.hpp"
#include "videovault/app/music_queue_edit.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace core = videovault::core;
using videovault::app::LocalAudioReader;
using videovault::app::LocalAudioPrefetch;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n'; return 1; } } while (false)

std::vector<unsigned char> wav(int rate, int frames, std::int16_t value) {
    std::vector<unsigned char> out;
    const auto word = [&](std::uint32_t v, int n) { for (int i=0; i<n; ++i) out.push_back(static_cast<unsigned char>(v >> (8*i))); };
    const auto str = [&](const char* v) { for (int i=0;i<4;++i) out.push_back(v[i]); };
    str("RIFF"); word(36U + frames * 4U, 4); str("WAVE"); str("fmt "); word(16,4);
    word(1,2); word(2,2); word(rate,4); word(rate*4U,4); word(4,2); word(16,2);
    str("data"); word(frames*4U,4);
    for (int i=0;i<frames*2;++i) word(static_cast<std::uint16_t>(value),2);
    return out;
}
std::unique_ptr<LocalAudioPrefetch::Prepared> wait(LocalAudioPrefetch& prefetch, std::uint64_t ticket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto next = prefetch.take(ticket)) return next;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}
std::vector<std::int16_t> decode(LocalAudioReader& reader) {
    std::vector<std::int16_t> all, block;
    while (reader.read(&block)) all.insert(all.end(), block.begin(), block.end());
    return all;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using videovault::app::MusicQueueEdit;
    for (int order : {0, 1, 2}) {
        std::vector<int> queue{10, 20, 30};
        int current = 1;
        const int at = MusicQueueEdit::insert(queue, current, std::vector<int>{40, 50}, true, order);
        CHECK(queue[static_cast<std::size_t>(current)] == 20);
        if (order == 1) {
            CHECK((queue == std::vector<int>{10, 50, 40, 20, 30}));
            CHECK(queue[static_cast<std::size_t>(current-1)] == 40);
        } else CHECK((queue == std::vector<int>{10, 20, 40, 50, 30}));
        std::vector<int> bag{4, 0, 2, 3};
        MusicQueueEdit::prioritize(bag, at, 2);
        CHECK(bag.back() == at);
        bag.pop_back(); CHECK(bag.back() == at+1);
        CHECK(MusicQueueEdit::afterRemoval(current, current, 4, order) == (order == 1 ? current-1 : current));
    }
    CHECK(MusicQueueEdit::afterRemoval(2, 0, 2, 0) == 1);
    CHECK(MusicQueueEdit::afterRemoval(2, 2, 2, 0) == 1);
    CHECK(MusicQueueEdit::afterRemoval(0, 0, 0, 0) == 0);
    CHECK(MusicQueueEdit::afterMove(1, 1, 2) == 2);
    CHECK(MusicQueueEdit::afterMove(2, 1, 2) == 1);
    CHECK(MusicQueueEdit::afterMove(0, 1, 2) == 0);
    std::vector<int> duplicates{7, 7}; int currentDuplicate = 1;
    MusicQueueEdit::insert(duplicates, currentDuplicate, std::vector<int>{7}, true, 0);
    CHECK(duplicates.size() == 3U && currentDuplicate == 1);
    std::vector<int> empty; int emptyCurrent = 0;
    MusicQueueEdit::insert(empty, emptyCurrent, std::vector<int>{8}, true, 1);
    CHECK(empty.size() == 1U && emptyCurrent == 0);
    std::cout << "Queue edits: normal/reverse/shuffle play-next, current-row preservation, duplicates, remove/reorder and empty queue passed.\n";
    QTemporaryDir dir(qEnvironmentVariable("TMPDIR", QDir::tempPath()) + QStringLiteral("/mvp-local-audio-XXXXXX"));
    CHECK(dir.isValid());
    auto created = core::Vault::create(std::filesystem::u8path(dir.path().toUtf8().constData()) / "vault", "local-audio-test", {8192,1,1});
    CHECK(created);
    auto vault = std::make_shared<core::Vault>(std::move(created.value()));
    const auto a = vault->import_bytes(wav(48000, 48000, 2000), "a.wav"); CHECK(a);
    const auto b = vault->import_bytes(wav(48000, 48000, -3000), "b.wav"); CHECK(b);
    const auto c = vault->import_bytes(wav(44100, 44100, 4000), "c.wav"); CHECK(c);
    QString error;
    LocalAudioReader first;
    CHECK(first.open(vault, a.value(), "audio.wav", &error));
    LocalAudioPrefetch prefetch;
    prefetch.request(vault, b.value(), "audio.wav", 1);
    auto ready = wait(prefetch, 1); CHECK(ready && ready->reader && ready->error.isEmpty());
    // One block, not an entire second, is all that preparation retains.
    CHECK(!ready->samples.empty() && ready->samples.size() <= 48000U);
    auto joined = decode(first);
    CHECK(joined.size() == 96000U);
    CHECK(std::all_of(joined.begin(), joined.end(), [](auto s) { return s == 2000; }));
    joined.insert(joined.end(), ready->samples.begin(), ready->samples.end());
    auto rest = decode(*ready->reader); joined.insert(joined.end(), rest.begin(), rest.end());
    CHECK(joined.size() == 192000U);
    CHECK(joined[95999] == 2000 && joined[96000] == -3000);
    CHECK(std::all_of(joined.begin()+96000, joined.end(), [](auto s) { return s == -3000; }));
    std::cout << "Same-rate encrypted PCM boundary: 192000 samples, exact concatenation, no inserted silence or lost tail.\n";
    LocalAudioReader mixed;
    CHECK(mixed.open(vault, c.value(), "audio.wav", &error));
    const auto normalized = decode(mixed);
    CHECK(normalized.size() == 96000U);
    CHECK(normalized[1000] == 4000);
    CHECK(mixed.seek(0));
    CHECK(decode(mixed) == normalized);
    for (const int target : {500, 125, 345, 755, 990, 0}) {
        CHECK(mixed.seek(target));
        const auto afterSeek = decode(mixed);
        const auto expected = static_cast<std::size_t>((1000 - target) * 48 * 2);
        std::cout << "Seek " << target << " ms retained " << afterSeek.size()
                  << " interleaved samples (expected " << expected << ").\n";
        // A 44.1-kHz seek rounds to an input frame; allow one stereo output
        // frame, not the previous 1000-sample tolerance, after resampling.
        CHECK(afterSeek.size() + 2U >= expected && afterSeek.size() <= expected + 2U);
        CHECK(!afterSeek.empty() && afterSeek[afterSeek.size() / 2] == 4000);
    }
    std::cout << "Mixed-rate: 44100 Hz -> 48000 Hz stereo, exact 96000 samples including resampler drain; repeated partial-packet seeks passed.\n";
    for (std::uint64_t ticket=2; ticket<26; ++ticket) {
        prefetch.request(vault, a.value(), "audio.wav", ticket*2);
        prefetch.request(vault, b.value(), "audio.wav", ticket*2+1);
        CHECK(!prefetch.take(ticket*2));
        auto replacement = wait(prefetch, ticket*2+1);
        CHECK(replacement && replacement->reader && !replacement->samples.empty());
        CHECK(replacement->samples.front() == -3000);
        prefetch.cancel();
        CHECK(!prefetch.take(ticket*2+1));
    }
    prefetch.request(vault, -999, "audio.wav", 100);
    auto failed = wait(prefetch,100); CHECK(failed && !failed->reader && !failed->error.isEmpty());
    prefetch.request(vault, a.value(), "audio.wav", 101);
    auto completed = wait(prefetch,101); CHECK(completed && completed->reader);
    prefetch.request(vault, b.value(), "audio.wav", 102);
    vault->lock();
    CHECK(!prefetch.take(102));
    std::vector<std::int16_t> block;
    CHECK(!completed->reader->read(&block) && block.empty());
    CHECK(!completed->reader->seek(0));
    prefetch.cancel();
    LocalAudioReader locked;
    CHECK(!locked.open(vault, a.value(), "audio.wav", &error));
    std::cout << "24 prefetch supersession/cancel cycles, bad-item failure, lock/revocation, teardown passed.\n";
    return 0;
}
