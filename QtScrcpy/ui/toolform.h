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

class Device;   // 前向声明，避免引入完整头文件
class QLabel;   // 前向声明，提示标签使用
// 右侧吸附式工具栏：随视频窗口显示，提供截屏、录制、按键、设置等快捷控制
class ToolForm : public MagneticWidget
{
    Q_OBJECT

public:
    explicit ToolForm(QWidget *adsorbWidget, AdsorbPositions adsorbPos);
    ~ToolForm();

    void setSerial(const QString& serial); // 绑定当前操作的设备序列号
    bool isHost();                         // 是否为群控主机（影响部分按钮可用性）

protected:
    // 鼠标按下/释放/移动：实现工具栏拖拽与吸附到视频窗口右侧
    void mousePressEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void resizeEvent(QResizeEvent *event) override;
    // 事件过滤器：监听设置按钮的鼠标进入事件，悬停时弹出扩展菜单
    bool eventFilter(QObject *watched, QEvent *event) override;

    void showEvent(QShowEvent *event);
    void hideEvent(QHideEvent *event);

private slots:
    // 各按钮点击槽：向当前绑定设备发送对应的控制指令
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
    void initStyle();                       // 加载样式表并设置按钮图标
    void updateGroupControl();              // 根据群控身份刷新群控按钮可用性
    void updateCameraMode();                // 相机模式下隐藏不相关按钮
    void updateRecordUi();                  // 刷新录制状态与计时显示
    void updateButtonSpacing();             // 按可见按钮数量动态均分间距
    void createSettingsMenu();              // 创建悬停弹出的设置子菜单窗口
    void showSettingsMenu();                // 在设置按钮旁显示扩展菜单
    void hideSettingsMenu();                // 隐藏扩展菜单并停止隐藏检测定时器
    void startActivity(const QString &action); // 通过 adb 打开指定 Android 设置页
    void showScreenshotHint();              // 显示“已截图，可以粘贴”轻量提示

private:
    Ui::ToolForm *ui;
    QPoint m_dragPosition;          // 拖拽时鼠标相对窗口左上角的偏移
    QString m_serial;               // 当前绑定设备序列号
    bool m_showTouch = false;       // 是否显示触摸反馈点
    bool m_cameraTorch = false;     // 相机闪光灯开关状态
    bool m_isHost = false;          // 是否处于群控主机身份
    bool m_recording = false;       // 是否正在录制
    QTimer m_recordTimer;           // 定时刷新录制计时
    QTimer m_settingsMenuTimer;     // 检测鼠标是否离开设置菜单区域
    QTimer m_toastTimer;            // 截图提示自动隐藏定时
    QElapsedTimer m_recordElapsed;  // 录制已持续时长
    QWidget *m_settingsMenu = nullptr; // 悬停弹出的设置子菜单（无边框工具窗口）
    QLabel *m_toast = nullptr;          // 截图成功后的轻量提示标签
};

#endif // TOOLFORM_H
