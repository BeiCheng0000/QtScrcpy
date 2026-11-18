#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QPointer>
#include <QSharedPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"
#include "mousetrackrecorder.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QYUVOpenGLWidget;
class QLabel;
class MetalVideoWidget;
class QVariantAnimation;
class VideoRecorder;
class VideoForm : public QWidget, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool showToolBar = true, int decodeMode = 0, QWidget *parent = 0);
    ~VideoForm();

    void staysOnTop(bool top = true);
    void updateShowSize(const QSize &newSize);
    void updateRender(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV);
    void setSerial(const QString& serial);
    QRect getGrabCursorRect();
    const QSize &frameSize();
    void resizeSquare();
    void removeBlackRect();
    void showFPS(bool show);
    void switchFullScreen();
    bool isHost();

    // 屏幕录制：由工具栏的录制按钮触发。
    bool isRecording() const;
    bool isRecordingSupported() const;
    bool startRecord();
    void stopRecord();

    // 批量安装（悬停菜单「安装 apk」）：串行执行，逐项发 apkInstallProgress/Finished
    void installApkBatch(const QStringList &paths);
    void cancelInstallBatch();

signals:
    void recordingStateChanged();
    // 首个视频帧写入录制器（视频 pts 0 点）：工具栏录制计时以此为起点，
    // 与视频文件的实际内容时长保持一致
    void recordingClockStarted();
    // 用户主动关闭投屏窗口：主窗口据此豁免该设备的自动重连
    //（否则关窗口会被当成意外掉线，窗口被自动重连“复活”）
    void windowClosedByUser(const QString &serial);
    // 批量 apk 安装：每个 apk 完成一项状态，队列结束后汇总
    void apkInstallProgress(const QString &path, const QString &status);
    void apkInstallFinished(int successCount, int failCount);

private slots:
    void onRecordingFinished(const QString &filePath, qint64 durationMs, qint64 bytes, bool hasAudio);
    void onRecordingError(const QString &error);

private:
    QString buildRecordFilePath() const;
    void openRecordFolder(const QString &filePath) const;

    // 让窗口几何尺寸与视频宽高比保持同步。否则最大化（或从最小化还原）后会
    // 留下大片黑边。
    QSize fittedWindowSize(bool fillScreen);
    void fitWindowToFrame(bool fillScreen);
    bool hasExcessiveLetterbox() const;

    void onFrame(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    // VideoToolbox Metal 路径帧回调（仅 macOS arm64）
    void onFrameMetal(void* cvPixelBuffer, int width, int height) override;
    void updateFPS(quint32 fps, quint32 skipped) override;
    void onVideoSessionChanged(const QSize &size, bool clientResized) override;
    void grabCursor(bool grab) override;

    // 皮肤（手机边框）已废弃：原 updateStyleSheet/getMargins 随之删除
    void initUI();
    // 连接加载遮罩：让遮罩始终覆盖画面区域并保持顶层
    void syncLoadingOverlay();

    void showToolForm(bool show = true);
    void syncToolFormGeometry();
    // 全屏模式下的边缘唤出工具栏：鼠标贴到屏幕最右侧边界才出现，
    // 移出工具栏范围即收起
    void updateFullscreenToolbar();
    void showFullscreenToolbar();
    void hideFullscreenToolbar();
    // 进入全屏时以全透明预驻留工具栏窗口（消化 DWM 合成切换，避免黑闪）
    void prepareFullscreenToolbar();
    void exitFullscreenToolbar();
    void startFullscreenToolbarFade(double from, double to);
    void moveCenter();
    void installShortcut();
    QRect getScreenRect();
    // 窗口越界回收：显示器插拔后，若窗口落在任何屏幕的可用区域之外，
    // 将其移回主屏居中，避免成为看不见的"幽灵窗口"。
    void ensureOnScreen();
    // 是否与任一屏幕的可用区域有交集（含边界相交判定）
    bool isRectOnAnyScreen(const QRect &rect) const;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // 获取当前视频渲染 widget（OpenGL 或 Metal 容器）
    QWidget* videoWidget() const;
    // 是否使用 Metal 渲染路径
    bool isMetalMode() const;

    // 拖入 APK 安装：窗口内浮层提示「正在安装/安装成功/安装失败」
    void installApk(const QString &apkPath);
    void installOne(const QString &apkPath, const std::function<void(bool, const QString &)> &done);
    void installNextApk();
    void showInstallTip(const QString &text, int autoHideMs);
    void syncInstallTip();
    QPointer<QLabel> m_installTipLabel;
    QTimer m_installTipTimer;
    bool m_recordingClockEmitted = false; // 本次录制是否已广播视频 0 点（recordingClockStarted）
    QStringList m_installQueue;
    bool m_installBusy = false;
    bool m_installCancelled = false;
    int m_installOk = 0;
    int m_installFail = 0;

    // 录制时把投屏窗口内的鼠标事件交给轨迹记录器（叠加进视频 + events.json）
    void trackMouse(const QPointF &posInVideo, const QSize &videoSize, QEvent::Type type, Qt::MouseButton button);
    void trackWheel(const QPointF &posInVideo, const QSize &videoSize, int angleDeltaY);
    QSharedPointer<MouseTrackRecorder> m_trackRecorder = QSharedPointer<MouseTrackRecorder>::create();

    // ui
    Ui::videoForm *ui;
    QPointer<ToolForm> m_toolForm;
    QPointer<QWidget> m_loadingWidget;
    QPointer<QYUVOpenGLWidget> m_videoWidget;

    // Metal 渲染路径（仅 macOS arm64）
    QPointer<MetalVideoWidget> m_metalWidget;

    QPointer<QLabel> m_fpsLabel;

    // 录屏
    QPointer<VideoRecorder> m_recorder;
    QString m_recordAudioKey;
    bool m_closing = false;

    // 智能最大化状态。最大化时把画面放大到屏幕可容纳的最大尺寸（而不是留黑边），
    // 但窗口仍保持普通状态，使系统的最大化/还原按钮能正常切换。这些字段记录
    // 最大化前的几何尺寸以及当前是否处于智能最大化状态。
    bool m_smartMaximized = false;
    QRect m_restoredGeometry;
    bool m_stateAdjusting = false;

    //inside member
    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    QPoint m_fullScreenBeforePos;
    QString m_serial;
    int m_decodeMode = 0;
    bool m_metalFirstFrame = true;  // Metal 首次帧标记
    bool m_flexDisplay = false;
    bool m_preventAutoResize = false;
    QTimer m_flexResizeTimer;
    QSize m_pendingDisplaySize;

    // 全屏模式边缘唤出工具栏：轮询全局光标位置（全屏时窗口收不到
    // 屏幕边缘之外区域的鼠标事件，轮询是最可靠的方式）
    QTimer m_fullscreenToolbarTimer;
    bool m_fullscreenToolbarShown = false;
    // 工具栏悬浮淡入动画（父对象为 m_toolForm，随其销毁）
    QVariantAnimation *m_toolbarFade = nullptr;
    // 工具栏是否以全透明驻留在屏幕右缘（Windows 预驻留方案）
    bool m_fsToolbarResident = false;

    //Whether to display the toolbar when connecting a device.
    bool show_toolbar = true;
};

#endif // VIDEOFORM_H
