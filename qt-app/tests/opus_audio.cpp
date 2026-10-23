// Real Ogg/Opus, generated in RAM by the linked FFmpeg encoder. No GUI/device.
#include "videovault/app/local_audio_pipeline.hpp"
#include "videovault/app/byte_source.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}
namespace core = videovault::core;
using namespace videovault::app;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n'; return 1; } } while(false)
void require(int result) { if (result < 0) throw std::runtime_error("FFmpeg fixture generation failed: " + std::to_string(result)); }
constexpr int seconds = 90;
// Native experimental encoder is used only for test fixture creation, NOT playback.
// This synthesizes a tone, not a counterfeit encoded byte sequence. The actual
// encoder and Ogg muxer produce every byte consumed by the production decoder.
std::vector<unsigned char> fixture() {
    const AVCodec* codec = avcodec_find_encoder_by_name("opus");
    if (!codec) throw std::runtime_error("FFmpeg native opus fixture encoder is unavailable");
    struct Resources {
        AVCodecContext* encoder = nullptr;
        AVFormatContext* mux = nullptr;
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        ~Resources() {
            av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&encoder);
            if (mux) { if (mux->pb) { unsigned char* data = nullptr; avio_close_dyn_buf(mux->pb, &data); av_free(data); } avformat_free_context(mux); }
        }
    } r;
    r.encoder = avcodec_alloc_context3(codec);
    if (!r.encoder || !r.frame || !r.packet) throw std::bad_alloc();
    auto* e = r.encoder;
    e->sample_rate = 48000; e->time_base = {1,48000}; e->sample_fmt = AV_SAMPLE_FMT_FLTP;
    e->bit_rate = 192000; e->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&e->ch_layout, 2);
#else
    e->channels = 2; e->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    require(avcodec_open2(e, codec, nullptr));
    require(avformat_alloc_output_context2(&r.mux, nullptr, "ogg", nullptr));
    AVStream* stream = avformat_new_stream(r.mux, nullptr);
    if (!stream) throw std::bad_alloc();
    stream->time_base = e->time_base;
    require(avcodec_parameters_from_context(stream->codecpar, e));
    av_dict_set(&stream->metadata, "title", "Encrypted Opus regression", 0);
    require(avio_open_dyn_buf(&r.mux->pb));
    require(avformat_write_header(r.mux, nullptr));
    auto* frame = r.frame;
    frame->format = e->sample_fmt; frame->sample_rate = e->sample_rate; frame->nb_samples = e->frame_size;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    require(av_channel_layout_copy(&frame->ch_layout, &e->ch_layout));
#else
    frame->channel_layout = e->channel_layout; frame->channels = e->channels;
