#include "apkmanagerwindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "adbprocess.h"
#include "config.h"

namespace
{
    constexpr int COL_NAME = 0;
    constexpr int COL_PKG = 1;
    constexpr int COL_VERSION = 2;
    constexpr int COL_ACTIONS = 3;
    constexpr int ROLE_PKG = Qt::UserRole + 1;

    // 首次加载并发度：太高会争抢 adb/pull 带宽，4 是实测折中值
    constexpr int MAX_CONCURRENT_PARSES = 4;
    constexpr quint32 APK_CACHE_MAGIC = 0x41504B43; // "APKC"
    constexpr quint32 APK_CACHE_VERSION = 1;

    // ---- 磁盘缓存与解析的共享工具：APK 管理窗口与连接时后台预热共用，保证缓存格式一致 ----

    QString apkCacheFilePath(const QString &serial)
    {
        const QString safe = QString(serial).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                                                     QStringLiteral("_"));
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + QStringLiteral("/apkmanager_cache/") + safe + QStringLiteral(".bin");
    }

    void readDiskCache(const QString &serial, QMap<QString, ApkAppInfo> &out)
    {
        QFile file(apkCacheFilePath(serial));
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }
        QDataStream in(&file);
        in.setVersion(QDataStream::Qt_5_15);
        quint32 magic = 0, version = 0;
        in >> magic >> version;
        if (in.status() != QDataStream::Ok || magic != APK_CACHE_MAGIC || version != APK_CACHE_VERSION) {
            return;
        }
        quint32 count = 0;
        in >> count;
        for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
            QString pkg, label, ver;
            QImage image;
            in >> pkg >> label >> ver >> image;
            if (pkg.isEmpty()) {
                continue;
            }
            ApkAppInfo info;
            info.label = label;
            info.version = ver;
            if (!image.isNull()) {
                info.icon = QPixmap::fromImage(image);
            }
            info.parsed = true;
            out.insert(pkg, info);
        }
    }

    void writeDiskCache(const QString &serial, const QMap<QString, ApkAppInfo> &cache)
    {
        const QString path = apkCacheFilePath(serial);
        QDir().mkpath(QFileInfo(path).absolutePath());
        const QString tmp = path + QStringLiteral(".tmp");
        QFile file(tmp);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }
        QDataStream out(&file);
        out.setVersion(QDataStream::Qt_5_15);
        out << APK_CACHE_MAGIC << APK_CACHE_VERSION;
        quint32 count = 0;
        for (auto it = cache.constBegin(); it != cache.constEnd(); ++it) {
            if (it.value().parsed) {
                ++count;
            }
        }
        out << count;
        for (auto it = cache.constBegin(); it != cache.constEnd(); ++it) {
            const ApkAppInfo &info = it.value();
            if (!info.parsed) {
                continue;
            }
            out << it.key() << info.label << info.version << info.icon.toImage();
        }
        file.close();
        QFile::remove(path);
        QFile::rename(tmp, path);
    }

    QPixmap apkDefaultIcon()
    {
        static const QPixmap pm = []() {
            QPixmap p(36, 36);
            p.fill(QColor(90, 90, 90));
            return p;
        }();
        return pm;
    }

    bool apkParseBadging(const QString &text, QString &label, QString &version, QString &iconPath)
    {
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        static const QRegularExpression versionRe(QStringLiteral("versionName='([^']*)'"));
        static const QRegularExpression labelRe(QStringLiteral("application-label:'([^']*)'"));
        static const QRegularExpression appRe(QStringLiteral("application: label='([^']*)' icon='([^']*)'"));
        static const QRegularExpression iconRe(QStringLiteral("application-icon-\\d+:'([^']*)'"));

        bool foundPackage = false;
        for (const QString &line : lines) {
            if (line.startsWith(QStringLiteral("package:"))) {
                foundPackage = true;
                auto m = versionRe.match(line);
                if (m.hasMatch()) version = m.captured(1);
            }
            auto lm = labelRe.match(line);
            if (lm.hasMatch()) label = lm.captured(1);
            auto am = appRe.match(line);
            if (am.hasMatch()) {
                if (label.isEmpty()) label = am.captured(1);
                iconPath = am.captured(2);
            }
            if (iconPath.isEmpty()) {
                auto im = iconRe.match(line);
                if (im.hasMatch()) iconPath = im.captured(1);
            }
        }
        return foundPackage;
    }

    QString apkFindAapt()
    {
        static QString cached;
        static bool checked = false;
        if (checked) {
            return cached;
        }
        checked = true;
        QString aapt = QStandardPaths::findExecutable(QStringLiteral("aapt"));
        if (!aapt.isEmpty()) {
            cached = aapt;
            return cached;
        }
        QStringList roots;
        const QString local = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                              + QStringLiteral("/AppData/Local/Android/Sdk");
        roots << local;
        QByteArray env = qgetenv("ANDROID_HOME");
        if (!env.isEmpty()) roots << QString::fromLocal8Bit(env);
        env = qgetenv("ANDROID_SDK_ROOT");
        if (!env.isEmpty()) roots << QString::fromLocal8Bit(env);
        for (const QString &root : roots) {
            QDir buildTools(root + QStringLiteral("/build-tools"));
            if (!buildTools.exists()) continue;
            QStringList versions = buildTools.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (int i = versions.size() - 1; i >= 0; --i) {
                const QString candidate = buildTools.filePath(versions.at(i) + QStringLiteral("/aapt.exe"));
                if (QFileInfo::exists(candidate)) {
                    cached = candidate;
                    return cached;
                }
            }
        }
        return cached;
    }

    ApkAppInfo apkFallbackInfo(const QString &pkg)
    {
        ApkAppInfo info;
        info.label = pkg;
        info.icon = apkDefaultIcon();
        info.parsed = true;
        return info;
    }

    // 解析单个已拉取的 apk：aapt dump badging + tar 解图标。窗口与预热 worker 共用。
    ApkAppInfo apkParseFile(const QString &pkg, const QString &localApk, const QString &workDir)
    {
        ApkAppInfo info = apkFallbackInfo(pkg);
        const QString aapt = apkFindAapt();
        if (aapt.isEmpty()) {
            return info;
        }
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(aapt, QStringList() << QStringLiteral("dump") << QStringLiteral("badging") << localApk);
        if (proc.waitForFinished(10000)) {
            const QString text = QString::fromUtf8(proc.readAllStandardOutput());
            QString label, version, iconPath;
            if (apkParseBadging(text, label, version, iconPath)) {
                if (!label.isEmpty()) info.label = label;
                if (!version.isEmpty()) info.version = version;
                if (!iconPath.isEmpty()) {
                    QProcess tar;
                    tar.setWorkingDirectory(workDir);
                    tar.start(QStringLiteral("tar"), QStringList() << QStringLiteral("-xf") << localApk << iconPath);
                    if (tar.waitForFinished(8000)) {
                        const QString iconFile = QDir(workDir).filePath(iconPath);
                        QPixmap pm(iconFile);
                        if (!pm.isNull()) info.icon = pm;
                        QFile::remove(iconFile);
                    }
                }
            }
        }
        return info;
    }

    QStringList apkParsePkgList(const QString &out)
    {
        QStringList pkgs;
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QStringLiteral("package:"))) {
                const QString pkg = line.mid(QStringLiteral("package:").size()).trimmed();
                if (!pkg.isEmpty()) pkgs.append(pkg);
            }
        }
        pkgs.removeDuplicates();
        return pkgs;
    }

    // 连接设备时的后台预热 worker：无 UI、无 Q_OBJECT（用 std::function 回调），
    // 复用上面的共享工具完成 list -> pm path -> pull -> aapt 解析 -> 写磁盘缓存。
    // 自管理生命周期：全部完成后 deleteLater()；进程内同一 serial 只跑一次。
    class ApkCacheWarmer : public QObject
    {
    public:
        ApkCacheWarmer(const QString &serial, QObject *parent = nullptr)
            : QObject(parent), m_serial(serial)
        {
            m_workDir = QDir::tempPath() + QStringLiteral("/qtscrcpy_apkwarm_")
                        + QString::number(QCoreApplication::applicationPid()) + QStringLiteral("_")
                        + QString(serial).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                                                  QStringLiteral("_"));
            QDir().mkpath(m_workDir);
        }

        ~ApkCacheWarmer() override
        {
            QDir dir(m_workDir);
            if (dir.exists()) {
                dir.removeRecursively();
            }
        }

        void start()
        {
            adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
                     << QStringLiteral("list") << QStringLiteral("packages") << QStringLiteral("-3"),
                     15000,
                     [this](bool ok, bool timedOut, const QString &out) {
                         if (!ok || timedOut) {
                             finish();
                             return;
                         }
                         m_queue = apkParsePkgList(out);
                         if (m_queue.isEmpty()) {
                             finish();
                             return;
                         }
                         // 已缓存且已解析的包直接跳过，只补新增/未解析的
                         QMap<QString, ApkAppInfo> existing;
                         readDiskCache(m_serial, existing);
                         QStringList todo;
                         for (const QString &pkg : m_queue) {
                             if (existing.contains(pkg) && existing.value(pkg).parsed) {
                                 m_cache.insert(pkg, existing.value(pkg));
                             } else {
                                 todo.append(pkg);
                             }
                         }
                         if (todo.isEmpty()) {
                             finish();
                             return;
                         }
                         m_queue = todo;
                         while (m_active < MAX_CONCURRENT_PARSES && !m_queue.isEmpty()) {
                             parseOne(m_queue.takeFirst());
                         }
                     });
        }

    private:
        void adbAsync(const QStringList &args, int timeoutMs,
                      const std::function<void(bool, bool, const QString &)> &done)
        {
            auto *adb = new qsc::AdbProcess(this);
            adb->executeWithTimeout(m_serial, args, timeoutMs,
                                    [adb, done](bool ok, bool timedOut, const QString &out) {
                                        if (done) done(ok, timedOut, out);
                                        adb->deleteLater();
                                    });
        }

        void parseOne(const QString &pkg)
        {
            ++m_active;
            adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
                     << QStringLiteral("path") << pkg,
                     15000,
                     [this, pkg](bool ok, bool timedOut, const QString &out) {
                         QString devicePath;
                         const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
                         for (const QString &raw : lines) {
                             const QString line = raw.trimmed();
                             if (line.startsWith(QStringLiteral("package:"))) {
                                 devicePath = line.mid(QStringLiteral("package:").size()).trimmed();
                                 break;
                             }
                         }
                         if (!ok || timedOut || devicePath.isEmpty()) {
                             m_cache.insert(pkg, apkFallbackInfo(pkg));
                             oneFinished();
                             return;
                         }
                         const QString safe = QString(pkg).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                                                                    QStringLiteral("_"));
                         const QString localApk = m_workDir + QStringLiteral("/") + safe + QStringLiteral(".apk");
                         QFile::remove(localApk);
                         adbAsync(QStringList() << QStringLiteral("pull") << devicePath << localApk,
                                  60000,
                                  [this, pkg, localApk](bool pullOk, bool pullTimedOut, const QString &) {
                                      const ApkAppInfo info = (pullOk && !pullTimedOut)
                                                              ? apkParseFile(pkg, localApk, m_workDir)
                                                              : apkFallbackInfo(pkg);
                                      QFile::remove(localApk);
                                      m_cache.insert(pkg, info);
                                      oneFinished();
                                  });
                     });
        }

        void oneFinished()
        {
            --m_active;
            if (!m_queue.isEmpty()) {
                parseOne(m_queue.takeFirst());
                return;
            }
            if (m_active == 0) {
                finish();
            }
        }

        void finish()
        {
            if (!m_cache.isEmpty()) {
                writeDiskCache(m_serial, m_cache);
            }
            deleteLater();
        }

        QString m_serial;
        QString m_workDir;
        QStringList m_queue;
        QMap<QString, ApkAppInfo> m_cache;
        int m_active = 0;
    };

    // 进程内去重：同一 serial 只预热一次，重复连接/多窗口不会叠加 adb
    QSet<QString> &warmedSerials()
    {
        static QSet<QString> set;
        return set;
    }
}

