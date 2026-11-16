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

protected:
    void closeEvent(QCloseEvent *event);

private:
    Ui::Widget *ui;
    qsc::AdbProcess m_adb;
    WirelessController *m_wireless = nullptr;
    QSystemTrayIcon *m_hideIcon;
    QGroupBox *m_advancedDisplayGroup = nullptr;
    QComboBox *m_configLanguageBox = nullptr;
    QLineEdit *m_configTitleEdit = nullptr;
    QLineEdit *m_configPushPathEdit = nullptr;
    QSpinBox *m_configMaxFpsSpin = nullptr;
    QCheckBox *m_configRenderExpiredCheck = nullptr;
    QSpinBox *m_configDecoderThreadsSpin = nullptr;
    QComboBox *m_configOpenGlBox = nullptr;
    QLineEdit *m_configServerPathEdit = nullptr;
    QLineEdit *m_configAdbPathEdit = nullptr;
    QLineEdit *m_configCodecOptionsEdit = nullptr;
    QLineEdit *m_configCodecNameEdit = nullptr;
    QComboBox *m_configLogLevelBox = nullptr;
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
