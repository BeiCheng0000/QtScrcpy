#ifndef VIDEORECORDER_H
#define VIDEORECORDER_H

#include <QObject>
#include <QString>
#include <QImage>
#include <functional>

#include <atomic>

/*
 * Realtime screen recorder.
 *
 * Video frames are YUV420P planes as delivered by the scrcpy decoder, audio is
 * interleaved 16-bit PCM as delivered by the sndcpy channel. Both are pushed
 * from their own producer threads and consumed by a single encoding thread,
 * which muxes H.264 + AAC into an mp4 file.
 *
 * The encoder size is fixed by start(); later frames with a different size
 * (device rotation) are letterboxed into the encoder frame so the resulting
 * file keeps a constant resolution.
 */
class VideoRecorder : public QObject
{
    Q_OBJECT

public:
    explicit VideoRecorder(QObject *parent = nullptr);
    ~VideoRecorder() override;

    bool start(const QString &filePath, int width, int height, int fps = 30, int videoBitrate = 8000000,
               int sampleRate = 48000, int channels = 2, int audioBitrate = 128000);
    void pushVideoFrame(const uint8_t *dataY, const uint8_t *dataU, const uint8_t *dataV, int width, int height,
                        int linesizeY, int linesizeU, int linesizeV);
    void pushAudioData(const char *data, int size);
    void stop();

    bool isRecording() const;
    QString filePath() const;
    // 输出文件中包含音轨时为 true
    bool hasAudio() const;

    // 录制时的画面叠加钩子（鼠标轨迹等）：编码线程在每帧送编码器前调用。
    // img：原始帧缩放后的画面图像（不含 letterbox 黑边）；
    // ptsMs：该帧录制时间戳（毫秒，与本地时钟同基准）；
    // scale：img 像素 / 原始帧像素。
    // 必须在 start() 成功之后、stop() 之前设置；传空回调可随时卸载。
    using OverlayPainter = std::function<void(QImage &img, qint64 ptsMs, double scale)>;
    void setOverlayPainter(OverlayPainter painter);

signals:
    void errorOccurred(const QString &error);
    void finished(const QString &filePath, qint64 durationMs, qint64 bytes, bool hasAudio);

private:
    class Impl;
    Impl *m_impl = nullptr;
};

#endif // VIDEORECORDER_H
