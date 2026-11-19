#ifndef DIALOG_H
#define DIALOG_H

#include <QWidget>
#include <QPointer>
#include <QMessageBox>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QListWidget>
#include <QTimer>
#include <QMap>
#include <QSet>


#include "adbprocess.h"
#include "wirelesscontroller.h"
#include "../QtScrcpyCore/include/QtScrcpyCore.h"
#include "audio/audiooutput.h"
#include "presetconfigdialog.h"

namespace Ui
{
    class Widget;
}

class QYUVOpenGLWidget;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QGroupBox;
class QPushButton;
class QSpinBox;
class FileManagerWindow;
class GroupControlWidget;
class Dialog : public QWidget
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = 0);
    ~Dialog();

    void outLog(const QString &log, bool newLine = true);
    bool filterLog(const QString &log);
    void getIPbyIp();
    // 显示主窗口（例如从托盘隐藏状态恢复）并为其赋予焦点
    void bringToFront();

private slots:
    void onDeviceConnected(bool success, const QString& serial, const QString& deviceName, const QSize& size);
    void onDeviceDisconnected(QString serial);

    void on_updateDevice_clicked();
    void on_startServerBtn_clicked();
    void on_stopServerBtn_clicked();
    void on_wirelessConnectBtn_clicked();
    void on_startAdbdBtn_clicked();
    void on_getIPBtn_clicked();
    void on_wirelessDisConnectBtn_clicked();
    void on_selectRecordPathBtn_clicked();
    void on_recordPathEdt_textChanged(const QString &arg1);
    void on_adbCommandBtn_clicked();
    void on_stopAdbBtn_clicked();
    void on_clearOut_clicked();
    void on_stopAllServerBtn_clicked();
    void on_refreshGameScriptBtn_clicked();
    void on_applyScriptBtn_clicked();
    void on_recordScreenCheck_clicked(bool checked);
    void on_usbConnectBtn_clicked();
    void on_wifiConnectBtn_clicked();
    void on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item);
    void on_updateNameBtn_clicked();
    void on_useSingleModeCheck_clicked();
    void on_serialBox_currentIndexChanged(const QString &arg1);

    void on_startAudioBtn_clicked();

    void on_stopAudioBtn_clicked();

    void on_installSndcpyBtn_clicked();

    void on_autoStartAudioCheck_toggled(bool checked);

    void on_autoUpdatecheckBox_toggled(bool checked);

    void on_codecModeBox_currentIndexChanged(int index);
    void on_presetConfigBtn_clicked();

    void on_videoSourceBox_currentIndexChanged(int index);
    void on_refreshCameraBtn_clicked();

    void showIpEditMenu(const QPoint &pos);
    void on_fileManagerBtn_clicked();

    // ---- 无线调试（Android 11+ 配对）----
    void on_deviceIpEdt_activated(int index);
    void onWirelessStateChanged(int state, const QString &text);
    void onWirelessFailed(const QString &reason);
    void onWirelessConnected(const QString &addr);
    void onWirelessPaired(const QString &pairAddr);

private:
    bool checkAdbRun();
    void initUI();
    void updateBootConfig(bool toView = true);
    void execAdbCmd();
    void delayMs(int ms);
    QString getGameScript(const QString &fileName);
    void slotActivated(QSystemTrayIcon::ActivationReason reason);
    int findDeviceFromeSerialBox(bool wifi);
    quint32 getBitRate();
    quint32 getWirelessBitRate();
    const QString &getServerPath();
    void initAdvancedConfigUi(QWidget *parent);
    void loadAdvancedConfig();
    void saveAdvancedConfig();
    void openUserDirectory(const QString &path);
    void refreshApps();
    void updateVideoSourceUi();
    void initAdvancedDisplayUi();
    void updateAdvancedDisplayUi();
    void loadIpHistory();
    void saveIpHistory(const QString &ip);
    void loadPortHistory();
    void savePortHistory(const QString &port);

    // 无线调试
    void setupWirelessUi();
    quint16 wirelessConnectPort() const;
    void applyWirelessTargetToUi(const QString &ip, quint16 connectPort, quint16 pairPort);
    void updateWirelessControls();

    void showPortEditMenu(const QPoint &pos);
    void syncPresetLevelToUi();

    // ---- 自动重连（意外掉线后有限次重试）----
    void startServerFor(const QString &serial); // on_startServerBtn_clicked 的参数化版本
    void tryReconnect();
    void stopAutoReconnect();

