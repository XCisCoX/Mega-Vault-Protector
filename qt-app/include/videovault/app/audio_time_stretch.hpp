#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace videovault::app {

// Pitch-preserving playback-speed change for the encrypted music player.
//
// The old implementation was a plain overlap-add of Hann windows: it summed
// overlapping copies without phase alignment, which comb-filters the signal
// (audible as "lower quality") and loses energy, so the volume dropped as soon
// as the speed left 1.0x. Telegram Desktop does not hand-roll this either: its
// FFmpeg build enables `--enable-filter=atempo` and the loader runs the audio
// through that filter.
//
// So this now uses the same FFmpeg `atempo` filter (WSOLA inside libavfilter):
// pitch and loudness are preserved and the artefacts of the home-made
// stretcher are gone. Where libavfilter is unavailable (the Android core is
// built `--disable-everything` without filters), a corrected OLA with energy
// normalisation is used instead, so the build never silently degrades to raw
// resampling.
class AudioTimeStretch final {
public:
    AudioTimeStretch();
    ~AudioTimeStretch();
    AudioTimeStretch(const AudioTimeStretch&) = delete;
    AudioTimeStretch& operator=(const AudioTimeStretch&) = delete;

    // True when this build stretches with FFmpeg's atempo instead of the
    // fallback, for diagnostics and tests.
    [[nodiscard]] static bool usesAtempo();

    // Drops buffered input and closes the filter graph. Call on seek, track
    // change, pause reset and device format change.
    void reset();

    // Feeds interleaved s16 PCM and returns the stretched PCM that is ready.
    // `consumedFrames` (optional) receives the number of input frames taken, so
    // the player's position keeps following the media clock, not the output.
    std::vector<std::int16_t> process(const std::int16_t* samples, std::size_t count,
        int channels, int rate, double speed, std::size_t* consumedFrames);

    // Flushes the tail of the last chunk at end of stream.
    std::vector<std::int16_t> flush(int channels, int rate, double speed,
        std::size_t* consumedFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace videovault::app
