#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>

#include "server.h"

#define DEVICE_NAME_FIELD_LENGTH 64
#define VIDEO_META_LENGTH 16
#define SOCKET_NAME_PREFIX "scrcpy"
#define MAX_CONNECT_COUNT 30
// 启动阶段自动重试次数（每次重试前会清理设备端残留 server）
#define MAX_START_RETRY 2
// 重试前的退避时间：给设备端 adb/pkill 留出回收时间
#define START_RETRY_DELAY_MS 500
// reverse 模式的接受连接超时。原实现仅 1 秒，而 Android 14+/定制 ROM 上
// app_process 冷启动 + MediaCodec 初始化经常超过 1 秒，会误判为启动失败。
#define ACCEPT_TIMEOUT_MS 6000

static quint32 bufferRead32be(quint8 *buf)
{
    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

Server::Server(QObject *parent) : QObject(parent)
{
    connect(&m_workProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);
    connect(&m_serverProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);

    connect(&m_serverSocket, &QTcpServer::newConnection, this, [this]() {
        QTcpSocket *tmp = m_serverSocket.nextPendingConnection();
        if (dynamic_cast<VideoSocket *>(tmp)) {
            m_videoSocket = dynamic_cast<VideoSocket *>(tmp);
            if (!m_videoSocket->isValid() || !readInfo(m_videoSocket, m_deviceName, m_deviceSize)) {
                // 读取设备信息失败：走统一重试，而不是直接宣告失败
                retryStart("read device info failed");
            }
        } else {
            m_controlSocket = tmp;
            if (m_controlSocket && m_controlSocket->isValid()) {
                // we don't need the server socket anymore
                // 仅 m_videoSocket 即可
                m_serverSocket.close();
                // we don't need the adb tunnel anymore
                disableTunnelReverse();
                m_tunnelEnabled = false;
                m_sessionActive = true;
                emit serverStarted(true, m_deviceName, m_deviceSize);
            } else {
                retryStart("invalid control socket");
            }
            stopAcceptTimeoutTimer();
        }
    });
}

Server::~Server() {}

bool Server::pushServer()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.push(m_params.serial, m_params.serverLocalPath, m_params.serverRemotePath);
    return true;
}

bool Server::enableTunnelReverse()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.reverse(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')), m_params.localPort);
    return true;
}

bool Server::disableTunnelReverse()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->reverseRemove(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}

bool Server::enableTunnelForward()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.forward(m_params.serial, m_params.localPort, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
bool Server::disableTunnelForward()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}

