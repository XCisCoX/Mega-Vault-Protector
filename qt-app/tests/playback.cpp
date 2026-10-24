// Headless playback test: exercises MediaDecoder (the same engine the player
// window uses) against an imported encrypted video — frame decoding, audio
// samples, and seeking — with no GUI involved.
#include "videovault/core/ffmpeg_compat.hpp"
#include "videovault/app/media_decoder.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QLibraryInfo>
#include <QSlider>
#include <QThreadPool>
#include <QSemaphore>
#include <QtConcurrent/QtConcurrentRun>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using videovault::app::DecodeStep;

namespace videovault::app {
struct PlayerWindowTestAccess {
    static bool videoWorkerStarted(const PlayerWindow& player) { return player.worker_.joinable(); }
    static bool checkDelayedFirstPicture(const std::shared_ptr<core::Vault>& vault, std::int64_t id) {
        PlayerWindow player(vault, id, QStringLiteral("delayed first picture regression"));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents();
            if (player.playheadMs_.load() >= 2000 && !player.surface_->frame().isNull()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    static bool checkFrameDelivery(const std::shared_ptr<core::Vault>& vault) {
        PlayerWindow player(vault, -1, QStringLiteral("frame delivery regression"));
        player.quit_.store(true);
        player.worker_.join();
        player.timer_.stop();
        player.openFailed_.store(false);
        QImage picture(32, 32, QImage::Format_RGB32);
        picture.fill(Qt::green);
        std::thread producer([&] { player.publishFrame(picture, 17); });
        producer.join();
        QApplication::processEvents();
        return !player.surface_->frame().isNull()
            && player.surface_->frame().pixelColor(0, 0) == QColor(Qt::green);
    }
    static bool checkRecoveryBudget(PlayerWindow& player) {
        if (!player.audioOutput_ || player.audioOutput_->state() != QAudio::ActiveState) {
            std::printf("No active audio backend: hardware recovery check skipped.\n");
            return true;
        }
        std::printf("Real audio backend active: %llu PCM samples consumed.\n",
            static_cast<unsigned long long>(player.audioSink_->consumed_samples()));
        player.audioRestartAttempts_ = 5;
        player.tick();
        return player.audioRestartAttempts_ == 0 && !player.audioDeviceFailed_.load();
    }
    static bool checkRepeatAudioReset(const std::shared_ptr<core::Vault>& vault) {
        PlayerWindow player(vault, -1, QStringLiteral("audio epoch regression"));
        player.quit_.store(true);
        player.worker_.join();
        player.timer_.stop();
        player.openFailed_.store(false);
        player.audioSink_ = new AudioSink(&player);
        player.audioSinkAtomic_.store(player.audioSink_);
        const std::int16_t pcm[] = {101, 102, 103, 104};
        player.audioSink_->append(pcm, 4);
        char pulled[4];
        player.audioSink_->read(pulled, sizeof(pulled));
        // Repeat/restart comes from the decode worker and bypasses doSeek.
        player.requestSeek(0);
        player.tick();
        return player.audioSink_->consumed_samples() == 0
            && player.audioSink_->buffered_samples() == 0;
    }
    static bool checkPausedSeeking(PlayerWindow& player) {
        const auto spin = [](int ms) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < end) {
                QApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        if (!player.playing_.load()) player.togglePlayPause();
        player.togglePlayPause();
        // Several rapid requests must converge on the last seek, without
        // resuming the audio device behind a paused picture.
        for (const auto target : {200, 2500, 700, 1600}) player.doSeek(target);
        spin(350);
        if (player.audioSeekPending_.load() || player.playing_.load()
            || player.playheadMs_.load() != 1600
            || (player.audioSink_ && player.audioSink_->consumed_samples() != 0)) return false;
        player.togglePlayPause();
        spin(450);
        return player.playheadMs_.load() > 1600 && player.playheadMs_.load() < 2400;
    }
    static bool checkRepeatedPlayback(PlayerWindow& player) {
        player.repeatWatch_.store(2);
        player.restartFromStart();
        const auto first = player.audioSeekSerial_.load();
        auto observed = first;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < end) {
            QApplication::processEvents();
            const auto serial = player.audioSeekSerial_.load();
            if (serial != observed && !player.audioSeekPending_.load()) {
                if (player.playheadMs_.load() > 500) {
                    std::fprintf(stderr, "Repeat epoch %llu -> %llu committed at %lld ms.\n",
                        static_cast<unsigned long long>(first), static_cast<unsigned long long>(serial),
                        static_cast<long long>(player.playheadMs_.load()));
                    return false;
                }
                observed = serial;
                if (observed >= first + 2) {
                    player.repeatWatch_.store(0);
                    std::printf("Real worker repeated twice with a reset audio-master clock.\n");
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::fprintf(stderr, "Repeat timeout: epochs %llu -> %llu, position %lld, playing %d, pending %d.\n",
            static_cast<unsigned long long>(first), static_cast<unsigned long long>(player.audioSeekSerial_.load()),
            static_cast<long long>(player.playheadMs_.load()), int(player.playing_.load()),
            int(player.audioSeekPending_.load()));
        return false;
    }
};
}

namespace {

template<class Player>
std::unique_ptr<Player> makeStillPlayer(const std::shared_ptr<videovault::core::Vault>& vault,
                                      std::int64_t id) {
    if constexpr (std::is_constructible_v<Player, decltype(vault), std::int64_t,
                                          const QString&, QWidget*, bool>) {
        return std::make_unique<Player>(vault, id, QStringLiteral("initial still"), nullptr, true);
    } else {
        return {};
    }
}


class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVaultProtect_Playback_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return EXIT_FAILURE;
}

// ctest starts in the build tree, not the repo. Walk parents so docs/ and
// tests/fixtures resolve the same way on Windows and Linux CI.
std::filesystem::path repoPath(const std::filesystem::path& relative) {
    std::error_code error;
    auto dir = std::filesystem::current_path(error);
    if (error) {
        return {};
    }
    for (int i = 0; i < 8; ++i) {
        const auto candidate = dir / relative;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
        if (!dir.has_parent_path()) {
            break;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

videovault::core::Argon2Parameters testParameters() {
    videovault::core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

// Writes a Matroska fixture with an MJPEG video stream (64x64, 10 fps,
// 20 frames) and a PCM s16le audio stream (48 kHz stereo, 0.1 s per frame).
bool writeFixture(const std::filesystem::path& path, const int audioDelayMs = 0,
    const int startMs = 0, const int audioFrames = 20, const int videoStride = 1,
    const int videoFrames = 20) {
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int kFps = 10;
    const int kFrames = videoFrames;
    constexpr int kAudioRate = 48000;
    constexpr int kAudioChannels = 2;
    constexpr int kSamplesPerFrame = kAudioRate / kFps; // 4800

    // Video source frames (RGB24 gradient).
    const int frame_size = kWidth * kHeight * 3;
    std::vector<unsigned char> rgb(
        static_cast<std::size_t>(frame_size) * kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto offset = static_cast<std::size_t>(
                    frame * frame_size + (y * kWidth + x) * 3);
                rgb[offset + 0] = static_cast<unsigned char>((x * 255 / kWidth + frame * 3) & 0xff);
                rgb[offset + 1] = static_cast<unsigned char>((y * 255 / kHeight) & 0xff);
                rgb[offset + 2] = static_cast<unsigned char>(((x + y) * 255 / (kWidth + kHeight) + frame * 5) & 0xff);
            }
        }
    }

    // 440 Hz tone, s16le stereo.
    std::vector<std::int16_t> tone(
        static_cast<std::size_t>(kSamplesPerFrame) * kAudioChannels * audioFrames);
    for (std::size_t i = 0; i < tone.size() / kAudioChannels; ++i) {
        const auto value = static_cast<std::int16_t>(
            8000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / kAudioRate));
        tone[i * kAudioChannels + 0] = value;
        tone[i * kAudioChannels + 1] = value;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    codec->width = kWidth;
    codec->height = kHeight;
    codec->time_base = AVRational{1, kFps};
    codec->pix_fmt = AV_PIX_FMT_YUVJ420P;
    codec->color_range = AVCOL_RANGE_JPEG;
    if (avcodec_open2(codec, encoder, nullptr) < 0) {
        return false;
    }

    SwsContext* scaler = sws_getContext(
        kWidth, kHeight, AV_PIX_FMT_RGB24,
        kWidth, kHeight, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUVJ420P;
    yuv->width = kWidth;
    yuv->height = kHeight;
    av_frame_get_buffer(yuv, 0);
    const uint8_t* src_slices[4] = {rgb.data(), nullptr, nullptr, nullptr};
    const int src_strides[4] = {kWidth * 3, 0, 0, 0};

    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, "matroska", nullptr) < 0
        || format == nullptr) {
        return false;
    }

    AVStream* video_stream = avformat_new_stream(format, nullptr);
    video_stream->id = 0;
    video_stream->time_base = AVRational{1, kFps};
    AVCodecParameters* vpar = video_stream->codecpar;
    vpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vpar->codec_id = AV_CODEC_ID_MJPEG;
    vpar->format = AV_PIX_FMT_YUVJ420P;
    vpar->width = kWidth;
    vpar->height = kHeight;

    AVStream* audio_stream = avformat_new_stream(format, nullptr);
    audio_stream->id = 1;
    audio_stream->time_base = AVRational{1, kAudioRate};
    AVCodecParameters* apar = audio_stream->codecpar;
    apar->codec_type = AVMEDIA_TYPE_AUDIO;
    apar->codec_id = AV_CODEC_ID_PCM_S16LE;
    apar->format = AV_SAMPLE_FMT_S16;
    apar->sample_rate = kAudioRate;
    videovault::core::ffmpeg_compat::default_channels(apar, kAudioChannels);
    apar->bits_per_coded_sample = 16;
    apar->block_align = kAudioChannels * 2;
    apar->bit_rate = kAudioRate * kAudioChannels * 16;

    if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        return false;
    }
    if (avformat_write_header(format, nullptr) < 0) {
        return false;
    }

    const AVRational video_tb = video_stream->time_base; // after header
    const AVRational audio_tb = audio_stream->time_base;
    const AVRational source_tb = AVRational{1, kFps};

    AVPacket* packet = av_packet_alloc();
    bool success = true;
    for (int frame = 0; frame < audioFrames; ++frame) {
        if (frame % videoStride == 0 && frame / videoStride < kFrames) {
            sws_scale(scaler, src_slices, src_strides, 0, kHeight, yuv->data, yuv->linesize);
            yuv->pts = av_rescale_q(frame, source_tb, video_tb);
            if (avcodec_send_frame(codec, yuv) < 0
                || avcodec_receive_packet(codec, packet) < 0) {
                success = false;
                break;
            }
            packet->stream_index = video_stream->index;
            packet->pts = av_rescale_q(frame, source_tb, video_tb)
                + av_rescale_q(startMs, AVRational{1, 1000}, video_tb);
            packet->dts = packet->pts;
            if (av_interleaved_write_frame(format, packet) < 0) {
                success = false;
                break;
            }
            av_packet_unref(packet);
            src_slices[0] += frame_size;
        }

        const auto audio_offset = static_cast<std::size_t>(frame) * kSamplesPerFrame * kAudioChannels;
        packet->data = reinterpret_cast<std::uint8_t*>(
            const_cast<std::int16_t*>(tone.data() + audio_offset));
        packet->size = kSamplesPerFrame * kAudioChannels * 2;
        packet->stream_index = audio_stream->index;
        packet->pts = av_rescale_q(
            static_cast<std::int64_t>(frame) * kSamplesPerFrame
                + static_cast<std::int64_t>(audioDelayMs + startMs) * kAudioRate / 1000,
            AVRational{1, kAudioRate}, audio_tb);
        packet->dts = packet->pts;
        if (av_interleaved_write_frame(format, packet) < 0) {
            success = false;
            break;
        }
        av_packet_unref(packet);
    }
    av_write_trailer(format);
    avio_close(format->pb);
    avformat_free_context(format);
    avcodec_free_context(&codec);
    sws_freeContext(scaler);
    av_frame_free(&yuv);
    av_packet_free(&packet);
    return success;
}

// MP4 fixture with an INTERFRAME codec (MPEG-4) — a keyframe every kGop
// frames — plus AAC audio: the shape of real user videos. The Matroska/MJPEG
// fixture above is all-keyframes, which makes seeking trivially correct;
// this one exercises the sparse-keyframe seek path (the reported bug).
bool writeMp4Fixture(const std::filesystem::path& path, const int keyframeInterval = 15,
    const int bFrames = 0, const int audioDurationMs = 3000, const int videoDelayMs = 0) {
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;
    constexpr int kFps = 15;
    constexpr int kFrames = 45; // 3.0 s

    constexpr int kAudioRate = 48000;
    constexpr int kAudioChannels = 2;
    constexpr int kAacSamples = 1024;

    const int frame_size = kWidth * kHeight * 3;
    std::vector<unsigned char> rgb(
        static_cast<std::size_t>(frame_size) * kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto offset = static_cast<std::size_t>(
                    frame * frame_size + (y * kWidth + x) * 3);
                rgb[offset + 0] = static_cast<unsigned char>((x * 255 / kWidth + frame * 7) & 0xff);
                rgb[offset + 1] = static_cast<unsigned char>((y * 255 / kHeight) & 0xff);
                rgb[offset + 2] = static_cast<unsigned char>(((x + y) * 255 / (kWidth + kHeight) + frame * 11) & 0xff);
            }
        }
    }

    const AVCodec* venc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (venc == nullptr) {
        return false;
    }
    AVCodecContext* vcodec = avcodec_alloc_context3(venc);
    vcodec->width = kWidth;
    vcodec->height = kHeight;
    vcodec->time_base = AVRational{1, kFps};
    vcodec->pix_fmt = AV_PIX_FMT_YUV420P;
    vcodec->gop_size = keyframeInterval;
    vcodec->max_b_frames = bFrames;
    if (avcodec_open2(vcodec, venc, nullptr) < 0) {
        avcodec_free_context(&vcodec);
        return false;
    }

    const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (aenc == nullptr) {
        avcodec_free_context(&vcodec);
        return false;
    }
    AVCodecContext* acodec = avcodec_alloc_context3(aenc);
    acodec->sample_rate = kAudioRate;
    acodec->time_base = AVRational{1, kAudioRate};
    acodec->bit_rate = 128000;
    videovault::core::ffmpeg_compat::default_channels(acodec, kAudioChannels);
    acodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (avcodec_open2(acodec, aenc, nullptr) < 0) {
        avcodec_free_context(&vcodec);
        avcodec_free_context(&acodec);
        return false;
    }

    SwsContext* scaler = sws_getContext(
        kWidth, kHeight, AV_PIX_FMT_RGB24,
        kWidth, kHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = kWidth;
    yuv->height = kHeight;
    av_frame_get_buffer(yuv, 0);
    const uint8_t* src_slices[4] = {rgb.data(), nullptr, nullptr, nullptr};
    const int src_strides[4] = {kWidth * 3, 0, 0, 0};

    AVFrame* aframe = av_frame_alloc();
    aframe->format = AV_SAMPLE_FMT_FLTP;
    aframe->sample_rate = kAudioRate;
    videovault::core::ffmpeg_compat::copy_layout_to_frame(aframe, acodec);
    aframe->nb_samples = kAacSamples;
    av_frame_get_buffer(aframe, 0);

    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, "mp4", nullptr) < 0
        || format == nullptr) {
        return false;
    }
    AVStream* video_stream = avformat_new_stream(format, nullptr);
    video_stream->time_base = AVRational{1, kFps};
    if (avcodec_parameters_from_context(video_stream->codecpar, vcodec) < 0) {
        return false;
    }
    AVStream* audio_stream = avformat_new_stream(format, nullptr);
    audio_stream->time_base = AVRational{1, kAudioRate};
    if (avcodec_parameters_from_context(audio_stream->codecpar, acodec) < 0) {
        return false;
    }

