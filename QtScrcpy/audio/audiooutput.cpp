#include <QAudioOutput>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTime>
#include <QTimer>

#include <utility>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#endif

#include "audiooutput.h"

namespace {
constexpr int kBaseAudioPort = 28200;
}

AudioSession::AudioSession(const QString &serial, int port, QObject *parent)
    : QObject(parent), serial(serial), port(port)
{
}

AudioSession::~AudioSession()
{
    stop();
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
        m_sndcpy.waitForFinished(500);
    }
}

void AudioSession::addListener(const QString &key, std::function<void(const char *, int)> listener)
{
    QMutexLocker locker(&m_listenerMutex);
    m_listeners.insert(key, std::move(listener));
}

void AudioSession::removeListener(const QString &key)
{
    QMutexLocker locker(&m_listenerMutex);
    m_listeners.remove(key);
}

void AudioSession::clearListeners()
{
    QMutexLocker locker(&m_listenerMutex);
    m_listeners.clear();
}

bool AudioSession::start()
{
    QElapsedTimer timeConsumeCount;
    timeConsumeCount.start();
    if (!runSndcpyProcess(true)) {
        qWarning() << "AudioOutput::" << serial << "start sndcpy failed";
        return false;
    }
    qInfo() << "AudioOutput::" << serial << "run sndcpy cost:" << timeConsumeCount.elapsed() << "ms on port" << port;

    startAudioOutput();
    startRecvData();
    running = true;
    return true;
}

void AudioSession::stop()
{
    running = false;
    stopRecvData();
    stopAudioOutput();
    clearListeners();
}

void AudioSession::stopBridge()
{
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
        m_sndcpy.waitForFinished(500);
    }
    QString adb = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN32
    adb += QStringLiteral("/adb.exe");
#else
    adb += QStringLiteral("/adb");
#endif
    if (!QFileInfo::exists(adb)) {
        qWarning() << "AudioOutput::cannot find adb to close audio bridge:" << adb;
        return;
    }

    const QStringList baseArgs{QStringLiteral("-s"), serial};
    const QStringList commands[] = {
        QStringList{QStringLiteral("forward"), QStringLiteral("--remove"),
                    QStringLiteral("tcp:%1").arg(port)},
        QStringList{QStringLiteral("shell"), QStringLiteral("am"), QStringLiteral("force-stop"),
                    QStringLiteral("com.rom1v.sndcpy")},
    };

    for (const QStringList &command : commands) {
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(adb, baseArgs + command);
        if (!process.waitForStarted(500)) {
            continue;
        }
        if (!process.waitForFinished(800)) {
            process.kill();
            process.waitForFinished(200);
        }
    }
}

bool AudioSession::runSndcpyProcess(bool wait)
{
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
        m_sndcpy.waitForFinished(500);
    }
    connect(&m_sndcpy, &QProcess::readyReadStandardOutput, this, [this]() {
        qInfo() << "AudioOutput::" << serial << QString(m_sndcpy.readAllStandardOutput());
    });
    connect(&m_sndcpy, &QProcess::readyReadStandardError, this, [this]() {
        qInfo() << "AudioOutput::" << serial << QString(m_sndcpy.readAllStandardError());
    });

#ifdef Q_OS_WIN32
    const QStringList params{serial, QString::number(port)};
    // sndcpy.bat 在其工作目录中查找 adb.exe 与 sndcpy.apk
    m_sndcpy.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_sndcpy.start(QStringLiteral("sndcpy.bat"), params);
#else
    const QStringList params{QStringLiteral("sndcpy.sh"), serial, QString::number(port)};
    m_sndcpy.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_sndcpy.start(QStringLiteral("bash"), params);
#endif

    if (!wait) {
        return true;
    }
    if (!m_sndcpy.waitForStarted(5000)) {
        qWarning() << "AudioOutput::" << serial << "start sndcpy process failed";
        return false;
    }
    // 绝不无限阻塞 UI 线程：脚本的等待循环若卡住（如设备端 grep 无匹配），
    // 20 秒后强杀进程并放弃该设备的音频，绝不能拖垮投屏连接本身。
    if (!m_sndcpy.waitForFinished(20000)) {
        qWarning() << "AudioOutput::" << serial << "sndcpy process timed out, killing";
        m_sndcpy.kill();
        m_sndcpy.waitForFinished(1000);
        return false;
    }
    return true;
}

