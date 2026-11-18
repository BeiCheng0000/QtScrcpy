#include "groupcontrolwidget.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QMessageBox>

#include "apkinstalldialog.h"
#include "videoform.h"
#include "../util/config.h"

GroupControlWidget::GroupControlWidget(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_titleLabel = new QLabel(tr("Select target devices (checked = included in batch operations)"), this);
    m_titleLabel->setWordWrap(true);
    root->addWidget(m_titleLabel);

    m_deviceList = new QListWidget(this);
    m_deviceList->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(m_deviceList, 1);

    // 选择按钮行
    auto *selectRow = new QHBoxLayout();
    m_allBtn = new QPushButton(tr("Select all"), this);
    m_noneBtn = new QPushButton(tr("Select none"), this);
    m_invertBtn = new QPushButton(tr("Invert"), this);
    selectRow->addWidget(m_allBtn);
    selectRow->addWidget(m_noneBtn);
    selectRow->addWidget(m_invertBtn);
    selectRow->addStretch();
    root->addLayout(selectRow);

    // 主控设置
    m_hostBtn = new QPushButton(tr("Set checked device as control host"), this);
    root->addWidget(m_hostBtn);

    // 批量操作按钮
    m_actionTitle = new QLabel(tr("Batch device actions"), this);
    m_actionTitle->setStyleSheet("font-weight: bold;");
    root->addWidget(m_actionTitle);
    auto *actionRow = new QHBoxLayout();
    for (int kind = 0; kind < 6; ++kind) {
        auto *btn = new QPushButton(actionText(kind), this);
        btn->setProperty("kind", kind);
        m_actionBtns.append(btn);
        connect(btn, &QPushButton::clicked, this, &GroupControlWidget::onBatchAction);
        actionRow->addWidget(btn);
    }
    root->addLayout(actionRow);

    // 批量安装 APK（复用单设备批量安装对话框的文件选择逻辑）
    m_installBtn = new QPushButton(tr("Install APK on checked devices"), this);
    root->addWidget(m_installBtn);

    m_summary = new QLabel(tr("No connected devices"), this);
    root->addWidget(m_summary);

    m_log = new QTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText(tr("Batch operation log"));
    m_log->setMaximumHeight(140);
    root->addWidget(m_log);

    connect(m_allBtn, &QPushButton::clicked, this, &GroupControlWidget::onSelectAll);
    connect(m_noneBtn, &QPushButton::clicked, this, &GroupControlWidget::onSelectNone);
    connect(m_invertBtn, &QPushButton::clicked, this, &GroupControlWidget::onInvert);
    connect(m_hostBtn, &QPushButton::clicked, this, &GroupControlWidget::onSetHost);
    connect(m_installBtn, &QPushButton::clicked, this, &GroupControlWidget::onInstallApk);
}

GroupControlWidget::~GroupControlWidget() = default;

void GroupControlWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void GroupControlWidget::retranslateUi()
{
    m_titleLabel->setText(tr("Select target devices (checked = included in batch operations)"));
    m_allBtn->setText(tr("Select all"));
    m_noneBtn->setText(tr("Select none"));
    m_invertBtn->setText(tr("Invert"));
    m_hostBtn->setText(tr("Set checked device as control host"));
    m_actionTitle->setText(tr("Batch device actions"));
    for (int i = 0; i < m_actionBtns.size() && i < 6; ++i) {
        m_actionBtns.at(i)->setText(actionText(i));
    }
    m_installBtn->setText(tr("Install APK on checked devices"));
    m_log->setPlaceholderText(tr("Batch operation log"));
    m_summary->setText(tr("%1 connected device(s), %2 checked")
                           .arg(m_deviceList->count()).arg(checkedSerials().size()));
}

QString GroupControlWidget::actionText(int kind) const
{
    switch (kind) {
    case 0: return tr("Back");
    case 1: return tr("Home");
    case 2: return tr("Menu");
    case 3: return tr("Power");
    case 4: return tr("Rotate");
    case 5: return tr("Screenshot");
    default: return QString();
    }
}

QString GroupControlWidget::displayName(const QString &serial) const
{
    const QString nick = Config::getInstance().getNickName(serial);
    return nick.isEmpty() ? serial : (nick + " - " + serial);
}

void GroupControlWidget::syncDevices(const QStringList &serials)
{
    // 保存旧勾选状态
    QSet<QString> checked;
    for (int i = 0; i < m_deviceList->count(); ++i) {
        QListWidgetItem *item = m_deviceList->item(i);
        if (item->checkState() == Qt::Checked) {
            checked.insert(item->data(Qt::UserRole).toString());
        }
    }
    m_deviceList->clear();
    for (const QString &serial : serials) {
        auto *item = new QListWidgetItem(displayName(serial), m_deviceList);
        item->setData(Qt::UserRole, serial);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(serial) ? Qt::Checked : Qt::Unchecked);
    }
    m_summary->setText(tr("%1 connected device(s), %2 checked").arg(serials.size()).arg(checkedSerials().size()));
}

void GroupControlWidget::deviceDisconnected(const QString &serial)
{
    for (int i = m_deviceList->count() - 1; i >= 0; --i) {
        if (m_deviceList->item(i)->data(Qt::UserRole).toString() == serial) {
            delete m_deviceList->takeItem(i);
        }
    }
    for (int i = m_installJobs.size() - 1; i >= 0; --i) {
        if (m_installJobs.at(i).serial == serial) {
            m_installJobs.removeAt(i);
        }
    }
    m_summary->setText(tr("%1 connected device(s), %2 checked").arg(m_deviceList->count()).arg(checkedSerials().size()));
}