bool Server::execute()
{
    if (m_serverProcess.isRuning()) {
        m_serverProcess.kill();
    }
    QStringList args;
    args << "shell";
    args << QString("CLASSPATH=%1").arg(m_params.serverRemotePath);
    args << "app_process";

#ifdef SERVER_DEBUGGER
#define SERVER_DEBUGGER_PORT "5005"

    args <<
#ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,server=y,address="
#else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
#endif
        SERVER_DEBUGGER_PORT,
#endif

        args << "/"; // 未使用;
    args << "com.genymobile.scrcpy.Server";
    args << m_params.serverVersion;

    args << QString("video_bit_rate=%1").arg(QString::number(m_params.bitRate));
    const bool cameraMode = m_params.videoSource == qsc::VIDEO_SOURCE_CAMERA;
    if (cameraMode) {
        args << "video_source=camera";
        if (!m_params.cameraId.isEmpty()) {
            args << QString("camera_id=%1").arg(m_params.cameraId);
        } else {
            args << QString("camera_facing=%1")
                        .arg(m_params.cameraFacing == qsc::CAMERA_FACING_FRONT ? "front" : "back");
        }
    }
    if (!m_params.logLevel.isEmpty()) {
        args << QString("log_level=%1").arg(m_params.logLevel);
    }
    if (m_params.maxSize > 0) {
        args << QString("max_size=%1").arg(QString::number(m_params.maxSize));
    }
    if (m_params.maxFps > 0) {
        args << QString("max_fps=%1").arg(QString::number(m_params.maxFps));
    }

    // 采集方向参数（@90 表示锁定并旋转 90°，无 @ 表示不锁定）
    // 有@表示锁定，没@不锁定
    // 有值表示指定方向，没值表示原始方向
    if (1 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@%1").arg(m_params.captureOrientation);
    } else if (2 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@");
    } else {
        args << QString("capture_orientation=%1").arg(m_params.captureOrientation);
    }
    if (m_tunnelForward) {
        args << QString("tunnel_forward=true");
    }
    if (!cameraMode) {
        if (!m_params.crop.isEmpty()) {
            args << QString("crop=%1").arg(m_params.crop);
        }
        if (!m_params.newDisplay.isEmpty()) {
            args << QString("new_display=%1").arg(m_params.newDisplay);
            if (m_params.flexDisplay) {
                args << "flex_display=true";
            }
            if (!m_params.vdDestroyContent) {
                args << "vd_destroy_content=false";
            }
            if (!m_params.vdSystemDecorations) {
                args << "vd_system_decorations=false";
            }
            if (!m_params.displayImePolicy.isEmpty()) {
                args << QString("display_ime_policy=%1").arg(m_params.displayImePolicy);
            }
        } else if (m_params.displayId > 0) {
            args << QString("display_id=%1").arg(m_params.displayId);
        }
    }
    if (!m_params.control) {
        args << QString("control=false");
    }
    // 默认是0，不需要设置
    // args << "display_id=0";
    // 默认是false，不需要设置
    // args << "show_touches=false";
    if (!cameraMode) {
        if (m_params.stayAwake) {
            args << QString("stay_awake=true");
        }
        if (m_params.keepActive) {
            args << "keep_active=true";
        }
    }
    // 编码选项
    // https://github.com/Genymobile/scrcpy/commit/080a4ee3654a9b7e96c8ffe37474b5c21c02852a
    // <https://d.android.com/reference/android/media/MediaFormat>
    if (!m_params.codecOptions.isEmpty()) {
        args << QString("video_codec_options=%1").arg(m_params.codecOptions);
    }
    if (!m_params.codecName.isEmpty()) {
        args << QString("video_encoder=%1").arg(m_params.codecName);
    }
    args << "audio=false";
    // 服务端默认-1，可不传
    if (-1 != m_params.scid) {
        args << QString("scid=%1").arg(m_params.scid, 8, 16, QChar('0'));
    }

    // 默认是false，不需要设置
    // args << "power_off_on_close=false";

    // 下面的参数都用服务端默认值即可，尽量减少参数传递，传参太长导致三星手机报错：stack corruption detected (-fstack-protector)
    /*
    args << "clipboard_autosync=true";    
    args << "downsize_on_error=true";
    args << "cleanup=true";
    args << "power_on=true";
    
    args << "send_device_meta=true";
    args << "send_frame_meta=true";
    args << "send_dummy_byte=true";
    args << "raw_video_stream=false";
    */

#ifdef SERVER_DEBUGGER
    qInfo("Server debugger waiting for a client on device port " SERVER_DEBUGGER_PORT "...");
    // 在电脑上执行
    //     adb forward tcp:5005 tcp:5005
    // 然后在 Android Studio 中：Run > Debug > Edit configurations...
    // 左侧点击 '+'、"Remote"，填写：
    //     Host: localhost
    //     Port: 5005
    // 然后点击 "Debug"
#endif

    // adb -s P7C0218510000537 shell CLASSPATH=/data/local/tmp/scrcpy-server app_process / com.genymobile.scrcpy.Server 0 8000000 false
    // 备注：裁剪输入格式为 "width:height:x:y"，空字符串表示不裁剪，例如："100:200:0:0"
    // 这条adb命令是阻塞运行的，m_serverProcess进程不会退出了
    m_serverProcess.execute(m_params.serial, args);
    return true;
}

bool Server::start(Server::ServerParams params)
{
    m_params = params;
    // 外部（重新）发起连接时才重置重试计数与挂起标记
    m_startRetryCount = 0;
    m_retryPending = false;
    m_sessionActive = false;
    m_stopRequested = false;
    m_serverStartStep = SSS_PUSH;
    return startServerByStep();
}

