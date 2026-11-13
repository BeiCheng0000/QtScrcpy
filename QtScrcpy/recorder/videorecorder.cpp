#include "videorecorder.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// FFmpeg 5.1 将 AVCodecContext/AVFrame 的声道布局字段迁移到了
// AVChannelLayout。保留两套代码路径，使同一份源码既能针对 scrcpy 打包的
// FFmpeg（4.x）编译，也能针对更新版本编译。
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define FFMPEG_HAS_CH_LAYOUT 1
#else
#define FFMPEG_HAS_CH_LAYOUT 0
#endif

namespace
{
const int kMaxVideoQueue = 8;          // 帧数，30fps 下约 250ms
const int kAudioPendingMaxMs = 250;    // sndcpy TCP 流的抖动缓冲上限
const int kAudioWarmupMs = 100;        // 丢弃 sndcpy 启动时的积压数据
const int kAudioPumpPeriodMs = 5;      // 音频时钟泵送间隔
const int kAudioCatchUpMaxMs = 200;    // 编码器卡顿后最多追帧时长

const char *kVideoEncoderCandidates[] = {
    "libx264",
    "h264_nvenc",
    "h264_qsv",
    "h264_mf",
    "h264_videotoolbox",
    "mpeg4",
};

void fillBlack(AVFrame *frame, int height)
{
    const int heights[3] = {height, (height + 1) / 2, (height + 1) / 2};
    for (int i = 0; i < 3; ++i) {
        const uint8_t value = (i == 0) ? 16 : 128;
        std::memset(frame->data[i], value, static_cast<size_t>(frame->linesize[i]) * heights[i]);
    }
}

// av_err2str() uses a C compound literal and does not compile as C++.
QString ffErrorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}
} // namespace

class VideoRecorder::Impl
{
public:
    struct VideoFrame
    {
        std::vector<uint8_t> y;
        std::vector<uint8_t> u;
        std::vector<uint8_t> v;
        int width = 0;
        int height = 0;
        int strideY = 0;
        int strideU = 0;
        int strideV = 0;
        qint64 ptsMs = 0;
    };

    QString finalPath;
    QString tempPath;
    int encW = 0;
    int encH = 0;
    int fps = 30;
    int sampleRate = 48000;
    int channels = 2;
    bool audioEnabled = false;

    AVFormatContext *fmt = nullptr;
    AVStream *vStream = nullptr;
    AVStream *aStream = nullptr;
    AVCodecContext *vCtx = nullptr;
    AVCodecContext *aCtx = nullptr;
    AVFrame *vFrame = nullptr;
    AVFrame *aFrame = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
    SwrContext *swr = nullptr;

    std::deque<VideoFrame> vQueue;
    // sndcpy 提供的是原始字节流：需缓存字节直到凑满一整帧 PCM
    // (or a whole AAC frame) can be consumed. Never split on TCP boundaries.
    std::vector<uint8_t> audioPending;
    std::vector<int16_t> audioAccum; // 交错存放的 s16 样本，仅工作线程使用
    int audioFrameSize = 1024;
    qint64 audioWrittenSamples = 0; // 逻辑音频时间轴，仅工作线程/停止时使用
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::thread worker;

    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    VideoRecorder *owner = nullptr;
    bool started = false; // start() 成功后置 true，stop() 必须等待工作线程结束
    bool headerWritten = false;
    bool failed = false;
    QString error;

    QElapsedTimer clock;
    qint64 lastVPts = -1;
    std::atomic<qint64> audioSamples{0};
    std::atomic<qint64> videoFrameCount{0};
    int swsSrcW = 0;
    int swsSrcH = 0;

    bool open(const QString &path, int width, int height, int videoFps, int videoBitrate, int audioSampleRate,
              int audioChannels, int audioBitrate);
    void closeAll();
    bool flushEncoders();
    bool initVideo(int videoBitrate);
    bool initAudio(int audioBitrate);
    void workerLoop();
    bool encodeVideo(VideoFrame &frame);
    bool pumpAudio();
    bool encodeAudioFrame(const int16_t *samples, int nbSamples);
    void flushAudioAccumulator();
    bool drainEncoder(AVCodecContext *ctx, AVStream *stream);
    void fail(const QString &message);
};