    if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        return false;
    }
    if (avformat_write_header(format, nullptr) < 0) {
        return false;
    }

    const AVRational video_tb = video_stream->time_base; // after header
    const AVRational audio_tb = audio_stream->time_base;
    const AVRational source_tb = AVRational{1, kFps};

    AVPacket* packet = av_packet_alloc();
    bool success = true;

    const auto writeVideoPacket = [&] {
        packet->stream_index = video_stream->index;
        av_packet_rescale_ts(packet, vcodec->time_base, video_tb);
        const auto delay = av_rescale_q(videoDelayMs, AVRational{1, 1000}, video_tb);
        packet->pts += delay;
        packet->dts += delay;
        const bool wrote = av_write_frame(format, packet) >= 0;
        av_packet_unref(packet);
        return wrote;
    };

    auto encode_audio_chunk = [&](const std::int64_t start_sample) {
        for (int ch = 0; ch < kAudioChannels; ++ch) {
            auto* samples = reinterpret_cast<float*>(aframe->data[ch]);
            for (int i = 0; i < kAacSamples; ++i) {
                const auto t = static_cast<double>(start_sample + i);
                samples[i] = static_cast<float>(
                    0.6 * std::sin(2.0 * 3.141592653589793 * 440.0 * t / kAudioRate));
            }
        }
        aframe->pts = start_sample;
        if (avcodec_send_frame(acodec, aframe) < 0) {
            return false;
        }
        while (avcodec_receive_packet(acodec, packet) == 0) {
            packet->stream_index = audio_stream->index;
            packet->pts = av_rescale_q(packet->pts, AVRational{1, kAudioRate}, audio_tb);
            packet->dts = packet->pts;
            if (av_write_frame(format, packet) < 0) {
                return false;
            }
            av_packet_unref(packet);
        }
        return true;
    };