bool Server::connectTo()
{
    if (SSS_RUNNING != m_serverStartStep) {
        qWarning("server not run");
        return false;
    }

    if (!m_tunnelForward && !m_videoSocket) {
        startAcceptTimeoutTimer();
        return true;
    }

    startConnectTimeoutTimer();
    return true;
}

bool Server::isReverse()
{
    return !m_tunnelForward;
}

Server::ServerParams Server::getParams()
{
    return m_params;
}

void Server::timerEvent(QTimerEvent *event)
{
    if (event && m_acceptTimeoutTimer == event->timerId()) {
        stopAcceptTimeoutTimer();
        retryStart("accept connection timeout");
    } else if (event && m_connectTimeoutTimer == event->timerId()) {
        onConnectTimer();
    }
}

VideoSocket* Server::removeVideoSocket()
{
    VideoSocket* socket = m_videoSocket;
    m_videoSocket = Q_NULLPTR;
    return socket;
}

QTcpSocket *Server::getControlSocket()
{
    return m_controlSocket;
}

void Server::stop()
{
    // 外部主动停止（用户点停止/断开/窗口关闭）时，取消尚未执行的自动重试，
    // 避免停止后又被重试逻辑拉起来
    m_retryPending = false;
    m_sessionActive = false;
    m_stopRequested = true;

    if (m_tunnelForward) {
        stopConnectTimeoutTimer();
    } else {
        stopAcceptTimeoutTimer();
    }

    if (m_controlSocket) {
        m_controlSocket->close();
        m_controlSocket->deleteLater();
    }
    // 连接失败路径上视频 socket 可能已建立但未被 Device 取走，这里一并释放
    if (m_videoSocket) {
        m_videoSocket->close();
        m_videoSocket->deleteLater();
        m_videoSocket = Q_NULLPTR;
    }
    // 忽略失败
    m_serverProcess.kill();
    if (m_tunnelEnabled) {
        if (m_tunnelForward) {
            disableTunnelForward();
        } else {
            disableTunnelReverse();
        }
        m_tunnelForward = false;
        m_tunnelEnabled = false;
    }
    m_serverSocket.close();
}

bool Server::startServerByStep()
{
    bool stepSuccess = false;
    // 推送、建立隧道并启动服务端
    if (SSS_NULL != m_serverStartStep) {
        switch (m_serverStartStep) {
        case SSS_PUSH:
            stepSuccess = pushServer();
            break;
        case SSS_ENABLE_TUNNEL_REVERSE:
            stepSuccess = enableTunnelReverse();
            break;
        case SSS_ENABLE_TUNNEL_FORWARD:
            stepSuccess = enableTunnelForward();
            break;
        case SSS_EXECUTE_SERVER:
            // 服务端将连接到我们的服务端 socket
            stepSuccess = execute();
            break;
        default:
            break;
        }
    }

    if (!stepSuccess) {
        emit serverStarted(false);
    }
    return stepSuccess;
}

bool Server::readInfo(VideoSocket *videoSocket, QString &deviceName, QSize &size)
{
    QElapsedTimer timer;
    timer.start();
    unsigned char buf[DEVICE_NAME_FIELD_LENGTH + VIDEO_META_LENGTH];
    while (videoSocket->bytesAvailable() < (DEVICE_NAME_FIELD_LENGTH + VIDEO_META_LENGTH)) {
        videoSocket->waitForReadyRead(300);
        if (timer.elapsed() > 3000) {
            qInfo("readInfo timeout");
            return false;
        }
    }
    qDebug() << "readInfo wait time:" << timer.elapsed();

    qint64 len = videoSocket->read((char *)buf, sizeof(buf));
    if (len < DEVICE_NAME_FIELD_LENGTH + VIDEO_META_LENGTH) {
        qInfo("Could not retrieve device information");
        return false;
    }
    buf[DEVICE_NAME_FIELD_LENGTH - 1] = '\0'; // in case the client sends garbage
    deviceName = QString::fromUtf8((const char *)buf);

    // scrcpy 4.x：先是 codec id（4 字节），随后是会话元数据（flags、宽、高）。
    size.setWidth(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 8]));
    size.setHeight(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 12]));

    return true;
}

