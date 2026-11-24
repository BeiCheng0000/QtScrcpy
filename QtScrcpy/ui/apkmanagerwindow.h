#ifndef APKMANAGERWINDOW_H
#define APKMANAGERWINDOW_H

#include <QMap>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QMenu;

namespace qsc
{
    class AdbProcess;
}

/// 单个应用的解析结果（供 APK 管理窗口与连接时的后台预热共享同一磁盘缓存格式）
struct ApkAppInfo
{
    QString label;
    QString version;
    QPixmap icon;
    bool parsed = false; // 是否已完成解析（区分“解析失败”与“尚未解析”）
};

/// 单设备 APK 管理窗口：列出第三方应用（图标/名称/包名/版本号），支持打开/卸载/清缓存/杀进程。
/// 应用名称与图标来自 aapt 解析 apk；未安装 Android SDK 时降级为包名 + 占位图标，版本号走 dumpsys。
class ApkManagerWindow : public QWidget
{
    Q_OBJECT
public:
    explicit ApkManagerWindow(const QString &serial, QWidget *parent = nullptr);
    ~ApkManagerWindow() override;

    // 设备连接时后台预热缓存：异步解析该设备第三方应用并写入磁盘，
    // 使之后打开 APK 管理窗口直接命中缓存、秒开。无 UI、不阻塞连接流程。
    static void warmUpCache(const QString &serial);
    static void clearWarmedCache(const QString &serial); // 设备断开时清除，重连可再预热

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override; // 表格 Ctrl+C 复制

private slots:
    void onRefreshClicked();
    void onTableContextMenu(const QPoint &pos); // 表格右键菜单（复制文本）
    void onPkgListResult(bool ok, bool timedOut, const QString &out);
    void onRefreshVerifyResult(bool ok, bool timedOut, const QString &out); // 刷新后台静默校验回调
    void onOpenClicked();
    void onUninstallClicked();
    void onClearCacheClicked();
    void onForceStopClicked();

private:
    using AppInfo = ApkAppInfo; // 与磁盘缓存/预热 worker 共用同一结构

    void buildUi();
    void retranslateUi();
    void startLoad(bool useCache);
    void applyToRow(const QString &pkg, const AppInfo &info);
    void setStatus(const QString &text);

    // 刷新快路径：内存缓存完整时直接复用，避免重复 adb / pull / aapt
    bool cacheComplete() const;                                  // 所有包都已解析完成
    void verifyPackageListForRefresh();                          // 后台静默校验包集合
    void setupRowsForPackages(const QStringList &pkgs);          // 用已知包名铺行并启动并发解析
    void rebuildFromCache();                                     // 兜底：用缓存瞬时重建表格
    static QStringList parsePackageList(const QString &out);     // 从 pm list 输出提取并去重包名
    static bool samePackageSet(const QStringList &a, const QStringList &b);

    // 首次加载：并发解析 + 磁盘缓存（窗口重开/程序重启仍可秒开）
    void loadDiskCache();                                        // 启动时载入磁盘缓存
    void saveDiskCache() const;                                  // 解析完成后整体原子写回
    QString cacheFilePath() const;                               // 按 serial 隔离的缓存文件
    void pumpParseQueue();                                       // 调度最多 N 个并发解析任务
    void startParseOne(const QString &pkg, int generation);      // 单个包：pm path -> pull -> 解析
    void onParseOneFinished();                                   // 单任务完成，继续调度
    AppInfo parseApk(const QString &pkg, const QString &localApk);
    AppInfo fallbackInfo(const QString &pkg) const;

    // 设置单元格文本并同步悬浮提示（超长文本鼠标悬浮显示全文）
    static void setItemText(QTableWidgetItem *item, const QString &text);
    void copyText(const QString &text); // 复制到剪贴板并在状态栏提示

    QWidget *buildActionWidget(const QString &pkg);
    QString currentPkgFromSender() const;

    // 对当前设备执行一次 adb 命令，结果回调 done(ok, timedOut, out)
    void adbAsync(const QStringList &args, int timeoutMs,
                  const std::function<void(bool, bool, const QString &)> &done);

    // 操作按钮
    void openApp(const QString &pkg);
    void uninstallApp(const QString &pkg);
    void clearAppCache(const QString &pkg);
    void forceStopApp(const QString &pkg);

    static QString findAdb();
    QString findAapt(); // 结果缓存于成员，避免每个包重复扫描 SDK 目录
    static bool parseBadging(const QString &text, QString &label, QString &version, QString &iconPath);
    static QPixmap defaultIcon();

    QString m_serial;
    QTableWidget *m_table = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QLabel *m_statusLabel = nullptr;

    QStringList m_pkgQueue;   // 待解析包队列（顺序与表格行一致）
    QString m_localApkDir;    // 本地临时目录
    bool m_loading = false;

    int m_loadGeneration = 0; // 每次重新加载 +1，用于丢弃上一批的并发回调
    int m_activeParses = 0;   // 正在飞行的解析任务数

    // 解析缓存：内存 + 磁盘持久化，避免窗口重开/重启后重复 pull/aapt
    QMap<QString, AppInfo> m_cache;
    QStringList m_cachedPkgs;  // 与表格行一一对应的包名顺序，缓存完整性以它为准
    bool m_verifying = false;  // 后台静默校验进行中，防止重复点击叠加 adb
};

#endif // APKMANAGERWINDOW_H
