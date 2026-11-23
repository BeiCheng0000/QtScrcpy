#include <QDebug>
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QSpinBox>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>

#include "config.h"
#include "dialog.h"
#include "ui_dialog.h"
#include "videoform.h"
#include "filemanagerwindow.h"
#include "groupcontrolwidget.h"
#include "nowheelcontrols.h"
#include "../groupcontroller/groupcontroller.h"

#ifdef Q_OS_WIN32
#include "../util/winutils.h"
#endif

QString s_keyMapPath = "";
QString s_defaultKeyMapPath = "";

namespace {
class ComboBoxItemDelegate final : public QStyledItemDelegate
{
public:
    explicit ComboBoxItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(qMax(hint.height(), 30));
        return hint;
    }
};
} // namespace

const QString &getKeyMapPath()
{
    if (s_keyMapPath.isEmpty()) {
        s_keyMapPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_KEYMAP_PATH"));
        if (s_keyMapPath.isEmpty()) {
            s_keyMapPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/keymap";
        }
        if (!QDir().mkpath(s_keyMapPath)) {
            qWarning() << "Failed to create keymap directory:" << s_keyMapPath;
        }
    }
    return s_keyMapPath;
}

const QString &getDefaultKeyMapPath()
{
    if (s_defaultKeyMapPath.isEmpty()) {
        s_defaultKeyMapPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_DEFAULT_KEYMAP_PATH"));
        if (s_defaultKeyMapPath.isEmpty()) {
            s_defaultKeyMapPath = QCoreApplication::applicationDirPath() + "/keymap";
        }
    }
    return s_defaultKeyMapPath;
}

void initializeKeyMapDirectory()
{
    const QDir defaultDir(getDefaultKeyMapPath());
    const QDir userDir(getKeyMapPath());
    if (!defaultDir.exists() || !userDir.exists()) {
        return;
    }

    const QFileInfoList defaults = defaultDir.entryInfoList(QStringList() << "*.json", QDir::Files);
    for (const QFileInfo &fileInfo : defaults) {
        const QString userFile = userDir.filePath(fileInfo.fileName());
        if (!QFileInfo::exists(userFile) && !QFile::copy(fileInfo.filePath(), userFile)) {
            qWarning() << "Failed to copy default keymap to:" << userFile;
        }
    }
}

Dialog::Dialog(QWidget *parent) : QWidget(parent), ui(new Ui::Widget)
{
    ui->setupUi(this);
    initializeKeyMapDirectory();
    initUI();

    updateBootConfig(true);

    // Intel Mac 上如果配置文件遗留 decodeMode=1，强制重置为 0
    // （VideoToolbox 选项在 initUI() 中已被移除）
#if defined(Q_OS_MACOS) && !defined(__arm64__)
    if (ui->decodeModeBox->currentIndex() != 0) {
        ui->decodeModeBox->setCurrentIndex(0);
    }
#endif

    on_useSingleModeCheck_clicked();
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect availableGeometry = screen->availableGeometry();
        move(availableGeometry.x() + (availableGeometry.width() - width()) / 2,
             availableGeometry.y() + (availableGeometry.height() - height()) / 2);
    }
    on_updateDevice_clicked();

    connect(&m_autoUpdatetimer, &QTimer::timeout, this, &Dialog::on_updateDevice_clicked);
    if (ui->autoUpdatecheckBox->isChecked()) {
        m_autoUpdatetimer.start(5000);
    }
    // 意外掉线自动重连：4 秒一轮，共 5 轮（约 20 秒窗口，覆盖 USB
    // 重新枚举/无线波动恢复所需时间）。每一轮末尾都会重启定时器，
    // 由下一轮开头的 getDevice 检查判定本轮是否已成功。
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(4000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Dialog::tryReconnect);

    connect(&m_adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        QString log = "";
        bool newLine = true;
        QStringList args = m_adb.arguments();

        switch (processResult) {
        case qsc::AdbProcess::AER_ERROR_START:
            break;
        case qsc::AdbProcess::AER_SUCCESS_START:
            log = "adb run";
            newLine = false;
            break;
        case qsc::AdbProcess::AER_ERROR_EXEC:
            //log = m_adb.getErrorOut();
            if (args.contains("ifconfig") && args.contains("wlan0")) {
                getIPbyIp();
            }
            break;
        case qsc::AdbProcess::AER_ERROR_MISSING_BINARY:
            log = "adb not found";
            break;
        case qsc::AdbProcess::AER_SUCCESS_EXEC:
            //log = m_adb.getStdOut();
            if (args.contains("devices")) {
                QStringList devices = m_adb.getDevicesSerialFromStdOut();
                ui->serialBox->clear();
                ui->connectedPhoneList->clear();
                for (auto &item : devices) {
                    ui->serialBox->addItem(item);
                    ui->connectedPhoneList->addItem(Config::getInstance().getNickName(item) + "-" + item);
                }
            } else if (args.contains("show") && args.contains("wlan0")) {
                QString ip = m_adb.getDeviceIPFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            } else if (args.contains("ifconfig") && args.contains("wlan0")) {
                QString ip = m_adb.getDeviceIPFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            } else if (args.contains("ip -o a")) {
                QString ip = m_adb.getDeviceIPByIpFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            }
            break;
        }
        if (!log.isEmpty()) {
            outLog(log, newLine);
        }
    });

    m_hideIcon = new QSystemTrayIcon(this);
    m_hideIcon->setIcon(QIcon(":/image/tray/logo.png"));
    m_menu = new QMenu(this);
    m_quit = new QAction(this);
    m_showWindow = new QAction(this);
    m_showWindow->setText(tr("show"));
    m_quit->setText(tr("quit"));
    m_menu->addAction(m_showWindow);
    m_menu->addAction(m_quit);
    m_hideIcon->setContextMenu(m_menu);
    m_hideIcon->show();
    connect(m_showWindow, &QAction::triggered, this, &Dialog::show);
    connect(m_quit, &QAction::triggered, this, [this]() {
        m_hideIcon->hide();
        qApp->quit();
    });
    connect(m_hideIcon, &QSystemTrayIcon::activated, this, &Dialog::slotActivated);

    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceConnected, this, &Dialog::onDeviceConnected);
    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceDisconnected, this, &Dialog::onDeviceDisconnected);
}

Dialog::~Dialog()
{
    qDebug() << "~Dialog()";
    updateBootConfig(false);
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
    delete ui;
}

void Dialog::initUI()
{
    setAttribute(Qt::WA_DeleteOnClose);
    //setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint);

    setWindowTitle(Config::getInstance().getTitle());
#ifdef Q_OS_LINUX
    // 设置窗口图标（继承 main.cpp 中设置的程序图标）
    // If application icon was set, this will use it automatically
    if (!qApp->windowIcon().isNull()) {
        setWindowIcon(qApp->windowIcon());
    }
#endif

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)this->winId(), true);
#endif

    ui->bitRateEdit->setValidator(new QIntValidator(1, 99999, this));
    ui->wirelessBitRateEdit->setValidator(new QIntValidator(1, 99999, this));

    ui->presetConfigBtn->setVisible(false);

    // 设备文件管理入口：与"启动服务/停止服务"同排，随当前选中设备打开独立窗口
    m_fileManagerBtn = new QPushButton(tr("文件管理"), ui->startServerBtn->parentWidget());
    m_fileManagerBtn->setMinimumHeight(ui->startServerBtn->minimumHeight());
    m_fileManagerBtn->setToolTip(tr("浏览设备文件，支持拖拽上传、下载与文本编辑"));
    ui->horizontalLayout_server->addWidget(m_fileManagerBtn);
    connect(m_fileManagerBtn, &QPushButton::clicked, this, &Dialog::on_fileManagerBtn_clicked);

    // 填充编码模式下拉框。索引 0 是 .ui 中固定的“默认（自动）”项，
    // 其下依次追加所有已注册的编码预设。
    for (const EncoderPreset &preset : EncoderPresetRegistry::all()) {
        ui->codecModeBox->addItem(preset.displayName);
    }

    ui->maxSizeBox->addItem("640");
    ui->maxSizeBox->addItem("720");
    ui->maxSizeBox->addItem("1080");
    ui->maxSizeBox->addItem("1280");
    ui->maxSizeBox->addItem("1920");
    ui->maxSizeBox->addItem(tr("original"));

    // 码率与分辨率联动：高分辨率（1920/原画）下码率过低会明显模糊，
    // 这里只纠正“明显不匹配”的组合到可用下限，不影响画面同步率
    //（USB 与 Wi-Fi 带宽远高于 12 Mbps，编码器码控也不是延迟瓶颈）。
    ui->bitRateEdit->setToolTip(tr("USB 连接使用；建议 12 Mbps 或更高，原画分辨率下低于 8 Mbps 会明显模糊"));
    ui->wirelessBitRateEdit->setToolTip(
        tr("无线（IP:端口）连接使用；Wi-Fi 带宽与稳定性受限，建议 2~8 Mbps，过高会卡顿或花屏"));
    ui->wirelessBitRateLabel->setToolTip(ui->wirelessBitRateEdit->toolTip());
    connect(ui->maxSizeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 4) { // 0..3 依次为 640/720/1080/1280
            return;
        }
        const quint32 current = getBitRate();
        if (current >= 8000000) {
            return;
        }
        ui->bitRateEdit->blockSignals(true);
        ui->bitRateBox->blockSignals(true);
        ui->bitRateEdit->setText(QString::number(12000000 / 1000000));
        ui->bitRateBox->setCurrentText("Mbps");
        ui->bitRateEdit->blockSignals(false);
        ui->bitRateBox->blockSignals(false);
        outLog(tr("已为高分辨率自动提升码率至 12 Mbps（可在设置中调整）"));
    });

    ui->videoSourceBox->addItem(tr("display"));
    ui->videoSourceBox->addItem(tr("camera"));
    ui->cameraFacingBox->addItem(tr("back"));
    ui->cameraFacingBox->addItem(tr("front"));

    ui->formatBox->addItem("mp4");
    ui->formatBox->addItem("mkv");

    ui->lockOrientationBox->addItem(tr("no lock"));
    ui->lockOrientationBox->addItem("0");
    ui->lockOrientationBox->addItem("90");
    ui->lockOrientationBox->addItem("180");
    ui->lockOrientationBox->addItem("270");
    ui->lockOrientationBox->setCurrentIndex(0);

    ui->decodeModeBox->addItem(tr("FFmpeg + OpenGL (Universal Default)"));
    ui->decodeModeBox->addItem(tr("VideoToolbox + Metal (Apple Silicon Only)"));
    ui->decodeModeBox->setCurrentIndex(0);

#ifndef Q_OS_MACOS
    // 非 macOS：隐藏整个解码模式控件行
    ui->decodeModeLabel->hide();
    ui->decodeModeBox->hide();
#elif !defined(__arm64__)
    // Intel Mac：移除 VideoToolbox 选项，用户只看到 FFmpeg
    ui->decodeModeBox->removeItem(1);