QStringList GroupControlWidget::checkedSerials() const
{
    QStringList result;
    for (int i = 0; i < m_deviceList->count(); ++i) {
        QListWidgetItem *item = m_deviceList->item(i);
        if (item->checkState() == Qt::Checked) {
            result.append(item->data(Qt::UserRole).toString());
        }
    }
    return result;
}

void GroupControlWidget::log(const QString &line)
{
    m_log->append(line);
}

void GroupControlWidget::onSelectAll()
{
    for (int i = 0; i < m_deviceList->count(); ++i) {
        m_deviceList->item(i)->setCheckState(Qt::Checked);
    }
    m_summary->setText(tr("%1 connected device(s), %2 checked").arg(m_deviceList->count()).arg(checkedSerials().size()));
}

void GroupControlWidget::onSelectNone()
{
    for (int i = 0; i < m_deviceList->count(); ++i) {
        m_deviceList->item(i)->setCheckState(Qt::Unchecked);
    }
    m_summary->setText(tr("%1 connected device(s), %2 checked").arg(m_deviceList->count()).arg(0));
}

void GroupControlWidget::onInvert()
{
    for (int i = 0; i < m_deviceList->count(); ++i) {
        QListWidgetItem *item = m_deviceList->item(i);
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
    m_summary->setText(tr("%1 connected device(s), %2 checked").arg(m_deviceList->count()).arg(checkedSerials().size()));
}

void GroupControlWidget::onSetHost()
{
    const QStringList serials = checkedSerials();
    if (serials.isEmpty()) {
        QMessageBox::information(this, tr("Group control"), tr("No device checked"));
        return;
    }
    // 第一台勾选项为主控，其余取消主控（输入广播只由主控窗口触发）
    for (const QString &serial : serials) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) {
            continue;
        }
        auto *form = static_cast<VideoForm *>(device->getUserData());
        if (form) {
            form->setHost(serial == serials.first());
        }
    }
    log(tr("Set host: %1").arg(serials.first()));
}

void GroupControlWidget::onBatchAction()
{
    const int kind = sender()->property("kind").toInt();
    const QStringList serials = checkedSerials();
    if (serials.isEmpty()) {
        QMessageBox::information(this, tr("Group control"), tr("No device checked"));
        return;
    }
    int ok = 0;
    for (const QString &serial : serials) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) {
            continue;
        }
        switch (kind) {
        case 0: device->postGoBack(); break;
        case 1: device->postGoHome(); break;
        case 2: device->postGoMenu(); break;
        case 3: device->postPower(); break;
        case 4: device->rotateDevice(); break;
        case 5: device->screenshot(); break;
        default: break;
        }
        ++ok;
    }
    log(tr("%1 sent to %2 device(s)").arg(actionText(qBound(0, kind, 5))).arg(ok));
}

void GroupControlWidget::onInstallApk()
{
    const QStringList serials = checkedSerials();
    if (serials.isEmpty()) {
        QMessageBox::information(this, tr("Install APK"), tr("No device checked"));
        return;
    }
    m_installDlg = new ApkInstallDialog(this);
    m_installDlg->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_installDlg, &ApkInstallDialog::installRequested, this, &GroupControlWidget::onApkRequested);
    connect(m_installDlg, &ApkInstallDialog::cancelled, this, &GroupControlWidget::onApkCancelled);
    m_installDlg->show();
}

void GroupControlWidget::onApkRequested(const QStringList &paths)
{
    const QStringList serials = checkedSerials();
    for (const QString &serial : serials) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) {
            log(tr("[%1] device disconnected, skipped").arg(serial));
            continue;
        }
        auto *form = static_cast<VideoForm *>(device->getUserData());
        if (!form) {
            continue;
        }
        InstallJob job;
        job.serial = serial;
        job.paths = paths;
        m_installJobs.append(job);

        // 每台设备的队列独立，设备间并行；先断开旧的进度连接防止重复安装串信号
        disconnect(form, &VideoForm::apkInstallProgress, this, nullptr);
        disconnect(form, &VideoForm::apkInstallFinished, this, nullptr);
        connect(form, &VideoForm::apkInstallProgress, this,
                [this, serial](const QString &path, const QString &status) {
            onApkProgress(serial, path, status);
            if (m_installDlg) {
                m_installDlg->setItemStatus(path, status);
            }
        });
        connect(form, &VideoForm::apkInstallFinished, this,
                [this, serial](int ok, int fail) {
            onApkFinished(serial, ok, fail);
            // 对话框按最后完成的设备汇总（每设备完成时各调用一次 finish）
            if (m_installDlg) {
                m_installDlg->finish(ok, fail);
            }
        });
        form->installApkBatch(paths);
        log(tr("[%1] queued %2 apk(s)").arg(serial).arg(paths.size()));
    }
}

void GroupControlWidget::onApkCancelled()
{
    for (const QString &serial : checkedSerials()) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) {
            continue;
        }
        auto *form = static_cast<VideoForm *>(device->getUserData());
        if (form) {
            form->cancelInstallBatch();
        }
    }
    log(tr("Install cancelled"));
}

void GroupControlWidget::onApkProgress(const QString &serial, const QString &path, const QString &status)
{
    log(tr("[%1] %2: %3").arg(serial).arg(path.section('/', -1)).arg(status));
}

void GroupControlWidget::onApkFinished(const QString &serial, int successCount, int failCount)
{
    for (int i = m_installJobs.size() - 1; i >= 0; --i) {
        if (m_installJobs.at(i).serial == serial) {
            m_installJobs.removeAt(i);
            break;
        }
    }
    log(tr("[%1] finished: %2 success, %3 failed").arg(serial).arg(successCount).arg(failCount));
}
