#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHideEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include "adbprocess.h"
#include "iconhelper.h"
#include "apkinstalldialog.h"
#include "apkmanagerwindow.h"
#include "deviceloggrabber.h"
#include "toolform.h"
#include "ui_toolform.h"
#include "videoform.h"
#include "../groupcontroller/groupcontroller.h"

ToolForm::ToolForm(QWidget *adsorbWidget, AdsorbPositions adsorbPos) : MagneticWidget(adsorbWidget, adsorbPos), ui(new Ui::ToolForm)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    //setWindowFlags(windowFlags() & ~Qt::WindowMinMaxButtonsHint);

    // updateButtonSpacing() 会在工具栏较高时增大布局间距。默认的 SizeConstraint
    // 会撑大布局的最小尺寸，导致视频窗口还原后工具栏无法缩回。因此强制由
    // VideoForm 完全控制工具栏尺寸，不使用布局自带的尺寸约束。
    layout()->setSizeConstraint(QLayout::SetNoConstraint);

    updateGroupControl();

    initStyle();
    updateButtonSpacing();

    m_recordTimer.setInterval(1000);
    connect(&m_recordTimer, &QTimer::timeout, this, &ToolForm::updateRecordUi);
    if (VideoForm *form = qobject_cast<VideoForm *>(parent())) {
        connect(form, &VideoForm::recordingStateChanged, this, &ToolForm::updateRecordUi);
        // 录制计时对齐到视频 0 点（首帧写入录制器的时刻）：
        // 点击按钮→首帧之间的空窗不计入显示时长，与生成视频的内容时长一致
        connect(form, &VideoForm::recordingClockStarted, this, [this]() { m_recordElapsed.restart(); });
    }

    createSettingsMenu();
    ui->expandSettingsBtn->installEventFilter(this);

    // 使用非模态浮层而非 QMenu，这样点击主设置按钮仍能直接进入系统设置页，
    // 而悬停时才展开更多子项；定时器在指针离开按钮与浮层两个区域后自动关闭。
    m_settingsMenuTimer.setInterval(150);
    connect(&m_settingsMenuTimer, &QTimer::timeout, this, [this]() {
        if (!m_settingsMenu || !m_settingsMenu->isVisible()) {
            m_settingsMenuTimer.stop();
            return;
        }

        const QPoint cursor = QCursor::pos();
        QRect keep(ui->expandSettingsBtn->mapToGlobal(QPoint(0, 0)), ui->expandSettingsBtn->size());
        keep = keep.united(QRect(m_settingsMenu->pos(), m_settingsMenu->size()));
        keep.adjust(-8, -8, 8, 8);
        if (!keep.contains(cursor)) {
            hideSettingsMenu();
        }
    });

    m_toastTimer.setSingleShot(true);
    connect(&m_toastTimer, &QTimer::timeout, this, [this]() {
        if (m_toast) {
            m_toast->hide();
        }
    });

    // 设备日志抓取器：状态变化驱动菜单文案与齿轮红点；结果经通用 toast 提示。
    // 本窗口销毁时 Grabber 析构自动结束抓取会话并落盘。
    m_logGrabber = new DeviceLogGrabber(this);
    connect(m_logGrabber, &DeviceLogGrabber::stateChanged, this, &ToolForm::updateLogGrabUi);
    connect(m_logGrabber, &DeviceLogGrabber::message, this, &ToolForm::showToastText);
    connect(m_logGrabber, &DeviceLogGrabber::captureFinished, this, &ToolForm::onDeviceLogCaptured);

    // 齿轮按钮右上角“抓取中”红点（仅状态提示，不接收鼠标事件）
    m_grabDot = new QLabel(ui->expandSettingsBtn);
    m_grabDot->setStyleSheet(QStringLiteral("background:#e53935; border-radius:4px;"));
    m_grabDot->setFixedSize(8, 8);
    m_grabDot->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_grabDot->hide();
}

ToolForm::~ToolForm()
{
    delete ui;
}

void ToolForm::setSerial(const QString &serial)
{
    m_serial = serial;
    updateCameraMode();
}

bool ToolForm::isHost()
{
    return m_isHost;
}