#endif

    // 加载IP历史记录
    loadIpHistory();

    // 加载端口历史记录
    loadPortHistory();

    // 无线调试（Android 11+ 配对）控件与控制器
    setupWirelessUi();

    // 为deviceIpEdt添加右键菜单
    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->deviceIpEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showIpEditMenu);
    }
    
    // 为devicePortEdt添加右键菜单
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->devicePortEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showPortEditMenu);
    }
    initAdvancedDisplayUi();

    auto *configTabs = new QTabWidget(ui->rightWidget);
    auto *startConfigPage = new QWidget(configTabs);
    auto *advancedDisplayPage = new QWidget(configTabs);
    auto *advancedConfigPage = new QWidget(configTabs);
    auto *startConfigLayout = new QVBoxLayout(startConfigPage);
    auto *advancedDisplayLayout = new QVBoxLayout(advancedDisplayPage);
    startConfigLayout->setContentsMargins(0, 0, 0, 0);
    advancedDisplayLayout->setContentsMargins(0, 0, 0, 0);

    ui->verticalLayout_5->removeWidget(ui->adbGroupBox);
    ui->verticalLayout_5->removeWidget(ui->outEdit);
    ui->verticalLayout_6->removeWidget(ui->configGroupBox);
    ui->verticalLayout_6->removeWidget(ui->usbGroupBox);
    ui->verticalLayout_6->removeWidget(ui->wirelessGroupBox);
    ui->verticalLayout_6->removeWidget(m_advancedDisplayGroup);
    delete ui->verticalLayout_6->takeAt(0);

    ui->verticalLayout_5->addWidget(ui->usbGroupBox);
    ui->verticalLayout_5->addWidget(ui->wirelessGroupBox);
    startConfigLayout->addWidget(ui->configGroupBox);
    startConfigLayout->addWidget(ui->adbGroupBox);
    startConfigLayout->addWidget(ui->outEdit, 1);
    advancedDisplayLayout->addWidget(m_advancedDisplayGroup);
    advancedDisplayLayout->addStretch();
    initAdvancedConfigUi(advancedConfigPage);
    // 群控页签：集中管理多设备批量操作与批量安装
    m_groupWidget = new GroupControlWidget(configTabs);
    configTabs->addTab(startConfigPage, tr("Start Config"));
    configTabs->addTab(advancedDisplayPage, tr("Advanced Display"));
    configTabs->addTab(advancedConfigPage, tr("Advanced Config"));
    m_groupTabWidget = configTabs;
    m_groupTabIndex = configTabs->addTab(m_groupWidget, tr("Group Control"));
    configTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    ui->verticalLayout_6->addWidget(configTabs);

    ui->leftWidget->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    ui->leftWidget->setMinimumWidth(0);
    ui->horizontalLayout_11->setStretch(0, 1);
    ui->horizontalLayout_11->setStretch(1, 2);
    // 无线区域第一行：IP 输入框占剩余宽度，其余控件按内容宽度排列
    ui->wirelessRow1->setStretch(0, 1);
    ui->wirelessRow1->setStretch(2, 0);

    for (QComboBox *comboBox : findChildren<QComboBox *>()) {
        comboBox->view()->setItemDelegate(new ComboBoxItemDelegate(comboBox->view()));
    }

    QSizePolicy simpleModePolicy = ui->simpleGroupBox->sizePolicy();
    simpleModePolicy.setVerticalPolicy(QSizePolicy::Expanding);
    ui->simpleGroupBox->setSizePolicy(simpleModePolicy);

    QSizePolicy logPolicy = ui->outEdit->sizePolicy();
    logPolicy.setVerticalPolicy(QSizePolicy::Expanding);
    ui->outEdit->setSizePolicy(logPolicy);
    ui->verticalLayout_5->setStretch(0, 0);
    ui->verticalLayout_5->setStretch(1, 1);
    ui->verticalLayout_5->setStretch(2, 0);
    ui->verticalLayout_5->setStretch(3, 0);
    ui->verticalLayout_4->setStretch(2, 1);
    ui->verticalLayout_6->setStretch(0, 1);
}

void Dialog::initAdvancedDisplayUi()
{
    m_advancedDisplayGroup = new QGroupBox(tr("Advanced display"), this);
    m_advancedDisplayGroup->setCheckable(true);
    m_advancedDisplayGroup->setChecked(false);
    auto *layout = new QFormLayout(m_advancedDisplayGroup);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_displayModeBox = new FocusWheelComboBox(m_advancedDisplayGroup);
    m_displayModeBox->addItem(tr("Primary display"));
    m_displayModeBox->addItem(tr("Existing display ID"));
    m_displayModeBox->addItem(tr("New virtual display"));
    layout->addRow(tr("Display mode"), m_displayModeBox);

    m_displayIdEdit = new QLineEdit(m_advancedDisplayGroup);
    m_displayIdEdit->setPlaceholderText("1");
    layout->addRow(tr("Display ID"), m_displayIdEdit);

    m_newDisplayEdit = new QLineEdit(m_advancedDisplayGroup);
    m_newDisplayEdit->setPlaceholderText("1920x1080/240");
    layout->addRow(tr("Virtual size / DPI"), m_newDisplayEdit);

    m_cropEdit = new QLineEdit(m_advancedDisplayGroup);
    m_cropEdit->setPlaceholderText("width:height:x:y");
    layout->addRow(tr("Crop"), m_cropEdit);

    m_flexDisplayCheck = new QCheckBox(tr("Resize virtual display with window"), m_advancedDisplayGroup);
    layout->addRow(m_flexDisplayCheck);
    m_displayImePolicyBox = new FocusWheelComboBox(m_advancedDisplayGroup);
    m_displayImePolicyBox->addItem(tr("Server default"), "");
    m_displayImePolicyBox->addItem("local", "local");
    m_displayImePolicyBox->addItem("fallback", "fallback");
    m_displayImePolicyBox->addItem("hide", "hide");
    layout->addRow(tr("IME policy"), m_displayImePolicyBox);
    m_vdSystemDecorationsCheck = new QCheckBox(tr("Show system decorations"), m_advancedDisplayGroup);
    m_vdSystemDecorationsCheck->setChecked(true);
    layout->addRow(m_vdSystemDecorationsCheck);
    m_vdDestroyContentCheck = new QCheckBox(tr("Destroy content on close"), m_advancedDisplayGroup);
    m_vdDestroyContentCheck->setChecked(true);
    layout->addRow(m_vdDestroyContentCheck);
    m_keepActiveCheck = new QCheckBox(tr("Keep device active"), m_advancedDisplayGroup);
    layout->addRow(m_keepActiveCheck);
    auto *startAppWidget = new QWidget(m_advancedDisplayGroup);
    auto *startAppLayout = new QHBoxLayout(startAppWidget);
    startAppLayout->setContentsMargins(0, 0, 0, 0);
    m_startAppBox = new FocusWheelComboBox(startAppWidget);
    m_startAppBox->setEditable(true);
    m_startAppBox->setInsertPolicy(QComboBox::NoInsert);
    m_startAppBox->setPlaceholderText("com.android.settings");
    m_startAppBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_refreshAppsBtn = new QPushButton(tr("refresh"), startAppWidget);
    m_refreshAppsBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_refreshAppsBtn->setFixedWidth(m_refreshAppsBtn->sizeHint().width());
    startAppLayout->addWidget(m_startAppBox);
    startAppLayout->addWidget(m_refreshAppsBtn);
    layout->addRow(tr("Start app"), startAppWidget);

    connect(m_displayModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Dialog::updateAdvancedDisplayUi);
    connect(m_flexDisplayCheck, &QCheckBox::toggled, this, &Dialog::updateAdvancedDisplayUi);
    connect(m_refreshAppsBtn, &QPushButton::clicked, this, &Dialog::refreshApps);
    updateAdvancedDisplayUi();
}

void Dialog::initAdvancedConfigUi(QWidget *parent)
{
    auto *pageLayout = new QVBoxLayout(parent);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto *scrollArea = new QScrollArea(parent);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    auto *group = new QGroupBox(tr("Application configuration"), content);
    m_appConfigGroup = group;
    auto *layout = new QFormLayout(group);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_configLanguageBox = new FocusWheelComboBox(group);
    m_configLanguageBox->addItem(tr("Automatic"), "Auto");
    m_configLanguageBox->addItem("简体中文", "zh_CN");
    m_configLanguageBox->addItem("English", "en_US");
    m_configLanguageBox->addItem("日本語", "ja_JP");
    m_configLanguageBox->addItem("한국어", "ko_KR");
    layout->addRow(tr("Language"), m_configLanguageBox);

    m_configTitleEdit = new QLineEdit(group);
    layout->addRow(tr("Window title"), m_configTitleEdit);

    m_configPushPathEdit = new QLineEdit(group);
    m_configPushPathEdit->setPlaceholderText("/sdcard/");
    layout->addRow(tr("Push file path"), m_configPushPathEdit);

    m_configMaxFpsSpin = new FocusWheelSpinBox(group);
    m_configMaxFpsSpin->setRange(0, 1000);
    m_configMaxFpsSpin->setSpecialValueText(tr("Unlimited"));
    layout->addRow(tr("Maximum FPS"), m_configMaxFpsSpin);

    // 无线（ip:port）专用最大帧率：受限 Wi-Fi 通道（尤其 2.4GHz Wi-Fi 4）
    // 实测吞吐可能低于视频码率 + 音频之和，不限帧会通道积压、延迟累积。
    m_configWirelessMaxFpsSpin = new FocusWheelSpinBox(group);
    m_configWirelessMaxFpsSpin->setRange(0, 240);
    m_configWirelessMaxFpsSpin->setSpecialValueText(tr("Unlimited"));
    m_configWirelessMaxFpsSpin->setToolTip(
        tr("Wireless (IP:port) only. 2.4GHz Wi-Fi may not carry unlimited FPS plus audio; "
           "30 FPS halves bandwidth demand without visible quality loss."));
    layout->addRow(tr("Wireless max FPS"), m_configWirelessMaxFpsSpin);

    m_configRenderExpiredCheck = new QCheckBox(tr("Render expired video frames"), group);
    layout->addRow(m_configRenderExpiredCheck);

    m_configDecoderThreadsSpin = new FocusWheelSpinBox(group);
    m_configDecoderThreadsSpin->setRange(0, 16);
    m_configDecoderThreadsSpin->setSpecialValueText(tr("Auto"));
    m_configDecoderThreadsSpin->setToolTip(
        tr("FFmpeg decoder threads. 1 = single thread (old behavior, lowest latency); "
           "frame threading adds about 1-2 frames of pipeline latency. Takes effect on next connection."));
    layout->addRow(tr("Decoder threads"), m_configDecoderThreadsSpin);

    m_configOpenGlBox = new FocusWheelComboBox(group);
    m_configOpenGlBox->addItem(tr("Automatic"), -1);
    m_configOpenGlBox->addItem(tr("Software OpenGL"), 0);
    m_configOpenGlBox->addItem(tr("OpenGL ES"), 1);
    m_configOpenGlBox->addItem(tr("Desktop OpenGL"), 2);
    m_configOpenGlBox->setToolTip(tr("Takes effect after restarting QtScrcpy."));
    layout->addRow(tr("OpenGL backend"), m_configOpenGlBox);

    m_configServerPathEdit = new QLineEdit(group);
    m_configServerPathEdit->setPlaceholderText("/data/local/tmp/scrcpy-server.jar");
    layout->addRow(tr("Device server path"), m_configServerPathEdit);

    m_configAdbPathEdit = new QLineEdit(group);
    m_configAdbPathEdit->setPlaceholderText(tr("Use bundled ADB"));
    layout->addRow(tr("ADB executable"), m_configAdbPathEdit);

    m_configCodecOptionsEdit = new QLineEdit(group);
    m_configCodecOptionsEdit->setPlaceholderText("profile=1,level=2");
    layout->addRow(tr("Codec options"), m_configCodecOptionsEdit);

    m_configCodecNameEdit = new QLineEdit(group);
    m_configCodecNameEdit->setPlaceholderText("OMX.qcom.video.encoder.avc");
    layout->addRow(tr("Codec name"), m_configCodecNameEdit);

    m_configLogLevelBox = new FocusWheelComboBox(group);
    m_configLogLevelBox->addItem("verbose", "verbose");
    m_configLogLevelBox->addItem("debug", "debug");
    m_configLogLevelBox->addItem("info", "info");
    m_configLogLevelBox->addItem("warn", "warn");
    m_configLogLevelBox->addItem("error", "error");
    layout->addRow(tr("Log level"), m_configLogLevelBox);

    // 录制设置（此前只能手改 userdata.ini，无 UI 开关）
    auto *recordGroup = new QGroupBox(tr("Recording"), content);
    m_recordGroupTitle = recordGroup;
    auto *recordLayout = new QFormLayout(recordGroup);
    recordLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_recordAudioCheck = new QCheckBox(tr("Record audio"), recordGroup);
    recordLayout->addRow(m_recordAudioCheck);

    m_recordMouseTrackCheck = new QCheckBox(tr("Overlay mouse track"), recordGroup);
    recordLayout->addRow(m_recordMouseTrackCheck);

    m_recordFpsSpin = new FocusWheelSpinBox(recordGroup);
    m_recordFpsSpin->setRange(1, 120);
    recordLayout->addRow(tr("Record FPS"), m_recordFpsSpin);

    m_recordVideoBitrateSpin = new FocusWheelSpinBox(recordGroup);
    m_recordVideoBitrateSpin->setRange(200000, 100000000);
    m_recordVideoBitrateSpin->setSingleStep(500000);
    recordLayout->addRow(tr("Video bitrate (bps)"), m_recordVideoBitrateSpin);

    m_recordAudioBitrateSpin = new FocusWheelSpinBox(recordGroup);
    m_recordAudioBitrateSpin->setRange(32000, 320000);
    m_recordAudioBitrateSpin->setSingleStep(16000);
    recordLayout->addRow(tr("Audio bitrate (bps)"), m_recordAudioBitrateSpin);

    contentLayout->addWidget(recordGroup);

    auto *buttons = new QHBoxLayout();
    m_openConfigButton = new QPushButton(tr("Open Config Directory"), group);
    m_openKeymapButton = new QPushButton(tr("Open Keymap Directory"), group);
    m_saveButton = new QPushButton(tr("Save"), group);
    buttons->addWidget(m_openConfigButton);
    buttons->addWidget(m_openKeymapButton);
    buttons->addStretch();
    buttons->addWidget(m_saveButton);
    layout->addRow(buttons);

    contentLayout->addWidget(group);
    contentLayout->addStretch();
    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea);

    connect(m_openConfigButton, &QPushButton::clicked, this, [this] {
        openUserDirectory(Config::getInstance().getConfigDirectory());
    });
    connect(m_openKeymapButton, &QPushButton::clicked, this, [this] {
        openUserDirectory(getKeyMapPath());
    });
    connect(m_saveButton, &QPushButton::clicked, this, &Dialog::saveAdvancedConfig);
    loadAdvancedConfig();
}