void Server::startAcceptTimeoutTimer()
{
    stopAcceptTimeoutTimer();
    m_acceptTimeoutTimer = startTimer(ACCEPT_TIMEOUT_MS);
}

void Server::stopAcceptTimeoutTimer()
{
    if (m_acceptTimeoutTimer) {
        killTimer(m_acceptTimeoutTimer);
        m_acceptTimeoutTimer = 0;
    }
}

void Server::startConnectTimeoutTimer()
{
    stopConnectTimeoutTimer();
    m_connectTimeoutTimer = startTimer(300);
}

void Server::stopConnectTimeoutTimer()
{
    if (m_connectTimeoutTimer) {
        killTimer(m_connectTimeoutTimer);
        m_connectTimeoutTimer = 0;
    }
    m_connectCount = 0;
}

void Server::onConnectTimer()
{
    // 设备端服务端需要时间启动
    // 这里连接太早时间不够导致安卓监听socket还没有建立，readInfo会失败，所以采取定时重试策略
    // 每隔100ms尝试一次，最多尝试MAX_CONNECT_COUNT次
    QString deviceName;
    QSize deviceSize;
    bool success = false;

    VideoSocket *videoSocket = new VideoSocket();
    QTcpSocket *controlSocket = new QTcpSocket();

    videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
    if (!videoSocket->waitForConnected(1000)) {
        // 连接到adb很快的，这里失败不重试
        m_connectCount = MAX_CONNECT_COUNT;
        qWarning("video socket connect to server failed");
        goto result;
    }

    controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
    if (!controlSocket->waitForConnected(1000)) {
        // 连接到adb很快的，这里失败不重试
        m_connectCount = MAX_CONNECT_COUNT;
        qWarning("control socket connect to server failed");
        goto result;
    }

    if (QTcpSocket::ConnectedState == videoSocket->state()) {
        // 即使设备离线连接也会“成功”，真正连接成功以收到数据为准，
        // 因为这里连接的是电脑侧的 adb server
        videoSocket->waitForReadyRead(1000);
        // 在隧道转发模式下，设备会先发送 1 字节
        QByteArray data = videoSocket->read(1);
        if (!data.isEmpty() && readInfo(videoSocket, deviceName, deviceSize)) {
            success = true;
            goto result;
        } else {
            qWarning("video socket connect to server read device info failed, try again");
            goto result;
        }
    } else {
        qWarning("connect to server failed");
        m_connectCount = MAX_CONNECT_COUNT;
        goto result;
    }

result:
    if (success) {
        stopConnectTimeoutTimer();
        m_videoSocket = videoSocket;
        // 在隧道转发模式下，设备会先发送 1 字节
        controlSocket->read(1);
        m_controlSocket = controlSocket;
        // we don't need the adb tunnel anymore
        disableTunnelForward();
        m_tunnelEnabled = false;
        m_startRetryCount = 0;
        m_sessionActive = true;
        emit serverStarted(success, deviceName, deviceSize);
        return;
    }

    if (videoSocket) {
        videoSocket->deleteLater();
    }
    if (controlSocket) {
        controlSocket->deleteLater();
    }

    if (MAX_CONNECT_COUNT <= m_connectCount++) {
        stopConnectTimeoutTimer();
        retryStart("connect to server timeout");
    }
}

void Server::retryStart(const QString &reason)
{
    // 已排定一次重试：期间重复的失败上报直接忽略，避免并发重启
    if (m_retryPending) {
        return;
    }
    m_serverStartStep = SSS_NULL;
    // 清理本次尝试的残留：socket、adb 进程、隧道。
    // stop() 会置位 m_stopRequested，这里是重试路径内部的清理，
    // 需要复位该标记，否则后续步骤的失败会被误判为"已被外部停止"
    stop();
    m_stopRequested = false;

    if (m_startRetryCount >= MAX_START_RETRY) {
        qCritical("server start failed after %u retries: %s",
                  m_startRetryCount,
                  reason.toStdString().c_str());
        m_startRetryCount = 0;
        emit serverStarted(false);
        return;
    }

    ++m_startRetryCount;
    m_retryPending = true;
    qWarning("restart server (%u/%u), reason: %s",
             m_startRetryCount,
             MAX_START_RETRY,
             reason.toStdString().c_str());

    // 设备端上一次的 app_process 可能还没退出（进程崩溃残留/被系统挂起），
    // 它会占住显示与抽象 socket，导致本次启动直接失败。重试前先清理。
    killStaleServer();

    QTimer::singleShot(START_RETRY_DELAY_MS, this, [this]() {
        // 期间被外部 stop() 取消了重试（m_retryPending 被置 false）
        if (!m_retryPending || SSS_NULL != m_serverStartStep) {
            m_retryPending = false;
            return;
        }
        m_retryPending = false;
        m_serverStartStep = SSS_PUSH;
        startServerByStep();
    });
}

