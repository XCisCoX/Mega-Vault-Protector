#pragma once

#include <algorithm>
#include <cstdint>

// Audio-master playback clock for the desktop video player.
//
// The picture follows PCM that QAudioOutput has actually pulled from the sink,
// not wall time. A wall clock and the audio device run at slightly different
// rates and a decode stall cannot be represented at all, so a wall-driven
// picture drifts away from the sound and jumps after every stall. Counting
// device pulls makes both sides advance on one clock; a stall freezes the
// picture instead of desynchronising it.
//
// The device pulls ahead of the speaker by its own buffer, so the audible
// position is the pulled position minus that lead.
namespace videovault::app::video_pacing {

constexpr int kMaxLeadMs = 250;

// Milliseconds of PCM represented by interleaved s16 samples.
inline std::int64_t pulledMs(const std::uint64_t consumedSamples,
                             const int channels, const int rate) {
    if (channels <= 0 || rate <= 0) {
        return 0;
    }
    const auto frames = consumedSamples / static_cast<std::uint64_t>(channels);
    return static_cast<std::int64_t>(frames * 1000U / static_cast<std::uint64_t>(rate));
}

// How far the device's buffered PCM leads the speaker, from its buffer size.
inline int clampLeadMs(const int deviceBufferBytes, const int bytesPerSecond) {
    if (deviceBufferBytes <= 0 || bytesPerSecond <= 0) {
        return 0;
    }
    const auto ms = static_cast<std::int64_t>(deviceBufferBytes) * 1000
        / static_cast<std::int64_t>(bytesPerSecond);
    return static_cast<int>(std::clamp<std::int64_t>(ms, 0, kMaxLeadMs));
}

// Media position of the audio that is audible now. `baseMs` is the PTS of the
// first sample queued after a clear or seek, or -1 when the PCM timeline is
// not established yet.
inline std::int64_t audibleMs(const std::int64_t baseMs,
                              const std::uint64_t consumedSamples,
                              const int channels, const int rate, const int leadMs,
                              const std::int64_t sincePullMs = 0) {
    if (baseMs < 0 || channels <= 0 || rate <= 0) {
        return baseMs;
    }
    const auto lead = std::clamp(leadMs, 0, kMaxLeadMs);
    // Native audio backends pull in chunks, not at the video refresh rate.
    // Interpolate only PCM already handed to the device, never past its end.
    // This smooths a stepped device clock without becoming a drifting wall
    // clock; a starved device stops after its queued audio actually drains.
    const auto interpolation = consumedSamples > 0
        ? std::clamp<std::int64_t>(sincePullMs, 0, lead) : 0;
    return baseMs + pulledMs(consumedSamples, channels, rate) - lead + interpolation;
}

} // namespace videovault::app::video_pacing