void Dialog::loadAdvancedConfig()
{
    Config &config = Config::getInstance();
    m_configLanguageBox->setCurrentIndex(qMax(0, m_configLanguageBox->findData(config.getLanguage())));
    m_configTitleEdit->setText(config.getTitle());
    m_configPushPathEdit->setText(config.getPushFilePath());
    m_configMaxFpsSpin->setValue(qBound(0, config.getMaxFps(), m_configMaxFpsSpin->maximum()));
    m_configWirelessMaxFpsSpin->setValue(qBound(0, config.getWirelessMaxFps(), m_configWirelessMaxFpsSpin->maximum()));
    m_configRenderExpiredCheck->setChecked(config.getRenderExpiredFrames() != 0);
    m_configDecoderThreadsSpin->setValue(qBound(0, config.getDecoderThreads(), m_configDecoderThreadsSpin->maximum()));
    m_configOpenGlBox->setCurrentIndex(qMax(0, m_configOpenGlBox->findData(config.getDesktopOpenGL())));
    m_configServerPathEdit->setText(config.getServerPath());
    m_configAdbPathEdit->setText(config.getAdbPath());
    m_configCodecOptionsEdit->setText(config.getCodecOptions());
    m_configCodecNameEdit->setText(config.getCodecName());
    m_configLogLevelBox->setCurrentIndex(qMax(0, m_configLogLevelBox->findData(config.getLogLevel())));

    // 录制设置（userdata.ini）
    const UserBootConfig boot = config.getUserBootConfig();
    m_recordAudioCheck->setChecked(boot.recordAudio);
    m_recordMouseTrackCheck->setChecked(boot.recordMouseTrack);
    m_recordFpsSpin->setValue(qBound(m_recordFpsSpin->minimum(), boot.recordFps, m_recordFpsSpin->maximum()));
    m_recordVideoBitrateSpin->setValue(qBound(m_recordVideoBitrateSpin->minimum(), boot.recordVideoBitrate, m_recordVideoBitrateSpin->maximum()));
    m_recordAudioBitrateSpin->setValue(qBound(m_recordAudioBitrateSpin->minimum(), boot.recordAudioBitrate, m_recordAudioBitrateSpin->maximum()));
}

void Dialog::saveAdvancedConfig()
{
    QString pushPath = m_configPushPathEdit->text().trimmed();
    if (!pushPath.isEmpty() && !pushPath.endsWith('/')) {
        pushPath += '/';
    }

    // ADB 路径：允许填目录（自动补 adb.exe），并校验文件确实存在，
    // 避免保存一个无效路径后在运行时被静默忽略、退回内置 adb。
    QString adbPath = m_configAdbPathEdit->text().trimmed();
    if (!adbPath.isEmpty()) {
        QFileInfo adbInfo(adbPath);
        if (adbInfo.isDir()) {
#ifdef Q_OS_WIN32
            adbInfo = QFileInfo(QDir(adbPath).absoluteFilePath("adb.exe"));
#else
            adbInfo = QFileInfo(QDir(adbPath).absoluteFilePath("adb"));
#endif
        }
        if (!adbInfo.isFile()) {
            QMessageBox::warning(this, tr("Invalid ADB path"),
                                 tr("ADB executable not found:\n%1").arg(adbPath));
            return;
        }
        adbPath = adbInfo.absoluteFilePath();
        m_configAdbPathEdit->setText(adbPath);
    }

    const QString newLanguage = m_configLanguageBox->currentData().toString();
    const bool languageChanged = (newLanguage != Config::getInstance().getLanguage());

    QMap<QString, QVariant> values;
    values.insert("Language", newLanguage);
    values.insert("WindowTitle", m_configTitleEdit->text().trimmed());
    values.insert("PushFilePath", pushPath);
    values.insert("MaxFps", m_configMaxFpsSpin->value());
    values.insert("WirelessMaxFps", m_configWirelessMaxFpsSpin->value());
    values.insert("RenderExpiredFrames", m_configRenderExpiredCheck->isChecked() ? 1 : 0);
    values.insert("DecoderThreads", m_configDecoderThreadsSpin->value());
    values.insert("UseDesktopOpenGL", m_configOpenGlBox->currentData());
    values.insert("ServerPath", m_configServerPathEdit->text().trimmed());
    values.insert("AdbPath", adbPath);
    values.insert("CodecOptions", m_configCodecOptionsEdit->text().trimmed());
    values.insert("CodecName", m_configCodecNameEdit->text().trimmed());
    values.insert("LogLevel", m_configLogLevelBox->currentData());

    // 录制设置保存到 userdata.ini（与投屏启动配置同文件）
    UserBootConfig boot = Config::getInstance().getUserBootConfig();
    boot.recordAudio = m_recordAudioCheck->isChecked();
    boot.recordMouseTrack = m_recordMouseTrackCheck->isChecked();
    boot.recordFps = m_recordFpsSpin->value();
    boot.recordVideoBitrate = m_recordVideoBitrateSpin->value();
    boot.recordAudioBitrate = m_recordAudioBitrateSpin->value();
    Config::getInstance().setUserBootConfig(boot);

    if (!Config::getInstance().updateCommonConfig(values)) {
        QMessageBox::warning(this, tr("Save failed"), tr("Unable to save config.ini."));
        return;
    }
    // 立即应用新的 ADB 路径，无需重启；路径为空表示回退到内置 adb
    qsc::AdbProcess::setAdbPath(adbPath);
    m_configPushPathEdit->setText(pushPath);

    if (languageChanged) {
        // 语言立即生效：重载翻译器 + 广播 LanguageChange，不必重启程序
        retranslateApplication();
        outLog(tr("Language changed, interface updated"));
    }
    outLog(adbPath.isEmpty() ? tr("ADB path: bundled adb") : tr("ADB path: %1").arg(adbPath));
    QMessageBox::information(this, tr("Configuration saved"),
                             languageChanged
                                 ? tr("Settings saved. Language changed immediately; restart for other changes to take effect.")
                                 : tr("The settings were saved to config.ini. Restart QtScrcpy for all changes to take effect."));
}

void Dialog::openUserDirectory(const QString &path)
{
    if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, tr("Open directory failed"), path);
    }
}

void Dialog::updateAdvancedDisplayUi()
{
    if (!m_displayModeBox) {
        return;
    }
    const bool existing = m_displayModeBox->currentIndex() == 1;
    const bool virtualDisplay = m_displayModeBox->currentIndex() == 2;
    m_displayIdEdit->setEnabled(existing);
    m_newDisplayEdit->setEnabled(virtualDisplay);
    m_flexDisplayCheck->setEnabled(virtualDisplay);
    if (!virtualDisplay) {
        m_flexDisplayCheck->setChecked(false);
    }
    m_displayImePolicyBox->setEnabled(virtualDisplay);
    m_vdSystemDecorationsCheck->setEnabled(virtualDisplay);
    m_vdDestroyContentCheck->setEnabled(virtualDisplay);
    m_cropEdit->setEnabled(!m_flexDisplayCheck->isChecked());
}

void Dialog::updateBootConfig(bool toView)
{
    if (toView) {
        UserBootConfig config = Config::getInstance().getUserBootConfig();

        if (config.bitRate == 0) {
            ui->bitRateBox->setCurrentText("Mbps");
        } else if (config.bitRate % 1000000 == 0) {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000000));
            ui->bitRateBox->setCurrentText("Mbps");
        } else {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000));
            ui->bitRateBox->setCurrentText("Kbps");
        }

        // 无线（ip:port）专用比特率
        if (config.wirelessBitRate == 0) {
            ui->wirelessBitRateBox->setCurrentText("Mbps");
        } else if (config.wirelessBitRate % 1000000 == 0) {
            ui->wirelessBitRateEdit->setText(QString::number(config.wirelessBitRate / 1000000));
            ui->wirelessBitRateBox->setCurrentText("Mbps");
        } else {
            ui->wirelessBitRateEdit->setText(QString::number(config.wirelessBitRate / 1000));
            ui->wirelessBitRateBox->setCurrentText("Kbps");
        }

        ui->maxSizeBox->setCurrentIndex(config.maxSizeIndex);
        ui->formatBox->setCurrentIndex(config.recordFormatIndex);
        ui->recordPathEdt->setText(config.recordPath);
        ui->lockOrientationBox->setCurrentIndex(config.lockOrientationIndex);
        ui->framelessCheck->setChecked(config.framelessWindow);
        ui->recordScreenCheck->setChecked(config.recordScreen);
        ui->notDisplayCheck->setChecked(config.recordBackground);
        ui->useReverseCheck->setChecked(config.reverseConnect);
        ui->fpsCheck->setChecked(config.showFPS);
        ui->alwaysTopCheck->setChecked(config.windowOnTop);
        ui->closeScreenCheck->setChecked(config.autoOffScreen);
        ui->stayAwakeCheck->setChecked(config.keepAlive);
        ui->useSingleModeCheck->setChecked(config.simpleMode);
        ui->autoUpdatecheckBox->setChecked(config.autoUpdateDevice);
        ui->showToolbar->setChecked(config.showToolbar);
        ui->quitOnCloseCheck->setChecked(config.quitOnClose);
        ui->autoStartAudioCheck->setChecked(config.autoStartAudio);
        ui->decodeModeBox->setCurrentIndex(config.decodeMode);
        ui->codecModeBox->setCurrentIndex(config.codecModeIndex);

        // 恢复编码预设对话框的状态（若该对话框此前已打开）
        if (m_presetDialog) {
            m_presetDialog->setLevel(config.presetLevel);
        }
        ui->videoSourceBox->setCurrentIndex(config.videoSource);
        ui->cameraFacingBox->setCurrentIndex(qBound(0, config.cameraFacing, 1));
        if (m_advancedDisplayGroup) {
            m_advancedDisplayGroup->setChecked(config.advancedDisplay);
            m_displayModeBox->setCurrentIndex(qBound(0, config.displayMode, 2));
            m_displayIdEdit->setText(config.displayId);
            m_newDisplayEdit->setText(config.newDisplay);
            m_cropEdit->setText(config.crop);
            m_flexDisplayCheck->setChecked(config.flexDisplay);
            m_displayImePolicyBox->setCurrentIndex(qMax(0, m_displayImePolicyBox->findData(config.displayImePolicy)));
            m_vdSystemDecorationsCheck->setChecked(config.vdSystemDecorations);
            m_vdDestroyContentCheck->setChecked(config.vdDestroyContent);
            m_keepActiveCheck->setChecked(config.keepActive);
            m_startAppBox->setEditText(config.startApp);
            updateAdvancedDisplayUi();
        }
        updateVideoSourceUi();
    } else {
        UserBootConfig config = Config::getInstance().getUserBootConfig();

        config.bitRate = getBitRate();
        config.wirelessBitRate = getWirelessBitRate();
        config.maxSizeIndex = ui->maxSizeBox->currentIndex();
        config.recordFormatIndex = ui->formatBox->currentIndex();
        config.recordPath = ui->recordPathEdt->text();
        config.lockOrientationIndex = ui->lockOrientationBox->currentIndex();
        config.recordScreen = ui->recordScreenCheck->isChecked();
        config.recordBackground = ui->notDisplayCheck->isChecked();
        config.reverseConnect = ui->useReverseCheck->isChecked();
        config.showFPS = ui->fpsCheck->isChecked();
        config.windowOnTop = ui->alwaysTopCheck->isChecked();
        config.autoOffScreen = ui->closeScreenCheck->isChecked();
        config.framelessWindow = ui->framelessCheck->isChecked();
        config.keepAlive = ui->stayAwakeCheck->isChecked();
        config.simpleMode = ui->useSingleModeCheck->isChecked();
        config.autoUpdateDevice = ui->autoUpdatecheckBox->isChecked();
        config.showToolbar = ui->showToolbar->isChecked();
        config.quitOnClose = ui->quitOnCloseCheck->isChecked();
        config.autoStartAudio = ui->autoStartAudioCheck->isChecked();
        config.decodeMode = ui->decodeModeBox->currentIndex();
        config.codecModeIndex = ui->codecModeBox->currentIndex();

        // Save encoder preset dialog state (if opened)
        if (m_presetDialog) {
            config.presetLevel = m_presetDialog->selectedLevel();
        }

        config.videoSource = ui->videoSourceBox->currentIndex();
        config.cameraFacing = qMin(ui->cameraFacingBox->currentIndex(), 1);
        if (m_advancedDisplayGroup) {
            config.advancedDisplay = m_advancedDisplayGroup->isChecked();
            config.displayMode = m_displayModeBox->currentIndex();
            config.displayId = m_displayIdEdit->text().trimmed();
            config.newDisplay = m_newDisplayEdit->text().trimmed();
            config.crop = m_cropEdit->text().trimmed();
            config.flexDisplay = m_flexDisplayCheck->isChecked();
            config.displayImePolicy = m_displayImePolicyBox->currentData().toString();
            config.vdSystemDecorations = m_vdSystemDecorationsCheck->isChecked();
            config.vdDestroyContent = m_vdDestroyContentCheck->isChecked();
            config.keepActive = m_keepActiveCheck->isChecked();
            config.startApp = m_startAppBox->currentData().toString();
            if (config.startApp.isEmpty()) {
                config.startApp = m_startAppBox->currentText().trimmed();
            }
        }

        // 保存当前IP到历史记录
        QString currentIp = ui->deviceIpEdt->currentText().trimmed();
        if (!currentIp.isEmpty()) {
            saveIpHistory(currentIp);
        }

        Config::getInstance().setUserBootConfig(config);
    }
}

