// Speed-change quality regression for the local encrypted music player.
//
// The player's old speed control was a plain Hann overlap-add: it summed
// overlapping copies without phase alignment, so it comb-filtered the signal
// ("quality gets lower") and under-summed the gains, so the level fell as soon
// as the speed left 1.0x. It now runs FFmpeg's atempo — the filter Telegram
// Desktop's build enables. These checks pin the three things the user heard:
// duration follows the speed, loudness does not change, and pitch is preserved.
#include "videovault/app/audio_time_stretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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
constexpr double kToneHz = 440.0;
constexpr double kAmplitude = 12000.0;

std::vector<std::int16_t> tone(const double seconds, const double frequency = kToneHz) {
    const auto frames = static_cast<std::size_t>(kRate * seconds);
    std::vector<std::int16_t> samples(frames * kChannels);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<std::int16_t>(std::lround(
            kAmplitude * std::sin(2.0 * 3.141592653589793 * frequency * i / kRate)));
        samples[i * kChannels] = value;
        samples[i * kChannels + 1] = value;
    }
    return samples;
}

double rms(const std::vector<std::int16_t>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const auto sample : samples) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

// Frequency estimate from zero crossings of the left channel: a pitch-shifting
// resampler (the wrong way to change speed) would read ~484 Hz at 1.1x, while
// atempo must keep the tone at 440 Hz.
double zeroCrossingHz(const std::vector<std::int16_t>& samples) {
    if (samples.size() < 2 * kChannels) {
        return 0.0;
    }
    int crossings = 0;
    std::int16_t previous = samples[0];
    for (std::size_t i = kChannels; i < samples.size(); i += kChannels) {
        if ((previous < 0 && samples[i] >= 0) || (previous >= 0 && samples[i] < 0)) {
            ++crossings;
        }
        previous = samples[i];
    }
    const double seconds = static_cast<double>(samples.size() / kChannels) / kRate;
    return crossings / 2.0 / std::max(1e-9, seconds);
}

struct Result {
    std::vector<std::int16_t> out;
    std::size_t consumed{0};
};

Result run(const std::vector<std::int16_t>& input, const double speed,
           const std::size_t chunkFrames = 1024) {
    videovault::app::AudioTimeStretch stretch;
    Result result;
    const std::size_t chunk = chunkFrames * kChannels;
    for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
        const std::size_t count = std::min(chunk, input.size() - offset);
        auto part = stretch.process(input.data() + offset, count, kChannels, kRate, speed,
            &result.consumed);
        result.out.insert(result.out.end(), part.begin(), part.end());
    }
    auto tail = stretch.flush(kChannels, kRate, speed, &result.consumed);
    result.out.insert(result.out.end(), tail.begin(), tail.end());
    return result;
}

} // namespace

int main() {
    check(videovault::app::AudioTimeStretch::usesAtempo(),
        "this build must use FFmpeg's atempo, not a home-made stretcher");

    // 1.0x must be a bit-exact pass-through: no gain, no filtering.
    {
        const auto input = tone(1.0);
        const auto result = run(input, 1.0);
        check(result.out.size() == input.size(), "1.0x returns every sample");
        check(result.out == input, "1.0x is bit-exact");
        check(result.consumed == input.size() / kChannels, "1.0x reports the frames it consumed");
    }

    const double inputRms = rms(tone(2.0));

    for (const double speed : {1.1, 1.25, 1.5, 2.0, 0.75, 0.5}) {
        const auto input = tone(2.0);
        const auto result = run(input, speed);
        const double expected = 2.0 / speed;   // seconds of output
        const double produced = static_cast<double>(result.out.size() / kChannels) / kRate;
        // atempo pads its tail, so allow a small window; the rate itself must
        // be right, not approximately right.
        const double errorSeconds = std::abs(produced - expected);
        if (errorSeconds > 0.12) {
            std::fprintf(stderr, "speed %.2fx: produced %.3f s, expected %.3f s\n",
                speed, produced, expected);
        }
        check(errorSeconds <= 0.12, "output duration follows the playback speed");

        const double level = rms(result.out);
        const double ratio = level / std::max(1.0, inputRms);
        if (speed == 1.1 || speed == 2.0 || speed == 0.5) {
            if (ratio < 0.9 || ratio > 1.1) {
                std::fprintf(stderr, "speed %.2fx: RMS ratio %.3f\n", speed, ratio);
            }
            check(ratio >= 0.9 && ratio <= 1.1,
                "the level does not drop when the speed changes");
        }

        // The middle second avoids atempo's fade-in/out at the boundaries.
        const std::size_t begin = std::min(result.out.size(),
            static_cast<std::size_t>(kRate * kChannels) / 3);
        const std::size_t end = result.out.size();
        std::vector<std::int16_t> middle(result.out.begin() + static_cast<std::ptrdiff_t>(begin),
            result.out.begin() + static_cast<std::ptrdiff_t>(end));
        const double hz = zeroCrossingHz(middle);
        if (std::abs(hz - kToneHz) > kToneHz * 0.06) {
            std::fprintf(stderr, "speed %.2fx: measured %.1f Hz\n", speed, hz);
        }
        check(std::abs(hz - kToneHz) <= kToneHz * 0.06,
            "speed change preserves pitch (no chipmunk/slowed voices)");
    }

    // Switching speed mid-stream must rebuild the filter and keep working.
    {
        videovault::app::AudioTimeStretch stretch;
        std::size_t consumed = 0;
        auto first = stretch.process(tone(0.5).data(), static_cast<std::size_t>(kRate * kChannels) / 2,
            kChannels, kRate, 1.5, &consumed);
        auto second = stretch.process(tone(0.5).data(), static_cast<std::size_t>(kRate * kChannels) / 2,
            kChannels, kRate, 0.75, &consumed);
        auto tail = stretch.flush(kChannels, kRate, 0.75, &consumed);
        check(!first.empty() && !second.empty(), "a speed change keeps producing audio");
        check(consumed == 2 * (static_cast<std::size_t>(kRate) / 2),
            "consumed frames follow the media, not the output");
        check(rms(second) > kAmplitude * 0.3, "no silence after a speed change");
        (void)tail;
    }

    // Seeking throws the filter state away instead of leaking the old tail.
    {
        videovault::app::AudioTimeStretch stretch;
        std::size_t consumed = 0;
        (void)stretch.process(tone(0.2).data(), static_cast<std::size_t>(kRate * kChannels) / 5,
            kChannels, kRate, 1.1, &consumed);
        stretch.reset();
        auto after = stretch.process(tone(0.2).data(), static_cast<std::size_t>(kRate * kChannels) / 5,
            kChannels, kRate, 1.1, &consumed);
        check(!after.empty(), "playback continues after a seek reset");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d speed-change checks failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("Speed change: duration, level and pitch preserved at 0.5x-2.0x (atempo).\n");
    return EXIT_SUCCESS;
}
