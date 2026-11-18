#include "filemanagerwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableView>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const int ROLE_SORT = Qt::UserRole + 1;
const int ROLE_ENTRY = Qt::UserRole + 2;

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

QString parentPath(const QString &path)
{
    const int idx = path.lastIndexOf('/');
    if (idx < 0) {
        return QStringLiteral("/");
    }
    if (0 == idx) {
        return QStringLiteral("/");
    }
    return path.left(idx);
}

} // namespace

FileManagerWindow::FileManagerWindow(const QString &serial, QWidget *parent)
    : QWidget(parent, Qt::Window), m_serial(serial)
{
    setAcceptDrops(true);
    m_manager = new qsc::FileManager(serial, this);

    buildUi();
    retranslateUi();

    connect(m_manager, &qsc::FileManager::dirListed, this, &FileManagerWindow::onDirListed);
    connect(m_manager, &qsc::FileManager::listFailed, this, &FileManagerWindow::onListFailed);
    connect(m_manager, &qsc::FileManager::transferStarted, this, &FileManagerWindow::onTransferStarted);
    connect(m_manager, &qsc::FileManager::transferProgress, this, &FileManagerWindow::onTransferProgress);
    connect(m_manager, &qsc::FileManager::transferItemFinished, this, &FileManagerWindow::onTransferItemFinished);
    connect(m_manager, &qsc::FileManager::transferFinished, this, &FileManagerWindow::onTransferFinished);
    connect(m_manager, &qsc::FileManager::textFileLoaded, this, &FileManagerWindow::onTextFileLoaded);
    connect(m_manager, &qsc::FileManager::textFileLoadFailed, this, &FileManagerWindow::onTextFileLoadFailed);
    connect(m_manager, &qsc::FileManager::textFileSaved, this, &FileManagerWindow::onTextFileSaved);
    connect(m_manager, &qsc::FileManager::deleteFinished, this, &FileManagerWindow::onDeleteFinished);

    navigateTo(QStringLiteral("/sdcard"));
}

FileManagerWindow::~FileManagerWindow() = default;

void FileManagerWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ---- 导航栏 ----
    auto *nav = new QHBoxLayout;
    nav->setSpacing(4);

    m_backBtn = new QToolButton(this);
    m_backBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_backBtn->setAutoRaise(true);

    m_forwardBtn = new QToolButton(this);
    m_forwardBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    m_forwardBtn->setAutoRaise(true);

    m_upBtn = new QToolButton(this);
    m_upBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_upBtn->setAutoRaise(true);

    m_refreshBtn = new QToolButton(this);
    m_refreshBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshBtn->setAutoRaise(true);

    m_uploadBtn = new QToolButton(this);
    m_uploadBtn->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_uploadBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_downloadBtn = new QToolButton(this);
    m_downloadBtn->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_downloadBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    nav->addWidget(m_backBtn);
    nav->addWidget(m_forwardBtn);
    nav->addWidget(m_upBtn);
    nav->addWidget(m_refreshBtn);
    nav->addWidget(m_uploadBtn);
    nav->addWidget(m_downloadBtn);

    auto *crumbScroll = new QScrollArea(this);
    crumbScroll->setWidgetResizable(true);
    crumbScroll->setFrameShape(QFrame::NoFrame);
    crumbScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    crumbScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    crumbScroll->setMinimumHeight(30);
    crumbScroll->setMaximumHeight(34);
    m_crumbWidget = new QWidget(crumbScroll);
    m_crumbLayout = new QHBoxLayout(m_crumbWidget);
    m_crumbLayout->setContentsMargins(2, 0, 2, 0);
    m_crumbLayout->setSpacing(0);
    crumbScroll->setWidget(m_crumbWidget);
    nav->addWidget(crumbScroll, 1);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setMinimumWidth(220);
    nav->addWidget(m_pathEdit);

    root->addLayout(nav);

    // ---- 文件列表 ----
    m_model = new QStandardItemModel(0, 4, this);

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(ROLE_SORT);
    // 动态排序保持开启：关闭时新增行不会重新排序，列表会退回 find 的原始输出顺序，
    // 表现为"进入新目录/刷新后排序变了"。批量插入期间在 applyEntries 里临时关闭。

    m_view = new QTableView(this);
    m_view->setModel(m_proxy);
    m_view->setSortingEnabled(true);
    // QHeaderView 默认指示方向是降序（文件排最前），显式改为
    // "名称升序、文件夹在前"作为初始排序，用户点列头后再跟随用户
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setAlternatingRowColors(true);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->setVisible(false);
    m_view->verticalHeader()->setDefaultSectionSize(26);
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    // 让拖拽事件落到窗口本身（保留整窗投放）
    m_view->setAcceptDrops(false);
    m_view->viewport()->setAcceptDrops(false);
    m_view->setDragDropMode(QAbstractItemView::NoDragDrop);
    root->addWidget(m_view, 1);

    // ---- 状态行 ----
    auto *statusRow = new QHBoxLayout;
    m_statusLabel = new QLabel(this);
    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    m_progress->setTextVisible(true);
    m_progress->setMaximumWidth(280);
    statusRow->addWidget(m_statusLabel, 1);
    statusRow->addWidget(m_progress);
    root->addLayout(statusRow);

    connect(m_backBtn, &QToolButton::clicked, this, &FileManagerWindow::goBack);
    connect(m_forwardBtn, &QToolButton::clicked, this, &FileManagerWindow::goForward);
    connect(m_upBtn, &QToolButton::clicked, this, &FileManagerWindow::goUp);
    connect(m_refreshBtn, &QToolButton::clicked, this, &FileManagerWindow::refresh);
    connect(m_uploadBtn, &QToolButton::clicked, this, &FileManagerWindow::uploadViaDialog);
    connect(m_downloadBtn, &QToolButton::clicked, this, &FileManagerWindow::downloadSelected);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        QString p = m_pathEdit->text().trimmed();
        if (p.isEmpty()) {
            return;
        }
        if (!p.startsWith('/')) {
            p = joinPath(m_currentPath, p);
        }
        navigateTo(p);
    });
    connect(m_view, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const QModelIndex src = m_proxy->mapToSource(index);
        if (!src.isValid()) {
            return;
        }
        QStandardItem *item = m_model->item(src.row(), 0);
        if (!item) {
            return;
        }
        const auto entry = item->data(ROLE_ENTRY).value<qsc::DeviceFileEntry>();
        if (entry.isDir) {
            navigateTo(entry.path);
        } else if (entry.isLink) {
            // 软链接可能指向目录：直接尝试进入，指向文件时由后端回"该路径不是文件夹"
            navigateTo(entry.path);
        } else if (isEditableName(entry.name)) {
            openEditor(entry.path);
        } else {
            m_statusLabel->setText(tr("%1 不是可编辑的文本文件，可右键下载").arg(entry.name));
        }
    });
    connect(m_view, &QTableView::customContextMenuRequested, this, &FileManagerWindow::showContextMenu);

    resize(1000, 660);
}

void FileManagerWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void FileManagerWindow::retranslateUi()
{
    setWindowTitle(tr("File manager - %1").arg(m_serial));
    m_backBtn->setToolTip(tr("Back (Alt+Left)"));
    m_forwardBtn->setToolTip(tr("Forward (Alt+Right)"));
    m_upBtn->setToolTip(tr("Parent folder (Backspace)"));
    m_refreshBtn->setToolTip(tr("Refresh (F5)"));
    m_uploadBtn->setText(tr("Upload"));
    m_uploadBtn->setToolTip(tr("Upload files to current folder (you can also drag files/folders onto the window)"));
    m_downloadBtn->setText(tr("Download"));
    m_downloadBtn->setToolTip(tr("Download selected files/folders to computer"));
    m_pathEdit->setPlaceholderText(tr("Enter device path and press Enter to jump"));
    m_model->setHorizontalHeaderLabels(
        QStringList() << tr("Name") << tr("Modified") << tr("Type") << tr("Size"));
    if (m_statusLabel->text().isEmpty()) {
        m_statusLabel->setText(tr("Ready"));
    }
}

