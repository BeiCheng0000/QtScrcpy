#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QHash>
#include <QMutex>
#include <QThread>
#include <QProcess>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class QAudioSink;
class QAudioOutput;
class QIODevice;
class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    // Single process wide audio channel, shared by playback and recording.
    static AudioOutput &instance();

    bool start(const QString& serial, int port);
    void stop();
    void installonly(const QString& serial, int port);
    bool isRunning() const;
    QString serial() const;

    // Stop playback AND tear down the device side of the audio bridge when the
    // device window is closed: adb forward + the sndcpy capture activity.
    // Safe to call repeatedly and harmless for a different device's stream.
    void stopForDevice(const QString& serial);

    // Listeners receive the raw s16le/48k/stereo PCM pushed by sndcpy.
    // They are invoked from the socket thread and must not block.
    void addPcmListener(const QString &key, std::function<void(const char *, int)> listener);
    void removePcmListener(const QString &key);

private:
    bool runSndcpyProcess(const QString& serial, int port, bool wait = true);
    void startAudioOutput();
    void stopAudioOutput();
    void startRecvData(int port);
    void stopRecvData();
    void teardownDeviceBridge(const QString& serial);

signals:
    void connectTo(int port);

private:
    QPointer<QIODevice> m_outputDevice;
    QThread m_workerThread;
    QProcess m_sndcpy;
    QVector<char> m_buffer;
    bool m_running = false;
    QString m_serial;
    int m_port = 28200;
    QMutex m_listenerMutex;
    QHash<QString, std::function<void(const char *, int)>> m_pcmListeners;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput* m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
};

#endif // AUDIOOUTPUT_H