void ApkManagerWindow::warmUpCache(const QString &serial)
{
    if (serial.isEmpty() || warmedSerials().contains(serial)) {
        return;
    }
    warmedSerials().insert(serial);
    auto *warmer = new ApkCacheWarmer(serial);
    warmer->start();
}

void ApkManagerWindow::clearWarmedCache(const QString &serial)
{
    warmedSerials().remove(serial);
}

ApkManagerWindow::ApkManagerWindow(const QString &serial, QWidget *parent)
    : QWidget(parent, Qt::Tool), m_serial(serial)
{
    m_localApkDir = QDir::tempPath() + QStringLiteral("/qtscrcpy_apkmanager_")
                    + QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(m_localApkDir);
    buildUi();
    retranslateUi();
    loadDiskCache(); // 先恢复磁盘缓存，首次展示即可命中，避免重复 pull/aapt
    startLoad(false);
}

ApkManagerWindow::~ApkManagerWindow()
{
    saveDiskCache(); // 即使中途关闭窗口，也保留已解析的结果供下次秒开
    // 清理本次拉取的临时 apk 与解出的图标
    QDir dir(m_localApkDir);
    if (dir.exists()) {
        for (const QString &name : dir.entryList(QDir::Files)) {
            dir.remove(name);
        }
        for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            dir.removeRecursively();
            break;
        }
        QDir().rmdir(m_localApkDir);
    }
}

