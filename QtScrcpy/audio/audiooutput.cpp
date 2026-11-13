#include <QAudioOutput>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTime>

#include <utility>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#endif

#include "audiooutput.h"

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{
    m_running = false;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_audioOutput = nullptr;
#else
    m_audioSink = nullptr;
#endif
    connect(&m_sndcpy, &QProcess::readyReadStandardOutput, this, [this]() {
        qInfo() << QString("AudioOutput::") << QString(m_sndcpy.readAllStandardOutput());
    });
    connect(&m_sndcpy, &QProcess::readyReadStandardError, this, [this]() {
        qInfo() << QString("AudioOutput::") << QString(m_sndcpy.readAllStandardError());
    });
}

AudioOutput::~AudioOutput()
{
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
    }
    stop();
}

AudioOutput &AudioOutput::instance()
{
    // 故意泄漏：音频桥接必须比各控件存活更久，随进程退出而释放，
    // 而不是在静态析构阶段被销毁。
    static AudioOutput *s_instance = new AudioOutput();
    return *s_instance;
}

bool AudioOutput::isRunning() const
{
    return m_running;
}

QString AudioOutput::serial() const
{
    return m_serial;
}

void AudioOutput::addPcmListener(const QString &key, std::function<void(const char *, int)> listener)
{
    QMutexLocker locker(&m_listenerMutex);
    m_pcmListeners.insert(key, std::move(listener));
}

void AudioOutput::removePcmListener(const QString &key)
{
    QMutexLocker locker(&m_listenerMutex);
    m_pcmListeners.remove(key);
}

bool AudioOutput::start(const QString& serial, int port)
{
    if (m_running) {
        stop();
    }
    m_serial.clear();
    m_port = port;

    QElapsedTimer timeConsumeCount;
    timeConsumeCount.start();
    bool ret = runSndcpyProcess(serial, port);
    qInfo() << "AudioOutput::run sndcpy cost:" << timeConsumeCount.elapsed() << "milliseconds";
    if (!ret) {
        return ret;
    }

    startAudioOutput();
    startRecvData(port);

    m_running = true;
    m_serial = serial;
    return true;
}

void AudioOutput::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;

    stopRecvData();
    stopAudioOutput();

    // 任何监听器都不应比音频会话存活更久（例如设备窗口已关闭
    // 时仍附属着的录制器）。
    {
        QMutexLocker locker(&m_listenerMutex);
        m_pcmListeners.clear();
    }
    m_serial.clear();
}

void AudioOutput::stopForDevice(const QString& serial)
{
    if (!m_running) {
        return; // 已停止，保持调用幂等
    }
    if (!m_serial.isEmpty() && m_serial != serial) {
        return; // 音频属于另一台已连接设备
    }

    stop();

    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
    }
    teardownDeviceBridge(serial);
}

void AudioOutput::teardownDeviceBridge(const QString& serial)
{
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

    QStringList baseArgs;
    if (!serial.isEmpty()) {
        baseArgs << QStringLiteral("-s") << serial;
    }

    const QStringList commands[] = {
        QStringList{QStringLiteral("forward"), QStringLiteral("--remove"),
                    QStringLiteral("tcp:%1").arg(m_port)},
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

void AudioOutput::installonly(const QString &serial, int port)
{
    runSndcpyProcess(serial, port, false);
}

bool AudioOutput::runSndcpyProcess(const QString &serial, int port, bool wait)
{
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
    }

#ifdef Q_OS_WIN32
    QStringList params{serial, QString::number(port)};
    // sndcpy.bat 会在其工作目录中查找 adb.exe 与 sndcpy.apk。
    // 始终在可执行文件旁边运行它，这样即使程序从其他目录启动，录制音频也能正常工作。
    m_sndcpy.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_sndcpy.start("sndcpy.bat", params);
#else
    QStringList params{"sndcpy.sh", serial, QString::number(port)};
    m_sndcpy.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_sndcpy.start("bash", params);
#endif

    if (!wait) {
        return true;
    }

    if (!m_sndcpy.waitForStarted()) {
        qWarning() << "AudioOutput::start sndcpy process failed";
        return false;
    }
    if (!m_sndcpy.waitForFinished()) {
        qWarning() << "AudioOutput::sndcpy process crashed";
        return false;
    }

    return true;
}

void AudioOutput::startAudioOutput()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) {
        return;
    }

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
    connect(m_audioOutput, &QAudioOutput::stateChanged, this, [](QAudio::State state) {
        qInfo() << "AudioOutput::audio state changed:" << state;
    });
    m_audioOutput->setBufferSize(48000*2*15/1000 * 20);
    m_outputDevice = m_audioOutput->start();
#else
    if (m_audioSink) {
        return;
    }

    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
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

void AudioOutput::stopAudioOutput()
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

void AudioOutput::startRecvData(int port)
{
    if (m_workerThread.isRunning()) {
        stopRecvData();
    }

    auto audioSocket = new QTcpSocket();
    audioSocket->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, audioSocket, &QObject::deleteLater);

    connect(this, &AudioOutput::connectTo, audioSocket, [audioSocket](int port) {
        audioSocket->connectToHost(QHostAddress::LocalHost, port);
        if (!audioSocket->waitForConnected(500)) {
            qWarning("AudioOutput::audio socket connect failed");
            return;
        }
        qInfo("AudioOutput::audio socket connect success");
    });
    connect(audioSocket, &QIODevice::readyRead, audioSocket, [this, audioSocket]() {
        qint64 recv = audioSocket->bytesAvailable();
        //qDebug() << "AudioOutput::recv data:" << recv;
        if (recv <= 0) {
            return;
        }

        if (m_buffer.size() < recv) {
            m_buffer.resize(static_cast<int>(recv));
        }

        qint64 count = audioSocket->read(m_buffer.data(), recv);
        if (count <= 0) {
            return;
        }

        // 录制时播放可能不可用，因此直接转发原始 PCM
        // to the listeners first.
        {
            QMutexLocker locker(&m_listenerMutex);
            for (auto it = m_pcmListeners.constBegin(); it != m_pcmListeners.constEnd(); ++it) {
                it.value()(m_buffer.data(), static_cast<int>(count));
            }
        }

        if (m_outputDevice) {
            m_outputDevice->write(m_buffer.data(), count);
        }
    });
    connect(audioSocket, &QTcpSocket::stateChanged, audioSocket, [](QAbstractSocket::SocketState state) {
        qInfo() << "AudioOutput::audio socket state changed:" << state;

    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(audioSocket, &QTcpSocket::errorOccurred, audioSocket, [](QAbstractSocket::SocketError error) {
        qInfo() << "AudioOutput::audio socket error occurred:" << error;
    });
#else
    connect(audioSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), audioSocket, [](QAbstractSocket::SocketError error) {
        qInfo() << "AudioOutput::audio socket error occurred:" << error;
    });
#endif

    m_workerThread.start();
    emit connectTo(port);
}

void AudioOutput::stopRecvData()
{
    if (!m_workerThread.isRunning()) {
        return;
    }

    m_workerThread.quit();
    m_workerThread.wait();
}