void AudioSession::startAudioOutput()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
    if (!info.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported, cannot play audio.";
        return;
    }
    m_audioOutput = new QAudioOutput(format, this);
    connect(m_audioOutput, &QAudioOutput::stateChanged, this, [this](QAudio::State state) {
        qInfo() << "AudioOutput::" << serial << "audio state changed:" << state;
    });
    m_audioOutput->setBufferSize(48000 * 2 * 15 / 1000 * 20);
    m_outputDevice = m_audioOutput->start();
#else
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
    if (!defaultDevice.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported, cannot play audio.";
        return;
    }
    m_audioSink = new QAudioSink(defaultDevice, format, this);
    m_outputDevice = m_audioSink->start();
    if (!m_outputDevice) {
        qWarning() << "AudioOutput::audio output device not available, cannot play audio.";
        delete m_audioSink;
        m_audioSink = nullptr;
        return;
    }
#endif
}

void AudioSession::stopAudioOutput()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
    }
#else
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
#endif
    m_outputDevice = nullptr;
}

void AudioSession::startRecvData()
{
    if (m_workerThread.isRunning()) {
        stopRecvData();
    }

    auto audioSocket = new QTcpSocket();
    audioSocket->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, audioSocket, &QObject::deleteLater);

    // sndcpy 脚本的设备端 Activity 就绪可能远晚于脚本退出（就绪检查恒真），
    // 一次性 500ms 连接会永久失败。改为在 worker 线程定时重试，直至连上。
    connect(this, &AudioSession::connectTo, audioSocket, [this, audioSocket](int p) {
        auto tryConnect = [this, audioSocket, p]() {
            if (!running.load()) {
                return; // 会话已停止，不再重连
            }
            if (audioSocket->state() != QAbstractSocket::UnconnectedState) {
                return; // 正在连接或已连接
            }
            audioSocket->connectToHost(QHostAddress::LocalHost, p);
        };
        tryConnect();
        // 失败/断开后每 1 秒重试（Qt 事件循环在 worker 线程中运行）
        QTimer *retry = new QTimer(audioSocket);
        retry->setInterval(1000);
        connect(retry, &QTimer::timeout, audioSocket, tryConnect);
        connect(audioSocket, &QAbstractSocket::stateChanged, audioSocket,
                [retry](QAbstractSocket::SocketState state) {
            if (state == QAbstractSocket::ConnectedState) {
                retry->stop();
            } else if (state == QAbstractSocket::UnconnectedState) {
                retry->start();
            }
        });
        retry->start();
    });
    connect(audioSocket, &QIODevice::readyRead, audioSocket, [this, audioSocket]() {
        const qint64 recv = audioSocket->bytesAvailable();
        if (recv <= 0) {
            return;
        }
        if (m_buffer.size() < recv) {
            m_buffer.resize(static_cast<int>(recv));
        }
        const qint64 count = audioSocket->read(m_buffer.data(), recv);
        if (count <= 0) {
            return;
        }

        // 仅分发给本设备会话的监听器（每台设备的录制器只收到自己的声音）
        {
            QMutexLocker locker(&m_listenerMutex);
            for (auto it = m_listeners.constBegin(); it != m_listeners.constEnd(); ++it) {
                it.value()(m_buffer.data(), static_cast<int>(count));
            }
        }
        if (m_outputDevice) {
            m_outputDevice->write(m_buffer.data(), count);
        }
    });
    connect(audioSocket, &QTcpSocket::stateChanged, audioSocket, [this](QAbstractSocket::SocketState state) {
        qInfo() << "AudioOutput::" << serial << "audio socket state changed:" << state;
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(audioSocket, &QTcpSocket::errorOccurred, audioSocket, [this](QAbstractSocket::SocketError error) {
        qInfo() << "AudioOutput::" << serial << "audio socket error occurred:" << error;
    });
#else
    connect(audioSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), audioSocket, [this](QAbstractSocket::SocketError error) {
        qInfo() << "AudioOutput::" << serial << "audio socket error occurred:" << error;
    });
#endif

    m_workerThread.start();
    emit connectTo(port);
}

