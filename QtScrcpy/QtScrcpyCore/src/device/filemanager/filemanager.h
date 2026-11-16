#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QTimer>
#include <QMetaType>

#include <functional>

#include "adbprocess.h"

namespace qsc {

/// 设备侧一个目录条目
struct DeviceFileEntry
{
    QString name;
    QString path;    // 绝对路径
    bool isDir = false;
    bool isLink = false;
    qint64 size = 0;  // 目录为 0
    qint64 mtime = 0; // epoch 秒
};

/// 单设备文件操作通道：目录枚举、上传、下载、文本读取/回写。
/// 所有 adb 调用在同一线程内串行执行，结果通过信号返回给 UI。
class FileManager : public QObject
{
    Q_OBJECT
public:
    explicit FileManager(const QString &serial, QObject *parent = nullptr);
    ~FileManager() override;

    const QString &serial() const { return m_serial; }

    /// 浏览目录
    void listDir(const QString &path);
    /// PC -> 设备。localFiles 与 devicePaths 一一对应（目录由调用方展开，
    /// 需保留结构的场景由调用方拼好目标路径，父目录会自动创建）
    void uploadFiles(const QStringList &localFiles, const QStringList &devicePaths, bool overwrite);
    /// 设备 -> PC
    void downloadFiles(const QStringList &deviceFiles, const QString &localDir, bool overwrite);
    /// 读取文本文件（内容回到 textFileLoaded）
    void readTextFile(const QString &devicePath);
    /// 回写文本文件。expectedMtime 非 0 时做冲突检测；force 跳过检测
    void writeTextFile(const QString &devicePath, const QByteArray &data,
                       const QString &encoding, qint64 expectedMtime, bool force);
    /// 删除设备上的文件/目录（含子内容）。执行前会预检父目录可写性，
    /// 只读时拒绝执行并经 deleteFinished(false) 返回原因
    void deletePaths(const QStringList &devicePaths);
    /// 取消当前及排队中的任务（不阻塞）
    void cancelAll();

    bool isBusy() const { return !m_queue.isEmpty() || nullptr != m_currentAdb; }

signals:
    void dirListed(const QString &path, const QVector<qsc::DeviceFileEntry> &entries);
    void listFailed(const QString &path, const QString &reason);

    void transferStarted(int totalItems);
    void transferProgress(int index, const QString &name, qint64 done, qint64 total);
    void transferItemFinished(int index, const QString &name, bool ok, const QString &error);
    void transferFinished(int succeeded, int failed, bool canceled);

    void textFileLoaded(const QString &path, const QString &content,
                        const QString &encoding, qint64 mtime);
    void textFileLoadFailed(const QString &path, const QString &reason);
    void textFileSaved(const QString &path, bool ok, const QString &error);

    void deleteFinished(const QStringList &paths, bool ok, const QString &error);

private:
    struct Task
    {
        enum Type { List, Upload, Download, ReadText, WriteText, Delete } type = List;
        QString path;         // List/Read/Write 目标
        QString dir;          // Download 目标目录
        QStringList files;    // 传输的文件（上传时是本地源；下载时是设备源）
        QStringList targets;  // 上传时与 files 一一对应的设备目标路径
        QByteArray data;      // WriteText 内容
        QString encoding;
        qint64 expectedMtime = 0;
        bool overwrite = false;
        bool force = false;
    };

    // 运行一次 adb 命令；cb 在命令结束时回调（失败时 out 为空）
    void runAdb(const QStringList &args,
                const std::function<void(bool ok, const QString &out, const QString &err)> &cb);
    // 不占用主任务槽的辅助调用（进度轮询用）
    void runAdbDetached(const QStringList &args,
                        const std::function<void(bool ok, const QString &out)> &cb);

    void enqueue(const Task &task);
    void processNext();
    void executeTask(const Task &task);
    void finishTask();

    void doUpload(const Task &task);
    void doUploadStep(const Task &task, int index, int okCount, int failCount, bool canceled);
    void doDownload(const Task &task);
    void doDownloadStep(const Task &task, int index, int okCount, int failCount, bool canceled);
    void doReadText(const Task &task);
    void doWriteText(const Task &task);
    void doDelete(const Task &task);

    void startUploadPoll(const QString &remotePath, int index, qint64 total);
    void startDownloadPoll(const QString &localPath, int index);
    void stopUploadPoll();

    QString classifyError(const QString &err) const;

private:
    QString m_serial;
    QQueue<Task> m_queue;
    qsc::AdbProcess *m_currentAdb = nullptr;
    QTimer m_execTimer;
    bool m_canceled = false;

    // 上传进度轮询
    QTimer m_pollTimer;
    bool m_pollBusy = false;
    QString m_pollRemote;
    QString m_pollLocalPath;
    qint64 m_pollTotal = 0;
    int m_pollIndex = 0;

    QString m_currentTransferName;
};

} // namespace qsc

Q_DECLARE_METATYPE(qsc::DeviceFileEntry)

#endif // FILEMANAGER_H