bool VideoRecorder::Impl::open(const QString &path, int width, int height, int videoFps, int videoBitrate,
                               int audioSampleRate, int audioChannels, int audioBitrate)
{
    finalPath = path;
    tempPath = path + ".part";
    fps = videoFps > 0 ? videoFps : 30;
    sampleRate = audioSampleRate;
    channels = audioChannels;

    // H.264 requires even dimensions.
    encW = width & ~1;
    encH = height & ~1;
    if (encW < 16 || encH < 16) {
        error = QStringLiteral("invalid video size %1x%2").arg(width).arg(height);
        return false;
    }

    int ret = avformat_alloc_output_context2(&fmt, nullptr, "mp4", tempPath.toUtf8().constData());
    if (ret < 0 || !fmt) {
        error = QStringLiteral("cannot create mp4 muxer");
        return false;
    }
    fmt->flags |= AVFMT_FLAG_AUTO_BSF;

    if (!initVideo(videoBitrate)) {
        closeAll();
        return false;
    }

    // 音频是尽力而为：缺少 AAC 编码器时不得影响视频采集。
    audioEnabled = (sampleRate > 0 && channels > 0) && initAudio(audioBitrate);
    if (!audioEnabled) {
        qWarning() << "VideoRecorder: audio track disabled";
    }

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt->pb, tempPath.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            error = QStringLiteral("cannot open output file: %1").arg(ffErrorString(ret));
            closeAll();
            return false;
        }
    }

    ret = avformat_write_header(fmt, nullptr);
    if (ret < 0) {
        error = QStringLiteral("cannot write mp4 header: %1").arg(ffErrorString(ret));
        closeAll();
        return false;
    }
    headerWritten = true;

    pkt = av_packet_alloc();
    if (!pkt) {
        error = QStringLiteral("cannot allocate packet");
        closeAll();
        return false;
    }

    clock.start();
    running = true;
    stopping = false;
    worker = std::thread(&VideoRecorder::Impl::workerLoop, this);
    started = true;
    return true;
}

bool VideoRecorder::Impl::initVideo(int videoBitrate)
{
    const AVCodec *selected = nullptr;
    for (const char *name : kVideoEncoderCandidates) {
        selected = avcodec_find_encoder_by_name(name);
        if (selected) {
            break;
        }
    }
    if (!selected) {
        error = QStringLiteral("no H.264 encoder available in the bundled FFmpeg");
        return false;
    }

    vStream = avformat_new_stream(fmt, nullptr);
    if (!vStream) {
        error = QStringLiteral("cannot create video stream");
        return false;
    }

    vCtx = avcodec_alloc_context3(selected);
    if (!vCtx) {
        error = QStringLiteral("cannot allocate video encoder");
        return false;
    }

    vCtx->codec_id = selected->id;
    vCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    vCtx->width = encW;
    vCtx->height = encH;
    vCtx->time_base = AVRational{1, 1000};
    vCtx->framerate = AVRational{fps, 1};
    vCtx->gop_size = fps * 2;
    vCtx->max_b_frames = 0;
    vCtx->bit_rate = videoBitrate;
    vCtx->rc_max_rate = videoBitrate;
    vCtx->rc_buffer_size = videoBitrate * 2;
    vCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (selected->pix_fmts) {
        vCtx->pix_fmt = selected->pix_fmts[0];
        for (int i = 0; selected->pix_fmts[i] != AV_PIX_FMT_NONE; ++i) {
            if (selected->pix_fmts[i] == AV_PIX_FMT_YUV420P) {
                vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
                break;
            }
        }
    } else {
        vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    if (std::strcmp(selected->name, "libx264") == 0) {
        av_opt_set(vCtx->priv_data, "preset", "veryfast", 0);
        av_opt_set(vCtx->priv_data, "profile", "main", 0);
    }

    int ret = avcodec_open2(vCtx, selected, nullptr);
    if (ret < 0) {
        error = QStringLiteral("cannot open video encoder %1: %2")
                    .arg(QString::fromUtf8(selected->name), ffErrorString(ret));
        avcodec_free_context(&vCtx);
        return false;
    }
    qInfo() << "VideoRecorder: video encoder" << selected->name << encW << "x" << encH;

    ret = avcodec_parameters_from_context(vStream->codecpar, vCtx);
    if (ret < 0) {
        error = QStringLiteral("cannot copy video parameters");
        return false;
    }
    vStream->time_base = vCtx->time_base;

    vFrame = av_frame_alloc();
    if (!vFrame) {
        error = QStringLiteral("cannot allocate video frame");
        return false;
    }
    vFrame->format = vCtx->pix_fmt;
    vFrame->width = encW;
    vFrame->height = encH;
    ret = av_frame_get_buffer(vFrame, 32);
    if (ret < 0) {
        error = QStringLiteral("cannot allocate video frame buffer");
        return false;
    }
    return true;
}

bool VideoRecorder::Impl::initAudio(int audioBitrate)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        return false;
    }

    aCtx = avcodec_alloc_context3(codec);
    if (!aCtx) {
        return false;
    }
    aCtx->codec_id = AV_CODEC_ID_AAC;
    aCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    aCtx->sample_rate = sampleRate;
    aCtx->bit_rate = audioBitrate;
    aCtx->time_base = AVRational{1, sampleRate};
    aCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    aCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#if FFMPEG_HAS_CH_LAYOUT
    av_channel_layout_default(&aCtx->ch_layout, channels);
