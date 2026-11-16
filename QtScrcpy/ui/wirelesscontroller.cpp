#include "wirelesscontroller.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include "adbprocess.h"
#include "config.h"

namespace {
// 超时参数：实测不可达地址时 adb pair / connect 都要约 21 秒才返回，
// 这里留足余量并在超时后强制结束进程，避免用户以为卡死。
const int kPairTimeoutMs = 30000;
const int kConnectTimeoutMs = 40000;
const int kDevicesTimeoutMs = 10000;
const int kDiscoverTimeoutMs = 15000;
const int kDisconnectTimeoutMs = 15000;

QString addrOf(const QString &ip, quint16 port)
{
    return QString("%1:%2").arg(ip).arg(port);
}
} // namespace

WirelessController::WirelessController(qsc::AdbProcess *adb, QObject *parent)
    : QObject(parent)
    , m_adb(adb)
{
}

bool WirelessController::isValidHost(const QString &host)
{
    const QString t = host.trimmed();
    if (t.isEmpty() || t.size() > 253) {
        return false;
    }
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:%\\[\\]-]+$"));
    return re.match(t).hasMatch();
}

bool WirelessController::isConnectSuccess(const QString &out)
{
    const QString lower = out.toLower();
    if (lower.contains("cannot connect") || lower.contains("failed to connect")
        || lower.contains("unable to connect")) {
        return false;
    }
    return lower.contains("connected to") || lower.contains("already connected");
}

QString WirelessController::pairErrorText(const QString &out, bool timedOut)
{
    if (timedOut) {
        return tr("配对超时（30 秒无响应）：请确认手机与电脑在同一局域网、防火墙/VPN 未拦截，且配对端口填写正确");
    }
    const QString lower = out.toLower();
    if (lower.contains("wrong password") || lower.contains("incorrect")
        || lower.contains("failed to authenticate")) {
        return tr("配对码错误：请核对手机「无线调试」界面显示的 6 位配对码；多次错误后请重新打开无线调试界面");
    }
    if (lower.contains("unable to start pairing") || lower.contains("cannot start pairing")) {
        return tr("配对端口已失效：请在手机上重新打开「无线调试」，复制新的配对端口与配对码");
    }
    if (lower.contains("protocol fault") || lower.contains("connection refused")
        || lower.contains("no route") || lower.contains("timed out")) {
        return tr("无法连接配对端口：请确认端口填写正确（不是连接端口），且手机与电脑在同一局域网");
    }
    if (lower.contains("usage:")) {
        return tr("内置 adb 不支持配对：请升级 platform-tools 至 30.0.0 以上");
    }
    return out.trimmed().isEmpty() ? tr("配对失败，adb 未返回原因") : out.trimmed().left(200);
}

QString WirelessController::connectErrorText(const QString &out, bool timedOut)
{
    if (timedOut) {
        return tr("连接超时（40 秒无响应）：请检查网段是否一致、是否开启 VPN、防火墙是否放行");
    }
    const QString lower = out.toLower();
    if (lower.contains("cannot connect") || lower.contains("failed to connect")
        || lower.contains("unable to connect")) {
        return tr("无法连接连接端口：请确认「无线调试」仍处于开启状态，且填写的是主界面显示的连接端口（不是配对端口）");
    }
    if (lower.contains("no route") || lower.contains("unreachable")) {
        return tr("网络不可达：请确认手机与电脑在同一网段、设备未锁屏休眠");
    }
    if (lower.contains("usage:")) {
        return tr("adb 命令异常：请检查内置 adb 是否可用");
    }
    return out.trimmed().isEmpty() ? tr("连接失败，adb 未返回原因") : out.trimmed().left(200);
}

QVector<WirelessController::DiscoveredDevice> WirelessController::parseMdnsOutput(const QString &out)
{
    QVector<DiscoveredDevice> result;
    QHash<QString, int> indexByHost;

    const QStringList lines = out.split('\n');
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith("List of discovered")) {
            continue;
        }
        // 格式：服务名 \t 服务类型 \t IP:端口
        const QStringList fields = trimmed.split('\t');
        if (fields.size() < 3) {
            continue;
        }
        const QString serviceName = fields.at(0).trimmed();
        const QString type = fields.at(1).trimmed();
        if (!type.contains("_adb-tls-")) {
            continue;
        }
        const QString addr = fields.at(2).trimmed();
        const int colon = addr.lastIndexOf(':');
        if (colon <= 0) {
            continue;
        }
        QString host = addr.left(colon);
        bool ok = false;
        const quint16 port = addr.mid(colon + 1).toUShort(&ok);
        if (!ok || 0 == port) {
            continue;
        }
        // mdns 的 IPv6 地址可能带 %wlan0 接口后缀
        const int pct = host.indexOf('%');
        if (pct > 0) {
            host = host.left(pct);
        }

        int idx = indexByHost.value(host, -1);
        if (idx < 0) {
            DiscoveredDevice d;
            d.ip = host;
            d.serviceName = serviceName;
            result.append(d);
            idx = result.size() - 1;
            indexByHost.insert(host, idx);
        }
        if (type.contains("pairing")) {
            result[idx].pairPort = port;
        } else if (type.contains("connect")) {
            result[idx].connectPort = port;
        }
    }
    return result;
}

void WirelessController::setState(State s, const QString &text)
{
    m_state = s;
    emit stateChanged(int(s), text);
}

void WirelessController::failWith(const QString &reason)
{
    setState(Failed, reason);
    emit failed(reason);
}