void Dialog::execAdbCmd()
{
    if (checkAdbRun()) {
        return;
    }
    QString cmd = ui->adbCommandEdt->text().trimmed();
    outLog("adb " + cmd, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", Qt::SkipEmptyParts));
#else
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", QString::SkipEmptyParts));
#endif
}

void Dialog::delayMs(int ms)
{
    QTime dieTime = QTime::currentTime().addMSecs(ms);

    while (QTime::currentTime() < dieTime) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
}

QString Dialog::getGameScript(const QString &fileName)
{
    if (fileName.isEmpty()) {
        return "";
    }

    QFile loadFile(getKeyMapPath() + "/" + fileName);
    if (!loadFile.exists()) {
        loadFile.setFileName(getDefaultKeyMapPath() + "/" + fileName);
    }
    if (!loadFile.open(QIODevice::ReadOnly)) {
        outLog("open file failed:" + fileName, true);
        return "";
    }

    QString ret = loadFile.readAll();
    loadFile.close();
    return ret;
}

void Dialog::slotActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
#ifdef Q_OS_WIN32
        this->show();
#endif
        break;
    default:
        break;
    }
}

void Dialog::bringToFront()
{
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
}

// 刷新 QFormLayout 中由 addRow(label, widget) 自动生成的 QLabel。
// Designer 的 retranslateUi() 不覆盖这些运行期创建的标签，需手动逐行回填。
static void retranslateFormLabels(QLayout *layout, const QStringList &labels)
{
    auto *form = qobject_cast<QFormLayout *>(layout);
    if (!form) {
        return;
    }
    for (int i = 0; i < form->rowCount() && i < labels.size(); ++i) {
        if (labels.at(i).isEmpty()) {
            continue;
        }
        auto *item = form->itemAt(i, QFormLayout::LabelRole);
        if (item && item->widget()) {
            if (auto *label = qobject_cast<QLabel *>(item->widget())) {
                label->setText(labels.at(i));
            }
        }
    }
}

static void setComboItemText(QComboBox *box, int index, const QString &text)
{
    if (box && index >= 0 && index < box->count()) {
        box->setItemText(index, text);
    }
}

void Dialog::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::LanguageChange) {
        return;
    }
    ui->retranslateUi(this);

    // 文件管理入口
    if (m_fileManagerBtn) {
        m_fileManagerBtn->setText(tr("File manager"));
        m_fileManagerBtn->setToolTip(tr("Browse device files, supports drag-and-drop upload, download and text editing"));
    }

    // 编码模式下拉：预设名称随语言变化
    if (ui->codecModeBox->count() > 0) {
        const int current = ui->codecModeBox->currentIndex();
        ui->codecModeBox->blockSignals(true);
        // 索引 0 是 .ui 固定项，由 retranslateUi() 已刷新；仅重建预设项
        for (int i = 1; i < ui->codecModeBox->count(); ++i) {
            const QVector<EncoderPreset> &presets = EncoderPresetRegistry::all();
            const int presetIndex = i - 1;
            if (presetIndex < presets.size()) {
                ui->codecModeBox->setItemText(i, presets.at(presetIndex).displayName);
            }
        }
        ui->codecModeBox->setCurrentIndex(current);
        ui->codecModeBox->blockSignals(false);
    }

    // 主界面动态下拉项
    setComboItemText(ui->maxSizeBox, ui->maxSizeBox->count() - 1, tr("original"));
    ui->bitRateEdit->setToolTip(tr("For USB; 12 Mbps or higher recommended; below 8 Mbps is visibly blurry at original resolution"));
    ui->wirelessBitRateEdit->setToolTip(
        tr("For wireless (IP:port); Wi-Fi bandwidth and stability are limited, 2~8 Mbps recommended"));
    ui->wirelessBitRateLabel->setToolTip(ui->wirelessBitRateEdit->toolTip());
    setComboItemText(ui->videoSourceBox, 0, tr("display"));
    setComboItemText(ui->videoSourceBox, 1, tr("camera"));
    setComboItemText(ui->cameraFacingBox, 0, tr("back"));
    setComboItemText(ui->cameraFacingBox, 1, tr("front"));
    setComboItemText(ui->lockOrientationBox, 0, tr("no lock"));
    setComboItemText(ui->decodeModeBox, 0, tr("FFmpeg + OpenGL (Universal Default)"));
    setComboItemText(ui->decodeModeBox, 1, tr("VideoToolbox + Metal (Apple Silicon Only)"));

    // 高级显示组
    if (m_advancedDisplayGroup) {
        m_advancedDisplayGroup->setTitle(tr("Advanced display"));
        setComboItemText(m_displayModeBox, 0, tr("Primary display"));
        setComboItemText(m_displayModeBox, 1, tr("Existing display ID"));
        setComboItemText(m_displayModeBox, 2, tr("New virtual display"));
        setComboItemText(m_displayImePolicyBox, 0, tr("Server default"));
        m_flexDisplayCheck->setText(tr("Resize virtual display with window"));
        m_vdSystemDecorationsCheck->setText(tr("Show system decorations"));
        m_vdDestroyContentCheck->setText(tr("Destroy content on close"));
        m_keepActiveCheck->setText(tr("Keep device active"));
        m_refreshAppsBtn->setText(tr("refresh"));
        retranslateFormLabels(m_advancedDisplayGroup->layout(),
                              { tr("Display mode"), tr("Display ID"), tr("Virtual size / DPI"),
                                tr("Crop"), QString(), tr("IME policy"), QString(), QString(),
                                QString(), tr("Start app") });
    }

    // 应用配置组
    if (m_appConfigGroup) {
        m_appConfigGroup->setTitle(tr("Application configuration"));
        setComboItemText(m_configLanguageBox, 0, tr("Automatic"));
        m_configMaxFpsSpin->setSpecialValueText(tr("Unlimited"));
        m_configWirelessMaxFpsSpin->setSpecialValueText(tr("Unlimited"));
        m_configWirelessMaxFpsSpin->setToolTip(
            tr("Wireless (IP:port) only. 2.4GHz Wi-Fi may not carry unlimited FPS plus audio; "
               "30 FPS halves bandwidth demand without visible quality loss."));
        m_configDecoderThreadsSpin->setSpecialValueText(tr("Auto"));
        m_configDecoderThreadsSpin->setToolTip(
            tr("FFmpeg decoder threads. 1 = single thread (old behavior, lowest latency); "
               "frame threading adds about 1-2 frames of pipeline latency. Takes effect on next connection."));
        setComboItemText(m_configOpenGlBox, 0, tr("Automatic"));
        setComboItemText(m_configOpenGlBox, 1, tr("Software OpenGL"));
        setComboItemText(m_configOpenGlBox, 2, tr("OpenGL ES"));
        setComboItemText(m_configOpenGlBox, 3, tr("Desktop OpenGL"));
        m_configOpenGlBox->setToolTip(tr("Takes effect after restarting QtScrcpy."));
        m_configAdbPathEdit->setPlaceholderText(tr("Use bundled ADB"));
        m_configRenderExpiredCheck->setText(tr("Render expired video frames"));
        m_openConfigButton->setText(tr("Open Config Directory"));
        m_openKeymapButton->setText(tr("Open Keymap Directory"));
        m_saveButton->setText(tr("Save"));
        retranslateFormLabels(m_appConfigGroup->layout(),
                              { tr("Language"), tr("Window title"), tr("Push file path"), tr("Maximum FPS"),
                                tr("Wireless max FPS"), QString(), tr("Decoder threads"), tr("OpenGL backend"),
                                tr("Device server path"), tr("ADB executable"), tr("Codec options"),
                                tr("Codec name"), tr("Log level") });
    }

    // 无线调试运行期设置的 tooltip / placeholder
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setToolTip(
            tr("Wireless debugging: the port after the IP on the device's main screen; leave empty on Android 10 or below to use 5555"));
    }
    ui->pairPortEdt->setToolTip(tr("Optional: port in the device's 'Pair using PIN' dialog, must be filled together with the PIN"));
    ui->pairCodeEdt->setPlaceholderText(tr("6-digit PIN"));
    ui->pairCodeEdt->setToolTip(tr("Optional: 6-digit PIN shown in the device's 'Pair using PIN' dialog, must be filled together with the pair port; used for this pairing only and not saved"));

    // 群控页签
    if (m_groupTabWidget && m_groupTabIndex >= 0) {
        m_groupTabWidget->setTabText(m_groupTabIndex, tr("Group Control"));
    }
    if (m_groupWidget) {
        m_groupWidget->retranslateUi();
    }

    // 录制设置
    m_recordGroupTitle->setTitle(tr("Recording"));
    m_recordAudioCheck->setText(tr("Record audio"));
    m_recordMouseTrackCheck->setText(tr("Overlay mouse track"));
    retranslateFormLabels(m_recordGroupTitle->layout(),
                          { QString(), QString(), tr("Record FPS"), tr("Video bitrate (bps)"),
                            tr("Audio bitrate (bps)") });
}

void Dialog::closeEvent(QCloseEvent *event)
{
    if (ui->quitOnCloseCheck->isChecked()) {
        // 用户选择退出系统托盘：关闭窗口即退出程序。忽略该事件（而非接受），
        // 避免 WA_DeleteOnClose 在此处删除本对话框；main() 会在事件循环返回后删除它。
        m_hideIcon->hide();
        event->ignore();
        qApp->quit();
        return;
    }

    this->hide();
    if (!Config::getInstance().getTrayMessageShown()) {
        Config::getInstance().setTrayMessageShown(true);
        m_hideIcon->showMessage(tr("Notice"),
                                tr("Hidden here!"),
                                QSystemTrayIcon::Information,
                                3000);
    }
    event->ignore();
}

void Dialog::on_updateDevice_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("update devices...", false);
    m_adb.execute("", QStringList() << "devices");
}

void Dialog::updateVideoSourceUi()
{
    const bool camera = ui->videoSourceBox->currentIndex() == qsc::VIDEO_SOURCE_CAMERA;
    ui->cameraFacingLabel->setEnabled(camera);
    ui->cameraFacingBox->setEnabled(camera);
    ui->refreshCameraBtn->setEnabled(camera);
    ui->closeScreenCheck->setEnabled(!camera);
    ui->stayAwakeCheck->setEnabled(!camera);
    ui->gameBox->setEnabled(!camera);
    ui->refreshGameScriptBtn->setEnabled(!camera);
    ui->applyScriptBtn->setEnabled(!camera);
    ui->installSndcpyBtn->setEnabled(!camera);
    ui->startAudioBtn->setEnabled(!camera);
    if (m_advancedDisplayGroup) {
        m_advancedDisplayGroup->setEnabled(!camera);
    }
}

void Dialog::on_videoSourceBox_currentIndexChanged(int)
{
    updateVideoSourceUi();
}

void Dialog::on_startServerBtn_clicked()
{
    startServerFor(ui->serialBox->currentText().trimmed());
}