#else
    aCtx->channels = channels;
    aCtx->channel_layout = av_get_default_channel_layout(channels);
#endif
    if (codec->sample_fmts) {
        aCtx->sample_fmt = codec->sample_fmts[0];
    } else {
        aCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

    int ret = avcodec_open2(aCtx, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&aCtx);
        return false;
    }

#if FFMPEG_HAS_CH_LAYOUT
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, channels);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, aCtx->ch_layout.nb_channels);
    ret = swr_alloc_set_opts2(&swr, &outLayout, aCtx->sample_fmt, aCtx->sample_rate, &inLayout, AV_SAMPLE_FMT_S16,
                              sampleRate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
#else
    swr = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(aCtx->channels), aCtx->sample_fmt,
                             aCtx->sample_rate, av_get_default_channel_layout(channels), AV_SAMPLE_FMT_S16, sampleRate,
                             0, nullptr);
    ret = swr ? 0 : AVERROR(ENOMEM);
#endif
    if (ret < 0 || !swr || swr_init(swr) < 0) {
        qWarning() << "VideoRecorder: cannot init resampler";
        if (swr) {
            swr_free(&swr);
        }
        avcodec_free_context(&aCtx);
        aStream = nullptr;
        return false;
    }

    aFrame = av_frame_alloc();
    if (!aFrame) {
        swr_free(&swr);
        avcodec_free_context(&aCtx);
        return false;
    }

    // 流必须在编码器完全初始化后才能添加，否则 avformat_write_header()
    // 会因空流而失败。
    aStream = avformat_new_stream(fmt, nullptr);
    if (!aStream) {
        av_frame_free(&aFrame);
        swr_free(&swr);
        avcodec_free_context(&aCtx);
        return false;
    }
    ret = avcodec_parameters_from_context(aStream->codecpar, aCtx);
    if (ret < 0) {
        av_frame_free(&aFrame);
        swr_free(&swr);
        avcodec_free_context(&aCtx);
        aStream = nullptr;
        return false;
    }
    aStream->time_base = aCtx->time_base;
    audioFrameSize = aCtx->frame_size > 0 ? aCtx->frame_size : 1024;
    return true;
}

void VideoRecorder::Impl::fail(const QString &message)
{
    if (!failed) {
        failed = true;
        error = message;
        qWarning() << "VideoRecorder:" << message;
    }
    running = false;
}

void VideoRecorder::Impl::workerLoop()
{
    while (true) {
        VideoFrame vf;
        bool hasV = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            // 音频由墙上时钟驱动，而非由数据包到达驱动，因此工作线程会周期性
            // 唤醒，在 sndcpy 出现空隙时填充静音。
            queueCv.wait_for(lock, std::chrono::milliseconds(kAudioPumpPeriodMs),
                             [this]() { return stopping.load() || !vQueue.empty(); });
            if (!vQueue.empty()) {
                vf = std::move(vQueue.front());
                vQueue.pop_front();
                hasV = true;
            }
        }

        if (hasV && running.load()) {
            if (!encodeVideo(vf)) {
                break;
            }
        }
        if (running.load() && audioEnabled && !pumpAudio()) {
            break;
        }
        if (stopping.load() && !hasV) {
            break;
        }
        if (!running.load()) {
            break;
        }
    }

    // An encoding error stops the loop while the UI still believes recording
    // is active: finish the file and report the failure from the owner thread.
    if (!stopping.load() && failed && owner) {
        VideoRecorder *recorder = owner;
        QMetaObject::invokeMethod(
            recorder, [recorder]() { recorder->stop(); }, Qt::QueuedConnection);
    }
}