#endif
    require(av_frame_get_buffer(frame, 0));
    const auto drain = [&] {
        int status;
        while ((status = avcodec_receive_packet(e, r.packet)) >= 0) {
            av_packet_rescale_ts(r.packet, e->time_base, stream->time_base);
            r.packet->stream_index = stream->index;
            require(av_interleaved_write_frame(r.mux, r.packet));
            av_packet_unref(r.packet);
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) require(status);
    };
    for (int at = 0; at < seconds * 48000; at += frame->nb_samples) {
        require(av_frame_make_writable(frame));
        for (int channel = 0; channel < 2; ++channel) {
            auto* data = reinterpret_cast<float*>(frame->data[channel]);
            for (int i = 0; i < frame->nb_samples; ++i)
                data[i] = static_cast<float>(0.2 * std::sin(6.283185307179586 * (channel ? 660 : 440) * (at+i) / 48000));
        }
        frame->pts = at; require(avcodec_send_frame(e, frame)); drain();
    }
    require(avcodec_send_frame(e, nullptr)); drain(); require(av_write_trailer(r.mux));
    unsigned char* data = nullptr;
    const int size = avio_close_dyn_buf(r.mux->pb, &data); r.mux->pb = nullptr;
    require(size);
    std::vector<unsigned char> result(data, data + size); av_free(data);
    return result;
}
// Instrument only the ByteSource boundary; decryption/authentication is always
// the real vault range API. Small read fragments exercise incremental demux.
class Ranges final : public ByteSource {
public:
    std::shared_ptr<core::Vault> vault;
    std::int64_t id, length, position = 0;
    std::size_t bytes = 0, reads = 0;
    Ranges(std::shared_ptr<core::Vault> v, std::int64_t selected, std::int64_t size)
        : vault(std::move(v)), id(selected), length(size) {}
    int read(unsigned char* output, int count) override {
        auto result = vault->read_video_range(id, position, std::min(count, 4096));
        if (!result) return AVERROR(EACCES);
        const auto& data = result.value();
        if (!data.empty()) std::memcpy(output, data.data(), data.size());
        position += data.size(); bytes += data.size(); ++reads;
        return static_cast<int>(data.size());
    }
    std::int64_t seek(std::int64_t offset, int whence) override {
        if (whence == kSeekSize) return length;
        const auto target = (whence == SEEK_CUR ? position : whence == SEEK_END ? length : 0) + offset;
        if (target < 0 || target > length) return -1;
        return position = target;
    }
    std::int64_t size() const override { return length; }
};
std::size_t decode(LocalAudioReader& reader, double* energy) {
    std::vector<std::int16_t> block;
    std::size_t count = 0;
    while (reader.read(&block)) { count += block.size(); for (const auto sample : block) *energy += double(sample) * sample; }
    return count;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        CHECK(decoder && av_find_input_format("ogg"));
        AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_OPUS);
        CHECK(parser); av_parser_close(parser);
        std::cout << "Linked FFmpeg " << av_version_info() << ": decoder=" << decoder->name
                  << ", Ogg demuxer and Opus parser present; libopus decoder="
                  << (avcodec_find_decoder_by_name("libopus") ? "yes" : "no") << '\n';
        auto encoded = fixture();
        CHECK(encoded.size() > (1U << 20));
        CHECK(std::memcmp(encoded.data(), "OggS", 4) == 0);
        QTemporaryDir dir(qEnvironmentVariable("TMPDIR", QDir::tempPath()) + "/mvp-opus-XXXXXX");
        CHECK(dir.isValid());
        const auto root = std::filesystem::u8path(dir.path().toUtf8().constData());
        auto created = core::Vault::create(root / "vault", "opus-regression", {8192,1,1}); CHECK(created);
        auto vault = std::make_shared<core::Vault>(std::move(created.value()));
        // A different catalog row must not become the selected track.
        auto decoy = vault->import_bytes(std::vector<unsigned char>{1,2,3,4}, "not-a-song.bin"); CHECK(decoy);
        auto selected = vault->import_bytes(encoded, "selected.opus"); CHECK(selected);
        auto identical = vault->read_video_bytes(selected.value()); CHECK(identical && identical.value() == encoded);
        identical.value().clear();
        const auto files = [&] {
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if (entry.is_regular_file()) paths.push_back(entry.path());
            std::sort(paths.begin(), paths.end()); return paths;
        };
        const auto before = files();
        QString error;
        Ranges ranges(vault, selected.value(), static_cast<std::int64_t>(encoded.size()));
        MediaDecoder incremental; incremental.prepare_for_audio();
        CHECK(incremental.open_source(&ranges, &error, "selected.opus"));
        CHECK(incremental.has_audio() && !incremental.has_video());
        CHECK(incremental.pump_audio(9600, nullptr));
        const auto first = incremental.take_audio_samples();
        CHECK(!first.empty() && ranges.reads > 1 && ranges.bytes < encoded.size());
        CHECK(std::any_of(first.begin(), first.end(), [](auto s) { return std::abs(int(s)) > 100; }));
        std::cout << "Progressive encrypted selected-row read: " << ranges.bytes << '/' << encoded.size()
                  << " bytes, " << ranges.reads << " bounded reads before first PCM.\n";
        LocalAudioReader local;
        if (!local.open(vault, selected.value(), "selected.opus", &error)) { std::cerr << error.toStdString() << '\n'; return 1; }
        CHECK(std::abs(local.duration() - seconds*1000) < 100);
        CHECK(local.tags().title == "Encrypted Opus regression");
        double energy = 0;
        const auto count = decode(local, &energy);
        CHECK(count >= std::size_t(seconds*48000*2 - 4000) && count <= std::size_t(seconds*48000*2 + 4000));
        const auto rms = std::sqrt(energy / count); CHECK(rms > 1000 && rms < 10000);
        CHECK(local.seek(45000));
        double seekEnergy = 0; const auto remaining = decode(local, &seekEnergy);
        CHECK(remaining >= 44U*48000*2 && remaining <= 46U*48000*2);
        CHECK(seekEnergy > 0);
        CHECK(local.seek(0));
        std::vector<std::int16_t> block; CHECK(local.read(&block) && !block.empty());
        std::cout << "LocalAudioReader: " << count << " s16 stereo samples, RMS=" << rms
                  << ", duration=" << local.duration() << "ms; midpoint seek samples=" << remaining << ".\n";
        vault->lock();
        CHECK(!local.read(&block) && block.empty()); CHECK(!local.seek(0));
        CHECK(!vault->read_video_range(selected.value(), 0, 1024));
        LocalAudioReader locked; CHECK(!locked.open(vault, selected.value(), "selected.opus", &error));
        incremental.close();
        CHECK(files() == before);
        // A plaintext Ogg staging file anywhere in this isolated test tree is a
        // failure. This checks disk artifacts, not OS paging or memory dumps.
        for (const auto& path : files()) {
            std::ifstream input(path, std::ios::binary); char magic[4]{}; input.read(magic, 4);
            CHECK(std::memcmp(magic, "OggS", 4) != 0);
            CHECK(path.extension() != ".opus" && path.extension() != ".ogg" && path.extension() != ".wav");
        }
        std::cout << "Byte-identical encrypted import, no playback-created files/plaintext staging in test tree, hard-lock read/seek/open rejection passed.\n";
        return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
