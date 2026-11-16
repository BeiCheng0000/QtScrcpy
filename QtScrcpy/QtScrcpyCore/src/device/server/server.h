#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QPointer>
#include <QSize>

#include "../../../include/QtScrcpyCoreDef.h"
#include "adbprocess.h"
#include "tcpserver.h"
#include "videosocket.h"

class Server : public QObject
{
    Q_OBJECT

    enum SERVER_START_STEP
    {
        SSS_NULL,
        SSS_PUSH,
        SSS_ENABLE_TUNNEL_REVERSE,
        SSS_ENABLE_TUNNEL_FORWARD,
        SSS_EXECUTE_SERVER,
        SSS_RUNNING,
    };

public:
    struct ServerParams
    {
        // 必填
        QString serial = "";              // 设备序列号
        QString serverLocalPath = "";     // 本地安卓server路径

        // 可选
        QString serverRemotePath = "/data/local/tmp/scrcpy-server.jar";    // 要推送到远端设备的server路径
        quint16 localPort = 27183;     // reverse时本地监听端口
        quint16 maxSize = 720;         // 视频分辨率
        quint32 bitRate = 8000000;     // 视频比特率
        quint32 maxFps = 0;            // 视频最大帧率
        qsc::VideoSource videoSource = qsc::VIDEO_SOURCE_DISPLAY;
        qsc::CameraFacing cameraFacing = qsc::CAMERA_FACING_BACK;
        QString cameraId = "";
        bool useReverse = true;        // true:先使用adb reverse，失败后自动使用adb forward；false:直接使用adb forward
        int captureOrientationLock = 0; // 是否锁定采集方向 0不锁定 1锁定指定方向 2锁定原始方向
        int captureOrientation = 0;     // 采集方向 0 90 180 270
        int stayAwake = false;         // 是否保持唤醒
        QString serverVersion = "4.1"; // server版本
        QString logLevel = "debug";  // log级别 verbose/debug/info/warn/error
        // 编码选项 ""表示默认
        // 例如 CodecOptions="profile=1,level=2"
        // 更多编码选项参考 https://d.android.com/reference/android/media/MediaFormat
        QString codecOptions = "";
        // 指定编码器名称(必须是H.264编码器)，""表示默认
        // 例如 CodecName="OMX.qcom.video.encoder.avc"
        QString codecName = "";

        QString crop = "";             // 视频裁剪
        qint32 displayId = 0;
        QString newDisplay = "";
        bool flexDisplay = false;
        bool vdDestroyContent = true;
        bool vdSystemDecorations = true;
        QString displayImePolicy = "";
        bool keepActive = false;
        bool control = true;           // 安卓端是否接收键鼠控制
        qint32 scid = -1;             // 随机数，作为localsocket名字后缀，方便同时连接同一个设备多次
    };

    explicit Server(QObject *parent = nullptr);
    virtual ~Server();

    bool start(Server::ServerParams params);
    void stop();
    bool isReverse();
    Server::ServerParams getParams();
    VideoSocket *removeVideoSocket();
    QTcpSocket *getControlSocket();

signals:
    void serverStarted(bool success, const QString &deviceName = "", const QSize &size = QSize());
    void serverStoped();

private slots:
    void onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult);

protected:
    void timerEvent(QTimerEvent *event);

private:
    bool pushServer();
    bool enableTunnelReverse();
    bool disableTunnelReverse();
    bool enableTunnelForward();
    bool disableTunnelForward();
    bool execute();
    bool connectTo();
    bool startServerByStep();
    bool readInfo(VideoSocket *videoSocket, QString &deviceName, QSize &size);
    void startAcceptTimeoutTimer();
    void stopAcceptTimeoutTimer();
    void startConnectTimeoutTimer();
    void stopConnectTimeoutTimer();
    void onConnectTimer();
    // 启动失败后的统一重试入口：清理现场 -> 清理设备端残留 server -> 重新走启动流程；
    // 重试次数用尽才 emit serverStarted(false)
    void retryStart(const QString &reason);
    // 清理设备端残留的 scrcpy server 进程（上次异常退出可能留下僵尸进程占住显示）
    void killStaleServer();

private:
    qsc::AdbProcess m_workProcess;
    qsc::AdbProcess m_serverProcess;
    TcpServer m_serverSocket; // 仅在 !tunnel_forward 时使用
    QPointer<VideoSocket> m_videoSocket = Q_NULLPTR;
    QPointer<QTcpSocket> m_controlSocket = Q_NULLPTR;
    bool m_tunnelEnabled = false;
    bool m_tunnelForward = false; // use "adb forward" instead of "adb reverse"
    int m_acceptTimeoutTimer = 0;
    int m_connectTimeoutTimer = 0;
    quint32 m_connectCount = 0;
    // 启动阶段的自动重试计数（含清理设备端僵尸 server 后重试）
    quint32 m_startRetryCount = 0;
    // 已排定一次重试（期间重复的失败上报直接忽略，避免并发重启）
    bool m_retryPending = false;
    // 会话是否已真正建立（serverStarted(true) 已发出）。
    // 用于区分"启动阶段进程退出"（应重试）与"运行中服务端停止"（应正常断开）
    bool m_sessionActive = false;
    // 是否已有明确的 stop 请求（用户断开/视频流结束触发的清理）。
    // stop() 之后的进程退出信号只是清理的结果，绝不能再触发重试，
    // 否则会在 Device 已断开后复活出无人管理的僵尸连接。
    bool m_stopRequested = false;
    QString m_deviceName = "";
    QSize m_deviceSize = QSize();
    ServerParams m_params;

    SERVER_START_STEP m_serverStartStep = SSS_NULL;
};

#endif // SERVER_H
