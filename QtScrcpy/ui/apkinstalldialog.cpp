#include "apkinstalldialog.h"
#include "apkpathutils.h"

#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
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
    setAcceptDrops(true);
    resize(680, 480);

    auto *layout = new QVBoxLayout(this);
    m_hintLabel = new QLabel(this);
    layout->addWidget(m_hintLabel);

    auto *inputRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(this);
    m_addBtn = new QPushButton(this);
    m_fileBtn = new QPushButton(this);
    m_dirBtn = new QPushButton(this);
    inputRow->addWidget(m_pathEdit, 1);
    inputRow->addWidget(m_addBtn);
    inputRow->addWidget(m_fileBtn);
    inputRow->addWidget(m_dirBtn);
    layout->addLayout(inputRow);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(m_list, 1);

    auto *optRow = new QHBoxLayout;
    m_recursive = new QCheckBox(this);
    m_recursive->setChecked(true);
    m_removeBtn = new QPushButton(this);
    m_summary = new QLabel(this);
    optRow->addWidget(m_recursive);
    optRow->addWidget(m_removeBtn);
    optRow->addStretch(1);
    optRow->addWidget(m_summary);
    layout->addLayout(optRow);

    auto *btnRow = new QHBoxLayout;
    m_startBtn = new QPushButton(this);
    m_cancelBtn = new QPushButton(this);
    btnRow->addStretch(1);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_cancelBtn);
    layout->addLayout(btnRow);

    connect(m_addBtn, &QPushButton::clicked, this, &ApkInstallDialog::addFromInput);
    connect(m_fileBtn, &QPushButton::clicked, this, &ApkInstallDialog::browseFiles);
    connect(m_dirBtn, &QPushButton::clicked, this, &ApkInstallDialog::browseDirs);
    connect(m_removeBtn, &QPushButton::clicked, this, &ApkInstallDialog::removeSelected);
    connect(m_startBtn, &QPushButton::clicked, this, &ApkInstallDialog::startInstall);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ApkInstallDialog::cancelInstall);

    retranslateUi();
}

void ApkInstallDialog::retranslateUi()
{
    setWindowTitle(tr("Batch install APK"));
    m_hintLabel->setText(tr("Drop apk files or folders here, or enter a path (UNC network paths supported):"));
    m_pathEdit->setPlaceholderText(tr("e.g. C:\\apks\\app.apk  or  \\\\host\\share\\app.apk  or a folder"));
    m_addBtn->setText(tr("Add"));
    m_fileBtn->setText(tr("Choose files"));
    m_dirBtn->setText(tr("Choose folder"));
    m_list->setToolTip(tr("You can drag files or folders directly onto this list"));
    m_recursive->setText(tr("Include apk files in subfolders"));
    m_removeBtn->setText(tr("Remove selected"));
    m_startBtn->setText(tr("Start install"));
    m_cancelBtn->setText(m_busy ? tr("Cancel install") : tr("Close"));
    refreshSummary();
    refreshItemTexts();
}

void ApkInstallDialog::refreshSummary()
{
    switch (m_summaryMode) {
    case SummaryMode::Total:
        m_summary->setText(tr("%1 APK file(s)").arg(m_list->count()));
        break;
    case SummaryMode::Installing:
        m_summary->setText(tr("Installing 0/%1").arg(m_list->count()));
        break;
    case SummaryMode::Finished:
        m_summary->setText(tr("Done: %1 succeeded / %2 failed").arg(m_successCount).arg(m_failCount));
        break;
    case SummaryMode::Cancelled:
        m_summary->setText(tr("Cancelled"));
        break;
    case SummaryMode::None:
    default:
        m_summary->clear();
        break;
    }
}

void ApkInstallDialog::refreshItemTexts()
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const QString path = item->data(Qt::UserRole).toString();
        const QString status = item->data(Qt::UserRole + 1).toString();
        item->setText(path + QStringLiteral("  [") + status + QStringLiteral("]"));
    }
}

void ApkInstallDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
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
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, apk);
        item->setData(Qt::UserRole + 1, tr("Pending"));
        item->setText(apk + QStringLiteral("  [") + tr("Pending") + QStringLiteral("]"));
        m_list->addItem(item);
        ++added;
    }
    if (added > 0) {
        m_summaryMode = SummaryMode::Total;
        refreshSummary();
    }
    if (!skipped.isEmpty()) {
        QMessageBox::warning(this, tr("Skipped invalid paths"), skipped.join(QLatin1Char('\n')), QMessageBox::Ok);
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
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Choose APK files to install"), QString(),
        tr("APK files (*.apk);;All files (*.*)"));
    addPaths(files);
}

void ApkInstallDialog::browseDirs()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose folder containing APK files"));
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
    m_summaryMode = SummaryMode::Total;
    refreshSummary();
}

void ApkInstallDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_startBtn->setEnabled(!busy && m_list->count() > 0);
    m_cancelBtn->setText(busy ? tr("Cancel install") : tr("Close"));
}

void ApkInstallDialog::startInstall()
{
    QStringList paths;
    for (int i = 0; i < m_list->count(); ++i) {
        const QString path = m_list->item(i)->data(Qt::UserRole).toString();
        paths.append(path);
        m_list->item(i)->setData(Qt::UserRole + 1, tr("Pending"));
    }
    if (paths.isEmpty()) {
        QMessageBox::information(this, tr("Batch install APK"), tr("Please add APK files or folders first"),
                                 QMessageBox::Ok);
        return;
    }
    refreshItemTexts();
    setBusy(true);
    m_summaryMode = SummaryMode::Installing;
    refreshSummary();
    emit installRequested(paths);
}

void ApkInstallDialog::setItemStatus(const QString &path, const QString &status)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(Qt::UserRole).toString() == path) {
            item->setData(Qt::UserRole + 1, status);
            item->setText(path + QStringLiteral("  [") + status + QStringLiteral("]"));
            m_list->scrollToItem(item);
            break;
        }
    }
}

void ApkInstallDialog::finish(int successCount, int failCount)
{
    setBusy(false);
    m_successCount = successCount;
    m_failCount = failCount;
    m_summaryMode = SummaryMode::Finished;
    refreshSummary();
}

void ApkInstallDialog::cancelInstall()
{
    if (m_busy) {
        emit cancelled();
        finish(0, 0);
        m_summaryMode = SummaryMode::Cancelled;
        refreshSummary();
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