void ApkManagerWindow::buildUi()
{
    // Qt::Tool 让窗口始终位于父窗口（投屏页面）之上；WindowStaysOnTopHint 保证层级稳定，
    // 保留最小/最大化按钮便于操作。
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint | Qt::WindowStaysOnTopHint);
    resize(960, 560); // 加宽以容纳图标+名称列，避免名称被挤压
    setWindowTitle(tr("APK管理"));

    auto *root = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    // 名称列按“图标+名称文字”内容算宽（ResizeToContents），保证名称永不被挤压到消失；
    // 包名列用 Stretch 吸收剩余宽度，过长文本靠 tooltip 显示全文；
    // 这样既不会因长包名/长版本号/宽操作列把名称列挤没，也保留长文本的查看手段。
    m_table->horizontalHeader()->setSectionResizeMode(COL_NAME, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(COL_PKG, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(COL_VERSION, QHeaderView::ResizeToContents);
    // 操作列固定宽度（在 onPkgListResult 中按按钮实际文字宽度设置），保证按钮文字不被压缩
    m_table->horizontalHeader()->setSectionResizeMode(COL_ACTIONS, QHeaderView::Fixed);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->setIconSize(QSize(36, 36));
    // 右键菜单复制文本 + Ctrl+C 复制选中单元格
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->installEventFilter(this);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &ApkManagerWindow::onTableContextMenu);
    root->addWidget(m_table);

    auto *bottom = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setMinimumWidth(10);
    bottom->addWidget(m_statusLabel, 1);
    m_refreshBtn = new QPushButton(this);
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    connect(m_refreshBtn, &QPushButton::clicked, this, &ApkManagerWindow::onRefreshClicked);
    bottom->addWidget(m_refreshBtn);
    root->addLayout(bottom);
}

