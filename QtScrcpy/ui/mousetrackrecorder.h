#ifndef MOUSETRACKRECORDER_H
#define MOUSETRACKRECORDER_H

#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QMutex>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QEvent>
#include <Qt>

class QMouseEvent;
class QWheelEvent;

/*
 * 录制时的鼠标操作轨迹记录器。
 *
 * 事件侧（UI 线程）：VideoForm 把投屏视频控件内的鼠标事件坐标（先 mapFrom 到
 * 视频控件）按 inputconvertnormal 同款公式换算成设备画面（frameSize）坐标，
 * 连同事件类型记录下来；同时可选地把操作轨迹落盘为 JSON Lines（与录制的
 * 视频文件同名，扩展名 .events.json），时间轴与视频 pts 对齐。
 *
 * 合成侧（录制编码线程）：VideoRecorder 在每帧送编码器前回调 paintAt()，
 * 在画面图像上绘制高亮光标、点击波纹、拖拽尾迹，把鼠标操作直接叠进录制的
 * mp4 视频。
 *
 * 事件与绘制的时基一致：startSession() 启动的本地毫秒时钟，与 VideoRecorder
 * 的录制时钟在同一线程先后启动，起点误差可忽略。
 */
class MouseTrackRecorder
{
public:
    enum RippleType
    {
        Press,        // 左键（触摸）
        DoubleClick,  // 双击
        RightButton,  // 右键（Back）
        MiddleButton, // 中键（Home）
    };

    struct Style
    {
        QColor pressColor = QColor(255, 206, 61);   // 左键波纹/光标主色（高亮金）
        QColor rightColor = QColor(255, 106, 106);  // 右键波纹（亮珊瑚红）
        QColor middleColor = QColor(72, 214, 201);  // 中键波纹（亮青）
        QColor trailColor = QColor(255, 206, 61);   // 拖拽尾迹
        int cursorRadius = 18;  // 设备画面像素
        int rippleMs = 600;     // 波纹生命周期
        int trailMs = 1000;     // 尾迹渐隐窗口
        int wheelFlashMs = 400; // 滚动方向指示显示时长
    };

    MouseTrackRecorder();

    // UI 线程：记录鼠标事件。posInVideo 为视频控件内坐标。
    void onMouseEvent(const QPointF &posInVideo, const QSize &videoSize, const QSize &frameSize, QEvent::Type type,
                      Qt::MouseButton button);
    void onWheelEvent(const QPointF &posInVideo, const QSize &videoSize, const QSize &frameSize, int angleDeltaY);

    // 开始/结束一次录制会话。eventsPath 非空时把操作轨迹落盘（JSON Lines）。
    void startSession(const QString &eventsPath);
    void endSession();
    bool isActive() const;

    // 编码线程：在画面图像 img（原始帧缩放 scale 后，不含 letterbox 黑边）上
    // 绘制 ptsMs 时刻应显示的光标/波纹/尾迹。
    void paintAt(QImage &img, qint64 ptsMs, double scale);

private:
    struct Ripple
    {
        qint64 ts = 0;
        QPointF pos; // 设备画面坐标
        RippleType type = Press;
    };
    struct Pt
    {
        qint64 ts = 0;
        QPointF pos;
    };

    QPointF toFramePos(const QPointF &posInVideo, const QSize &videoSize, const QSize &frameSize) const;
    void appendEventLog(const QString &line);

    mutable QMutex m_mutex;
    Style m_style;
    QElapsedTimer m_clock;

    QPointF m_cursor;         // 最近事件位置（设备画面坐标）
    QPointF m_cursorSmoothed; // 平滑后的光标（绘制用）
    bool m_cursorValid = false;
    bool m_dragging = false;

    // 方案 A：参考图抠图素材（构造时加载一次，编码线程只读 QImage）
    QImage m_gloveNormal;  // 悬停：原图
    QImage m_glovePressed; // 按下：整体压暗到 90%，绘制时再下压 + 轻旋

    QVector<Ripple> m_ripples;
    QVector<Pt> m_trail;

    int m_wheelDir = 0;    // >0 上滚 <0 下滚
    qint64 m_wheelTs = 0;

    QFile m_logFile;
    bool m_sessionActive = false;
};

#endif // MOUSETRACKRECORDER_H