// 启动一台设备的投屏会话。serial 由调用方给出（按钮 = 当前下拉框选中项；
// 自动重连 = 掉线设备）。其余参数沿用当前界面配置。
void Dialog::startServerFor(const QString &serial)
{
    outLog("start server...", false);

    // 勾选状态实时同步到配置：VideoForm 构造时读 Config.autoOffScreen 决定
    // 是否启用投屏端无操作熄屏，而完整配置保存要等程序关闭（~Dialog），
    // 这里不提前同步的话，本会话内勾选后连接的设备（含多台）全部不生效
    {
        UserBootConfig bootCfg = Config::getInstance().getUserBootConfig();
        bootCfg.autoOffScreen = ui->closeScreenCheck->isChecked();
        Config::getInstance().setUserBootConfig(bootCfg);
    }

    // 这里“original”的 toUShort 结果为 0 也是可接受的情况
    quint16 videoSize = ui->maxSizeBox->currentText().trimmed().toUShort();
    qsc::DeviceParams params;
    params.serial = serial;
    params.maxSize = videoSize;
    // 比特率按连接方式区分：无线设备 serial 形如 ip:port，用无线专用档位；
    // USB 设备用主档位。（编码预设若自带码率，下面会覆盖该值）
    const bool wifiDevice = params.serial.contains(QLatin1Char(':'));
    params.bitRate = wifiDevice ? getWirelessBitRate() : getBitRate();
    outLog(tr("bitrate: %1 Mbps (%2)").arg(params.bitRate / 1000000.0, 0, 'g', 4)
               .arg(wifiDevice ? tr("wireless") : tr("usb")), false);
    // on devices with Android >= 10, the capture frame rate can be limited
    // 帧率上限按连接方式区分：无线默认 30（受限通道防积压，见 config.cpp 注释）
    params.maxFps = static_cast<quint32>(wifiDevice ? Config::getInstance().getWirelessMaxFps()
                                                    : Config::getInstance().getMaxFps());
    outLog(tr("max fps: %1 (%2)").arg(params.maxFps)
               .arg(params.maxFps ? tr("limited") : tr("unlimited")), false);
    params.decodeThreads = Config::getInstance().getDecoderThreads();
    // 控制台 FPS 诊断日志跟随"显示 FPS"开关
    params.fpsLogEnabled = ui->fpsCheck->isChecked();
    params.videoSource = static_cast<qsc::VideoSource>(ui->videoSourceBox->currentIndex());
    params.cameraFacing = ui->cameraFacingBox->currentIndex() == 1
            ? qsc::CAMERA_FACING_FRONT
            : qsc::CAMERA_FACING_BACK;
    params.cameraId = ui->cameraFacingBox->currentData().toString();
    const bool camera = params.videoSource == qsc::VIDEO_SOURCE_CAMERA;
    params.closeScreen = !camera && ui->closeScreenCheck->isChecked();
    params.useReverse = ui->useReverseCheck->isChecked();
    params.display = !ui->notDisplayCheck->isChecked();
    params.renderExpiredFrames = Config::getInstance().getRenderExpiredFrames();
    if (ui->lockOrientationBox->currentIndex() > 0) {
        params.captureOrientationLock = 1;
        params.captureOrientation = (ui->lockOrientationBox->currentIndex() - 1) * 90;
    }
    // 摄像头传感器默认以横屏方向输出视频流。在此把默认
    // 摄像头预览旋转为竖屏，同时保留用户显式
    // 选择的画面方向。
    if (camera && params.captureOrientationLock == 0) {
        params.captureOrientation = 90;
    }
    params.stayAwake = !camera && ui->stayAwakeCheck->isChecked();
    params.recordFile = ui->recordScreenCheck->isChecked();
    params.recordPath = ui->recordPathEdt->text().trimmed();
    params.recordFileFormat = ui->formatBox->currentText().trimmed();
    params.serverLocalPath = getServerPath();
    params.serverRemotePath = Config::getInstance().getServerPath();
    params.pushFilePath = Config::getInstance().getPushFilePath();
    params.gameScript = camera ? QString() : getGameScript(ui->gameBox->currentText());
    params.logLevel = Config::getInstance().getLogLevel();
    // 应用编码预设（选中预设模式时）或 ini 中的默认值。
    const int codecModeIndex = ui->codecModeBox->currentIndex();
    if (codecModeIndex > 0) {
        const EncoderPreset &preset = EncoderPresetRegistry::all().at(codecModeIndex - 1);
        // 优先取打开对话框的选择，否则回退到已保存的档位。
        int level = 1;
        if (m_presetDialog) {
            level = m_presetDialog->selectedLevel();
        } else {
            level = Config::getInstance().getUserBootConfig().presetLevel;
        }
        QString codecOptions;
        quint32 bitRate = 0;
        preset.buildParams(level, codecOptions, bitRate);
        params.codecOptions = codecOptions;
        if (!preset.codecName.isEmpty()) {
            params.codecName = preset.codecName;
        }
        // 编码预设的码率只作用于 USB：无线（Wi-Fi）链路上预设的高码率
        // 会直接导致卡顿，因此无线始终使用「无线比特率」档位
        if (bitRate > 0 && !wifiDevice) {
            params.bitRate = bitRate;
        }
    } else {
        params.codecOptions = Config::getInstance().getCodecOptions();
        params.codecName = Config::getInstance().getCodecName();
    }
    params.scid = QRandomGenerator::global()->bounded(1, 10000) & 0x7FFFFFFF;
    params.decodeMode = ui->decodeModeBox->currentIndex();
    // 被禁用的控件可能因持久化设置而仍处于选中状态。高级
    // 显示选项对摄像头采集无效，因此绝不把它们
    // 写入摄像头会话参数。
    if (!camera && m_advancedDisplayGroup && m_advancedDisplayGroup->isChecked()) {
        const int displayMode = m_displayModeBox->currentIndex();
        if (displayMode == 1) {
            bool ok = false;
            const int displayId = m_displayIdEdit->text().trimmed().toInt(&ok);
            if (!ok || displayId < 0) {
                outLog(tr("invalid display ID"));
                return;
            }
            params.displayId = displayId;
        } else if (displayMode == 2) {
            params.newDisplay = m_newDisplayEdit->text().trimmed();
            if (params.newDisplay.isEmpty()) {
                // 与 scrcpy 的 flex 默认值（160 dpi 下 1280x960）保持一致，
                // 同时保留显式的命令行参数。
                params.newDisplay = "1280x960/160";
            }
            params.flexDisplay = m_flexDisplayCheck->isChecked();
            params.vdSystemDecorations = m_vdSystemDecorationsCheck->isChecked();
            params.vdDestroyContent = m_vdDestroyContentCheck->isChecked();
            params.displayImePolicy = m_displayImePolicyBox->currentData().toString();
        }
        params.keepActive = m_keepActiveCheck->isChecked();
        params.startApp = m_startAppBox->currentData().toString();
        if (params.startApp.isEmpty()) {
            params.startApp = m_startAppBox->currentText().trimmed();
        }
        // scrcpy 禁止在 flex 显示模式下裁剪。保留用户输入的值供后续非 flex
        // 会话使用，但绝不将其传给服务端。
        params.crop = params.flexDisplay ? QString() : m_cropEdit->text().trimmed();
    }
    if (params.flexDisplay && (params.newDisplay.isEmpty() || !params.display || !params.crop.isEmpty())) {
        outLog(tr("flex display requires video, a new virtual display, and no crop"));
        return;
    }

    const bool needsVirtualDisplayCheck = !params.newDisplay.isEmpty();
    if (!camera && !needsVirtualDisplayCheck) {
        qsc::IDeviceManage::getInstance().connectDevice(params);
        return;
    }

    auto *versionAdb = new qsc::AdbProcess(this);
    connect(versionAdb, &qsc::AdbProcess::adbProcessResult, this,
            [this, versionAdb, camera, params](qsc::AdbProcess::ADB_EXEC_RESULT result) {
        if (result == qsc::AdbProcess::AER_SUCCESS_EXEC) {
            bool ok = false;
            const int sdk = versionAdb->getStdOut().trimmed().toInt(&ok);
            const int minimumSdk = camera ? 31 : 29;
            if (ok && sdk >= minimumSdk) {
                qsc::IDeviceManage::getInstance().connectDevice(params);
            } else {
                outLog(camera ? tr("camera preview requires Android 12 or later")
                              : tr("virtual display requires Android 10 or later"));
            }
            versionAdb->deleteLater();
        } else if (result == qsc::AdbProcess::AER_ERROR_EXEC
                   || result == qsc::AdbProcess::AER_ERROR_START
                   || result == qsc::AdbProcess::AER_ERROR_MISSING_BINARY) {
            outLog(tr("could not verify Android version for camera preview"));
            versionAdb->deleteLater();
        }
    });
    versionAdb->execute(params.serial, QStringList() << "shell" << "getprop" << "ro.build.version.sdk");
}

void Dialog::on_refreshCameraBtn_clicked()
{
    const QString serial = ui->serialBox->currentText().trimmed();
    if (serial.isEmpty()) {
        outLog(tr("no device"));
        return;
    }
    if (qsc::IDeviceManage::getInstance().getDevice(serial)) {
        outLog(tr("stop preview first"));
        return;
    }

    ui->refreshCameraBtn->setEnabled(false);
    ui->refreshCameraBtn->setText("...");

    auto *pushAdb = new qsc::AdbProcess(this);
    connect(pushAdb, &qsc::AdbProcess::adbProcessResult, this,
            [this, pushAdb, serial](qsc::AdbProcess::ADB_EXEC_RESULT result) {
        if (result == qsc::AdbProcess::AER_SUCCESS_EXEC) {
            pushAdb->deleteLater();

            auto *listAdb = new qsc::AdbProcess(this);
            connect(listAdb, &qsc::AdbProcess::adbProcessResult, this,
                    [this, listAdb](qsc::AdbProcess::ADB_EXEC_RESULT listResult) {
                if (listResult == qsc::AdbProcess::AER_SUCCESS_START) {
                    return;
                }
                ui->refreshCameraBtn->setText(tr("refresh"));
                ui->refreshCameraBtn->setEnabled(ui->videoSourceBox->currentIndex() == qsc::VIDEO_SOURCE_CAMERA);

                if (listResult != qsc::AdbProcess::AER_SUCCESS_EXEC) {
                    outLog(tr("camera refresh failed"));
                    listAdb->deleteLater();
                    return;
                }

                const QString output = listAdb->getStdOut() + '\n' + listAdb->getErrorOut();
                const QRegularExpression pattern(R"(--camera-id=(\S+)\s+\(([^,\)]+))");
                QRegularExpressionMatchIterator matches = pattern.globalMatch(output);

                const int facingIndex = ui->cameraFacingBox->currentIndex();
                ui->cameraFacingBox->clear();
                ui->cameraFacingBox->addItem(tr("back"));
                ui->cameraFacingBox->addItem(tr("front"));

                int cameraCount = 0;
                while (matches.hasNext()) {
                    const QRegularExpressionMatch match = matches.next();
                    const QString cameraId = match.captured(1);
                    const QString facing = match.captured(2).trimmed();
                    ui->cameraFacingBox->addItem(QString("%1 (%2)").arg(cameraId, facing), cameraId);
                    ++cameraCount;
                }

                ui->cameraFacingBox->setCurrentIndex(qMin(facingIndex, ui->cameraFacingBox->count() - 1));
                outLog(cameraCount ? tr("camera refreshed") : tr("no camera"));
                listAdb->deleteLater();
            });

            QStringList args;
            args << "shell";
            args << QString("CLASSPATH=%1").arg(Config::getInstance().getServerPath());
            args << "app_process" << "/" << "com.genymobile.scrcpy.Server" << "4.1";
            args << "list_cameras=true" << "log_level=info";
            listAdb->execute(serial, args);
        } else if (result == qsc::AdbProcess::AER_ERROR_EXEC
                   || result == qsc::AdbProcess::AER_ERROR_START
                   || result == qsc::AdbProcess::AER_ERROR_MISSING_BINARY) {
            ui->refreshCameraBtn->setText(tr("refresh"));
            ui->refreshCameraBtn->setEnabled(ui->videoSourceBox->currentIndex() == qsc::VIDEO_SOURCE_CAMERA);
            outLog(tr("camera refresh failed"));
            pushAdb->deleteLater();
        }
    });
    pushAdb->push(serial, getServerPath(), Config::getInstance().getServerPath());
}