    std::int64_t nextAudioSample = 0;
    for (int frame = 0; frame < kFrames; ++frame) {
        sws_scale(scaler, src_slices, src_strides, 0, kHeight, yuv->data, yuv->linesize);
        yuv->pts = frame; // codec time base, not the muxer's rewritten time base
        if (avcodec_send_frame(vcodec, yuv) < 0) {
            success = false;
            break;
        }
        while (avcodec_receive_packet(vcodec, packet) == 0) {
            if (!writeVideoPacket()) {
                success = false;
                break;
            }
        }
        if (!success) break;

        // One 1024-sample AAC packet is NOT one 15 fps picture. Generate a
        // full, matching audio duration rather than a 960 ms soundtrack.
        const auto audioEnd = static_cast<std::int64_t>(frame + 1) * kAudioRate / kFps;
        while (nextAudioSample < audioEnd) {
            if (!encode_audio_chunk(nextAudioSample)) {
                success = false;
                break;
            }
            nextAudioSample += kAacSamples;
        }
        if (!success) break;
        src_slices[0] += frame_size;
    }

    // Optional long AAC tail: video packet cadence must not dictate how much
    // resident audio a single decoder call is allowed to accumulate.
    while (success && nextAudioSample < static_cast<std::int64_t>(audioDurationMs) * kAudioRate / 1000) {
        success = encode_audio_chunk(nextAudioSample);
        nextAudioSample += kAacSamples;
    }

    // Flush both encoders.
    avcodec_send_frame(vcodec, nullptr);
    while (avcodec_receive_packet(vcodec, packet) == 0) {
        if (!writeVideoPacket()) success = false;
    }
    avcodec_send_frame(acodec, nullptr);
    while (avcodec_receive_packet(acodec, packet) == 0) {
        packet->stream_index = audio_stream->index;
        packet->pts = av_rescale_q(packet->pts, AVRational{1, kAudioRate}, audio_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
        }
        av_packet_unref(packet);
    }

    av_write_trailer(format);
    avio_close(format->pb);
    avformat_free_context(format);
    avcodec_free_context(&vcodec);
    avcodec_free_context(&acodec);
    sws_freeContext(scaler);
    av_frame_free(&yuv);
    av_frame_free(&aframe);
    av_packet_free(&packet);
    return success;
}

