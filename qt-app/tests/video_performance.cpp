// Opt-in, visible native playback measurement. No hardware-dependent pass mark.
// MVP_VIDEO_PERF_MODE=all|1080p|4k (default all), MVP_VIDEO_PERF_SECONDS=2..3,
// MVP_VIDEO_PERF_ROOT=absolute scratch directory, MVP_VIDEO_PERF_WINDOW_WIDTH=1280.
// MVP_VIDEO_PERF_B_FRAMES=1 enables MPEG4 max_b_frames=2, with exact frame-count assertion.
// Paint-event counts measure Qt raster dispatch, NOT compositor/monitor scanout.
#include "videovault/core/ffmpeg_compat.hpp"
#include "videovault/core/vault.hpp"
#include "videovault/app/media_decoder.hpp"
#include "videovault/app/player_window.hpp"
#include <QApplication>
#include <QAudioDeviceInfo>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSettings>
#include <QWindow>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <set>
#include <vector>

namespace videovault::app {
struct PlayerWindowTestAccess {
    static bool active(const PlayerWindow& p) {
        return p.audioOutput_ && p.audioOutput_->state() == QAudio::ActiveState;
    }
    static quint64 consumed(const PlayerWindow& p) {
        return p.audioSink_ ? p.audioSink_->consumed_samples() : 0;
    }
    static int audioError(const PlayerWindow& p) {
        return p.audioOutput_ ? int(p.audioOutput_->error()) : -1;
    }
    static bool exerciseControls(PlayerWindow& p) {
        const bool initial = p.playing_.load();
        p.playButton_->click();
        const bool toggled = p.playing_.load() != initial;
        p.playButton_->click();
        const bool restored = p.playing_.load() == initial;
        p.fullscreenButton_->click();
        const bool full = p.isFullScreen();
        p.fullscreenButton_->click();
        p.showChrome();
        p.uiHideTimer_.stop();
        return toggled && restored && full && !p.isFullScreen();
    }
};
}
namespace {
using namespace videovault;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(int value, const char* message) { require(value >= 0, message); }
std::filesystem::path fs(const QString& s) { return std::filesystem::path(s.toStdWString()); }
struct Fixture {
    AVFormatContext* mux = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    AVFrame *picture = nullptr, *sound = nullptr;
    AVPacket* packet = nullptr;
    ~Fixture() {
        av_packet_free(&packet); av_frame_free(&picture); av_frame_free(&sound);
        avcodec_free_context(&video); avcodec_free_context(&audio);
        if (mux) { if (mux->pb) avio_closep(&mux->pb); avformat_free_context(mux); }
    }
    void send(AVCodecContext* codec, AVFrame* frame, AVStream* stream) {
        check(avcodec_send_frame(codec, frame), "encoder send");
        int ret;
        while ((ret = avcodec_receive_packet(codec, packet)) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(mux, packet), "mux packet");
            av_packet_unref(packet);
        }
        require(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF, "encoder receive");
    }
};
void generate(const QString& path, int w, int h, int frames) {
    Fixture f;
    check(avformat_alloc_output_context2(&f.mux, nullptr, "mp4", nullptr), "allocate MP4");
    const auto* venc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const auto* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    require(venc && aenc, "MPEG4 and AAC encoders required");
    f.video = avcodec_alloc_context3(venc); f.audio = avcodec_alloc_context3(aenc);
    require(f.video && f.audio, "allocate codecs");
    auto* v = f.video; auto* a = f.audio;
    v->width = w; v->height = h; v->pix_fmt = AV_PIX_FMT_YUV420P;
    v->time_base = AVRational{1,60}; v->framerate = AVRational{60,1};
    v->gop_size = 60; v->max_b_frames = qEnvironmentVariableIsSet("MVP_VIDEO_PERF_B_FRAMES") ? 2 : 0;
    v->thread_count = 4;
    v->flags |= AV_CODEC_FLAG_QSCALE; v->global_quality = 3 * FF_QP2LAMBDA;
    a->sample_rate = 48000; a->time_base = AVRational{1,48000};
    a->sample_fmt = AV_SAMPLE_FMT_FLTP; a->bit_rate = 128000;
    core::ffmpeg_compat::default_channels(a, 2);
    if (f.mux->oformat->flags & AVFMT_GLOBALHEADER) {
        v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    check(avcodec_open2(v, venc, nullptr), "open MPEG4");
    check(avcodec_open2(a, aenc, nullptr), "open AAC");
    auto* vs = avformat_new_stream(f.mux, nullptr);
    auto* as = avformat_new_stream(f.mux, nullptr);
    require(vs && as, "allocate streams");
    vs->time_base = v->time_base; vs->avg_frame_rate = v->framerate; as->time_base = a->time_base;
    check(avcodec_parameters_from_context(vs->codecpar,v), "video parameters");
    check(avcodec_parameters_from_context(as->codecpar,a), "audio parameters");
    check(avio_open(&f.mux->pb,path.toUtf8().constData(),AVIO_FLAG_WRITE), "open fixture");
    check(avformat_write_header(f.mux,nullptr), "MP4 header");
    f.picture = av_frame_alloc(); f.sound = av_frame_alloc(); f.packet = av_packet_alloc();
    require(f.picture && f.sound && f.packet, "allocate frames");
    auto* pic = f.picture; pic->format = v->pix_fmt; pic->width = w; pic->height = h;
    check(av_frame_get_buffer(pic,32), "picture buffer");
    auto* snd = f.sound; snd->format = a->sample_fmt; snd->sample_rate = a->sample_rate;
    core::ffmpeg_compat::copy_layout_to_frame(snd,a); snd->nb_samples = a->frame_size;
    check(av_frame_get_buffer(snd,0), "sound buffer");
    int64_t audioSample = 0;
    for (int n=0; n<frames; ++n) {
        check(av_frame_make_writable(pic), "writable picture");
        // One incremental YUV frame: moving gradients and spatial detail.
        for (int plane=0; plane<3; ++plane) {
            const int pw = plane ? w/2 : w, ph = plane ? h/2 : h;
            for (int y=0; y<ph; ++y) {
                auto* row = pic->data[plane] + y*pic->linesize[plane];
                for (int x=0; x<pw; ++x)
                    row[x] = uint8_t(plane ? 48 + ((x/8+y/8+n*(plane+1))%160)
                                                    : 24 + ((x/4+y/4+n*3)%208));
            }
        }
        pic->pts = n; pic->quality = v->global_quality;
        f.send(v,pic,vs);
        // AAC chunks are 1024 samples, NOT one chunk per 60fps picture.
        while (audioSample < int64_t(n+1)*48000/60) {
            check(av_frame_make_writable(snd), "writable sound");
            for (int ch=0; ch<2; ++ch) {
                auto* out = reinterpret_cast<float*>(snd->data[ch]);
                for (int j=0; j<snd->nb_samples; ++j)
                    out[j] = float(0.12*std::sin(6.283185307179586*440*(audioSample+j)/48000.0));
            }
            snd->pts = audioSample; f.send(a,snd,as); audioSample += snd->nb_samples;
        }
    }
    f.send(v,nullptr,vs); f.send(a,nullptr,as);
    check(av_write_trailer(f.mux), "MP4 trailer");
}
class PaintProbe : public QObject {
public:
    app::VideoSurface* surface = nullptr;
    QElapsedTimer clock;
    std::set<qint64> keys;
    std::vector<double> times;
    int paints = 0;
    QSize lastSize;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == surface && event->type() == QEvent::Paint && surface->isVisible()) {
            ++paints;
            const auto& image = surface->frame();
            if (!image.isNull() && keys.insert(image.cacheKey()).second) {
                times.push_back(clock.nsecsElapsed()/1e6); lastSize = image.size();
            }
        }
        return false; // actual production VideoSurface::paintEvent runs unchanged
    }
};
double percentile(std::vector<double> v, double fraction) {
    if (v.empty()) return 0;
    std::sort(v.begin(),v.end()); return v[size_t((v.size()-1)*fraction)];
}
QJsonObject measure(QApplication& application, const QString& root, const QString& label,
                    int w, int h, int count) {
    const QString fixture = root+"/"+label+".mp4";
    std::printf("GENERATE %s %dx%d60 frames=%d\n",qPrintable(fixture),w,h,count);
    generate(fixture,w,h,count);
    core::Argon2Parameters params; params.memory_kib=8192; params.iterations=2; params.parallelism=1;
    auto made = core::Vault::create(fs(root+"/vault-"+label),"disposable performance fixture",params);
    require(bool(made), "create disposable vault");
    auto vault = std::make_shared<core::Vault>(std::move(made.value()));
    const auto imported = vault->import_file(fs(fixture));
    require(bool(imported), "encrypted import");
    QJsonObject result{{"label",label},{"source_width",w},{"source_height",h},
        {"source_fps",60},{"expected_frames",count},{"codec","mpeg4+aac"},
        {"encoder_max_b_frames",qEnvironmentVariableIsSet("MVP_VIDEO_PERF_B_FRAMES") ? 2 : 0},
        {"fixture_bytes",double(QFile(fixture).size())}};
    {
        app::MediaDecoder decoder; QString error;
        QElapsedTimer decode; decode.start();
        require(decoder.open(vault,imported.value(),&error),qPrintable(error));
        const double opened = decode.nsecsElapsed()/1e6;
        require(decoder.video_width()==w && decoder.video_height()==h && decoder.has_audio(),"fixture metadata");
        int frames=0; quint64 pcm=0; qint64 lastPts=-1; app::DecodedFrame frame;
        for (;;) {
            const auto step = decoder.decode_next_video_frame(&frame);
            pcm+=decoder.take_audio_samples().size();
            if (step == app::DecodeStep::End) break;
            if (step == app::DecodeStep::AudioReady) continue;
            require(frame.pts_ms>lastPts && frame.image.size()==QSize(w,h),"decode PTS/resolution");
            lastPts=frame.pts_ms; ++frames;
        }
        pcm+=decoder.take_audio_samples().size();
        const double elapsed=decode.nsecsElapsed()/1e6;
        result["decode_open_ms"]=opened; result["decode_full_resolution_ms"]=elapsed-opened;
        result["decode_fps"]=frames*1000.0/(elapsed-opened); result["decoded_frames"]=frames;
        result["decoded_pcm_interleaved_samples"]=double(pcm); result["decoded_last_pts_ms"]=double(lastPts);
        require(frames==count && pcm>0,"all source frames and audio must decode");
        require(std::abs(double(pcm)/96.0-count*1000.0/60)<80,"AAC duration must match video");
    }
    // Remove plaintext: both measurements/playback now use only the encrypted vault.
    require(QFile::remove(fixture),"remove plaintext fixture");
    PaintProbe probe; probe.clock.start();
    app::PlayerWindow player(vault,imported.value(),label+" native 60fps measurement");
    player.setAttribute(Qt::WA_DeleteOnClose,false);
    player.resize(qBound(640,qEnvironmentVariableIntValue("MVP_VIDEO_PERF_WINDOW_WIDTH") ?
        qEnvironmentVariableIntValue("MVP_VIDEO_PERF_WINDOW_WIDTH") : 1280,3840),800);
    probe.surface=player.findChild<app::VideoSurface*>(); require(probe.surface,"video surface");
    probe.surface->installEventFilter(&probe);
    bool ended=false;
    QObject::connect(&player,&app::PlayerWindow::playbackFinished,&player,[&]{ended=true;});
    player.show(); player.raise(); player.activateWindow();
    QEventLoop loop; QTimer heartbeat,deadline; heartbeat.setTimerType(Qt::PreciseTimer);
    std::vector<double> gaps; double previous=probe.clock.nsecsElapsed()/1e6;
    int activeTicks=0, observedTicks=0, audioError=-1; quint64 consumed=0;
    bool screenshot=false, exposed=false; QSize surfaceSize;
    const QString screenshotPath=root+"/"+label+"-windows.png";
    QObject::connect(&heartbeat,&QTimer::timeout,&loop,[&]{
        const double now=probe.clock.nsecsElapsed()/1e6;
        gaps.push_back(now-previous); previous=now; ++observedTicks;
        if (app::PlayerWindowTestAccess::active(player)) ++activeTicks;
        consumed=std::max(consumed,app::PlayerWindowTestAccess::consumed(player));
        audioError=app::PlayerWindowTestAccess::audioError(player);
        exposed |= player.windowHandle() && player.windowHandle()->isExposed();
        surfaceSize=probe.surface->size();
        if (ended || now>count*1000.0/60+4500) loop.quit();
    });
    // Desktop capture is taken after measurement: grabWindow itself must not
    // add an expensive screenshot stall to the heartbeat/paint statistics.
    heartbeat.start(10); deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    deadline.start(count*1000/60+8000); loop.exec(); heartbeat.stop();
    probe.surface->removeEventFilter(&probe);
    if (player.screen()) screenshot=player.screen()->grabWindow(player.winId()).save(screenshotPath);
    const bool controlsWorked = app::PlayerWindowTestAccess::exerciseControls(player);
    QEventLoop settle;
    QTimer::singleShot(150, &settle, &QEventLoop::quit);
    settle.exec();
    const QString controlsScreenshot = root+"/"+label+"-controls-windows.png";
    const bool controlsSaved = player.screen()
        && player.screen()->grabWindow(player.winId()).save(controlsScreenshot);
    std::vector<double> intervals;
    for (size_t i=1;i<probe.times.size();++i) intervals.push_back(probe.times[i]-probe.times[i-1]);
    result["window_exposed"]=exposed; result["playback_finished"]=ended;
    result["paint_events"]=probe.paints; result["unique_frames_painted"]=int(probe.keys.size());
    result["painted_frame_width"]=probe.lastSize.width(); result["painted_frame_height"]=probe.lastSize.height();
    result["surface_width"]=surfaceSize.width(); result["surface_height"]=surfaceSize.height();
    result["paint_fps_first_to_last"]=probe.times.size()>1 ?
        (probe.times.size()-1)*1000.0/(probe.times.back()-probe.times.front()) : 0;
    result["first_frame_ms"]=probe.times.empty() ? -1 : probe.times.front();
    result["paint_interval_p95_ms"]=percentile(intervals,.95);
    result["paint_interval_max_ms"]=percentile(intervals,1);
    result["heartbeat_ticks"]=observedTicks; result["heartbeat_p95_ms"]=percentile(gaps,.95);
    result["heartbeat_max_ms"]=percentile(gaps,1); result["audio_active_ticks"]=activeTicks;
    result["audio_consumed_interleaved_samples"]=double(consumed); result["final_audio_error"]=audioError;
    result["screenshot"]=screenshotPath; result["screenshot_saved"]=screenshot;
    result["controls_verified"]=controlsWorked;
    result["controls_screenshot"]=controlsScreenshot;
    result["controls_screenshot_saved"]=controlsSaved;
    result["observation_ms"]=previous;
    std::printf("RESULT %s\n",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    require(exposed && screenshot && !probe.keys.empty() && controlsWorked && controlsSaved,
        "visible native paint, working playback/fullscreen controls and screenshots required");
    const auto minimumFps = qEnvironmentVariableIntValue("MVP_VIDEO_PERF_MIN_PAINT_FPS");
    if (minimumFps > 0) require(result["paint_fps_first_to_last"].toDouble() >= minimumFps,
        "native frame delivery is below the requested performance threshold");
    player.hide(); application.processEvents();
    return result;
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        QString base=qEnvironmentVariable("MVP_VIDEO_PERF_ROOT");
        if (base.isEmpty()) base=qEnvironmentVariable("LOCALAPPDATA")+"/hermes/cache/scratch";
        require(QDir::isAbsolutePath(base),"MVP_VIDEO_PERF_ROOT must be absolute");
        const QString root=QDir(base).absoluteFilePath("video-performance-"+
            QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss-zzz"));
        require(QDir().mkpath(root),"create scratch run directory");
        qputenv("QT_QPA_PLATFORM","windows");
        QApplication application(argc,argv); application.setQuitOnLastWindowClosed(false);
        application.setOrganizationName("MegaVaultDisposableVideoPerformance");
        application.setApplicationName("NativeVideoPerformance-"+QString::number(application.applicationPid()));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,root+"/settings");
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,root+"/settings-system");
        require(application.platformName()=="windows","actual Windows Qt plugin required");
        std::printf("Native Qt platform=%s output=%s audio_device=%s\n",qPrintable(application.platformName()),
                    qPrintable(root),qPrintable(QAudioDeviceInfo::defaultOutputDevice().deviceName()));
        const QString mode=qEnvironmentVariable("MVP_VIDEO_PERF_MODE","all");
        require(mode=="all"||mode=="1080p"||mode=="4k","mode must be all,1080p,4k");
        bool ok=false; double seconds=qEnvironmentVariable("MVP_VIDEO_PERF_SECONDS").toDouble(&ok);
        if (!ok) seconds=2.5; require(seconds>=2 && seconds<=3,"seconds must be 2..3");
        const int count=int(std::round(seconds*60)); QJsonArray results;
        if (mode=="all"||mode=="1080p") results.append(measure(application,root,"1080p60",1920,1080,count));
        if (mode=="all"||mode=="4k") results.append(measure(application,root,"4k60",3840,2160,count));
        QJsonObject report{{"platform",application.platformName()},{"qt_version",qVersion()},
            {"paint_metric","unique QImage cache keys dispatched to unmodified visible VideoSurface paintEvent; not monitor scanout"},
            {"results",results}};
        QFile file(root+"/results.json"); require(file.open(QIODevice::WriteOnly),"write report");
        require(file.write(QJsonDocument(report).toJson())>0,"save report"); file.close();
        std::printf("REPORT %s/results.json\n",qPrintable(root)); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}