void ToolForm::setHost(bool host)
{
    if (m_isHost == host) {
        return;
    }
    m_isHost = host;
    updateGroupControl(); // 同步按钮颜色与 GroupController 的 observer 注册
}

void ToolForm::updateCameraMode()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    const bool camera = device && device->isCameraMode();

    ui->groupControlBtn->setVisible(!camera);
    ui->expandNotifyBtn->setVisible(!camera);
    ui->expandSettingsBtn->setVisible(!camera);
    if (camera) {
        hideSettingsMenu();
    }
    ui->rotateBtn->setVisible(!camera);
    ui->touchBtn->setVisible(!camera);
    ui->openScreenBtn->setVisible(!camera);
    ui->closeScreenBtn->setVisible(!camera);
    ui->powerBtn->setVisible(!camera);
    ui->volumeUpBtn->setVisible(!camera);
    ui->volumeDownBtn->setVisible(!camera);
    ui->appSwitchBtn->setVisible(!camera);
    ui->menuBtn->setVisible(!camera);
    ui->homeBtn->setVisible(!camera);
    ui->returnBtn->setVisible(!camera);
    ui->cameraTorchBtn->setVisible(camera);
    ui->cameraZoomOutBtn->setVisible(camera);
    ui->cameraZoomInBtn->setVisible(camera);
    updateButtonSpacing();
}

void ToolForm::initStyle()
{
    IconHelper::Instance()->SetIcon(ui->fullScreenBtn, QChar(0xf0b2), 15);
    IconHelper::Instance()->SetIcon(ui->menuBtn, QChar(0xf096), 15);
    IconHelper::Instance()->SetIcon(ui->homeBtn, QChar(0xf1db), 15);
    //IconHelper::Instance()->SetIcon(ui->returnBtn, QChar(0xf104), 15);
    IconHelper::Instance()->SetIcon(ui->returnBtn, QChar(0xf053), 15);
    IconHelper::Instance()->SetIcon(ui->appSwitchBtn, QChar(0xf24d), 15);
    IconHelper::Instance()->SetIcon(ui->volumeUpBtn, QChar(0xf028), 15);
    IconHelper::Instance()->SetIcon(ui->volumeDownBtn, QChar(0xf027), 15);
    IconHelper::Instance()->SetIcon(ui->openScreenBtn, QChar(0xf06e), 15);
    IconHelper::Instance()->SetIcon(ui->closeScreenBtn, QChar(0xf070), 15);
    IconHelper::Instance()->SetIcon(ui->powerBtn, QChar(0xf011), 15);
    IconHelper::Instance()->SetIcon(ui->expandNotifyBtn, QChar(0xf103), 15);
    IconHelper::Instance()->SetIcon(ui->expandSettingsBtn, QChar(0xf013), 15);
    IconHelper::Instance()->SetIcon(ui->rotateBtn, QChar(0xf021), 15);
    IconHelper::Instance()->SetIcon(ui->screenShotBtn, QChar(0xf0c4), 15);
    IconHelper::Instance()->SetIcon(ui->recordBtn, QChar(0xf03d), 15);
    IconHelper::Instance()->SetIcon(ui->touchBtn, QChar(0xf111), 15);
    IconHelper::Instance()->SetIcon(ui->groupControlBtn, QChar(0xf0c0), 15);
    IconHelper::Instance()->SetIcon(ui->cameraTorchBtn, QChar(0xf0eb), 15);
    IconHelper::Instance()->SetIcon(ui->cameraZoomOutBtn, QChar(0xf010), 15);
    IconHelper::Instance()->SetIcon(ui->cameraZoomInBtn, QChar(0xf00e), 15);
}

void ToolForm::updateGroupControl()
{
    if (m_isHost) {
        ui->groupControlBtn->setStyleSheet("color: red");
    } else {
        ui->groupControlBtn->setStyleSheet("color: green");
    }

    GroupController::instance().updateDeviceState(m_serial);
}

void ToolForm::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        m_dragPosition = event->globalPos() - frameGeometry().topLeft();
#else
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
#endif
        event->accept();
    }
}

void ToolForm::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
}

void ToolForm::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        move(event->globalPos() - m_dragPosition);
#else
        move(event->globalPosition().toPoint() - m_dragPosition);