void Dialog::refreshApps()
{
    const QString serial = ui->serialBox->currentText().trimmed();
    if (serial.isEmpty()) {
        outLog(tr("no device"));
        return;
    }
    if (qsc::IDeviceManage::getInstance().getDevice(serial)) {
        outLog(tr("stop server first"));
        return;
    }

    m_refreshAppsBtn->setEnabled(false);
    m_refreshAppsBtn->setText("...");

    auto restoreRefreshButton = [this]() {
        m_refreshAppsBtn->setText(tr("refresh"));
        m_refreshAppsBtn->setEnabled(ui->videoSourceBox->currentIndex() == qsc::VIDEO_SOURCE_DISPLAY);
    };

    auto *pushAdb = new qsc::AdbProcess(this);
    connect(pushAdb, &qsc::AdbProcess::adbProcessResult, this,
            [this, pushAdb, serial, restoreRefreshButton](qsc::AdbProcess::ADB_EXEC_RESULT result) {
        if (result == qsc::AdbProcess::AER_SUCCESS_EXEC) {
            pushAdb->deleteLater();

            auto *listAdb = new qsc::AdbProcess(this);
            connect(listAdb, &qsc::AdbProcess::adbProcessResult, this,
                    [this, listAdb, restoreRefreshButton](qsc::AdbProcess::ADB_EXEC_RESULT listResult) {
                if (listResult == qsc::AdbProcess::AER_SUCCESS_START) {
                    return;
                }
                restoreRefreshButton();

                if (listResult != qsc::AdbProcess::AER_SUCCESS_EXEC) {
                    outLog(tr("app refresh failed"));
                    listAdb->deleteLater();
                    return;
                }

                const QString selected = m_startAppBox->currentData().toString().isEmpty()
                        ? m_startAppBox->currentText().trimmed()
                        : m_startAppBox->currentData().toString();
                const QString output = listAdb->getStdOut() + '\n' + listAdb->getErrorOut();
                const QRegularExpression pattern(
                        R"(^\s*[\*\-]\s+(.+?)\s+([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+)\s*$)",
                        QRegularExpression::MultilineOption);
                QRegularExpressionMatchIterator matches = pattern.globalMatch(output);

                m_startAppBox->clear();
                m_startAppBox->addItem(QString(), QString());
                int appCount = 0;
                while (matches.hasNext()) {
                    const QRegularExpressionMatch match = matches.next();
                    const QString name = match.captured(1).trimmed();
                    const QString packageName = match.captured(2);
                    m_startAppBox->addItem(QString("%1 (%2)").arg(name, packageName), packageName);
                    ++appCount;
                }

                const int selectedIndex = m_startAppBox->findData(selected);
                if (selectedIndex >= 0) {
                    m_startAppBox->setCurrentIndex(selectedIndex);
                } else {
                    m_startAppBox->setEditText(selected);
                }
                outLog(appCount ? tr("apps refreshed") : tr("no launchable app"));
                listAdb->deleteLater();
            });

            QStringList args;
            args << "shell";
            args << QString("CLASSPATH=%1").arg(Config::getInstance().getServerPath());
            args << "app_process" << "/" << "com.genymobile.scrcpy.Server" << "4.1";
            args << "list_apps=true" << "log_level=info";
            listAdb->execute(serial, args);
        } else if (result == qsc::AdbProcess::AER_ERROR_EXEC
                   || result == qsc::AdbProcess::AER_ERROR_START
                   || result == qsc::AdbProcess::AER_ERROR_MISSING_BINARY) {
            restoreRefreshButton();
            outLog(tr("app refresh failed"));
            pushAdb->deleteLater();
        }
    });
    pushAdb->push(serial, getServerPath(), Config::getInstance().getServerPath());
}

void Dialog::on_stopServerBtn_clicked()
{
    const QString serial = ui->serialBox->currentText().trimmed();
    m_manualDisconnects.insert(serial); // 主动断开：不做自动重连
    if (qsc::IDeviceManage::getInstance().disconnectDevice(serial)) {
        outLog("stop server");
    }
}

void Dialog::on_wirelessConnectBtn_clicked()
{
    // 控制器使用独立的 adb 进程 + 超时兜底，不再与主界面命令抢 m_adb 的单实例通道
    if (m_wireless->isBusy()) {
        outLog(tr("无线操作进行中，请稍候…"));
        return;
    }

    const QString ip = ui->deviceIpEdt->currentText().trimmed();
    if (ip.isEmpty()) {
        outLog("error: device ip is null", false);
        return;
    }
    if (!WirelessController::isValidHost(ip)) {
        outLog(tr("error: 设备 IP 不合法"), false);
        return;
    }

    const quint16 connectPort = wirelessConnectPort();
    const QString pairPortText = ui->pairPortEdt->currentText().trimmed();
    const QString code = ui->pairCodeEdt->text().trimmed();

    saveIpHistory(ip);
    savePortHistory(QString::number(connectPort));

    WirelessController::Target target;
    target.ip = ip;
    target.connectPort = connectPort;

    if (!pairPortText.isEmpty() || !code.isEmpty()) {
        // Android 11+ 无线调试：配对端口与配对码是成对的选填项——填一个就必须都填
        if (pairPortText.isEmpty() || code.isEmpty()) {
            outLog(tr("error: 配对端口与配对码需同时填写（或都留空仅做直连）"), false);
            QMessageBox::warning(this, tr("信息不完整"),
                                 tr("配对端口与配对码是一组配对信息，需要同时填写；若设备已配对过，请将两项都清空后直接连接。"));
            return;
        }
        const uint pairPort = pairPortText.toUInt();
        if (0 == pairPort || pairPort > 65535) {
            outLog(tr("error: 配对端口不合法，请填写手机「使用配对码配对设备」弹窗中的端口"), false);
            return;
        }
        target.pairPort = quint16(pairPort);
        m_wireless->pairAndConnect(target, code);
    } else {
        // Android 10 及以下 / 已配对设备：直接连接（连接端口留空按 5555 处理）
        m_wireless->connectOnly(target);
    }
}

void Dialog::on_startAdbdBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("start devices adbd...", false);
    // 通过 adb 开启 tcpip 监听（端口 5555）
    QStringList adbArgs;
    adbArgs << "tcpip";
    adbArgs << "5555";
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::outLog(const QString &log, bool newLine)
{
    // 避免子线程直接更新界面
    QString backLog = log;
    QTimer::singleShot(0, this, [this, backLog, newLine]() {
        ui->outEdit->append(backLog);
        if (newLine) {
            ui->outEdit->append("<br/>");
        }
    });
}

bool Dialog::filterLog(const QString &log)
{
    if (log.contains("app_proces")) {
        return true;
    }
    if (log.contains("Unable to set geometry")) {
        return true;
    }
    return false;
}

bool Dialog::checkAdbRun()
{
    if (m_adb.isRuning()) {
        outLog("wait for the end of the current command to run");
    }
    return m_adb.isRuning();
}

void Dialog::on_getIPBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }

    outLog("get ip...", false);
    // 示例：adb -s ... shell ifconfig wlan0（查询无线网卡地址）
    // or
    // 示例：adb -s ... shell ip -f inet addr show wlan0（查询 IP 地址）
    QStringList adbArgs;
#if 0
    adbArgs << "shell";
    adbArgs << "ip";
    adbArgs << "-f";
    adbArgs << "inet";
    adbArgs << "addr";
    adbArgs << "show";
    adbArgs << "wlan0";
#else
    adbArgs << "shell";
    adbArgs << "ifconfig";
    adbArgs << "wlan0";
#endif
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::getIPbyIp()
{
    if (checkAdbRun()) {
        return;
    }

    QStringList adbArgs;
    adbArgs << "shell";
    adbArgs << "ip -o a";

    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    Q_UNUSED(deviceName);
    if (!success) {
        // 服务端启动失败（内部已自动重试若干次）：原来静默返回，用户只能看到
        // 镜像窗口始终打不开，被感知为"启动不成功且没有原因"。这里给出明确反馈。
        const bool autoRetry = (serial == m_reconnectSerial);
        if (autoRetry) {
            // 自动重连中的失败只记日志，下一轮定时器会继续尝试；
            // 不弹托盘，避免重试期间连发多条“启动失败”通知
            outLog(tr("auto reconnect attempt failed for %1, will retry").arg(serial), false);
        } else {
            const QString message = tr("设备 %1 启动投屏服务失败，请检查 USB 连接后重试").arg(serial);
            outLog(QString("[错误] %1").arg(message), true);
            if (m_hideIcon) {
                m_hideIcon->showMessage(tr("投屏服务启动失败"), message, QSystemTrayIcon::Warning, 6000);
            }
        }
        // 启动失败不视为“意外掉线”，避免自动重连逻辑对从未连接过的设备反复重试
        m_connectedSerials.remove(serial);
        return;
    }
    m_connectedSerials.insert(serial);
    if (m_groupWidget) {
        m_groupWidget->syncDevices(m_connectedSerials.values());
    }
    if (serial == m_reconnectSerial) {
        stopAutoReconnect(); // 掉线设备已自动恢复
    }
    auto videoForm = new VideoForm(ui->framelessCheck->isChecked(), ui->showToolbar->isChecked(), ui->decodeModeBox->currentIndex());
    videoForm->setSerial(serial);
    // 用户点 X 关闭投屏窗口 = 主动断开：标记豁免，防止自动重连把窗口复活。
    // 掉线清理路径（onDeviceDisconnected）也会 close 窗口，但会先把自己登记到
    // m_internalClosing：那种 close 不代表用户操作，不得触发豁免
    connect(videoForm, &VideoForm::windowClosedByUser, this, [this](const QString &closedSerial) {
        if (m_internalClosing.contains(closedSerial)) {
            m_internalClosing.remove(closedSerial); // 程序清理引发的 close，忽略
            return;
        }
        m_manualDisconnects.insert(closedSerial);
        if (closedSerial == m_reconnectSerial) {
            stopAutoReconnect();
        }
    });

    qsc::IDeviceManage::getInstance().getDevice(serial)->setUserData(static_cast<void*>(videoForm));
    qsc::IDeviceManage::getInstance().getDevice(serial)->registerDeviceObserver(videoForm);


    videoForm->showFPS(ui->fpsCheck->isChecked());

    if (ui->alwaysTopCheck->isChecked()) {
        videoForm->staysOnTop();
    }

#ifndef Q_OS_WIN32
    // 必须在 updateShowSize 之前显示
    videoForm->show();
#endif
    QString name = Config::getInstance().getNickName(serial);
    if (name.isEmpty()) {
        name = Config::getInstance().getTitle();
    }
    videoForm->setWindowTitle(name + "-" + serial);
    videoForm->updateShowSize(size);

    bool deviceVer = size.height() > size.width();
    QRect rc = Config::getInstance().getRect(serial);
    bool rcVer = rc.height() > rc.width();
    // 宽度与高度比例相同
    if (rc.isValid() && (deviceVer == rcVer)) {
        // 备注：resize 用于修复 setGeometry 与 magneticwidget 的兼容问题
        videoForm->resize(rc.size());
        videoForm->setGeometry(rc);
    }

#ifdef Q_OS_WIN32
    // windows是show太早可以看到resize的过程
    QTimer::singleShot(200, videoForm, [videoForm](){videoForm->show();});
#endif

    GroupController::instance().addDevice(serial);

    // 关闭设备窗口会彻底拆除 sndcpy 音频桥接
    // (VideoForm::closeEvent -> AudioOutput::stopForDevice). Nothing used to
    // 重新连接时重建桥接，否则刚连接的设备会一直静音，
    // 直到点击“开始音频”或开始录制才意外启动桥接。
    // 稍作延迟，让视频窗口可见后再执行（阻塞的）sndcpy 初始化；
    // 以窗口作为上下文，可在设备被立即关闭时取消该延时任务。
    QTimer::singleShot(300, videoForm, [serial]() {
        if (!Config::getInstance().getUserBootConfig().autoStartAudio) {
            return;
        }
        auto &audio = AudioOutput::instance();
        if (audio.isRunning(serial)) {
            qInfo() << "audio bridge already active for" << serial << ", skip auto start";
            return;
        }
        if (!qsc::IDeviceManage::getInstance().getDevice(serial)) {
            return; // 延迟期间设备已消失
        }
        // 每台设备独立会话/端口：第二台不再因第一台占用单桥而静音
        if (!audio.start(serial)) {
            qWarning() << "auto start audio failed for" << serial;
            return;
        }
        qInfo() << "auto start audio success for" << serial << "on port" << audio.portOf(serial);
    });
}

void Dialog::onDeviceDisconnected(QString serial)
{
    // 覆盖所有断开路径（关闭窗口、停止服务、拔线）：
    // 绝不能让已消失设备的 sndcpy 音频桥接继续播放。
    AudioOutput::instance().stopForDevice(serial);
    GroupController::instance().removeDevice(serial);

    // 文件管理窗口依附于设备会话：设备断开即关闭，避免对失效设备继续操作
    if (m_fileWindows.contains(serial)) {
        QPointer<FileManagerWindow> win = m_fileWindows.take(serial);
        if (win) {
            win->close();
        }
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }
    auto data = device->getUserData();
    if (data) {
        VideoForm* vf = static_cast<VideoForm*>(data);
        qsc::IDeviceManage::getInstance().getDevice(serial)->deRegisterDeviceObserver(vf);
        // close 会触发 VideoForm::closeEvent → windowClosedByUser；
        // 这是掉线清理而非用户操作，先登记避免被误标记为手动断开
        m_internalClosing.insert(serial);
        vf->close();
        m_internalClosing.remove(serial); // close 同步触发 closeEvent 并被 lambda 消费；若窗口已在关闭流程则清除残留
        vf->deleteLater();
    }

    // ---- 自动重连（仅意外掉线）----
    m_connectedSerials.remove(serial);
    if (m_groupWidget) {
        m_groupWidget->deviceDisconnected(serial);
    }
    if (m_manualDisconnects.contains(serial)) {
        m_manualDisconnects.remove(serial); // 手动断开：一次标记只豁免一次
        return;
    }
    // 教学投屏常见痛点：USB 抖动/无线波动导致会话被销毁，必须从主窗口
    // 手动重连。这里保留窗口生命周期外的自动恢复：3s × 3 次重试。
    m_reconnectSerial = serial;
    m_reconnectTries = 5;
    m_reconnectTimer.start();
    outLog(tr("device %1 disconnected, auto reconnecting...").arg(serial), false);
}

