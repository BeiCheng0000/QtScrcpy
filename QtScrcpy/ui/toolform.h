#ifndef TOOLFORM_H
#define TOOLFORM_H

#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"
#include "magneticwidget.h"

namespace Ui
{
    class ToolForm;
}

class Device;
class QLabel;
class ToolForm : public MagneticWidget
{
    Q_OBJECT

public:
    explicit ToolForm(QWidget *adsorbWidget, AdsorbPositions adsorbPos);
    ~ToolForm();

    void setSerial(const QString& serial);
    bool isHost();

protected:
    void mousePressEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    void showEvent(QShowEvent *event);
    void hideEvent(QHideEvent *event);

private slots:
    void on_fullScreenBtn_clicked();
    void on_returnBtn_clicked();
    void on_homeBtn_clicked();
    void on_menuBtn_clicked();
    void on_appSwitchBtn_clicked();
    void on_powerBtn_clicked();
    void on_screenShotBtn_clicked();
    void on_recordBtn_clicked();
    void on_volumeUpBtn_clicked();
    void on_volumeDownBtn_clicked();
    void on_closeScreenBtn_clicked();
    void on_expandNotifyBtn_clicked();
    void on_expandSettingsBtn_clicked();
    void on_rotateBtn_clicked();
    void on_touchBtn_clicked();
    void on_cameraTorchBtn_clicked();
    void on_cameraZoomOutBtn_clicked();
    void on_cameraZoomInBtn_clicked();
    void on_groupControlBtn_clicked();
    void on_openScreenBtn_clicked();

private:
    void initStyle();
    void updateGroupControl();
    void updateCameraMode();
    void updateRecordUi();
    void updateButtonSpacing();
    void createSettingsMenu();
    void showSettingsMenu();
    void hideSettingsMenu();
    void startActivity(const QString &action);
    void showScreenshotHint();

private:
    Ui::ToolForm *ui;
    QPoint m_dragPosition;
    QString m_serial;
    bool m_showTouch = false;
    bool m_cameraTorch = false;
    bool m_isHost = false;
    bool m_recording = false;
    QTimer m_recordTimer;
    QTimer m_settingsMenuTimer;
    QTimer m_toastTimer;
    QElapsedTimer m_recordElapsed;
    QWidget *m_settingsMenu = nullptr;
    QLabel *m_toast = nullptr;
};

#endif // TOOLFORM_H
