#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QHash>
#include <QMutex>
#include <QProcess>
#include <QPointer>
#include <QThread>
#include <QVector>

#include <atomic>
#include <functional>

class QAudioSink;
class QAudioOutput;
class QIODevice;

// 单台设备的音频会话：sndcpy 进程 + adb forward 端口 + 接收线程 + 输出流。
// 每台设备一个实例，由 AudioOutput 统一管理；多路输出由系统混音。
class AudioSession : public QObject
{
    Q_OBJECT
public:
    explicit AudioSession(const QString &serial, int port, QObject *parent = nullptr);
    ~AudioSession();

    bool start();
    void stop();                 // 停止播放/接收，不清桥接（由 stopForDevice 拆）
    void stopBridge();           // 拆除 adb forward + 设备端 sndcpy Activity

    void addListener(const QString &key, std::function<void(const char *, int)> listener);
    void removeListener(const QString &key);
    void clearListeners();

    const QString serial;
    const int port;
    std::atomic<bool> running{false}; // 接收线程读取，主线程写入

signals:
    void connectTo(int port);

private:
    bool runSndcpyProcess(bool wait);
    void startAudioOutput();
    void stopAudioOutput();
    void startRecvData();
    void stopRecvData();

    QPointer<QIODevice> m_outputDevice;
    QThread m_workerThread;
    QProcess m_sndcpy;
    QVector<char> m_buffer;
    QMutex m_listenerMutex;
    QHash<QString, std::function<void(const char *, int)>> m_listeners;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput *m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
};

class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    // 多设备音频管理器：每台设备一个独立会话（进程/端口/输出流）。
    // 每会话的 PCM 只分发给本设备的监听器，避免跨设备录制串音。
    static AudioOutput &instance();

    // port = 0 或端口已被占用时自动从 28200 起分配空闲端口
    bool start(const QString &serial, int port = 0);
    void stop();                       // 停止全部会话
    void stopForDevice(const QString &serial); // 停止并拆除指定设备桥接
    void installonly(const QString &serial, int port = 0);

    bool isRunning() const;                    // 是否有任意会话在播放
    bool isRunning(const QString &serial) const; // 指定设备是否已有音频会话
    QString serial() const;                    // 最近一次启动音频的设备
    int portOf(const QString &serial) const;   // 指定设备实际使用的端口（未连接返回 0）

    // 监听器收到本设备 sndcpy 推送的原始 s16le/48k/立体声 PCM。
    // 它们在 socket 线程中被调用，不得阻塞。
    void addPcmListener(const QString &serial, const QString &key,
                        std::function<void(const char *, int)> listener);
    void removePcmListener(const QString &serial, const QString &key);

private:
    int availablePort(int preferred) const;
    void teardownDeviceBridge(const QString &serial, int port);

    QHash<QString, AudioSession *> m_sessions;
    QString m_lastSerial;
};

#endif // AUDIOOUTPUT_H