// ---------------- 导航 ----------------

void FileManagerWindow::navigateTo(const QString &path, bool recordHistory)
{
    m_currentPath = path;
    m_pathEdit->clear();
    rebuildBreadcrumb(path);
    m_statusLabel->setText(tr("正在加载 %1 …").arg(path));
    m_manager->listDir(path);

    if (recordHistory) {
        // 浏览器语义：新导航截断前进栈
        while (m_history.size() > m_historyPos + 1) {
            m_history.removeLast();
        }
        if (m_history.isEmpty() || m_history.last() != path) {
            m_history.append(path);
            if (m_history.size() > 100) {
                m_history.removeFirst();
            }
        }
        m_historyPos = m_history.size() - 1;
    }
    updateNavState();
}

void FileManagerWindow::refresh()
{
    m_statusLabel->setText(tr("正在刷新 …"));
    m_manager->listDir(m_currentPath);
}

void FileManagerWindow::goBack()
{
    if (m_historyPos <= 0) {
        return;
    }
    --m_historyPos;
    navigateTo(m_history.at(m_historyPos), false);
}

void FileManagerWindow::goForward()
{
    if (m_historyPos < 0 || m_historyPos + 1 >= m_history.size()) {
        return;
    }
    ++m_historyPos;
    navigateTo(m_history.at(m_historyPos), false);
}

void FileManagerWindow::goUp()
{
    if (m_currentPath == "/" || m_currentPath.isEmpty()) {
        return;
    }
    QString up = parentPath(m_currentPath);
    if (up == m_currentPath) {
        up = "/";
    }
    navigateTo(up);
}

void FileManagerWindow::updateNavState()
{
    m_backBtn->setEnabled(m_historyPos > 0);
    m_forwardBtn->setEnabled(m_historyPos >= 0 && m_historyPos + 1 < m_history.size());
    m_upBtn->setEnabled(!m_currentPath.isEmpty() && m_currentPath != "/");
}

