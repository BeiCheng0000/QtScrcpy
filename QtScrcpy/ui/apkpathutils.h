#ifndef APKPATHUTILS_H
#define APKPATHUTILS_H

#include <QString>
#include <QStringList>

// APK 路径收集工具：把用户输入/拖入的文件、目录（含 UNC 网络路径）
// 规范化并展开为 apk 文件列表。仅依赖 QtCore，便于独立验证。
namespace ApkPathUtils
{
// rawPaths 可以是 apk 文件、目录、UNC 路径；目录内 apk 按后缀（忽略大小写）筛选。
// recursive=true 时递归子目录。返回列表已去重；失败的项写入 skipped（每条为“原始路径: 原因”）。
QStringList expand(const QStringList &rawPaths, bool recursive, QStringList *skipped = nullptr);

// 去掉首尾空白与资源管理器复制路径时常见的成对引号，统一 // 与 \\ 前缀。
QString normalize(const QString &raw);

// 传给 adb 前把 '/' 转成平台原生分隔符（Windows UNC 需要反斜杠）。
QString toAdbPath(const QString &path);
} // namespace ApkPathUtils

#endif // APKPATHUTILS_H
