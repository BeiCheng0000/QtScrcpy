#include "filemanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif

namespace {

/// 单引号包裹并转义，供设备端 sh 使用，避免路径空格/特殊字符破坏命令
QString shellQuote(const QString &s)
{
    QString v = s;
    v.replace("'", "'\\''");
    return "'" + v + "'";
}

/// 二进制判定：前 8KB 出现 NUL 即视为二进制
bool isBinaryData(const QByteArray &data)
{
    const int check = qMin(data.size(), 8192);
    return data.left(check).contains('\0');
}

QString decodeText(const QByteArray &raw, QString *encoding)
{
    if (raw.startsWith("\xEF\xBB\xBF")) {
        *encoding = QStringLiteral("UTF-8");
        return QString::fromUtf8(raw.mid(3));
    }
    const QString utf8 = QString::fromUtf8(raw);
    if (utf8.toUtf8() == raw) {
        *encoding = QStringLiteral("UTF-8");
        return utf8;
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QTextCodec *gbk = QTextCodec::codecForName("GBK");
    if (gbk) {
        *encoding = QStringLiteral("GBK");
        return gbk->toUnicode(raw);
    }
#endif
    *encoding = QStringLiteral("UTF-8");
    return utf8;
}

QByteArray encodeText(const QString &text, const QString &encoding)
{
    if (encoding.compare(QStringLiteral("GBK"), Qt::CaseInsensitive) == 0) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QTextCodec *gbk = QTextCodec::codecForName("GBK");
        if (gbk) {
            return gbk->fromUnicode(text);
        }
#endif
    }
    return text.toUtf8();
}

QString joinPath(const QString &dir, const QString &name)
{
    if (dir.isEmpty()) {
        return name;
    }
    if (dir.endsWith('/')) {
        return dir + name;
    }
    return dir + "/" + name;
}

/// 目录枚举哨兵：用于区分"命令未执行/无权限/不是目录"与"合法的空目录"
const QString kListBegin = QStringLiteral("__QSCLS_BEGIN__");
const QString kListEnd = QStringLiteral("__QSCLS_END__");
const QString kListDenied = QStringLiteral("__QSCLS_DENIED__");
const QString kListNotDir = QStringLiteral("__QSCLS_NOTDIR__");

} // namespace

namespace qsc {

FileManager::FileManager(const QString &serial, QObject *parent)
    : QObject(parent), m_serial(serial)
{
    m_execTimer.setSingleShot(true);
    connect(&m_execTimer, &QTimer::timeout, this, [this]() {
        // 命令超时：杀掉 adb 进程，由结果回调继续流程（避免界面永久卡住）
        if (m_currentAdb) {
            qWarning() << "file manager: adb command timeout for" << m_serial;
            m_currentAdb->kill();
        }
    });

    m_pollTimer.setInterval(600);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() {
        if (m_pollBusy) {
            return;
        }
        if (!m_pollLocalPath.isEmpty()) {
            // 下载：本地文件大小即可反映进度，无需访问设备
            const qint64 done = QFileInfo(m_pollLocalPath).size();
            emit transferProgress(m_pollIndex, m_currentTransferName, done, m_pollTotal);
            return;
        }
        if (m_pollRemote.isEmpty()) {
            return;
        }
        m_pollBusy = true;
        const QString remote = m_pollRemote;
        const int index = m_pollIndex;
        const qint64 total = m_pollTotal;
        const QString name = m_currentTransferName;
        runAdbDetached(QStringList() << "shell" << ("stat -c %s " + shellQuote(remote)), [this, index, name, total](bool ok, const QString &out) {
            m_pollBusy = false;
            if (ok) {
                const qint64 done = out.trimmed().toLongLong();
                emit transferProgress(index, name, done, total);
            }
        });
    });
}

FileManager::~FileManager()
{
    m_pollTimer.stop();
    m_execTimer.stop();
}