bool VideoRecorder::Impl::encodeVideo(VideoFrame &frame)
{
    int ret = av_frame_make_writable(vFrame);
    if (ret < 0) {
        fail(QStringLiteral("video frame is not writable"));
        return false;
    }

    if (frame.width == encW && frame.height == encH) {
        const int heights[3] = {encH, (encH + 1) / 2, (encH + 1) / 2};
        const int srcStride[3] = {frame.strideY, frame.strideU, frame.strideV};
        const uint8_t *srcData[3] = {frame.y.data(), frame.u.data(), frame.v.data()};
        for (int i = 0; i < 3; ++i) {
            const int width = (i == 0) ? encW : (encW + 1) / 2;
            for (int y = 0; y < heights[i]; ++y) {
                std::memcpy(vFrame->data[i] + static_cast<size_t>(y) * vFrame->linesize[i],
                            srcData[i] + static_cast<size_t>(y) * srcStride[i], static_cast<size_t>(width));
            }
        }
    } else {
        fillBlack(vFrame, encH);
        const double scale = std::min(static_cast<double>(encW) / frame.width, static_cast<double>(encH) / frame.height);
        const int dstW = std::max(2, static_cast<int>(frame.width * scale) & ~1);
        const int dstH = std::max(2, static_cast<int>(frame.height * scale) & ~1);
        if (!sws || swsSrcW != frame.width || swsSrcH != frame.height) {
            sws_freeContext(sws);
            sws = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P, dstW, dstH, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            swsSrcW = frame.width;
            swsSrcH = frame.height;
            if (!sws) {
                fail(QStringLiteral("cannot create scaler for %1x%2").arg(frame.width).arg(frame.height));
                return false;
            }
        }

        const int x0 = ((encW - dstW) / 2) & ~1;
        const int y0 = ((encH - dstH) / 2) & ~1;
        uint8_t *dstData[4] = {nullptr, nullptr, nullptr, nullptr};
        int dstStride[4] = {0, 0, 0, 0};
        dstData[0] = vFrame->data[0] + static_cast<size_t>(y0) * vFrame->linesize[0] + x0;
        dstData[1] = vFrame->data[1] + static_cast<size_t>(y0 / 2) * vFrame->linesize[1] + x0 / 2;
        dstData[2] = vFrame->data[2] + static_cast<size_t>(y0 / 2) * vFrame->linesize[2] + x0 / 2;
        dstStride[0] = vFrame->linesize[0];
        dstStride[1] = vFrame->linesize[1];
        dstStride[2] = vFrame->linesize[2];

        const uint8_t *srcData[4] = {frame.y.data(), frame.u.data(), frame.v.data(), nullptr};
        int srcStride[4] = {frame.strideY, frame.strideU, frame.strideV, 0};
        sws_scale(sws, srcData, srcStride, 0, frame.height, dstData, dstStride);
    }

    qint64 pts = frame.ptsMs;
    if (pts <= lastVPts) {
        pts = lastVPts + 1;
    }
    lastVPts = pts;
    vFrame->pts = pts;

    ret = avcodec_send_frame(vCtx, vFrame);
    if (ret < 0) {
        fail(QStringLiteral("cannot send video frame: %1").arg(ffErrorString(ret)));
        return false;
    }
    ++videoFrameCount;
    return drainEncoder(vCtx, vStream);
}

