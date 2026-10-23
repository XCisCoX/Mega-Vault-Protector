#pragma once

// FFmpeg channel-layout compatibility.
//
// Ubuntu 22.04 ships FFmpeg 4.4, which still describes audio channels with the
// deprecated AVCodecContext::channels / channel_layout pair. FFmpeg 5.1 replaced
// them with AVChannelLayout, which is what Ubuntu 24.04, the Windows vcpkg
// build and the Android NDK build provide. One source tree has to build against
// both, so every channel-layout use goes through these helpers instead of
// touching ch_layout directly.
//
// FFmpeg 7.x headers carry no extern "C" guards, so the includes stay wrapped.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

// libavutil 57 is the FFmpeg 5.1 release that introduced AVChannelLayout.
#if LIBAVUTIL_VERSION_MAJOR < 57
#define MVP_FFMPEG_LEGACY_CHANNELS 1
#else
#define MVP_FFMPEG_LEGACY_CHANNELS 0
#endif

namespace videovault::core::ffmpeg_compat {

// Channel count of a stream's parameters, or 0 when the container says nothing.
inline int channels(const AVCodecParameters* parameters) {
    if (parameters == nullptr) {
        return 0;
    }
#if MVP_FFMPEG_LEGACY_CHANNELS
    return parameters->channels;
#else
    return parameters->ch_layout.nb_channels;
#endif
}

inline int channels(const AVCodecContext* context) {
    if (context == nullptr) {
        return 0;
    }
#if MVP_FFMPEG_LEGACY_CHANNELS
    return context->channels;
#else
    return context->ch_layout.nb_channels;
#endif
}

// Gives a decoder context a default layout when the container left it empty, so
// swresample always has a defined input layout to convert from.
inline void default_channels(AVCodecContext* context, const int channel_count) {
    if (context == nullptr) {
        return;
    }
    const int wanted = channel_count > 0 ? channel_count : 2;
#if MVP_FFMPEG_LEGACY_CHANNELS
    if (context->channels <= 0) {
        context->channels = wanted;
    }
    if (context->channel_layout == 0) {
        context->channel_layout = av_get_default_channel_layout(context->channels);
    }
#else
    if (context->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&context->ch_layout, wanted);
    }
#endif
}

// Same for a codecpar an encoder is about to be opened with.
inline void default_channels(AVCodecParameters* parameters, const int channel_count) {
    if (parameters == nullptr) {
        return;
    }
    const int wanted = channel_count > 0 ? channel_count : 2;
#if MVP_FFMPEG_LEGACY_CHANNELS
    parameters->channels = wanted;
    parameters->channel_layout = av_get_default_channel_layout(wanted);
#else
    av_channel_layout_default(&parameters->ch_layout, wanted);
#endif
}

// True when two streams describe the same channel arrangement. Used to refuse a
// remux whose segments change codec layout mid-stream.
inline bool same_layout(const AVCodecParameters* left, const AVCodecParameters* right) {
#if MVP_FFMPEG_LEGACY_CHANNELS
    return left->channels == right->channels
        && left->channel_layout == right->channel_layout;
#else
    return av_channel_layout_compare(&left->ch_layout, &right->ch_layout) == 0;
#endif
}

inline void copy_layout_to_frame(AVFrame* frame, const AVCodecContext* context) {
    if (frame == nullptr || context == nullptr) {
        return;
    }
#if MVP_FFMPEG_LEGACY_CHANNELS
    frame->channels = context->channels;
    frame->channel_layout = context->channel_layout;
#else
    av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
#endif
}

// Stereo 16-bit output resampler fed by a decoder's own format. The caller owns
// the returned context and frees it with swr_free().
inline int make_stereo_resampler(
    SwrContext** out,
    const int out_rate,
    const AVCodecContext* in) {
    if (out == nullptr || in == nullptr) {
        return AVERROR(EINVAL);
    }
#if MVP_FFMPEG_LEGACY_CHANNELS
    const int64_t in_layout = in->channel_layout != 0
        ? in->channel_layout
        : av_get_default_channel_layout(in->channels > 0 ? in->channels : 2);
    *out = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, out_rate,
        in_layout, in->sample_fmt, in->sample_rate, 0, nullptr);
    return *out != nullptr ? 0 : AVERROR(ENOMEM);
#else
    AVChannelLayout stereo{};
    av_channel_layout_default(&stereo, 2);
    const int status = swr_alloc_set_opts2(out, &stereo, AV_SAMPLE_FMT_S16, out_rate,
        &in->ch_layout, in->sample_fmt, in->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    return status;
#endif
}

// Stereo-in/stereo-out 16-bit resampler used to change only the sample rate.
inline int make_stereo_rate_resampler(
    SwrContext** out,
    const int out_rate,
    const int in_rate) {
    if (out == nullptr) {
        return AVERROR(EINVAL);
    }
#if MVP_FFMPEG_LEGACY_CHANNELS
    *out = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, out_rate,
        AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
    return *out != nullptr ? 0 : AVERROR(ENOMEM);
#else
    AVChannelLayout stereo{};
    av_channel_layout_default(&stereo, 2);
    const int status = swr_alloc_set_opts2(out, &stereo, AV_SAMPLE_FMT_S16, out_rate,
        &stereo, AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    return status;
#endif
}

} // namespace videovault::core::ffmpeg_compat