// Strict post-seek verification: the first decoded frame must be at/near the
// requested target (not a silent restart at 0, not the stale pre-seek
// position), pts must advance forward, and decoding must reach the target.
int checkSeek(videovault::app::MediaDecoder& decoder, const char* label,
              const std::int64_t target_ms, const std::int64_t max_gap_ms) {
    if (decoder.seek_to(target_ms) < 0) {
        std::fprintf(stderr, "%s: seek_to(%lld) returned false\n",
            label, static_cast<long long>(target_ms));
        return 1;
    }
    std::int64_t first_pts = -1;
    std::int64_t prev_pts = -1;
    std::int64_t last_pts = -1;
    bool reached_target = false;
    int frames = 0;
    for (int guard = 0; guard < 300; ++guard) {
        videovault::app::DecodedFrame frame;
        const auto step = decoder.decode_next_video_frame(&frame);
        if (step == DecodeStep::End) break;
        if (step == DecodeStep::AudioReady) {
            (void)decoder.take_audio_samples();
            continue;
        }
        if (first_pts < 0) {
            first_pts = frame.pts_ms;
        }
        if (prev_pts >= 0 && frame.pts_ms < prev_pts) {
            std::fprintf(stderr, "%s: pts went backwards (%lld -> %lld)\n",
                label, static_cast<long long>(prev_pts), static_cast<long long>(frame.pts_ms));
            return 1;
        }
        prev_pts = frame.pts_ms;
        last_pts = frame.pts_ms;
        ++frames;
        (void)decoder.take_audio_samples();
        if (frame.pts_ms >= target_ms) {
            reached_target = true;
            break;
        }
    }
    if (frames < 1) {
        std::fprintf(stderr, "%s: no frames decoded after the seek\n", label);
        return 1;
    }
    if (first_pts < target_ms - max_gap_ms) {
        std::fprintf(stderr, "%s: first post-seek frame %lld is too far before target %lld (silent restart at 0?)\n",
            label, static_cast<long long>(first_pts), static_cast<long long>(target_ms));
        return 1;
    }
    if (first_pts > target_ms + 250) {
        std::fprintf(stderr, "%s: first post-seek frame %lld is after target %lld (stale pre-seek position?)\n",
            label, static_cast<long long>(first_pts), static_cast<long long>(target_ms));
        return 1;
    }
    if (!reached_target) {
        std::fprintf(stderr, "%s: decode did not advance past the target %lld (last %lld)\n",
            label, static_cast<long long>(target_ms), static_cast<long long>(last_pts));
        return 1;
    }
    std::printf("  %s: ok (first %lld ms, reached %lld ms)\n",
        label, static_cast<long long>(first_pts), static_cast<long long>(last_pts));
    return 0;
}

// Solid red RGB PNG. No alpha plane: this is the file that used to open as
// a blank window because the converted frame's alpha byte stayed 0.
bool writeRgbPng(const std::filesystem::path& path, const int width, const int height) {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (encoder == nullptr) {
        return false;
    }
    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    if (codec == nullptr) {
        return false;
    }
    codec->width = width;
    codec->height = height;
    codec->pix_fmt = AV_PIX_FMT_RGB24;
    codec->time_base = AVRational{1, 25};
    if (avcodec_open2(codec, encoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        return false;
    }
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        return false;
    }
    for (int y = 0; y < height; ++y) {
        auto* row = frame->data[0] + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame->linesize[0]);
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = 220;
            row[x * 3 + 1] = 20;
            row[x * 3 + 2] = 30;
        }
    }
    const bool sent = avcodec_send_frame(codec, frame) >= 0;
    av_frame_free(&frame);
    AVPacket* packet = av_packet_alloc();
    const bool got = sent && avcodec_receive_packet(codec, packet) >= 0 && packet->size > 0;
    bool wrote = false;
    if (got) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(packet->data), packet->size);
        wrote = static_cast<bool>(out);
    }
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    return wrote;
}

} // namespace

