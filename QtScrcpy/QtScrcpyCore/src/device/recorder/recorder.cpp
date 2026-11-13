#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>

#include "compat.h"
#include "recorder.h"

static const AVRational SCRCPY_TIME_BASE = { 1, 1000000 }; // 时间戳单位：微秒

Recorder::Recorder(const QString &fileName, QObject *parent) : QThread(parent), m_fileName(fileName), m_format(guessRecordFormat(fileName)) {}

Recorder::~Recorder() {}

AVPacket *Recorder::packetNew(const AVPacket *packet)
{
    AVPacket *rec = av_packet_alloc();
    if (!rec) {
        return Q_NULLPTR;
    }

    if (av_packet_ref(rec, packet)) {
        delete rec;
        return Q_NULLPTR;
    }
    return rec;
}

void Recorder::packetDelete(AVPacket *packet)
{
    av_packet_unref(packet);
    av_packet_free(&packet);
}

void Recorder::queueClear()
{
    while (!m_queue.isEmpty()) {
        packetDelete(m_queue.dequeue());
    }
}

void Recorder::setFrameSize(const QSize &declaredFrameSize)
{
    m_declaredFrameSize = declaredFrameSize;
}

void Recorder::setFormat(Recorder::RecorderFormat format)
{
    m_format = format;
}

bool Recorder::open()
{
    // 解码器
    const AVCodec* inputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!inputCodec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    QString formatName = recorderGetFormatName(m_format);
    Q_ASSERT(!formatName.isEmpty());
    const AVOutputFormat *format = findMuxer(formatName.toUtf8());
    if (!format) {
        qCritical("Could not find muxer");
        return false;
    }

    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx) {
        qCritical("Could not allocate output context");
        return false;
    }

    // 与已废弃的 API（av_oformat_next()）不同，av_muxer_iterate()
    // 特意返回指向常量的指针，但 AVFormatContext.oformat
    // 仍期望非常量指针（它尚未随之更新）
    // <https://github.com/FFmpeg/FFmpeg/commit/0694d8702421e7aff1340038559c438b61bb30dd>

    m_formatCtx->oformat = (AVOutputFormat *)format;

    QString comment = "Recorded by QtScrcpy " + QCoreApplication::applicationVersion();
    av_dict_set(&m_formatCtx->metadata, "comment", comment.toUtf8(), 0);

    AVStream *outStream = avformat_new_stream(m_formatCtx, inputCodec);
    if (!outStream) {
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
        return false;
    }

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    outStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codecpar->codec_id = inputCodec->id;
    outStream->codecpar->format = AV_PIX_FMT_YUV420P;
    outStream->codecpar->width = m_declaredFrameSize.width();
    outStream->codecpar->height = m_declaredFrameSize.height();
#else
    outStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codec->codec_id = inputCodec->id;
    outStream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    outStream->codec->width = m_declaredFrameSize.width();
    outStream->codec->height = m_declaredFrameSize.height();
#endif

    int ret = avio_open(&m_formatCtx->pb, m_fileName.toUtf8().toStdString().c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical() << QString("Failed to open output file: %1 %2").arg(errorbuf).arg(m_fileName).toUtf8().toStdString().c_str();
        // ostream 会在上下文清理时一并释放
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
        return false;
    }

    return true;
}

void Recorder::close()
{
    if (Q_NULLPTR != m_formatCtx) {
        if (m_headerWritten) {
            int ret = av_write_trailer(m_formatCtx);
            if (ret < 0) {
                qCritical() << QString("Failed to write trailer to %1").arg(m_fileName).toUtf8().toStdString().c_str();
                m_failed = true;
            } else {
                qInfo() << QString("success record %1").arg(m_fileName).toStdString().c_str();
            }
        } else {
            // 录制的文件为空
            m_failed = true;
        }
        avio_close(m_formatCtx->pb);
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
    }
}

bool Recorder::write(AVPacket *packet)
{
    if (!m_headerWritten) {
        if (packet->pts != AV_NOPTS_VALUE) {
            qCritical("The first packet is not a config packet");
            return false;
        }
        bool ok = recorderWriteHeader(packet);
        if (!ok) {
            return false;
        }
        m_headerWritten = true;
        return true;
    }

    if (packet->pts == AV_NOPTS_VALUE) {
        // 忽略配置包
        return true;
    }

    recorderRescalePacket(packet);
    return av_write_frame(m_formatCtx, packet) >= 0;
}

const AVOutputFormat *Recorder::findMuxer(const char *name)
{
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
    void *opaque = Q_NULLPTR;
#endif
    const AVOutputFormat *outFormat = Q_NULLPTR;
    do {
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
        outFormat = av_muxer_iterate(&opaque);
#else
        outFormat = av_oformat_next(outFormat);
#endif
        // 直到为空或名称匹配 "name"
    } while (outFormat && strcmp(outFormat->name, name));
    return outFormat;
}

