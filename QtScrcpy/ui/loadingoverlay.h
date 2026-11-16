#ifndef LOADINGOVERLAY_H
#define LOADINGOVERLAY_H

#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;

// 连接加载遮罩：方案A「手机轮廓 + 数据流」，三阶段递进动画。
//
// 全部为 QPainter 矢量绘制（60fps 定时重绘），不依赖任何图片资源。
// 视觉规格：
//   - 逻辑画布 320x240，等比缩放居中映射到控件尺寸（缩放上限 2.2）；
//   - 手机轮廓 96x172 圆角16 描边3px，内屏内缩6px；
//   - 数据流粒子恒定自左向右"流入"手机，穿过内屏时增亮；
//   - 三阶段：0=握手 1=通道建立 2=首帧解码，切换时全部动态参数
//     在 400ms 内线性插值；
//   - 阶段2 持续 1.5s 自动推进到阶段3（首帧回调先到则立即推进）；
//   - 退场：fadeOutAndDelete() 后整体透明度 250ms 淡出并自毁。
class LoadingOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit LoadingOverlay(QWidget *parent = nullptr);

    // 推进到指定阶段（阶段只前进不回退；进行中的过渡会平滑重启）
    void advancePhase(int phase);
    // 更新主文案（例如 15 秒无画面时的安抚文案）
    void setText(const QString &text);
    // 首帧纹理就绪：整体淡出后 emit fadedOut() 并 deleteLater()
    void fadeOutAndDelete();

signals:
    void fadedOut();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Particle
    {
        qint64 born = 0;  // 出生时刻（m_clock 毫秒）
        qreal y = 0;      // 纵向位置（逻辑坐标）
        qreal seed = 0;   // 0..1，用于颜色微差
    };
    // 某一阶段的全部动画参数（切换时对每项做线性插值）
    struct PhaseParams
    {
        qreal interval = 0;   // 粒子发射间隔 ms
        qreal speed = 0;      // 粒子速度 px/s
        QColor c0, c1;        // 粒子双色（屏幕外/屏幕内）
        qreal trail = 0;      // 拖尾长度 px
        qreal glow = 0;       // 轮廓光晕强度
        QColor outline;       // 轮廓阶段色
        bool breathe = false; // 轮廓是否呼吸（仅阶段1）
        int fx = 0;           // 屏幕内特效级别：0 无 / 1 上升微光带 / 2 扫光脉冲
    };

    PhaseParams blendParams() const;
    void tick();

    QElapsedTimer m_clock;
    QTimer *m_timer = nullptr;
    QVector<Particle> m_particles;
    qreal m_spawnAcc = 0;
    qint64 m_lastTick = 0;

    int m_phaseFrom = 0;        // 过渡起始阶段
    int m_phaseTo = 0;          // 当前目标阶段
    qint64 m_switchAt = 0;      // 最近一次阶段切换时刻 ms
    qint64 m_phase2At = -1;     // 进入阶段2的时刻，用于 1.5s 自动推进
    QString m_text;
    qreal m_fade = 1.0;         // 整体透明度（退场淡出）
    bool m_fading = false;
};

#endif // LOADINGOVERLAY_H
