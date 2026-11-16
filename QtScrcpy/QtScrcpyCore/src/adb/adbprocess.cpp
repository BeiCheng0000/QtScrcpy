#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>

#include "adbprocess.h"
#include "adbprocessimpl.h"

QString g_adbPath;

namespace qsc {

AdbProcess::AdbProcess(QObject *parent)
    : QObject(parent)
    , m_adbImpl(new AdbProcessImpl())
{
    connect(m_adbImpl, &AdbProcessImpl::adbProcessImplResult, this, &qsc::AdbProcess::adbProcessResult);
}

AdbProcess::~AdbProcess()
{
    if (m_adbImpl->isRuning()) {
        m_adbImpl->kill();
    }
    delete m_adbImpl;
}

void AdbProcess::setAdbPath(const QString &adbPath)
{
    g_adbPath = adbPath;
    // 清除缓存，使新设置的路径立即生效（否则进程内会一直沿用首次解析的结果）
    AdbProcessImpl::resetAdbPath();
}

void AdbProcess::execute(const QString &serial, const QStringList &args)
{
    m_adbImpl->execute(serial, args);
}

void AdbProcess::executeWithTimeout(const QString &serial, const QStringList &args, int timeoutMs,
                                    const std::function<void(bool ok, bool timedOut, const QString &out)> &done)
{
    // 独立 QProcess：无线配对/连接这类"可能卡 20 秒以上"的命令不能挤占主页面的
    // 串行通道，否则整个界面（含投屏）都在等它；超时由自身定时器兜底。
    auto *proc = new QProcess(this);
    proc->setProgram(AdbProcessImpl::getAdbPath());

    QStringList adbArgs;
    if (!serial.isEmpty()) {
        adbArgs << "-s" << serial;
    }
    adbArgs << args;
    proc->setArguments(adbArgs);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    qDebug() << QString("adb(timeout %1ms): %2 %3").arg(timeoutMs).arg(proc->program(), adbArgs.join(" "));

    auto timedOut = std::make_shared<bool>(false);
    auto delivered = std::make_shared<bool>(false);

    auto *timer = new QTimer(proc);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, proc, [proc, timedOut]() {
        *timedOut = true;
        proc->kill();
    });

    QObject::connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), proc,
                     [proc, timer, timedOut, delivered, done](int exitCode, QProcess::ExitStatus) {
                         if (*delivered) {
                             return;
                         }
                         *delivered = true;
                         timer->stop();
                         const QString out = QString::fromUtf8(proc->readAll()).trimmed();
                         if (done) {
                             done(0 == exitCode, *timedOut, out);
                         }
                         proc->deleteLater();
                     });

    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [proc, timer, delivered, done](QProcess::ProcessError error) {
                         if (*delivered) {
                             return;
                         }
                         if (QProcess::FailedToStart == error) {
                             *delivered = true;
                             timer->stop();
                             if (done) {
                                 done(false, false, QStringLiteral("failed to start adb"));
                             }
                             proc->deleteLater();
                         }
                     });

    timer->start(qMax(1000, timeoutMs));
    proc->start();
}

bool AdbProcess::isRuning()
{
    return m_adbImpl->isRuning();
}

void AdbProcess::setShowTouchesEnabled(const QString &serial, bool enabled)
{
    m_adbImpl->setShowTouchesEnabled(serial, enabled);
}

void AdbProcess::kill()
{
    m_adbImpl->kill();
}

QStringList AdbProcess::arguments()
{
    return m_adbImpl->arguments();
}

QStringList AdbProcess::getDevicesSerialFromStdOut()
{
    return m_adbImpl->getDevicesSerialFromStdOut();
}

QString AdbProcess::getDeviceIPFromStdOut()
{
    return m_adbImpl->getDeviceIPFromStdOut();
}

QString AdbProcess::getDeviceIPByIpFromStdOut()
{
    return m_adbImpl->getDeviceIPByIpFromStdOut();
}

QString AdbProcess::getStdOut()
{
    return m_adbImpl->getStdOut();
}

QString AdbProcess::getErrorOut()
{
    return m_adbImpl->getErrorOut();
}

void AdbProcess::forward(const QString &serial, quint16 localPort, const QString &deviceSocketName)
{
    m_adbImpl->forward(serial, localPort, deviceSocketName);
}

void AdbProcess::forwardRemove(const QString &serial, quint16 localPort)
{
    m_adbImpl->forwardRemove(serial, localPort);
}

void AdbProcess::reverse(const QString &serial, const QString &deviceSocketName, quint16 localPort)
{
    m_adbImpl->reverse(serial, deviceSocketName, localPort);
}

void AdbProcess::reverseRemove(const QString &serial, const QString &deviceSocketName)
{
    m_adbImpl->reverseRemove(serial, deviceSocketName);
}

void AdbProcess::push(const QString &serial, const QString &local, const QString &remote)
{
    m_adbImpl->push(serial, local, remote);
}

void AdbProcess::install(const QString &serial, const QString &local)
{
    m_adbImpl->install(serial, local);
}

void AdbProcess::removePath(const QString &serial, const QString &path)
{
    m_adbImpl->removePath(serial, path);
}

}