#endif
        event->accept();
    }
}

void ToolForm::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this); // 运行时切换语言
        retranslateDynamicUi();
    }
}

void ToolForm::retranslateDynamicUi()
{
    // 设置菜单按钮由代码创建，Designer 无法自动刷新
    const QString texts[] = { tr("Open all apps"), tr("About this device"), tr("Open network panel"),
                              tr("Install apk") };
    for (int i = 0; i < m_settingsMenuButtons.size() && i < 4; ++i) {
        m_settingsMenuButtons.at(i)->setText(texts[i]);
    }
    // 设备日志入口（抓取项文案随状态切换）
    if (m_clearLogBtn) {
        m_clearLogBtn->setText(tr("清空设备日志"));
    }
    if (m_grabLogBtn) {
        const bool grabbing = m_logGrabber && m_logGrabber->isGrabbing();
        m_grabLogBtn->setText(grabbing ? tr("停止抓取设备日志") : tr("开始抓取设备日志"));
    }
    if (m_switchEnvBtn) {
        m_switchEnvBtn->setText(tr("切换环境"));
    }
    if (m_apkManagerBtn) {
        m_apkManagerBtn->setText(tr("APK管理"));
    }
    if (m_settingsMenu && m_settingsMenu->isVisible()) {
        m_settingsMenu->adjustSize();
    }
    // 截图提示是临时 label，若当前正显示则同步更新
    if (m_toast && m_toast->isVisible()) {
        m_toast->setText(tr("Screenshot copied, ready to paste"));
        m_toast->adjustSize();
    }
}

void ToolForm::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    qDebug() << "show event";
}

void ToolForm::hideEvent(QHideEvent *event)
{
    Q_UNUSED(event)
    hideSettingsMenu();
    if (m_toast) {
        m_toast->hide();
    }
    qDebug() << "hide event";
}

bool ToolForm::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->expandSettingsBtn && event && QEvent::Enter == event->type()) {
        showSettingsMenu();
    }
    return MagneticWidget::eventFilter(watched, event);
}