bool VideoRecorder::Impl::encodeAudioFrame(const int16_t *samples, int nbSamples)
{
    av_frame_unref(aFrame);
    aFrame->format = aCtx->sample_fmt;
    aFrame->sample_rate = aCtx->sample_rate;
    aFrame->nb_samples = nbSamples;
#if FFMPEG_HAS_CH_LAYOUT
    av_channel_layout_copy(&aFrame->ch_layout, &aCtx->ch_layout);
#else
    aFrame->channels = aCtx->channels;
    aFrame->channel_layout = aCtx->channel_layout;
#endif

    int ret = av_frame_get_buffer(aFrame, 0);
    if (ret < 0) {
        fail(QStringLiteral("cannot allocate audio frame"));
        return false;
    }

    // 始终喂入完整的 AAC 帧：过短的帧会让编码器在每收到一个 TCP 包后都填充静音，
    // 导致音频被拉长并割裂。
    const uint8_t *inData[1] = {reinterpret_cast<const uint8_t *>(samples)};
    ret = swr_convert(swr, aFrame->data, nbSamples, inData, nbSamples);
    if (ret < 0) {
        fail(QStringLiteral("audio resample failed"));
        return false;
    }

    aFrame->pts = audioSamples.load();
    audioSamples += nbSamples;

    ret = avcodec_send_frame(aCtx, aFrame);
    if (ret < 0) {
        fail(QStringLiteral("cannot send audio frame: %1").arg(ffErrorString(ret)));
        return false;
    }
    return drainEncoder(aCtx, aStream);
}

void VideoRecorder::Impl::flushAudioAccumulator()
{
    if (!audioEnabled || !aCtx || audioAccum.empty()) {
        return;
    }
    const size_t frameSamples = static_cast<size_t>(audioFrameSize) * channels;
    if (audioAccum.size() < frameSamples) {
        audioAccum.insert(audioAccum.end(), frameSamples - audioAccum.size(), 0);
    }
    encodeAudioFrame(audioAccum.data(), audioFrameSize);
    audioAccum.clear();
}

bool VideoRecorder::Impl::pumpAudio()
{
    if (!audioEnabled || !aCtx) {
        return true;
    }

    // 音频 PTS 与视频 PTS 使用同一个墙上时钟。任何缺失的 sndcpy 数据都用静音填充，
    // 而不是缩短时间轴。
    const qint64 elapsedMs = clock.elapsed();
    const qint64 expected = elapsedMs * sampleRate / 1000;
    qint64 due = expected - audioWrittenSamples;
    if (due <= 0) {
        return true;
    }
    const qint64 maxCatchUp = static_cast<qint64>(sampleRate) * kAudioCatchUpMaxMs / 1000;
    if (due > maxCatchUp) {
        due = maxCatchUp;
    }
    const int needed = static_cast<int>(due);

    const int bytesPerSample = channels * 2;
    std::vector<uint8_t> bytes;
    int silence = 0;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // sndcpy 在启动时会一次性倾泻其缓冲区。丢弃这笔积压，
        // 避免音频从第一帧起就落后视频 100-200ms。
        size_t discard = 0;
        // 即便在预热阶段也保留一个较小的（40ms）抖动缓冲：若每个
        // 字节都丢弃，下一次突发到来前会出现可察觉的静音空隙。
        const size_t targetPending = static_cast<size_t>(sampleRate) * bytesPerSample * 40 / 1000;
        const size_t maxPending = static_cast<size_t>(sampleRate) * bytesPerSample * kAudioPendingMaxMs / 1000;
        const size_t limit = (elapsedMs < kAudioWarmupMs) ? targetPending : maxPending;
        if (audioPending.size() > limit) {
            discard = audioPending.size() - limit;
            discard -= discard % bytesPerSample; // 保持 PCM 采样边界对齐
        }
        if (discard > 0) {
            audioPending.erase(audioPending.begin(), audioPending.begin() + discard);
        }

        const int available = static_cast<int>(audioPending.size() / bytesPerSample);
        const int take = std::min(needed, available);
        const int takeBytes = take * bytesPerSample;
        if (takeBytes > 0) {
            bytes.assign(audioPending.begin(), audioPending.begin() + takeBytes);
            audioPending.erase(audioPending.begin(), audioPending.begin() + takeBytes);
        }
        silence = needed - take;
    }

    if (!bytes.empty()) {
        const int16_t *pcm = reinterpret_cast<const int16_t *>(bytes.data());
        audioAccum.insert(audioAccum.end(), pcm, pcm + bytes.size() / sizeof(int16_t));
    }
    if (silence > 0) {
        audioAccum.insert(audioAccum.end(), static_cast<size_t>(silence) * channels, 0);
    }
    audioWrittenSamples += needed;

    const size_t frameSamples = static_cast<size_t>(audioFrameSize) * channels;
    while (audioAccum.size() >= frameSamples) {
        if (!encodeAudioFrame(audioAccum.data(), audioFrameSize)) {
            return false;
        }
        audioAccum.erase(audioAccum.begin(), audioAccum.begin() + frameSamples);
    }
    return true;
}

