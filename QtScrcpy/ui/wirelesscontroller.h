#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace qsc {
class AdbProcess;
}

/// 无线调试（Android 11+ 配对）控制器。
///
/// 把 pair → connect → devices 校验串成一个可观测、可超时、可失败分类的
/// 状态机，替代 Dialog 里用 delayMs() 阻塞串流程的旧做法。
/// 所有 adb 调用走 AdbProcess::executeWithTimeout（独立进程 + 超时兜底），
/// 不占用主界面的串行 adb 通道。
class WirelessController : public QObject
{
    Q_OBJECT
public:
    enum State {
        Idle,        // 空闲
        Discovering, // 正在 mDNS 搜索
        Pairing,     // 正在配对
        Connecting,  // 正在连接
        Connected,   // 已连接
        Failed       // 失败（等待用户处理）
    };
    Q_ENUM(State)

    struct Target
    {
        QString ip;
        quint16 connectPort = 0; // 无线调试主界面「IP 地址和端口」中的端口
        quint16 pairPort = 0;    // 「使用配对码配对设备」弹窗中的端口
    };

    struct DiscoveredDevice
    {
        QString ip;
        quint16 pairPort = 0;
        quint16 connectPort = 0;
        QString serviceName;
    };

    explicit WirelessController(qsc::AdbProcess *adb, QObject *parent = nullptr);

    State state() const { return m_state; }
    bool isBusy() const
    {
        return Discovering == m_state || Pairing == m_state || Connecting == m_state;
    }

    /// 配对并连接（首次连接 Android 11+ 设备）
    void pairAndConnect(const Target &target, const QString &code);
    /// 仅连接（已配对过的设备 / Android 10 及以下的 tcpip 端口）
    void connectOnly(const Target &target);
    /// 断开（带端口，修掉旧实现只传 IP 的问题）
    void disconnectTarget(const Target &target);
    /// mDNS 搜索局域网内的无线调试设备
    void discover();

    /// ---- 以下静态函数便于独立测试 ----
    /// 解析 `adb mdns services` 输出，按 IP 聚合配对端口与连接端口
    static QVector<DiscoveredDevice> parseMdnsOutput(const QString &out);
    /// 配对失败原因（out 为合并后的 adb 输出）
    static QString pairErrorText(const QString &out, bool timedOut);
    /// 连接失败原因（connect 失败时退出码仍为 0，只能看文本）
    static QString connectErrorText(const QString &out, bool timedOut);
    /// adb connect 是否成功（文本判定）
    static bool isConnectSuccess(const QString &out);
    /// 主机名/IP 合法性（宽松校验，允许 IPv4 / IPv6 / 主机名）
    static bool isValidHost(const QString &host);

signals:
    void stateChanged(int state, const QString &text);
    void logMessage(const QString &text);
    void paired(const QString &pairAddr);
    void connected(const QString &connectAddr);
    void failed(const QString &reason);
    void discovered(const QVector<WirelessController::DiscoveredDevice> &devices);

private:
    void setState(State s, const QString &text);
    void failWith(const QString &reason);
    void doConnect(const Target &target);

private:
    qsc::AdbProcess *m_adb = nullptr;
    State m_state = Idle;
};