void FileManagerWindow::rebuildBreadcrumb(const QString &path)
{
    // 清空旧层级
    while (QLayoutItem *item = m_crumbLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    QString acc;
    auto addSegment = [this](const QString &text, const QString &target, bool current) {
        if (m_crumbLayout->count() > 0) {
            auto *sep = new QLabel(">", m_crumbWidget);
            sep->setStyleSheet("color:#888;");
            m_crumbLayout->addWidget(sep);
        }
        auto *btn = new QToolButton(m_crumbWidget);
        btn->setText(text);
        btn->setAutoRaise(true);
        btn->setEnabled(!current);
        btn->setCursor(Qt::PointingHandCursor);
        const QString dest = target;
        connect(btn, &QToolButton::clicked, this, [this, dest]() { navigateTo(dest); });
        m_crumbLayout->addWidget(btn);
    };

    addSegment("/", "/", parts.isEmpty());
    for (int i = 0; i < parts.size(); ++i) {
        acc += "/" + parts.at(i);
        addSegment(parts.at(i), acc, i == parts.size() - 1);
    }
    m_crumbLayout->addStretch(1);
}

// ---------------- 数据渲染 ----------------

void FileManagerWindow::onDirListed(const QString &path, const QVector<qsc::DeviceFileEntry> &entries)
{
    if (path != m_currentPath) {
        return; // 过期的响应
    }
    applyEntries(entries);
    int dirCount = 0;
    for (const auto &e : entries) {
        if (e.isDir) {
            ++dirCount;
        }
    }
    m_statusLabel->setText(tr("%1 项（%2 个文件夹，%3 个文件）").arg(entries.size()).arg(dirCount).arg(entries.size() - dirCount));
}

void FileManagerWindow::onListFailed(const QString &path, const QString &reason)
{
    m_statusLabel->setText(tr("加载失败：%1").arg(reason));
    if (path == m_currentPath) {
        QMessageBox::warning(this, tr("无法打开目录"),
                             tr("无法打开 %1\n%2").arg(path, reason));
    }
}

void FileManagerWindow::applyEntries(const QVector<qsc::DeviceFileEntry> &entries)
{
    // 批量插入时关掉动态排序（避免逐行重排），插入完成后按当前列头一次性排序
    m_proxy->setDynamicSortFilter(false);
    m_model->removeRows(0, m_model->rowCount());
    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);

    for (const auto &e : entries) {
        auto *nameItem = new QStandardItem(e.isDir ? dirIcon : fileIcon, e.name);
        nameItem->setData(QString(e.isDir ? "0" : "1") + e.name.toLower(), ROLE_SORT);
        nameItem->setData(QVariant::fromValue(e), ROLE_ENTRY);
        nameItem->setToolTip(e.path);

        auto *timeItem = new QStandardItem(formatTime(e.mtime));
        timeItem->setData(static_cast<qlonglong>(e.mtime), ROLE_SORT);
        timeItem->setToolTip(e.mtime > 0 ? QDateTime::fromSecsSinceEpoch(e.mtime).toString("yyyy/M/d HH:mm:ss")
                                         : QString());

        auto *typeItem = new QStandardItem(typeText(e));
        typeItem->setData(typeText(e), ROLE_SORT);

        auto *sizeItem = new QStandardItem(e.isDir ? QStringLiteral("-") : formatSize(e.size));
        sizeItem->setData(static_cast<qlonglong>(e.isDir ? -1 : e.size), ROLE_SORT);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_model->appendRow(QList<QStandardItem *>() << nameItem << timeItem << typeItem << sizeItem);
    }
    m_proxy->setDynamicSortFilter(true);
    // 始终按列头当前指示的列与方向重排：首次是"名称升序"（文件夹在前），
    // 之后跟随用户点过的列头，刷新/进入新目录都不会改变排序方式
    const int column = qMax(0, m_view->horizontalHeader()->sortIndicatorSection());
    const Qt::SortOrder order = m_view->horizontalHeader()->sortIndicatorOrder();
    m_proxy->sort(column, order);
    m_view->horizontalHeader()->setSortIndicator(column, order);
}

QString FileManagerWindow::typeText(const qsc::DeviceFileEntry &entry)
{
    if (entry.isDir) {
        return tr("文件夹");
    }
    if (entry.isLink) {
        return tr("链接");
    }
    const QString suffix = QFileInfo(entry.name).suffix().toLower();
    if (suffix.isEmpty()) {
        return tr("文件");
    }
    static const QStringList image = {"png", "jpg", "jpeg", "gif", "bmp", "webp", "heic", "svg"};
    static const QStringList video = {"mp4", "mkv", "avi", "mov", "3gp", "webm", "flv", "ts"};
    static const QStringList audio = {"mp3", "wav", "flac", "aac", "ogg", "m4a", "amr", "opus"};
    static const QStringList text = {"txt", "ini", "json", "xml", "html", "htm", "css", "js", "py",
                                     "java", "kt", "sh", "log", "md", "csv", "prop", "conf", "yml", "yaml"};
    static const QStringList archive = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz"};
    if (image.contains(suffix)) {
        return tr("图片");
    }
    if (video.contains(suffix)) {
        return tr("视频");
    }
    if (audio.contains(suffix)) {
        return tr("音频");
    }
    if (text.contains(suffix)) {
        return tr("文本");
    }
    if (archive.contains(suffix)) {
        return tr("压缩包");
    }
    if (suffix == "apk") {
        return tr("安装包");
    }
    return suffix.toUpper() + tr(" 文件");
}