void Dialog::tryReconnect()
{
    if (m_reconnectSerial.isEmpty()) {
        return;
    }
    const QString serial = m_reconnectSerial;
    // 判定用 m_connectedSerials 而非 getDevice：上一轮 startServerFor 若
    // 还在异步失败中，其 Device 仍留在设备表里，getDevice 非空但会话
    // 并未建立；只有 deviceConnected(true) 才会置位该集合。
    if (m_connectedSerials.contains(serial)) {
        stopAutoReconnect(); // 已被用户手动重连/本轮重连成功
        return;
    }
    if (m_reconnectTries <= 0) {
        outLog(tr("auto reconnect failed for %1, please reconnect manually").arg(serial), false);
        stopAutoReconnect();
        return;
    }
    --m_reconnectTries;

    // USB 设备直接重建会话；无线设备（serial = ip:port）先恢复 adb 连接。
    // 本轮无论成功与否，末尾都重启定时器：成功与否由下一轮开头的
    // getDevice 检查判定（startServerFor 的结果是异步的，无法就地判断）。
    const bool wifi = serial.contains(QLatin1Char(':'));
    if (!wifi) {
        startServerFor(serial);
        if (!m_reconnectSerial.isEmpty()) {
            m_reconnectTimer.start(); // 下一轮检查/重试
        }
        return;
    }
    auto *adb = new qsc::AdbProcess(this);
    adb->executeWithTimeout(serial, QStringList() << "connect" << serial, 8000,
                            [this, adb, serial](bool ok, bool timedOut, const QString &out) {
        adb->deleteLater();
        if (m_reconnectSerial.isEmpty()) {
            return; // 重连已被用户操作取代（手动连接/手动停止）
        }
        if (ok && !timedOut && out.contains(QLatin1String("connected to"), Qt::CaseInsensitive)) {
            outLog(tr("adb connect to %1 success, restarting session").arg(serial), false);
            startServerFor(serial);
        } else {
            outLog(tr("adb connect to %1 failed (%2), retrying...")
                       .arg(serial, out.trimmed().section(QLatin1Char('\n'), -1)), false);
        }
        if (!m_reconnectSerial.isEmpty()) {
            m_reconnectTimer.start(); // 下一轮检查/重试
        }
    });
}

void Dialog::stopAutoReconnect()
{
    m_reconnectSerial.clear();
    m_reconnectTries = 0;
    m_reconnectTimer.stop();
}

void Dialog::on_fileManagerBtn_clicked()
{
    const QString serial = ui->serialBox->currentText().trimmed();
    if (serial.isEmpty()) {
        QMessageBox::warning(this, tr("文件管理"), tr("请先选择一台已连接的设备"));
        return;
    }

    if (m_fileWindows.contains(serial)) {
        QPointer<FileManagerWindow> existing = m_fileWindows.value(serial);
        if (existing) {
            existing->showNormal();
            existing->raise();
            existing->activateWindow();
            return;
        }
        m_fileWindows.remove(serial);
    }

    auto *window = new FileManagerWindow(serial, this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    m_fileWindows.insert(serial, window);
    connect(window, &QObject::destroyed, this, [this, serial]() {
        m_fileWindows.remove(serial);
    });
    window->show();
    window->raise();
    window->activateWindow();
}

void Dialog::on_wirelessDisConnectBtn_clicked()
{
    if (m_wireless->isBusy()) {
        outLog(tr("无线操作进行中，请稍候…"));
        return;
    }
    const QString ip = ui->deviceIpEdt->currentText().trimmed();
    if (ip.isEmpty()) {
        outLog("error: device ip is null", false);
        return;
    }
    // 带上端口断开：只传 IP 会把同一 IP 的其它端口一起断掉（旧实现的问题）
    const QString addr = ip + QLatin1Char(':') + QString::number(wirelessConnectPort());
    m_manualDisconnects.insert(addr); // 用户主动断开无线：掉线回调不做自动重连
    WirelessController::Target target;
    target.ip = ip;
    target.connectPort = wirelessConnectPort();
    m_wireless->disconnectTarget(target);
}

void Dialog::setupWirelessUi()
{
    // 端口提示与输入约束
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setToolTip(
            tr("无线调试：「无线调试」主界面 IP 地址后的端口；Android 10 及以下留空按 5555"));
    }
    // 配对端口/配对码是成对的选填项：仅首次连接 Android 11+ 设备时填写
    if (ui->pairPortEdt->lineEdit()) {
        auto *lineEdit = ui->pairPortEdt->lineEdit();
        lineEdit->setFrame(false);
        lineEdit->setTextMargins(0, 0, 0, 0);
        lineEdit->setMaxLength(6);
        lineEdit->setValidator(new QIntValidator(1, 65535, this));
        // 空 item 列表的可编辑 QComboBox 内部 lineEdit 会被压得很窄，
        // 导致输入的数字只露出几位；按 6 位数字宽度兜底，保证完整可见
        lineEdit->setMinimumWidth(lineEdit->fontMetrics().horizontalAdvance(QStringLiteral("000000")) + 12);
    }
    ui->pairPortEdt->setToolTip(tr("选填：手机「无线调试 → 使用配对码配对设备」弹窗中的端口，需与配对码同时填写"));
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setValidator(new QIntValidator(1, 65535, this));
    }
    ui->pairCodeEdt->setPlaceholderText(tr("6 位数字"));
    ui->pairCodeEdt->setToolTip(tr("选填：手机「无线调试 → 使用配对码配对设备」中显示的 6 位配对码，需与配对端口同时填写；仅用于本次配对，不会被保存"));
    ui->pairCodeEdt->setValidator(new QIntValidator(0, 999999999, this));

    m_wireless = new WirelessController(&m_adb, this);
    connect(m_wireless, &WirelessController::stateChanged, this, &Dialog::onWirelessStateChanged);
    connect(m_wireless, &WirelessController::paired, this, &Dialog::onWirelessPaired);
    connect(m_wireless, &WirelessController::failed, this, &Dialog::onWirelessFailed);
    connect(m_wireless, &WirelessController::connected, this, &Dialog::onWirelessConnected);

    // 带入上次成功连接的无线目标（不含配对码）
    const QStringList targets = Config::getInstance().getWirelessTargets();
    if (!targets.isEmpty()) {
        const QStringList fields = targets.first().split('|');
        if (fields.size() >= 3) {
            applyWirelessTargetToUi(fields.at(0), fields.at(1).toUShort(), fields.at(2).toUShort());
        }
    }
    updateWirelessControls();
}

quint16 Dialog::wirelessConnectPort() const
{
    const QString text = ui->devicePortEdt->currentText().trimmed();
    if (text.isEmpty()) {
        // 老流程（USB + tcpip）沿用 5555 默认端口
        return 5555;
    }
    const uint port = text.toUInt();
    return (port > 0 && port <= 65535) ? quint16(port) : 0;
}

void Dialog::applyWirelessTargetToUi(const QString &ip, quint16 connectPort, quint16 pairPort)
{
    Q_UNUSED(pairPort) // 配对是一次性的，历史记录不再带出配对端口/配对码
    ui->deviceIpEdt->setCurrentText(ip);
    if (connectPort > 0) {
        ui->devicePortEdt->setCurrentText(QString::number(connectPort));
    }
}

void Dialog::updateWirelessControls()
{
    const bool busy = m_wireless && m_wireless->isBusy();
    ui->wirelessConnectBtn->setEnabled(!busy);
    ui->wirelessDisConnectBtn->setEnabled(!busy);
}

void Dialog::on_deviceIpEdt_activated(int index)
{
    Q_UNUSED(index)
    // 用户从历史里选了 IP：带出该 IP 最近一次成功使用的端口
    const QString ip = ui->deviceIpEdt->currentText().trimmed();
    for (const QString &entry : Config::getInstance().getWirelessTargets()) {
        const QStringList fields = entry.split('|');
        if (fields.size() >= 3 && fields.at(0) == ip) {
            applyWirelessTargetToUi(fields.at(0), fields.at(1).toUShort(), fields.at(2).toUShort());
            break;
        }
    }
}

void Dialog::onWirelessStateChanged(int state, const QString &text)
{
    static const QMap<int, QString> colors = {
        {int(WirelessController::Idle), "#9e9e9e"},
        {int(WirelessController::Discovering), "#42a5f5"},
        {int(WirelessController::Pairing), "#42a5f5"},
        {int(WirelessController::Connecting), "#42a5f5"},
        {int(WirelessController::Connected), "#4caf50"},
        {int(WirelessController::Failed), "#e57373"},
    };
    ui->wirelessStateLabel->setStyleSheet(QString("color:%1;").arg(colors.value(state, "#ffffff")));
    // 状态行宽度固定（Ignoring 策略）：超长文本截断省略显示，悬停 tooltip 看全文，
    // 避免连接失败等原因文案把无线区布局撑爆
    const QString shown = ui->wirelessStateLabel->fontMetrics().elidedText(
        text, Qt::ElideRight, qMax(80, ui->wirelessStateLabel->width() - 4));
    ui->wirelessStateLabel->setText(shown);
    ui->wirelessStateLabel->setToolTip(text);
    outLog(text);
    updateWirelessControls();
}

void Dialog::onWirelessPaired(const QString &pairAddr)
{
    outLog(tr("配对成功：%1").arg(pairAddr));
}

void Dialog::onWirelessFailed(const QString &reason)
{
    outLog(tr("无线调试失败：%1").arg(reason));
    QMessageBox::warning(this, tr("无线调试失败"), reason);
}

void Dialog::onWirelessConnected(const QString &addr)
{
    // 配对是一次性的：连接成功后清空配对端口与配对码，避免下次误触发配对流程
    ui->pairPortEdt->clearEditText();
    ui->pairCodeEdt->clear();

    on_updateDevice_clicked();
    // 连接成功后直接在设备列表里选中该 IP:port，省去用户再点一次
    const int index = ui->serialBox->findText(addr);
    if (index >= 0) {
        ui->serialBox->setCurrentIndex(index);
    }
}

void Dialog::on_selectRecordPathBtn_clicked()
{
    QFileDialog::Options options = QFileDialog::DontResolveSymlinks | QFileDialog::ShowDirsOnly;
    QString directory = QFileDialog::getExistingDirectory(this, tr("select path"), "", options);
    ui->recordPathEdt->setText(directory);
}

void Dialog::on_recordPathEdt_textChanged(const QString &arg1)
{
    ui->recordPathEdt->setToolTip(arg1.trimmed());
    ui->notDisplayCheck->setCheckable(!arg1.trimmed().isEmpty());
}

void Dialog::on_adbCommandBtn_clicked()
{
    execAdbCmd();
}

void Dialog::on_stopAdbBtn_clicked()
{
    m_adb.kill();
}

void Dialog::on_clearOut_clicked()
{
    ui->outEdit->clear();
}

void Dialog::on_stopAllServerBtn_clicked()
{
    // 先标记全部为手动断开，防止掉线回调触发自动重连
    for (const QString &serial : m_connectedSerials) {
        m_manualDisconnects.insert(serial);
    }
    m_reconnectSerial.clear();
    m_reconnectTimer.stop();
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
}

void Dialog::on_refreshGameScriptBtn_clicked()
{
    ui->gameBox->clear();
    initializeKeyMapDirectory();
    const QDir userDir(getKeyMapPath());
    const QDir defaultDir(getDefaultKeyMapPath());
    if (!userDir.exists() && !defaultDir.exists()) {
        outLog("keymap directory not find", true);
        return;
    }
    QSet<QString> fileNames;
    const QList<QDir> directories {userDir, defaultDir};
    for (const QDir &dir : directories) {
        const QFileInfoList list = dir.entryInfoList(QStringList() << "*.json", QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo &fileInfo : list) {
            if (!fileNames.contains(fileInfo.fileName())) {
                fileNames.insert(fileInfo.fileName());
                ui->gameBox->addItem(fileInfo.fileName());
            }
        }
    }
}

void Dialog::on_applyScriptBtn_clicked()
{
    auto curSerial = ui->serialBox->currentText().trimmed();
    auto device = qsc::IDeviceManage::getInstance().getDevice(curSerial);
    if (!device) {
        return;
    }

    device->updateScript(getGameScript(ui->gameBox->currentText()));
}