void FileManager::runAdb(const QStringList &args,
                         const std::function<void(bool, const QString &, const QString &)> &cb)
{
    qsc::AdbProcess *adb = new qsc::AdbProcess(this);
    m_currentAdb = adb;
    m_execTimer.start(30000);
    connect(adb, &qsc::AdbProcess::adbProcessResult, this,
            [this, adb, cb](qsc::AdbProcess::ADB_EXEC_RESULT result) {
                if (qsc::AdbProcess::AER_SUCCESS_START == result) {
                    return;
                }
                const bool ok = (qsc::AdbProcess::AER_SUCCESS_EXEC == result);
                const QString out = adb->getStdOut();
                const QString err = adb->getErrorOut();
                if (m_currentAdb == adb) {
                    m_currentAdb = nullptr;
                    m_execTimer.stop();
                }
                adb->deleteLater();
                cb(ok, out, err);
            });
    adb->execute(m_serial, args);
}

void FileManager::runAdbDetached(const QStringList &args,
                                 const std::function<void(bool, const QString &)> &cb)
{
    qsc::AdbProcess *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, this,
            [adb, cb](qsc::AdbProcess::ADB_EXEC_RESULT result) {
                if (qsc::AdbProcess::AER_SUCCESS_START == result) {
                    return;
                }
                const bool ok = (qsc::AdbProcess::AER_SUCCESS_EXEC == result);
                const QString out = adb->getStdOut();
                adb->deleteLater();
                cb(ok, out);
            });
    adb->execute(m_serial, args);
}

void FileManager::enqueue(const Task &task)
{
    m_queue.enqueue(task);
    if (!m_currentAdb) {
        processNext();
    }
}

void FileManager::processNext()
{
    if (nullptr != m_currentAdb || m_queue.isEmpty()) {
        return;
    }
    m_canceled = false;
    const Task task = m_queue.dequeue();
    executeTask(task);
}

void FileManager::finishTask()
{
    QTimer::singleShot(0, this, [this]() { processNext(); });
}