int main() {
    // Unbuffered progress output: if a photo-stage check crashes, the last
    // completed step must still reach CTest instead of vanishing with the
    // buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TemporaryDirectory temp;
    using namespace videovault::core;

    const auto fixture = temp.path() / L"fixture.mkv";
    if (!writeFixture(fixture)) {
        return fail("Generating the AV fixture must succeed.");
    }

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "playback test password", testParameters());
    if (!created) {
        return fail("Creating a vault for the playback test must succeed.");
    }
    const auto imported = created.value().import_file(fixture);
    if (!imported) {
        return fail("Importing the fixture must succeed.");
    }

    // Realistic interframe fixture (MPEG-4 in MP4 + AAC, sparse keyframes) —
    // imported BEFORE the vault object is moved into the first decoder.
    const auto mp4_fixture = temp.path() / L"fixture.mp4";
    if (!writeMp4Fixture(mp4_fixture)) {
        return fail("Generating the MP4 interframe fixture must succeed.");
    }
    const auto imported2 = created.value().import_file(mp4_fixture);
    if (!imported2) {
        return fail("Importing the MP4 fixture must succeed.");
    }

    const auto longGopFixture = temp.path() / L"long-gop.mp4";
    if (!writeMp4Fixture(longGopFixture, 45, 2)) return fail("Generate long-GOP/B-frame MP4.");
    const auto longGopId = created.value().import_file(longGopFixture);
    if (!longGopId) return fail("Import long-GOP MP4.");

    const auto aacTailFixture = temp.path() / L"aac-tail.mp4";
    if (!writeMp4Fixture(aacTailFixture, 45, 2, 50000)) return fail("Generate long AAC tail.");
    const auto aacTailId = created.value().import_file(aacTailFixture);
    if (!aacTailId) return fail("Import long AAC tail.");

    const auto delayedFixture = temp.path() / L"delayed-audio.mkv";
    if (!writeFixture(delayedFixture, 2000)) return fail("Generate delayed-audio fixture.");
    const auto delayedId = created.value().import_file(delayedFixture);
    if (!delayedId) return fail("Import delayed-audio fixture.");

    const auto shiftedFixture = temp.path() / L"shifted-timestamps.mkv";
    if (!writeFixture(shiftedFixture, 0, 3000)) return fail("Generate nonzero-origin fixture.");
    const auto shiftedId = created.value().import_file(shiftedFixture);
    if (!shiftedId) return fail("Import nonzero-origin fixture.");

    const auto delayedVideoFixture = temp.path() / L"delayed-video.mp4";
    if (!writeMp4Fixture(delayedVideoFixture, 15, 0, 3000, 2000)) return fail("Generate delayed-video fixture.");
    const auto delayedVideoId = created.value().import_file(delayedVideoFixture);
    if (!delayedVideoId) return fail("Import delayed-video fixture.");

    // Three pictures at 0/15/30 seconds and 50 seconds of stereo PCM.
    // A bounded pull alone cannot bound PCM resident inside one demux call.
    const auto tailFixture = temp.path() / L"audio-tail.mkv";
    if (!writeFixture(tailFixture, 0, 0, 500, 150, 3)) return fail("Generate audio-tail fixture.");
    const auto tailId = created.value().import_file(tailFixture);
    if (!tailId) return fail("Import audio-tail fixture.");
    auto tailVault = Vault::open(root, "playback test password");
    videovault::app::MediaDecoder tailDecoder;
    QString tailError;
    if (!tailVault || !tailDecoder.open(std::make_shared<Vault>(std::move(tailVault.value())),
        tailId.value(), &tailError)) return fail("Open audio-tail fixture.");
    videovault::app::DecodedFrame tailFrame;
    for (const int target : {0, 10555, 345, 45000, 0}) {
        if (tailDecoder.seek_to(target) < 0) return fail("Seek sparse video/audio tail.");
        if (tailDecoder.resident_audio_samples() != 0) return fail("Seek must release resident PCM.");
        std::size_t pcmCount = 0, peak = 0;
        int frames = 0, yields = 0;
        bool yieldedBetweenPictures = false;
        bool ended = false;
        for (int guard = 0; guard < 10000; ++guard) {
            const auto step = tailDecoder.decode_next_video_frame(&tailFrame);
            peak = std::max(peak, tailDecoder.resident_audio_samples());
            if (peak > videovault::app::MediaDecoder::kMaxResidentAudioSamples + 9600U)
                return fail("A single decode call must yield before resident PCM exceeds its cap.");
            if (step == DecodeStep::VideoFrame) ++frames;
            if (step == DecodeStep::AudioReady) {
                ++yields;
                yieldedBetweenPictures |= frames > 0 && frames < 3;
                // Without draining, a repeated call must yield, not grow or
                // report EOF. Partial pulls retain full-block accounting.
                const auto before = tailDecoder.resident_audio_samples();
                if (tailDecoder.decode_next_video_frame(&tailFrame) != DecodeStep::AudioReady
                    || tailDecoder.resident_audio_samples() != before)
                    return fail("Audio backpressure must retain pending audio without demuxing.");
            }
            pcmCount += tailDecoder.take_audio_samples(8192).size();
            if (step == DecodeStep::End) { ended = true; break; }
        }
        while (tailDecoder.buffered_audio_samples() > 0)
            pcmCount += tailDecoder.take_audio_samples(8192).size();
        const auto expected = static_cast<std::size_t>(50000 - target) * 96;
        if (!ended || pcmCount != expected || tailDecoder.resident_audio_samples() != 0
            || (target == 0 && (frames != 3 || yields == 0 || !yieldedBetweenPictures))) {
            std::fprintf(stderr, "Tail seek %d: frames=%d yields=%d PCM=%zu expected=%zu peak=%zu\n",
                target, frames, yields, pcmCount, expected, peak);
            return fail("Sparse video/long audio tails must yield without losing PCM or video.");
        }
        std::printf("Bounded tail seek %d: %d frames, %d yields, %zu PCM, peak %zu samples.\n",
            target, frames, yields, pcmCount, peak);
    }
    tailDecoder.close();
    // The audio-only API shares the same engine, including oversized requests.
    tailDecoder.prepare_for_audio();
    auto audioTailVault = Vault::open(root, "playback test password");
    if (!audioTailVault || !tailDecoder.open(std::make_shared<Vault>(std::move(audioTailVault.value())),
        tailId.value(), &tailError)) return fail("Open audio-only tail fixture.");
    std::size_t audioOnlyCount = 0;
    while (tailDecoder.pump_audio(std::numeric_limits<int>::max(), nullptr)) {
        if (tailDecoder.resident_audio_samples() > videovault::app::MediaDecoder::kMaxResidentAudioSamples + 9600U)
            return fail("Oversized pump request must be clamped to resident PCM cap.");
        audioOnlyCount += tailDecoder.take_audio_samples(8192).size();
    }
    if (audioOnlyCount != 50000U * 96U) return fail("Bounded audio-only pump must preserve every sample.");
    tailDecoder.close();

    videovault::app::MediaDecoder decoder;
    QString open_error;
    if (!decoder.open(
            std::make_shared<Vault>(std::move(created.value())),
            imported.value(), &open_error)) {
        std::fprintf(stderr, "MediaDecoder open failed: %s\n", qPrintable(open_error));
        return EXIT_FAILURE;
    }
    if (!decoder.is_open()) {
        return fail("The decoder must report open.");
    }
    if (decoder.duration_ms() < 1500 || decoder.duration_ms() > 2500) {
        return fail("The decoded duration must match the fixture.");
    }
    if (decoder.video_width() != 64 || decoder.video_height() != 64) {
        return fail("The decoded resolution must match the fixture.");
    }
    if (!decoder.has_audio()
        || decoder.audio_sample_rate() != 48000
        || decoder.audio_channels() != 2) {
        return fail("The fixture's audio stream must be detected as 48 kHz stereo.");
    }

    // The streaming cache budget is settable and applies to the decoder.
    decoder.set_stream_cache_bytes(8U << 20);
    if (decoder.stream_cache_bytes() != (8U << 20)) {
        return fail("The streaming cache budget must be settable.");
    }

    // Decode a bounded number of frames; the fixture has 20.
    int video_frames = 0;
    bool first_frame_valid = false;
    for (int guard = 0; guard < 60; ++guard) {
        videovault::app::DecodedFrame frame;
        const auto step = decoder.decode_next_video_frame(&frame);
        if (step == DecodeStep::End) break;
        if (step == DecodeStep::AudioReady) {
            (void)decoder.take_audio_samples();
            continue;
        }
        if (video_frames == 0) {
            first_frame_valid = !frame.image.isNull()
                && frame.image.width() == 64
                && frame.image.height() == 64;
        }
        ++video_frames;
        (void)decoder.take_audio_samples();
    }
    if (video_frames < 10) {
        return fail("Fewer than 10 video frames were decoded.");
    }
    if (!first_frame_valid) {
        return fail("The first decoded frame must be a valid 64x64 image.");
    }

    // Seeking must work and produce more frames at/after the target — this
    // guards against a seek silently restarting from the beginning.
    if (decoder.seek_to(1000) < 0) {
        return fail("Seeking to 1 second must succeed.");
    }
    int post_seek_frames = 0;
    bool post_seek_at_target = false;
    for (int guard = 0; guard < 30; ++guard) {
        videovault::app::DecodedFrame frame;
        const auto step = decoder.decode_next_video_frame(&frame);
        if (step == DecodeStep::End) break;
        if (step == DecodeStep::AudioReady) {
            (void)decoder.take_audio_samples();
            continue;
        }
        if (post_seek_frames == 0 && frame.pts_ms < 950) {
            return fail("The first post-seek frame must be at or near the seek target.");
        }
        ++post_seek_frames;
    }
    if (post_seek_frames < 1) {
        return fail("Decoding after a seek must produce frames.");
    }

    // The audio path must have produced PCM samples (the fixture has sound).
    auto samples = decoder.take_audio_samples();
    if (samples.empty()) {
        return fail("Decoding the fixture must produce audio samples.");
    }

    // Display-size decoding: sws must output exactly the requested size.
    if (decoder.seek_to(0) < 0) {
        return fail("Seeking back to the start must succeed.");
    }
    decoder.set_display_size(32, 32);
    videovault::app::DecodedFrame small;
    if (decoder.decode_next_video_frame(&small) != DecodeStep::VideoFrame
        || small.image.isNull()
        || small.image.width() != 32
        || small.image.height() != 32) {
        return fail("Display-size decoding must output the requested 32x32 size.");
    }

    std::printf("Playback decode checks succeeded (%d frames, %zu audio samples).\n",
        video_frames, samples.size());

    // Match the real worker: decode reference pictures from the preceding
    // keyframe, but queue audio only from the exact target, including the
    // boundary packet's tail. Counting PCM catches offsets hidden by a seek
    // bar that advances correctly while playing old sound.
    for (const std::int64_t target : {1055, 345, 1750, 0}) {
        if (decoder.seek_to(target) < 0) return fail("PCM seek must succeed.");
        std::size_t count = 0;
        videovault::app::DecodedFrame frame;
        while (decoder.decode_next_video_frame(&frame) != DecodeStep::End) {
            count += decoder.take_audio_samples().size();
        }
        count += decoder.take_audio_samples().size();
        const auto expected = static_cast<std::size_t>((2000 - target) * 48 * 2);
        if (count != expected) {
            std::fprintf(stderr, "Seek audio at %lld ms: %zu samples, expected %zu (offset %.1f ms).\n",
                static_cast<long long>(target), count, expected,
                (static_cast<double>(count) - expected) / 96.0);
            return fail("Seek must remove preroll PCM without losing the boundary packet tail.");
        }
    }
    std::printf("Sample-exact PCM seek/preroll checks succeeded.\n");
    decoder.close();

    auto aacTailVault = Vault::open(root, "playback test password");
    videovault::app::MediaDecoder aacTailDecoder;
    if (!aacTailVault || !aacTailDecoder.open(std::make_shared<Vault>(std::move(aacTailVault.value())),
        aacTailId.value(), &open_error)) return fail("Open encrypted AAC tail.");
    std::size_t aacPcm = 0, aacPeak = 0;
    int aacFrames = 0, aacYields = 0;
    bool aacEnded = false;
    for (int guard = 0; guard < 10000; ++guard) {
        const auto step = aacTailDecoder.decode_next_video_frame(&tailFrame);
        aacPeak = std::max(aacPeak, aacTailDecoder.resident_audio_samples());
        if (step == DecodeStep::VideoFrame) ++aacFrames;
        if (step == DecodeStep::AudioReady) ++aacYields;
        aacPcm += aacTailDecoder.take_audio_samples(8192).size();
        if (step == DecodeStep::End) { aacEnded = true; break; }
    }
    while (aacTailDecoder.buffered_audio_samples() > 0)
        aacPcm += aacTailDecoder.take_audio_samples(8192).size();
    // AAC pads the last input to a 1024-frame boundary. Initial encoder delay
    // is timestamped before zero and trimmed, not counted as soundtrack.
    constexpr std::size_t expectedAacPcm = ((50000U * 48U + 1023U) / 1024U) * 1024U * 2U;
    if (!aacEnded || aacFrames != 45 || aacYields == 0 || aacPcm != expectedAacPcm
        || aacPeak > videovault::app::MediaDecoder::kMaxResidentAudioSamples + 2048U
        || aacTailDecoder.resident_audio_samples() != 0) {
        std::fprintf(stderr, "AAC tail: %d frames, %d yields, %zu PCM expected %zu, peak %zu.\n",
            aacFrames, aacYields, aacPcm, expectedAacPcm, aacPeak);
        return fail("Long AAC tail and reordered video must both survive bounded codec draining.");
    }
    std::printf("Bounded AAC tail: %d frames, %d yields, %zu PCM, peak %zu samples.\n",
        aacFrames, aacYields, aacPcm, aacPeak);
    aacTailDecoder.close();

    auto delayedVault = Vault::open(root, "playback test password");
    videovault::app::MediaDecoder delayedDecoder;
    if (!delayedVault || !delayedDecoder.open(
        std::make_shared<Vault>(std::move(delayedVault.value())), delayedId.value(), &open_error)) {
        return fail("Open delayed-audio fixture.");
    }
    for (const auto target : {0, 345, 1345}) {
        if (delayedDecoder.seek_to(target) < 0) return fail("Seek delayed-audio fixture.");
        std::vector<std::int16_t> pcm;
        videovault::app::DecodedFrame frame;
        while (delayedDecoder.decode_next_video_frame(&frame) != DecodeStep::End) {
            auto part = delayedDecoder.take_audio_samples();
            pcm.insert(pcm.end(), part.begin(), part.end());
        }
        auto tail = delayedDecoder.take_audio_samples();
        pcm.insert(pcm.end(), tail.begin(), tail.end());
        const auto silent = static_cast<std::size_t>(std::max(0, 2000 - target)) * 96;
        if (pcm.size() != static_cast<std::size_t>(4000 - target) * 96
            || !std::all_of(pcm.begin(), pcm.begin() + std::min(silent, pcm.size()),
                [](std::int16_t sample) { return sample == 0; })) {
            return fail("Audio timestamps must retain the two-second delay, including after seeks.");
        }
    }
    delayedDecoder.close();
    std::printf("Timestamped audio delay and seek checks succeeded.\n");

    auto shiftedVault = Vault::open(root, "playback test password");
    videovault::app::MediaDecoder shiftedDecoder;
    if (!shiftedVault || !shiftedDecoder.open(
        std::make_shared<Vault>(std::move(shiftedVault.value())), shiftedId.value(), &open_error)) {
        return fail("Open shifted timestamp fixture.");
    }
    videovault::app::DecodedFrame shiftedFrame;
    if (shiftedDecoder.decode_next_video_frame(&shiftedFrame) != DecodeStep::VideoFrame || shiftedFrame.pts_ms != 0) {
        return fail("Nonzero container timestamps must share a zero-based playback origin.");
    }
    if (shiftedDecoder.seek_to(1055) < 0) return fail("Seek shifted timestamp fixture.");
    std::size_t shiftedPcm = 0;
    while (shiftedDecoder.decode_next_video_frame(&shiftedFrame) != DecodeStep::End) {
        shiftedPcm += shiftedDecoder.take_audio_samples().size();
    }
    shiftedPcm += shiftedDecoder.take_audio_samples().size();
    if (shiftedPcm != 90720) return fail("Shifted timestamps must seek to the same sample-exact target.");
    shiftedDecoder.close();

    // --- Strict post-seek verification on the interframe MP4 fixture. This
    // is the shape of real user videos; the MJPEG fixture above is
    // all-keyframes and cannot reproduce sparse-keyframe seek bugs.
    auto opened = Vault::open(root, "playback test password");
    if (!opened) {
        return fail("Reopening the vault for the MP4 fixture must succeed.");
    }
    videovault::app::MediaDecoder decoder2;
    QString open_error2;
    if (!decoder2.open(
            std::make_shared<Vault>(std::move(opened.value())),
            imported2.value(), &open_error2)) {
        std::fprintf(stderr, "MP4 decoder open failed: %s\n", qPrintable(open_error2));
        return EXIT_FAILURE;
    }
    if (decoder2.duration_ms() < 2500 || decoder2.duration_ms() > 3500) {
        return fail("The MP4 duration must match its 3-second fixture.");
    }
    if (decoder2.video_width() != 160 || decoder2.video_height() != 120) {
        return fail("The MP4 resolution must match the fixture.");
    }
    if (!decoder2.has_audio() || decoder2.audio_sample_rate() != 48000) {
        return fail("The MP4 fixture's AAC stream must be detected (48 kHz).");
    }

    std::printf("Strict post-seek verification (interframe MP4):\n");
    // Keyframes at 0 / 1000 / 2000 ms; max backward gap = 1000 ms (+200 slack).
    if (checkSeek(decoder2, "seek to 1500 ms", 1500, 1200) != 0) {
        return EXIT_FAILURE;
    }
    if (checkSeek(decoder2, "seek to 2500 ms", 2500, 1200) != 0) {
        return EXIT_FAILURE;
    }
    if (checkSeek(decoder2, "seek to 500 ms", 500, 1200) != 0) {
        return EXIT_FAILURE;
    }

    std::printf("MP4 interframe seek checks succeeded.\n");
    decoder2.close();

    auto longGopVault = Vault::open(root, "playback test password");
    videovault::app::MediaDecoder longGopDecoder;
    if (!longGopVault || !longGopDecoder.open(
        std::make_shared<Vault>(std::move(longGopVault.value())), longGopId.value(), &open_error)) {
        return fail("Open long-GOP fixture through the encrypted decoder.");
    }
    int bFrameCount = 0;
    videovault::app::DecodedFrame bFrame;
    for (;;) {
        const auto step = longGopDecoder.decode_next_video_frame(&bFrame);
        (void)longGopDecoder.take_audio_samples();
        if (step == DecodeStep::End) break;
        if (step == DecodeStep::VideoFrame) ++bFrameCount;
    }
    if (bFrameCount != 45) {
        std::fprintf(stderr, "B-frame decode returned %d of 45 pictures.\n", bFrameCount);
        return fail("Reordered video frames must all be drained at EOF.");
    }
    for (const std::int64_t target : {2255, 500, 1750, 0}) {
        if (longGopDecoder.seek_to(target) < 0) return fail("AAC seek must succeed.");
        std::size_t count = 0;
        videovault::app::DecodedFrame frame;
        while (longGopDecoder.decode_next_video_frame(&frame) != DecodeStep::End) {
            count += longGopDecoder.take_audio_samples().size();
        }
        count += longGopDecoder.take_audio_samples().size();
        const double remainingMs = static_cast<double>(count) / 96.0;
        // AAC padding/container rounding can add a partial packet, not seconds
        // of sound from the preceding keyframe (0 ms in this entire fixture).
        if (std::abs(remainingMs - (3000 - target)) > 80) {
            std::fprintf(stderr, "Long-GOP AAC seek %lld ms leaves %.1f ms of sound.\n",
                static_cast<long long>(target), remainingMs);
            return fail("Sparse-keyframe seek must not retain seconds of old AAC sound.");
        }
        std::printf("  Long-GOP AAC seek %lld ms: %.1f ms of sound remains.\n",
            static_cast<long long>(target), remainingMs);
    }
    longGopDecoder.close();

    // PNG is a single-frame image with no filename on the custom AVIO. The
    // viewer must still open it (image2 is extension-keyed) and return an
    // opaque frame — a transparent PNG used to decode as a hole on Windows.
    {
        static const unsigned char kTinyPng[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
            0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
            0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x4F, 0x25, 0xC4, 0xD6, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44,
            0x41, 0x54, 0x78, 0x9C, 0x63, 0xFC, 0xCF, 0xC0, 0x50, 0x0F, 0x00, 0x04,
            0x85, 0x01, 0x80, 0x5B, 0xF7, 0x9E, 0x31, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        const auto png_path = temp.path() / L"tiny.png";
        {
            std::ofstream out(png_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(kTinyPng), sizeof(kTinyPng));
            if (!out) {
                return fail("Writing the PNG fixture must succeed.");
            }
        }
        const auto png_root = temp.path() / L"png-vault";
        auto png_vault = Vault::create(png_root, "png password", testParameters());
        if (!png_vault) {
            return fail("Creating a vault for the PNG decode check must succeed.");
        }
        const auto png_id = png_vault.value().import_file(png_path);
        if (!png_id) {
            return fail("Importing a PNG must succeed.");
        }
        auto png_opened = std::make_shared<Vault>(std::move(png_vault.value()));
        videovault::app::MediaDecoder png_decoder;
        QString png_error;
        if (!png_decoder.open(png_opened, png_id.value(), &png_error)) {
            std::fprintf(stderr, "PNG decoder open failed: %s\n", qPrintable(png_error));
            return EXIT_FAILURE;
        }
        if (png_decoder.video_codec_name() != "png") {
            return fail("A PNG must open as the png codec.");
        }
        videovault::app::DecodedFrame png_frame;
        if (png_decoder.decode_next_video_frame(&png_frame) != DecodeStep::VideoFrame
            || png_frame.image.isNull()
            || png_frame.image.width() < 1
            || png_frame.image.height() < 1
            || png_frame.image.pixelColor(0, 0).alpha() != 255) {
            return fail("Decoding a PNG must produce an opaque frame.");
        }
        std::printf("PNG decode checks succeeded (%dx%d).\n",
            png_frame.image.width(), png_frame.image.height());
    }

    // --- Headless PlayerWindow seek test. Runs the REAL player on the
    // offscreen platform: worker thread, tick timer, clock re-anchor, and the
    // slider-release -> doSeek path (simulated by setting the slider value and
    // emitting sliderReleased, which the connected handler routes to doSeek).
    qputenv("QT_PLUGIN_PATH", QLibraryInfo::location(QLibraryInfo::PluginsPath).toUtf8());
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    int player_argc = 1;
    char player_name[] = "megavaultprotect-headless-player-test";
    char* player_argv[1] = {player_name};
    QApplication player_app(player_argc, player_argv);

    auto epochVault = Vault::open(root, "playback test password");
    if (!epochVault) return fail("Reopen fixture for audio epoch tests.");
    auto epochShared = std::make_shared<Vault>(std::move(epochVault.value()));
    if (!videovault::app::PlayerWindowTestAccess::checkDelayedFirstPicture(epochShared, delayedVideoId.value())) {
        return fail("A future first video timestamp must not deadlock the audio-master startup gate.");
    }
    if (!videovault::app::PlayerWindowTestAccess::checkFrameDelivery(epochShared)) {
        return fail("A due frame must reach the surface without waiting for the controls timer.");
    }
    if (!videovault::app::PlayerWindowTestAccess::checkRepeatAudioReset(epochShared)) {
        return fail("Worker-originated repeat must reset the PCM counter and discard old audio.");
    }

    const auto spin = [&player_app](const std::chrono::milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            player_app.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    if (!qEnvironmentVariableIsSet("MVP_PHOTOS_ONLY")) {
    auto reopened = Vault::open(root, "playback test password");
    if (!reopened) {
        return fail("Reopening the vault for the headless player must succeed.");
    }
    videovault::app::PlayerWindow player(
        std::make_shared<Vault>(std::move(reopened.value())),
        imported2.value(), QStringLiteral("headless seek test"));
    auto* slider = player.findChild<QSlider*>();
    if (slider == nullptr) {
        return fail("The player must expose its position slider.");
    }


    // Let playback start from the beginning.
    spin(std::chrono::milliseconds(1500));
    const int pos_before = slider->value();
    if (!videovault::app::PlayerWindowTestAccess::checkRecoveryBudget(player)) {
        return fail("Successful audio playback must reset the consecutive failure budget.");
    }
    if (pos_before < 200) {
        std::fprintf(stderr, "Playback must start advancing (slider %d ms).\n", pos_before);
        return EXIT_FAILURE;
    }

    // Simulate dragging the thumb to the middle and releasing it.
    slider->setValue(1500);
    emit slider->sliderReleased();
    spin(std::chrono::milliseconds(800));
    const int pos_after = slider->value();
    if (pos_after < 1100 || pos_after > 2900) {
        std::fprintf(stderr, "Seek to 1500 ms failed: slider at %d ms.\n", pos_after);
        return EXIT_FAILURE;
    }
    spin(std::chrono::milliseconds(800));
    const int pos_later = slider->value();
    if (pos_later <= pos_after) {
        std::fprintf(stderr, "Playback did not advance past the seek point (%d -> %d ms).\n",
            pos_after, pos_later);
        return EXIT_FAILURE;
    }

    // Simulate seeking backwards.
    slider->setValue(400);
    emit slider->sliderReleased();
    spin(std::chrono::milliseconds(800));
    const int pos_back = slider->value();
    if (pos_back < 0 || pos_back > 900) {
        std::fprintf(stderr, "Seek back to 400 ms failed: slider at %d ms.\n", pos_back);
        return EXIT_FAILURE;
    }

    std::printf("Headless player seek checks succeeded (%d -> %d -> %d -> %d ms).\n",
        pos_before, pos_after, pos_later, pos_back);
    if (!videovault::app::PlayerWindowTestAccess::checkPausedSeeking(player)) {
        return fail("Rapid paused seeks must keep audio paused, retain the last target and resume in sync.");
    }
    if (!videovault::app::PlayerWindowTestAccess::checkRepeatedPlayback(player)) {
        return fail("Repeated playback must reset its clock on every loop.");
    }
    }

    // A real RGB photo, not a 1x1 test pattern, opened in the actual window.
    // This is the path that stayed black: the player must show an opaque frame.
    {
        std::printf("Photo block: writing the PNG...\n");
        const auto photo_path = temp.path() / L"photo.png";
        if (!writeRgbPng(photo_path, 320, 240)) {
            return fail("Writing a 320x240 PNG must succeed.");
        }
        std::printf("Photo block: creating the vault...\n");
        const auto photo_root = temp.path() / L"photo-vault";
        auto photo_vault = Vault::create(photo_root, "photo password", testParameters());
        if (!photo_vault) {
            return fail("Creating a vault for the photo PNG must succeed.");
        }
        std::printf("Photo block: importing...\n");
        const auto photo_id = photo_vault.value().import_file(photo_path);
        if (!photo_id) {
            return fail("Importing a 320x240 PNG must succeed.");
        }
        std::printf("Photo block: thumbnail...\n");
        auto photo_opened = std::make_shared<Vault>(std::move(photo_vault.value()));
        const auto thumb = photo_opened->thumbnail(photo_id.value());
        if (!thumb || thumb.value().bytes.empty()) {
            return fail("A 320x240 PNG must get a thumbnail at import.");
        }
        std::printf("Photo block: constructing the player...\n");
        std::fflush(stdout);
        videovault::app::PlayerWindow photo_player(
            photo_opened, photo_id.value(), QStringLiteral("photo.png"));
        std::printf("Photo block: player constructed, waiting for the picture...\n");
        std::fflush(stdout);
        spin(std::chrono::milliseconds(800));
        auto* surface = photo_player.findChild<videovault::app::VideoSurface*>();
        if (surface == nullptr) {
            return fail("The player must have a video surface.");
        }
        if (surface->frame().isNull()) {
            std::fprintf(stderr, "PNG player status: %s\n",
                qPrintable(surface->statusText()));
            return fail("Opening a PNG in the player must show the picture.");
        }
        const QColor pixel = surface->frame().pixelColor(
            surface->frame().width() / 2, surface->frame().height() / 2);
        if (pixel.alpha() != 255 || pixel.red() < 180) {
            std::fprintf(stderr, "PNG pixel rgba %d %d %d %d\n",
                pixel.red(), pixel.green(), pixel.blue(), pixel.alpha());
            return fail("The opened PNG must be an opaque red picture, not a blank frame.");
        }
        std::printf("PNG player open checks succeeded (%dx%d).\n",
            surface->frame().width(), surface->frame().height());

        std::printf("Photo block: initial-still construction...\n");
        std::fflush(stdout);
        auto initialStill = makeStillPlayer<videovault::app::PlayerWindow>(photo_opened, photo_id.value());
        if (!initialStill) {
            return fail("The constructor must support initial still-image mode without a video launch.");
        }
        if (videovault::app::PlayerWindowTestAccess::videoWorkerStarted(*initialStill)) {
            return fail("An initial still must never start an FFmpeg worker.");
        }
        spin(std::chrono::milliseconds(800));
        if (initialStill->findChild<videovault::app::VideoSurface*>()->frame().size() != QSize(320, 240)) {
            return fail("Initial still mode must load the full-resolution photo.");
        }
        initialStill->playStill(photo_opened, photo_id.value());
        if (videovault::app::PlayerWindowTestAccess::videoWorkerStarted(*initialStill)) {
            return fail("Still navigation must not launch an FFmpeg worker.");
        }
        std::printf("Initial still/navigation no-FFmpeg checks succeeded.\n");

        // Occupy every global-pool slot. Photo opening must have its own
        // bounded executor, independent of imports and gallery work.
        std::printf("Photo block: global-pool starvation check...\n");
        std::fflush(stdout);
        auto* global = QThreadPool::globalInstance();
        const int oldLimit = global->maxThreadCount();
        global->setMaxThreadCount(1);
        QSemaphore started, release;
        auto blocker = QtConcurrent::run([&] { started.release(); release.acquire(); });
        started.acquire();
        photo_player.playStill(photo_opened, photo_id.value());
        spin(std::chrono::milliseconds(800));
        const bool independentPhoto = !surface->frame().isNull()
            && surface->frame().size() == QSize(320, 240);
        release.release();
        blocker.waitForFinished();
        global->setMaxThreadCount(oldLimit);
        if (!independentPhoto) {
            return fail("Still photo opening must not starve behind the global thread pool.");
        }
        std::printf("Still photo dedicated-pool/full-resolution checks succeeded.\n");

        const std::filesystem::path png_samples[] = {
            std::filesystem::path("docs") / "MVP-logo.png",
            std::filesystem::path("docs") / "MVP-logo-banner.png",
        };
        for (const auto& relative : png_samples) {
            const auto sample = repoPath(relative);
            if (sample.empty()) {
                std::fprintf(stderr, "Missing repo PNG %ls\n", relative.c_str());
                return fail("A real PNG from the repo must be present.");
            }
            const auto sample_id = photo_opened->import_file(sample);
            if (!sample_id) {
                std::fprintf(stderr, "Import failed for %ls\n", sample.c_str());
                return fail("Importing a real PNG from the repo must succeed.");
            }
            videovault::app::MediaDecoder sample_decoder;
            QString sample_error;
            if (!sample_decoder.open(photo_opened, sample_id.value(), &sample_error)) {
                std::fprintf(stderr, "Open failed for %ls: %s\n",
                    sample.c_str(), qPrintable(sample_error));
                return fail("A real PNG must open in the viewer.");
            }
            videovault::app::DecodedFrame sample_frame;
            if (sample_decoder.decode_next_video_frame(&sample_frame) != DecodeStep::VideoFrame
                || sample_frame.image.isNull()
                || sample_frame.image.pixelColor(0, 0).alpha() != 255
                || sample_frame.image.constBits()[3] != 255) {
                std::fprintf(stderr, "Decode failed for %ls (%dx%d codec %s)\n",
                    sample.c_str(), sample_decoder.video_width(),
                    sample_decoder.video_height(),
                    sample_decoder.video_codec_name().c_str());
                return fail("A real PNG must decode to an opaque frame.");
            }
            std::printf("Real PNG opened: %ls %dx%d %s\n",
                sample.c_str(), sample_frame.image.width(), sample_frame.image.height(),
                sample_decoder.video_codec_name().c_str());
        }
    }
    return EXIT_SUCCESS;
}