void ToolForm::createSettingsMenu()
{
    m_settingsMenu = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint);
    m_settingsMenu->setObjectName(QStringLiteral("settingsMenu"));
    m_settingsMenu->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_settingsMenu->setStyleSheet(R"(
        QWidget#settingsMenu {
            background: #2b2b2b;
            border: 1px solid #5a5a5a;
            border-radius: 4px;
        }
        QWidget#settingsMenu QPushButton {
            color: #f0f0f0;
            background: transparent;
            border: none;
            text-align: left;
            padding: 8px 14px;
            min-width: 108px;
            font-size: 12px;
        }
        QWidget#settingsMenu QPushButton:hover {
            background: #3d7fbf;
            border-radius: 3px;
        }
    )");

    QVBoxLayout *layout = new QVBoxLayout(m_settingsMenu);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    struct Entry
    {
        const char *action;
    };
    const Entry entries[] = {
        { "android.settings.APPLICATION_SETTINGS" },
        { "android.settings.DEVICE_INFO_SETTINGS" },
        { "android.settings.WIRELESS_SETTINGS" },
        { nullptr }, // action 为空 -> 打开批量安装对话框
    };
    const QString texts[] = { tr("Open all apps"), tr("About this device"), tr("Open network panel"),
                              tr("Install apk") };
    m_settingsMenuButtons.clear();
    for (int i = 0; i < 4; ++i) {
        const Entry &entry = entries[i];
        QPushButton *button = new QPushButton(texts[i], m_settingsMenu);
        m_settingsMenuButtons.append(button);
        button->setCursor(Qt::PointingHandCursor);
        if (entry.action) {
            const QString action = QString::fromUtf8(entry.action);
            connect(button, &QPushButton::clicked, this, [this, action]() {
                hideSettingsMenu();
                startActivity(action);
            });
        } else {
            connect(button, &QPushButton::clicked, this, [this]() {
                hideSettingsMenu();
                auto *dlg = new ApkInstallDialog(this);
                dlg->setAttribute(Qt::WA_DeleteOnClose, true);
                if (VideoForm *form = qobject_cast<VideoForm *>(parent())) {
                    connect(dlg, &ApkInstallDialog::installRequested, form, &VideoForm::installApkBatch);
                    connect(form, &VideoForm::apkInstallProgress, dlg, &ApkInstallDialog::setItemStatus);
                    connect(form, &VideoForm::apkInstallFinished, dlg, &ApkInstallDialog::finish);
                    connect(dlg, &ApkInstallDialog::cancelled, form, &VideoForm::cancelInstallBatch);
                }
                dlg->show();
            });
        }
        layout->addWidget(button);
    }

    // 分隔线 + 设备日志操作区：清空 / 抓取开始-停止（同一入口按状态切换文案）
    auto *separator = new QFrame(m_settingsMenu);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet(QStringLiteral("color: #5a5a5a;"));
    layout->addWidget(separator);

    m_clearLogBtn = new QPushButton(tr("清空设备日志"), m_settingsMenu);
    m_clearLogBtn->setCursor(Qt::PointingHandCursor);
    connect(m_clearLogBtn, &QPushButton::clicked, this, [this]() {
        hideSettingsMenu();
        if (m_logGrabber) {
            m_logGrabber->clear(m_serial);
        }
    });
    layout->addWidget(m_clearLogBtn);

    m_grabLogBtn = new QPushButton(tr("开始抓取设备日志"), m_settingsMenu);
    m_grabLogBtn->setCursor(Qt::PointingHandCursor);
    connect(m_grabLogBtn, &QPushButton::clicked, this, [this]() {
        hideSettingsMenu();
        if (!m_logGrabber) {
            return;
        }
        if (m_logGrabber->isGrabbing()) {
            m_logGrabber->stop();
        } else {
            m_logGrabber->start(m_serial);
        }
    });
    layout->addWidget(m_grabLogBtn);

    // 分隔线 + 切换环境：点击后在按钮处弹出环境下拉菜单（正式/生产、灰度、测试、开发）
    auto *envSeparator = new QFrame(m_settingsMenu);
    envSeparator->setFrameShape(QFrame::HLine);
    envSeparator->setFrameShadow(QFrame::Sunken);
    envSeparator->setStyleSheet(QStringLiteral("color: #5a5a5a;"));
    layout->addWidget(envSeparator);

    m_switchEnvBtn = new QPushButton(tr("切换环境"), m_settingsMenu);
    m_switchEnvBtn->setCursor(Qt::PointingHandCursor);
    connect(m_switchEnvBtn, &QPushButton::clicked, this, &ToolForm::showEnvironmentMenu);
    layout->addWidget(m_switchEnvBtn);

    m_apkManagerBtn = new QPushButton(tr("APK管理"), m_settingsMenu);
    m_apkManagerBtn->setCursor(Qt::PointingHandCursor);
    connect(m_apkManagerBtn, &QPushButton::clicked, this, &ToolForm::openApkManager);
    layout->addWidget(m_apkManagerBtn);

    m_settingsMenu->adjustSize();
}

void ToolForm::showSettingsMenu()
{
    if (!m_settingsMenu || !ui->expandSettingsBtn->isVisible()) {
        return;
    }

    const QPoint topLeft = ui->expandSettingsBtn->mapToGlobal(QPoint(0, 0));
    m_settingsMenu->adjustSize();

    QRect screen;
    if (QScreen *primary = QGuiApplication::primaryScreen()) {
        screen = primary->availableGeometry();
    }

    int x = topLeft.x() - m_settingsMenu->width() - 6;
    int y = topLeft.y();
    if (!screen.isEmpty()) {
        if (x < screen.left()) {
            x = topLeft.x() + ui->expandSettingsBtn->width() + 6;
        }
        x = qBound(screen.left(), x, screen.right() - m_settingsMenu->width() + 1);
        y = qBound(screen.top(), y, screen.bottom() - m_settingsMenu->height() + 1);
    }

    m_settingsMenu->move(x, y);
    m_settingsMenu->show();
    m_settingsMenu->raise();
    if (!m_settingsMenuTimer.isActive()) {
        m_settingsMenuTimer.start();
    }
}