bool VideoRecorder::Impl::drainEncoder(AVCodecContext *ctx, AVStream *stream)
{
    while (true) {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            fail(QStringLiteral("encode failed: %1").arg(ffErrorString(ret)));
            return false;
        }
        av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        ret = av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            fail(QStringLiteral("cannot write packet: %1").arg(ffErrorString(ret)));
            return false;
        }
    }
}

bool VideoRecorder::Impl::flushEncoders()
{
    bool ok = true;
    if (vCtx) {
        avcodec_send_frame(vCtx, nullptr);
        ok = drainEncoder(vCtx, vStream) && ok;
    }
    if (aCtx) {
        avcodec_send_frame(aCtx, nullptr);
        ok = drainEncoder(aCtx, aStream) && ok;
    }
    return ok;
}

void VideoRecorder::Impl::closeAll()
{
    if (sws) {
        sws_freeContext(sws);
        sws = nullptr;
    }
    if (swr) {
        swr_free(&swr);
    }
    if (aFrame) {
        av_frame_free(&aFrame);
    }
    if (vFrame) {
        av_frame_free(&vFrame);
    }
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (aCtx) {
        avcodec_free_context(&aCtx);
    }
    if (vCtx) {
        avcodec_free_context(&vCtx);
    }
    if (fmt) {
        if (fmt->pb && !(fmt->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt->pb);
        }
        avformat_free_context(fmt);
        fmt = nullptr;
    }
    aStream = nullptr;
    vStream = nullptr;
}

VideoRecorder::VideoRecorder(QObject *parent) : QObject(parent), m_impl(new Impl)
{
}

VideoRecorder::~VideoRecorder()
{
    stop();
    delete m_impl;
    m_impl = nullptr;
}

bool VideoRecorder::start(const QString &filePath, int width, int height, int fps, int videoBitrate, int sampleRate,
                          int channels, int audioBitrate)
{
    if (!m_impl || m_impl->running.load()) {
        return false;
    }
    m_impl->failed = false;
    m_impl->error.clear();
    m_impl->videoFrameCount = 0;
    m_impl->audioSamples = 0;
    m_impl->lastVPts = -1;
    m_impl->audioPending.clear();
    m_impl->audioAccum.clear();
    m_impl->audioWrittenSamples = 0;
    m_impl->owner = this;

    if (!m_impl->open(filePath, width, height, fps, videoBitrate, sampleRate, channels, audioBitrate)) {
        const QString err = m_impl->error;
        m_impl->closeAll();
        QFile::remove(m_impl->tempPath);
        emit errorOccurred(err);
        return false;
    }
    return true;
}

void VideoRecorder::pushVideoFrame(const uint8_t *dataY, const uint8_t *dataU, const uint8_t *dataV, int width,
                                   int height, int linesizeY, int linesizeU, int linesizeV)
{
    if (!m_impl || !m_impl->running.load() || !dataY || !dataU || !dataV) {
        return;
    }
    if (width <= 0 || height <= 0 || linesizeY <= 0 || linesizeU <= 0 || linesizeV <= 0) {
        return;
    }

    Impl::VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.strideY = linesizeY;
    frame.strideU = linesizeU;
    frame.strideV = linesizeV;
    frame.ptsMs = m_impl->clock.elapsed();

    const int heights[3] = {height, (height + 1) / 2, (height + 1) / 2};
    const int strides[3] = {linesizeY, linesizeU, linesizeV};
    frame.y.resize(static_cast<size_t>(linesizeY) * heights[0]);
    frame.u.resize(static_cast<size_t>(linesizeU) * heights[1]);
    frame.v.resize(static_cast<size_t>(linesizeV) * heights[2]);
    for (int y = 0; y < heights[0]; ++y) {
        std::memcpy(frame.y.data() + static_cast<size_t>(y) * linesizeY, dataY + static_cast<size_t>(y) * linesizeY,
                    linesizeY);
    }
    for (int y = 0; y < heights[1]; ++y) {
        std::memcpy(frame.u.data() + static_cast<size_t>(y) * linesizeU, dataU + static_cast<size_t>(y) * linesizeU,
                    linesizeU);
        std::memcpy(frame.v.data() + static_cast<size_t>(y) * linesizeV, dataV + static_cast<size_t>(y) * linesizeV,
                    linesizeV);
    }

    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    if (m_impl->vQueue.size() >= static_cast<size_t>(kMaxVideoQueue)) {
        m_impl->vQueue.pop_front(); // 丢弃最旧的帧，绝不阻塞解码器
    }
    m_impl->vQueue.emplace_back(std::move(frame));
    m_impl->queueCv.notify_one();
}