void ApkManagerWindow::retranslateUi()
{
    setWindowTitle(tr("APK管理"));
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << tr("图标名称") << tr("包名") << tr("版本号") << tr("操作"));
    m_refreshBtn->setText(tr("刷新"));
}

void ApkManagerWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void ApkManagerWindow::setStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

void ApkManagerWindow::setItemText(QTableWidgetItem *item, const QString &text)
{
    if (!item) {
        return;
    }
    item->setText(text);
    // 悬浮显示完整文本（单元格宽度不足时也能看到全文）
    item->setToolTip(text);
}

void ApkManagerWindow::copyText(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    setStatus(tr("已复制到剪贴板"));
}

bool ApkManagerWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->matches(QKeySequence::Copy)) {
            QTableWidgetItem *item = m_table->currentItem();
            if (item) {
                copyText(item->text());
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ApkManagerWindow::onTableContextMenu(const QPoint &pos)
{
    QTableWidgetItem *item = m_table->itemAt(pos);
    if (!item) {
        return;
    }
    const int row = item->row();
    auto *nameItem = m_table->item(row, COL_NAME);
    auto *pkgItem = m_table->item(row, COL_PKG);
    auto *versionItem = m_table->item(row, COL_VERSION);
    const QString name = nameItem ? nameItem->text() : QString();
    const QString pkg = pkgItem ? pkgItem->text() : QString();
    const QString version = versionItem ? versionItem->text() : QString();

    QMenu menu(this);
    QAction *copyName = menu.addAction(tr("复制名称"));
    QAction *copyPkg = menu.addAction(tr("复制包名"));
    QAction *copyVersion = menu.addAction(tr("复制版本号"));
    menu.addSeparator();
    QAction *copyAll = menu.addAction(tr("复制整行"));

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (chosen == copyName) {
        copyText(name);
    } else if (chosen == copyPkg) {
        copyText(pkg);
    } else if (chosen == copyVersion) {
        copyText(version);
    } else if (chosen == copyAll) {
        copyText(name + QStringLiteral("\t") + pkg + QStringLiteral("\t") + version);
    }
}

void ApkManagerWindow::adbAsync(const QStringList &args, int timeoutMs,
                                const std::function<void(bool, bool, const QString &)> &done)
{
    auto *adb = new qsc::AdbProcess(this);
    adb->executeWithTimeout(m_serial, args, timeoutMs,
                            [adb, done](bool ok, bool timedOut, const QString &out) {
                                if (done) {
                                    done(ok, timedOut, out);
                                }
                                adb->deleteLater();
                            });
}

void ApkManagerWindow::onRefreshClicked()
{
    if (m_loading) {
        return;
    }
    // 快路径：缓存完整说明表格当前内容已全部来自缓存。刷新点击瞬时返回，
    // 不发 adb、不清空、不重建 widget；只在后台静默校验到包集合变化时才重载。
    if (cacheComplete()) {
        // 行数与缓存不一致属于异常残留：瞬时重建一次，页面布局保持完全一致
        if (m_table->rowCount() != m_cachedPkgs.size()) {
            rebuildFromCache();
        }
        verifyPackageListForRefresh();
        return;
    }
    // 首次加载或缓存不完整：完整异步加载
    startLoad(false);
}

void ApkManagerWindow::startLoad(bool useCache)
{
    Q_UNUSED(useCache); // 解析结果始终进 m_cache；是否复用由 cacheComplete() 判定
    if (m_loading) {
        return;
    }
    m_loading = true;
    m_refreshBtn->setEnabled(false);
    m_table->setRowCount(0);
    setStatus(tr("正在获取应用列表…"));

    adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
             << QStringLiteral("list") << QStringLiteral("packages") << QStringLiteral("-3"),
             15000,
             [this](bool ok, bool timedOut, const QString &out) {
                 onPkgListResult(ok, timedOut, out);
             });
}

void ApkManagerWindow::onPkgListResult(bool ok, bool timedOut, const QString &out)
{
    if (!ok || timedOut) {
        m_loading = false;
        m_refreshBtn->setEnabled(true);
        setStatus(tr("获取应用列表失败"));
        return;
    }
    setupRowsForPackages(parsePackageList(out));
}

QStringList ApkManagerWindow::parsePackageList(const QString &out)
{
    return apkParsePkgList(out);
}

bool ApkManagerWindow::samePackageSet(const QStringList &a, const QStringList &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    return QSet<QString>(a.begin(), a.end()) == QSet<QString>(b.begin(), b.end());
}

bool ApkManagerWindow::cacheComplete() const
{
    if (m_cachedPkgs.isEmpty()) {
        return false;
    }
    for (const QString &pkg : m_cachedPkgs) {
        auto it = m_cache.constFind(pkg);
        if (it == m_cache.constEnd() || !it->parsed) {
            return false;
        }
    }
    return true;
}

void ApkManagerWindow::setupRowsForPackages(const QStringList &pkgs)
{
    if (pkgs.isEmpty()) {
        m_cachedPkgs.clear();
        m_loading = false;
        m_refreshBtn->setEnabled(true);
        setStatus(tr("未找到第三方应用"));
        return;
    }

    // 新一轮加载：递增代号让上一批并发回调失效，避免旧结果覆盖新表格
    ++m_loadGeneration;
    m_activeParses = 0;

    // 记录本次展示顺序；缓存完整性以它为准（卸载/新装后会随之更新）
    m_cachedPkgs = pkgs;
    m_pkgQueue = pkgs;

    // 先按包名铺满列表，随后逐行异步补全图标/名称/版本
    m_table->setRowCount(pkgs.size());
    int actionsWidth = 0;
    for (int i = 0; i < pkgs.size(); ++i) {
        const QString &pkg = pkgs.at(i);
        auto *nameItem = new QTableWidgetItem(defaultIcon(), pkg);
        nameItem->setData(ROLE_PKG, pkg);
        setItemText(nameItem, pkg);
        m_table->setItem(i, COL_NAME, nameItem);
        auto *pkgItem = new QTableWidgetItem(pkg);
        pkgItem->setData(ROLE_PKG, pkg);
        setItemText(pkgItem, pkg);
        m_table->setItem(i, COL_PKG, pkgItem);
        auto *versionItem = new QTableWidgetItem(QStringLiteral("…"));
        setItemText(versionItem, QStringLiteral("…"));
        m_table->setItem(i, COL_VERSION, versionItem);

        QWidget *actions = buildActionWidget(pkg);
        m_table->setCellWidget(i, COL_ACTIONS, actions);
        actionsWidth = qMax(actionsWidth, actions->property("preferredWidth").toInt());
    }
    if (actionsWidth > 0) {
        m_table->setColumnWidth(COL_ACTIONS, actionsWidth);
    }

    m_loading = true;
    m_refreshBtn->setEnabled(false);
    pumpParseQueue();
}

void ApkManagerWindow::verifyPackageListForRefresh()
{
    if (m_verifying) {
        return;
    }
    m_verifying = true;
    // QProcess::start() 在 Windows 上要创建子进程（实测约 30~40ms），
    // 推迟到下一事件循环，让刷新点击处理本身真正瞬时返回。
    QTimer::singleShot(0, this, [this]() {
        adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
                 << QStringLiteral("list") << QStringLiteral("packages") << QStringLiteral("-3"),
                 15000,
                 [this](bool ok, bool timedOut, const QString &out) {
                     onRefreshVerifyResult(ok, timedOut, out);
                 });
    });
}