QString FileManagerWindow::formatSize(qint64 bytes)
{
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    }
    const double kb = bytes / 1024.0;
    if (kb < 1024) {
        return QString("%1 KB").arg(kb, 0, 'f', kb < 10 ? 2 : 1);
    }
    const double mb = kb / 1024.0;
    if (mb < 1024) {
        return QString("%1 MB").arg(mb, 0, 'f', mb < 10 ? 2 : 1);
    }
    return QString("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

QString FileManagerWindow::formatTime(qint64 epochSec)
{
    if (epochSec <= 0) {
        return QStringLiteral("-");
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(epochSec);
    const qint64 diff = dt.secsTo(QDateTime::currentDateTime());
    if (diff >= 0 && diff < 60) {
        return tr("刚刚");
    }
    if (diff >= 60 && diff < 3600) {
        return tr("%1 分钟前").arg(diff / 60);
    }
    if (diff >= 3600 && diff < 86400) {
        return tr("%1 小时前").arg(diff / 3600);
    }
    if (diff >= 86400 && diff < 7 * 86400) {
        return tr("%1 天前").arg(diff / 86400);
    }
    return dt.toString("yyyy/M/d");
}

// ---------------- 选择与操作 ----------------

QStringList FileManagerWindow::selectedPaths() const
{
    QStringList paths;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows(0);
    for (const QModelIndex &index : rows) {
        const QModelIndex src = m_proxy->mapToSource(index);
        QStandardItem *item = m_model->item(src.row(), 0);
        if (!item) {
            continue;
        }
        const auto entry = item->data(ROLE_ENTRY).value<qsc::DeviceFileEntry>();
        if (!entry.path.isEmpty()) {
            paths << entry.path;
        }
    }
    return paths;
}

bool FileManagerWindow::isEditableName(const QString &name) const
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    static const QStringList text = {"txt", "ini", "json", "xml", "html", "htm", "css", "js", "py",
                                     "java", "kt", "sh", "log", "md", "csv", "prop", "conf", "yml", "yaml"};
    if (text.contains(suffix)) {
        return true;
    }
    // 无扩展名的配置文件（config.ini 之类已在上面覆盖）
    return suffix.isEmpty() && !name.startsWith('.');
}

void FileManagerWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    QMenu menu(this);

    if (index.isValid()) {
        const QModelIndex src = m_proxy->mapToSource(index);
        QStandardItem *item = m_model->item(src.row(), 0);
        const auto entry = item ? item->data(ROLE_ENTRY).value<qsc::DeviceFileEntry>() : qsc::DeviceFileEntry();

        if (entry.isDir || entry.isLink) {
            menu.addAction(tr("打开"), this, [this, entry]() { navigateTo(entry.path); });
        } else if (isEditableName(entry.name)) {
            menu.addAction(tr("编辑"), this, [this, entry]() { openEditor(entry.path); });
        }
        menu.addAction(tr("下载到…"), this, &FileManagerWindow::downloadSelected);
        menu.addSeparator();
        menu.addAction(tr("复制路径"), this, [entry]() {
            QApplication::clipboard()->setText(entry.path);
        });
        menu.addSeparator();

        // 删除作用于"选中集合 ∪ 右键所在行"：右键落在已选行上时删除全部选中项，
        // 落在未选行上时只删除该行。右键空白处（index 无效）不提供删除入口。
        QSet<QString> picked;
        for (const QModelIndex &si : m_view->selectionModel()->selectedRows()) {
            const QModelIndex ssrc = m_proxy->mapToSource(si);
            QStandardItem *sitem = m_model->item(ssrc.row(), 0);
            if (sitem) {
                picked << sitem->data(ROLE_ENTRY).value<qsc::DeviceFileEntry>().path;
            }
        }
        if (!entry.path.isEmpty()) {
            picked << entry.path;
        }
        const QStringList deleteTargets = picked.values();
        if (!deleteTargets.isEmpty()) {
            QAction *delAct = menu.addAction(tr("删除"), this, [this, deleteTargets]() {
                confirmDelete(deleteTargets);
            });
            delAct->setToolTip(tr("删除选中的文件/目录（目录将连同内容一起删除，不可恢复）"));
        }
        menu.addSeparator();
    }
    menu.addAction(tr("刷新"), this, &FileManagerWindow::refresh);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void FileManagerWindow::confirmDelete(const QStringList &devicePaths)
{
    if (devicePaths.isEmpty()) {
        return;
    }

    // 确认弹窗：警示不可逆；默认聚焦"取消"，回车不会直接触发删除
    const QString text = devicePaths.size() == 1
                             ? tr("确定要删除“%1”吗？\n\n目录将被连同其全部内容一起删除，此操作无法撤销。")
                                   .arg(devicePaths.first())
                             : tr("确定要删除所选的 %1 个条目吗？\n\n目录将被连同其全部内容一起删除，此操作无法撤销。")
                                   .arg(devicePaths.size());
    QMessageBox box(QMessageBox::Warning, tr("确认删除"), text,
                    QMessageBox::Ok | QMessageBox::Cancel, this);
    box.setButtonText(QMessageBox::Ok, tr("删除"));
    box.setButtonText(QMessageBox::Cancel, tr("取消"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (devicePaths.size() > 1) {
        box.setDetailedText(devicePaths.join(QLatin1Char('\n')));
    }
    if (box.exec() != QMessageBox::Ok) {
        return; // 取消：不做任何操作
    }

    m_statusLabel->setText(tr("正在删除 %1 个条目…").arg(devicePaths.size()));
    m_manager->deletePaths(devicePaths);
}

void FileManagerWindow::onDeleteFinished(const QStringList &paths, bool ok, const QString &error)
{
    if (ok) {
        m_statusLabel->setText(tr("删除完成（%1 个条目）").arg(paths.size()));
    } else {
        m_statusLabel->setText(tr("删除失败：%1").arg(error));
        QMessageBox::warning(this, tr("删除失败"),
                             error.isEmpty() ? tr("设备返回了未知错误") : error);
    }
    // 无论成败都刷新：失败时可能已删除其中一部分
    refresh();
}

void FileManagerWindow::uploadViaDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("选择要上传的文件"));
    if (files.isEmpty()) {
        return;
    }
    startUpload(files, m_currentPath);
}

void FileManagerWindow::downloadSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) {
        m_statusLabel->setText(tr("请先选择要下载的文件或文件夹"));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择保存位置"));
    if (dir.isEmpty()) {
        return;
    }
    m_pendingTransfers = paths.size();
    m_manager->downloadFiles(paths, dir, false);
}

