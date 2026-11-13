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

    // 进程级单一音频通道，播放与录制共用。
    static AudioOutput &instance();

    bool start(const QString& serial, int port);
    void stop();
    void installonly(const QString& serial, int port);
    bool isRunning() const;
    QString serial() const;

    // 设备窗口关闭时，停止播放并拆除音频桥接的设备侧：
    // adb 转发 + sndcpy 采集 Activity。可重复调用，且对其它设备的音频流无害。
    void stopForDevice(const QString& serial);

    // 监听器收到 sndcpy 推送的原始 s16le/48k/立体声 PCM 数据。
    // 它们在 socket 线程中被调用，不得阻塞。
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