void ToolForm::hideSettingsMenu()
{
    m_settingsMenuTimer.stop();
    if (m_settingsMenu) {
        m_settingsMenu->hide();
    }
}

void ToolForm::openApkManager()
{
    hideSettingsMenu();
    if (m_serial.isEmpty()) {
        showToastText(tr("未连接设备，无法管理 APK"));
        return;
    }
    if (m_apkManager.isNull()) {
        // parent 设为投屏窗口（ToolForm 的 parent 即 VideoForm），保证窗口层级绑定；
        // ApkManagerWindow 内部会设为 Qt::Tool + 置顶，始终悬浮于投屏页面之上。
        m_apkManager = new ApkManagerWindow(m_serial, parentWidget());
        m_apkManager->setAttribute(Qt::WA_DeleteOnClose, true);
    }
    m_apkManager->showNormal();
    m_apkManager->raise();
    m_apkManager->activateWindow();
}

// 环境切换：在“切换环境”按钮位置弹出下拉菜单，选择后执行对应切换。
// 四个环境与 切换环境v1.5.bat 一致：
//   正式/生产 -> 删除 /sdcard/config.ini（恢复默认）
//   灰度/测试/开发 -> 写 gray/test/dev 到临时 config.ini 并 push 到 /sdcard/config.ini
// 切换完成后统一查询并清除所有包名含 com.jxw 的应用缓存。
void ToolForm::showEnvironmentMenu()
{
    hideSettingsMenu();
    if (m_serial.isEmpty()) {
        showToastText(tr("未连接设备，无法切换环境"));
        return;
    }

    // 用字面量 tr(...) 以便 lupdate 抽取；value 为空表示删除配置（正式/生产）
    struct EnvItem
    {
        QString value;
        QString label;
    };
    const QList<EnvItem> items = {
        { QString(), tr("正式/生产") },
        { QStringLiteral("gray"), tr("灰度") },
        { QStringLiteral("test"), tr("测试") },
        { QStringLiteral("dev"), tr("开发") },
    };

    QMenu menu;
    menu.setObjectName(QStringLiteral("envMenu"));
    menu.setStyleSheet(R"(
        QMenu#envMenu {
            background: #2b2b2b;
            border: 1px solid #5a5a5a;
        }
        QMenu#envMenu::item { color: #f0f0f0; padding: 6px 22px 6px 14px; }
        QMenu#envMenu::item:selected { background: #3d7fbf; }
    )");
    for (const EnvItem &item : items) {
        QAction *action = menu.addAction(item.label);
        action->setData(QVariant(item.value));
    }

    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen) {
        return;
    }

    const QString value = chosen->data().toString();
    const QString label = chosen->text();
    switchEnvironment(value, label);
}

