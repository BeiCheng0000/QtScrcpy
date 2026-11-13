#ifndef RECORDER_H
#define RECORDER_H
#include <QMutex>
#include <QQueue>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWaitCondition>

extern "C"
{
#include "libavformat/avformat.h"
}

class Recorder : public QThread
{
    Q_OBJECT
public:
    enum RecorderFormat
    {
        RECORDER_FORMAT_NULL = 0,
        RECORDER_FORMAT_MP4,
        RECORDER_FORMAT_MKV,
    };

    Recorder(const QString &fileName, QObject *parent = Q_NULLPTR);
    virtual ~Recorder();

    void setFrameSize(const QSize &declaredFrameSize);
    void setFormat(Recorder::RecorderFormat format);
    bool open();
    void close();
    bool write(AVPacket *packet);
    bool startRecorder();
    void stopRecorder();
    bool push(const AVPacket *packet);

private:
    const AVOutputFormat *findMuxer(const char *name);
    bool recorderWriteHeader(const AVPacket *packet);
    void recorderRescalePacket(AVPacket *packet);
    QString recorderGetFormatName(Recorder::RecorderFormat format);
    RecorderFormat guessRecordFormat(const QString &fileName);

private:
    AVPacket *packetNew(const AVPacket *packet);
    void packetDelete(AVPacket *packet);
    void queueClear();

protected:
    void run();

private:
    QString m_fileName = "";
    AVFormatContext *m_formatCtx = Q_NULLPTR;
    QSize m_declaredFrameSize;
    bool m_headerWritten = false;
    RecorderFormat m_format = RECORDER_FORMAT_NULL;
    QMutex m_mutex;
    QWaitCondition m_recvDataCond;
    bool m_stopped = false; // 由流读取方在 recorder_stop() 时设置
    bool m_failed = false;  // 在数据包写入失败时设置
    QQueue<AVPacket *> m_queue;
    // 只有收到下一个数据包后才能写入当前数据包，这样才能
    // 计算它的时长（next_pts - current_pts）
    // "previous" 只会被录制线程访问，因此不需要
    // 用互斥锁保护
    AVPacket *m_previous = Q_NULLPTR;
};

#endif // RECORDER_H