// ---------------- 拖拽上传 ----------------

void FileManagerWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void FileManagerWindow::dropEvent(QDropEvent *event)
{
    QStringList locals;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            locals << url.toLocalFile();
        }
    }
    if (locals.isEmpty()) {
        return;
    }

    // 落点对应的文件夹优先作为目标
    const QPoint pos = m_view->viewport()->mapFrom(this, event->pos());
    QString target = dropTargetDir(pos);

    startUpload(locals, target);
    event->acceptProposedAction();
}

QString FileManagerWindow::dropTargetDir(const QPoint &pos) const
{
    const QModelIndex index = m_view->indexAt(pos);
    if (index.isValid()) {
        const QModelIndex src = m_proxy->mapToSource(index);
        QStandardItem *item = m_model->item(src.row(), 0);
        if (item) {
            const auto entry = item->data(ROLE_ENTRY).value<qsc::DeviceFileEntry>();
            if (entry.isDir) {
                return entry.path;
            }
        }
    }
    return m_currentPath;
}

void FileManagerWindow::startUpload(const QStringList &localPaths, const QString &deviceDir)
{
    QStringList locals;
    QStringList remotes;
    QStringList conflicts;

    // 当前目录已有条目名（用于冲突提示）
    QStringList existing;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (QStandardItem *item = m_model->item(row, 0)) {
            existing << item->text();
        }
    }

    for (const QString &path : localPaths) {
        const QFileInfo fi(path);
        if (fi.isDir()) {
            const QString rootName = fi.fileName();
            if (existing.contains(rootName)) {
                conflicts << rootName;
            }
            QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                            QDirIterator::Subdirectories);
            const QDir baseDir(fi.absoluteFilePath());
            while (it.hasNext()) {
                const QString file = it.next();
                const QString rel = baseDir.relativeFilePath(file);
                locals << file;
                remotes << joinPath(joinPath(deviceDir, rootName), rel);
            }
        } else if (fi.isFile()) {
            if (existing.contains(fi.fileName())) {
                conflicts << fi.fileName();
            }
            locals << fi.absoluteFilePath();
            remotes << joinPath(deviceDir, fi.fileName());
        }
    }

    if (locals.isEmpty()) {
        QMessageBox::information(this, tr("上传"), tr("没有可上传的文件"));
        return;
    }

    if (!conflicts.isEmpty()) {
        QString preview = conflicts.mid(0, 8).join("\n");
        if (conflicts.size() > 8) {
            preview += tr("\n… 共 %1 项").arg(conflicts.size());
        }
        const auto ret = QMessageBox::question(this, tr("目标已存在"),
                                               tr("以下项目在设备上已存在，是否覆盖？\n\n%1").arg(preview),
                                               QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            m_statusLabel->setText(tr("已取消上传"));
            return;
        }
    }

    m_pendingTransfers = locals.size();
    m_transferErrors.clear();
    m_manager->uploadFiles(locals, remotes, true);
}