void FileManager::executeTask(const Task &task)
{
    switch (task.type) {
    case Task::List: {
        QString base = task.path;
        if (base.isEmpty()) {
            base = QStringLiteral("/sdcard");
        }
        // 枚举协议（不使用命令退出码判定成败）：
        //  - 空目录下 `xargs stat` 会无参执行 stat 并返回非零，目录中只要有个别条目
        //    stat 失败（权限不足/损坏软链）整批也返回非零 —— 旧写法据此把空目录和
        //    这些正常目录全部误判为"无法打开"。这里改用哨兵标记区分真实状态；
        //  - `find -exec ... {} +` 由 find 自行分批，空目录时不执行任何命令，
        //    个别条目失败也不影响其余输出；
        //  - '%n|%F|%s|%Y' 固定 4 字段，解析时从右切 3 个 '|'，
        //    文件名含空格/竖线均不会错位。
        const QString cmd = QStringLiteral("export LC_ALL=C; if cd ") + shellQuote(base) +
                            QStringLiteral(" 2>/dev/null; then if [ ! -r . ]; then echo ") + kListDenied +
                            QStringLiteral("; else echo ") + kListBegin +
                            QStringLiteral("; find . -mindepth 1 -maxdepth 1 -exec stat -c '%n|%F|%s|%Y' {} + 2>/dev/null; echo ") +
                            kListEnd + QStringLiteral("; fi; elif [ -e ") + shellQuote(base) +
                            QStringLiteral(" ]; then echo ") + kListNotDir +
                            QStringLiteral("; else echo ") + kListDenied + QStringLiteral("; fi");
        runAdb(QStringList() << "shell" << cmd, [this, base](bool ok, const QString &out, const QString &err) {
            QVector<DeviceFileEntry> entries;
            const int beginIdx = out.indexOf(kListBegin);
            const int endIdx = out.lastIndexOf(kListEnd);

            if (!ok || beginIdx < 0 || endIdx < beginIdx) {
                // 命令没跑起来，或路径不可用：给出可区分的原因，而不是笼统的"操作失败"
                QString reason;
                if (out.contains(kListNotDir)) {
                    reason = tr("该路径不是文件夹");
                } else if (out.contains(kListDenied)) {
                    reason = tr("无法访问该文件夹：权限不足或路径已不存在");
                } else {
                    reason = classifyError(err);
                }
                emit listFailed(base, reason);
                finishTask();
                return;
            }

            const QString body = out.mid(beginIdx + kListBegin.size(),
                                         endIdx - beginIdx - kListBegin.size());
            const QStringList lines = body.split('\n');
            for (const QString &line : lines) {
                if (line.trimmed().isEmpty()) {
                    continue;
                }
                const int p3 = line.lastIndexOf('|');
                if (p3 <= 0) {
                    continue;
                }
                const int p2 = line.lastIndexOf('|', p3 - 1);
                if (p2 <= 0) {
                    continue;
                }
                const int p1 = line.lastIndexOf('|', p2 - 1);
                if (p1 < 0) {
                    continue;
                }
                QString name = line.left(p1);
                if (name.startsWith("./")) {
                    name = name.mid(2);
                }
                if (name.isEmpty() || name == "." || name == "..") {
                    continue;
                }
                DeviceFileEntry e;
                e.name = name;
                e.path = joinPath(base, name);
                const QString type = line.mid(p1 + 1, p2 - p1 - 1);
                e.isDir = (type == "directory");
                e.isLink = (type == "symbolic link");
                e.size = line.mid(p2 + 1, p3 - p2 - 1).toLongLong();
                e.mtime = line.mid(p3 + 1).trimmed().toLongLong();
                entries.append(e);
            }
            emit dirListed(base, entries);
            finishTask();
        });
        break;
    }
    case Task::Upload:
        emit transferStarted(task.files.size());
        emit transferProgress(0, QString(), 0, 0);
        doUpload(task);
        break;
    case Task::Download:
        emit transferStarted(task.files.size());
        emit transferProgress(0, QString(), 0, 0);
        doDownload(task);
        break;
    case Task::ReadText:
        doReadText(task);
        break;
    case Task::WriteText:
        doWriteText(task);
        break;
    case Task::Delete:
        doDelete(task);
        break;
    }
}

void FileManager::listDir(const QString &path)
{
    Task t;
    t.type = Task::List;
    t.path = path;
    enqueue(t);
}

void FileManager::uploadFiles(const QStringList &localFiles, const QStringList &devicePaths, bool overwrite)
{
    if (localFiles.isEmpty() || localFiles.size() != devicePaths.size()) {
        return;
    }
    Task t;
    t.type = Task::Upload;
    t.files = localFiles;
    t.targets = devicePaths;
    t.overwrite = overwrite;
    enqueue(t);
}

void FileManager::downloadFiles(const QStringList &deviceFiles, const QString &localDir, bool overwrite)
{
    if (deviceFiles.isEmpty()) {
        return;
    }
    Task t;
    t.type = Task::Download;
    t.files = deviceFiles;
    t.dir = localDir;
    t.overwrite = overwrite;
    enqueue(t);
}

void FileManager::cancelAll()
{
    m_canceled = true;
    m_queue.clear();
    stopUploadPoll();
    if (m_currentAdb) {
        m_currentAdb->kill();
    }
}

void FileManager::doUpload(const Task &task)
{
    doUploadStep(task, 0, 0, 0, false);
}