void ApkManagerWindow::onRefreshVerifyResult(bool ok, bool timedOut, const QString &out)
{
    m_verifying = false;
    if (!ok || timedOut) {
        // 静默失败：保留当前展示，不打断用户操作
        return;
    }
    const QStringList fresh = parsePackageList(out);
    if (fresh.isEmpty()) {
        return;
    }
    if (samePackageSet(fresh, m_cachedPkgs)) {
        // 集合未变：页面已是最新，完全不触碰表格
        return;
    }
    // 集合变化（新装/卸载）：复用本次校验结果重载，不再重复发一次 adb list
    m_loading = true;
    m_refreshBtn->setEnabled(false);
    m_table->setRowCount(0);
    setStatus(tr("正在获取应用列表…"));
    setupRowsForPackages(fresh);
}

void ApkManagerWindow::rebuildFromCache()
{
    m_table->setRowCount(m_cachedPkgs.size());
    int actionsWidth = 0;
    for (int i = 0; i < m_cachedPkgs.size(); ++i) {
        const QString &pkg = m_cachedPkgs.at(i);
        auto it = m_cache.constFind(pkg);
        const AppInfo *info = (it != m_cache.constEnd() && it->parsed) ? &(*it) : nullptr;
        const QString name = info ? info->label : pkg;
        const QPixmap icon = (info && !info->icon.isNull()) ? info->icon : defaultIcon();

        auto *nameItem = new QTableWidgetItem(icon, name);
        nameItem->setData(ROLE_PKG, pkg);
        setItemText(nameItem, name);
        m_table->setItem(i, COL_NAME, nameItem);

        auto *pkgItem = new QTableWidgetItem(pkg);
        pkgItem->setData(ROLE_PKG, pkg);
        setItemText(pkgItem, pkg);
        m_table->setItem(i, COL_PKG, pkgItem);

        const QString version = info
                                ? (info->version.isEmpty() ? QStringLiteral("-") : info->version)
                                : QStringLiteral("…");
        auto *versionItem = new QTableWidgetItem(version);
        setItemText(versionItem, version);
        m_table->setItem(i, COL_VERSION, versionItem);

        QWidget *actions = buildActionWidget(pkg);
        m_table->setCellWidget(i, COL_ACTIONS, actions);
        actionsWidth = qMax(actionsWidth, actions->property("preferredWidth").toInt());
    }
    if (actionsWidth > 0) {
        m_table->setColumnWidth(COL_ACTIONS, actionsWidth);
    }
}