void ToolForm::switchEnvironment(const QString &envKey, const QString &envLabel)
{
    if (m_envSwitching) {
        showToastText(tr("环境切换进行中，请稍候"));
        return;
    }
    m_envSwitching = true;
    showToastText(tr("正在切换到「%1」环境…").arg(envLabel));

    QPointer<ToolForm> guard = this;
    auto switchDone = [this, guard, envLabel](bool ok, bool timedOut, const QString &out) {
        if (!guard) {
            return;
        }
        if (timedOut || !ok) {
            m_envSwitching = false;
            const QString detail = out.trimmed();
            QString msg = tr("切换「%1」环境失败").arg(envLabel);
            if (!detail.isEmpty()) {
                msg += QStringLiteral("：") + detail;
            }
            showToastText(msg);
            return;
        }
        // 配置已就位，查询所有包名含 com.jxw 的应用后逐个清缓存
        adbExecAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("pm list packages com.jxw"),
                     15000,
                     [this, guard, envLabel](bool listOk, bool listTimeout, const QString &listOut) {
                         if (!guard) {
                             return;
                         }
                         QStringList packages;
                         if (listOk && !listTimeout) {
                             for (const QString &rawLine : listOut.split('\n')) {
                                 const QString line = rawLine.trimmed();
                                 if (line.startsWith(QStringLiteral("package:"))) {
                                     packages.append(line.mid(QStringLiteral("package:").size()).trimmed());
                                 }
                             }
                         }
                         if (packages.isEmpty()) {
                             m_envSwitching = false;
                             showToastText(tr("已切换到「%1」环境，未找到需清缓存的 com.jxw 应用").arg(envLabel));
                             return;
                         }
                         clearJxwPackages(0, packages, 0, 0);
                     });
    };

    if (envKey.isEmpty()) {
        // 正式/生产：删除配置文件。rm 对不存在文件返回非 0，用 "rm -f" 保证幂等成功。
        adbExecAsync(QStringList() << QStringLiteral("shell") << QStringLiteral("rm -f /sdcard/config.ini"),
                     15000, switchDone);
    } else {
        // 灰度/测试/开发：写本地临时 config.ini（无换行，与 bat 的 set /p 一致）后 push。
        QString safeSerial = m_serial;
        safeSerial.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
        const QString localPath = QDir::tempPath() + QStringLiteral("/qtscrcpy_env_config_")
                                  + safeSerial + QStringLiteral(".ini");
        QFile file(localPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_envSwitching = false;
            showToastText(tr("切换环境失败：无法写入临时配置文件"));
            return;
        }
        const QByteArray content = envKey.toUtf8();
        const bool written = file.write(content) == content.size();
        file.close();
        if (!written) {
            m_envSwitching = false;
            showToastText(tr("切换环境失败：无法写入临时配置文件"));
            return;
        }
        QPointer<ToolForm> selfGuard = this;
        adbExecAsync(QStringList() << QStringLiteral("push") << localPath << QStringLiteral("/sdcard/config.ini"),
                     20000,
                     [this, selfGuard, localPath, switchDone](bool ok, bool timedOut, const QString &out) {
                         QFile::remove(localPath); // 无论成败都清理临时文件
                         if (!selfGuard) {
                             return;
                         }
                         switchDone(ok, timedOut, out);
                     });
    }
}

void ToolForm::clearJxwPackages(int index, const QStringList &packages, int cleared, int failed)
{
    if (index >= packages.size()) {
        m_envSwitching = false;
        QString msg = tr("环境切换完成：已清除 %1 个 com.jxw 应用缓存").arg(cleared);
        if (failed > 0) {
            msg += tr("，%1 个失败").arg(failed);
        }
        showToastText(msg);
        return;
    }

    const QString package = packages.at(index);
    QPointer<ToolForm> guard = this;
    adbExecAsync(QStringList() << QStringLiteral("shell") << (QStringLiteral("pm clear ") + package), 15000,
                 [this, guard, packages, index, cleared, failed](bool ok, bool timedOut, const QString &out) {
                     if (!guard) {
                         return;
                     }
                     // pm clear 成功输出 "Success"；部分 ROM 退出码为 0 却失败，故以输出为准
                     const bool success = ok && !timedOut
                                          && out.contains(QStringLiteral("Success"), Qt::CaseInsensitive);
                     clearJxwPackages(index + 1, packages, cleared + (success ? 1 : 0), failed + (success ? 0 : 1));
                 });
}

void ToolForm::adbExecAsync(const QStringList &args, int timeoutMs,
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

void ToolForm::startActivity(const QString &action)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        device->startActivity(action);
    }
}

void ToolForm::showScreenshotHint()
{
    if (!m_toast) {
        m_toast = new QLabel(this, Qt::Tool | Qt::FramelessWindowHint);
        m_toast->setAttribute(Qt::WA_ShowWithoutActivating, true);
        m_toast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_toast->setAlignment(Qt::AlignCenter);
        m_toast->setStyleSheet(R"(QLabel {
            color: #ffffff;
            background: #2b2b2b;
            border: 1px solid #5a5a5a;
            border-radius: 4px;
            padding: 8px 14px;
            font-size: 13px;
        })");
    }

    m_toast->setText(tr("Screenshot copied, ready to paste"));
    m_toast->adjustSize();

    const QPoint anchor = ui->screenShotBtn->mapToGlobal(QPoint(0, 0));
    int x = anchor.x() - m_toast->width() - 8;
    int y = anchor.y() + (ui->screenShotBtn->height() - m_toast->height()) / 2;
    if (QScreen *primary = QGuiApplication::primaryScreen()) {
        const QRect screen = primary->availableGeometry();
        x = qBound(screen.left(), x, screen.right() - m_toast->width() + 1);
        y = qBound(screen.top(), y, screen.bottom() - m_toast->height() + 1);
    }
    m_toast->move(x, y);
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(2200);
}

