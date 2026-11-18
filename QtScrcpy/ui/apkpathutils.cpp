#include "apkpathutils.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

QString ApkPathUtils::normalize(const QString &raw)
{
    QString s = raw.trimmed();
    if (s.size() >= 2 && ((s.startsWith('"') && s.endsWith('"')) || (s.startsWith('\'') && s.endsWith('\'')))) {
        s = s.mid(1, s.size() - 2).trimmed();
    }
    // 用户可能从资源管理器复制 "\\server\share" 或 "//server/share"
    if (s.startsWith("\\\\") || s.startsWith("//")) {
        s = "\\\\" + s.mid(2);
    }
    return s;
}

QString ApkPathUtils::toAdbPath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

QStringList ApkPathUtils::expand(const QStringList &rawPaths, bool recursive, QStringList *skipped)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &raw : rawPaths) {
        const QString path = normalize(raw);
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        if (!info.exists()) {
            if (skipped) {
                skipped->append(QCoreApplication::translate("ApkInstallDialog", "%1: path does not exist or network share is unreachable").arg(raw.trimmed()));
            }
            continue;
        }
        auto addFile = [&](const QString &file) {
            const QFileInfo fi(file);
            if (!fi.isFile() || fi.suffix().compare(QLatin1String("apk"), Qt::CaseInsensitive) != 0) {
                return;
            }
            const QString key = fi.absoluteFilePath();
            if (!seen.contains(key)) {
                seen.insert(key);
                result.append(key);
            }
        };
        if (info.isFile()) {
            if (info.suffix().compare(QLatin1String("apk"), Qt::CaseInsensitive) == 0) {
                addFile(path);
            } else if (skipped) {
                skipped->append(QCoreApplication::translate("ApkInstallDialog", "%1: not an apk file").arg(raw.trimmed()));
            }
        } else if (info.isDir()) {
            QDirIterator it(path, QStringList() << QStringLiteral("*.apk") << QStringLiteral("*.APK"), QDir::Files,
                            recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
            while (it.hasNext()) {
                addFile(it.next());
            }
        } else if (skipped) {
            skipped->append(QCoreApplication::translate("ApkInstallDialog", "%1: not a file or directory").arg(raw.trimmed()));
        }
    }
    return result;
}
