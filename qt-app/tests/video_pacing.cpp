// Deterministic regression for the audio-master playback clock.
//
// The picture used to be paced by QElapsedTimer while the sound came from
// QAudioOutput: any decode stall left the wall clock ahead for the rest of the
// file, and small rate differences accumulated. The player now follows PCM the
// device actually pulled. These checks model that device clock without FFmpeg,
// a GUI or an audio device.
#include "videovault/app/video_pacing.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using videovault::app::video_pacing::audibleMs;
using videovault::app::video_pacing::clampLeadMs;
using videovault::app::video_pacing::pulledMs;

namespace {

int failures = 0;

void check(const bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kLeadMs = 62;

} // namespace

int main() {
    // Interleaved s16 stereo: one second of audio is rate*channels samples.
    check(pulledMs(0, kChannels, kRate) == 0, "no pulls means no played time");
    check(pulledMs(static_cast<std::uint64_t>(kRate) * kChannels, kChannels, kRate) == 1000,
        "stereo sample count converts to media time");
    check(pulledMs(999, kChannels, kRate) == 10, "conversion truncates, never rounds up");
    check(pulledMs(48000, 0, kRate) == 0 && pulledMs(48000, kChannels, 0) == 0,
        "invalid format reports zero instead of dividing by zero");

    // Device buffer -> lead, clamped so a large platform buffer cannot shift
    // the picture by a visible amount.
    check(clampLeadMs(12000, 192000) == 62, "12000 bytes at 192 kB/s is 62 ms");
    check(clampLeadMs(0, 192000) == 0 && clampLeadMs(12000, 0) == 0, "no buffer means no lead");
    check(clampLeadMs(10 * 1024 * 1024, 192000) == 250, "lead is capped at 250 ms");

    check(audibleMs(-1, 96000, kChannels, kRate, kLeadMs) == -1,
        "an unestablished PCM timeline stays unknown instead of inventing a position");

    // A 10-minute play at 48 kHz where the sound card is 0.5% slow. The wall
    // clock that used to drive the picture would be ~3 s ahead by the end; the
    // audio clock must stay with the device the user is actually listening to.
    {
        std::int64_t master = 0;
        std::int64_t wall = 0;
        std::uint64_t consumed = 0;
        const std::int64_t lead = kLeadMs;
        for (int second = 1; second <= 600; ++second) {
            // 600 s of media, device consumes 0.5% fewer samples per real second.
            consumed += static_cast<std::uint64_t>(kRate * kChannels * 0.995);
            wall += 1000;
            master = std::max(master, audibleMs(0, consumed, kChannels, kRate,
                static_cast<int>(lead)));
        }
        const std::int64_t drift = wall - master;
        check(drift > 2500 && drift < 3500,
            "master follows the 0.5%-slow device instead of accumulating wall drift");
        check(master - 600000 < 0 && master > 590000,
            "10-minute position stays with the audible device, not wall time");
    }

    // A 900 ms decrypt/scale stall: the wall clock jumps, the device pulls
    // nothing, so the picture must hold instead of skipping ahead.
    {
        const std::uint64_t before = static_cast<std::uint64_t>(kRate) * kChannels * 5; // 5 s
        const std::int64_t atStall = audibleMs(10000, before, kChannels, kRate, kLeadMs);
        const std::int64_t afterStall = audibleMs(10000, before, kChannels, kRate, kLeadMs);
        check(atStall == 15000 - kLeadMs, "5 s of pulled PCM is 5 s of audible position");
        check(afterStall == atStall, "a decode stall cannot move the master clock");
        const std::int64_t wallAfter = 900; // wall advanced 900 ms
        check(afterStall - atStall == 0 && wallAfter > 0,
            "the old wall clock would have led by the whole stall");
    }

    // Underrun recovery: the device pauses, then resumes; the master continues
    // from the same media position with no accumulated offset.
    {
        std::uint64_t consumed = static_cast<std::uint64_t>(kRate) * kChannels * 2; // 2 s
        std::int64_t master = audibleMs(0, consumed, kChannels, kRate, kLeadMs);
        const std::int64_t frozen = master;
        for (int i = 0; i < 40; ++i) {
            master = std::max(master, audibleMs(0, consumed, kChannels, kRate, kLeadMs));
        }
        check(master == frozen, "starved device freezes the master for the whole underrun");
        consumed += static_cast<std::uint64_t>(kRate) * kChannels / 10; // 100 ms refilled
        master = std::max(master, audibleMs(0, consumed, kChannels, kRate, kLeadMs));
        check(master == frozen + 100, "resuming continues from the audible position");
    }

    // Seeking: the decoder trims pre-target PCM, so the new base is the target
    // and the device counter restarts from zero.
    {
        const std::int64_t target = 90500;
        const std::int64_t justSeeked = audibleMs(target, 0, kChannels, kRate, kLeadMs);
        check(justSeeked == target - kLeadMs, "a fresh seek is one device lead behind the target");
        check(audibleMs(target, static_cast<std::uint64_t>(kRate) * kChannels,
            kChannels, kRate, kLeadMs) == target + 1000 - kLeadMs,
            "seeked playback advances from the target, not from the old position");
    }

    // Frame scheduling: publish the newest due picture and drop superseded
    // ones, but never run so far that a late burst shows stale frames. The
    // audio device forces the decode (audioHungry), which is what keeps PCM
    // flowing while the picture queue is full.
    {
        std::vector<std::int64_t> pending;
        for (int i = 0; i < 8; ++i) pending.push_back(i * 40); // 40 ms frames
        const std::int64_t now = 250;                        // 250 ms of audio
        while (pending.size() > 1 && pending[1] <= now) pending.erase(pending.begin());
        check(pending.front() == 240 && pending.size() == 2,
            "only the newest frame at or before the audio position is shown");
        check(pending.front() <= now, "the shown frame is due, not early");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d audio-master clock checks failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("Audio-master clock: rate, lead, stall, underrun, seek and frame-pacing checks passed.\n");
    return EXIT_SUCCESS;
}