// ---------------- 传输反馈 ----------------

void FileManagerWindow::onTransferStarted(int total)
{
    m_pendingTransfers = total;
    m_transferErrors.clear();
    m_progress->setVisible(true);
    m_progress->setRange(0, qMax(1, total));
    m_progress->setValue(0);
    m_statusLabel->setText(tr("开始传输，共 %1 个文件…").arg(total));
}

void FileManagerWindow::onTransferProgress(int index, const QString &name, qint64 done, qint64 total)
{
    m_progress->setVisible(true);
    m_progress->setRange(0, qMax(1, m_pendingTransfers));
    m_progress->setValue(index);
    QString text = tr("第 %1/%2 个：%3").arg(index + 1).arg(m_pendingTransfers).arg(name);
    if (total > 0) {
        text += QString(" (%1%)").arg(qBound<qint64>(0, done * 100 / total, 100));
    }
    m_statusLabel->setText(text);
}

void FileManagerWindow::onTransferItemFinished(int index, const QString &name, bool ok, const QString &error)
{
    Q_UNUSED(index)
    if (!ok) {
        m_transferErrors << QString("%1：%2").arg(name, error);
    }
}

void FileManagerWindow::onTransferFinished(int succeeded, int failed, bool canceled)
{
    m_progress->setVisible(false);
    m_statusLabel->setText(canceled ? tr("传输已取消（成功 %1，失败 %2）").arg(succeeded).arg(failed)
                                    : tr("传输完成：成功 %1，失败 %2").arg(succeeded).arg(failed));

    if (failed > 0) {
        QMessageBox::warning(this, tr("传输失败"),
                             m_transferErrors.mid(0, 12).join("\n"));
    }
    m_transferErrors.clear();
    refresh();
}

// ---------------- 编辑 ----------------