void FileManager::doUploadStep(const Task &task, int index, int okCount, int failCount, bool canceled)
{
    Q_UNUSED(canceled)
    if (m_canceled) {
        emit transferFinished(okCount, failCount, true);
        finishTask();
        return;
    }
    if (index >= task.files.size()) {
        emit transferFinished(okCount, failCount, false);
        finishTask();
        return;
    }
    const QString local = task.files.at(index);
    const QFileInfo info(local);
    const QString name = info.fileName();
    const QString remote = task.targets.at(index);
    m_currentTransferName = name;
    emit transferProgress(index, name, 0, info.size());

    // 先确保目标父目录存在（拖拽文件夹上传会保留相对结构）
    const int slash = remote.lastIndexOf('/');
    const QString parent = slash > 0 ? remote.left(slash) : QString();
    const QString mkdirCmd = parent.isEmpty() ? QString()
                                             : ("mkdir -p " + shellQuote(parent));

    auto doPush = [this, task, index, local, remote, name, okCount, failCount, info]() {
        startUploadPoll(remote, index, info.size());
        runAdb(QStringList() << "push" << local << remote,
               [this, task, index, name, okCount, failCount](bool ok, const QString &out, const QString &err) {
                   Q_UNUSED(out)
                   stopUploadPoll();
                   if (ok) {
                       emit transferItemFinished(index, name, true, QString());
                   } else {
                       emit transferItemFinished(index, name, false, classifyError(err));
                   }
                   doUploadStep(task, index + 1, okCount + (ok ? 1 : 0), failCount + (ok ? 0 : 1), false);
               });
    };

    if (mkdirCmd.isEmpty()) {
        doPush();
        return;
    }
    runAdb(QStringList() << "shell" << mkdirCmd, [doPush](bool ok, const QString &out, const QString &err) {
        Q_UNUSED(out)
        Q_UNUSED(err)
        Q_UNUSED(ok) // 目录已存在/权限问题都交给后续 push 报具体错误
        doPush();
    });
}

void FileManager::doDownload(const Task &task)
{
    doDownloadStep(task, 0, 0, 0, false);
}

void FileManager::doDownloadStep(const Task &task, int index, int okCount, int failCount, bool canceled)
{
    Q_UNUSED(canceled)
    if (m_canceled) {
        emit transferFinished(okCount, failCount, true);
        finishTask();
        return;
    }
    if (index >= task.files.size()) {
        emit transferFinished(okCount, failCount, false);
        finishTask();
        return;
    }
    const QString remote = task.files.at(index);
    const QString name = QFileInfo(remote).fileName();
    QString local = joinPath(task.dir, name);
    if (!task.overwrite && QFileInfo::exists(local)) {
        // 自动改名，避免静默覆盖本地文件
        const QFileInfo fi(local);
        const QString base = fi.completeBaseName();
        const QString suffix = fi.suffix();
        int seq = 1;
        QString candidate;
        do {
            candidate = joinPath(task.dir, base + QString(" (%1)").arg(seq) + (suffix.isEmpty() ? QString() : "." + suffix));
            ++seq;
        } while (QFileInfo::exists(candidate) && seq < 1000);
        local = candidate;
    }
    m_currentTransferName = name;
    emit transferProgress(index, name, 0, 0);
    startDownloadPoll(local, index);

    runAdb(QStringList() << "pull" << remote << local,
           [this, task, index, name, local, okCount, failCount](bool ok, const QString &out, const QString &err) {
               Q_UNUSED(out)
               stopUploadPoll();
               if (ok) {
                   emit transferProgress(index, name, QFileInfo(local).size(), QFileInfo(local).size());
                   emit transferItemFinished(index, name, true, QString());
               } else {
                   emit transferItemFinished(index, name, false, classifyError(err));
               }
               doDownloadStep(task, index + 1, okCount + (ok ? 1 : 0), failCount + (ok ? 0 : 1), false);
           });
}

void FileManager::startUploadPoll(const QString &remotePath, int index, qint64 total)
{
    m_pollLocalPath.clear();
    m_pollRemote = remotePath;
    m_pollTotal = total;
    m_pollIndex = index;
    if (total > 512 * 1024) { // 小文件轮询无意义
        m_pollTimer.start();
    }
}

void FileManager::startDownloadPoll(const QString &localPath, int index)
{
    m_pollRemote.clear();
    m_pollLocalPath = localPath;
    m_pollTotal = 0;
    m_pollIndex = index;
    m_pollTimer.start();
}

