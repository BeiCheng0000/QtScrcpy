#ifndef DEVICELOGGRABBER_H
#define DEVICELOGGRABBER_H

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QProcess>

class QTextStream;

// 设备日志抓取器（每个投屏窗口一个实例，随 ToolForm 生命周期销毁）。
// - start(): 打开时间戳命名的本地日志文件，启动独立 adb logcat 进程流式写入
// - stop():  结束进程、flush 落盘，经 finished 统一收尾并提示保存路径
// - clear(): 一次性执行 logcat -c，清空设备端日志缓冲区（与抓取会话互不影响）
// 状态机：Idle ⇄ Grabbing；同设备不允许第二个抓取会话（互斥）；
// 进程异常退出（设备拔线等）自动回退 Idle 并提示文件已保存。
// 日志内容保持 adb logcat 原生输出，不做任何排版加工；
// 仅做编码归一：统一 UTF-8（含 BOM）落盘，兼容设备侧 GBK 中文行。
class DeviceLogGrabber : public QObject
{
    Q_OBJECT

public:
    explicit DeviceLogGrabber(QObject *parent = nullptr);
    ~DeviceLogGrabber();

    bool isGrabbing() const { return m_grabbing; }
    QString currentFilePath() const { return m_file.fileName(); }

public slots:
    void start(const QString &serial);  // 开始抓取（结果经 stateChanged/message 反馈）
    void stop();                        // 停止抓取（用户主动：正常收尾并提示路径）
    void clear(const QString &serial);  // 清空设备端日志缓冲区

signals:
    void stateChanged(bool grabbing);   // 状态变化 → UI 更新按钮文案/红点
    void message(const QString &text);  // 轻量提示（toast 文案：启动/清空/错误）
    void captureFinished(const QString &filePath, bool error); // 抓取会话结束 → 弹窗告知保存位置（error=异常中断）

private:
    QString resolveAdbOrNotify();       // 返回 adb 路径；不可用时发提示并返回空
    QString sanitizedSerial(const QString &serial) const; // serial 中的 ip:port 等字符转为合法目录名
    void writeLine(const QByteArray &raw); // 解码为 UTF-8 文本并写入一行（保持原始内容）
    void finalizeFile();                // 冲刷残留行与缓冲（幂等）
    void cleanup(bool normal = true);   // 进程/文件统一收尾（幂等），回退 Idle 并广播

    QPointer<QProcess> m_proc;          // 长时 logcat 抓取进程
    QPointer<QProcess> m_clearProc;     // 一次性 logcat -c 进程
    QFile m_file;                       // 当前抓取会话的落盘文件
    QTextStream *m_stream = nullptr;    // UTF-8（含 BOM）文本流，带缓冲，保证中文不乱码
    QByteArray m_lineBuffer;            // 仅保留当前未凑齐一行的字节（内存 O(1)）
    int m_lineCount = 0;                // 已写入行数（定期冲刷，进行中的文件可实时查看）
    QString m_serial;
    bool m_grabbing = false;            // 当前是否处于抓取会话中
    bool m_stoppingByUser = false;      // 区分用户主动停止与异常中断（影响提示文案）
};

#endif // DEVICELOGGRABBER_H
