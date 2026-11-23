#include "deviceloggrabber.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>

#include "../util/config.h"

// 逐行解码：优先 UTF-8；出现非法序列（U+FFFD）时回退本地编码（GBK）。
// 解决两类中文乱码：设备侧个别应用以 GBK 写中文日志、统一以 UTF-8 落盘。
static QString decodeLogBytes(const QByteArray &bytes)
{
    const QString utf8 = QString::fromUtf8(bytes);
    if (!utf8.contains(QChar(0xFFFD))) {
        return utf8;
    }
    const QString local = QString::fromLocal8Bit(bytes);
    return local.contains(QChar(0xFFFD)) ? utf8 : local;
}

DeviceLogGrabber::DeviceLogGrabber(QObject *parent)
    : QObject(parent)
{
}

DeviceLogGrabber::~DeviceLogGrabber()
{
    // 析构兜底：窗口关闭/程序退出时保证 logcat 进程结束、文件完整落盘
    if (m_proc) {
        disconnect(m_proc, nullptr, this, nullptr);
        m_proc->kill();
        m_proc->waitForFinished(1000);
        m_proc->deleteLater();
        m_proc.clear();
    }
    finalizeFile();
    if (m_stream) {
        m_stream->flush();
        delete m_stream;
        m_stream = nullptr;
    }
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
}

QString DeviceLogGrabber::resolveAdbOrNotify()
{
    const QString adb = Config::getInstance().getAdbPath();
    if (adb.isEmpty() || !QFileInfo::exists(adb)) {
        emit message(QObject::tr("未找到 adb，请在设置中检查 adb 路径"));
        return QString();
    }
    return adb;
}

QString DeviceLogGrabber::sanitizedSerial(const QString &serial) const
{
    // 无线设备 serial 形如 192.168.1.5:5555，':' 等字符在 Windows 目录名中非法
    static const QRegularExpression invalid(QStringLiteral("[^A-Za-z0-9_.-]"));
    QString out = serial;
    out.replace(invalid, QStringLiteral("_"));
    return out;
}

void DeviceLogGrabber::start(const QString &serial)
{
    if (m_grabbing) {
        emit message(QObject::tr("设备日志抓取已在进行中"));
        return;
    }
    const QString adb = resolveAdbOrNotify();
    if (adb.isEmpty()) {
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/logcat/") + sanitizedSerial(serial);
    if (!QDir().mkpath(dir)) {
        emit message(QObject::tr("无法创建日志目录：%1").arg(dir));
        return;
    }
    const QString path = dir + QStringLiteral("/")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss")) + QStringLiteral(".log");
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        emit message(QObject::tr("无法写入日志文件：%1").arg(path));
        return;
    }

    // UTF-8 + BOM：Windows 记事本等查看器会正确识别编码，中文不再显示为乱码。
    // QTextStream 带缓冲，逐行写入仍是 O(1) 内存、不随日志总量增长。
    if (m_stream) {
        delete m_stream;
    }
    m_stream = new QTextStream(&m_file);
    m_stream->setCodec("UTF-8");
    m_stream->setGenerateByteOrderMark(true);
    m_lineBuffer.clear();
    m_lineCount = 0;

    m_serial = serial;
    m_stoppingByUser = false;
    m_proc = new QProcess(this);
    m_proc->setProgram(adb);
    m_proc->setArguments({ QStringLiteral("-s"), serial, QStringLiteral("logcat"), QStringLiteral("-v"), QStringLiteral("time") });
    m_proc->setProcessChannelMode(QProcess::MergedChannels);

    // 流式写盘：按行切分→解码→原样写入（adb logcat 原生格式），缓冲只留未凑齐的半行
    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_stream) {
            return;
        }
        m_lineBuffer += m_proc->readAllStandardOutput();
        int idx = 0;
        while ((idx = m_lineBuffer.indexOf('\n')) >= 0) {
            QByteArray line = m_lineBuffer.left(idx);
            m_lineBuffer.remove(0, idx + 1);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            writeLine(line);
        }
    });
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        // 保存结果不再走 toast，改由 captureFinished 触发"已保存"弹窗
        // （含打开文件夹定位文件），与录制视频结束的交互一致
        const bool byUser = m_stoppingByUser;
        const QString path = m_file.fileName();
        cleanup(byUser);
        emit captureFinished(path, !byUser);
    });

    m_proc->start();
    // 同步判定启动结果（FailedToStart 通常立即返回）：失败则手动收尾
    if (m_proc->waitForStarted(2000)) {
        m_grabbing = true;
        emit stateChanged(true);
        emit message(QObject::tr("正在抓取设备日志…"));
    } else {
        cleanup(false);
        emit message(QObject::tr("adb logcat 启动失败，请检查 adb 与设备"));
    }
}

void DeviceLogGrabber::stop()
{
    if (!m_grabbing || !m_proc) {
        return;
    }
    m_stoppingByUser = true;
    // adb logcat 客户端被终止对设备端无副作用，直接 kill 最可靠
    m_proc->kill();
    m_proc->waitForFinished(1000); // finished 槽负责统一收尾与提示
}

void DeviceLogGrabber::clear(const QString &serial)
{
    if (m_clearProc) {
        return;
    }
    const QString adb = resolveAdbOrNotify();
    if (adb.isEmpty()) {
        return;
    }
    m_clearProc = new QProcess(this);
    m_clearProc->setProgram(adb);
    m_clearProc->setArguments({ QStringLiteral("-s"), serial, QStringLiteral("logcat"), QStringLiteral("-c") });
    connect(m_clearProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) {
        if (m_clearProc) {
            m_clearProc->deleteLater();
            m_clearProc.clear();
        }
        emit message(exitCode == 0 ? QObject::tr("设备日志已清空")
                                   : QObject::tr("清空设备日志失败（退出码 %1）").arg(exitCode));
    });
    m_clearProc->start();
    // 同步判定启动失败（FinishedToStart 时 finished 不发）：立刻清理并提示
    if (m_clearProc->state() == QProcess::NotRunning && !m_clearProc->waitForStarted(2000)) {
        m_clearProc->deleteLater();
        m_clearProc.clear();
        emit message(QObject::tr("adb logcat 启动失败，请检查 adb 与设备"));
    }
}

void DeviceLogGrabber::writeLine(const QByteArray &raw)
{
    if (!m_stream) {
        return;
    }
    // 仅做编码归一，内容按 logcat 原样输出（字段顺序、格式均不变）
    *m_stream << decodeLogBytes(raw) << "\n";
    // 长时抓取时定期冲刷，用户查看进行中的文件也能看到最新内容
    if (++m_lineCount % 100 == 0) {
        m_stream->flush();
    }
}

void DeviceLogGrabber::finalizeFile()
{
    if (!m_stream) {
        return;
    }
    // 进程结束时可能残留不带换行的最后一行
    if (!m_lineBuffer.isEmpty()) {
        QByteArray rest = m_lineBuffer;
        m_lineBuffer.clear();
        if (rest.endsWith('\r')) {
            rest.chop(1);
        }
        writeLine(rest);
    }
    m_stream->flush();
}

void DeviceLogGrabber::cleanup(bool normal)
{
    Q_UNUSED(normal)
    if (m_proc) {
        disconnect(m_proc, nullptr, this, nullptr);
        m_proc->deleteLater();
        m_proc.clear();
    }
    finalizeFile();
    if (m_stream) {
        m_stream->flush();
        delete m_stream;
        m_stream = nullptr;
    }
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    if (m_grabbing) {
        m_grabbing = false;
        emit stateChanged(false);
    }
}