void FileManager::stopUploadPoll()
{
    m_pollTimer.stop();
    m_pollRemote.clear();
    m_pollLocalPath.clear();
    m_pollBusy = false;
}

void FileManager::doReadText(const Task &task)
{
    const QString path = task.path;
    // 先取 mtime 用于后续冲突检测，再拉取内容
    runAdb(QStringList() << "shell" << ("stat -c %Y " + shellQuote(path)),
           [this, path](bool ok, const QString &out, const QString &err) {
               Q_UNUSED(err)
               const qint64 mtime = ok ? out.trimmed().toLongLong() : 0;
               const QString tmp = QDir::tempPath() + "/qtscrcpy_edit_" +
                                   QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tmp";
               runAdb(QStringList() << "pull" << path << tmp,
                      [this, path, tmp, mtime](bool pullOk, const QString &out, const QString &pullErr) {
                          Q_UNUSED(out)
                          QFile f(tmp);
                          if (!pullOk || !f.open(QIODevice::ReadOnly)) {
                              QFile::remove(tmp);
                              emit textFileLoadFailed(path, pullOk ? tr("读取临时文件失败") : classifyError(pullErr));
                              finishTask();
                              return;
                          }
                          const QByteArray raw = f.readAll();
                          f.close();
                          QFile::remove(tmp);
                          if (isBinaryData(raw)) {
                              emit textFileLoadFailed(path, tr("该文件是二进制文件，不支持编辑"));
                              finishTask();
                              return;
                          }
                          QString encoding;
                          const QString text = decodeText(raw, &encoding);
                          emit textFileLoaded(path, text, encoding, mtime);
                          finishTask();
                      });
           });
}

void FileManager::doWriteText(const Task &task)
{
    const QString path = task.path;
    auto pushContent = [this, task, path](bool force) {
        const QString tmp = QDir::tempPath() + "/qtscrcpy_save_" +
                            QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tmp";
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit textFileSaved(path, false, tr("无法写入本地临时文件"));
            finishTask();
            return;
        }
        f.write(encodeText(QString::fromUtf8(task.data), task.encoding));
        f.close();

        const QString remoteTmp = path + ".qtscrcpy.tmp";
        runAdb(QStringList() << "push" << tmp << remoteTmp,
               [this, path, tmp, remoteTmp, force](bool ok, const QString &out, const QString &err) {
                   Q_UNUSED(out)
                   if (!ok) {
                       QFile::remove(tmp);
                       emit textFileSaved(path, false, classifyError(err));
                       finishTask();
                       return;
                   }
                   // 先写临时名前再原子 mv，避免写一半中断破坏原文件
                   const QString cmd = "mv -f " + shellQuote(remoteTmp) + " " + shellQuote(path);
                   runAdb(QStringList() << "shell" << cmd,
                          [this, path, tmp, force](bool mvOk, const QString &out2, const QString &err2) {
                              Q_UNUSED(out2)
                              QFile::remove(tmp);
                              Q_UNUSED(force)
                              emit textFileSaved(path, mvOk, mvOk ? QString() : classifyError(err2));
                              finishTask();
                          });
               });
    };

    if (task.force || task.expectedMtime <= 0) {
        pushContent(true);
        return;
    }
    // 冲突检测：设备端 mtime 已变化说明内容被其他端修改过
    runAdb(QStringList() << "shell" << ("stat -c %Y " + shellQuote(path)),
           [this, path, task, pushContent](bool ok, const QString &out, const QString &err) {
               Q_UNUSED(err)
               if (ok) {
                   const qint64 now = out.trimmed().toLongLong();
                   if (now > 0 && now != task.expectedMtime) {
                       emit textFileSaved(path, false, QStringLiteral("conflict"));
                       finishTask();
                       return;
                   }
               }
               pushContent(false);
           });
}