void AudioSession::stopRecvData()
{
    if (!m_workerThread.isRunning()) {
        return;
    }
    m_workerThread.quit();
    m_workerThread.wait();
}

// ---------------------------------------------------------------- AudioOutput

AudioOutput::AudioOutput(QObject *parent) : QObject(parent)
{
}

AudioOutput::~AudioOutput()
{
    const QStringList serials = m_sessions.keys();
    for (const QString &s : serials) {
        stopForDevice(s);
    }
}

AudioOutput &AudioOutput::instance()
{
    // 故意泄漏：音频桥接必须比各控件存活更久，随进程退出而释放
    static AudioOutput *s_instance = new AudioOutput();
    return *s_instance;
}

int AudioOutput::availablePort(int preferred) const
{
    QHash<int, bool> used;
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value() && it.value()->running) {
            used.insert(it.value()->port, true);
        }
    }
    if (preferred > 0 && !used.contains(preferred)) {
        return preferred;
    }
    int port = kBaseAudioPort;
    while (used.contains(port)) {
        ++port;
    }
    return port;
}

bool AudioOutput::start(const QString &serial, int port)
{
    if (serial.trimmed().isEmpty()) {
        return false;
    }
    // 同一设备重复启动：先彻底拆掉旧桥，避免进程/forward 残留
    if (m_sessions.contains(serial)) {
        stopForDevice(serial);
    }
    const int actualPort = availablePort(port);
    auto *session = new AudioSession(serial, actualPort, this);
    if (!session->start()) {
        session->deleteLater();
        return false;
    }
    m_sessions.insert(serial, session);
    m_lastSerial = serial;
    qInfo() << "AudioOutput::audio bridge started for" << serial << "on port" << actualPort;
    return true;
}

void AudioOutput::stop()
{
    const QStringList serials = m_sessions.keys();
    for (const QString &s : serials) {
        stopForDevice(s);
    }
}

void AudioOutput::stopForDevice(const QString &serial)
{
    auto it = m_sessions.find(serial);
    if (it == m_sessions.end() || !it.value()) {
        return; // 幂等：该设备无音频会话
    }
    AudioSession *session = it.value();
    m_sessions.erase(it);
    session->stop();
    session->stopBridge();
    session->deleteLater();
    if (m_lastSerial == serial) {
        m_lastSerial.clear();
    }
    qInfo() << "AudioOutput::audio bridge stopped for" << serial;
}

void AudioOutput::installonly(const QString &serial, int port)
{
    // 仅安装/唤醒 sndcpy，不持有会话：使用独立的一次性进程
#ifdef Q_OS_WIN32
    const QString program = QStringLiteral("sndcpy.bat");
    const QStringList params{serial, QString::number(port > 0 ? port : kBaseAudioPort)};
#else
    const QString program = QStringLiteral("bash");
    const QStringList params{QStringLiteral("sndcpy.sh"), serial,
                             QString::number(port > 0 ? port : kBaseAudioPort)};
#endif
    auto *process = new QProcess(this);
    process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process,
            [process](int, QProcess::ExitStatus) { process->deleteLater(); });
    process->start(program, params);
}

bool AudioOutput::isRunning() const
{
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value() && it.value()->running) {
            return true;
        }
    }
    return false;
}

bool AudioOutput::isRunning(const QString &serial) const
{
    auto it = m_sessions.constFind(serial);
    return it != m_sessions.constEnd() && it.value() && it.value()->running;
}

QString AudioOutput::serial() const
{
    return m_lastSerial;
}

int AudioOutput::portOf(const QString &serial) const
{
    auto it = m_sessions.constFind(serial);
    return (it != m_sessions.constEnd() && it.value()) ? it.value()->port : 0;
}

void AudioOutput::addPcmListener(const QString &serial, const QString &key,
                                 std::function<void(const char *, int)> listener)
{
    auto it = m_sessions.find(serial);
    if (it != m_sessions.end() && it.value()) {
        it.value()->addListener(key, std::move(listener));
    }
}

void AudioOutput::removePcmListener(const QString &serial, const QString &key)
{
    auto it = m_sessions.find(serial);
    if (it != m_sessions.end() && it.value()) {
        it.value()->removeListener(key);
    }
}