bool Recorder::recorderWriteHeader(const AVPacket *packet)
{
    AVStream *ostream = m_formatCtx->streams[0];
    quint8 *extradata = (quint8 *)av_malloc(packet->size * sizeof(quint8));
    if (!extradata) {
        qCritical("Cannot allocate extradata");
        return false;
    }
    // 将首个数据包拷贝到 extradata
    memcpy(extradata, packet->data, packet->size);

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    ostream->codecpar->extradata = extradata;
    ostream->codecpar->extradata_size = packet->size;
#else
    ostream->codec->extradata = extradata;
    ostream->codec->extradata_size = packet->size;
#endif

    int ret = avformat_write_header(m_formatCtx, NULL);
    if (ret < 0) {
        qCritical("Failed to write header recorder file");
        return false;
    }
    return true;
}

void Recorder::recorderRescalePacket(AVPacket *packet)
{
    AVStream *ostream = m_formatCtx->streams[0];
    av_packet_rescale_ts(packet, SCRCPY_TIME_BASE, ostream->time_base);
}

QString Recorder::recorderGetFormatName(Recorder::RecorderFormat format)
{
    switch (format) {
    case RECORDER_FORMAT_MP4:
        return "mp4";
    case RECORDER_FORMAT_MKV:
        return "matroska";
    default:
        return "";
    }
}

Recorder::RecorderFormat Recorder::guessRecordFormat(const QString &fileName)
{
    if (4 > fileName.length()) {
        return Recorder::RECORDER_FORMAT_NULL;
    }
    QFileInfo fileInfo = QFileInfo(fileName);
    QString ext = fileInfo.suffix();
    if (0 == ext.compare("mp4")) {
        return Recorder::RECORDER_FORMAT_MP4;
    }
    if (0 == ext.compare("mkv")) {
        return Recorder::RECORDER_FORMAT_MKV;
    }

    return Recorder::RECORDER_FORMAT_NULL;
}

void Recorder::run()
{
    qint64 ptsOrigin = AV_NOPTS_VALUE;
    for (;;) {
        AVPacket *rec = Q_NULLPTR;
        {
            QMutexLocker locker(&m_mutex);
            while (!m_stopped && m_queue.isEmpty()) {
                m_recvDataCond.wait(&m_mutex);
            }

            // 如果已设置停止，仍继续处理剩余事件（以
            // 完成录制），之后才真正停止
            if (m_stopped && m_queue.isEmpty()) {
                AVPacket *last = m_previous;
                if (last) {
                    last->pts -= ptsOrigin;
                    last->dts = last->pts;
                    // 为最后一个数据包指定一个随意的时长
                    last->duration = 100000;
                    bool ok = write(last);
                    if (!ok) {
                        // 未能写入最后一帧并不严重，后续帧
                        // 都不会依赖它，因此生成的文件
                        // 依然有效
                        qWarning("Could not record last packet");
                    }
                    packetDelete(last);
                }
                break;
            }

            rec = m_queue.dequeue();
        }

        // recorder->previous 仅由本线程写入，无需加锁
        AVPacket *previous = m_previous;
        m_previous = rec;

        if (!previous) {
            // 刚收到第一个数据包
            continue;
        }

        // 配置包没有 PTS，必须忽略
        if (rec->pts != AV_NOPTS_VALUE && previous->pts != AV_NOPTS_VALUE) {
            // 现在可以计算出上一个数据包的时长
            previous->duration = rec->pts - previous->pts;
        }

        if (previous->pts != AV_NOPTS_VALUE) {
            if (ptsOrigin == AV_NOPTS_VALUE) {
                ptsOrigin = previous->pts;
            }
            previous->pts -= ptsOrigin;
            previous->dts = previous->pts;
        }        

        bool ok = write(previous);
        packetDelete(previous);
        if (!ok) {
            qCritical("Could not record packet");
            QMutexLocker locker(&m_mutex);
            m_failed = true;
            // 丢弃待处理的数据包
            queueClear();
            break;
        }
    }

    qDebug("Recorder thread ended");
}

bool Recorder::startRecorder()
{
    start();
    return true;
}

void Recorder::stopRecorder()
{
    QMutexLocker locker(&m_mutex);
    m_stopped = true;
    m_recvDataCond.wakeOne();
}

bool Recorder::push(const AVPacket *packet)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(!m_stopped);

    if (m_failed) {
        // 拒绝任何新数据包（这将停止流）
        return false;
    }

    AVPacket *rec = packetNew(packet);
    if (rec) {
        m_queue.enqueue(rec);
        m_recvDataCond.wakeOne();
    }
    return rec != Q_NULLPTR;
}