QWidget *ApkManagerWindow::buildActionWidget(const QString &pkg)
{
    auto *widget = new QWidget(m_table);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    struct ActionDef
    {
        QString text;
        void (ApkManagerWindow::*slot)();
    };
    const ActionDef defs[] = {
        { tr("打开"), &ApkManagerWindow::onOpenClicked },
        { tr("卸载"), &ApkManagerWindow::onUninstallClicked },
        { tr("清缓存"), &ApkManagerWindow::onClearCacheClicked },
        { tr("杀进程"), &ApkManagerWindow::onForceStopClicked },
    };

    // 关键：全局 qss 给 QPushButton 设了 padding: 0 16px（左右共 32px），
    // 若用 horizontalAdvance + 固定余量算宽度会漏算 padding，导致“清缓存/杀进程”文字被裁。
    // 正确做法是先创建真实按钮、用其 sizeHint() —— Qt 会按当前 style + qss padding + 字体
    // 自动算出恰好容纳文字的宽度，再统一取最大值，保证任何 DPI/语言下文字都完整显示。
    QList<QPushButton *> buttons;
    for (const ActionDef &def : defs) {
        auto *btn = new QPushButton(def.text, widget);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("pkg", pkg);
        connect(btn, &QPushButton::clicked, this, def.slot);
        layout->addWidget(btn);
        buttons.append(btn);
    }
    int btnW = 0;
    const QFontMetrics fm(buttons.isEmpty() ? widget->font() : buttons.first()->font());
    for (int i = 0; i < buttons.size(); ++i) {
        // sizeHint 已含 qss padding；另取 fontMetrics 文字宽 + 40px 作为下限
        // （40 = 左右各 16px qss padding + 8px 余量），双保险防止任何环境下裁字。
        const int textFloor = fm.horizontalAdvance(defs[i].text) + 40;
        btnW = qMax(btnW, qMax(buttons.at(i)->sizeHint().width(), textFloor));
    }
    // 统一样式：等宽 + 固定高度，避免布局挤压；+2px 兜底 focus 边框等差异
    const int finalBtnW = btnW + 2;
    for (QPushButton *btn : buttons) {
        btn->setFixedWidth(finalBtnW);
    }
    // 显式算出操作区总宽度（4 按钮 + 3 间距 + 左右 margin），存为属性供列宽使用。
    // 不依赖 QWidget::sizeHint()，避免 setFixedWidth 后 hint 未刷新导致列宽偏小再次裁字。
    const int totalW = buttons.size() * finalBtnW
                       + (buttons.size() - 1) * layout->spacing()
                       + layout->contentsMargins().left() + layout->contentsMargins().right();
    widget->setProperty("preferredWidth", totalW);
    return widget;
}

QString ApkManagerWindow::currentPkgFromSender() const
{
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn) {
        return QString();
    }
    return btn->property("pkg").toString();
}