void FileManager::readTextFile(const QString &devicePath)
{
    Task t;
    t.type = Task::ReadText;
    t.path = devicePath;
    enqueue(t);
}

void FileManager::writeTextFile(const QString &devicePath, const QByteArray &data,
                                const QString &encoding, qint64 expectedMtime, bool force)
{
    Task t;
    t.type = Task::WriteText;
    t.path = devicePath;
    t.data = data;
    t.encoding = encoding;
    t.expectedMtime = expectedMtime;
    t.force = force;
    enqueue(t);
}

void FileManager::deletePaths(const QStringList &devicePaths)
{
    QStringList paths;
    for (const QString &p : devicePaths) {
        // 顶层保护：拒绝删除根目录与空路径
        if (p.isEmpty() || p == QLatin1String("/")) {
            continue;
        }
        paths << p;
    }
    if (paths.isEmpty()) {
        return;
    }
    Task t;
    t.type = Task::Delete;
    t.files = paths;
    enqueue(t);
}

void FileManager::doDelete(const Task &task)
{
    const QStringList &paths = task.files;

    // 预检父目录可写性（能否 unlink 由父目录的写权限决定，文件自身只读也能删）。
    // 只读时输出 RO 标记并直接退出 shell，保证 rm 根本不会执行
    QSet<QString> parents;
    for (const QString &p : paths) {
        const int slash = p.lastIndexOf('/');
        if (slash > 0) {
            parents.insert(p.left(slash));
        }
    }
    QString cmd = QStringLiteral("export LC_ALL=C; ");
    for (const QString &d : parents) {
        cmd += QStringLiteral("test -w ") + shellQuote(d) + QStringLiteral(" || { echo __QSCL_RO__; exit 0; }; ");
    }
    cmd += QStringLiteral("rm -rf");
    for (const QString &p : paths) {
        cmd += QLatin1Char(' ') + shellQuote(p);
    }
    // rm 的错误输出直接并到 stdout；末尾用退出码哨兵区分成败
    cmd += QStringLiteral(" 2>&1; echo __QSCL_DEL__ $?");

    runAdb(QStringList() << "shell" << cmd, [this, paths](bool ok, const QString &out, const QString &err) {
        QString error;
        if (!ok) {
            error = classifyError(err);
        } else if (out.contains("__QSCL_RO__")) {
            error = tr("权限不足：条目所在目录为只读，无法删除");
        } else {
            const QString delMark = QStringLiteral("__QSCL_DEL__");
            const int delIdx = out.lastIndexOf(delMark);
            const int code = delIdx >= 0
                                 ? out.mid(delIdx + delMark.size()).trimmed().toInt()
                                 : 1;
            if (code != 0) {
                // rm 的报错行位于哨兵之前
                const QString rmErr = delIdx >= 0 ? out.left(delIdx).trimmed() : out.trimmed();
                error = rmErr.isEmpty() ? tr("删除失败，设备返回未知错误") : classifyError(rmErr);
            }
        }
        emit deleteFinished(paths, error.isEmpty(), error);
        finishTask();
    });
}

QString FileManager::classifyError(const QString &err) const
{
    const QString lower = err.toLower();
    if (lower.contains("no such file") || lower.contains("does not exist")) {
        return tr("文件或目录不存在");
    }
    if (lower.contains("permission denied") || lower.contains("permission")) {
        return tr("权限不足，无法访问");
    }
    if (lower.contains("not found") || lower.contains("offline") || lower.contains("no devices")) {
        return tr("设备未连接");
    }
    if (lower.contains("no space left")) {
        return tr("设备存储空间不足");
    }
    if (lower.contains("read-only")) {
        return tr("目标位置只读");
    }
    if (lower.contains("timeout") || lower.contains("timed out")) {
        return tr("操作超时，请重试");
    }
    if (err.trimmed().isEmpty()) {
        return tr("操作失败");
    }
    return err.trimmed().left(200);
}

} // namespace qsc
