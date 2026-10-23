// Pitch-preserving speed change for the local encrypted music player.
//
// FFmpeg atempo is the same filter Telegram Desktop's build enables
// (`--enable-filter=atempo` in Telegram/build/prepare/prepare.py). Using it
// keeps pitch and level; the previous hand-written overlap-add lost energy and
// comb-filtered the signal at any speed other than 1.0x.
#include "videovault/app/audio_time_stretch.hpp"

#include "videovault/core/ffmpeg_compat.hpp"

#include <algorithm>
#include <cmath>

#if MVP_HAVE_AVFILTER
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace videovault::app {
namespace {

bool bypassSpeed(const double speed) {
    return std::abs(speed - 1.0) < 0.01;
}

} // namespace

struct AudioTimeStretch::Impl {
    int channels{0};
    int rate{0};
    double speed{1.0};
    std::vector<std::int16_t> pending;

#if MVP_HAVE_AVFILTER
    AVFilterGraph* graph{nullptr};
    AVFilterContext* source{nullptr};
    AVFilterContext* sink{nullptr};
    AVFrame* frame{nullptr};
    std::int64_t pts{0};

    void closeGraph() {
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (graph != nullptr) {
            avfilter_graph_free(&graph);
        }
        source = nullptr;
        sink = nullptr;
    }

    bool build(const int wantedChannels, const int wantedRate, const double wantedSpeed) {
        closeGraph();
        graph = avfilter_graph_alloc();
        if (graph == nullptr) {
            return false;
        }
        const AVFilter* sourceFilter = avfilter_get_by_name("abuffer");
        const AVFilter* sinkFilter = avfilter_get_by_name("abuffersink");
        const AVFilter* atempo = avfilter_get_by_name("atempo");
        if (sourceFilter == nullptr || sinkFilter == nullptr || atempo == nullptr) {
            closeGraph();
            return false;
        }
        char layout[64] = {};
#if MVP_FFMPEG_LEGACY_CHANNELS
        std::snprintf(layout, sizeof(layout), "0x%llx",
            static_cast<unsigned long long>(av_get_default_channel_layout(wantedChannels)));
#else
        AVChannelLayout defaultLayout;
        av_channel_layout_default(&defaultLayout, wantedChannels);
        av_channel_layout_describe(&defaultLayout, layout, sizeof(layout));
#endif
        char arguments[256] = {};
        std::snprintf(arguments, sizeof(arguments),
            "time_base=1/%d:sample_rate=%d:sample_fmt=s16:channel_layout=%s",
            wantedRate, wantedRate, layout);
        if (avfilter_graph_create_filter(&source, sourceFilter, "in", arguments, nullptr, graph) < 0) {
            closeGraph();
            return false;
        }
        AVFilterContext* tempo = nullptr;
        char tempoArguments[64] = {};
        std::snprintf(tempoArguments, sizeof(tempoArguments), "tempo=%.6f", wantedSpeed);
        if (avfilter_graph_create_filter(&tempo, atempo, "tempo", tempoArguments, nullptr, graph) < 0
            || avfilter_link(source, 0, tempo, 0) < 0) {
            closeGraph();
            return false;
        }
        if (avfilter_graph_create_filter(&sink, sinkFilter, "out", nullptr, nullptr, graph) < 0
            || avfilter_link(tempo, 0, sink, 0) < 0) {
            closeGraph();
            return false;
        }
        if (avfilter_graph_config(graph, nullptr) < 0) {
            closeGraph();
            return false;
        }
        // Small output frames keep the player's chunk cadence close to real time.
        av_buffersink_set_frame_size(sink, 1024);
        frame = av_frame_alloc();
        if (frame == nullptr) {
            closeGraph();
            return false;
        }
        channels = wantedChannels;
        rate = wantedRate;
        speed = wantedSpeed;
        pts = 0;
        return true;
    }

    void pull(std::vector<std::int16_t>* out) {
        while (sink != nullptr) {
            av_frame_unref(frame);
            const int status = av_buffersink_get_frame(sink, frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                break;
            }
            if (status < 0) {
                break;
            }
#if MVP_FFMPEG_LEGACY_CHANNELS
            const int frameChannels = frame->channels;
#else
            const int frameChannels = frame->ch_layout.nb_channels;
#endif
            if (frame->format == AV_SAMPLE_FMT_S16 && frameChannels > 0) {
                const auto samples = reinterpret_cast<const std::int16_t*>(frame->data[0]);
                const std::size_t count = static_cast<std::size_t>(frame->nb_samples)
                    * static_cast<std::size_t>(frameChannels);
                out->insert(out->end(), samples, samples + count);
            }
            av_frame_unref(frame);
        }
    }

    std::vector<std::int16_t> feed(const std::int16_t* samples, const std::size_t count,
        const bool draining) {
        std::vector<std::int16_t> out;
        if (graph == nullptr) {
            return out;
        }
        if (draining) {
            av_buffersrc_add_frame_flags(source, nullptr, 0);
            pull(&out);
            return out;
        }
        const int channelsNow = channels > 0 ? channels : 1;
        std::size_t offset = 0;
        while (offset < count) {
            const int frames = static_cast<int>(
                std::min<std::size_t>((count - offset) / static_cast<std::size_t>(channelsNow), 4096));
            if (frames <= 0) {
                break;
            }
            av_frame_unref(frame);
            frame->format = AV_SAMPLE_FMT_S16;
            frame->sample_rate = rate;
            frame->nb_samples = frames;
#if MVP_FFMPEG_LEGACY_CHANNELS
            frame->channels = channelsNow;
            frame->channel_layout = av_get_default_channel_layout(channelsNow);
#else
            av_channel_layout_default(&frame->ch_layout, channelsNow);
#endif
            if (av_frame_get_buffer(frame, 0) < 0) {
                break;
            }
            std::memcpy(frame->data[0], samples + offset,
                static_cast<std::size_t>(frames) * static_cast<std::size_t>(channelsNow)
                    * sizeof(std::int16_t));
            frame->pts = pts;
            pts += frames;
            const std::size_t added = static_cast<std::size_t>(frames)
                * static_cast<std::size_t>(channelsNow);
            offset += added;
            if (av_buffersrc_add_frame_flags(source, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                break;
            }
            pull(&out);
        }
        av_frame_unref(frame);
        return out;
    }
#endif

    // Energy-normalised overlap-add fallback for builds without libavfilter.
    static constexpr int windowLength = 2048;
    static constexpr int hopLength = 512;
    std::vector<float> overlap;
    int normaliseIndex{0};
    std::vector<float> window;

    void ensureWindow() {
        if (!window.empty()) {
            return;
        }
        window.resize(windowLength);
        for (int i = 0; i < windowLength; ++i) {
            window[static_cast<std::size_t>(i)] = static_cast<float>(
                0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / windowLength));
        }
    }

    std::vector<std::int16_t> fallback(const std::int16_t* samples, const std::size_t count,
        const int wantedChannels, const double wantedSpeed) {
        std::vector<std::int16_t> out;
        ensureWindow();
        if (count > 0 && samples != nullptr) {
            pending.insert(pending.end(), samples, samples + count);
        }
        const int hop = hopLength;
        const int analysis = std::max(1, static_cast<int>(std::lround(hop * wantedSpeed)));
        while (static_cast<int>(pending.size()) >= windowLength * wantedChannels) {
            // Accumulate one window, then emit the first hop with the gain the
            // window actually sums to. Dividing by the true sum is what keeps
            // the level constant; the old code divided by a partial sum, which
            // is why the volume fell at other speeds.
            for (int i = 0; i < windowLength; ++i) {
                const float weight = window[static_cast<std::size_t>(i)];
                for (int channel = 0; channel < wantedChannels; ++channel) {
                    const auto index = static_cast<std::size_t>(i * wantedChannels + channel);
                    if (index >= overlap.size()) {
                        overlap.resize(index + 1, 0.f);
                    }
                    overlap[index] += weight
                        * static_cast<float>(pending[static_cast<std::size_t>(index)]);
                }
            }
            std::vector<float> gain(static_cast<std::size_t>(hop), 0.f);
            for (int i = 0; i < hop; ++i) {
                gain[static_cast<std::size_t>(i)] = window[static_cast<std::size_t>(i)];
                const int previous = i + hop;
                if (previous < windowLength) {
                    gain[static_cast<std::size_t>(i)] += window[static_cast<std::size_t>(previous)];
                }
            }
            for (int i = 0; i < hop; ++i) {
                const float norm = std::max(1e-4f, gain[static_cast<std::size_t>(i)]);
                for (int channel = 0; channel < wantedChannels; ++channel) {
                    const auto index = static_cast<std::size_t>(i * wantedChannels + channel);
                    const int sample = static_cast<int>(std::lround(overlap[index] / norm));
                    out.push_back(static_cast<std::int16_t>(std::clamp(sample, -32768, 32767)));
                }
            }
            const std::size_t shift = static_cast<std::size_t>(hop * wantedChannels);
            if (overlap.size() > shift) {
                std::copy(overlap.begin() + static_cast<std::ptrdiff_t>(shift), overlap.end(),
                    overlap.begin());
                std::fill(overlap.end() - static_cast<std::ptrdiff_t>(shift), overlap.end(), 0.f);
            } else {
                std::fill(overlap.begin(), overlap.end(), 0.f);
            }
            const auto drop = static_cast<std::ptrdiff_t>(analysis * wantedChannels);
            pending.erase(pending.begin(), pending.begin() + drop);
        }
        return out;
    }
};

AudioTimeStretch::AudioTimeStretch() : impl_(std::make_unique<Impl>()) {}
AudioTimeStretch::~AudioTimeStretch() = default;

bool AudioTimeStretch::usesAtempo() {
#if MVP_HAVE_AVFILTER
    return avfilter_get_by_name("atempo") != nullptr;
#else
    return false;
#endif
}

void AudioTimeStretch::reset() {
#if MVP_HAVE_AVFILTER
    impl_->closeGraph();
#endif
    impl_->pending.clear();
    impl_->overlap.clear();
    impl_->channels = 0;
    impl_->rate = 0;
    impl_->speed = 1.0;
}

std::vector<std::int16_t> AudioTimeStretch::process(const std::int16_t* samples,
    const std::size_t count, const int channels, const int rate, const double speed,
    std::size_t* consumedFrames) {
    if (channels <= 0 || rate <= 0) {
        return {};
    }
    if (count > 0 && samples == nullptr) {
        return {};
    }
    if (bypassSpeed(speed)) {
        // 1.0x: hand the PCM through untouched - no filter, no gain change.
#if MVP_HAVE_AVFILTER
        if (impl_->graph != nullptr) {
            impl_->closeGraph();
        }
#endif
        impl_->pending.clear();
        impl_->overlap.clear();
        if (consumedFrames != nullptr) {
            *consumedFrames += count / static_cast<std::size_t>(channels);
        }
        if (count == 0) {
            return {};
        }
        return std::vector<std::int16_t>(samples, samples + count);
    }
    if (consumedFrames != nullptr) {
        *consumedFrames += count / static_cast<std::size_t>(channels);
    }
#if MVP_HAVE_AVFILTER
    const bool usable = impl_->graph != nullptr && impl_->channels == channels
        && impl_->rate == rate && std::abs(impl_->speed - speed) < 0.001;
    if (!usable && !impl_->build(channels, rate, speed)) {
        return impl_->fallback(samples, count, channels, speed);
    }
    auto out = impl_->feed(samples, count, false);
    if (out.empty() && count > 0 && impl_->graph == nullptr) {
        return impl_->fallback(samples, count, channels, speed);
    }
    return out;
#else
    return impl_->fallback(samples, count, channels, speed);
#endif
}

std::vector<std::int16_t> AudioTimeStretch::flush(const int channels, const int rate,
    const double speed, std::size_t* consumedFrames) {
    if (channels <= 0 || rate <= 0) {
        return {};
    }
    if (bypassSpeed(speed)) {
        auto out = impl_->pending;
        if (consumedFrames != nullptr) {
            *consumedFrames += out.size() / static_cast<std::size_t>(channels);
        }
        impl_->pending.clear();
        return out;
    }
#if MVP_HAVE_AVFILTER
    if (impl_->graph != nullptr && impl_->channels == channels && impl_->rate == rate) {
        auto out = impl_->feed(nullptr, 0, true);
        impl_->closeGraph();
        return out;
    }
#endif
    return impl_->fallback(nullptr, 0, channels, speed);
}

} // namespace videovault::app