void Server::killStaleServer()
{
    // 使用独立的 adb 进程，不占用 m_workProcess，也不阻塞启动流程；
    // 失败（设备离线/无 pkill）时静默忽略，仅作为提高成功率的补救措施。
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    QStringList args;
    args << "shell"
         << "pkill"
         << "-f"
         << "com.genymobile.scrcpy";
    adb->execute(m_params.serial, args);
}

void Server::onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    if (sender() == &m_workProcess) {
        if (SSS_NULL != m_serverStartStep) {
            switch (m_serverStartStep) {
            case SSS_PUSH:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    if (m_params.useReverse) {
                        m_serverStartStep = SSS_ENABLE_TUNNEL_REVERSE;
                    } else {
                        m_tunnelForward = true;
                        m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    }
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb push failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            case SSS_ENABLE_TUNNEL_REVERSE:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    // 在应用层，设备端被称为“服务端”，因为它提供视频流与控制；
                    // 但在网络层，客户端监听、服务端反向连接客户端。这样客户端可以在
                    // 启动服务端程序前就先监听，无需在服务端 socket 就绪前尝试连接。
                    m_serverSocket.setMaxPendingConnections(2);
                    if (!m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
                        qCritical() << QString("Could not listen on port %1").arg(m_params.localPort).toStdString().c_str();
                        m_serverStartStep = SSS_NULL;
                        disableTunnelReverse();
                        emit serverStarted(false);
                        break;
                    }

                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    // 有一些设备reverse会报错more than o'ne device，adb的bug
                    // https://github.com/Genymobile/scrcpy/issues/5
                    qCritical("adb reverse failed");
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    startServerByStep();
                }
                break;
            case SSS_ENABLE_TUNNEL_FORWARD:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb forward failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            default:
                break;
            }
        }
    }
    if (sender() == &m_serverProcess) {
        if (SSS_EXECUTE_SERVER == m_serverStartStep) {
            if (qsc::AdbProcess::AER_SUCCESS_START == processResult) {
                m_serverStartStep = SSS_RUNNING;
                m_tunnelEnabled = true;
                connectTo();
            } else if (qsc::AdbProcess::AER_SUCCESS_EXEC != processResult) {
                // 进程在启动阶段就结束了：app_process 崩溃/CrashExit/被系统杀掉/
                // adb 连接抖动。原实现只处理 AER_ERROR_START，AER_ERROR_EXEC 会
                // 落空 -> 既不上报失败也不启动超时，界面永远停在加载中。
                qCritical("adb shell start server failed (result=%d)", static_cast<int>(processResult));
                retryStart("server exited during startup");
            }
        } else if (SSS_RUNNING == m_serverStartStep) {
            m_serverStartStep = SSS_NULL;
            if (!m_stopRequested && !m_sessionActive) {
                // 会话尚未建立（还在等设备端反向连接/握手）进程就退出了，且
                // 没有任何 stop 请求：典型是 app_process 崩溃（jar 损坏、被
                // ROM 杀、SELinux 拒绝）。原实现直接静默断开，用户只看到
                // "启动不成功"。这里走自动重试。
                qCritical("server exited before session established");
                retryStart("server exited before session established");
            } else {
                // 正常路径：会话已建立后服务端停止（用户断开/流结束触发的清理），
                // 或者 stop() 已主动清理（此时进程退出只是清理的结果）
                m_sessionActive = false;
                emit serverStoped();
            }
        }
    }
}