void ApkManagerWindow::pumpParseQueue()
{
    // 内存/磁盘缓存命中的行瞬时填充；未命中的包最多同时跑 MAX_CONCURRENT_PARSES 个，
    // 避免 7~N 个包串行执行 pm path + pull + aapt + tar 造成线性累加的等待。
    while (m_activeParses < MAX_CONCURRENT_PARSES && !m_pkgQueue.isEmpty()) {
        const QString pkg = m_pkgQueue.takeFirst();

        auto it = m_cache.constFind(pkg);
        if (it != m_cache.constEnd() && it->parsed) {
            applyToRow(pkg, *it);
            continue;
        }

        ++m_activeParses;
        startParseOne(pkg, m_loadGeneration);
    }

    if (m_activeParses == 0 && m_pkgQueue.isEmpty()) {
        saveDiskCache();
        m_loading = false;
        m_refreshBtn->setEnabled(true);
        setStatus(tr("已加载 %1 个应用").arg(m_table->rowCount()));
    }
}

void ApkManagerWindow::onParseOneFinished()
{
    if (m_activeParses > 0) {
        --m_activeParses;
    }
    pumpParseQueue();
}

void ApkManagerWindow::startParseOne(const QString &pkg, int generation)
{
    setStatus(tr("正在解析 %1").arg(pkg));

    const QString aapt = findAapt();
    if (aapt.isEmpty()) {
        // 未安装 Android SDK：降级为 dumpsys 取版本号，名称=包名，图标=占位
        adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("dumpsys")
                 << QStringLiteral("package") << pkg,
                 15000,
                 [this, pkg, generation](bool ok, bool timedOut, const QString &out) {
                     if (generation != m_loadGeneration) {
                         return; // 上一批任务的结果，直接丢弃
                     }
                     AppInfo info = fallbackInfo(pkg);
                     if (ok && !timedOut) {
                         QRegularExpression re(QStringLiteral("versionName[=: ]+([^\\s]+)"));
                         auto m = re.match(out);
                         if (m.hasMatch()) {
                             info.version = m.captured(1);
                         }
                     }
                     m_cache.insert(pkg, info);
                     applyToRow(pkg, info);
                     onParseOneFinished();
                 });
        return;
    }

    // 正常路径：pm path -> pull -> aapt 解析（每个回调都带 pkg/generation，互不干扰）
    adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
             << QStringLiteral("path") << pkg,
             15000,
             [this, pkg, generation](bool ok, bool timedOut, const QString &out) {
                 if (generation != m_loadGeneration) {
                     return;
                 }
                 QString devicePath;
                 for (const QString &raw : out.split('\n', Qt::SkipEmptyParts)) {
                     const QString line = raw.trimmed();
                     if (line.startsWith(QStringLiteral("package:"))) {
                         devicePath = line.mid(QStringLiteral("package:").size()).trimmed();
                         break;
                     }
                 }
                 if (!ok || timedOut || devicePath.isEmpty()) {
                     AppInfo info = fallbackInfo(pkg);
                     m_cache.insert(pkg, info);
                     applyToRow(pkg, info);
                     onParseOneFinished();
                     return;
                 }

                 const QString safe = QString(pkg).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                                                            QStringLiteral("_"));
                 const QString localApk = m_localApkDir + QStringLiteral("/") + safe + QStringLiteral(".apk");
                 QFile::remove(localApk);
                 adbAsync(QStringList() << QStringLiteral("pull") << devicePath << localApk,
                          60000,
                          [this, pkg, localApk, generation](bool pullOk, bool pullTimedOut, const QString &) {
                              if (generation != m_loadGeneration) {
                                  QFile::remove(localApk);
                                  return;
                              }
                              const AppInfo info = (pullOk && !pullTimedOut)
                                                   ? parseApk(pkg, localApk) : fallbackInfo(pkg);
                              QFile::remove(localApk);
                              m_cache.insert(pkg, info);
                              applyToRow(pkg, info);
                              onParseOneFinished();
                          });
             });
}

ApkManagerWindow::AppInfo ApkManagerWindow::parseApk(const QString &pkg, const QString &localApk)
{
    return apkParseFile(pkg, localApk, m_localApkDir);
}

ApkManagerWindow::AppInfo ApkManagerWindow::fallbackInfo(const QString &pkg) const
{
    return apkFallbackInfo(pkg);
}

QString ApkManagerWindow::cacheFilePath() const
{
    return apkCacheFilePath(m_serial);
}

void ApkManagerWindow::loadDiskCache()
{
    readDiskCache(m_serial, m_cache);
}

void ApkManagerWindow::saveDiskCache() const
{
    writeDiskCache(m_serial, m_cache);
}