void VideoRecorder::pushAudioData(const char *data, int size)
{
    if (!m_impl || !m_impl->running.load() || !m_impl->audioEnabled || !data || size <= 0) {
        return;
    }

    const int bytesPerSample = m_impl->channels * 2;
    if (bytesPerSample <= 0) {
        return;
    }

    const uint8_t *begin = reinterpret_cast<const uint8_t *>(data);
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    // 保持字节级流。TCP 读取边界是任意的：残留的半个样本必须保留到下一个包到达。
    m_impl->audioPending.insert(m_impl->audioPending.end(), begin, begin + size);

    const size_t maxPending =
        static_cast<size_t>(m_impl->sampleRate) * bytesPerSample * kAudioPendingMaxMs / 1000;
    if (m_impl->audioPending.size() > maxPending) {
        size_t discard = m_impl->audioPending.size() - maxPending;
        discard -= discard % bytesPerSample;
        m_impl->audioPending.erase(m_impl->audioPending.begin(), m_impl->audioPending.begin() + discard);
    }
    m_impl->queueCv.notify_one();
}

void VideoRecorder::stop()
{
    if (!m_impl) {
        return;
    }
    if (!m_impl->started) {
        return;
    }
    m_impl->started = false;
    m_impl->running = false;

    m_impl->stopping = true;
    m_impl->queueCv.notify_all();
    if (m_impl->worker.joinable()) {
        m_impl->worker.join();
    }

    const qint64 durationMs = m_impl->clock.elapsed();
    bool ok = !m_impl->failed;
    if (m_impl->headerWritten) {
        // 将音频时间轴与最后一帧视频对齐，并在关闭文件前输出最后一帧
        //（已用静音填充）AAC 帧。
        ok = m_impl->pumpAudio() && ok;
        m_impl->flushAudioAccumulator();
        ok = m_impl->flushEncoders() && ok;
        const int ret = av_write_trailer(m_impl->fmt);
        if (ret < 0) {
            ok = false;
            qWarning() << "VideoRecorder: cannot write trailer" << ffErrorString(ret);
        }
    }

    const qint64 frames = m_impl->videoFrameCount.load();
    const bool hadAudio = m_impl->audioEnabled && m_impl->audioSamples.load() > 0;
    m_impl->closeAll();
    m_impl->headerWritten = false;

    QString finalPath = m_impl->finalPath;
    const QString tempPath = m_impl->tempPath;

    if (frames == 0) {
        QFile::remove(tempPath);
        emit errorOccurred(QStringLiteral("no video frame was captured"));
        return;
    }
    if (!ok) {
        // 将未完成的文件保留在目标文件旁边，方便用户尝试恢复，并上报失败。
        QFile::remove(finalPath);
        const QString partPath = finalPath + ".part";
        if (tempPath != partPath) {
            QFile::remove(partPath);
            QFile::rename(tempPath, partPath);
        }
        const QString err = m_impl->error.isEmpty() ? QStringLiteral("recording aborted") : m_impl->error;
        emit errorOccurred(err);
        return;
    }

    QFile::remove(finalPath);
    if (!QFile::rename(tempPath, finalPath)) {
        if (!QFile::copy(tempPath, finalPath)) {
            emit errorOccurred(QStringLiteral("cannot move recording to %1").arg(finalPath));
            return;
        }
        QFile::remove(tempPath);
    }

    const qint64 bytes = QFileInfo(finalPath).size();
    emit finished(finalPath, durationMs, bytes, hadAudio);
}

bool VideoRecorder::isRecording() const
{
    return m_impl && m_impl->running.load();
}

QString VideoRecorder::filePath() const
{
    return m_impl ? m_impl->finalPath : QString();
}

bool VideoRecorder::hasAudio() const
{
    return m_impl && m_impl->audioEnabled;
}
