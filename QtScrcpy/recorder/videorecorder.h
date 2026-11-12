#ifndef VIDEORECORDER_H
#define VIDEORECORDER_H

#include <QObject>
#include <QString>

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
    // true when an audio track is present in the output file
    bool hasAudio() const;

signals:
    void errorOccurred(const QString &error);
    void finished(const QString &filePath, qint64 durationMs, qint64 bytes, bool hasAudio);

private:
    class Impl;
    Impl *m_impl = nullptr;
};

#endif // VIDEORECORDER_H