protected:
    void closeEvent(QCloseEvent *event);
    void changeEvent(QEvent *event) override; // 运行时语言切换刷新文案

private:
    Ui::Widget *ui;
    qsc::AdbProcess m_adb;
    WirelessController *m_wireless = nullptr;
    QTimer m_reconnectTimer;          // 单发定时：意外掉线后逐次重试
    QString m_reconnectSerial;        // 正在自动重连的设备
    int m_reconnectTries = 0;         // 剩余重试次数
    QSet<QString> m_manualDisconnects; // 用户主动断开的设备：不自动重连
    QSet<QString> m_internalClosing;   // 程序清理中正在 close 的窗口：其 windowClosedByUser 信号不算用户操作
    QSet<QString> m_connectedSerials;  // 当前已连接设备（用于“停止全部”标记手动断开）
    QSystemTrayIcon *m_hideIcon;
    GroupControlWidget *m_groupWidget = nullptr; // 群控页签（运行期创建）
    class QTabWidget *m_groupTabWidget = nullptr;
    int m_groupTabIndex = -1;
    QGroupBox *m_advancedDisplayGroup = nullptr;
    QComboBox *m_configLanguageBox = nullptr;
    QLineEdit *m_configTitleEdit = nullptr;
    QLineEdit *m_configPushPathEdit = nullptr;
    QSpinBox *m_configMaxFpsSpin = nullptr;
    QSpinBox *m_configWirelessMaxFpsSpin = nullptr;
    QCheckBox *m_configRenderExpiredCheck = nullptr;
    QSpinBox *m_configDecoderThreadsSpin = nullptr;
    QComboBox *m_configOpenGlBox = nullptr;
    QLineEdit *m_configServerPathEdit = nullptr;
    QLineEdit *m_configAdbPathEdit = nullptr;
    QLineEdit *m_configCodecOptionsEdit = nullptr;
    QLineEdit *m_configCodecNameEdit = nullptr;
    QComboBox *m_configLogLevelBox = nullptr;
    QGroupBox *m_appConfigGroup = nullptr;       // 应用配置组（运行期创建，语言切换刷新标题）
    QPushButton *m_openConfigButton = nullptr;
    QPushButton *m_openKeymapButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QGroupBox *m_recordGroupTitle = nullptr; // 录制设置组（语言切换时刷新标题）
    // 录制设置（userdata.ini）
    QCheckBox *m_recordAudioCheck = nullptr;
    QCheckBox *m_recordMouseTrackCheck = nullptr;
    QSpinBox *m_recordFpsSpin = nullptr;
    QSpinBox *m_recordVideoBitrateSpin = nullptr;
    QSpinBox *m_recordAudioBitrateSpin = nullptr;
    QComboBox *m_displayModeBox = nullptr;
    QLineEdit *m_displayIdEdit = nullptr;
    QLineEdit *m_newDisplayEdit = nullptr;
    QLineEdit *m_cropEdit = nullptr;
    QCheckBox *m_flexDisplayCheck = nullptr;
    QComboBox *m_displayImePolicyBox = nullptr;
    QCheckBox *m_vdSystemDecorationsCheck = nullptr;
    QCheckBox *m_vdDestroyContentCheck = nullptr;
    QCheckBox *m_keepActiveCheck = nullptr;
    QComboBox *m_startAppBox = nullptr;
    QPushButton *m_refreshAppsBtn = nullptr;
    QMenu *m_menu;
    QAction *m_showWindow;
    QAction *m_quit;
    QTimer m_autoUpdatetimer;
    quint32 m_prevBitRate = 2000000;
    int m_prevMaxSizeIndex = 0;
    QPointer<PresetConfigDialog> m_presetDialog;
    QPushButton *m_fileManagerBtn = nullptr;
    // 每台设备一个独立文件管理窗口
    QMap<QString, QPointer<FileManagerWindow>> m_fileWindows;
};

#endif // DIALOG_H