void WirelessController::pairAndConnect(const Target &target, const QString &code)
{
    if (isBusy()) {
        return;
    }
    if (!isValidHost(target.ip)) {
        failWith(tr("设备 IP 不合法"));
        return;
    }
    if (0 == target.pairPort) {
        failWith(tr("请填写配对端口（手机「无线调试 → 使用配对码配对设备」弹窗中的端口）"));
        return;
    }
    if (0 == target.connectPort) {
        failWith(tr("请填写连接端口（手机「无线调试」主界面 IP 后显示的端口）"));
        return;
    }
    const QString pairAddr = addrOf(target.ip, target.pairPort);
    if (code.trimmed().isEmpty()) {
        failWith(tr("请填写手机显示的配对码"));
        return;
    }

    setState(Pairing, tr("正在配对 %1 …").arg(pairAddr));
    // 配对码打码，绝不写入日志
    emit logMessage(tr("adb pair %1 ******").arg(pairAddr));

    m_adb->executeWithTimeout(QString(), QStringList() << "pair" << pairAddr << code.trimmed(), kPairTimeoutMs,
                              [this, target, pairAddr](bool ok, bool timedOut, const QString &out) {
                                  if (timedOut) {
                                      failWith(pairErrorText(out, true));
                                      return;
                                  }
                                  if (!ok || !out.contains("Successfully paired")) {
                                      failWith(pairErrorText(out, false));
                                      return;
                                  }
                                  emit logMessage(tr("配对响应：%1").arg(out.trimmed().left(120)));
                                  emit paired(pairAddr);
                                  doConnect(target);
                              });
}

void WirelessController::connectOnly(const Target &target)
{
    if (isBusy()) {
        return;
    }
    if (!isValidHost(target.ip)) {
        failWith(tr("设备 IP 不合法"));
        return;
    }
    if (0 == target.connectPort) {
        failWith(tr("请填写连接端口"));
        return;
    }
    doConnect(target);
}

void WirelessController::doConnect(const Target &target)
{
    const QString addr = addrOf(target.ip, target.connectPort);
    emit logMessage(tr("正在连接 %1 …").arg(addr));
    emit logMessage(tr("adb connect %1").arg(addr));
    setState(Connecting, tr("正在连接 %1 …").arg(addr));

    m_adb->executeWithTimeout(QString(), QStringList() << "connect" << addr, kConnectTimeoutMs,
                              [this, target, addr](bool, bool timedOut, const QString &out) {
                                  if (timedOut) {
                                      failWith(connectErrorText(out, true));
                                      return;
                                  }
                                  // adb connect 失败时退出码仍为 0，只能按文本判定
                                  if (!isConnectSuccess(out)) {
                                      failWith(connectErrorText(out, false));
                                      return;
                                  }
                                  emit logMessage(tr("连接响应：%1").arg(out.trimmed().left(120)));

                                  // 二次确认：设备必须真正出现在 adb devices 中
                                  m_adb->executeWithTimeout(QString(), QStringList() << "devices", kDevicesTimeoutMs,
                                                            [this, target, addr](bool, bool, const QString &dout) {
                                                                if (!dout.contains(addr)) {
                                                                    failWith(tr("已连接但设备尚未就绪：请稍后在设备列表刷新重试"));
                                                                    return;
                                                                }
                                                                // 配对是一次性的：历史里只保留 ip + 连接端口，
                                                                // 不再带出配对端口（每次重开无线调试都会变）
                                                                Config::getInstance().saveWirelessTarget(
                                                                    target.ip, target.connectPort, 0);
                                                                setState(Connected, tr("已连接 %1").arg(addr));
                                                                emit connected(addr);
                                                            });
                              });
}

void WirelessController::disconnectTarget(const Target &target)
{
    if (isBusy()) {
        return;
    }
    if (!isValidHost(target.ip)) {
        failWith(tr("设备 IP 不合法"));
        return;
    }
    // 有端口就按 ip:port 断开，避免把同 IP 的其它端口一起断掉
    const QString addr = target.connectPort > 0 ? addrOf(target.ip, target.connectPort) : target.ip.trimmed();
    emit logMessage(tr("adb disconnect %1").arg(addr));

    m_adb->executeWithTimeout(QString(), QStringList() << "disconnect" << addr, kDisconnectTimeoutMs,
                              [this, addr](bool, bool timedOut, const QString &out) {
                                  if (timedOut) {
                                      failWith(tr("断开超时：请确认设备是否已离线"));
                                      return;
                                  }
                                  const QString lower = out.toLower();
                                  // 设备已离线/本来就没连接，按成功处理
                                  const bool ok = lower.contains("disconnected") || lower.contains("no such device")
                                                  || lower.contains("not found") || out.trimmed().isEmpty();
                                  if (!ok) {
                                      failWith(out.trimmed().left(200));
                                      return;
                                  }
                                  setState(Idle, tr("已断开 %1").arg(addr));
                                  emit logMessage(tr("已断开 %1").arg(addr));
                              });
}

void WirelessController::discover()
{
    if (isBusy()) {
        return;
    }
    setState(Discovering, tr("正在搜索局域网内的无线调试设备…"));
    emit logMessage(tr("adb mdns services"));

    m_adb->executeWithTimeout(QString(), QStringList() << "mdns" << "services", kDiscoverTimeoutMs,
                              [this](bool, bool timedOut, const QString &out) {
                                  if (timedOut) {
                                      failWith(tr("搜索超时：请确认手机与电脑在同一局域网"));
                                      return;
                                  }
                                  const QVector<DiscoveredDevice> devices = parseMdnsOutput(out);
                                  if (devices.isEmpty()) {
                                      setState(Idle, tr("未发现设备：请确认手机与电脑同一局域网，且「无线调试」界面已打开"));
                                  } else {
                                      setState(Idle, tr("发现 %1 台设备").arg(devices.size()));
                                  }
                                  emit discovered(devices);
                              });
}
