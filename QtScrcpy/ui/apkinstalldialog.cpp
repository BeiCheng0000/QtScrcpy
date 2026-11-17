#include "apkinstalldialog.h"
#include "apkpathutils.h"

#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSet>
#include <QUrl>
#include <QVBoxLayout>

ApkInstallDialog::ApkInstallDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("批量安装 APK"));
    setAcceptDrops(true);
    resize(680, 480);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("拖入 apk 文件或目录，或输入路径（支持 \\\\server\\share 网络路径）："), this));

    auto *inputRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(
        QStringLiteral("例如：C:\\apks\\app.apk  或  \\\\172.26.20.199\\研发部\\安卓\\...\\xx.apk  或目录"));
    auto *addBtn = new QPushButton(QStringLiteral("添加"), this);
    auto *fileBtn = new QPushButton(QStringLiteral("选择文件"), this);
    auto *dirBtn = new QPushButton(QStringLiteral("选择目录"), this);
    inputRow->addWidget(m_pathEdit, 1);
    inputRow->addWidget(addBtn);
    inputRow->addWidget(fileBtn);
    inputRow->addWidget(dirBtn);
    layout->addLayout(inputRow);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setToolTip(QStringLiteral("可将文件或目录直接拖到此处"));
    layout->addWidget(m_list, 1);

    auto *optRow = new QHBoxLayout;
    m_recursive = new QCheckBox(QStringLiteral("包含子目录中的 apk"), this);
    m_recursive->setChecked(true);
    auto *removeBtn = new QPushButton(QStringLiteral("移除选中"), this);
    m_summary = new QLabel(this);
    optRow->addWidget(m_recursive);
    optRow->addWidget(removeBtn);
    optRow->addStretch(1);
    optRow->addWidget(m_summary);
    layout->addLayout(optRow);

    auto *btnRow = new QHBoxLayout;
    m_startBtn = new QPushButton(QStringLiteral("开始安装"), this);
    m_cancelBtn = new QPushButton(QStringLiteral("取消"), this);
    btnRow->addStretch(1);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_cancelBtn);
    layout->addLayout(btnRow);

    connect(addBtn, &QPushButton::clicked, this, &ApkInstallDialog::addFromInput);
    connect(fileBtn, &QPushButton::clicked, this, &ApkInstallDialog::browseFiles);
    connect(dirBtn, &QPushButton::clicked, this, &ApkInstallDialog::browseDirs);
    connect(removeBtn, &QPushButton::clicked, this, &ApkInstallDialog::removeSelected);
    connect(m_startBtn, &QPushButton::clicked, this, &ApkInstallDialog::startInstall);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ApkInstallDialog::cancelInstall);
}

void ApkInstallDialog::addPaths(const QStringList &rawPaths)
{
    QStringList skipped;
    const QStringList apks = ApkPathUtils::expand(rawPaths, m_recursive->isChecked(), &skipped);
    QSet<QString> existing;
    for (int i = 0; i < m_list->count(); ++i) {
        existing.insert(m_list->item(i)->data(Qt::UserRole).toString());
    }
    int added = 0;
    for (const QString &apk : apks) {
        if (existing.contains(apk)) {
            continue;
        }
        auto *item = new QListWidgetItem(apk + QStringLiteral("  [等待]"));
        item->setData(Qt::UserRole, apk);
        m_list->addItem(item);
        ++added;
    }
    if (added > 0) {
        m_summary->setText(QStringLiteral("共 %1 个 apk").arg(m_list->count()));
    }
    if (!skipped.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("跳过无效路径"), skipped.join(QLatin1Char('\n')), QMessageBox::Ok);
    }
}

void ApkInstallDialog::addFromInput()
{
    const QString text = m_pathEdit->text();
    if (text.trimmed().isEmpty()) {
        return;
    }
    addPaths(QStringList() << text);
    m_pathEdit->clear();
}

void ApkInstallDialog::browseFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择要安装的 apk"), QString(),
                                                            QStringLiteral("APK 文件 (*.apk);;所有文件 (*.*)"));
    addPaths(files);
}

void ApkInstallDialog::browseDirs()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择包含 apk 的目录"));
    if (!dir.isEmpty()) {
        addPaths(QStringList() << dir);
    }
}

void ApkInstallDialog::removeSelected()
{
    if (m_busy) {
        return;
    }
    const QList<QListWidgetItem *> selected = m_list->selectedItems();
    for (QListWidgetItem *item : selected) {
        delete m_list->takeItem(m_list->row(item));
    }
    m_summary->setText(QStringLiteral("共 %1 个 apk").arg(m_list->count()));
}

void ApkInstallDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_startBtn->setEnabled(!busy && m_list->count() > 0);
    m_cancelBtn->setText(busy ? QStringLiteral("取消安装") : QStringLiteral("关闭"));
}

void ApkInstallDialog::startInstall()
{
    QStringList paths;
    for (int i = 0; i < m_list->count(); ++i) {
        const QString path = m_list->item(i)->data(Qt::UserRole).toString();
        paths.append(path);
        m_list->item(i)->setText(path + QStringLiteral("  [等待]"));
    }
    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("批量安装 APK"), QStringLiteral("请先添加 apk 文件或目录"),
                                 QMessageBox::Ok);
        return;
    }
    setBusy(true);
    m_summary->setText(QStringLiteral("正在安装 0/%1").arg(paths.size()));
    emit installRequested(paths);
}

void ApkInstallDialog::setItemStatus(const QString &path, const QString &status)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(Qt::UserRole).toString() == path) {
            item->setText(path + QStringLiteral("  [") + status + QStringLiteral("]"));
            m_list->scrollToItem(item);
            break;
        }
    }
}

void ApkInstallDialog::finish(int successCount, int failCount)
{
    setBusy(false);
    m_summary->setText(QStringLiteral("完成：成功 %1 / 失败 %2").arg(successCount).arg(failCount));
}

void ApkInstallDialog::cancelInstall()
{
    if (m_busy) {
        emit cancelled();
        finish(0, 0);
        m_summary->setText(QStringLiteral("已取消"));
    } else {
        close();
    }
}

void ApkInstallDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QDialog::dragEnterEvent(event);
    }
}

void ApkInstallDialog::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (!mime || !mime->hasUrls()) {
        QDialog::dropEvent(event);
        return;
    }
    QStringList paths;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    addPaths(paths);
    event->acceptProposedAction();
}

void ApkInstallDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        emit cancelled();
    }
    QDialog::closeEvent(event);
}