void ToolForm::showToastText(const QString &text)
{
    if (!m_toast) {
        m_toast = new QLabel(this, Qt::Tool | Qt::FramelessWindowHint);
        m_toast->setAttribute(Qt::WA_ShowWithoutActivating, true);
        m_toast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_toast->setAlignment(Qt::AlignCenter);
        m_toast->setStyleSheet(R"(QLabel {
            color: #ffffff;
            background: #2b2b2b;
            border: 1px solid #5a5a5a;
            border-radius: 4px;
            padding: 8px 14px;
            font-size: 13px;
        })");
    }

    m_toast->setText(text);
    m_toast->adjustSize();

    const QPoint anchor = ui->expandSettingsBtn->mapToGlobal(QPoint(0, 0));
    int x = anchor.x() - m_toast->width() - 8;
    int y = anchor.y() + (ui->expandSettingsBtn->height() - m_toast->height()) / 2;
    if (QScreen *primary = QGuiApplication::primaryScreen()) {
        const QRect screen = primary->availableGeometry();
        x = qBound(screen.left(), x, screen.right() - m_toast->width() + 1);
        y = qBound(screen.top(), y, screen.bottom() - m_toast->height() + 1);
    }
    m_toast->move(x, y);
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(2600);
}

void ToolForm::onDeviceLogCaptured(const QString &path, bool error)
{
    // 与录制视频结束弹窗同款：保存路径 + 文件大小 + 打开文件夹定位 + 确认
    QFileInfo fileInfo(path);
    QMessageBox box(this);
    box.setIcon(error ? QMessageBox::Warning : QMessageBox::Information);
    box.setWindowTitle(tr("设备日志"));
    box.setText(error ? tr("日志抓取被中断，已保存到：\n%1").arg(QDir::toNativeSeparators(path))
                      : tr("已保存到：\n%1").arg(QDir::toNativeSeparators(path)));
    box.setInformativeText(tr("大小：%1 KB").arg(fileInfo.size() / 1024.0, 0, 'f', 1));
    QPushButton *openButton = box.addButton(tr("打开文件夹"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Ok);
    box.exec();
    if (box.clickedButton() == openButton) {
        openLogFileFolder(path);
    }
}

void ToolForm::openLogFileFolder(const QString &filePath) const
{
    // 与录制完成"打开文件夹"一致：优先资源管理器 /select 定位到文件本身
    const QString dir = QFileInfo(filePath).absolutePath();
#if defined(Q_OS_WIN32)
    if (!QProcess::startDetached(QStringLiteral("explorer.exe"),
                                 QStringList{QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(filePath))})) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
#elif defined(Q_OS_MACOS)
    if (!QProcess::startDetached(QStringLiteral("open"), QStringList{QStringLiteral("-R"), filePath})) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}

void ToolForm::updateLogGrabUi(bool grabbing)
{
    if (m_grabLogBtn) {
        m_grabLogBtn->setText(grabbing ? tr("停止抓取设备日志") : tr("开始抓取设备日志"));
    }
    if (m_grabDot) {
        m_grabDot->setVisible(grabbing);
        if (grabbing) {
            placeGrabDot();
        }
    }
    if (m_settingsMenu && m_settingsMenu->isVisible()) {
        m_settingsMenu->adjustSize();
    }
}

void ToolForm::placeGrabDot()
{
    if (m_grabDot && ui->expandSettingsBtn) {
        // 齿轮按钮右上角悬出一点，不遮挡图标
        m_grabDot->move(ui->expandSettingsBtn->width() - 10, 4);
        m_grabDot->raise();
    }
}

void ToolForm::on_fullScreenBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }

    dynamic_cast<VideoForm*>(parent())->switchFullScreen();
}

void ToolForm::on_returnBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postGoBack();
}

void ToolForm::on_homeBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postGoHome();
}

void ToolForm::on_menuBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postGoMenu();
}

void ToolForm::on_appSwitchBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postAppSwitch();
}

void ToolForm::on_powerBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postPower();
}

