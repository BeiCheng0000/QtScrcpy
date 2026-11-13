#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QYUVOpenGLWidget;
class QLabel;
class MetalVideoWidget;
class VideoRecorder;
class VideoForm : public QWidget, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool skin = true, bool showToolBar = true, int decodeMode = 0, QWidget *parent = 0);
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

    // Screen recording, driven by the toolbar record button.
    bool isRecording() const;
    bool isRecordingSupported() const;
    bool startRecord();
    void stopRecord();

signals:
    void recordingStateChanged();

private slots:
    void onRecordingFinished(const QString &filePath, qint64 durationMs, qint64 bytes, bool hasAudio);
    void onRecordingError(const QString &error);

private:
    QString buildRecordFilePath() const;
    void openRecordFolder(const QString &filePath) const;

    // Keep the window geometry in sync with the video aspect ratio. Without
    // this, maximizing (or restoring from minimize) leaves a huge letterbox.
    QSize fittedWindowSize(bool fillScreen);
    void fitWindowToFrame(bool fillScreen);
    bool hasExcessiveLetterbox() const;

    void onFrame(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    // VideoToolbox Metal 路径帧回调（仅 macOS arm64）
    void onFrameMetal(void* cvPixelBuffer, int width, int height) override;
    void updateFPS(quint32 fps) override;
    void onVideoSessionChanged(const QSize &size, bool clientResized) override;
    void grabCursor(bool grab) override;

    void updateStyleSheet(bool vertical);
    QMargins getMargins(bool vertical);
    void initUI();

    void showToolForm(bool show = true);
    void syncToolFormGeometry();
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

    // ui
    Ui::videoForm *ui;
    QPointer<ToolForm> m_toolForm;
    QPointer<QWidget> m_loadingWidget;
    QPointer<QYUVOpenGLWidget> m_videoWidget;

    // Metal 渲染路径（仅 macOS arm64）
    QPointer<MetalVideoWidget> m_metalWidget;

    QPointer<QLabel> m_fpsLabel;

    // screen recording
    QPointer<VideoRecorder> m_recorder;
    QString m_recordAudioKey;
    bool m_closing = false;

    // Smart-maximize toggle state. On maximize we grow the picture to the
    // largest size the screen can hold (instead of leaving black bands) but
    // keep the window in the normal state, so the native maximize/restore
    // button still toggles it. These fields remember the pre-maximize geometry
    // and whether a smart maximize is currently active.
    bool m_smartMaximized = false;
    QRect m_restoredGeometry;
    bool m_stateAdjusting = false;

    //inside member
    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    bool m_skin = true;
    QPoint m_fullScreenBeforePos;
    QString m_serial;
    int m_decodeMode = 0;
    bool m_metalFirstFrame = true;  // Metal 首次帧标记
    bool m_flexDisplay = false;
    bool m_preventAutoResize = false;
    QTimer m_flexResizeTimer;
    QSize m_pendingDisplaySize;

    //Whether to display the toolbar when connecting a device.
    bool show_toolbar = true;
};

#endif // VIDEOFORM_H