void Dialog::on_recordScreenCheck_clicked(bool checked)
{
    if (!checked) {
        return;
    }

    QString fileDir(ui->recordPathEdt->text().trimmed());
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        ui->recordScreenCheck->setChecked(false);
    }
}

void Dialog::on_usbConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);
    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_startServerBtn_clicked();
}

int Dialog::findDeviceFromeSerialBox(bool wifi)
{
    QString regStr = "\\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\:([0-9]|[1-9]\\d|[1-9]\\d{2}|[1-9]\\d{3}|[1-5]\\d{4}|6[0-4]\\d{3}|65[0-4]\\d{2}|655[0-2]\\d|6553[0-5])\\b";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp regIP(regStr);
#else
    QRegularExpression regIP(regStr);
#endif
    for (int i = 0; i < ui->serialBox->count(); ++i) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        bool isWifi = regIP.exactMatch(ui->serialBox->itemText(i));
#else
        bool isWifi = regIP.match(ui->serialBox->itemText(i)).hasMatch();
#endif
        bool found = wifi ? isWifi : !isWifi;
        if (found) {
            return i;
        }
    }

    return -1;
}

void Dialog::on_wifiConnectBtn_clicked()
{
    // 已填写配对端口/配对码：这是 Android 11+ 无线调试，直接走配对流程（免 USB）
    if (!ui->pairPortEdt->currentText().trimmed().isEmpty() || !ui->pairCodeEdt->text().trimmed().isEmpty()) {
        on_wirelessConnectBtn_clicked();
        return;
    }

    on_stopAllServerBtn_clicked();
    delayMs(200);

    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_getIPBtn_clicked();
    delayMs(200);

    on_startAdbdBtn_clicked();
    delayMs(1000);

    on_wirelessConnectBtn_clicked();
    delayMs(2000);

    on_updateDevice_clicked();
    delayMs(200);

    int firstWifiDevice = findDeviceFromeSerialBox(true);
    if (-1 == firstWifiDevice) {
        qWarning() << "No wifi device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstWifiDevice);

    on_startServerBtn_clicked();
}

void Dialog::on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item)
{
    Q_UNUSED(item);
    ui->serialBox->setCurrentIndex(ui->connectedPhoneList->currentRow());
    on_startServerBtn_clicked();
}

void Dialog::on_updateNameBtn_clicked()
{
    if (ui->serialBox->count() != 0) {
        if (ui->userNameEdt->text().isEmpty()) {
            Config::getInstance().setNickName(ui->serialBox->currentText(), "Phone");
        } else {
            Config::getInstance().setNickName(ui->serialBox->currentText(), ui->userNameEdt->text());
        }

        on_updateDevice_clicked();

        qDebug() << "Update OK!";
    } else {
        qWarning() << "No device is connected!";
    }
}

void Dialog::on_useSingleModeCheck_clicked()
{
    if (ui->useSingleModeCheck->isChecked()) {
        ui->rightWidget->hide();
    } else {
        ui->rightWidget->show();
    }

    adjustSize();
}

void Dialog::on_serialBox_currentIndexChanged(const QString &arg1)
{
    ui->userNameEdt->setText(Config::getInstance().getNickName(arg1));
}

quint32 Dialog::getBitRate()
{
    return ui->bitRateEdit->text().trimmed().toUInt() *
            (ui->bitRateBox->currentText() == QString("Mbps") ? 1000000 : 1000);
}

quint32 Dialog::getWirelessBitRate()
{
    return ui->wirelessBitRateEdit->text().trimmed().toUInt() *
            (ui->wirelessBitRateBox->currentText() == QString("Mbps") ? 1000000 : 1000);
}

const QString &Dialog::getServerPath()
{
    static QString serverPath;
    if (serverPath.isEmpty()) {
        serverPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_SERVER_PATH"));
        QFileInfo fileInfo(serverPath);
        if (serverPath.isEmpty() || !fileInfo.isFile()) {
            serverPath = QCoreApplication::applicationDirPath() + "/scrcpy-server";
        }
    }
    return serverPath;
}

void Dialog::on_startAudioBtn_clicked()
{
    if (ui->serialBox->count() == 0) {
        qWarning() << "No device is connected!";
        return;
    }

    const QString serial = ui->serialBox->currentText().trimmed();
    auto &audio = AudioOutput::instance();
    if (audio.isRunning(serial)) {
        outLog(tr("audio already running for %1").arg(serial), false);
        return;
    }
    if (!audio.start(serial)) {
        outLog(tr("start audio failed for %1").arg(serial), false);
    }
}

void Dialog::on_stopAudioBtn_clicked()
{
    // 只拆除当前选中设备的桥接，不影响其他设备正在播放的音频
    const QString serial = ui->serialBox->currentText().trimmed();
    AudioOutput::instance().stopForDevice(serial);
}

void Dialog::on_installSndcpyBtn_clicked()
{
    if (ui->serialBox->count() == 0) {
        qWarning() << "No device is connected!";
        return;
    }
    AudioOutput::instance().installonly(ui->serialBox->currentText(), 28200);
}

void Dialog::on_autoStartAudioCheck_toggled(bool checked)
{
    // 该决定在下一次设备连接时做出，这可能远早于
    // updateBootConfig(false) 持久化其余界面设置。
    UserBootConfig config = Config::getInstance().getUserBootConfig();
    config.autoStartAudio = checked;
    Config::getInstance().setUserBootConfig(config);
}

void Dialog::on_autoUpdatecheckBox_toggled(bool checked)
{
    if (checked) {
        m_autoUpdatetimer.start(5000);
    } else {
        m_autoUpdatetimer.stop();
    }
}

void Dialog::loadIpHistory()
{
    // 无线调试目标里的 IP 优先，随后合并旧 IP 历史，保证两种连接流程的下拉都可用
    QStringList ipList;
    for (const QString &entry : Config::getInstance().getWirelessTargets()) {
        const QString ip = entry.split('|').value(0).trimmed();
        if (!ip.isEmpty() && !ipList.contains(ip)) {
            ipList << ip;
        }
    }
    for (const QString &ip : Config::getInstance().getIpHistory()) {
        if (!ip.isEmpty() && !ipList.contains(ip)) {
            ipList << ip;
        }
    }

    const QString current = ui->deviceIpEdt->currentText();
    ui->deviceIpEdt->clear();
    ui->deviceIpEdt->addItems(ipList);
    ui->deviceIpEdt->setEditText(current);

    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setFrame(false);
        ui->deviceIpEdt->lineEdit()->setTextMargins(0, 0, 0, 0);
        ui->deviceIpEdt->lineEdit()->setMaxLength(128);
        ui->deviceIpEdt->lineEdit()->setPlaceholderText("192.168.0.1");
    }
}

void Dialog::saveIpHistory(const QString &ip)
{
    if (ip.isEmpty()) {
        return;
    }
    
    Config::getInstance().saveIpHistory(ip);
    
    // 更新ComboBox
    loadIpHistory();
    ui->deviceIpEdt->setCurrentText(ip);
}

void Dialog::showIpEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->deviceIpEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearIpHistory();
        loadIpHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->deviceIpEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}

void Dialog::loadPortHistory()
{
    QStringList portList = Config::getInstance().getPortHistory();
    ui->devicePortEdt->clear();
    ui->devicePortEdt->addItems(portList);

    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setFrame(false);
        ui->devicePortEdt->lineEdit()->setTextMargins(0, 0, 0, 0);
        ui->devicePortEdt->lineEdit()->setMaxLength(6);
        ui->devicePortEdt->lineEdit()->setPlaceholderText("5555");
    }
}

void Dialog::savePortHistory(const QString &port)
{
    if (port.isEmpty()) {
        return;
    }
    
    Config::getInstance().savePortHistory(port);
    
    // 更新ComboBox
    loadPortHistory();
    ui->devicePortEdt->setCurrentText(port);
}

void Dialog::showPortEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->devicePortEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearPortHistory();
        loadPortHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->devicePortEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}


void Dialog::on_codecModeBox_currentIndexChanged(int index)
{
    // 更新码率时阻塞信号，避免递归调用
    ui->bitRateEdit->blockSignals(true);
    ui->bitRateBox->blockSignals(true);

    const EncoderPreset *preset = nullptr;
    if (index > 0) {
        preset = &EncoderPresetRegistry::all().at(index - 1);
    }

    if (index == 0) {
        // 默认：恢复之前的设置
        if (m_prevBitRate == 0) {
            ui->bitRateEdit->setText("12");
            ui->bitRateBox->setCurrentText("Mbps");
        } else if (m_prevBitRate % 1000000 == 0) {
            ui->bitRateEdit->setText(QString::number(m_prevBitRate / 1000000));
            ui->bitRateBox->setCurrentText("Mbps");
        } else {
            ui->bitRateEdit->setText(QString::number(m_prevBitRate / 1000));
            ui->bitRateBox->setCurrentText("Kbps");
        }
        // 恢复之前的最大尺寸
        ui->maxSizeBox->setCurrentIndex(m_prevMaxSizeIndex);
    } else {
        // 预设模式：保存当前设置，供默认模式恢复使用
        m_prevBitRate = getBitRate();
        m_prevMaxSizeIndex = ui->maxSizeBox->currentIndex();

        // 同步码率与最大尺寸，使其与已保存的编码档位一致
        UserBootConfig config = Config::getInstance().getUserBootConfig();
        int level = config.presetLevel;
        quint32 bitrate = preset->levelBitRate(level);
        if (bitrate > 0) {
            if (bitrate % 1000000 == 0) {
                ui->bitRateEdit->setText(QString::number(bitrate / 1000000));
                ui->bitRateBox->setCurrentText("Mbps");
            } else {
                ui->bitRateEdit->setText(QString::number(bitrate / 1000));
                ui->bitRateBox->setCurrentText("Kbps");
            }
        }
        int maxSizeIdx = preset->levelMaxSizeIndex(level);
        if (maxSizeIdx >= 0) {
            ui->maxSizeBox->setCurrentIndex(maxSizeIdx);
        }
    }

    // 仅当预设包含可调档位时，才显示编码设置按钮
    ui->presetConfigBtn->setVisible(preset && preset->levelCount() >= 2);

    ui->bitRateEdit->blockSignals(false);
    ui->bitRateBox->blockSignals(false);
}

void Dialog::on_presetConfigBtn_clicked()
{
    const int codecModeIndex = ui->codecModeBox->currentIndex();
    if (codecModeIndex <= 0) {
        return;
    }
    const EncoderPreset &preset = EncoderPresetRegistry::all().at(codecModeIndex - 1);

    // (Re)create the dialog when the selected preset changes.
    if (!m_presetDialog || m_presetDialog->presetId() != preset.id) {
        m_presetDialog = new PresetConfigDialog(preset, this);
        // 仅在首次创建时恢复已保存的档位——后续打开
        // 保留用户在对话框中的上次选择。
        UserBootConfig bootConfig = Config::getInstance().getUserBootConfig();
        m_presetDialog->setLevel(bootConfig.presetLevel);

        // 用户切换档位时，实时同步码率与最大尺寸的界面
        connect(m_presetDialog.data(), &PresetConfigDialog::levelChanged,
                this, &Dialog::syncPresetLevelToUi);
    }

    m_presetDialog->exec();

    // 对话框关闭后同步——覆盖初始档位通过上面的 setLevel() 恢复、
    // 且未触发 levelChanged 信号的情况。
    syncPresetLevelToUi();
}

void Dialog::syncPresetLevelToUi()
{
    const int codecModeIndex = ui->codecModeBox->currentIndex();
    if (codecModeIndex <= 0 || !m_presetDialog) {
        return;
    }
    const EncoderPreset &preset = EncoderPresetRegistry::all().at(codecModeIndex - 1);
    int level = m_presetDialog->selectedLevel();

    // 更新码率
    quint32 bitrate = preset.levelBitRate(level);
    if (bitrate > 0) {
        ui->bitRateEdit->blockSignals(true);
        ui->bitRateBox->blockSignals(true);
        if (bitrate % 1000000 == 0) {
            ui->bitRateEdit->setText(QString::number(bitrate / 1000000));
            ui->bitRateBox->setCurrentText("Mbps");
        } else {
            ui->bitRateEdit->setText(QString::number(bitrate / 1000));
            ui->bitRateBox->setCurrentText("Kbps");
        }
        ui->bitRateEdit->blockSignals(false);
        ui->bitRateBox->blockSignals(false);
    }

    // 更新最大尺寸（仅当档位指定了尺寸时）
    int maxSizeIdx = preset.levelMaxSizeIndex(level);
    if (maxSizeIdx >= 0) {
        ui->maxSizeBox->setCurrentIndex(maxSizeIdx);
    }
}