void FileManagerWindow::openEditor(const QString &path)
{
    if (m_editor) {
        m_editor->raise();
        m_editor->activateWindow();
        return;
    }
    m_editorPath = path;
    m_editorSaveForce = false;
    m_statusLabel->setText(tr("正在读取 %1 …").arg(path));
    m_manager->readTextFile(path);
}

void FileManagerWindow::onTextFileLoaded(const QString &path, const QString &content,
                                         const QString &encoding, qint64 mtime)
{
    m_editorEncoding = encoding;
    m_editorMtime = mtime;

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("编辑 - %1").arg(path));
    dlg->resize(780, 560);

    auto *layout = new QVBoxLayout(dlg);
    auto *info = new QLabel(tr("编码：%1    换行：%2    保存时回写到设备原路径")
                                .arg(encoding, content.contains("\r\n") ? "CRLF" : "LF"),
                            dlg);
    info->setStyleSheet("color:#888;");
    layout->addWidget(info);

    auto *edit = new QPlainTextEdit(dlg);
    edit->setPlainText(content);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(edit, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dlg);
    layout->addWidget(buttons);

    m_editor = dlg;
    m_editorText = edit;

    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, path]() {
        if (!m_editorText) {
            return;
        }
        m_statusLabel->setText(tr("正在保存 %1 …").arg(path));
        m_manager->writeTextFile(path,
                                 m_editorText->toPlainText().toUtf8(),
                                 m_editorEncoding,
                                 m_editorSaveForce ? 0 : m_editorMtime,
                                 m_editorSaveForce);
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(dlg, &QObject::destroyed, this, [this]() {
        m_editorText = nullptr;
        m_editorPath.clear();
    });

    dlg->show();
    m_statusLabel->setText(tr("已打开 %1").arg(path));
}

void FileManagerWindow::onTextFileLoadFailed(const QString &path, const QString &reason)
{
    m_statusLabel->setText(tr("读取失败：%1").arg(reason));
    QMessageBox::warning(this, tr("无法打开文件"), tr("%1\n%2").arg(path, reason));
}

void FileManagerWindow::onTextFileSaved(const QString &path, bool ok, const QString &error)
{
    if (ok) {
        m_statusLabel->setText(tr("已保存 %1").arg(path));
        if (m_editor) {
            m_editor->close();
        }
        refresh();
        return;
    }
    if (error == QLatin1String("conflict")) {
        const auto ret = QMessageBox::question(this, tr("文件已被修改"),
                                               tr("设备上的文件在编辑期间已被其他程序修改。\n仍要覆盖保存吗？"),
                                               QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            m_editorSaveForce = true;
            retrySaveWithForce();
            return;
        }
        m_statusLabel->setText(tr("已取消保存"));
        return;
    }
    m_statusLabel->setText(tr("保存失败：%1").arg(error));
    QMessageBox::warning(this, tr("保存失败"), tr("%1\n%2").arg(path, error));
}

void FileManagerWindow::retrySaveWithForce()
{
    if (!m_editorText || m_editorPath.isEmpty()) {
        return;
    }
    m_manager->writeTextFile(m_editorPath,
                             m_editorText->toPlainText().toUtf8(),
                             m_editorEncoding,
                             0,
                             true);
}

// ---------------- 事件 ----------------

void FileManagerWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F5) {
        refresh();
        return;
    }
    if (event->key() == Qt::Key_Backspace) {
        goUp();
        return;
    }
    if (event->modifiers() & Qt::AltModifier) {
        if (event->key() == Qt::Key_Left) {
            goBack();
            return;
        }
        if (event->key() == Qt::Key_Right) {
            goForward();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void FileManagerWindow::closeEvent(QCloseEvent *event)
{
    if (m_manager) {
        m_manager->cancelAll();
    }
    QWidget::closeEvent(event);
}

void FileManagerWindow::setBusy(bool busy)
{
    m_progress->setVisible(busy);
}

void FileManagerWindow::showError(const QString &title, const QString &text)
{
    QMessageBox::warning(this, title, text);
}