void ToolForm::on_screenShotBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    if (device->screenshot()) {
        showScreenshotHint();
    }
}

void ToolForm::on_recordBtn_clicked()
{
    VideoForm *form = qobject_cast<VideoForm *>(parent());
    if (!form) {
        return;
    }

    if (form->isRecording()) {
        form->stopRecord();
    } else {
        form->startRecord();
    }
    updateRecordUi();
}

void ToolForm::updateRecordUi()
{
    VideoForm *form = qobject_cast<VideoForm *>(parent());
    const bool recording = form && form->isRecording();

    if (recording) {
        if (!m_recording) {
            m_recording = true;
            m_recordElapsed.start();
            m_recordTimer.start();
            ui->recordBtn->setStyleSheet("color: #e74c3c");
        }
        const qint64 seconds = m_recordElapsed.elapsed() / 1000;
        ui->recordBtn->setToolTip(tr("stop record %1:%2")
                                      .arg(seconds / 60, 2, 10, QChar('0'))
                                      .arg(seconds % 60, 2, 10, QChar('0')));
    } else if (m_recording) {
        m_recording = false;
        m_recordTimer.stop();
        ui->recordBtn->setStyleSheet("");
        IconHelper::Instance()->SetIcon(ui->recordBtn, QChar(0xf03d), 15);
        ui->recordBtn->setToolTip(tr("record screen (Ctrl+Alt+R)"));
    }
}

void ToolForm::on_volumeUpBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postVolumeUp();
}

void ToolForm::on_volumeDownBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postVolumeDown();
}

void ToolForm::on_closeScreenBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->setDisplayPower(false);
}

void ToolForm::on_expandNotifyBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->expandNotificationPanel();
}

void ToolForm::on_expandSettingsBtn_clicked()
{
    hideSettingsMenu();
    startActivity(QStringLiteral("android.settings.SETTINGS"));
}

void ToolForm::on_rotateBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        device->rotateDevice();
    }
}

void ToolForm::on_touchBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }

    m_showTouch = !m_showTouch;
    device->showTouch(m_showTouch);
}

void ToolForm::on_cameraTorchBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device || !device->isCameraMode()) {
        return;
    }
    m_cameraTorch = !m_cameraTorch;
    device->setCameraTorch(m_cameraTorch);
    ui->cameraTorchBtn->setStyleSheet(m_cameraTorch ? "color: #f0c419" : "");
}

void ToolForm::on_cameraZoomOutBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && device->isCameraMode()) {
        device->cameraZoomOut();
    }
}

void ToolForm::on_cameraZoomInBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && device->isCameraMode()) {
        device->cameraZoomIn();
    }
}

void ToolForm::on_groupControlBtn_clicked()
{
    m_isHost = !m_isHost;
    updateGroupControl();
}

void ToolForm::on_openScreenBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->setDisplayPower(true);
}

void ToolForm::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateButtonSpacing();
    placeGrabDot();
}

void ToolForm::updateButtonSpacing()
{
    QVBoxLayout *vbox = qobject_cast<QVBoxLayout *>(layout());
    if (!vbox) {
        return;
    }

    int buttonCount = 0;
    int buttonsHeight = 0;
    for (int i = 0; i < vbox->count(); ++i) {
        QLayoutItem *item = vbox->itemAt(i);
        QWidget *button = item ? item->widget() : nullptr;
        // 这里用 isHidden() 而非 isVisible()：工具栏窗口此刻可能尚未显示，
        // 但相机模式下按钮的最终可见性集合已经确定。
        if (!button || button->isHidden()) {
            continue;
        }
        ++buttonCount;
        buttonsHeight += button->sizeHint().height();
    }
    if (buttonCount < 2) {
        return;
    }

    const QMargins margins = vbox->contentsMargins();
    const int gaps = buttonCount - 1;
    const int available = height() - margins.top() - margins.bottom() - buttonsHeight;
    // 将全部剩余空间均匀分配到按钮之间，使第一个按钮贴顶边、最后一个按钮贴底边。
    const int spacing = qMax(0, available / gaps);
    if (vbox->spacing() != spacing) {
        vbox->setSpacing(spacing);
    }
}