void ApkManagerWindow::applyToRow(const QString &pkg, const AppInfo &info)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, COL_PKG);
        if (item && item->data(ROLE_PKG).toString() == pkg) {
            auto *nameItem = m_table->item(row, COL_NAME);
            if (nameItem) {
                setItemText(nameItem, info.label);
                if (!info.icon.isNull()) {
                    nameItem->setIcon(QIcon(info.icon));
                }
            }
            setItemText(m_table->item(row, COL_VERSION),
                        info.version.isEmpty() ? QStringLiteral("-") : info.version);
            return;
        }
    }
}

void ApkManagerWindow::openApp(const QString &pkg)
{
    adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("monkey")
             << QStringLiteral("-p") << pkg << QStringLiteral("1"),
             15000,
             [this, pkg](bool ok, bool timedOut, const QString &out) {
                 // monkey 在部分 ROM 上“启动失败”仍返回退出码 0，必须看输出文本判定。
                 const bool launched = ok && !timedOut
                                      && out.contains(QStringLiteral("Events injected"), Qt::CaseInsensitive);
                 if (launched) {
                     setStatus(tr("已打开 %1").arg(pkg));
                 } else {
                     // 附带失败原因，便于识别“无可启动 Activity”等情况
                     QString reason;
                     if (out.contains(QStringLiteral("No activities found"), Qt::CaseInsensitive)) {
                         reason = tr("应用没有可启动的界面");
                     }
                     setStatus(tr("打开失败：%1").arg(pkg) + (reason.isEmpty() ? QString() : QStringLiteral("（") + reason + QStringLiteral("）")));
                 }
             });
}

void ApkManagerWindow::uninstallApp(const QString &pkg)
{
    adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
             << QStringLiteral("uninstall") << pkg,
             20000,
             [this, pkg](bool ok, bool timedOut, const QString &out) {
                 const bool success = ok && !timedOut && out.contains(QStringLiteral("Success"), Qt::CaseInsensitive);
                 setStatus(success ? tr("已卸载 %1").arg(pkg) : tr("卸载失败：%1").arg(pkg));
                 if (success) {
                     m_cache.remove(pkg);
                     m_cachedPkgs.removeAll(pkg);
                     saveDiskCache();
                     startLoad(true);
                 }
             });
}

void ApkManagerWindow::clearAppCache(const QString &pkg)
{
    adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm")
             << QStringLiteral("clear") << pkg,
             15000,
             [this, pkg](bool ok, bool timedOut, const QString &out) {
                 const bool success = ok && !timedOut && out.contains(QStringLiteral("Success"), Qt::CaseInsensitive);
                 setStatus(success ? tr("已清除 %1 的缓存").arg(pkg) : tr("清缓存失败：%1").arg(pkg));
             });
}

void ApkManagerWindow::forceStopApp(const QString &pkg)
{
    adbAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("am")
             << QStringLiteral("force-stop") << pkg,
             15000,
             [this, pkg](bool ok, bool timedOut, const QString &out) {
                 setStatus((ok && !timedOut) ? tr("已杀掉 %1 的进程").arg(pkg) : tr("杀进程失败：%1").arg(pkg));
                 Q_UNUSED(out);
             });
}

void ApkManagerWindow::onOpenClicked()
{
    const QString pkg = currentPkgFromSender();
    if (!pkg.isEmpty()) openApp(pkg);
}

void ApkManagerWindow::onUninstallClicked()
{
    const QString pkg = currentPkgFromSender();
    if (pkg.isEmpty()) return;
    if (QMessageBox::question(this, tr("卸载确认"), tr("确定卸载 %1 吗？").arg(pkg),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
        uninstallApp(pkg);
    }
}

void ApkManagerWindow::onClearCacheClicked()
{
    const QString pkg = currentPkgFromSender();
    if (!pkg.isEmpty()) clearAppCache(pkg);
}

void ApkManagerWindow::onForceStopClicked()
{
    const QString pkg = currentPkgFromSender();
    if (!pkg.isEmpty()) forceStopApp(pkg);
}

QString ApkManagerWindow::findAdb()
{
    QString adb = Config::getInstance().getAdbPath();
    if (!adb.isEmpty() && QFileInfo::exists(adb)) {
        return adb;
    }
    return QStringLiteral("adb");
}

QString ApkManagerWindow::findAapt()
{
    // SDK 目录扫描进程内只做一次（共享工具内 static 缓存），窗口与预热 worker 共用
    return apkFindAapt();
}

bool ApkManagerWindow::parseBadging(const QString &text, QString &label, QString &version, QString &iconPath)
{
    return apkParseBadging(text, label, version, iconPath);
}

QPixmap ApkManagerWindow::defaultIcon()
{
    return apkDefaultIcon();
}
